// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusJsViewContext.h"

#include "VaCuusJs.h"
#include "VaCuusJsDomHandle.h"
#include "VaCuusJsEventListener.h"

namespace VaCuusJsViewContextInternal
{
/** Console magic values (JS_NewCFunctionMagic routes them into ConsoleThunk's Magic). */
enum EConsoleLevel : int32
{
	Log = 0,
	Info,
	Warn,
	Error
};
}	 // namespace VaCuusJsViewContextInternal

FVaCuusJsViewContext::FVaCuusJsViewContext(FVaCuusJsRuntime& InRuntime, uint32 InViewId, double InNowSeconds,
	bool bInTestRelaxTimerCutoff, int32 InTestTimerPassHardStop)
	: Runtime(InRuntime)
	, ViewId(InViewId)
	, PumpNowSeconds(InNowSeconds)
	, bTestRelaxTimerCutoff(bInTestRelaxTimerCutoff)
	, TestTimerPassHardStop(InTestTimerPassHardStop)
{
	// JS_NewContext (quickjs.h:521) installs the full intrinsic set
	// (quickjs.c:2532-2555). NO JS_AddIntrinsicBigInt call on purpose: BigInt is
	// baked into the base objects on this engine -- Task 1's E3 probe evaluated
	// `typeof 1n` in exactly this context recipe and logged "bigint"
	// (VaCuusJsSmokeTest.cpp, the E3 block). queueMicrotask is likewise core
	// (js_global_funcs, quickjs.c:55939) and deliberately NOT shadowed here.
	Ctx = JS_NewContext(InRuntime.GetRuntime());
	if (Ctx == nullptr)
	{
		// Reachable only with the JS heap already at its cap when the first script
		// arrives -- the engine refuses the allocation without collecting
		// (quickjs.c:1645-1647). Refuse the view loudly rather than crash; the next
		// frame's GC point reclaims, and a later script can try again.
		UE_LOG(LogVaCuusJS, Error,
			TEXT("JS_NewContext failed for view %u (JS heap at its cap?); the view gets no context this time"), ViewId);
		return;
	}

	JS_SetContextOpaque(Ctx, this);

	// The module loader (M4 Task 7). A per-RUNTIME registration by API shape
	// (JS_SetModuleLoaderFunc takes the JSRuntime, quickjs.h:1178-1180), so
	// every context construction re-installs the same three pointers -- an
	// idempotent write, and doing it here rather than in the runtime's
	// Construct keeps the runtime module ignorant of this class. The opaque is
	// DELIBERATELY null: both thunks receive the JSContext and route through
	// JS_GetContextOpaque -> GetSelfOrNull, because the loader must serve
	// whichever context is importing RIGHT NOW (with several views one runtime
	// hosts several worlds), and null is the dead-context answer the thunks
	// already speak. The module CACHE needs no installation and no teardown of
	// its own: loaded modules live on the JSContext (ctx->loaded_modules,
	// quickjs.c:532, initialized :2524, populated :29652) and die inside
	// JS_FreeContext's sweep (:2605) -- which is exactly why a document reload's
	// context recycle re-executes every module (spec 3.4/3.7, pinned by
	// VaCuus.Js.Modules.CacheDiesWithContext).
	JS_SetModuleLoaderFunc(
		InRuntime.GetRuntime(), &FVaCuusJsViewContext::ModuleNormalizeThunk, &FVaCuusJsViewContext::ModuleLoaderThunk, nullptr);

	InstallGlobals();
	InstallDomPrototypes();
}

