// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusEngine.h"
#include "VaCuusJsScriptHost.h"
#include "VaCuusScriptHost.h"
#include "VaCuusUIThread.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeExit.h"

#include <atomic>

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsSeamHostGatingTest, "VaCuus.Js.Seam.HostGating",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace VaCuusJsSeamTest
{
/**
 * A counting stub host: the seam's wiring observable. Statics rather than members
 * because the instance lives on the UI thread inside FVaCuusUIThread while the
 * test reads from the game thread.
 */
class FStubHost final : public IVaCuusScriptHost
{
public:
	static inline std::atomic<int32> LiveInstances{0};
	static inline std::atomic<int32> PumpFrames{0};
	static inline std::atomic<int32> GCCalls{0};
	static inline std::atomic<int32> InlineEntries{0};
	static inline std::atomic<int32> Shutdowns{0};

	static void ResetCounters()
	{
		PumpFrames = 0;
		GCCalls = 0;
		InlineEntries = 0;
		Shutdowns = 0;
	}

	FStubHost() { ++LiveInstances; }
	virtual ~FStubHost() override { --LiveInstances; }

	virtual void OnViewAdded(uint32) override {}
	virtual void OnViewRemoved(uint32) override {}
	virtual void OnDocumentReady(uint32, Rml::ElementDocument*) override {}
	virtual void OnDocumentClosing(uint32) override {}
	virtual void PumpFrame(double) override { ++PumpFrames; }
	virtual void CollectGarbage(const TCHAR*) override { ++GCCalls; }
	virtual void ExecuteScript(uint32, const FString&, const FString&) override {}
	virtual void OnInlineFrameEntry() override { ++InlineEntries; }
	virtual void Shutdown() override { ++Shutdowns; }
};

/** Trigger-then-wait, one frame at a time -- the wake event coalesces (M3 test pattern). */
bool PumpFrames(FVaCuusUIThread& Thread, int32 Count)
{
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const uint64 Before = Thread.GetFrameCount();
		Thread.Trigger();
		if (!Thread.WaitForFrameCount(Before + 1, 2.0))
		{
			return false;
		}
	}
	return true;
}
}	 // namespace VaCuusJsSeamTest

/**
 * The seam's gating contract (spec 3.1, 2(d)), end to end on the real UI thread:
 *
 *  (a) no factory ==> the thread boots hostless and frames run with no JS phase --
 *      HasScriptHost() is the observable (a factory-less thread has no other one);
 *      "no behavior change" at large is proven by the M3a/M3b suites, which run
 *      this whole plugin's test population against exactly that configuration
 *      everywhere the VaCuusJs factory is not in play;
 *  (b) a factory ==> the host is built at boot, PumpFrame and CollectGarbage run
 *      once per frame each (the two RunFrame phases), OnInlineFrameEntry never
 *      fires in threaded mode, and Shutdown() runs at thread stop;
 *  (c) vacuus.Js.Enable 0 at boot ==> no host despite a registered factory (the
 *      read-once kill switch);
 *  (d) registering while the thread is live ==> refused with the named error, the
 *      standing factory untouched -- proven by rebooting and finding the host
 *      the refused call tried to remove.
 */
bool FVaCuusJsSeamHostGatingTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsSeamTest;

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

	IConsoleVariable* EnableCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("vacuus.Js.Enable"));
	if (!TestNotNull(TEXT("vacuus.Js.Enable exists"), EnableCVar))
	{
		return false;
	}
	const int32 SavedEnable = EnableCVar->GetInt();

	// Whatever happens below, leave the process as found: thread down, cvar back,
	// and the PRODUCTION factory re-registered (this test is inside VaCuusJs, so it
	// can rebuild what FVaCuusJsModule::StartupModule registered).
	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
		EnableCVar->Set(SavedEnable, ECVF_SetByConsole);
		Module.SetScriptHostFactory(
			[]() -> TUniquePtr<IVaCuusScriptHost> { return MakeUnique<FVaCuusJsScriptHost>(); });
	};

	// (a) No factory: hostless boot, frames still run.
	{
		Module.SetScriptHostFactory(nullptr);
		FVaCuusUIThread* Thread = Module.GetOrStartUIThread();
		if (!TestNotNull(TEXT("(a) UI thread"), Thread))
		{
			return false;
		}
		TestFalse(TEXT("(a) no factory, no host"), Thread->HasScriptHost());
		TestTrue(TEXT("(a) frames run without a host"), PumpFrames(*Thread, 3));
		Module.StopUIThread();
	}

	// (b) Stub factory: host built at boot, both frame phases reach it, Shutdown at stop.
	{
		FStubHost::ResetCounters();
		Module.SetScriptHostFactory([]() -> TUniquePtr<IVaCuusScriptHost> { return MakeUnique<FStubHost>(); });

		FVaCuusUIThread* Thread = Module.GetOrStartUIThread();
		if (!TestNotNull(TEXT("(b) UI thread"), Thread))
		{
			return false;
		}
		TestTrue(TEXT("(b) the factory's host is live"), Thread->HasScriptHost());
		TestEqual(TEXT("(b) exactly one instance"), FStubHost::LiveInstances.load(), 1);

		TestTrue(TEXT("(b) frames run"), PumpFrames(*Thread, 3));
		const int32 Pumps = FStubHost::PumpFrames.load();
		TestTrue(FString::Printf(TEXT("(b) the pump phase ran with the frames (%d)"), Pumps), Pumps >= 3);
		TestEqual(TEXT("(b) and the GC point ran once per pump"), FStubHost::GCCalls.load(), Pumps);
		TestEqual(TEXT("(b) threaded mode never re-anchors the stack"), FStubHost::InlineEntries.load(), 0);

		// (d) Refused while live, with the named error -- and the standing factory intact.
		AddExpectedMessagePlain(TEXT("SetScriptHostFactory() must be called before the UI thread starts; ignored"),
			ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 1);
		Module.SetScriptHostFactory(nullptr);
		TestTrue(TEXT("(d) the live host is untouched by the refusal"), Thread->HasScriptHost());

		Module.StopUIThread();
		TestEqual(TEXT("(b) Shutdown() ran at thread stop"), FStubHost::Shutdowns.load(), 1);
		TestEqual(TEXT("(b) and the host was destroyed on the UI thread"), FStubHost::LiveInstances.load(), 0);

		// The reboot half of (d): the refused nulling changed nothing, so the stub
		// factory still stands and the next boot builds from it.
		FVaCuusUIThread* Rebooted = Module.GetOrStartUIThread();
		if (!TestNotNull(TEXT("(d) rebooted UI thread"), Rebooted))
		{
			return false;
		}
		TestTrue(TEXT("(d) the refused call left the factory standing"), Rebooted->HasScriptHost());
		Module.StopUIThread();
	}

	// (c) The kill switch, read once at boot.
	{
		EnableCVar->Set(0, ECVF_SetByConsole);
		FVaCuusUIThread* Thread = Module.GetOrStartUIThread();
		if (!TestNotNull(TEXT("(c) UI thread"), Thread))
		{
			return false;
		}
		TestFalse(TEXT("(c) vacuus.Js.Enable 0 boots hostless despite the factory"), Thread->HasScriptHost());
		TestEqual(TEXT("(c) the factory was never invoked"), VaCuusJsSeamTest::FStubHost::LiveInstances.load(), 0);
		Module.StopUIThread();
		EnableCVar->Set(SavedEnable, ECVF_SetByConsole);
	}

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
