// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusModelLayout.h"
#include "VaCuusSubsystem.h"
#include "VaCuusUIThread.h"
#include "VaCuusView.h"
#include "VaCuusViewStatus.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/ScopeExit.h"
#include "StructUtils/UserDefinedStruct.h"

// FStructVariableDescription is only FORWARD-declared by StructureEditorUtils.h:10, and
// GetVarDesc() hands back a TArray of them -- so indexing that array needs the definition,
// which lives here (UserDefinedStructEditorData.h:34).
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * SPEC 11's LAST **[unverified]** ROW, SETTLED: what happens to a live FVaCuusModelLayout when
 * the Blueprint struct under it is recompiled.
 *
 * The spec's mitigation reads "Strong ref + rebuild". THE FIRST HALF WORKS AND THE SECOND HALF
 * DOES NOT EXIST, and this test is the evidence for both halves at once.
 *
 * WHAT A RECOMPILE ACTUALLY DOES, read rather than assumed. Every structural edit funnels
 * through FStructureEditorUtils::OnStructureChanged, which sets Status = UDSS_Dirty and calls
 * CompileStructure (StructureEditorUtils.cpp:569-580). That reaches
 * FUserDefinedStructureCompilerUtils::CompileStruct, whose own comment says "Recompiled UDS keep
 * the same instance" (UserDefinedStructureCompilerUtils.cpp:595) -- so the UObject survives,
 * and a TStrongObjectPtr to it stays valid. But the first thing done to that surviving instance
 * is CleanAndSanitizeStruct, which calls DestroyChildPropertiesAndResetPropertyLinks
 * (:512, :225) -> DestroyPropertyLinkedList, which is a plain `delete FieldToDestroy` over the
 * whole chain (Class.cpp:2117-2126). Every FProperty is FREED and a fresh set is built by
 * CreateVariables (:244).
 *
 * FVaCuusModelLayout holds raw `const FProperty*` per field (VaCuusModelLayout.h's
 * FVaCuusModelField::Property), justified there by "an already-linked type's FProperty chain is
 * not rewritten at runtime". That is true of a NATIVE type and false of a UUserDefinedStruct in
 * the editor, which is exactly the case the strong reference was added for.
 *
 * THE CONSEQUENCES, IN ORDER OF SEVERITY, none of which this test provokes:
 *
 *  1. FVaCuusModelShadow's destructor calls Struct->DestroyStruct(Data) on a buffer laid out by
 *     the OLD properties using the NEW DestructorLink. For any heap-owning member that is a free
 *     of a pointer read from the wrong offset. This is the one that corrupts the heap, and it is
 *     why the test below builds a LAYOUT and never a shadow.
 *  2. Sample(), Publish() and ApplyUpdate() all dereference Fields[i].Property, i.e. freed
 *     memory, every frame.
 *  3. RmlUi keeps a raw void* to the UI shadow's base and revalidates it never, so even a
 *     correct rebuild would have to happen on the UI thread with the context still up.
 *
 * SCOPE, AND WHY THIS IS NOT A MERGE BLOCKER ON ITS OWN: UUserDefinedStruct recompilation is
 * editor-only -- FStructureEditorUtils lives in UnrealEd and nothing in a packaged game can
 * reach it -- so a shipped title is unaffected. In PIE it needs a designer to edit a Blueprint
 * struct WHILE a view bound over that struct is live, which the editor does not do implicitly.
 *
 * WHAT THIS TEST ASSERTS is the pair of facts the fix IS built on, so that it fails loudly if
 * either changes: the type object survives, and the properties do not. The listening arrived in
 * M6 (VaCuus-akj.16): FVaCuusStructRecompileGuard forwards every destructive PreChange into
 * UVaCuusSubsystem::NotifyStructPreRecompile, which condemns every BOUND MODEL over the type --
 * VaCuus.Model.BlueprintRecompileRefusal below drives that end to end. THIS test still runs
 * with that guard live and still passes untouched, because a bare FVaCuusModelLayout is not a
 * bound model and no walk can see it -- which is precisely what keeps it useful: it remains the
 * guard-less world, the RED EVIDENCE the refusal is measured against. Skip the guard (comment
 * out the module's StructRecompileGuard) and the refusal test's Errors vanish while THESE
 * dangling-property facts keep standing; that pairing is the restore-the-bug for a bug whose
 * unguarded form is heap corruption and cannot be run to completion deliberately.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelBlueprintRecompileTest, "VaCuus.Model.BlueprintRecompile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelBlueprintRecompileTest::RunTest(const FString& Parameters)
{
	if (!FStructureEditorUtils::UserDefinedStructEnabled())
	{
		// [UserDefinedStructure] bUseUserDefinedStructure gates the whole feature, and
		// CreateUserDefinedStruct answers null when it is off (StructureEditorUtils.cpp:42-64).
		AddInfo(TEXT("Skipped: user-defined structs are disabled in this configuration"));
		return true;
	}

	TStrongObjectPtr<UUserDefinedStruct> Struct(
		FStructureEditorUtils::CreateUserDefinedStruct(GetTransientPackage(), TEXT("VaCuusRecompileProbe"), RF_Transient));
	if (!TestTrue(TEXT("a Blueprint struct was created"), Struct.IsValid()))
	{
		return false;
	}

	// CreateUserDefinedStruct adds one bool member and compiles (StructureEditorUtils.cpp:58-60),
	// so there is something to bind before anything is edited.
	const FVaCuusModelLayout Layout(Struct.Get());
	if (!TestTrue(TEXT("the layout resolved the Blueprint struct"), Layout.IsValid()))
	{
		return false;
	}
	if (!TestEqual(TEXT("with the struct's one default member bound"), Layout.GetFields().Num(), 1))
	{
		return false;
	}

	const UScriptStruct* TypeBefore = Layout.GetStruct();
	const FProperty* PropertyBefore = Layout.GetFields()[0].Property;
	const int32 SizeBefore = Struct->GetStructureSize();
	const FString WireNameBefore = Layout.GetFields()[0].WireName;

	TArray<FStructVariableDescription>& VarDescs = FStructureEditorUtils::GetVarDesc(Struct.Get());
	if (!TestEqual(TEXT("the struct has one variable description"), VarDescs.Num(), 1))
	{
		return false;
	}
	const FGuid VarGuid = VarDescs[0].VarGuid;

	// ---- THE RECOMPILE. ----
	//
	// RenameVariable rather than AddVariable, only because it needs no FEdGraphPinType and
	// therefore no BlueprintGraph dependency in this module. It reaches the same door:
	// OnStructureChanged(RenamedVariable) -> Status = UDSS_Dirty -> CompileStructure
	// (StructureEditorUtils.cpp:347-373, :569-580). The change reason matters -- and this one is
	// not the exempt one: CleanAndSanitizeStruct skips the property teardown ONLY for
	// DefaultValueChanged (UserDefinedStructureCompilerUtils.cpp:221-232).
	if (!TestTrue(TEXT("the variable was renamed, which recompiles the struct"),
			FStructureEditorUtils::RenameVariable(Struct.Get(), VarGuid, TEXT("RenamedByTheTest"))))
	{
		return false;
	}

	// ---- FACT 1: THE TYPE OBJECT SURVIVES. ----
	//
	// So FVaCuusModelLayout's and FVaCuusModelShadow's TStrongObjectPtr are pointing at a live,
	// valid UScriptStruct -- the half of the spec's mitigation that does work. Nothing here is
	// protected by that; it is what keeps the failure from being a null dereference.
	TestTrue(TEXT("the recompile kept the same UScriptStruct instance"), Layout.GetStruct() == TypeBefore);
	TestTrue(TEXT("and it is still alive"), Struct.IsValid());
	TestEqual(TEXT("and the layout still names it"), Layout.GetStruct(), static_cast<const UScriptStruct*>(Struct.Get()));

	// ---- FACT 2: EVERY FProperty THE LAYOUT RESOLVED IS GONE. ----
	//
	// Compared by POINTER VALUE ONLY. PropertyBefore has been `delete`d, so dereferencing it --
	// including calling GetName() on it for a nicer message -- is a use-after-free. The whole
	// question is whether the address the layout is still holding is one of the struct's
	// properties, and address comparison answers exactly that.
	bool bOldPropertyStillPresent = false;
	int32 NumPropertiesAfter = 0;
	for (const FProperty* Property = Struct->PropertyLink; Property != nullptr; Property = Property->PropertyLinkNext)
	{
		++NumPropertiesAfter;
		bOldPropertyStillPresent |= (Property == PropertyBefore);
	}

	TestEqual(TEXT("the struct still has one property afterwards"), NumPropertiesAfter, 1);
	TestFalse(TEXT("but it is NOT the one the layout resolved -- the layout's FProperty* is dangling"),
		bOldPropertyStillPresent);

	// The layout is now describing a type it no longer matches, and says nothing about it. Both
	// of these read fine because they are the layout's own copies; that is the point.
	TestEqual(TEXT("the layout's cached wire name is the OLD one"), Layout.GetFields()[0].WireName, WireNameBefore);
	TestEqual(TEXT("...while the struct now authors it differently"),
		FStructureEditorUtils::GetVariableFriendlyNameForProperty(Struct.Get(), Struct->PropertyLink),
		FString(TEXT("RenamedByTheTest")));

	AddInfo(FString::Printf(
		TEXT("Blueprint struct recompile: type object %s (same instance), size %d -> %d, layout's FProperty* %s. ")
		TEXT("The layout holds a strong ref to the TYPE, which survives; it holds RAW pointers to the PROPERTIES, ")
		TEXT("which do not. There is no rebuild hook in M3a."),
		Layout.GetStruct() == TypeBefore ? TEXT("SURVIVED") : TEXT("REPLACED"), SizeBefore, Struct->GetStructureSize(),
		bOldPropertyStillPresent ? TEXT("still valid") : TEXT("DANGLING")));

	return true;
}

namespace VaCuusModelRecompileTest
{
/**
 * The no-context stub, VaCuusLiveReloadTest's shape: what THIS file asserts about the refusal
 * is the ENGINE-HOOK half -- a real UUserDefinedStruct recompile reaching the runtime walk
 * through FVaCuusStructRecompileGuard -- plus every game-thread observable. The real-context
 * half (RemoveDataModel, the registry eviction, the recovered document) is
 * VaCuus.Model.RecompileDrop in the runtime module, which can name a native struct but can
 * never recompile one; the two tests are one feature split along the module boundary.
 */
class FStubHost final : public IVaCuusDocumentHost
{
public:
	virtual bool Initialize(uint32 InViewId, const TSharedRef<FVaCuusViewStatus>& InStatus) override { return true; }
	virtual void Shutdown() override {}
	virtual void SetViewSize(FIntPoint ViewSize) override {}
	virtual void LoadDocumentFromFile(const FString& VfsPath, uint64 LoadSerial) override {}
	virtual void LoadDocumentFromMemory(const FString& RmlSource, uint64 LoadSerial) override {}
	virtual void CloseDocument() override {}
	virtual void SetVisible(bool bVisible) override {}
	virtual bool HasView() const override { return false; }
	virtual Rml::Context* GetContext() const override { return nullptr; }
	virtual void RecordAndPublishFrame() override {}
};

/** The standalone-instance fixture, shared with VaCuusLiveReloadTest for its reason: only a game instance with a WORLD CONTEXT is visible to the walk under test. */
struct FStandaloneInstance
{
	TStrongObjectPtr<UGameInstance> GameInstance;
	UVaCuusSubsystem* Subsystem = nullptr;

	FStandaloneInstance()
	{
		GameInstance.Reset(NewObject<UGameInstance>(GEngine));
		GameInstance->InitializeStandalone();
		Subsystem = GameInstance->GetSubsystem<UVaCuusSubsystem>();
	}

	~FStandaloneInstance()
	{
		UWorld* World = GameInstance->GetWorld();
		GameInstance->Shutdown();
		if (World)
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
	}

	FStandaloneInstance(const FStandaloneInstance&) = delete;
	FStandaloneInstance& operator=(const FStandaloneInstance&) = delete;
};

/** VaCuusLiveReloadTest's precondition set, for the same three reasons it gives. */
static FString WhySkip()
{
	if (!FStructureEditorUtils::UserDefinedStructEnabled())
	{
		return TEXT("user-defined structs are disabled in this configuration");
	}
	if (!FPlatformProcess::SupportsMultithreading())
	{
		return TEXT("no multithreading support, so there is no worker thread to drive");
	}
	if (!FSlateApplication::IsInitialized())
	{
		return TEXT("no FSlateApplication, so a game instance cannot be initialized");
	}
	if (GEngine == nullptr)
	{
		return TEXT("no GEngine");
	}
	return FString();
}

/** One UI frame at a time -- the VaCuusModelTestHost helper, restated here because that header is another module's Private tree. */
static bool RunFrames(FVaCuusUIThread& UIThread, int32 NumFrames)
{
	for (int32 Index = 0; Index < NumFrames; ++Index)
	{
		const uint64 Before = UIThread.GetFrameCount();
		UIThread.Trigger();
		if (!UIThread.WaitForFrameCount(Before + 1, 5.0))
		{
			return false;
		}
	}
	return true;
}
}	 // namespace VaCuusModelRecompileTest

/**
 * THE REFUSAL the facts above demanded, driven through the REAL door (VaCuus-akj.16, spec M6
 * 2(j)): FStructureEditorUtils::RenameVariable recompiles a UUserDefinedStruct, the engine's
 * BroadcastPreChange reaches FVaCuusStructRecompileGuard (subscribed by this very module's
 * StartupModule), and the runtime walk condemns every bound model over the type -- one Error
 * each, a destroyed game shadow, a fenced UI-side drop, and a Sample that refuses from then on.
 *
 * TWO MODELS, TWO MATCH RULES. The scalar model matches as the ROOT of its own recompiled
 * struct. The rows model matches through FVaCuusModelArrayDesc::ElementLayout -- its ROOT
 * struct is untouched; what recompiles is the ELEMENT type of its array field, the surface M3b
 * doubled and the reason the match rule has a second clause at all. (The outer struct is NOT
 * recompiled as a dependent here, so each struct broadcasts exactly once: the dependent
 * discovery skips any owner UDS whose outermost is the transient package --
 * ReplaceStructWithTempDuplicate's bValidStruct, UserDefinedStructureCompilerUtils.cpp:140-146
 * -- and every struct this test creates lives in GetTransientPackage(). A real asset-package
 * outer WOULD be re-added to ChangedStructs and broadcast in the same transaction; the walk's
 * already-condemned skip is what would keep that second broadcast at zero extra Errors, and
 * the exact expected-message counts below would catch any double-firing either way.)
 *
 * THE RESTORE-THE-BUG QUESTION, answered rather than skipped: the unguarded form of this bug is
 * heap corruption inside DestroyStruct, which no test may deliberately run to completion. The
 * red evidence is therefore split: VaCuus.Model.BlueprintRecompile above IS the guard-less
 * world (a bare layout the walk cannot see) and keeps asserting that the properties dangle;
 * this test asserts the guard turns exactly that state into refusals. Un-subscribe the guard in
 * FVaCuusEditorModule::StartupModule and this test fails on every expected message while the
 * facts above stay green -- each half fails for precisely the reason the other exists.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelRecompileRefusalTest, "VaCuus.Model.BlueprintRecompileRefusal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelRecompileRefusalTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelRecompileTest;

	const FString SkipReason = WhySkip();
	if (!SkipReason.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("Skipped: %s"), *SkipReason));
		return true;
	}
	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	// The refusal's log contract, each an assertion (a message that never arrives fails the
	// test) -- WARNING/ERROR ONLY, because the framework's expected-message counter lives in
	// the GWarn-side filter that plain Log lines never reach (FMsg routes only Warning and
	// Error through the feedback context; a Log-verbosity expectation reads "found 0 times"
	// forever). The refusal's Log-verbosity lines -- the fence summary, the replacement --
	// are asserted through state here and, on a live context, in VaCuus.Model.RecompileDrop.
	// The stub host has no context, so every bind's UI half reports exactly that -- two
	// initial binds plus the recovery re-bind make three.
	AddExpectedMessagePlain(TEXT("is torn down -- its struct"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, /*Occurrences=*/2);
	AddExpectedMessagePlain(TEXT("Sample refused -- the model was torn down"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, /*Occurrences=*/1);
	AddExpectedMessagePlain(TEXT("has no Rml context"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, /*Occurrences=*/3);

	FVaCuusModule& Module = FVaCuusModule::Get();
	FVaCuusUIThread* UIThread = Module.GetOrStartUIThread();
	if (!TestNotNull(TEXT("UI thread"), UIThread))
	{
		return false;
	}

	// Declared first so it runs LAST: the instance teardown still enqueues view removals.
	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	FStandaloneInstance Instance;
	if (!TestNotNull(TEXT("UVaCuusSubsystem on the standalone game instance"), Instance.Subsystem))
	{
		return false;
	}

	UVaCuusView* View = Instance.Subsystem->CreateView(MakeUnique<FStubHost>(), FIntPoint(320, 200));
	if (!TestNotNull(TEXT("the subsystem created a view"), View))
	{
		return false;
	}

	// ---- The three Blueprint structs: a scalar model root, and an outer type carrying an
	// ---- array of a row type (CreateUserDefinedStruct seeds each with one bool member,
	// ---- StructureEditorUtils.cpp:58-60).

	TStrongObjectPtr<UUserDefinedStruct> ScalarStruct(
		FStructureEditorUtils::CreateUserDefinedStruct(GetTransientPackage(), TEXT("VaCuusRefusalScalar"), RF_Transient));
	TStrongObjectPtr<UUserDefinedStruct> RowStruct(
		FStructureEditorUtils::CreateUserDefinedStruct(GetTransientPackage(), TEXT("VaCuusRefusalRow"), RF_Transient));
	TStrongObjectPtr<UUserDefinedStruct> OuterStruct(
		FStructureEditorUtils::CreateUserDefinedStruct(GetTransientPackage(), TEXT("VaCuusRefusalOuter"), RF_Transient));
	if (!TestTrue(TEXT("three Blueprint structs were created"),
			ScalarStruct.IsValid() && RowStruct.IsValid() && OuterStruct.IsValid()))
	{
		return false;
	}

	// TArray<RowStruct> member on the outer type. PC_Struct + SubCategoryObject + Array
	// container is the shape the struct editor's own type picker produces; AddVariable
	// recompiles the outer struct on the spot (OnStructureChanged), which the guard hears
	// as a broadcast with zero bound models -- deliberately exercised before any bind.
	const FEdGraphPinType RowArrayType(UEdGraphSchema_K2::PC_Struct, NAME_None, RowStruct.Get(),
		EPinContainerType::Array, /*bIsReference=*/false, FEdGraphTerminalType());
	if (!TestTrue(TEXT("the outer struct gained a TArray<Row> member"),
			FStructureEditorUtils::AddVariable(OuterStruct.Get(), RowArrayType)))
	{
		return false;
	}

	// ---- Two bound models: the walk's two match clauses. ----

	if (!TestTrue(TEXT("the scalar model bound"), View->BindModel(TEXT("recomp_scalar"), ScalarStruct.Get())))
	{
		return false;
	}
	if (!TestTrue(TEXT("the rows model bound"), View->BindModel(TEXT("recomp_rows"), OuterStruct.Get())))
	{
		return false;
	}
	if (!TestTrue(TEXT("frames drained the binds"), RunFrames(*UIThread, 2)))
	{
		return false;
	}

	// ---- Recompile 1: the ROOT match. RenameVariable is not the exempt DefaultValueChanged
	// ---- reason, so CleanAndSanitizeStruct tears the property chain down -- and the guard
	// ---- must have condemned the model BEFORE that (one Error, counted above).

	const FGuid ScalarVarGuid = FStructureEditorUtils::GetVarDesc(ScalarStruct.Get())[0].VarGuid;
	if (!TestTrue(TEXT("the scalar struct recompiled"),
			FStructureEditorUtils::RenameVariable(ScalarStruct.Get(), ScalarVarGuid, TEXT("RenamedScalar"))))
	{
		return false;
	}

	// The refused Sample, through the public door: same type object (it survives), fresh
	// instance of the NEW layout. Two calls, ONE latched Warning -- the count above.
	FStructOnScope LiveScalar(ScalarStruct.Get());
	View->UpdateModel(FName(TEXT("recomp_scalar")), ScalarStruct.Get(), LiveScalar.GetStructMemory());
	View->UpdateModel(FName(TEXT("recomp_scalar")), ScalarStruct.Get(), LiveScalar.GetStructMemory());
	TestTrue(TEXT("the dead entry still answers HasModel"), View->HasModel(FName(TEXT("recomp_scalar"))));
	TestTrue(TEXT("the untouched rows model is not condemned yet"), View->HasModel(FName(TEXT("recomp_rows"))));

	// ---- Recompile 2: the ELEMENT match. The ROW type recompiles; the rows model's own root
	// ---- does not -- and never will here: a transient-package owner is excluded from the
	// ---- dependent set (UserDefinedStructureCompilerUtils.cpp:140-146), so this broadcast is
	// ---- the only one the rows model ever hears and the Error counted above can only have
	// ---- come from the ElementLayout clause.

	const FGuid RowVarGuid = FStructureEditorUtils::GetVarDesc(RowStruct.Get())[0].VarGuid;
	if (!TestTrue(TEXT("the row struct recompiled"),
			FStructureEditorUtils::RenameVariable(RowStruct.Get(), RowVarGuid, TEXT("RenamedRow"))))
	{
		return false;
	}

	// ---- No crash is half the acceptance; the other half is that the system stays usable:
	// ---- frames run, and the name recovers through the replacement re-bind.

	if (!TestTrue(TEXT("frames still run after both refusals"), RunFrames(*UIThread, 2)))
	{
		return false;
	}
	if (!TestTrue(TEXT("the recovery re-bind replaced the dead entry"),
			View->BindModel(TEXT("recomp_scalar"), ScalarStruct.Get())))
	{
		return false;
	}
	if (!TestTrue(TEXT("frames drained the recovery bind"), RunFrames(*UIThread, 2)))
	{
		return false;
	}

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
