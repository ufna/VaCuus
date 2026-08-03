// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusSubsystem.h"
#include "VaCuusTestNullDocumentHost.h"
#include "VaCuusUIThread.h"
#include "VaCuusView.h"
#include "VaCuusViewStatus.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusReloadTest
{
//~ THE VIEWS BELOW USE FVaCuusTestNullDocumentHost -- no context, no RmlUi -- and that is enough
//~ deliberately: what this file asserts is a GAME-THREAD fact (the reload entry point enqueued a
//~ cache clear, and re-issued a load for this view) plus a counter the UI thread bumps. Giving
//~ the host a real context would add an RmlUi boot and a document parse to a test that would
//~ assert nothing more.

/**
 * A standalone UGameInstance carrying a live UVaCuusSubsystem, torn down in the order the
 * subsystem needs (Shutdown() clears the world context pointer, so the world is taken
 * first).
 *
 * BUILT THE HARD WAY: a bare NewObject<UGameInstance>() has an EMPTY subsystem collection,
 * so there would be no UVaCuusSubsystem to find. UGameInstance::InitializeStandalone()
 * creates a world CONTEXT (EWorldType::Game -- GameInstance.cpp:193), a world, and then
 * runs Init(), which initializes the collection. The context is the point: it is what puts
 * this instance into GEngine->GetWorldContexts() and so lets the test drive the real walk
 * inside ClearAssetCachesAndReloadAllViews() rather than a hand-fed subsystem.
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

/**
 * Preconditions. Skips rather than fails: a session with no worker thread and no GEngine
 * cannot run this at all, and reporting that as a reload failure would be a lie.
 *
 * NO FSlateApplication::IsInitialized() CHECK, unlike its sibling in VaCuusEditor, and the
 * omission is proved rather than assumed -- asking would mean linking Slate into this
 * Runtime module for one bool. UGameInstance::Init() reaches FSlateApplication::Get() only
 * inside `if (!IsRunningCommandlet())` / `if (!IsDedicatedServerInstance())`
 * (GameInstance.cpp:102,111,113). PreInit selects exactly one mode -- commandlet, regular
 * client, dedicated server or editor, all four guarded by the same IsModeSelected()
 * assertion (LaunchEngineLoop.cpp:2205-2208, :2250-2276) -- and creates Slate for
 * `!IsRunningDedicatedServer() && (bIsRegularClient || bHasEditorToken)`
 * (LaunchEngineLoop.cpp:3108-3119). A process that is neither a commandlet nor a dedicated
 * server is therefore a regular client or the editor, which is exactly the set that HAS an
 * FSlateApplication. The bad case does not exist.
 */
static FString WhySkip()
{
	if (!FPlatformProcess::SupportsMultithreading())
	{
		return TEXT("no multithreading support, so there is no worker thread to drive");
	}
	if (GEngine == nullptr)
	{
		return TEXT("no GEngine");
	}
	return FString();
}
}	 // namespace VaCuusReloadTest

