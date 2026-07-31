// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusJs.h"
#include "VaCuusJsRuntime.h"

#include "HAL/PlatformTime.h"

#include "quickjs.h"

#include <cstring>

#if WITH_DEV_AUTOMATION_TESTS

/*
 * LIBRARY-LEVEL, ON THE AUTOMATION THREAD, DELIBERATELY: every test here drives a
 * direct FVaCuusJsRuntime with no UI thread anywhere -- legal by the engine's own
 * contract (no thread-identity code; the one thread-sensitive datum is the stack
 * anchor, captured on the creating thread, quickjs.c:2019 -- create and use on one
 * thread and it is simply correct). What the runtime CANNOT get from cvars this way
 * it takes from direct FParams, so no test mutates global cvar state.
 */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsNoImplicitGCTest, "VaCuus.Js.Runtime.NoImplicitGC",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsOomAtCapTest, "VaCuus.Js.Runtime.OomAtCap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsWatchdogTest, "VaCuus.Js.Runtime.Watchdog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsStackHeadroomTest, "VaCuus.Js.Runtime.StackHeadroom",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsMallocDeltaTest, "VaCuus.Js.Runtime.MallocDelta",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace VaCuusJsRuntimeTest
{
/** JS_FreeContext before the runtime dies, on every exit path. */
struct FScopedContext
{
	explicit FScopedContext(FVaCuusJsRuntime& Runtime)
		: Ctx(JS_NewContext(Runtime.GetRuntime()))
	{
	}

	~FScopedContext()
	{
		if (Ctx != nullptr)
		{
			JS_FreeContext(Ctx);
		}
	}

	JSContext* Ctx = nullptr;
};

JSValue Eval(JSContext* Ctx, const char* Source, const char* Name = "<VaCuusJsRuntimeTest>")
{
	return JS_Eval(Ctx, Source, std::strlen(Source), Name, JS_EVAL_TYPE_GLOBAL);
}

/** Consumes the PENDING exception and renders it as a string (never leaves one behind). */
FString TakePendingExceptionString(JSContext* Ctx, bool* bOutUncatchable = nullptr)
{
	JSValue Exception = JS_GetException(Ctx);
	if (bOutUncatchable != nullptr)
	{
		*bOutUncatchable = JS_IsUncatchableError(Exception);
	}

	FString Result(TEXT("<unstringifiable>"));
	if (const char* Utf8 = JS_ToCString(Ctx, Exception))
	{
		Result = FString(UTF8_TO_TCHAR(Utf8));
		JS_FreeCString(Ctx, Utf8);
	}
	else
	{
		JS_FreeValue(Ctx, JS_GetException(Ctx));
	}
	JS_FreeValue(Ctx, Exception);
	return Result;
}

/**
 * The cyclic-garbage churn both GC tests run: objects that reference themselves,
 * so REFCOUNTING can never free them -- only the cycle collector can (that is
 * the entire reason quickjs has one). Plain `{}` garbage would vanish at scope
 * exit by refcount and prove nothing about GC.
 */
const char* GCyclicChurn = "for (let i = 0; i < 50000; i++) { const o = {}; o.self = o; }";
}	 // namespace VaCuusJsRuntimeTest

/**
 * Spec 2(b)/3.6: with JS_SetGCThreshold(rt, -1) at birth, the engine's implicit
 * collector -- whose ONLY trigger is object allocation past the threshold
 * (js_trigger_gc's sole call site is JS_NewObjectFromShape, quickjs.c:5748) --
 * never runs, so uncollected cycles pile up until OUR frame point collects.
 *
 * RESTORE-THE-BUG, RUN LIVE IN-SUITE: the second half constructs a runtime with
 * the disable line deliberately skipped (FParams::bTestLeaveEngineGCEnabled) and
 * runs the identical churn -- the engine then collects on its own at its 256 KiB
 * default threshold (quickjs.c:1980) and the live-byte climb this test's first
 * half asserts on DISAPPEARS. That is the proof the climb assertion would catch
 * a lost JS_SetGCThreshold(-1), and equally the proof the churn is real cyclic
 * garbage rather than refcount-collectable noise.
 */
bool FVaCuusJsNoImplicitGCTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsRuntimeTest;

	FVaCuusJsRuntime::FParams Params;
	Params.MemoryLimitMB = 64;
	Params.GCStepKB = 512;

	// Half 1: implicit GC off (production configuration).
	{
		FVaCuusJsRuntime Runtime(Params);
		FScopedContext Scoped(Runtime);
		if (!TestNotNull(TEXT("context"), Scoped.Ctx))
		{
			return false;
		}

		const int64 LiveBefore = Runtime.GetLiveBytes();
		JSValue Ret = Eval(Scoped.Ctx, GCyclicChurn);
		TestFalse(TEXT("the churn does not throw"), JS_IsException(Ret));
		JS_FreeValue(Scoped.Ctx, Ret);

		const int64 Climb = Runtime.GetLiveBytes() - LiveBefore;

		// 50k self-referencing objects dwarf the 256 KiB threshold the engine would
		// have collected at; if anything collected them, this cannot pass.
		TestTrue(FString::Printf(TEXT("live bytes climbed past any implicit threshold (climbed %lld)"), Climb),
			Climb >= 2 * 1024 * 1024);
		TestEqual(TEXT("no collection ran while allocating hard"), Runtime.GetNumCollections(), uint64(0));

		// The frame point is the only collector: growth >= the 512 KB step, so it fires.
		TestTrue(TEXT("the frame GC point collects on growth"), Runtime.CollectGarbage(TEXT("test")));
		TestEqual(TEXT("exactly one collection, ours"), Runtime.GetNumCollections(), uint64(1));
		TestTrue(TEXT("the collection reclaimed the cycles"),
			Runtime.GetLiveBytes() <= LiveBefore + Climb - 1024 * 1024);

		// And the step trigger declines with nothing new allocated.
		TestFalse(TEXT("a second immediate call declines (growth < step)"), Runtime.CollectGarbage(TEXT("test")));
		TestEqual(TEXT("the declined call collected nothing"), Runtime.GetNumCollections(), uint64(1));
	}

	// Half 2: the deliberately-broken state -- JS_SetGCThreshold(-1) skipped.
	{
		FVaCuusJsRuntime::FParams BrokenParams = Params;
		BrokenParams.bTestLeaveEngineGCEnabled = true;

		FVaCuusJsRuntime Runtime(BrokenParams);
		FScopedContext Scoped(Runtime);

		const int64 LiveBefore = Runtime.GetLiveBytes();
		JSValue Ret = Eval(Scoped.Ctx, GCyclicChurn);
		TestFalse(TEXT("the churn does not throw (broken half)"), JS_IsException(Ret));
		JS_FreeValue(Scoped.Ctx, Ret);

		const int64 Climb = Runtime.GetLiveBytes() - LiveBefore;
		TestTrue(FString::Printf(TEXT("with the engine's GC left on, the climb vanishes (climbed %lld)"), Climb),
			Climb < 2 * 1024 * 1024);
		TestEqual(TEXT("and our counter shows the engine's collections were not ours"),
			Runtime.GetNumCollections(), uint64(0));
	}

	return true;
}

/**
 * E5 (spec 2(b)): at the memory cap the engine does NOT collect and retry -- the
 * allocator returns NULL immediately (quickjs.c:1645-1647) and the script gets
 * InternalError "out of memory" (quickjs.c:8127-8135) even though a collection
 * would have freed space. The error sink recognizes it and arms the fallback;
 * the next CollectGarbage() collects REGARDLESS of the growth trigger -- proven
 * here with a step (1 GB) the growth cannot possibly reach.
 */
bool FVaCuusJsOomAtCapTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsRuntimeTest;

	AddExpectedMessagePlain(TEXT("JS exception in 'oom-e5': InternalError: out of memory"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("JS OOM fallback collection"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);

	FVaCuusJsRuntime::FParams Params;
	Params.MemoryLimitMB = 4;
	Params.GCStepKB = 1 << 20;	  // 1 GB: the growth trigger CANNOT fire; only the OOM flag can.

	FVaCuusJsRuntime Runtime(Params);
	FScopedContext Scoped(Runtime);
	if (!TestNotNull(TEXT("context"), Scoped.Ctx))
	{
		return false;
	}

	// Some collectable cyclic garbage first, so the fallback collection below has
	// real work -- then one allocation the 4 MB cap can never grant, so the OOM
	// fires with plenty of headroom left for building the error object itself.
	{
		JSValue Ret = Eval(Scoped.Ctx, "for (let i = 0; i < 1000; i++) { const o = {}; o.self = o; }");
		TestFalse(TEXT("the garbage churn fits under the cap"), JS_IsException(Ret));
		JS_FreeValue(Scoped.Ctx, Ret);
	}

	JSValue Ret = Eval(Scoped.Ctx, "globalThis.big = new ArrayBuffer(64 * 1024 * 1024)", "oom-e5");
	TestTrue(TEXT("allocating past the cap throws"), JS_IsException(Ret));
	JS_FreeValue(Scoped.Ctx, Ret);

	// E5's core: the engine refused WITHOUT attempting a collection.
	TestEqual(TEXT("no collect-and-retry happened at the cap"), Runtime.GetNumCollections(), uint64(0));

	// The sink consumes the InternalError (the expected-message assert above pins its
	// text), counts it, and arms the fallback.
	Runtime.ReportException(Scoped.Ctx, TEXT("oom-e5"));
	TestEqual(TEXT("the error was counted"), Runtime.GetNumErrors(), uint64(1));
	TestTrue(TEXT("the OOM fallback is armed"), Runtime.IsOomFallbackPending());

	// Growth is ~nothing against a 1 GB step, so only the armed flag can explain a
	// collection running here.
	TestTrue(TEXT("the fallback collection runs despite zero growth"), Runtime.CollectGarbage(TEXT("oom-e5")));
	TestEqual(TEXT("the fallback collection was counted"), Runtime.GetNumCollections(), uint64(1));
	TestFalse(TEXT("the flag is consumed by the collection"), Runtime.IsOomFallbackPending());

	// And the runtime is alive afterwards -- OOM is an exception, never a poisoning.
	{
		JSValue After = Eval(Scoped.Ctx, "1 + 1");
		int32 Sum = 0;
		TestEqual(TEXT("a later eval works"), JS_ToInt32(Scoped.Ctx, &Sum, After), 0);
		TestEqual(TEXT("and computes"), Sum, 2);
		JS_FreeValue(Scoped.Ctx, After);
	}

	return true;
}

/**
 * Spec 3.3: a script running past the per-entry deadline is interrupted with an
 * UNCATCHABLE error (script try/catch cannot swallow it, JS_ThrowInterrupted,
 * quickjs.c:8215-8219), the entry guard resets it at the boundary, and the next
 * entry runs normally.
 *
 * THE RED STATE, OBSERVED WITHOUT HANGING THE SUITE: the restore-the-bug half is
 * "disable the watchdog and the same script never terminates" -- which taken
 * literally is a suite hang. It is bounded here by giving the broken-state
 * runtime a TEST-OWNED escape interrupt handler with its own 300 ms deadline:
 * the script then demonstrably runs SIX TIMES past the point the 50 ms watchdog
 * kills it in the green half (elapsed >= 300 ms), the watchdog-trip counter
 * stays at zero (the deadline logic really is what is disabled), and the
 * escape's poll count proves the interpreter was executing and being polled the
 * whole time -- i.e. with no handler returning 1 the loop had no way to end.
 * Non-termination observed, bounded, no hang.
 */
bool FVaCuusJsWatchdogTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsRuntimeTest;

	// Green half: 50 ms watchdog (the Shipping default -- the tighter of the two).
	{
		AddExpectedMessagePlain(TEXT("JS watchdog: 'wd-loop' ran past its 50 ms deadline"), ELogVerbosity::Error,
			EAutomationExpectedMessageFlags::Contains, 1);
		AddExpectedMessagePlain(TEXT("JS watchdog: 'wd-catch' ran past its 50 ms deadline"), ELogVerbosity::Error,
			EAutomationExpectedMessageFlags::Contains, 1);

		FVaCuusJsRuntime::FParams Params;
		Params.WatchdogMs = 50;
		FVaCuusJsRuntime Runtime(Params);
		FScopedContext Scoped(Runtime);
		if (!TestNotNull(TEXT("context"), Scoped.Ctx))
		{
			return false;
		}

		// (a) The bare infinite loop trips.
		const double StartSeconds = FPlatformTime::Seconds();
		JSValue Ret;
		{
			FVaCuusJsEntryGuard Guard(Runtime, Scoped.Ctx, TEXT("wd-loop"));
			Ret = Eval(Scoped.Ctx, "for(;;);", "wd-loop");
		}
		const double ElapsedSeconds = FPlatformTime::Seconds() - StartSeconds;

		TestTrue(TEXT("the loop was interrupted"), JS_IsException(Ret));
		JS_FreeValue(Scoped.Ctx, Ret);
		TestEqual(TEXT("one trip counted"), Runtime.GetNumWatchdogTrips(), uint64(1));
		TestTrue(FString::Printf(TEXT("it ran to the deadline, not past it (%.0f ms)"), ElapsedSeconds * 1000.0),
			ElapsedSeconds >= 0.045 && ElapsedSeconds < 5.0);

		// The guard already reset the uncatchable bit at the boundary
		// (JS_ResetUncatchableError re-throws with the bit cleared, quickjs.h:829-833),
		// so what this entry's caller consumes is an ordinary pending exception.
		bool bUncatchable = true;
		const FString Message = TakePendingExceptionString(Scoped.Ctx, &bUncatchable);
		TestFalse(TEXT("the reset made the trip consumable"), bUncatchable);
		TestTrue(FString::Printf(TEXT("and it is the engine's interrupt error (%s)"), *Message),
			Message.Contains(TEXT("interrupted")));

		// (b) Uncatchability observed behaviorally: a script catch does NOT swallow it.
		{
			FVaCuusJsEntryGuard Guard(Runtime, Scoped.Ctx, TEXT("wd-catch"));
			Ret = Eval(Scoped.Ctx, "try { for(;;); } catch (e) { 'caught' }", "wd-catch");
		}
		TestTrue(TEXT("try/catch cannot swallow the trip"), JS_IsException(Ret));
		JS_FreeValue(Scoped.Ctx, Ret);
		TestEqual(TEXT("second trip counted"), Runtime.GetNumWatchdogTrips(), uint64(2));
		TakePendingExceptionString(Scoped.Ctx);

		// (c) The next guarded entry is completely normal.
		{
			FVaCuusJsEntryGuard Guard(Runtime, Scoped.Ctx, TEXT("wd-ok"));
			Ret = Eval(Scoped.Ctx, "1 + 1", "wd-ok");
		}
		int32 Sum = 0;
		TestEqual(TEXT("the next entry evaluates"), JS_ToInt32(Scoped.Ctx, &Sum, Ret), 0);
		TestEqual(TEXT("to the right answer"), Sum, 2);
		JS_FreeValue(Scoped.Ctx, Ret);
		TestEqual(TEXT("with no third trip"), Runtime.GetNumWatchdogTrips(), uint64(2));
	}

	// Red half: watchdog disabled (FParams::WatchdogMs = 0 -- the broken state),
	// bounded by the test's own escape handler as described above.
	{
		struct FEscapeState
		{
			double DeadlineSeconds = 0.0;
			int32 Polls = 0;
		};
		FEscapeState Escape;
		Escape.DeadlineSeconds = FPlatformTime::Seconds() + 0.3;

		FVaCuusJsRuntime::FParams Params;
		Params.WatchdogMs = 0;
		FVaCuusJsRuntime Runtime(Params);
		FScopedContext Scoped(Runtime);

		// Replace the runtime's interrupt handler with the test's bound: same poll
		// sites (every 10k ops, quickjs.c:479, :8221-8241), test-owned deadline.
		JS_SetInterruptHandler(Runtime.GetRuntime(),
			[](JSRuntime*, void* Opaque) -> int
			{
				FEscapeState* State = static_cast<FEscapeState*>(Opaque);
				++State->Polls;
				return FPlatformTime::Seconds() >= State->DeadlineSeconds ? 1 : 0;
			},
			&Escape);

		const double StartSeconds = FPlatformTime::Seconds();
		JSValue Ret;
		{
			FVaCuusJsEntryGuard Guard(Runtime, Scoped.Ctx, TEXT("wd-disabled"));
			Ret = Eval(Scoped.Ctx, "for(;;);", "wd-disabled");
		}
		const double ElapsedSeconds = FPlatformTime::Seconds() - StartSeconds;

		TestTrue(TEXT("only the escape ended it"), JS_IsException(Ret));
		JS_FreeValue(Scoped.Ctx, Ret);
		TestTrue(FString::Printf(TEXT("it sailed 6x past the 50 ms mark (%.0f ms)"), ElapsedSeconds * 1000.0),
			ElapsedSeconds >= 0.29);
		TestEqual(TEXT("the disabled watchdog never fired"), Runtime.GetNumWatchdogTrips(), uint64(0));
		TestTrue(TEXT("while the interpreter was executing and polling throughout"), Escape.Polls > 0);

		// No guard reset happened (no trip), so the escape's interrupt is STILL
		// uncatchable-pending -- consume it, and let its bit double as evidence.
		bool bUncatchable = false;
		TakePendingExceptionString(Scoped.Ctx, &bUncatchable);
		TestTrue(TEXT("and the disarmed guard reset nothing"), bUncatchable);
	}

	return true;
}

