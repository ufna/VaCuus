// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "HAL/PlatformProcess.h"
#include "VaCuusEngine.h"
#include "VaCuusUIThread.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusUIThreadLifecycleTest, "VaCuus.Threading.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusUIThreadLifecycleTest::RunTest(const FString& Parameters)
{
	if (!FPlatformProcess::SupportsMultithreading())
	{
		// FRunnableThread::Create() cannot hand back a thread here (commandlets,
		// -nothreading), and the production answer to that is the inline fallback,
		// which is a different code path with nothing to assert about a worker.
		AddInfo(TEXT("Skipped: this configuration has no multithreading support, so there is no worker thread to test"));
		return true;
	}

	// THE PRECONDITION THIS TEST NEEDS MOST, because of what it is about to build: a
	// SECOND FVaCuusUIThread, while the module already owns one per process. Run it with
	// RmlUi already booted -- a PIE session with vacuus.M1HUD up, say, which is exactly how
	// somebody runs it from the Session Frontend -- and the worker below claims a library
	// that is not free. It is refused, correctly, but the run then proves nothing about the
	// lifecycle and everything about the refusal, and until the M2 fix in
	// FVaCuusUIThread::Init() the attempt also left IsInUIThread() false on the real UI
	// thread for the rest of the process. Refuse to start rather than report a confusing
	// pass.
	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	// Engine reference resolved here, on the game thread: the worker must never look
	// the module up itself.
	FVaCuusUIThread UIThread(FVaCuusEngine::Get());
	if (!TestTrue(TEXT("Started"), UIThread.Start()))
	{
		return false;
	}

	TestNotEqual(TEXT("UI thread id differs from game thread"),
		UIThread.GetThreadId(), FPlatformTLS::GetCurrentThreadId());

	// Frames only advance when triggered — the loop must not spin.
	const uint64 Before = UIThread.GetFrameCount();
	FPlatformProcess::Sleep(0.05f);
	TestEqual(TEXT("Idle thread does not advance frames"), UIThread.GetFrameCount(), Before);

	// Triggers arriving while a frame is in flight coalesce, so the guarantee is
	// "at least one frame", never "exactly five".
	for (int32 Index = 0; Index < 5; ++Index)
	{
		UIThread.Trigger();
	}
	TestTrue(TEXT("Frames advanced after triggers"), UIThread.WaitForFrameCount(Before + 1, 2.0));

	UIThread.Stop();
	UIThread.Stop();	// idempotent
	TestFalse(TEXT("Stopped"), UIThread.IsRunning());

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