FVaCuusJsViewContext::~FVaCuusJsViewContext()
{
	if (Ctx == nullptr)
	{
		return;
	}

	// Never destroyed mid-pump: OnViewRemoved runs from DrainCommands, which the
	// frame finishes BEFORE PumpFrame (VaCuusUIThread.cpp RunFrame's phase order).
	check(RafRunning.IsEmpty());

	// THE NEUTER WALK -- death order (3), the half the demo never had (spec
	// 2(g)). This context is dying while its listeners' elements may still be
	// attached to a live tree; RmlUi's OnDetach for them arrives LATER (the old
	// tree's deferred teardown one frame after a recycle, Context.cpp:1557-1567,
	// or RemoveContext inside the document host's Shutdown -- which RemoveView
	// runs strictly AFTER this destructor, VaCuusUIThread.cpp RemoveView).
	// So: free every listener's JS ref NOW, against the still-live JSContext,
	// and mark the shells neutered -- the C++ objects stay allocated, owning
	// nothing, and the later OnDetach reclaims them without touching this dead
	// context (or the runtime, which on the shutdown path is dead by then too).
	// The containers are emptied wholesale; NeuterFromContext does not
	// unregister. RESTORE-THE-BUG: releasing refs in OnDetach only -- skipping
	// this walk's frees -- leaks exactly the still-attached listeners' function
	// refs, visible as a nonzero GetNumListenerRefs() after the recycle and as
	// the runtime destructor's live-byte checkf at teardown.
	for (TPair<FVaCuusJsListenerKey, FVaCuusJsEventListener*>& Pair : ListenerRegistry)
	{
		Pair.Value->NeuterFromContext();
	}
	ListenerRegistry.Empty();
	for (FVaCuusJsEventListener* Listener : AttributeListeners)
	{
		Listener->NeuterFromContext();
	}
	AttributeListeners.Empty();

	// NEUTER THE CACHE, ALSO BEFORE JS_FreeContext. It finalizes every surviving
	// wrapper, and each finalizer would otherwise reach back through its
	// OwnerContext into a map that is mid-destruction. Clearing the handshake
	// flag (and the pointer, for good measure) turns those finalizers into pure
	// releases: free the Owned ElementPtr -- legal, this destructor always runs
	// before Rml::Shutdown, so the instancers are alive (spec 2(g)) -- and
	// delete the handle. The values are BORROWED (WrapperCache's comment), so
	// emptying the map frees nothing.
	for (TPair<Rml::Element*, JSValue>& Pair : WrapperCache)
	{
		JSClassID ClassId = 0;
		if (FVaCuusJsElementHandle* Handle = static_cast<FVaCuusJsElementHandle*>(JS_GetAnyOpaque(Pair.Value, &ClassId)))
		{
			Handle->bInCache = false;
			Handle->OwnerContext = nullptr;
		}
	}
	WrapperCache.Empty();

	// Every held JSValue goes before the context, the context before the runtime
	// (research note quickjs-ng-0151.md section 2); the runtime destructor's
	// live-byte check is what would catch a miss here.
	JS_FreeValue(Ctx, StyleFactory);
	StyleFactory = JS_UNDEFINED;
	for (FTimer& Timer : Timers)
	{
		JS_FreeValue(Ctx, Timer.Fn);
	}
	for (FRafEntry& Entry : RafPending)
	{
		JS_FreeValue(Ctx, Entry.Fn);
	}
	Timers.Empty();
	RafPending.Empty();

	// NULL THE OPAQUE BEFORE THE FREE, because the free is not the end: a job
	// this context enqueued before death still sits on the RUNTIME's list with
	// dup'd argv (JS_EnqueueJob, quickjs.c:2146-2154), and those values' function
	// realms hold the context refcount above zero past the decrement below
	// (JS_FreeContext, quickjs.c:2681-2682) -- so the surviving allocation runs
	// that job later, from a surviving view's drain segment, and every thunk it
	// calls reads this opaque. Left pointing at `this`, that read is a dangling
	// pointer a check(non-null) happily passes; nulled, GetSelfOrNull answers
	// null and every thunk takes its graceful dead-context branch. The
	// Pump.Lifecycle test pins the scenario (green side only -- the broken state
	// is use-after-free, argued from the two cites above, not run).
	JS_SetContextOpaque(Ctx, nullptr);
	JS_FreeContext(Ctx);
	Ctx = nullptr;
}

