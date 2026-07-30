// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusEngine.h"
#include "VaCuusModelLayout.h"
#include "VaCuusSubsystem.h"
#include "VaCuusUIThread.h"
#include "VaCuusView.h"

#include "VaCuusModelLayoutTestTypes.h"
#include "VaCuusModelTestHost.h"

#include "Engine/GameInstance.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE PUBLIC API (M3a Task 7): UVaCuusView::BindModel / UpdateModel, and the three contracts
 * the header states.
 *
 * VaCuus.Model.Api is the WRONG-CALL MATRIX. Every one of the five specified mistakes is made
 * on purpose, and each is asserted twice: the log line it must produce (AddExpectedMessage
 * FAILS the test if the line never appears, so these are assertions rather than suppressions)
 * and the state it must not change. The state observable is NumOutstandingModelFields(), which
 * moves the instant the differ marks anything -- so "nothing was read" is checkable without a
 * round trip through the UI thread.
 *
 * VaCuus.Model.Reload is contract 3: a document reload leaves the model alone, and the model
 * keeps working afterwards with no rebind. Its structure is dictated by the heisenbug it is
 * built against -- see the comment there.
 *
 * The one case with no test is the wrong THREAD, which is a check(IsInGameThread()) exactly as
 * every other view mutator has: exercising it would abort the process.
 */
namespace VaCuusModelApiTest
{
using namespace VaCuusModelTest;

static const FName GModelName(TEXT("hud"));

/**
 * A view built the production way -- through UVaCuusSubsystem::CreateView -- on a subsystem
 * that was never added to a game instance's collection.
 *
 * THAT IS ENOUGH, AND DELIBERATELY LESS THAN VaCuus.LiveReload'S standalone game instance:
 * nothing on the path under test reads the game instance. CreateView goes to
 * FVaCuusModule::GetOrStartUIThread() for the thread, UVaCuusView::GetUIThread() goes back
 * through the subsystem to the same module, and Tick() walks the subsystem's own view array.
 * The game instance is used for one log line, which handles null. A real one, built with
 * InitializeStandalone(), would drag in FSlateApplication -- UGameInstance::Init() calls it
 * unconditionally to register a console listener -- for nothing.
 *
 * THE GAME INSTANCE IS STILL AN OBJECT, though, and cannot be skipped: UGameInstanceSubsystem
 * declares `Within = UGameInstance` (Subsystem.h), and StaticAllocateObject ensures on an
 * outer of the wrong class. It is never Init()ed, so it has no world and no subsystem
 * collection; this subsystem is constructed beside it rather than by it.
 */
struct FFixture
{
	TStrongObjectPtr<UGameInstance> GameInstance;
	TStrongObjectPtr<UVaCuusSubsystem> Subsystem;
	UVaCuusView* View = nullptr;
	FProbeHost* Host = nullptr;

	explicit FFixture(const TCHAR* ContextPrefix)
		: GameInstance(NewObject<UGameInstance>(GetTransientPackage()))
		, Subsystem(NewObject<UVaCuusSubsystem>(GameInstance.Get()))
	{
		TUniquePtr<FProbeHost> Owned = MakeUnique<FProbeHost>(ContextPrefix);
		Host = Owned.Get();
		View = Subsystem->CreateView(MoveTemp(Owned), FIntPoint(400, 300));
		if (View == nullptr)
		{
			Host = nullptr;
		}
	}

	FFixture(const FFixture&) = delete;
	FFixture& operator=(const FFixture&) = delete;

	/** One game frame: publish whatever UpdateModel marked, then run UI frames to apply it. */
	bool Frame(FVaCuusUIThread& UIThread, int32 NumUIFrames = 2)
	{
		Subsystem->Tick(0.016f);
		return RunFrames(UIThread, NumUIFrames);
	}

	void Destroy()
	{
		if (View != nullptr)
		{
			Subsystem->DestroyView(View);
		}
	}
};
}	 // namespace VaCuusModelApiTest

