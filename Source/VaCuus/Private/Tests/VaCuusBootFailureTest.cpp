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

namespace VaCuusBootFailureTest
{
//~ THE HOST BELOW IS FVaCuusTestNullDocumentHost(EVaCuusTestHostBoot::FailsInitialize) -- a host
//~ whose Initialize() FAILS, which is the whole point of this file. Per the AddView contract
//~ (VaCuusDocumentHost.h:53-55) a host that fails Initialize has rolled itself back and is simply
//~ dropped; that one has nothing to roll back. The failure is an ARGUMENT at the construction
//~ site rather than a buried `return false`, so the difference from the four passing stubs is
//~ visible where it is chosen.

/**
 * A standalone UGameInstance carrying a live UVaCuusSubsystem -- the VaCuusReloadTest.cpp
 * pattern, needed here because the failure under test spans the subsystem's CreateView()
 * (which returns the valid-looking handle) and the handle's own PollStatus().
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

/** Preconditions; skips rather than fails, per the reload test's argument. */
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
}	 // namespace VaCuusBootFailureTest

/**
 * A view whose host fails to boot must ADMIT it (M6 sweep, bead VaCuus-akj.13).
 *
 * The bug this pins down: CreateView() returns the handle before the UI thread drains
 * AddView (by design -- nothing RmlUi-affine on the game thread), and when Initialize()
 * then failed, nothing ever told the game thread. The handle stayed IsViewValid()==true
 * forever, every LoadDocument was enqueued and dropped at Verbose for the unknown view,
 * IsLoadPending() stuck true, and OnLoadCompleted never fired -- one UI-thread Error was
 * the only diagnostic in the whole process. The fix is FVaCuusViewStatus::BootState;
 * this test drives the full chain: drain stamps Failed, the next PollStatus() logs one
 * game-thread Error naming the view, invalidates the handle, and broadcasts
 * OnLoadCompleted(view, false) exactly once.
 *
 * RESTORE-THE-BUG: remove the Failed store in FVaCuusUIThread::AddView's Initialize
 * failure branch and this test goes red exactly as the bead describes -- the view stays
 * valid forever and the callback never fires (plus the unconsumed expected-error line).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusBootFailureTest, "VaCuus.View.BootFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusBootFailureTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusBootFailureTest;

	const FString SkipReason = WhySkip();
	if (!SkipReason.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("Skipped: %s"), *SkipReason));
		return true;
	}

	// Same argument as the reload test: a session already holding RmlUi makes the boot
	// below fail for the wrong reason, and this failure names the real one.
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

	FStandaloneInstance Instance;
	UVaCuusSubsystem* Subsystem = Instance.Subsystem;
	if (!TestNotNull(TEXT("UVaCuusSubsystem on the standalone game instance"), Subsystem))
	{
		return false;
	}

	// Both Errors are the product working as intended, and each must fire exactly once:
	// the UI thread's at the drain, the game thread's at the first poll that sees Failed.
	AddExpectedError(TEXT("failed to boot; it will produce no frames"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("never booted"), EAutomationExpectedErrorFlags::Contains, 1);

	UVaCuusView* View = Subsystem->CreateView(
		MakeUnique<FVaCuusTestNullDocumentHost>(EVaCuusTestHostBoot::FailsInitialize), FIntPoint(320, 200));
	if (!TestNotNull(TEXT("CreateView returned a handle"), View))
	{
		return false;
	}

	// The bead's premise, asserted as the starting state: before the drain the handle
	// cannot know better, so it LOOKS valid -- which is exactly why the stamp must exist.
	TestTrue(TEXT("The handle looks valid before the AddView is drained"), View->IsViewValid());

	// A load queued against the doomed view: the waiter this failure must unblock.
	View->LoadDocument(TEXT("m1_hud.rml"));
	TestTrue(TEXT("The load is pending before the drain"), View->IsLoadPending());

	int32 NumLoadCallbacks = 0;
	bool bLastCallbackSuccess = true;
	bool bViewValidInsideCallback = true;
	View->OnLoadCompleted.AddLambda(
		[&NumLoadCallbacks, &bLastCallbackSuccess, &bViewValidInsideCallback](UVaCuusView* InView, bool bSuccess)
		{
			++NumLoadCallbacks;
			bLastCallbackSuccess = bSuccess;
			bViewValidInsideCallback = InView != nullptr && InView->IsViewValid();
		});

	// One drain: AddView runs, Initialize() fails, BootState is stamped Failed.
	if (!TestTrue(TEXT("UI frames ran"), RunFrames(*UIThread, 2)))
	{
		return false;
	}

	// One poll: the first observation of Failed does the whole admission at once.
	View->PollStatus();

	TestFalse(TEXT("The handle is invalid after the poll that observed the boot failure"), View->IsViewValid());
	// The admission also gates IsLoadPending (M6 review note): the completed serial will
	// never advance for a dead view, so without the gate this answers true forever. Before
	// the poll it still answered true (asserted above) -- the flip lands AT the admission,
	// like every other observable on the handle.
	TestFalse(TEXT("Nothing is pending on the dead view once the failure is admitted"), View->IsLoadPending());
	TestEqual(TEXT("OnLoadCompleted fired exactly once"), NumLoadCallbacks, 1);
	TestFalse(TEXT("...with bSuccess=false"), bLastCallbackSuccess);
	TestFalse(TEXT("...after the handle was already invalidated (listeners see the honest state)"),
		bViewValidInsideCallback);

	// The latch: later polls of the still-Failed status must not repeat any of it.
	View->PollStatus();
	TestEqual(TEXT("A second poll does not re-fire the callback"), NumLoadCallbacks, 1);

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