void FVaCuusJsViewContext::InstallGlobals()
{
	using namespace VaCuusJsViewContextInternal;

	// JS_SetPropertyStr takes ownership of every value handed to it (the
	// JSValue-parameter rule, quickjs.h:199-201), so nothing below needs a free
	// except the global object itself. Failure is possible only at the memory
	// cap; a missing global then surfaces as a plain ReferenceError in the first
	// script, which is diagnosis enough for a state this degenerate.
	JSValue Global = JS_GetGlobalObject(Ctx);

	// `document` exists from birth and is NULL until a document is ready (spec
	// 3.4, tested): scripts can feature-test `document === null` instead of
	// tripping a ReferenceError. BindDocument swaps in the real Document wrapper
	// -- from OnDocumentReady / EnsureViewContext in production (M4 Task 6), or
	// through the host's test-only entry.
	JS_SetPropertyStr(Ctx, Global, "document", JS_NULL);

	// The `vacuus` host object (M4 Task 6 mints it, Task 9 populates it with
	// model()/log). Tier 1 surface: `vacuus.onUnload` -- assign a function and
	// DispatchUnload invokes it at close time. Seeded NULL rather than left
	// absent so `vacuus.onUnload === null` feature-tests cleanly, mirroring
	// `document`.
	JSValue Vacuus = JS_NewObject(Ctx);
	JS_SetPropertyStr(Ctx, Vacuus, "onUnload", JS_NULL);
	JS_SetPropertyStr(Ctx, Global, "vacuus", Vacuus);

	JSValue Console = JS_NewObject(Ctx);
	JS_SetPropertyStr(Ctx, Console, "log",
		JS_NewCFunctionMagic(Ctx, &FVaCuusJsViewContext::ConsoleThunk, "log", 0, JS_CFUNC_generic_magic, EConsoleLevel::Log));
	JS_SetPropertyStr(Ctx, Console, "info",
		JS_NewCFunctionMagic(Ctx, &FVaCuusJsViewContext::ConsoleThunk, "info", 0, JS_CFUNC_generic_magic, EConsoleLevel::Info));
	JS_SetPropertyStr(Ctx, Console, "warn",
		JS_NewCFunctionMagic(Ctx, &FVaCuusJsViewContext::ConsoleThunk, "warn", 0, JS_CFUNC_generic_magic, EConsoleLevel::Warn));
	JS_SetPropertyStr(Ctx, Console, "error",
		JS_NewCFunctionMagic(Ctx, &FVaCuusJsViewContext::ConsoleThunk, "error", 0, JS_CFUNC_generic_magic, EConsoleLevel::Error));
	JS_SetPropertyStr(Ctx, Global, "console", Console);

	JS_SetPropertyStr(Ctx, Global, "setTimeout",
		JS_NewCFunctionMagic(Ctx, &FVaCuusJsViewContext::SetTimerThunk, "setTimeout", 2, JS_CFUNC_generic_magic, 0));
	JS_SetPropertyStr(Ctx, Global, "setInterval",
		JS_NewCFunctionMagic(Ctx, &FVaCuusJsViewContext::SetTimerThunk, "setInterval", 2, JS_CFUNC_generic_magic, 1));

	// One C function under both names, the demo's shape (hud-demo
	// VacuusJs.cpp:405-416, registered :463-464): the two clears are one
	// operation, and hud.js leans on unknown ids -- including 0 -- being no-ops.
	JS_SetPropertyStr(Ctx, Global, "clearTimeout",
		JS_NewCFunction(Ctx, &FVaCuusJsViewContext::ClearTimerThunk, "clearTimeout", 1));
	JS_SetPropertyStr(Ctx, Global, "clearInterval",
		JS_NewCFunction(Ctx, &FVaCuusJsViewContext::ClearTimerThunk, "clearInterval", 1));

	JS_SetPropertyStr(Ctx, Global, "requestAnimationFrame",
		JS_NewCFunction(Ctx, &FVaCuusJsViewContext::RequestRafThunk, "requestAnimationFrame", 1));
	JS_SetPropertyStr(Ctx, Global, "cancelAnimationFrame",
		JS_NewCFunction(Ctx, &FVaCuusJsViewContext::CancelRafThunk, "cancelAnimationFrame", 1));

	JS_FreeValue(Ctx, Global);
}

void FVaCuusJsViewContext::Eval(const FString& Source, const FString& SourceName)
{
	if (Ctx == nullptr)
	{
		return;
	}

	const FTCHARToUTF8 SourceUtf8(*Source);
	const FTCHARToUTF8 NameUtf8(*SourceName);

	// The entry-guard contract (VaCuusJsRuntime.h): the guard wraps the JS call
	// ONLY and closes before the exception is consumed, so a watchdog trip has
	// been made catchable again by the time the sink reads it.
	JSValue Ret;
	{
		FVaCuusJsEntryGuard Guard(Runtime, Ctx, *SourceName);
		Ret = JS_Eval(Ctx, SourceUtf8.Get(), SourceUtf8.Length(), NameUtf8.Get(), JS_EVAL_TYPE_GLOBAL);
	}
	if (JS_IsException(Ret))
	{
		Runtime.ReportException(Ctx, *SourceName);
	}
	JS_FreeValue(Ctx, Ret);
}