/**
 * THE CACHE CLEAR, which is what actually makes an RCSS edit visible -- and which used to
 * ride as a flag on a per-view load command, behind the drain's FindHost() gate.
 *
 * The two properties that bug violated, and that this test exists to keep:
 *
 *  1. A RELOAD THAT REACHES ZERO LIVE VIEWS STILL CLEARS. RmlUi's parsed-stylesheet and
 *     template caches are process-global statics keyed on file name, and they outlive a PIE
 *     session (UVaCuusSubsystem::Deinitialize leaves the UI thread running). So "stop PIE,
 *     edit the .rcss, press Play" must not re-load the RML from disk and then take the
 *     PREVIOUS session's stylesheet. With the clear on the load command, that flush enqueued
 *     nothing and cleared nothing -- silently, and only for RCSS, which is the edit people
 *     make most.
 *  2. ONE FAN-OUT IS ONE CLEAR, not one per view. Three reloaded views used to clear three
 *     times, and clears 2 and 3 discarded the stylesheet view 1 had just re-parsed.
 *
 * IT LIVES IN THE RUNTIME MODULE, AND THAT IS THE POINT OF ITS SECOND LIFE (bead
 * VaCuus-akj.6.34). While the only paired entry point was the editor watcher's, this test
 * exercised an editor dispatcher, and nothing at all watched the door a RUNTIME reload hook
 * would reach for -- M3's data binding being the one that was about to appear. The pairing
 * moved to UVaCuusSubsystem::ClearAssetCachesAndReloadAllViews(), the per-instance fan-out
 * went private behind it, and the coverage moved with them.
 *
 * Observed through FVaCuusUIThread::GetNumAssetCacheClears() because RmlUi offers nothing to
 * ask about its caches. Polled rather than slept on: nothing else wakes the UI thread here
 * (no world is ticking the subsystem), so each poll re-arms Trigger().
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusReloadAssetCachesTest, "VaCuus.Reload.AssetCaches",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusReloadAssetCachesTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusReloadTest;

	const FString SkipReason = WhySkip();
	if (!SkipReason.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("Skipped: %s"), *SkipReason));
		return true;
	}

	// A FAILURE rather than a skip: it describes a session perfectly capable of running the
	// test, in which somebody else has taken the one resource it needs.
	// FVaCuusModule::GetOrStartUIThread() boots RmlUi on the worker it spawns and claims
	// ownership of the library there (FVaCuusUIThread::Init), so a session that already
	// holds it -- a PIE game with vacuus.M1HUD up, which is exactly the state somebody runs
	// these from in the Session Frontend -- makes that boot fail. The test would then get a
	// null thread and report "UI thread" instead of the real reason, and its ON_SCOPE_EXIT
	// StopUIThread() would stop a thread it did not start.
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

	// How many clears the UI thread applied since Before, once at least one has landed AND
	// the frame that applied it has finished.
	//
	// WAITING ON THE COUNTER, NOT ON A FRAME, and the difference is a real (if narrow) race
	// rather than a style preference. Trigger() is a coalescing auto-reset latch
	// (FVaCuusUIThread::Trigger), so a latch left set by something else can grant a frame
	// that drains nothing of ours; and FrameCount is incremented AFTER RunFrame() returns.
	// So a frame that had already passed DrainCommands() but not yet the increment when
	// FrameBefore was sampled satisfies FrameBefore + 1 while our clear is still queued --
	// red on a correct product. The counter is the thing under test and is directly
	// observable, so wait on it.
	//
	// THE SECOND HALF IS WHY THIS DOES NOT JUST RETURN 1: property 2 above is "one fan-out
	// is ONE clear", so the count has to be read when it cannot still grow. The queue is a
	// single-producer FIFO and one DrainCommands() pops everything present, so every clear
	// the fan-out enqueued is applied within one frame -- but a second one's counter bump
	// happens a few instructions after the first's. FrameCount sampled the moment the first
	// clear is visible is <= the index of the frame doing the draining, so waiting for it to
	// advance by one is a hard barrier past that whole drain.
	const auto ClearsApplied = [this, UIThread](uint64 Before) -> uint64
	{
		const double Deadline = FPlatformTime::Seconds() + 10.0;
		while (UIThread->GetNumAssetCacheClears() <= Before)
		{
			if (FPlatformTime::Seconds() > Deadline)
			{
				TestTrue(TEXT("An asset-cache clear was applied within 10 s"), false);
				return 0;
			}
			UIThread->Trigger();
			FPlatformProcess::Sleep(0.001f);
		}

		const uint64 FrameAtObservation = UIThread->GetFrameCount();
		UIThread->Trigger();
		if (!TestTrue(TEXT("The frame that applied the clear finished"),
				UIThread->WaitForFrameCount(FrameAtObservation + 1, 10.0)))
		{
			return 0;
		}

		return UIThread->GetNumAssetCacheClears() - Before;
	};

	//~ (1) NO VIEWS AT ALL. No game instance exists yet, so the fan-out finds nothing --
	//~ which is exactly the "edited the CSS between PIE sessions" case.
	{
		const uint64 ClearsBefore = UIThread->GetNumAssetCacheClears();
		TestEqual(TEXT("(1) With no game instance, no view is reloaded"),
			UVaCuusSubsystem::ClearAssetCachesAndReloadAllViews(TEXT("automation")), 0);
		TestEqual(TEXT("(1) ...and the RmlUi asset caches are dropped anyway"),
			ClearsApplied(ClearsBefore), static_cast<uint64>(1));
	}

	//~ (2) TWO VIEWS, ONE CLEAR.
	{
		FStandaloneInstance Instance;
		UVaCuusSubsystem* Subsystem = Instance.Subsystem;
		if (!TestNotNull(TEXT("UVaCuusSubsystem on the standalone game instance"), Subsystem))
		{
			return false;
		}

		UVaCuusView* FirstView = Subsystem->CreateView(MakeUnique<FVaCuusTestNullDocumentHost>(), FIntPoint(320, 200));
		UVaCuusView* SecondView = Subsystem->CreateView(MakeUnique<FVaCuusTestNullDocumentHost>(), FIntPoint(320, 200));
		if (!TestNotNull(TEXT("First view"), FirstView) || !TestNotNull(TEXT("Second view"), SecondView))
		{
			return false;
		}

		FirstView->LoadDocument(TEXT("m1_hud.rml"));
		SecondView->LoadDocument(TEXT("m1_hud.rml"));

		const uint64 ClearsBefore = UIThread->GetNumAssetCacheClears();
		TestEqual(TEXT("(2) Both views reloaded"),
			UVaCuusSubsystem::ClearAssetCachesAndReloadAllViews(TEXT("automation")), 2);
		TestEqual(TEXT("(2) ...at the cost of exactly one cache clear, not one per view"),
			ClearsApplied(ClearsBefore), static_cast<uint64>(1));
	}

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