/**
 * The API end to end, and then every way of calling it wrong.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelApiTest, "VaCuus.Model.Api",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelApiTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelApiTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no UI thread to drive"));
		return true;
	}

	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	// EACH OF THESE IS AN ASSERTION, not a suppression: an expected message that never arrives
	// fails the test. Occurrences 1, because each mistake below is made exactly once.
	AddExpectedMessagePlain(TEXT("before anything was bound under that name"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, /*Occurrences=*/1);
	AddExpectedMessagePlain(TEXT("but the model is bound over"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains,
		/*Occurrences=*/1);
	AddExpectedMessagePlain(TEXT("was given a null pointer"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains,
		/*Occurrences=*/1);
	AddExpectedMessagePlain(TEXT("already has a model called"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains,
		/*Occurrences=*/1);
	AddExpectedMessagePlain(TEXT("needs a struct type"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains,
		/*Occurrences=*/1);
	AddExpectedMessagePlain(TEXT("after a document load was already requested"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, /*Occurrences=*/1);

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

	FFixture Fixture(TEXT("vacuus_api_view"));
	if (!TestNotNull(TEXT("the subsystem created a view"), Fixture.View))
	{
		return false;
	}

	const UScriptStruct* Type = FVaCuusSamplerDefaultsModel::StaticStruct();
	const UScriptStruct* WrongType = FVaCuusLayoutTestModel::StaticStruct();

	// ---- 1. Bind, then load. That order is contract 1. ----

	if (!TestTrue(TEXT("BindModel succeeded"), Fixture.View->BindModel(GModelName, Type)))
	{
		return false;
	}
	TestTrue(TEXT("and the view reports the model"), Fixture.View->HasModel(GModelName));

	Fixture.View->LoadDocumentFromMemory(GDocument);

	FVaCuusSamplerDefaultsModel Live;
	Live.Title = TEXT("Alpha");
	Live.Health = 42.f;
	Fixture.View->UpdateModel(GModelName, Type, &Live);

	if (!TestTrue(TEXT("frames ran"), Fixture.Frame(*UIThread, 3)))
	{
		return false;
	}

	TestEqual(TEXT("the model was bound on the UI thread"), UIThread->GetNumBoundModels(), 1);
	TestEqual(TEXT("{{Title}} shows what UpdateModel was given"), Fixture.Host->Latest.Title, FString(TEXT("Alpha")));
	TestEqual(TEXT("and so does a data-attr expression"), FCString::Atof(*Fixture.Host->Latest.Health), 42.f);

	// The steady state every wrong call below is measured against: the UI has confirmed
	// applying everything, so the differ has nothing marked.
	TestEqual(TEXT("nothing is outstanding once the UI has caught up"), Fixture.View->NumOutstandingModelFields(GModelName), 0);

	// ---- 2. The wrong-call matrix. ----

	FVaCuusSamplerDefaultsModel Rejected;
	Rejected.Title = TEXT("MustNotAppear");
	Rejected.Health = 7.f;

	// (a) Called before bind -- or, equivalently, under a name nothing was bound as.
	Fixture.View->UpdateModel(FName(TEXT("never_bound")), Type, &Rejected);
	TestEqual(TEXT("an unbound name has no outstanding count at all"),
		Fixture.View->NumOutstandingModelFields(FName(TEXT("never_bound"))), int32(INDEX_NONE));

	// (b) Wrong type. THE ONE THAT MATTERS MOST: the layout's offsets applied to another
	// type read whatever sits at those bytes, and the first FString field makes that a crash
	// rather than a wrong number. The data pointer here really is an instance of the wrong
	// type, so a missing check would genuinely misread it.
	FVaCuusLayoutTestModel WrongTypeLive;
	WrongTypeLive.Title = TEXT("MustNotAppear");
	Fixture.View->UpdateModel(GModelName, WrongType, &WrongTypeLive);
	TestEqual(TEXT("a type mismatch reads nothing"), Fixture.View->NumOutstandingModelFields(GModelName), 0);

	// (c) Null data.
	Fixture.View->UpdateModel(GModelName, Type, nullptr);
	TestEqual(TEXT("a null pointer reads nothing"), Fixture.View->NumOutstandingModelFields(GModelName), 0);

	// (d) A second model under a name already taken -- refused, because RmlUi has no unbind
	// and re-creating the name would leave the FIRST model's shadow on screen.
	TestFalse(TEXT("a duplicate model name is refused"), Fixture.View->BindModel(GModelName, Type));
	TestFalse(TEXT("and a null type is refused"), Fixture.View->BindModel(FName(TEXT("nulltype")), nullptr));
	TestFalse(TEXT("...leaving nothing bound under that name"), Fixture.View->HasModel(FName(TEXT("nulltype"))));

	// THE POSITIVE CONTROL FOR THE OBSERVABLE. Without it every assertion above would also
	// pass against a NumOutstandingModelFields() that always returned 0.
	Live.Title = TEXT("Beta");
	Fixture.View->UpdateModel(GModelName, Type, &Live);
	TestEqual(TEXT("a correct call marks exactly the field that changed"), Fixture.View->NumOutstandingModelFields(GModelName), 1);

	if (!TestTrue(TEXT("frames ran"), Fixture.Frame(*UIThread, 3)))
	{
		return false;
	}
	TestEqual(TEXT("and it reaches the document"), Fixture.Host->Latest.Title, FString(TEXT("Beta")));
	TestEqual(TEXT("while every refused call left no trace"), FCString::Atof(*Fixture.Host->Latest.Health), 42.f);
	TestEqual(TEXT("and the echo cleared it again"), Fixture.View->NumOutstandingModelFields(GModelName), 0);

	// ---- 3. Contract 1's diagnostic: binding after a load has been requested. ----

	// Warns and still binds: the model is correct for the NEXT load, which is what a live
	// reload will do. What it cannot do is attach to the document that is already up.
	TestTrue(TEXT("a late bind still succeeds"), Fixture.View->BindModel(FName(TEXT("late")), Type));
	TestTrue(TEXT("...and is registered"), Fixture.View->HasModel(FName(TEXT("late"))));

	if (!TestTrue(TEXT("frames ran"), Fixture.Frame(*UIThread, 2)))
	{
		return false;
	}
	TestEqual(TEXT("both models exist on the UI thread"), UIThread->GetNumBoundModels(), 2);

	// ---- 4. The Blueprint surface reaches the same two functions. ----
	//
	// WHAT THIS CAN AND CANNOT SHOW. execCreateModelFromStruct's only work is reading the
	// wildcard pin's FStructProperty off the script stack and calling BindModel with it -- so
	// C++ and Blueprint check the same thing by construction, not by duplication. Driving the
	// thunk itself would need a compiled Blueprint graph; what is asserted here is the shape
	// that makes the wildcard pin a wildcard pin, because losing the metadata would silently
	// turn the node into an int32 pin that binds nothing.
	{
		UFunction* Create = UVaCuusView::StaticClass()->FindFunctionByName(TEXT("CreateModelFromStruct"));
		UFunction* Update = UVaCuusView::StaticClass()->FindFunctionByName(TEXT("UpdateWholeModel"));
		if (TestNotNull(TEXT("CreateModelFromStruct is reflected"), Create) &&
			TestNotNull(TEXT("UpdateWholeModel is reflected"), Update))
		{
			TestTrue(TEXT("both are BlueprintCallable"),
				(Create->FunctionFlags & FUNC_BlueprintCallable) != 0 && (Update->FunctionFlags & FUNC_BlueprintCallable) != 0);
#if WITH_METADATA
			TestEqual(TEXT("CreateModelFromStruct's wildcard pin is Struct"),
				Create->GetMetaData(TEXT("CustomStructureParam")), FString(TEXT("Struct")));
			TestEqual(TEXT("UpdateWholeModel's wildcard pin is Struct"), Update->GetMetaData(TEXT("CustomStructureParam")),
				FString(TEXT("Struct")));
#endif
			// The placeholder really is the second parameter, which is what the thunk's single
			// StepCompiledIn<FStructProperty> assumes.
			const FProperty* Second = Update->PropertyLink != nullptr ? Update->PropertyLink->PropertyLinkNext : nullptr;
			TestTrue(TEXT("and it is the second parameter"), Second != nullptr && Second->GetFName() == FName(TEXT("Struct")));
		}
	}

	// ---- 5. A dead view refuses, and does so distinguishably. ----

	Fixture.Destroy();
	TestFalse(TEXT("the view is invalid after DestroyView"), Fixture.View->IsViewValid());

	// The model is still REGISTERED -- Invalidate() deliberately keeps the map -- so this
	// takes the dead-view branch rather than the "you never bound that" one. Verbose, because
	// UpdateModel runs at frame rate and a view can outlive its driver by a frame.
	Live.Title = TEXT("AfterDeath");
	Fixture.View->UpdateModel(GModelName, Type, &Live);
	TestTrue(TEXT("a dead view still knows about its models"), Fixture.View->HasModel(GModelName));
	TestEqual(TEXT("but reads nothing"), Fixture.View->NumOutstandingModelFields(GModelName), 0);

	RunFrames(*UIThread, 1);
	TestEqual(TEXT("removing the view dropped both models on the UI thread"), UIThread->GetNumBoundModels(), 0);

	return true;
}

/**
 * CONTRACT 3: a document reload leaves the model alone, and the model keeps updating with no
 * rebind.
 *
 * THE ORDER OF THE LAST TWO STEPS IS THE WHOLE TEST, and it is the heisenbug this milestone is
 * built against. A newly added DataView is updated UNCONDITIONALLY -- DataViews::Update pushes
 * everything in `views_to_add` into `dirty_views` before it looks at a single dirty variable
 * (DataView.cpp:78-90) -- so a reload re-reads every value from the UI shadow whether or not
 * anything was dirtied. Writing a value, reloading, and then asserting it would therefore pass
 * with the dirty flag missing entirely. So the write that proves the model still works happens
 * AFTER the reload, with no reload between it and its assertion.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelReloadTest, "VaCuus.Model.Reload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelReloadTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelApiTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no UI thread to drive"));
		return true;
	}

	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

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

	FFixture Fixture(TEXT("vacuus_reload_view"));
	if (!TestNotNull(TEXT("the subsystem created a view"), Fixture.View))
	{
		return false;
	}

	const UScriptStruct* Type = FVaCuusSamplerDefaultsModel::StaticStruct();

	if (!TestTrue(TEXT("BindModel succeeded"), Fixture.View->BindModel(GModelName, Type)))
	{
		return false;
	}
	Fixture.View->LoadDocumentFromMemory(GDocument);

	FVaCuusSamplerDefaultsModel Live;
	Live.Title = TEXT("BeforeReload");
	Live.Health = 11.f;
	Fixture.View->UpdateModel(GModelName, Type, &Live);

	if (!TestTrue(TEXT("frames ran"), Fixture.Frame(*UIThread, 3)))
	{
		return false;
	}
	TestEqual(TEXT("the first document shows the value"), Fixture.Host->Latest.Title, FString(TEXT("BeforeReload")));
	TestEqual(TEXT("one document loaded so far"), Fixture.Host->NumDocumentsLoaded, 1);

	// ---- The reload. NOTHING is done to the model: no unbind (there is none), no rebind. ----

	Fixture.View->LoadDocumentFromMemory(GDocument);

	if (!TestTrue(TEXT("frames ran"), Fixture.Frame(*UIThread, 3)))
	{
		return false;
	}
	TestEqual(TEXT("the document really was rebuilt"), Fixture.Host->NumDocumentsLoaded, 2);
	TestTrue(TEXT("the model is still bound, with no rebind"), Fixture.View->HasModel(GModelName));
	TestEqual(TEXT("still one model on the UI thread, not two"), UIThread->GetNumBoundModels(), 1);

	// The new document resolved `data-model` against the SAME model and read the SAME UI
	// shadow. This assertion is the weak one -- see the class comment: a newly added view is
	// unconditionally dirty, so it would hold even if dirtying were broken. It is here to
	// pin that the model survived at all, not that dirtying works.
	TestEqual(TEXT("and the reloaded document shows the value the model still holds"), Fixture.Host->Latest.Title,
		FString(TEXT("BeforeReload")));

	// ---- The load-bearing half: a write AFTER the reload, asserted with no reload in between. ----

	Live.Title = TEXT("AfterReload");
	Live.Health = 22.f;
	Fixture.View->UpdateModel(GModelName, Type, &Live);
	TestEqual(TEXT("two fields changed"), Fixture.View->NumOutstandingModelFields(GModelName), 2);

	if (!TestTrue(TEXT("frames ran"), Fixture.Frame(*UIThread, 3)))
	{
		return false;
	}

	TestEqual(TEXT("the model keeps driving the reloaded document"), Fixture.Host->Latest.Title, FString(TEXT("AfterReload")));
	TestEqual(TEXT("...on every bound expression"), FCString::Atof(*Fixture.Host->Latest.Health), 22.f);
	TestEqual(TEXT("and the echo still comes back"), Fixture.View->NumOutstandingModelFields(GModelName), 0);
	TestEqual(TEXT("no further reload happened around the write"), Fixture.Host->NumDocumentsLoaded, 2);

	Fixture.Destroy();
	RunFrames(*UIThread, 1);

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
