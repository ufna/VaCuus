// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "HAL/IConsoleManager.h"
#include "VaCuusEngine.h"
#include "VaCuusScriptHost.h"
#include "VaCuusUIThread.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE INLINE VENUE, DRIVEN ON A MACHINE THAT HAS THREADS (bead VaCuus-akj.6.40).
 *
 * WHAT THE BEAD ASKED AND WHAT THE CODE ANSWERS. The bead is titled "inline mode makes
 * IsInUIThread() true on the game thread at all times", and the second half of that is false:
 * the identity is published by FVaCuusInlineUIThreadScope, which RESTORES the previous value
 * in its destructor, and that scope brackets exactly three windows -- StartInline()'s Init(),
 * each RunFrameInline(), and the destructor's Exit(). Between them the game thread is not the
 * UI thread and every check(FVaCuusUIThread::IsInUIThread()) in the plugin still catches a
 * mistake. Inside them the game thread genuinely IS RmlUi's owner, so the guard is TRUE rather
 * than vacuous: it permits exactly the calls the inline frame is entitled to make.
 *
 * WHY THAT NEEDED A TEST ANYWAY. Latching the id instead of scoping it -- i.e. making the
 * bead's title true -- is a one-line "simplification" with no visible symptom: every affinity
 * guard in the plugin would silently start accepting game-thread calls into RmlUi, and where
 * DO_CHECK is 0 the guards are not asserts at all, they are the entire enforcement of the
 * threading contract evaporating. An invariant with no observable rots; this is the observable.
 *
 * AND WHY IT IS NOT GATED ON SupportsMultithreading(). Every other UI-touching test in this
 * plugin opens with `if (!FPlatformProcess::SupportsMultithreading()) { AddInfo("Skipped");
 * return true; }` -- correctly, since they need a worker -- with the consequence that under
 * `-nothreading` the whole suite reports green having exercised nothing. StartInline() itself
 * asks no such question (it checks only IsInGameThread(), no prior Start(), and claimability),
 * so the venue can be entered deliberately from a threaded session. That is what this does,
 * and it is the only coverage inline mode has ever had.
 *
 * PRODUCTION REACHABILITY, for the record: the sole caller is FVaCuusModule::GetOrStartUIThread,
 * gated on !FPlatformProcess::SupportsMultithreading(), which is false only under `-nothreading`
 * or on a DEFAULT_NO_THREADING platform (GenericPlatformProcess.cpp:656-670). No passport venue
 * and no packaged target passes it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusInlineModeTest, "VaCuus.Threading.InlineMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusInlineModeTest::RunTest(const FString& Parameters)
{
	// The precondition VaCuus.Threading.Lifecycle needs, for the same reason: this builds a
	// SECOND FVaCuusUIThread, and a live one (a PIE session with a demo up, run from the
	// Session Frontend) owns RmlUi already. Init() would refuse, correctly, and the run would
	// then prove nothing about inline mode and everything about the refusal.
	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	// THE OBSERVER IS THE SCRIPT-HOST FACTORY, because it is the one piece of caller-supplied
	// code Init() runs -- and in inline mode Init() runs on the GAME thread inside the scope.
	// Nothing else is needed: no view, no document host, no RmlUi context.
	bool bFactoryRan = false;
	bool bFactorySawUIThread = false;
	bool bFactorySawGameThread = false;

	// vacuus.Js.Enable decides whether Init() calls the factory at all, and it is read ONCE at
	// boot, so it is forced on for the length of the boot and put straight back. Without this
	// the observation above would silently not happen in a session that turned JS off.
	IConsoleVariable* const JsEnable = IConsoleManager::Get().FindConsoleVariable(TEXT("vacuus.Js.Enable"));
	const int32 RestoreJsEnable = JsEnable ? JsEnable->GetInt() : 1;
	if (JsEnable)
	{
		JsEnable->Set(1, ECVF_SetByCode);
	}

	{
		// Captured by reference and never escapes: the factory is called during StartInline()
		// below, and the thread is destroyed before this scope ends.
		FVaCuusUIThread Inline(FVaCuusEngine::Get(),
			[&bFactoryRan, &bFactorySawUIThread, &bFactorySawGameThread]() -> TUniquePtr<IVaCuusScriptHost>
			{
				bFactoryRan = true;
				bFactorySawUIThread = FVaCuusUIThread::IsInUIThread();
				bFactorySawGameThread = IsInGameThread();

				// Null is a supported answer -- "no factory" and "a factory that declines" are
				// the same configuration to Init() -- and it keeps this test free of quickjs.
				return nullptr;
			});

		const bool bStarted = Inline.StartInline();
		if (JsEnable)
		{
			JsEnable->Set(RestoreJsEnable, ECVF_SetByCode);
		}

		if (!TestTrue(TEXT("The UI thread boots inline on the game thread"), bStarted))
		{
			return false;
		}

		TestTrue(TEXT("...and reports inline mode"), Inline.IsInlineMode());

		//~ (1) THE VENUE'S DEFINING PROPERTY, observed from inside the boot.
		TestTrue(TEXT("Init() ran the factory"), bFactoryRan);
		TestTrue(TEXT("INSIDE the inline scope the game thread IS the UI thread"), bFactorySawUIThread);

		// Both at once, and this is the honest statement of what inline mode costs: the two
		// identities coincide, so a call that is wrong on one thread and right on the other
		// cannot be distinguished here in either direction. Inline mode is not a venue in which
		// thread-affinity bugs can be found -- which is why it is not one the suite validates in.
		TestTrue(TEXT("...and it is STILL the game thread: inline mode collapses the two identities"),
			bFactorySawGameThread);

		//~ (2) THE RESTORE. This is the assertion the bead's own title would have failed, and
		//~ the one that fails if FVaCuusInlineUIThreadScope is ever turned into a latch.
		TestFalse(TEXT("BETWEEN inline frames the game thread is NOT the UI thread"),
			FVaCuusUIThread::IsInUIThread());

		//~ (3) A frame really runs on the caller, and unwinds the same way.
		const uint64 FramesBefore = Inline.GetFrameCount();
		Inline.RunFrameInline();
		TestEqual(TEXT("An inline frame runs on the calling thread and counts"),
			Inline.GetFrameCount(), FramesBefore + 1);
		TestFalse(TEXT("...and the scope is unwound again after it"), FVaCuusUIThread::IsInUIThread());

		// Trigger() is documented as a no-op inline; if it ever ran a frame instead, the count
		// would move and the "no one else runs frames" contract WaitForFrameCount rests on
		// would be false.
		const uint64 FramesAfterFrame = Inline.GetFrameCount();
		Inline.Trigger();
		TestEqual(TEXT("Trigger() runs no frame in inline mode"), Inline.GetFrameCount(), FramesAfterFrame);

		// Destruction here: no worker to join, so ~FVaCuusUIThread re-enters the scope on this
		// thread and runs Exit() -- whose own check(IsInUIThread()) is satisfied only because
		// it does. A latched id would hide that too.
	}

	//~ (4) And the identity is retracted with the thread, so nothing that follows this test
	//~ inherits a game thread that claims to be the UI thread.
	TestFalse(TEXT("After teardown the game thread is not the UI thread"), FVaCuusUIThread::IsInUIThread());
	TestFalse(TEXT("...and RmlUi is down again"), FVaCuusEngine::Get().IsInitialized());

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
