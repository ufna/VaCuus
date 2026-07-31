// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusJs.h"
#include "VaCuusJsRuntime.h"
#include "VaCuusJsScriptHost.h"
#include "VaCuusJsViewContext.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "Containers/Queue.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"

#include "quickjs.h"

#include <cstring>

#if WITH_DEV_AUTOMATION_TESTS

/*
 * THE PUMP, LIBRARY-LEVEL: every test here drives an FVaCuusJsScriptHost
 * directly on the automation thread with a hand-rolled clock -- legal by the
 * Task 2 contract (no thread-identity code in quickjs; create and use on one
 * thread and it is simply correct), and the only way to pin timer semantics to
 * EXACT deadlines. The clock is seconds, like the production NowSeconds
 * (Rml::GetSystemInterface()->GetElapsedTime()); tests advance it strictly,
 * as the real clock does. The one real-thread test is at the end of this file.
 */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsPumpRafTest, "VaCuus.Js.Pump.Raf",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsPumpTimersTest, "VaCuus.Js.Pump.Timers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsPumpTimerLivelockTest, "VaCuus.Js.Pump.TimerLivelock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsPumpJobsTest, "VaCuus.Js.Pump.Jobs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsPumpJobDrainLivelockTest, "VaCuus.Js.Pump.JobDrainLivelock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsPumpConsoleTest, "VaCuus.Js.Pump.Console",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsPumpLifecycleTest, "VaCuus.Js.Pump.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsPumpUIThreadIntegrationTest, "VaCuus.Js.Pump.UIThreadIntegration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace VaCuusJsPumpTest
{
constexpr uint32 TestViewId = 11;

/**
 * Evaluates Source in the view's context and renders the result as a string --
 * the tests' read-back channel (production reads nothing back; ExecuteScript is
 * fire-and-forget). Never leaves a pending exception behind.
 */
FString EvalString(FVaCuusJsScriptHost& Host, uint32 ViewId, const char* Source)
{
	FVaCuusJsViewContext* View = Host.FindViewContext(ViewId);
	if (View == nullptr || !View->IsValid())
	{
		return TEXT("<no context>");
	}
	JSContext* Ctx = View->GetContext();

	JSValue Ret = JS_Eval(Ctx, Source, std::strlen(Source), "<pump-test-probe>", JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(Ret))
	{
		JS_FreeValue(Ctx, Ret);
		JS_FreeValue(Ctx, JS_GetException(Ctx));
		return TEXT("<exception>");
	}

	FString Result(TEXT("<unstringifiable>"));
	if (const char* Utf8 = JS_ToCString(Ctx, Ret))
	{
		Result = FString(UTF8_TO_TCHAR(Utf8));
		JS_FreeCString(Ctx, Utf8);
	}
	else
	{
		JS_FreeValue(Ctx, JS_GetException(Ctx));
	}
	JS_FreeValue(Ctx, Ret);
	return Result;
}
}	 // namespace VaCuusJsPumpTest

/**
 * rAF semantics (spec 3.5): the swap-out list. A callback registered DURING the
 * run lands next frame -- the rule the demo's class-toggle animation-restart
 * idiom depends on (hud.js:83-87) -- all callbacks of a frame share one
 * millisecond timestamp, cancelAnimationFrame works on pending AND on same-batch
 * entries, and a throwing callback never skips its siblings.
 */
bool FVaCuusJsPumpRafTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsPumpTest;

	FVaCuusJsScriptHost Host;
	Host.OnViewAdded(TestViewId);
	Host.ExecuteScript(TestViewId,
		TEXT("globalThis.seq = [];")
		TEXT("requestAnimationFrame(ts => { seq.push('first@' + ts); requestAnimationFrame(() => seq.push('second')); });"),
		TEXT("raf-setup"));

	FVaCuusJsRuntime* Runtime = Host.GetRuntime();
	if (!TestNotNull(TEXT("the first script created the runtime"), Runtime))
	{
		return false;
	}
	const auto RafRuns = [Runtime]() { return Runtime->GetNumRafCallbacksRun(); };

	// NEXT-FRAME SEMANTICS: the nested registration does not run in the pump that
	// registered it.
	Host.PumpFrame(1.0);
	TestEqual(TEXT("pump 1 ran exactly the first callback"), RafRuns(), uint64(1));
	TestEqual(TEXT("with the shared ms timestamp"), EvalString(Host, TestViewId, "seq.join('|')"),
		FString(TEXT("first@1000")));

	Host.PumpFrame(2.0);
	TestEqual(TEXT("pump 2 ran the nested callback"), RafRuns(), uint64(2));
	TestEqual(TEXT("in order"), EvalString(Host, TestViewId, "seq.join('|')"), FString(TEXT("first@1000|second")));

	Host.PumpFrame(3.0);
	TestEqual(TEXT("an empty pump runs nothing"), RafRuns(), uint64(2));

	// cancelAnimationFrame on a PENDING entry.
	Host.ExecuteScript(TestViewId,
		TEXT("globalThis.hc = requestAnimationFrame(() => seq.push('canceled')); cancelAnimationFrame(hc);"),
		TEXT("raf-cancel"));
	Host.PumpFrame(4.0);
	TestEqual(TEXT("a canceled pending callback never runs"), RafRuns(), uint64(2));
	TestEqual(TEXT("and left no trace"), EvalString(Host, TestViewId, "seq.join('|')"),
		FString(TEXT("first@1000|second")));

	// A sibling cancel DURING the run (the entry is already in the swapped-out
	// batch), plus a throwing sibling that must not take the rest down.
	AddExpectedMessagePlain(TEXT("JS exception in 'requestAnimationFrame': Error: raf boom"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);
	Host.ExecuteScript(TestViewId,
		TEXT("seq = [];")
		TEXT("requestAnimationFrame(() => { seq.push('a'); cancelAnimationFrame(hb); });")
		TEXT("globalThis.hb = requestAnimationFrame(() => seq.push('b'));")
		TEXT("requestAnimationFrame(() => { throw new Error('raf boom'); });")
		TEXT("requestAnimationFrame(() => seq.push('d'));"),
		TEXT("raf-siblings"));
	const uint64 ErrorsBefore = Runtime->GetNumErrors();
	Host.PumpFrame(5.0);
	TestEqual(TEXT("three of four ran (the canceled one skipped, counted only when run)"), RafRuns(), uint64(5));
	TestEqual(TEXT("the throw skipped nobody"), EvalString(Host, TestViewId, "seq.join('|')"), FString(TEXT("a|d")));
	TestEqual(TEXT("and was counted"), Runtime->GetNumErrors(), ErrorsBefore + 1);

	// Handles are real and monotonic (the demo returned undefined and could
	// cancel nothing -- hud-demo VacuusJs.cpp:418-424; the handle is our addition).
	TestEqual(TEXT("handles are monotonic"),
		EvalString(Host, TestViewId,
			"globalThis.hA = requestAnimationFrame(() => {});"
			"globalThis.hB = requestAnimationFrame(() => {});"
			"String(typeof hA === 'number' && hB > hA)"),
		FString(TEXT("true")));
	Host.PumpFrame(6.0);	// flush the two probes
	TestEqual(TEXT("both probes ran"), RafRuns(), uint64(7));

	// Non-function: TypeError, same contract as the timers.
	TestEqual(TEXT("requestAnimationFrame(non-function) throws TypeError"),
		EvalString(Host, TestViewId,
			"(() => { try { requestAnimationFrame(1); } catch (e) { return String(e instanceof TypeError); } return 'no-throw'; })()"),
		FString(TEXT("true")));

	return true;
}

/**
 * Timer semantics: the frame-start cutoff (a timer registered during the pump
 * waits for the next one -- the green half whose assertion the livelock test
 * breaks), intervals re-arming from FIRE time rather than from the missed
 * deadline, both clears including self-clear from inside the callback, the
 * TypeError contract, the negative-ms clamp, no extra-args forwarding, and
 * exact fired-counter deltas throughout.
 */
bool FVaCuusJsPumpTimersTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsPumpTest;

	FVaCuusJsScriptHost Host;
	Host.OnViewAdded(TestViewId);

	// THE CUTOFF, GREEN: a 0 ms self-rearming timer fires EXACTLY ONCE per pump.
	// Registered outside the pump, its deadline is the previous pump's now (the
	// registration base), strictly before the next pump's cutoff -- due; the
	// re-registration from inside the callback prices at the CURRENT cutoff
	// exactly, which the strict compare excludes until next pump.
	Host.ExecuteScript(TestViewId,
		TEXT("globalThis.fired = 0;")
		TEXT("globalThis.rearm = function() { fired++; globalThis.rid = setTimeout(rearm, 0); };")
		TEXT("globalThis.rid = setTimeout(rearm, 0);"),
		TEXT("timer-rearm"));

	FVaCuusJsRuntime* Runtime = Host.GetRuntime();
	if (!TestNotNull(TEXT("the first script created the runtime"), Runtime))
	{
		return false;
	}
	const auto TimersFired = [Runtime]() { return Runtime->GetNumTimersFired(); };

	Host.PumpFrame(1.0);
	TestEqual(TEXT("one fire on pump 1 -- the re-registration waited"), TimersFired(), uint64(1));
	Host.PumpFrame(2.0);
	TestEqual(TEXT("one more on pump 2"), TimersFired(), uint64(2));
	TestEqual(TEXT("the script agrees"), EvalString(Host, TestViewId, "fired"), FString(TEXT("2")));

	// clearTimeout between pumps stops the chain.
	EvalString(Host, TestViewId, "clearTimeout(rid); 'ok'");
	Host.PumpFrame(3.0);
	TestEqual(TEXT("cleared: no third fire"), TimersFired(), uint64(2));

	// A 5 ms deadline is honored against the fake clock: not due 4 ms later, due
	// a full second later.
	Host.ExecuteScript(TestViewId, TEXT("globalThis.late = 0; setTimeout(() => late++, 5);"), TEXT("timer-5ms"));
	Host.PumpFrame(3.004);
	TestEqual(TEXT("4 ms into a 5 ms wait: not due"), TimersFired(), uint64(2));
	Host.PumpFrame(4.0);
	TestEqual(TEXT("due once the deadline passes"), TimersFired(), uint64(3));
	TestEqual(TEXT("and it ran"), EvalString(Host, TestViewId, "late"), FString(TEXT("1")));

	// INTERVALS RE-ARM FROM FIRE TIME (hud-demo VacuusJs.cpp:549-550): after
	// firing at 5.5, a 1 s interval is next due at 6.5 -- NOT at 6.0, which
	// re-arming from the original deadline (catch-up semantics) would give. The
	// pump at 6.2 is the discriminator between the two.
	Host.ExecuteScript(TestViewId,
		TEXT("globalThis.ticks = 0; globalThis.iid = setInterval(() => ticks++, 1000);"), TEXT("timer-interval"));
	Host.PumpFrame(5.5);
	TestEqual(TEXT("interval fire 1 (registered at 4.0, due from 5.0)"), TimersFired(), uint64(4));
	Host.PumpFrame(6.2);
	TestEqual(TEXT("re-armed from FIRE time: 6.5 is not due at 6.2"), TimersFired(), uint64(4));
	Host.PumpFrame(6.6);
	TestEqual(TEXT("interval fire 2 at 6.6"), TimersFired(), uint64(5));
	TestEqual(TEXT("the script counted both"), EvalString(Host, TestViewId, "ticks"), FString(TEXT("2")));
	EvalString(Host, TestViewId, "clearInterval(iid); 'ok'");

	// Self-clear from INSIDE the callback (the dup-before-call rule's reason,
	// hud-demo VacuusJs.cpp:553): the interval fires once and unregisters itself.
	Host.ExecuteScript(TestViewId,
		TEXT("globalThis.once = 0; globalThis.sid = setInterval(() => { once++; clearInterval(sid); }, 0);"),
		TEXT("timer-selfclear"));
	Host.PumpFrame(7.0);
	TestEqual(TEXT("the self-clearing interval fired"), TimersFired(), uint64(6));
	Host.PumpFrame(8.0);
	TestEqual(TEXT("exactly once"), TimersFired(), uint64(6));
	TestEqual(TEXT("script-side too"), EvalString(Host, TestViewId, "once"), FString(TEXT("1")));

	// Clearing a pending one-shot kills it; unknown ids -- including 0, hud.js's
	// "no timer" sentinel (hud.js:81, :86) -- and cross-clears are no-ops.
	Host.ExecuteScript(TestViewId,
		TEXT("globalThis.nope = 0; const t = setTimeout(() => nope++, 0);")
		TEXT("clearTimeout(t); clearTimeout(0); clearTimeout(123456); clearInterval(t);"),
		TEXT("timer-clears"));
	Host.PumpFrame(9.0);
	TestEqual(TEXT("the cleared one-shot never fired"), TimersFired(), uint64(6));
	TestEqual(TEXT("confirmed script-side"), EvalString(Host, TestViewId, "nope"), FString(TEXT("0")));

	// A throwing timer callback does not skip its sibling; the error is counted.
	AddExpectedMessagePlain(TEXT("JS exception in 'setTimeout': Error: timer boom"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);
	Host.ExecuteScript(TestViewId,
		TEXT("globalThis.after = 0;")
		TEXT("setTimeout(() => { throw new Error('timer boom'); }, 0);")
		TEXT("setTimeout(() => after++, 0);"),
		TEXT("timer-throw"));
	const uint64 ErrorsBefore = Runtime->GetNumErrors();
	Host.PumpFrame(10.0);
	TestEqual(TEXT("both fired despite the throw"), TimersFired(), uint64(8));
	TestEqual(TEXT("the sibling ran"), EvalString(Host, TestViewId, "after"), FString(TEXT("1")));
	TestEqual(TEXT("the throw was counted"), Runtime->GetNumErrors(), ErrorsBefore + 1);

	// The TypeError contract and the negative-ms clamp (hud-demo
	// VacuusJs.cpp:380-381, :385-386).
	TestEqual(TEXT("setTimeout(non-function) throws TypeError"),
		EvalString(Host, TestViewId,
			"(() => { try { setTimeout(42); } catch (e) { return String(e instanceof TypeError); } return 'no-throw'; })()"),
		FString(TEXT("true")));
	Host.ExecuteScript(TestViewId, TEXT("globalThis.neg = 0; setTimeout(() => neg++, -50);"), TEXT("timer-negative"));
	Host.PumpFrame(11.0);
	TestEqual(TEXT("negative ms clamps to 0 and fires next pump"), TimersFired(), uint64(9));
	TestEqual(TEXT("script-side"), EvalString(Host, TestViewId, "neg"), FString(TEXT("1")));

	// NO extra-args forwarding -- the documented demo shape (hud-demo
	// VacuusJs.cpp:554): callbacks are dispatched with zero arguments.
	Host.ExecuteScript(TestViewId,
		TEXT("globalThis.gotArgs = -1; setTimeout(function() { gotArgs = arguments.length; }, 0, 'x', 'y');"),
		TEXT("timer-args"));
	Host.PumpFrame(12.0);
	TestEqual(TEXT("extra setTimeout args are not forwarded"), EvalString(Host, TestViewId, "gotArgs"),
		FString(TEXT("0")));
	TestEqual(TEXT("final exact count"), TimersFired(), uint64(10));

	return true;
}

