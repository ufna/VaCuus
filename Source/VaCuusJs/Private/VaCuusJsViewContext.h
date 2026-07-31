// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusJsRuntime.h"

/**
 * One view's JSContext (M4 spec 3.4): the global object, the callback queues,
 * and nothing shared. Created ON DEMAND by FVaCuusJsScriptHost at the first
 * script that needs the view -- a view that never runs JS never allocates one --
 * and destroyed in OnViewRemoved / Shutdown, which frees every callback JSValue
 * before JS_FreeContext.
 *
 * WHAT A CONTEXT OWNS (all per view, none of it visible to sibling views):
 *  - the globals installed at creation: console.*, the timer quartet, the rAF
 *    pair, and `document` -- which is NULL until a document is ready (spec 3.4;
 *    M4 Task 6 replaces it with the real document object on OnDocumentReady);
 *  - the timer list and the rAF pending list, with the demo's two re-entrancy
 *    rules and the frame-start cutoff the demo lacked (see PumpCallbacks);
 *  - from M4 Task 4 on: the element identity cache and the wrapper prototypes --
 *    this class is where the DOM facade's per-view state hangs.
 *
 * NOT here: the job queue. Promise reactions and queueMicrotask ride the
 * RUNTIME-wide job list (JS_EnqueueJob/JS_ExecutePendingJob are JSRuntime
 * APIs, quickjs.h:1205-1210), so the bounded drain lives in the host's
 * PumpFrame, after this context's callback phases.
 *
 * THREADING: the owning host's -- production is the VaCuus UI thread; library
 * tests drive host and context on the automation thread (legal, the Task 2
 * contract: quickjs has no thread-identity code, access is exclusive).
 */
class FVaCuusJsViewContext final
{
public:
	/**
	 * InNowSeconds seeds the deadline base for timers registered before the first
	 * pump (see PumpNowSeconds); the two Test* knobs are the pump-livelock tests'
	 * restore-the-bug plumbing (FVaCuusJsScriptHost::FParams documents them).
	 */
	FVaCuusJsViewContext(FVaCuusJsRuntime& InRuntime, uint32 InViewId, double InNowSeconds,
		bool bInTestRelaxTimerCutoff, int32 InTestTimerPassHardStop);

	/** Frees every held callback JSValue, then the JSContext -- before the runtime, always. */
	~FVaCuusJsViewContext();

	FVaCuusJsViewContext(const FVaCuusJsViewContext&) = delete;
	FVaCuusJsViewContext& operator=(const FVaCuusJsViewContext&) = delete;

	/** False only when JS_NewContext itself failed (heap at the cap); every method no-ops then. */
	bool IsValid() const { return Ctx != nullptr; }
	JSContext* GetContext() const { return Ctx; }
	uint32 GetViewId() const { return ViewId; }

	/**
	 * Evaluates Source as a classic global script against this context,
	 * SourceName naming it in errors and backtraces. Entry-guarded (watchdog
	 * armed at this boundary); a throw is consumed by the runtime's error sink.
	 * A job the script enqueues (a .then, a queueMicrotask) does NOT run here --
	 * jobs drain in the pump (spec 3.5).
	 */
	void Eval(const FString& Source, const FString& SourceName);

	/**
	 * This context's two callback phases of the pump, in spec 3.5's order:
	 * (1) rAF -- swap-out list, so a callback registered during the run lands
	 * NEXT frame; (2) timers due before the frame started. The caller (the
	 * host's PumpFrame) follows with phase (3), the runtime-wide job drain.
	 * NowSeconds is the frame's one timestamp (the RmlUi animation clock),
	 * FIXED for the whole pump -- that fixity is the livelock defense, see the
	 * cutoff comment in the implementation.
	 */
	void PumpCallbacks(double NowSeconds);

private:
	/**
	 * The demo's timer record (hud-demo VacuusJs.cpp:32-39), UE-shaped. Ids are
	 * monotonic from 1 per context; IntervalSeconds < 0 marks a one-shot;
	 * bDead defers the actual free to the post-pass sweep so a clear from inside
	 * a callback never mutates the array mid-iteration.
	 */
	struct FTimer
	{
		int64 Id = 0;
		double DeadlineSeconds = 0.0;
		double IntervalSeconds = -1.0;
		JSValue Fn;
		bool bDead = false;
	};

	/**
	 * A pending rAF callback. Handles are monotonic from 1 per context, a
	 * namespace of their own -- cancelAnimationFrame(timerId) cancels nothing,
	 * documented deviation nobody should rely on either way. bCanceled covers
	 * the one window removal cannot: an entry already swapped into the running
	 * list when a sibling cancels it mid-run.
	 */
	struct FRafEntry
	{
		int64 Handle = 0;
		JSValue Fn;
		bool bCanceled = false;
	};

	void InstallGlobals();

	//~ The global-function thunks. `this` rides JS_GetContextOpaque
	//~ (quickjs.h:524-525), set to the owning FVaCuusJsViewContext before any
	//~ script can run. Signatures per JSCFunctionType (quickjs.h:1272-1286).
	static JSValue ConsoleThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv, int Magic);
	static JSValue SetTimerThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv, int Magic);
	static JSValue ClearTimerThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv);
	static JSValue RequestRafThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv);
	static JSValue CancelRafThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv);

	FVaCuusJsRuntime& Runtime;
	JSContext* Ctx = nullptr;
	const uint32 ViewId;

	/**
	 * The deadline base: the CURRENT pump's now while a pump runs (set first
	 * thing in PumpCallbacks and never advanced within it), the PREVIOUS pump's
	 * now between pumps. So a timer registered outside the pump (ExecuteScript
	 * runs in DrainCommands, one phase earlier in the same frame) is based at
	 * most one frame early -- the same one-frame quantization every timer has
	 * anyway -- and a timer registered DURING the pump is based exactly AT the
	 * cutoff, which is what the strict due-test excludes. Seeded by the host at
	 * creation from ITS last pump; 0.0 only for a context created before the
	 * process's first UI frame ever pumped.
	 */
	double PumpNowSeconds = 0.0;

	int64 NextTimerId = 1;
	int64 NextRafHandle = 1;

	TArray<FTimer> Timers;

	/** Callbacks for the NEXT pump; registrations always land here (hud-demo VacuusJs.cpp:42). */
	TArray<FRafEntry> RafPending;

	/** Non-empty only inside PumpCallbacks' rAF phase (the swapped-out running list). */
	TArray<FRafEntry> RafRunning;

	//~ Test-only knobs, forwarded from FVaCuusJsScriptHost::FParams (see there).
	const bool bTestRelaxTimerCutoff;
	const int32 TestTimerPassHardStop;
};