void FVaCuusJsViewContext::DispatchUnload()
{
	if (Ctx == nullptr)
	{
		return;
	}

	// Read through the property chain at DISPATCH time, not registration time:
	// whatever function `vacuus.onUnload` holds right now is the callback, so a
	// script can re-assign or clear it up to the last moment -- the on*
	// attribute's read-at-fire semantics, one level up. A script that clobbered
	// the `vacuus` global with a non-object simply has no callback; property
	// reads on a plain object cannot throw here.
	JSValue Global = JS_GetGlobalObject(Ctx);
	JSValue Vacuus = JS_GetPropertyStr(Ctx, Global, "vacuus");
	JS_FreeValue(Ctx, Global);
	if (!JS_IsObject(Vacuus))
	{
		JS_FreeValue(Ctx, Vacuus);
		return;
	}

	JSValue Fn = JS_GetPropertyStr(Ctx, Vacuus, "onUnload");
	JS_FreeValue(Ctx, Vacuus);
	if (!JS_IsFunction(Ctx, Fn))
	{
		JS_FreeValue(Ctx, Fn);
		return;
	}

	// The house entry shape (Eval's): guard wraps the call only, the exception
	// is consumed after it closes, a throw is the callback's own problem and
	// never escapes into the close path that asked.
	JSValue Ret;
	{
		FVaCuusJsEntryGuard Guard(Runtime, Ctx, TEXT("vacuus.onUnload"));
		Ret = JS_Call(Ctx, Fn, JS_UNDEFINED, 0, nullptr);
	}
	Runtime.NoteUnloadCallbackRun();
	if (JS_IsException(Ret))
	{
		Runtime.ReportException(Ctx, TEXT("vacuus.onUnload"));
	}
	JS_FreeValue(Ctx, Ret);
	JS_FreeValue(Ctx, Fn);
}

