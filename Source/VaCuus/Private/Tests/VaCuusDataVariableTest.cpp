// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusDataVariable.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusModelLayout.h"
#include "VaCuusModelShadow.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "VaCuusModelLayoutTestTypes.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"

#include <atomic>
#include "UObject/UnrealType.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/DataModelHandle.h>

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusDataVariableTest
{
/**
 * Everything the document exposes, read back out of the DOM after one Context::Update().
 *
 * READ AS ATTRIBUTES, NOT AS TEXT, and that is not a shortcut: DataViewAttribute::Update
 * runs the same expression, gets the same Variant and converts it with the same
 * `variant.Get<String>()` as the text view (DataViewDefault.cpp:66-86 against :341-380),
 * but it lands somewhere a test can read without a font engine or a laid-out text run.
 */
struct FObserved
{
	FString Ratio;
	FString Score;
	FString Level;
	FString NativeBool;
	FString BitOne;
	FString BitTwo;
	FString Title;
	FString Tag;
	FString Caption;
	FString Utf8Note;
	FString AnsiNote;
	FString Colour;
	FString Icon;
	FString OriginX;
	FString OriginY;
	FString Missing;
};

/** The model name in the document's `data-model` attribute. */
static const char* GModelName = "hud";

/**
 * Every supported kind bound through `data-attr-p`, plus the two probes that matter as
 * much as the values: a member the struct does NOT have, and a document-side ASSIGNMENT.
 *
 * `Origin.size` is deliberate. RmlUi handles `address.name == "size"` INSIDE
 * ArrayDefinition::Child (DataVariable.h:151-152) and nowhere else, so a struct asked for
 * it must produce a diagnostic and an empty DataVariable rather than dereference a null
 * definition -- DataVariable::Get does not null-check `definition` at all
 * (DataVariable.cpp:5-8).
 *
 * bBitfieldBool and bBitfieldTwo are seeded 0 and 1: they share a storage byte and an
 * element size and differ only in FieldMask, so reading them through RmlUi as different
 * values is the end-to-end form of the mask-aware read.
 */
static const TCHAR* GDocument = TEXT(R"(<rml>
<head><style>body { display: block; } div { display: block; }</style></head>
<body data-model="hud">
	<div id="ratio"   data-attr-p="Ratio"/>
	<div id="score"   data-attr-p="Score"/>
	<div id="level"   data-attr-p="Level"/>
	<div id="nbool"   data-attr-p="bNativeBool"/>
	<div id="bit1"    data-attr-p="bBitfieldBool"/>
	<div id="bit2"    data-attr-p="bBitfieldTwo"/>
	<div id="title"   data-attr-p="Title"/>
	<div id="tag"     data-attr-p="Tag"/>
	<div id="caption" data-attr-p="Caption"/>
	<div id="utf8"    data-attr-p="Utf8Note"/>
	<div id="ansi"    data-attr-p="AnsiNote"/>
	<div id="colour"  data-attr-p="Colour"/>
	<div id="icon"    data-attr-p="Icon"/>
	<div id="originx" data-attr-p="Origin.X"/>
	<div id="originy" data-attr-p="Origin.Y"/>
	<div id="missing" data-attr-p="Origin.size"/>
	<div id="btn"     data-event-click="Ratio = 99"/>
</body>
</rml>)");

/**
 * The probe host: a real Rml::Context on the real UI thread, running the real bind.
 *
 * WHY A HOST AND NOT A BARE CONTEXT ON THE TEST THREAD. FVaCuusDefinitionRegistry asserts
 * FVaCuusUIThread::IsInUIThread(), and that is backed by GVaCuusUIThreadId, which only the
 * UI thread's own boot publishes (VaCuusUIThread.cpp:665-669). Booting RmlUi from an
 * automation thread -- what VaCuus.Core.Boot does -- claims the LIBRARY without ever
 * making that thread the UI thread, so the assert would fire. Driving a real UI thread is
 * therefore not ceremony: it is the only configuration in which this code is allowed to
 * run at all, which is exactly the property worth testing.
 *
 * THREAD HAND-OFF: plain members written on the UI thread, read on the test thread only
 * after WaitForFrameCount() saw the frame counter advance, which the UI thread stores with
 * release ordering after RunFrame() returns.
 */
class FProbeHost final : public IVaCuusDocumentHost
{
public:
	virtual bool Initialize(uint32 InViewId, const TSharedRef<FVaCuusViewStatus>& InStatus) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		Status = InStatus;
		ContextName = FString::Printf(TEXT("vacuus_databind_view_%u"), InViewId);

		Context = Rml::CreateContext(Rml::String(TCHAR_TO_UTF8(*ContextName)), Rml::Vector2i(1, 1));
		if (Context == nullptr)
		{
			return false;
		}

		Layout = FVaCuusModelLayout(FVaCuusLayoutTestModel::StaticStruct());
		Shadow = FVaCuusModelShadow(FVaCuusLayoutTestModel::StaticStruct());
		SeedShadow();

		// THE REGISTRY IS PROCESS-WIDE, so "was it built or reused" is a property of this
		// process, not of this view. Recorded either way; the test asserts the second lookup
		// neither grew the map nor produced a different object.
		RegistryNumBefore = FVaCuusDefinitionRegistry::Num();
		Definitions = FVaCuusDefinitionRegistry::GetOrCreate(Layout);
		RegistryNumAfterFirst = FVaCuusDefinitionRegistry::Num();
		SecondLookup = FVaCuusDefinitionRegistry::GetOrCreate(Layout);
		RegistryNumAfterSecond = FVaCuusDefinitionRegistry::Num();

		// BEFORE LoadDocument, and that ordering is RmlUi's, not ours: `data-model` is read
		// exactly once, in Element::SetParent (Element.cpp:2203-2219), with no retry.
		Rml::DataModelConstructor Constructor = Context->CreateDataModel(GModelName);
		if (!Constructor)
		{
			return false;
		}

		ModelHandle = Constructor.GetModelHandle();
		NumBound = VaCuusData::BindModelVariables(Constructor, Layout, Shadow);

		return true;
	}

	virtual void Shutdown() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		CloseDocument();
		if (Context)
		{
			Rml::RemoveContext(Rml::String(TCHAR_TO_UTF8(*ContextName)));
			Context = nullptr;
		}

		// The shadow outlives nothing: RmlUi holds a raw void* into it and there is no unbind
		// API, so the context has to go first. That ordering is the whole reason the shadow is
		// a member of the host rather than of the test.
		Shadow.Reset();
	}

	virtual void SetViewSize(FIntPoint InViewSize) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		ViewSize = InViewSize;
		if (Context)
		{
			Context->SetDimensions(Rml::Vector2i(ViewSize.X, ViewSize.Y));
		}
	}

	virtual void LoadDocumentFromFile(const FString& VfsPath, uint64 LoadSerial) override { Report(LoadSerial, /*bSuccess=*/false); }

	virtual void LoadDocumentFromMemory(const FString& RmlSource, uint64 LoadSerial) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		if (Context == nullptr)
		{
			Report(LoadSerial, /*bSuccess=*/false);
			return;
		}

		Rml::ElementDocument* NewDocument =
			Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(*RmlSource)), "vacuus://databind.rml");
		if (NewDocument == nullptr)
		{
			Report(LoadSerial, /*bSuccess=*/false);
			return;
		}

		CloseDocument();
		RmlDocument = NewDocument;
		RmlDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document);
		Report(LoadSerial, /*bSuccess=*/true);
	}

	virtual void CloseDocument() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		if (RmlDocument)
		{
			RmlDocument->Close();
			RmlDocument = nullptr;
		}
	}

	virtual void SetVisible(bool bVisible) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		if (RmlDocument)
		{
			bVisible ? RmlDocument->Show() : RmlDocument->Hide();
		}
	}

	virtual bool HasView() const override
	{
		check(FVaCuusUIThread::IsInUIThread());
		return Context != nullptr && RmlDocument != nullptr && ViewSize.X > 0 && ViewSize.Y > 0;
	}

	virtual Rml::Context* GetContext() const override
	{
		check(FVaCuusUIThread::IsInUIThread());
		return Context;
	}

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		// PHASES ARE REQUESTED, NOT COUNTED, and the frame counter is why. Enqueuing a
		// command wakes the UI thread (FVaCuusUIThread::Enqueue -> Trigger), so the frames
		// that carry AddView and LoadDocument are not the frames the test asked for -- a
		// phase keyed on "how many frames have run" starts one phase ahead and stays there.
		// An extra frame here is a plain Update(), which is exactly what an idle view does.
		const int32 Requested = RequestedPhase.load(std::memory_order_acquire);
		if (Requested > CompletedPhase.load(std::memory_order_relaxed))
		{
			RunPhase(Requested);
			CompletedPhase.store(Requested, std::memory_order_release);
		}
		else
		{
			Context->Update();
		}

		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

	/** Test thread -> UI thread: the phase to run on the next recorded frame. */
	std::atomic<int32> RequestedPhase{0};

	/** UI thread -> test thread: the highest phase that has run. */
	std::atomic<int32> CompletedPhase{-1};

	//~ Post-frame observations; see the class comment for why plain members are safe.
	int32 NumBound = 0;
	int32 RegistryNumBefore = 0;
	int32 RegistryNumAfterFirst = 0;
	int32 RegistryNumAfterSecond = 0;
	const FVaCuusModelDefinitions* Definitions = nullptr;
	const FVaCuusModelDefinitions* SecondLookup = nullptr;
	int32 RefusedSetsBefore = 0;
	int32 RefusedSetsAfter = 0;
	float ShadowRatioAfterClick = 0.f;
	FObserved Initial;
	FObserved AfterClick;
	FObserved AfterDirty;

	/** Read on the test thread for the definition-shape assertions. Immutable after Initialize(). */
	FVaCuusModelLayout Layout;