/**
 * RESTORE-THE-BUG for the frame-start cutoff. Green half: under the strict
 * cutoff a 0 ms self-rearming timer fires once per pump (the Timers test's
 * headline assertion, repeated here as the control). Red half: FParams
 * relaxes the cutoff to the demo's at-or-before due-test (hud-demo
 * VacuusJs.cpp:547 against a deadline priced at the same now, :390) -- the
 * in-pump re-registration is then due IMMEDIATELY, the index loop appends as
 * fast as it consumes, and the pass cannot terminate.
 *
 * THE RED STATE, OBSERVED WITHOUT HANGING THE SUITE: the same job Task 2's
 * escape interrupt handler did for the watchdog test, here as a pump-internal
 * test-only bound (FParams::TestTimerPassHardStop) -- an interrupt-based
 * escape would end the pass only when a kill happens to land before the
 * callback's re-registration, which the fixed poll cadence cannot guarantee.
 * The pass demonstrably fires EXACTLY the bound (25000x the green half's 1)
 * and returns only because the bound cut it; the next pump does it again, so
 * this is a livelock, not a one-frame burst. Non-termination observed,
 * bounded, no hang.
 */
bool FVaCuusJsPumpTimerLivelockTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsPumpTest;

	constexpr int32 HardStop = 25000;
	const char* Setup = "globalThis.f = function() { setTimeout(f, 0); }; setTimeout(f, 0);";

	// Green control: strict cutoff, the SAME hard stop armed -- and never reached.
	{
		FVaCuusJsScriptHost::FParams Params;
		Params.TestTimerPassHardStop = HardStop;
		FVaCuusJsScriptHost Host(Params);
		Host.OnViewAdded(TestViewId);
		Host.ExecuteScript(TestViewId, FString(Setup), TEXT("livelock-green"));
		if (!TestNotNull(TEXT("runtime (green)"), Host.GetRuntime()))
		{
			return false;
		}

		Host.PumpFrame(1.0);
		TestEqual(TEXT("strict cutoff: one fire per pump"), Host.GetRuntime()->GetNumTimersFired(), uint64(1));
		Host.PumpFrame(2.0);
		TestEqual(TEXT("and one more"), Host.GetRuntime()->GetNumTimersFired(), uint64(2));
	}

	// Red half: the relaxed cutoff. The hard-stop warning is the bound firing --
	// expected exactly twice, once per pump, and never in the green half above.
	{
		AddExpectedMessagePlain(TEXT("hit its test-only hard stop (25000 fires in one pass)"), ELogVerbosity::Warning,
			EAutomationExpectedMessageFlags::Contains, 2);

		FVaCuusJsScriptHost::FParams Params;
		Params.bTestRelaxTimerCutoff = true;
		Params.TestTimerPassHardStop = HardStop;
		FVaCuusJsScriptHost Host(Params);
		Host.OnViewAdded(TestViewId);
		Host.ExecuteScript(TestViewId, FString(Setup), TEXT("livelock-red"));
		if (!TestNotNull(TEXT("runtime (red)"), Host.GetRuntime()))
		{
			return false;
		}

		Host.PumpFrame(1.0);
		TestEqual(TEXT("relaxed cutoff: the pass fired the ENTIRE bound in one pump"),
			Host.GetRuntime()->GetNumTimersFired(), uint64(HardStop));

		Host.PumpFrame(2.0);
		TestEqual(TEXT("and again next pump -- a livelock, not a burst"),
			Host.GetRuntime()->GetNumTimersFired(), uint64(2 * HardStop));
	}

	return true;
}

/**
 * The bounded job drain, green: (a) the pump order raf -> timers -> jobs,
 * pinned with a sequence log -- a promise resolved in a rAF callback runs its
 * .then in the SAME pump, after the timers (the documented deviation from
 * browser scheduling, spec 3.5); (b) the cap: a chain longer than
 * MaxJobsPerPump is cut with ONE Error naming the view, the remainder runs on
 * the following pumps, deltas exact, and the cap is a log line -- never a JS
 * exception.
 */
bool FVaCuusJsPumpJobsTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsPumpTest;

	FVaCuusJsScriptHost::FParams Params;
	Params.MaxJobsPerPump = 50;
	FVaCuusJsScriptHost Host(Params);
	Host.OnViewAdded(TestViewId);

	// (a) The ordering.
	Host.ExecuteScript(TestViewId,
		TEXT("globalThis.seq = [];")
		TEXT("requestAnimationFrame(() => { seq.push('raf'); Promise.resolve().then(() => seq.push('job')); });")
		TEXT("setTimeout(() => seq.push('timer'), 0);"),
		TEXT("jobs-order"));

	FVaCuusJsRuntime* Runtime = Host.GetRuntime();
	if (!TestNotNull(TEXT("the first script created the runtime"), Runtime))
	{
		return false;
	}
	const auto Jobs = [Runtime]() { return Runtime->GetNumJobsExecuted(); };

	Host.PumpFrame(1.0);
	TestEqual(TEXT("raf -> timers -> jobs, one pump"), EvalString(Host, TestViewId, "seq.join('|')"),
		FString(TEXT("raf|timer|job")));
	TestEqual(TEXT("exactly one job (the .then reaction)"), Jobs(), uint64(1));

	// (b) The cap. The eval runs the chain's first link itself (n=1) and leaves
	// one job queued; every executed job queues the next until n reaches 200.
	Host.ExecuteScript(TestViewId,
		TEXT("globalThis.n = 0; (function chain() { if (++n < 200) queueMicrotask(chain); })();"), TEXT("jobs-chain"));

	AddExpectedMessagePlain(TEXT("JS job drain hit vacuus.Js.MaxJobsPerPump (50) at view 11"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 3);
	const uint64 ErrorsBefore = Runtime->GetNumErrors();

	Host.PumpFrame(2.0);
	TestEqual(TEXT("pump 2: the cap's worth exactly"), Jobs(), uint64(1 + 50));
	TestEqual(TEXT("the chain is mid-flight"), EvalString(Host, TestViewId, "n"), FString(TEXT("51")));

	Host.PumpFrame(3.0);
	TestEqual(TEXT("pump 3: fifty more"), Jobs(), uint64(1 + 100));
	Host.PumpFrame(4.0);
	TestEqual(TEXT("pump 4: fifty more"), Jobs(), uint64(1 + 150));
	Host.PumpFrame(5.0);
	TestEqual(TEXT("pump 5: the remaining 49 -- nothing was lost at the cap"), Jobs(), uint64(1 + 199));
	TestEqual(TEXT("the chain completed"), EvalString(Host, TestViewId, "n"), FString(TEXT("200")));

	Host.PumpFrame(6.0);
	TestEqual(TEXT("drained: an empty pump executes nothing"), Jobs(), uint64(1 + 199));

	// The cap is a diagnostic, not an exception: the error COUNTER counts JS
	// exceptions only (spec 3.8), and the chain threw none.
	TestEqual(TEXT("the cap never touched the exception counter"), Runtime->GetNumErrors(), ErrorsBefore);

	return true;
}

/**
 * RESTORE-THE-BUG for the job cap. The self-requeuing microtask --
 * `(function f(){ queueMicrotask(f) })()` -- with the cap REMOVED
 * (FParams::MaxJobsPerPump = 0) and the watchdog disabled too, so BOTH
 * production defenses (spec 3.5's pair) are off and the drain has no bound of
 * its own.
 *
 * THE RED STATE, OBSERVED WITHOUT HANGING THE SUITE: the same technique as the
 * timer livelock's red half -- a pump-internal test-only bound
 * (FParams::TestJobDrainHardStop). The drain executes EXACTLY the bound
 * (1000x the production cap of 50 next door) and the queue is STILL non-empty
 * when it returns -- and is again after another pump. No cap Error appears
 * (the production path really was off; an unexpected Error would fail the
 * test), no exception was thrown, nothing else ended it. Non-termination
 * observed, bounded, no hang.
 */
bool FVaCuusJsPumpJobDrainLivelockTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsPumpTest;

	constexpr int32 HardStop = 50000;

	FVaCuusJsScriptHost::FParams Params;
	Params.MaxJobsPerPump = 0;				  // the broken state: no cap
	Params.RuntimeParams.WatchdogMs = 0;	  // and no watchdog -- the other defense off too
	Params.TestJobDrainHardStop = HardStop;
	FVaCuusJsScriptHost Host(Params);
	Host.OnViewAdded(TestViewId);
	Host.ExecuteScript(TestViewId, TEXT("(function f() { queueMicrotask(f); })();"), TEXT("jobs-livelock"));

	FVaCuusJsRuntime* Runtime = Host.GetRuntime();
	if (!TestNotNull(TEXT("runtime"), Runtime))
	{
		return false;
	}

	Host.PumpFrame(1.0);
	TestEqual(TEXT("the drain ran the ENTIRE test bound in one pump"), Runtime->GetNumJobsExecuted(), uint64(HardStop));
	TestTrue(TEXT("and the queue is STILL not empty -- nothing bounded it but the test"),
		JS_IsJobPending(Runtime->GetRuntime()));

	Host.PumpFrame(2.0);
	TestEqual(TEXT("same again next pump -- a livelock, not a burst"), Runtime->GetNumJobsExecuted(),
		uint64(2 * HardStop));
	TestTrue(TEXT("still pending"), JS_IsJobPending(Runtime->GetRuntime()));

	TestEqual(TEXT("no exception anywhere -- this is a pure livelock"), Runtime->GetNumErrors(), uint64(0));
	TestEqual(TEXT("and the disabled watchdog never fired"), Runtime->GetNumWatchdogTrips(), uint64(0));

	return true;
}