void FVaCuusJsViewContext::PumpCallbacks(double NowSeconds)
{
	if (Ctx == nullptr)
	{
		return;
	}

	// FIXED at pump top, deliberately, and everything below prices off it: the
	// rAF timestamp, the timer cutoff, and -- because this member is also the
	// registration base -- the deadline of every timer a callback registers
	// during this pump. That last coupling is the livelock defense; see the
	// cutoff comment in the timer pass.
	PumpNowSeconds = NowSeconds;

	//~ Phase 1: rAF, swap-out semantics. The pending list is swapped out BEFORE
	//~ the run, so a callback registered during the run lands in the fresh
	//~ RafPending and fires NEXT frame (hud-demo VacuusJs.cpp:531-534) -- the
	//~ rule the class-toggle animation-restart idiom depends on (clear a class,
	//~ re-add it inside the next rAF; hud.js:83-87).
	check(RafRunning.IsEmpty());
	Swap(RafRunning, RafPending);
	if (!RafRunning.IsEmpty())
	{
		// One timestamp for every callback this frame, in ms of the frame's now
		// (hud-demo VacuusJs.cpp:535-541) -- the same clock RmlUi advances its
		// animations on, sampled once by the frame loop (VaCuusUIThread.cpp's
		// PumpFrame call site).
		const JSValue TimestampMs = JS_NewFloat64(Ctx, NowSeconds * 1000.0);
		JSValueConst RafArgs[1] = {TimestampMs};

		// RafRunning cannot grow during the run (registrations land in RafPending),
		// so references stay valid; cancelAnimationFrame can only FLAG entries here.
		for (FRafEntry& Entry : RafRunning)
		{
			if (Entry.bCanceled)
			{
				JS_FreeValue(Ctx, Entry.Fn);
				continue;
			}

			JSValue Ret;
			{
				FVaCuusJsEntryGuard Guard(Runtime, Ctx, TEXT("requestAnimationFrame"));
				Ret = JS_Call(Ctx, Entry.Fn, JS_UNDEFINED, 1, RafArgs);
			}
			// Counted even on a throw: the callback RAN. And a throw never skips the
			// siblings -- each iteration consumes its own exception and moves on.
			Runtime.NoteRafCallbackRun();
			if (JS_IsException(Ret))
			{
				Runtime.ReportException(Ctx, TEXT("requestAnimationFrame"));
			}
			JS_FreeValue(Ctx, Ret);
			JS_FreeValue(Ctx, Entry.Fn);
		}
		RafRunning.Reset();
		JS_FreeValue(Ctx, TimestampMs);
	}

	//~ Phase 2: timers due before the frame started.
	const double CutoffSeconds = NowSeconds;
	int32 NumFiredThisPass = 0;

	// Index loop against the LIVE Num(): callbacks may append new timers
	// (hud-demo VacuusJs.cpp:544-545). An appended entry IS visited this pass --
	// and that is safe only because of the cutoff below.
	for (int32 Index = 0; Index < Timers.Num(); ++Index)
	{
		if (Timers[Index].bDead)
		{
			continue;
		}

		// THE FRAME-START CUTOFF, and the strictness is the whole point. Every
		// deadline is priced off PumpNowSeconds, which this pump FIXED at
		// CutoffSeconds above -- so a 0 ms timer registered by a callback during
		// this pump has DeadlineSeconds == CutoffSeconds exactly, and the strict
		// `<` excludes it until the next pump, whose cutoff will be strictly
		// later. The demo's due-test was `deadline <= now` with the same value on
		// both sides (VacuusJs.cpp:547 against the deadline set at :390), so a
		// 0 ms timer that re-registers itself was appended due-NOW into the array
		// being index-iterated: the pass could never reach the end -- the recorded
		// livelock (research note hud-demo-patterns.md section 4). The relaxed
		// branch is that bug, kept compilable ONLY so the livelock test's red
		// half can observe it (bounded by TestTimerPassHardStop).
		const bool bDue = bTestRelaxTimerCutoff ? Timers[Index].DeadlineSeconds <= CutoffSeconds
												: Timers[Index].DeadlineSeconds < CutoffSeconds;
		if (!bDue)
		{
			continue;
		}

		if (TestTimerPassHardStop > 0 && NumFiredThisPass >= TestTimerPassHardStop)
		{
			// TEST-ONLY bound (never armed in production): what makes the relaxed
			// branch's non-termination OBSERVABLE without hanging the suite -- the
			// red test asserts the pass fired exactly this many times and only then
			// returned, where the strict cutoff fires once.
			UE_LOG(LogVaCuusJS, Warning,
				TEXT("JS timer pass for view %u hit its test-only hard stop (%d fires in one pass)"),
				ViewId, TestTimerPassHardStop);
			break;
		}
		++NumFiredThisPass;

		const bool bIsInterval = Timers[Index].IntervalSeconds >= 0.0;
		if (bIsInterval)
		{
			// Re-arm from FIRE time (this frame's fixed now), not from the old
			// deadline: intervals drift under load instead of bursting to catch up
			// (hud-demo VacuusJs.cpp:549-550).
			Timers[Index].DeadlineSeconds = NowSeconds + Timers[Index].IntervalSeconds;
		}
		else
		{
			// One-shots die BEFORE the call (hud-demo VacuusJs.cpp:552): a
			// re-registration inside the callback is a NEW timer with a new id, and
			// a clear of the old id during the callback is a harmless no-op.
			Timers[Index].bDead = true;
		}

		// Dup BEFORE the call, because the callback may clear itself (hud-demo
		// VacuusJs.cpp:553) -- the sweep below would otherwise free the function
		// out from under later use. NO reference into Timers is held across the
		// call: the callback may append and reallocate the array.
		const JSValue Fn = JS_DupValue(Ctx, Timers[Index].Fn);
		JSValue Ret;
		{
			FVaCuusJsEntryGuard Guard(Runtime, Ctx, bIsInterval ? TEXT("setInterval") : TEXT("setTimeout"));
			Ret = JS_Call(Ctx, Fn, JS_UNDEFINED, 0, nullptr);
		}
		Runtime.NoteTimerFired();
		if (JS_IsException(Ret))
		{
			Runtime.ReportException(Ctx, bIsInterval ? TEXT("setInterval") : TEXT("setTimeout"));
		}
		JS_FreeValue(Ctx, Ret);
		JS_FreeValue(Ctx, Fn);
	}

	// Sweep dead entries AFTER the pass (hud-demo VacuusJs.cpp:557-566): the
	// pass itself only ever flags, so indexes stay meaningful throughout it.
	for (FTimer& Timer : Timers)
	{
		if (Timer.bDead)
		{
			JS_FreeValue(Ctx, Timer.Fn);
		}
	}
	Timers.RemoveAll([](const FTimer& Timer) { return Timer.bDead; });
}