/**
 * Spec 3.3's stack-headroom claim at library level: deep recursion dies as the
 * engine's own RangeError "Maximum call stack size exceeded" (the native-stack
 * check, quickjs.c:1952-1957, throwing at quickjs.c:8138-8141) under the 256 KB
 * JS budget -- never the guard page; the test surviving IS the assertion. The
 * UI-thread variant -- the same recursion on the real 2 MB thread under RmlUi
 * frames -- lands with M4 Task 3's context work, which is what makes JS runs on
 * that thread possible at all.
 */
bool FVaCuusJsStackHeadroomTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsRuntimeTest;

	FVaCuusJsRuntime Runtime;	 // cvar defaults; the 256 KB stack budget is unconditional.
	FScopedContext Scoped(Runtime);
	if (!TestNotNull(TEXT("context"), Scoped.Ctx))
	{
		return false;
	}

	// Exercised even though the anchor is already correct (captured on this thread
	// inside JS_NewRuntime2, quickjs.c:2019): this is the call inline mode leans on,
	// and it must at minimum not disturb a same-thread runtime.
	Runtime.UpdateStackTopOnThisThread();

	JSValue Ret = Eval(Scoped.Ctx, "function f() { return f() + 1 } f()", "deep-recursion");
	TestTrue(TEXT("unbounded recursion throws"), JS_IsException(Ret));
	JS_FreeValue(Scoped.Ctx, Ret);

	const FString Message = TakePendingExceptionString(Scoped.Ctx);
	TestTrue(FString::Printf(TEXT("as the engine's RangeError, not a crash (%s)"), *Message),
		Message.Contains(TEXT("call stack")));

	// The engine is healthy after the overflow.
	Ret = Eval(Scoped.Ctx, "1 + 1");
	int32 Sum = 0;
	TestEqual(TEXT("a later eval works"), JS_ToInt32(Scoped.Ctx, &Sum, Ret), 0);
	TestEqual(TEXT("and computes"), Sum, 2);
	JS_FreeValue(Scoped.Ctx, Ret);

	return true;
}