/**
 * console.* lands in LogVaCuusJS at the mapped verbosity (log/info -> Log,
 * warn -> Warning, error -> Error), args space-joined via JS_ToCString, an
 * unstringifiable arg prints as a placeholder instead of surfacing its
 * exception -- and console.error is a LOG level, never a JS error: the
 * exception counter stays untouched.
 */
bool FVaCuusJsPumpConsoleTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsPumpTest;

	AddExpectedMessagePlain(TEXT("hello world 42 [object Object]"), ELogVerbosity::Log,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("pump-console info-line"), ELogVerbosity::Log,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("pump-console warn-line"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("pump-console error-line"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("before <unprintable> after"), ELogVerbosity::Log,
		EAutomationExpectedMessageFlags::Contains, 1);

	FVaCuusJsScriptHost Host;
	Host.OnViewAdded(TestViewId);
	Host.ExecuteScript(TestViewId,
		TEXT("console.log('hello world', 42, {});")
		TEXT("console.info('pump-console', 'info-line');")
		TEXT("console.warn('pump-console', 'warn-line');")
		TEXT("console.error('pump-console', 'error-line');")
		TEXT("console.log('before', { toString() { throw new Error('nope'); } }, 'after');"),
		TEXT("console-test"));

	if (!TestNotNull(TEXT("runtime"), Host.GetRuntime()))
	{
		return false;
	}
	TestEqual(TEXT("console.error is a log level, not a JS exception"), Host.GetRuntime()->GetNumErrors(), uint64(0));

	return true;
}

/**
 * The container rules around the pump: `document` exists and is null before any
 * document (spec 3.4); contexts materialize at the first script, not at
 * registration; scripts for unknown views are refused loudly WITHOUT booting
 * the runtime; sibling contexts are isolated; a pump with nothing due moves no
 * counter; OnViewRemoved destroys the context with everything it holds -- the
 * runtime destructor's live-byte check (Task 2) is the leak observable, so
 * this test SURVIVING Shutdown is the assertion.
 */
bool FVaCuusJsPumpLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsPumpTest;

	FVaCuusJsScriptHost Host;

	// Unknown view: named refusal, and no runtime springs into being for it.
	AddExpectedMessagePlain(TEXT("ExecuteScript('lc-unknown') for unknown view 77 dropped"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);
	Host.ExecuteScript(77, TEXT("1"), TEXT("lc-unknown"));
	TestNull(TEXT("a refused script boots nothing"), Host.GetRuntime());

	// Contexts are lazy: registration alone creates nothing.
	Host.OnViewAdded(5);
	Host.OnViewAdded(9);
	TestNull(TEXT("no script, no context"), Host.FindViewContext(5));

	// `document` is a real global holding null -- feature-testable, not a
	// ReferenceError (Task 6 swaps in the real document).
	Host.ExecuteScript(5, TEXT("globalThis.docProbe = String(document === null && typeof document === 'object');"),
		TEXT("lc-document"));
	TestNotNull(TEXT("the first script materialized the context"), Host.FindViewContext(5));
	TestNotNull(TEXT("and the runtime"), Host.GetRuntime());
	TestEqual(TEXT("document === null before any document"), EvalString(Host, 5, "docProbe"), FString(TEXT("true")));

	// Isolation: each view gets its own global object.
	Host.ExecuteScript(9, TEXT("globalThis.who = 'nine';"), TEXT("lc-nine"));
	Host.ExecuteScript(5, TEXT("globalThis.who = 'five';"), TEXT("lc-five"));
	TestEqual(TEXT("view 9 kept its own global"), EvalString(Host, 9, "who"), FString(TEXT("nine")));
	TestEqual(TEXT("view 5 kept its own"), EvalString(Host, 5, "who"), FString(TEXT("five")));

	// A pump with nothing due: EXACT zero deltas on every fired counter.
	FVaCuusJsRuntime* Runtime = Host.GetRuntime();
	Host.PumpFrame(1.0);
	TestEqual(TEXT("idle pump: no timers"), Runtime->GetNumTimersFired(), uint64(0));
	TestEqual(TEXT("no rAF"), Runtime->GetNumRafCallbacksRun(), uint64(0));
	TestEqual(TEXT("no jobs"), Runtime->GetNumJobsExecuted(), uint64(0));

	// Removal tears the context down with its held callbacks; the pump then
	// iterates live contexts only, so nothing of view 5's ever fires again.
	Host.ExecuteScript(5,
		TEXT("setTimeout(() => 1, 0); setInterval(() => 1, 0); requestAnimationFrame(() => 1);")
		TEXT("Promise.resolve().then(() => 1);"),
		TEXT("lc-pending"));
	Host.OnViewRemoved(5);
	TestNull(TEXT("the context died with the view"), Host.FindViewContext(5));
	Host.PumpFrame(2.0);
	TestEqual(TEXT("the removed view's timers are gone, not pending"), Runtime->GetNumTimersFired(), uint64(0));
	TestEqual(TEXT("its rAF too"), Runtime->GetNumRafCallbacksRun(), uint64(0));

	// The one loose end: view 5's .then reaction was enqueued on the RUNTIME's
	// job list before the removal, and job entries hold their own refs -- it
	// executes (harmlessly) from a surviving view's drain segment. Pinned here
	// so the behavior is a decision, not an accident.
	TestEqual(TEXT("the already-enqueued job of the removed view still ran"), Runtime->GetNumJobsExecuted(), uint64(1));

	// Shutdown drops every context, then the runtime, whose destructor checks
	// live bytes back to zero -- surviving this line IS the no-leak assertion
	// (the Task 2 pattern). Then again, for idempotence.
	Host.Shutdown();
	TestNull(TEXT("the runtime is gone"), Host.GetRuntime());
	Host.Shutdown();

	return true;
}

namespace VaCuusJsPumpTest
{
/**
 * A view with no Rml context: enough for AddView to register it (Initialize
 * succeeds) while HasView() keeps it out of the record loop -- Task 3's pump
 * needs a REGISTERED view, not a rendered one, and `document` is null either
 * way until Task 6.
 */
class FNullDocumentHost final : public IVaCuusDocumentHost
{
public:
	virtual bool Initialize(uint32 /*InViewId*/, const TSharedRef<FVaCuusViewStatus>& /*InStatus*/) override
	{
		return true;
	}
	virtual void Shutdown() override {}
	virtual void SetViewSize(FIntPoint /*ViewSize*/) override {}
	virtual void LoadDocumentFromFile(const FString& /*VfsPath*/, uint64 /*LoadSerial*/) override {}
	virtual void LoadDocumentFromMemory(const FString& /*RmlSource*/, uint64 /*LoadSerial*/) override {}
	virtual void CloseDocument() override {}
	virtual void SetVisible(bool /*bVisible*/) override {}
	virtual bool HasView() const override { return false; }
	virtual Rml::Context* GetContext() const override { return nullptr; }
	virtual void RecordAndPublishFrame() override {}
};

/**
 * The REAL FVaCuusJsScriptHost wrapped for observability: forwards every seam
 * call unchanged, and drains a test-fed closure queue at the top of PumpFrame --
 * the one in-band way to run ExecuteScript ON the UI thread before Task 6 lands
 * the command plumbing. Statics because the instance lives inside
 * FVaCuusUIThread; the test thread reads Inner's ATOMIC counters only, after a
 * WaitForFrameCount (the frame counter's release/acquire pair orders everything
 * else, the M3 test pattern).
 */
class FWrappedProductionHost final : public IVaCuusScriptHost
{
public:
	static inline FVaCuusJsScriptHost* Inner = nullptr;
	static inline TQueue<TFunction<void()>, EQueueMode::Spsc> RunOnUIThread;

	FWrappedProductionHost()
		: Real(MakeUnique<FVaCuusJsScriptHost>())
	{
		RunOnUIThread.Empty();
		Inner = Real.Get();
	}

	virtual ~FWrappedProductionHost() override { Inner = nullptr; }

	virtual void OnViewAdded(uint32 ViewId) override { Real->OnViewAdded(ViewId); }
	virtual void OnViewRemoved(uint32 ViewId) override { Real->OnViewRemoved(ViewId); }
	virtual void OnDocumentReady(uint32 ViewId, Rml::ElementDocument* Document) override
	{
		Real->OnDocumentReady(ViewId, Document);
	}
	virtual void OnDocumentClosing(uint32 ViewId) override { Real->OnDocumentClosing(ViewId); }
	virtual void PumpFrame(double NowSeconds) override
	{
		TFunction<void()> Closure;
		while (RunOnUIThread.Dequeue(Closure))
		{
			Closure();
		}
		Real->PumpFrame(NowSeconds);
	}
	virtual void CollectGarbage(const TCHAR* Reason) override { Real->CollectGarbage(Reason); }
	virtual void ExecuteScript(uint32 ViewId, const FString& Source, const FString& SourceName) override
	{
		Real->ExecuteScript(ViewId, Source, SourceName);
	}
	virtual void OnInlineFrameEntry() override { Real->OnInlineFrameEntry(); }
	virtual void Shutdown() override { Real->Shutdown(); }

private:
	TUniquePtr<FVaCuusJsScriptHost> Real;
};

/** One UI frame at a time; the wake event coalesces (the M3/seam test pattern). */
bool PumpRealFrames(FVaCuusUIThread& Thread, int32 Count)
{
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const uint64 Before = Thread.GetFrameCount();
		Thread.Trigger();
		if (!Thread.WaitForFrameCount(Before + 1, 5.0))
		{
			return false;
		}
	}
	return true;
}
}	 // namespace VaCuusJsPumpTest