JSValue FVaCuusJsViewContext::ConsoleThunk(
	JSContext* Ctx, JSValueConst /*This*/, int Argc, JSValueConst* Argv, int Magic)
{
	using namespace VaCuusJsViewContextInternal;

	FString Joined;
	for (int32 Index = 0; Index < Argc; ++Index)
	{
		if (Index > 0)
		{
			Joined.AppendChar(TEXT(' '));
		}
		if (const char* Utf8 = JS_ToCString(Ctx, Argv[Index]))
		{
			Joined += UTF8_TO_TCHAR(Utf8);
			JS_FreeCString(Ctx, Utf8);
		}
		else
		{
			// JS_ToCString failed and set ITS OWN exception (a toString that
			// throws); a log call must not hand that back to the script, so clear
			// it and print a placeholder.
			JS_FreeValue(Ctx, JS_GetException(Ctx));
			Joined += TEXT("<unprintable>");
		}
	}

	// The UE_LOG verbosity argument must be a literal, hence the switch. info
	// maps alongside log itself: UE has no fourth severity below Warning worth
	// the distinction. DISPLAY, NOT Log, for the pair, and the choice is forced:
	// the automation framework's log capture only feeds Error/Warning/Display
	// into the expected-message matcher (FAutomationTestOutputDevice::Serialize,
	// AutomationTest.cpp:233-234, matched via AddInfo/AddEvent at :1747, :1796)
	// -- a Log-verbosity console.log could never be asserted by any test. Display
	// also reaches stdout, which is where a developer expects console.log to be.
	switch (Magic)
	{
		case EConsoleLevel::Warn:
			UE_LOG(LogVaCuusJS, Warning, TEXT("%s"), *Joined);
			break;
		case EConsoleLevel::Error:
			UE_LOG(LogVaCuusJS, Error, TEXT("%s"), *Joined);
			break;
		default:
			UE_LOG(LogVaCuusJS, Display, TEXT("%s"), *Joined);
			break;
	}
	return JS_UNDEFINED;
}

JSValue FVaCuusJsViewContext::SetTimerThunk(
	JSContext* Ctx, JSValueConst /*This*/, int Argc, JSValueConst* Argv, int Magic)
{
	FVaCuusJsViewContext* Self = GetSelfOrNull(Ctx);
	if (Self == nullptr)
	{
		return JS_UNDEFINED;	// dead context (a removed view's pinned job): no timer to mint
	}
	const bool bRepeat = Magic != 0;

	if (Argc < 1 || !JS_IsFunction(Ctx, Argv[0]))
	{
		// The demo's contract (hud-demo VacuusJs.cpp:380-381): no string-eval
		// form, a non-function is a TypeError.
		return JS_ThrowTypeError(Ctx, "%s: callback must be a function", bRepeat ? "setInterval" : "setTimeout");
	}

	double Ms = 0.0;
	if (Argc >= 2 && JS_ToFloat64(Ctx, &Ms, Argv[1]) != 0)
	{
		return JS_EXCEPTION;
	}
	// Negatives clamp to 0 (hud-demo VacuusJs.cpp:385-386); the negated
	// comparison folds NaN into the same clamp instead of minting a deadline
	// that never becomes due.
	if (!(Ms > 0.0))
	{
		Ms = 0.0;
	}

	// EXTRA ARGUMENTS ARE NOT FORWARDED to the callback, matching the demo's
	// zero-arg dispatch (hud-demo VacuusJs.cpp:554) -- a documented Tier 1
	// deviation from the web signature; bind or close over what you need.
	FTimer& Timer = Self->Timers.AddDefaulted_GetRef();
	Timer.Id = Self->NextTimerId++;
	Timer.DeadlineSeconds = Self->PumpNowSeconds + Ms / 1000.0;
	Timer.IntervalSeconds = bRepeat ? Ms / 1000.0 : -1.0;
	Timer.Fn = JS_DupValue(Ctx, Argv[0]);
	return JS_NewInt64(Ctx, Timer.Id);
}