private:
	void RunPhase(int32 Phase)
	{
		switch (Phase)
		{
			case 0:
				Context->Update();
				Initial = Capture();
				break;

			case 1:
				// SPEC 4 / I3. `data-event-click="Ratio = 99"` reaches VariableDefinition::Set
				// with no VaCuus code in between; the whole milestone rests on that write being
				// refused.
				RefusedSetsBefore = VaCuusData::GetNumRefusedSets();
				if (Rml::Element* Button = RmlDocument->GetElementById("btn"))
				{
					Button->Click();
				}
				Context->Update();
				RefusedSetsAfter = VaCuusData::GetNumRefusedSets();
				ShadowRatioAfterClick = ReadShadowRatio();
				AfterClick = Capture();
				break;

			case 2:
				// The read path after a legitimate write: exactly what Task 6's apply will do --
				// copy into the shadow, then dirty the top-level name.
				WriteShadowRatio(7.5f);
				ModelHandle.DirtyVariable("Ratio");
				Context->Update();
				AfterDirty = Capture();
				break;

			default:
				Context->Update();
				break;
		}
	}

	void Report(uint64 LoadSerial, bool bSuccess)
	{
		if (Status.IsValid() && LoadSerial != 0)
		{
			Status->LoadResult.store(
				static_cast<uint8>(bSuccess ? EVaCuusLoadResult::Succeeded : EVaCuusLoadResult::Failed), std::memory_order_relaxed);
			Status->LoadCompletedSerial.store(LoadSerial, std::memory_order_release);
		}
	}

	/**
	 * Fills the shadow the way the game thread will: build the value, then hand the whole
	 * struct to CopyScriptStruct. Per-property writes would be testing this test.
	 */
	void SeedShadow()
	{
		FVaCuusLayoutTestModel Source;
		Source.Ratio = 0.25f;
		Source.Score = -7;
		Source.Level = 200;
		Source.bNativeBool = true;
		Source.bBitfieldBool = 0;
		Source.bBitfieldTwo = 1;
		Source.Title = TEXT("Hello");
		Source.Tag = FName(TEXT("HeroUnit"));
		Source.Caption = FText::FromString(TEXT("Ready"));
		Source.Utf8Note = UTF8TEXT("caf\xC3\xA9");
		Source.AnsiNote = "ansi-note";
		Source.Colour = EVaCuusTestColour::Blue;
		Source.Icon = TSoftObjectPtr<UObject>(FSoftObjectPath(TEXT("/Game/UI/Icon.Icon")));
		Source.Origin.X = 11.f;
		Source.Origin.Y = 22.f;

		FVaCuusLayoutTestModel::StaticStruct()->CopyScriptStruct(Shadow.GetData(), &Source);
	}

	const FFloatProperty* RatioProperty() const
	{
		const FVaCuusModelField* Field = Layout.FindField(TEXT("Ratio"));
		return Field != nullptr ? CastField<FFloatProperty>(Field->Property) : nullptr;
	}

	float ReadShadowRatio() const
	{
		const FVaCuusModelField* Field = Layout.FindField(TEXT("Ratio"));
		const FFloatProperty* Property = RatioProperty();
		return (Field != nullptr && Property != nullptr)
			? Property->GetPropertyValue_InContainer(Field->ContainerPtr(Shadow.GetData()))
			: 0.f;
	}

	void WriteShadowRatio(float Value)
	{
		const FVaCuusModelField* Field = Layout.FindField(TEXT("Ratio"));
		if (const FFloatProperty* Property = RatioProperty())
		{
			Property->SetPropertyValue_InContainer(Field->ContainerPtr(Shadow.GetData()), Value);
		}
	}

	FString Attribute(const char* ElementId) const
	{
		if (RmlDocument == nullptr)
		{
			return FString();
		}

		Rml::Element* Element = RmlDocument->GetElementById(ElementId);
		return Element != nullptr ? FString(UTF8_TO_TCHAR(Element->GetAttribute<Rml::String>("p", Rml::String()).c_str())) : FString();
	}

	FObserved Capture() const
	{
		FObserved Out;
		Out.Ratio = Attribute("ratio");
		Out.Score = Attribute("score");
		Out.Level = Attribute("level");
		Out.NativeBool = Attribute("nbool");
		Out.BitOne = Attribute("bit1");
		Out.BitTwo = Attribute("bit2");
		Out.Title = Attribute("title");
		Out.Tag = Attribute("tag");
		Out.Caption = Attribute("caption");
		Out.Utf8Note = Attribute("utf8");
		Out.AnsiNote = Attribute("ansi");
		Out.Colour = Attribute("colour");
		Out.Icon = Attribute("icon");
		Out.OriginX = Attribute("originx");
		Out.OriginY = Attribute("originy");
		Out.Missing = Attribute("missing");
		return Out;
	}

	TSharedPtr<FVaCuusViewStatus> Status;
	FString ContextName;
	Rml::Context* Context = nullptr;
	Rml::ElementDocument* RmlDocument = nullptr;
	Rml::DataModelHandle ModelHandle;
	FVaCuusModelShadow Shadow;
	FIntPoint ViewSize = FIntPoint::ZeroValue;
};