/**
 * The pump on the REAL UI thread, end to end: the production FVaCuusJsScriptHost
 * built by the thread's factory path, a real registered view (AddView through
 * the command queue), PumpFrame running in its RunFrame phase on the RmlUi
 * clock, and console output crossing back as the observable. Also pins the
 * frame-start cutoff against the REAL clock: a 0 ms timer registered from
 * inside a rAF callback (i.e. during the pump) fires on the NEXT frame, never
 * the same one -- the library tests prove it with a fake clock, this proves the
 * production clock feeds the same semantics.
 */
bool FVaCuusJsPumpUIThreadIntegrationTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsPumpTest;

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
	EnableCVar->Set(1, ECVF_SetByConsole);	  // the host must exist; an ambient 0 would veto the factory

	// Leave the process as found: thread down, cvar back, and the PRODUCTION
	// factory re-registered (the seam test's teardown shape).
	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
		EnableCVar->Set(SavedEnable, ECVF_SetByConsole);
		Module.SetScriptHostFactory(
			[]() -> TUniquePtr<IVaCuusScriptHost> { return MakeUnique<FVaCuusJsScriptHost>(); });
	};

	Module.SetScriptHostFactory(
		[]() -> TUniquePtr<IVaCuusScriptHost> { return MakeUnique<FWrappedProductionHost>(); });

	FVaCuusUIThread* Thread = Module.GetOrStartUIThread();
	if (!TestNotNull(TEXT("UI thread"), Thread) || !TestTrue(TEXT("the host is live"), Thread->HasScriptHost()))
	{
		return false;
	}

	// A real view, through the real command queue.
	const uint32 ViewId = Thread->AllocateViewId();
	Thread->EnqueueAddView(ViewId, MakeUnique<FNullDocumentHost>(), FIntPoint::ZeroValue,
		MakeShared<FVaCuusViewStatus>());
	if (!TestTrue(TEXT("the AddView frame ran"), PumpRealFrames(*Thread, 1)))
	{
		return false;
	}

	FVaCuusJsScriptHost* Host = FWrappedProductionHost::Inner;
	if (!TestNotNull(TEXT("the wrapped production host exists"), Host))
	{
		return false;
	}

	AddExpectedMessagePlain(TEXT("vacuus-js-int raf true"), ELogVerbosity::Log,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("vacuus-js-int timer"), ELogVerbosity::Log,
		EAutomationExpectedMessageFlags::Contains, 1);

	// The script lands on the UI thread at the top of the NEXT PumpFrame: the
	// rAF is registered before the swap, so it runs in that same frame's rAF
	// phase; the 0 ms timer it registers is priced AT that frame's cutoff.
	FWrappedProductionHost::RunOnUIThread.Enqueue(
		[Host, ViewId]()
		{
			Host->ExecuteScript(ViewId,
				TEXT("requestAnimationFrame(ts => {")
				TEXT("    console.log('vacuus-js-int raf ' + (ts >= 0));")
				TEXT("    setTimeout(() => console.log('vacuus-js-int timer'), 0);")
				TEXT("});"),
				TEXT("integration"));
		});

	if (!TestTrue(TEXT("the rAF frame ran"), PumpRealFrames(*Thread, 1)))
	{
		return false;
	}
	FVaCuusJsRuntime* Runtime = Host->GetRuntime();
	if (!TestNotNull(TEXT("the script created the runtime on the UI thread"), Runtime))
	{
		return false;
	}
	TestEqual(TEXT("the rAF callback ran in its registration frame"), Runtime->GetNumRafCallbacksRun(), uint64(1));

	// THE CUTOFF ON THE REAL CLOCK: registered during the pump, the 0 ms timer
	// did NOT fire in the same frame...
	TestEqual(TEXT("the in-pump 0 ms timer waited"), Runtime->GetNumTimersFired(), uint64(0));

	// ...and fires on the next one (the real clock advanced past the fixed cutoff).
	if (!TestTrue(TEXT("the timer frame ran"), PumpRealFrames(*Thread, 1)))
	{
		return false;
	}
	TestEqual(TEXT("the timer fired one frame later"), Runtime->GetNumTimersFired(), uint64(1));

	// Retire the view mid-flight; the pump iterates live contexts only, so the
	// following frames must run clean.
	Thread->EnqueueRemoveView(ViewId);
	if (!TestTrue(TEXT("frames after the removal run"), PumpRealFrames(*Thread, 2)))
	{
		return false;
	}
	TestEqual(TEXT("no JS error anywhere in the run"), Runtime->GetNumErrors(), uint64(0));

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