JSValue FVaCuusJsViewContext::ClearTimerThunk(JSContext* Ctx, JSValueConst /*This*/, int Argc, JSValueConst* Argv)
{
	FVaCuusJsViewContext* Self = GetSelfOrNull(Ctx);
	if (Self == nullptr)
	{
		return JS_UNDEFINED;	// dead context: a clear is already a no-op for unknown ids
	}

	if (Argc < 1)
	{
		return JS_UNDEFINED;
	}
	int64_t Id = 0;
	if (JS_ToInt64(Ctx, &Id, Argv[0]) != 0)
	{
		return JS_EXCEPTION;
	}

	// Flag, never erase: the pump's sweep owns removal, so a clear from inside a
	// running callback cannot shift the array under the index loop. Unknown ids
	// -- including 0, which hud.js uses as its "no timer" sentinel (hud.js:81,
	// :86) -- fall through as no-ops.
	for (FTimer& Timer : Self->Timers)
	{
		if (Timer.Id == Id)
		{
			Timer.bDead = true;
		}
	}
	return JS_UNDEFINED;
}

JSValue FVaCuusJsViewContext::RequestRafThunk(JSContext* Ctx, JSValueConst /*This*/, int Argc, JSValueConst* Argv)
{
	FVaCuusJsViewContext* Self = GetSelfOrNull(Ctx);
	if (Self == nullptr)
	{
		return JS_UNDEFINED;	// dead context: no frame will ever come (the demo's rAF returned undefined too)
	}

	if (Argc < 1 || !JS_IsFunction(Ctx, Argv[0]))
	{
		return JS_ThrowTypeError(Ctx, "requestAnimationFrame: callback must be a function");
	}

	// Always into RafPending -- during a pump that is the NEXT frame's list (the
	// swap-out already happened). Returns a handle; the demo returned undefined
	// and had no cancel (hud-demo VacuusJs.cpp:418-424) -- the handle is Tier 1's
	// addition, monotonic like the timer ids but its own namespace.
	FRafEntry& Entry = Self->RafPending.AddDefaulted_GetRef();
	Entry.Handle = Self->NextRafHandle++;
	Entry.Fn = JS_DupValue(Ctx, Argv[0]);
	return JS_NewInt64(Ctx, Entry.Handle);
}

JSValue FVaCuusJsViewContext::CancelRafThunk(JSContext* Ctx, JSValueConst /*This*/, int Argc, JSValueConst* Argv)
{
	FVaCuusJsViewContext* Self = GetSelfOrNull(Ctx);
	if (Self == nullptr)
	{
		return JS_UNDEFINED;	// dead context: nothing pending to cancel
	}

	if (Argc < 1)
	{
		return JS_UNDEFINED;
	}
	int64_t Handle = 0;
	if (JS_ToInt64(Ctx, &Handle, Argv[0]) != 0)
	{
		return JS_EXCEPTION;
	}

	// A pending (next-frame) entry can simply be removed -- nothing iterates
	// RafPending while script runs.
	for (int32 Index = 0; Index < Self->RafPending.Num(); ++Index)
	{
		if (Self->RafPending[Index].Handle == Handle)
		{
			JS_FreeValue(Ctx, Self->RafPending[Index].Fn);
			Self->RafPending.RemoveAt(Index);
			return JS_UNDEFINED;
		}
	}

	// An entry already swapped into the running list -- a sibling canceling it
	// mid-run -- can only be FLAGGED: the run loop is iterating that array and
	// owns the frees. Browser semantics: a not-yet-run callback of the current
	// batch is still cancelable.
	for (FRafEntry& Entry : Self->RafRunning)
	{
		if (Entry.Handle == Handle)
		{
			Entry.bCanceled = true;
		}
	}
	return JS_UNDEFINED;
}
