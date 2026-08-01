// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusDataVariable.h"
#include "VaCuusEngine.h"
#include "VaCuusModelLayoutTestTypes.h"
#include "VaCuusModelTestHost.h"
#include "VaCuusSubsystem.h"
#include "VaCuusUIThread.h"
#include "VaCuusView.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusModelRecompileDropTest
{
using namespace VaCuusModelTest;

/**
 * The standalone-instance fixture, VaCuusReloadTest's shape verbatim and for its reason:
 * UVaCuusSubsystem::NotifyStructPreRecompile finds views by the SAME world-context walk the
 * reload and dump entry points use, so a subsystem that is not in GEngine->GetWorldContexts()
 * is invisible to the very code under test. InitializeStandalone() is what puts it there
 * (a Game world context, GameInstance.cpp:193). The Slate question is settled in that file's
 * WhySkip comment: a process that is neither a commandlet nor a dedicated server has one.
 */
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
}	 // namespace VaCuusModelRecompileDropTest

/**
 * THE RECOMPILE REFUSAL'S RUNTIME MACHINERY, END TO END ON A REAL CONTEXT (VaCuus-akj.16,
 * spec M6 2(j)): condemn -> fenced UI-side drop (RemoveDataModel, registry drops, buffer
 * teardown through the drop-state machine) -> refused Sample -> recovery re-bind through the
 * stale-definitions eviction -> a document showing live values again.
 *
 * DRIVEN WITH A NATIVE STRUCT, DELIBERATELY. NotifyStructPreRecompile is type-agnostic --
 * it matches on pointer identity -- and a native type is the only one this Runtime module
 * can name (FStructureEditorUtils lives in UnrealEd). What a native struct cannot do is
 * actually lose its property chain, so the two halves split cleanly across two tests:
 *
 *   - THIS test proves the runtime pipeline on a live Rml::Context, where the strongest
 *     observable exists: GetNumBoundModels() reads 0 THE MOMENT the notify call returns,
 *     which is the fence -- the UI-side teardown provably ran inside the caller's window,
 *     not on some later frame.
 *   - VaCuus.Model.BlueprintRecompileRefusal (VaCuusEditor) proves the editor half: a REAL
 *     UUserDefinedStruct recompile reaching this same pipeline through the engine's
 *     PreChange broadcast, ElementLayout matching included.
 *
 * The red evidence for the whole feature is VaCuus.Model.BlueprintRecompile in that same
 * editor file: the fact-recording test IS the guard-less world (it builds a bare layout no
 * walk can see), and its dangling-FProperty assertions are what every teardown step here
 * exists to outrun.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelRecompileDropTest, "VaCuus.Model.RecompileDrop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelRecompileDropTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelRecompileDropTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no worker thread to drive"));
		return true;
	}
	if (GEngine == nullptr)
	{
		AddInfo(TEXT("Skipped: no GEngine, so there is no world-context walk to drive"));
		return true;
	}
	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	// EACH OF THESE IS AN ASSERTION -- an expected message that never arrives fails the test.
	// WARNING/ERROR ONLY, and that constraint is the framework's, learned the red way: the
	// expected-message counter lives in the GWarn-side message filter, which plain Log lines
	// never reach (FMsg routes only Warning and Error through the feedback context), so a
	// Log-verbosity expectation reads "found 0 time(s)" forever. Everything the refusal says
	// at Log verbosity -- the fence summary, the stale mark, the rebuild, the replacement --
	// is asserted below through STATE (bound-model count, the eviction counter, BindModel's
	// return) instead of through its log line.
	AddExpectedMessagePlain(TEXT("is torn down -- its struct"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, /*Occurrences=*/1);
	AddExpectedMessagePlain(TEXT("Sample refused -- the model was torn down"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, /*Occurrences=*/1);

	// The recovery re-bind necessarily happens after loads were requested, so BindModel's
	// contract-1 Warning fires once -- and its advice ("only the NEXT load attaches") is
	// exactly what the recovery sequence then does.
	AddExpectedMessagePlain(TEXT("after a document load was already requested"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, /*Occurrences=*/1);

	FVaCuusModule& Module = FVaCuusModule::Get();
	FVaCuusUIThread* UIThread = Module.GetOrStartUIThread();
	if (!TestNotNull(TEXT("UI thread"), UIThread))
	{
		return false;
	}

	// Declared first so it runs LAST: the instance teardown below still enqueues its view
	// removal into the thread.
	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	FStandaloneInstance Instance;
	if (!TestNotNull(TEXT("UVaCuusSubsystem on the standalone game instance"), Instance.Subsystem))
	{
		return false;
	}

	TUniquePtr<FProbeHost> OwnedHost = MakeUnique<FProbeHost>(TEXT("vacuus_recompile_drop"));
	FProbeHost* Host = OwnedHost.Get();
	UVaCuusView* View = Instance.Subsystem->CreateView(MoveTemp(OwnedHost), FIntPoint(400, 300));
	if (!TestNotNull(TEXT("the subsystem created a view"), View))
	{
		return false;
	}

	const UScriptStruct* Type = FVaCuusSamplerDefaultsModel::StaticStruct();

	// ---- A live model on a live document, so the drop has everything to tear down. ----

	if (!TestTrue(TEXT("BindModel succeeded"), View->BindModel(TEXT("hud"), Type)))
	{
		return false;
	}
	View->LoadDocumentFromMemory(GDocument);

	FVaCuusSamplerDefaultsModel Live;
	Live.Title = TEXT("Alpha");
	View->UpdateModel(FName(TEXT("hud")), Type, &Live);
	Instance.Subsystem->Tick(0.016f);
	if (!TestTrue(TEXT("frames ran"), RunFrames(*UIThread, 3)))
	{
		return false;
	}

	TestEqual(TEXT("the document shows the pushed value"), Host->Latest.Title, FString(TEXT("Alpha")));
	if (!TestEqual(TEXT("one model is bound on the UI thread"), UIThread->GetNumBoundModels(), 1))
	{
		return false;
	}

	// ---- THE REFUSAL, and the fence read at its sharpest. ----

	const uint64 EvictionsBefore = FVaCuusDefinitionRegistry::GetNumStaleEvictions();

	const int32 NumRefused = UVaCuusSubsystem::NotifyStructPreRecompile(Type);
	TestEqual(TEXT("exactly the one model over the struct was refused"), NumRefused, 1);

	// No frame has been asked for BETWEEN the call and this line: a 0 here can only mean the
	// UI thread drained the drop INSIDE the notify call's fence -- which is the property the
	// whole 2(j) design ("fenced-synchronous first, Abandon() only on timeout") stands on.
	TestEqual(TEXT("the UI-side drop ran inside the fence window (bound models 0 on return)"),
		UIThread->GetNumBoundModels(), 0);

	// The game-side gate: refused, latched (two calls, one Warning -- the expected-message
	// count above is the assertion), and the map entry deliberately still answers HasModel.
	View->UpdateModel(FName(TEXT("hud")), Type, &Live);
	View->UpdateModel(FName(TEXT("hud")), Type, &Live);
	TestTrue(TEXT("the dead entry still answers HasModel (it is what carries the refusal)"),
		View->HasModel(FName(TEXT("hud"))));

	// The system keeps running with the condemned model still owned by the view: frames,
	// ticks, no crash. The document is still up -- only its data model is gone.
	Instance.Subsystem->Tick(0.016f);
	if (!TestTrue(TEXT("frames still run after the refusal"), RunFrames(*UIThread, 2)))
	{
		return false;
	}
	TestEqual(TEXT("the document itself survived the drop"), Host->NumDocumentsLoaded, 1);

	// ---- THE RECOVERY the refusal Error promises: re-bind, reload, live again. ----
	//
	// BindModel returning TRUE on a taken name is itself the replacement assertion: for a
	// LIVE duplicate it returns false with an Error (VaCuus.Model.Api asserts that side), so
	// true here can only be the dead-entry replacement branch. It then passes RmlUi's
	// CreateDataModel because the drop's RemoveDataModel freed the name (Context.cpp:1109),
	// and rebuilds the definition set through the stale-mark eviction (the counter below) --
	// with a NATIVE struct the properties happen to still be alive, which is exactly what
	// lets this path run to a green screen instead of a crash.
	if (!TestTrue(TEXT("the recovery re-bind succeeded"), View->BindModel(TEXT("hud"), Type)))
	{
		return false;
	}
	View->LoadDocumentFromMemory(GDocument);

	Live.Title = TEXT("Beta");
	View->UpdateModel(FName(TEXT("hud")), Type, &Live);
	Instance.Subsystem->Tick(0.016f);
	if (!TestTrue(TEXT("frames ran after the recovery"), RunFrames(*UIThread, 3)))
	{
		return false;
	}

	TestEqual(TEXT("the recovered model is bound on the UI thread again"), UIThread->GetNumBoundModels(), 1);
	TestEqual(TEXT("the reloaded document shows the recovered model's value"), Host->Latest.Title, FString(TEXT("Beta")));
	TestEqual(TEXT("the reload was a second document, not a resurrection"), Host->NumDocumentsLoaded, 2);

	// The eviction counter moved by exactly one: the notify's MarkDefinitionsStale command
	// found the cached set, and the recovery bind's GetOrCreate evicted and rebuilt it
	// rather than handing the (with a real recompile: dangling) corpse back out.
	TestEqual(TEXT("the stale definition set was evicted and rebuilt exactly once"),
		FVaCuusDefinitionRegistry::GetNumStaleEvictions(), EvictionsBefore + 1);

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