/**
 * The malloc hooks' observable (spec 3.3): a known-size allocation moves the
 * live-byte counter by at least that size, and releasing it moves the counter
 * back -- through FMemory, in the allocator's quantized accounting basis (see
 * the hooks). Also pins the constructor's accounting probe: a freshly built
 * runtime+context reads non-zero, i.e. FMemory::GetAllocSize answers on this
 * platform.
 */
bool FVaCuusJsMallocDeltaTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsRuntimeTest;

	FVaCuusJsRuntime Runtime;
	FScopedContext Scoped(Runtime);
	if (!TestNotNull(TEXT("context"), Scoped.Ctx))
	{
		return false;
	}

	const int64 LiveBoot = Runtime.GetLiveBytes();
	TestTrue(TEXT("the accounting sees the boot heap at all"), LiveBoot > 0);

	constexpr int64 BufferBytes = 1024 * 1024;
	JSValue Ret = Eval(Scoped.Ctx, "globalThis.buf = new ArrayBuffer(1024 * 1024)");
	TestFalse(TEXT("the buffer allocates"), JS_IsException(Ret));
	JS_FreeValue(Scoped.Ctx, Ret);

	const int64 LiveHeld = Runtime.GetLiveBytes();
	TestTrue(FString::Printf(TEXT("the counter moved by at least the buffer (moved %lld)"), LiveHeld - LiveBoot),
		LiveHeld - LiveBoot >= BufferBytes);

	// Dropping the only reference frees the data by REFCOUNT, immediately -- no
	// collection involved, which the collections counter double-checks.
	Ret = Eval(Scoped.Ctx, "globalThis.buf = null");
	TestFalse(TEXT("the release evaluates"), JS_IsException(Ret));
	JS_FreeValue(Scoped.Ctx, Ret);

	TestTrue(FString::Printf(TEXT("the counter moved back (now %lld over boot)"), Runtime.GetLiveBytes() - LiveBoot),
		Runtime.GetLiveBytes() <= LiveHeld - BufferBytes);
	TestEqual(TEXT("no collection was involved anywhere here"), Runtime.GetNumCollections(), uint64(0));

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