/** One UI frame at a time; the wake event coalesces, so N triggers are not N frames. */
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

/** Asks for a phase and pumps frames until the host reports it done. */
static bool RunPhase(FVaCuusUIThread& UIThread, FProbeHost& Host, int32 Phase)
{
	Host.RequestedPhase.store(Phase, std::memory_order_release);

	const double Deadline = FPlatformTime::Seconds() + 10.0;
	while (Host.CompletedPhase.load(std::memory_order_acquire) < Phase)
	{
		if (FPlatformTime::Seconds() > Deadline || !RunFrames(UIThread, 1))
		{
			return false;
		}
	}

	return true;
}
}	 // namespace VaCuusDataVariableTest

/**
 * THE ADAPTER, END TO END: a USTRUCT described only at runtime drives a real RmlUi
 * document, one definition object per property, no per-type codegen anywhere -- and the
 * document cannot write back.
 *
 * What is production here and what is not: only the driving is a test rig (the probe host
 * and the phase counter). The layout, the shadow, the definitions, the registry, the bind
 * and the Rml::Context are all the real ones, on the real UI thread, and the values are
 * read back out of the real DOM after a real Context::Update().
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusDataBindingTest, "VaCuus.Model.Binding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusDataBindingTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusDataVariableTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no UI thread to drive"));
		return true;
	}

	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	// `Origin.size` resolves its ROOT and then fails on the segment, which is the whole
	// point of binding it: the failure must name the struct and the member, not crash.
	// Occurrences 0 == "at least once", because the view re-evaluates whenever Origin is
	// dirtied and the count is not a property worth pinning.
	AddExpectedMessagePlain(
		TEXT("has no member 'size'"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, /*Occurrences=*/0);
	AddExpectedMessagePlain(TEXT("refused a document write to"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains,
		/*Occurrences=*/0);

	// RmlUi's own diagnostics for the same two events -- "Could not get value from data
	// variable", "Error during execution. Could not assign to variable." and the program dump
	// -- are deliberately NOT registered. They arrive as LogVaCuus Warnings through
	// FVaCuusSystemInterface::LogMessage, warnings do not fail an automation test, and
	// pinning the library's wording would make this test a hostage to the vendored SHA.

	FVaCuusModule& Module = FVaCuusModule::Get();
	FVaCuusUIThread* UIThread = Module.GetOrStartUIThread();
	if (!TestNotNull(TEXT("UI thread"), UIThread))
	{
		return false;
	}

	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();

	TUniquePtr<FProbeHost> OwnedHost = MakeUnique<FProbeHost>();
	FProbeHost* Host = OwnedHost.Get();

	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), FIntPoint(400, 300), Status);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GDocument, /*LoadSerial=*/1);

	if (!TestTrue(TEXT("the initial values were captured"), RunPhase(*UIThread, *Host, 0)))
	{
		return false;
	}
	if (!TestTrue(TEXT("the document loaded"),
			Status->LoadCompletedSerial.load(std::memory_order_acquire) == 1
				&& Status->LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded)))
	{
		return false;
	}

	// ---- 1. The bind itself. ----

	const TConstArrayView<FString> TopLevelNames = Host->Layout.GetTopLevelNames();
	TestEqual(TEXT("every top-level name was bound"), Host->NumBound, TopLevelNames.Num());

	if (!TestNotNull(TEXT("the registry produced definitions"), Host->Definitions))
	{
		return false;
	}
	TestEqual(TEXT("one bindable variable per top-level name"), Host->Definitions->GetTopLevelVariables().Num(), TopLevelNames.Num());

	// PROCESS-WIDE AND CACHED. The second lookup must be the same object and must not have
	// grown the map -- if it built a second set, every model of this type would carry its own
	// definitions and the "no per-instance state" argument for a process-wide registry would
	// be doing nothing.
	TestEqual(TEXT("the registry grew by exactly one model type"), Host->RegistryNumAfterFirst, Host->RegistryNumBefore + 1);
	TestEqual(TEXT("and the second lookup was a cache hit"), Host->RegistryNumAfterSecond, Host->RegistryNumAfterFirst);
	TestTrue(TEXT("returning the same definitions"), Host->SecondLookup == Host->Definitions);

	// ---- 2. The definition shapes. ----

	Rml::VariableDefinition* RatioDefinition = Host->Definitions->FindTopLevel(TEXT("Ratio"));
	Rml::VariableDefinition* OriginDefinition = Host->Definitions->FindTopLevel(TEXT("Origin"));
	if (TestNotNull(TEXT("Ratio has a definition"), RatioDefinition) && TestNotNull(TEXT("Origin has one too"), OriginDefinition))
	{
		// THE CLAIM THAT MADE DERIVING FROM BasePointerDefinition WORTH IT: its constructor
		// takes its variable type from the underlying definition (DataVariable.cpp:135), so a
		// property definition over a scalar reports Scalar without being told.
		TestTrue(TEXT("a leaf's property definition reports Scalar"), RatioDefinition->Type() == Rml::DataVariableType::Scalar);
		TestTrue(TEXT("a nested struct reports Struct"), OriginDefinition->Type() == Rml::DataVariableType::Struct);

		// And the flattened layout really did become a nested member map.
		const Rml::StringList Members = OriginDefinition->ReflectMemberNames();
		if (TestEqual(TEXT("Origin reflects both of its leaves"), int32(Members.size()), 2))
		{
			TestEqual(TEXT("the first is X"), FString(UTF8_TO_TCHAR(Members[0].c_str())), FString(TEXT("X")));
			TestEqual(TEXT("the second is Y"), FString(UTF8_TO_TCHAR(Members[1].c_str())), FString(TEXT("Y")));
		}
	}

	// ---- 3. The values, through a real Context::Update(). ----

	const FObserved& Initial = Host->Initial;

	// Numbers are compared as numbers: RmlUi formats a double with its own converter, and
	// pinning that spelling would test the library rather than the adapter.
	TestEqual(TEXT("a float reads through as its value"), FCString::Atof(*Initial.Ratio), 0.25f);
	TestEqual(TEXT("a signed int keeps its sign"), FCString::Atoi(*Initial.Score), -7);
	TestEqual(TEXT("an unsigned byte is not sign-extended"), FCString::Atoi(*Initial.Level), 200);
	TestEqual(TEXT("a nested leaf resolves through Child()"), FCString::Atof(*Initial.OriginX), 11.f);
	TestEqual(TEXT("and so does its sibling"), FCString::Atof(*Initial.OriginY), 22.f);

	// THE BITFIELD PAIR, END TO END. Both address the same byte with the same element size
	// and differ only in FieldMask, so any read that is not mask-aware returns the same value
	// for both.
	TestEqual(TEXT("a native bool reads true"), FCString::Atoi(*Initial.NativeBool), 1);
	TestEqual(TEXT("the first bitfield reads false"), FCString::Atoi(*Initial.BitOne), 0);
	TestEqual(TEXT("the second reads true from the same byte"), FCString::Atoi(*Initial.BitTwo), 1);

	TestEqual(TEXT("an FString"), Initial.Title, FString(TEXT("Hello")));
	TestEqual(TEXT("an FName, as its string"), Initial.Tag, FString(TEXT("HeroUnit")));
	TestEqual(TEXT("an FText, as its display string"), Initial.Caption, FString(TEXT("Ready")));

	// The UTF-8 kind carries a non-ASCII character, which is the whole reason its bytes are
	// copied rather than round-tripped through FString.
	TestEqual(TEXT("an FUtf8String survives as UTF-8"), Initial.Utf8Note, FString(TEXT("caf\u00E9")));
	TestEqual(TEXT("an FAnsiString"), Initial.AnsiNote, FString(TEXT("ansi-note")));

	// The enum is ONE variable holding the authored NAME (spec 3.4), not the number.
	TestEqual(TEXT("an enum reads as its authored name"), Initial.Colour, FString(TEXT("Blue")));

	// A soft reference reads as its path, and is never resolved.
	TestEqual(TEXT("a soft object reference reads as its path"), Initial.Icon, FString(TEXT("/Game/UI/Icon.Icon")));

	// A member the struct does not have produced a diagnostic (asserted above) and an empty
	// value, not a crash.
	TestEqual(TEXT("a member that does not exist reads empty"), Initial.Missing, FString());

	// ---- 4. Set() refuses: spec 4 / I3. ----

	if (!TestTrue(TEXT("the click frame ran"), RunPhase(*UIThread, *Host, 1)))
	{
		return false;
	}

	// The assignment really did reach VariableDefinition::Set...
	TestEqual(TEXT("the document's assignment reached Set() and was refused"), Host->RefusedSetsAfter, Host->RefusedSetsBefore + 1);

	// ...and the shadow is untouched. THIS is the assertion the whole invariant reduces to:
	// a write here is invisible to the game-side differ, which compares the live struct
	// against its OWN shadow, sees no change, sets no bit -- and the two shadows then
	// disagree forever with nothing on screen to show it.
	TestEqual(TEXT("and the shadow is unchanged"), Host->ShadowRatioAfterClick, 0.25f);

	// The DOM did not move either, because both RmlUi call sites skip their DirtyVariable
	// when Set returns false (DataControllerDefault.cpp:57-59, DataExpression.cpp:1185-1197).
	TestEqual(TEXT("and nothing on screen changed"), FCString::Atof(*Host->AfterClick.Ratio), 0.25f);

	// ---- 5. The read path still works afterwards. ----

	if (!TestTrue(TEXT("the dirty frame ran"), RunPhase(*UIThread, *Host, 2)))
	{
		return false;
	}

	TestEqual(TEXT("a shadow write plus DirtyVariable reaches the DOM"), FCString::Atof(*Host->AfterDirty.Ratio), 7.5f);
	TestEqual(TEXT("and an undirtied field is untouched"), Host->AfterDirty.Title, FString(TEXT("Hello")));

	UIThread->EnqueueRemoveView(ViewId);
	RunFrames(*UIThread, 1);

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
