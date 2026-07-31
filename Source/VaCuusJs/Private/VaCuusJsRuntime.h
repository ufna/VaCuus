// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include <atomic>

// The vendored header, reachable only inside this module (PrivateIncludePaths --
// see VaCuusJs.Build.cs). This header is itself Private/ for the same reason:
// nothing outside VaCuusJs may see a JSRuntime.
#include "quickjs.h"

/**
 * The process's one quickjs runtime (M4 spec 2(e)): capped, watched, counted.
 *
 * Owns the JSRuntime and everything registered on it -- the FMemory-forwarding
 * malloc hooks with their live-byte accounting, the memory cap, the JS stack
 * budget, the disabled implicit GC, the watchdog interrupt handler and the
 * promise-rejection tracker. It owns no JSContext: those are per view and land
 * with M4 Task 3.
 *
 * THREADING: no thread affinity of its own, matching the engine's contract --
 * quickjs has no thread-identity code at all; the one thread-sensitive datum is
 * the native-stack anchor, captured on the creating thread (quickjs.c:2019) and
 * moved with UpdateStackTopOnThisThread(). Production creates and uses this on
 * the VaCuus UI thread; library-level tests legally create and use one on the
 * automation thread. Access is EXCLUSIVE by contract ("no multi-threading
 * inside a runtime" -- the engine's whole official statement), which is why the
 * counters below are the only cross-thread-readable members.
 *
 * THE COUNTER PATTERN, and why it is NOT GNumRefusedSets' (the M3b layer:
 * plain file-static ints behind check(IsInUIThread()) accessors,
 * VaCuusDataVariable.cpp:852-855). Two reasons, both structural: these are
 * INSTANCE state -- a test runtime on the automation thread must not share
 * counters with a production runtime, and an asserted-UI-thread accessor would
 * simply fire there; and they have legitimate cross-thread READERS (stats and
 * diagnostics sample them from the game thread), which a plain int cannot
 * serve. So: std::atomic members, relaxed -- single writer (the owning
 * thread), any reader, no ordering to protect.
 */
class FVaCuusJsRuntime final
{
public:
	/**
	 * Construction knobs. Defaults (-1) resolve to the cvars --
	 * vacuus.Js.MemoryLimitMB, vacuus.Js.WatchdogMs, vacuus.Js.GCStepKB -- read
	 * once, at construction; tests pass direct values instead of mutating global
	 * cvar state.
	 */
	struct FParams
	{
		/** JS heap cap in MB; 0 means uncapped (quickjs "use 0 to disable", quickjs.h:498-499). */
		int32 MemoryLimitMB = -1;

		/**
		 * Per-entry watchdog deadline in ms; 0 disables arming entirely. 0 IS THE
		 * DELIBERATELY-BROKEN STATE the watchdog test's restore-the-bug half runs:
		 * with it, a `for(;;)` entered through the guard never terminates (the test
		 * bounds the proof with its own escape interrupt handler).
		 */
		int32 WatchdogMs = -1;

		/** Collect when live bytes grew this much since the last collection; <= 0 falls back to the cvar. */
		int32 GCStepKB = -1;

		/**
		 * TEST ONLY -- the no-implicit-GC test's restore-the-bug half: true skips the
		 * JS_SetGCThreshold(rt, -1) call, leaving the engine's own allocation-driven
		 * collector armed at its 256 KiB default (quickjs.c:1980). Production never
		 * sets this; it exists so the test can PROVE its climbing-bytes assertion
		 * fails when the disable line is missing.
		 */
		bool bTestLeaveEngineGCEnabled = false;
	};

	/** Cvar-configured; the production path. */
	FVaCuusJsRuntime();
	explicit FVaCuusJsRuntime(const FParams& InParams);

	/**
	 * JS_FreeRuntime, which frees still-queued jobs itself, runs a final GC and
	 * releases every block through our hooks (quickjs.c:2288-2348). Afterwards the
	 * live-byte counter is check()ed back to zero -- the Development-real leak
	 * observable spec 5 demands, because the engine's own
	 * assert(list_empty(&rt->gc_obj_list)) (quickjs.c:2348) is <assert.h> under
	 * UBT's global NDEBUG and therefore compiled out in every configuration this
	 * project runs.
	 */
	~FVaCuusJsRuntime();

	FVaCuusJsRuntime(const FVaCuusJsRuntime&) = delete;
	FVaCuusJsRuntime& operator=(const FVaCuusJsRuntime&) = delete;

	bool IsValid() const { return Runtime != nullptr; }
	JSRuntime* GetRuntime() const { return Runtime; }

	//~ The DOM facade's wrapper classes (M4 Task 4). Registered HERE because
	//~ JSClassID allocation and JS_NewClass are per-RUNTIME operations
	//~ (quickjs.h:693, :696); the per-CONTEXT half -- the prototypes -- is
	//~ JS_SetClassProto (quickjs.h:527), installed by each FVaCuusJsViewContext.
	//~ Two classes, one finalizer, one opaque shape (FVaCuusJsElementHandle):
	//~ a document IS an Rml::Element, it just carries a richer prototype.
	JSClassID GetElementClassId() const { return ElementClassId; }
	JSClassID GetDocumentClassId() const { return DocumentClassId; }

	//~ The event-object class (M4 Task 5): same split, own opaque shape
	//~ (FVaCuusJsEventHandle) and own finalizer -- an event wrapper holds no
	//~ ElementPtr and no cache entry, so sharing the DOM finalizer would be a
	//~ type confusion waiting for a payload.
	JSClassID GetEventClassId() const { return EventClassId; }

	/**
	 * Live bytes currently held from FMemory by this runtime, in the allocator's
	 * OWN accounting basis: the sum of FMemory::GetAllocSize over live blocks,
	 * not of requested byte counts. See HookMalloc for the two properties the
	 * accounting actually rests on -- add/subtract symmetry, and the
	 * constructor's zero-probe. This counter -- not JS_ComputeMemoryUsage, which
	 * walks the whole heap -- is what drives the per-frame GC trigger (spec 2(b)).
	 */
	int64 GetLiveBytes() const { return LiveBytes.load(std::memory_order_relaxed); }

	//~ The fired-counters (spec 6). Timers/rAF/jobs are incremented by the view
	//~ contexts from M4 Task 3 on; declared now so the idle gate's "exact zeros"
	//~ row has its observables from the first frame the pump exists.
	uint64 GetNumTimersFired() const { return NumTimersFired.load(std::memory_order_relaxed); }
	uint64 GetNumRafCallbacksRun() const { return NumRafCallbacksRun.load(std::memory_order_relaxed); }
	uint64 GetNumJobsExecuted() const { return NumJobsExecuted.load(std::memory_order_relaxed); }
	uint64 GetNumCollections() const { return NumCollections.load(std::memory_order_relaxed); }
	uint64 GetNumErrors() const { return NumErrors.load(std::memory_order_relaxed); }
	uint64 GetNumWatchdogTrips() const { return NumWatchdogTrips.load(std::memory_order_relaxed); }

	void NoteTimerFired() { NumTimersFired.fetch_add(1, std::memory_order_relaxed); }
	void NoteRafCallbackRun() { NumRafCallbacksRun.fetch_add(1, std::memory_order_relaxed); }
	void NoteJobExecuted() { NumJobsExecuted.fetch_add(1, std::memory_order_relaxed); }

	/**
	 * THE THREE-DEATH-ORDERS OBSERVABLE (M4 Task 5, spec 2(g)): the number of
	 * listener-held JS function refs currently alive, a GAUGE, not a fired-count.
	 * +1 when a listener dups its callback (addEventListener, or an on* snippet
	 * compiling at first fire), -1 at the release -- which happens on exactly one
	 * of the three paths: OnDetach after direct element destruction
	 * (Element.cpp:99 -> :112), OnDetach after a document unload's tree-wide
	 * detach sweep (Context.cpp:1565-1567), or the context destructor's neuter
	 * walk when the context dies first. Zero after any teardown is the invariant
	 * the death-order test asserts; a nonzero reading names the leaking path.
	 * An invariant with no observable cannot be tested -- this is the observable.
	 */
	int64 GetNumListenerRefs() const { return NumListenerRefs.load(std::memory_order_relaxed); }
	void NoteListenerRefAcquired() { NumListenerRefs.fetch_add(1, std::memory_order_relaxed); }
	void NoteListenerRefReleased() { NumListenerRefs.fetch_sub(1, std::memory_order_relaxed); }

	/** Wall-clock ms of the most recent collection; 0 until one has run. Owning thread. */
	double GetLastCollectionPauseMs() const { return LastCollectionPauseMs; }

	/** JSMemoryUsage.malloc_size sampled at the most recent collection. Owning thread. */
	uint64 GetLastCollectionHeapBytes() const { return LastCollectionHeapBytes; }

	/**
	 * The controlled GC point (spec 3.6). Collects when live bytes grew >= the
	 * configured step since the last collection, or when the OOM fallback is
	 * pending (spec 2(b): at the cap the engine returns NULL without collecting,
	 * quickjs.c:1645-1647 -- somebody has to reclaim afterwards, and it is us).
	 * Samples JS_ComputeMemoryUsage at collections only. Returns whether a
	 * collection ran. Owning thread.
	 */
	bool CollectGarbage(const TCHAR* Reason);

	/**
	 * Arms the OOM fallback so the next CollectGarbage() collects regardless of
	 * growth. Called by the error sink when it recognizes the engine's OOM
	 * InternalError; public because Task 3's pump paths will hit the same error
	 * from more entry points.
	 */
	void NoteOutOfMemory() { bOomFallbackPending = true; }

	bool IsOomFallbackPending() const { return bOomFallbackPending; }

	/**
	 * The M4 Task 2 error sink (Task 8 builds the overlay on top of it): consumes
	 * the pending exception -- JS_GetException returns AND clears, "cannot be
	 * called twice" (quickjs.c:7602-7610) -- logs message plus `stack` to
	 * LogVaCuusJS, counts it, and arms the OOM fallback when the exception is the
	 * engine's own InternalError "out of memory" (JS_ThrowOutOfMemory,
	 * quickjs.c:8127-8136; the implementation has the exact predicate and why).
	 * Call it AFTER a guarded entry's FVaCuusJsEntryGuard has closed, so a
	 * watchdog trip has already been made catchable again. Owning thread.
	 */
	void ReportException(JSContext* Ctx, const TCHAR* SourceName);

	/**
	 * JS_UpdateStackTop on the calling thread (quickjs.h:506-508: "should be
	 * called when changing thread"): re-captures the native-stack anchor the
	 * overflow check compares against (quickjs.c:1952-1957). Inline mode calls
	 * this once per frame via IVaCuusScriptHost::OnInlineFrameEntry().
	 */
	void UpdateStackTopOnThisThread();

private:
	friend struct FVaCuusJsEntryGuard;

	void Construct(const FParams& InParams);

	//~ The JSMallocFunctions hooks (quickjs.h:456-462). `Opaque` is the
	//~ FVaCuusJsRuntime*, wired through JS_NewRuntime2's opaque parameter.
	static void* HookCalloc(void* Opaque, size_t Count, size_t Size);
	static void* HookMalloc(void* Opaque, size_t Size);
	static void HookFree(void* Opaque, void* Ptr);
	static void* HookRealloc(void* Opaque, void* Ptr, size_t Size);
	static size_t HookUsableSize(const void* Ptr);

	/** The JSInterruptHandler (quickjs.h:1144-1146); non-zero return interrupts JS. */
	static int InterruptThunk(JSRuntime* Rt, void* Opaque);

	/** The JSHostPromiseRejectionTracker (quickjs.h:1138-1142). */
	static void RejectionThunk(
		JSContext* Ctx, JSValueConst Promise, JSValueConst Reason, bool bIsHandled, void* Opaque);

	JSRuntime* Runtime = nullptr;

	//~ See the accessors' comment. 0 = JS_INVALID_CLASS_ID (quickjs.h:692) until
	//~ Construct registers them.
	JSClassID ElementClassId = 0;
	JSClassID DocumentClassId = 0;
	JSClassID EventClassId = 0;

	/** See GetLiveBytes(). Atomic: written by the owning thread's hooks, read anywhere. */
	std::atomic<int64> LiveBytes{0};

	/** See GetNumListenerRefs(). Atomic for the same cross-thread-reader reason as the fired-counters. */
	std::atomic<int64> NumListenerRefs{0};

	std::atomic<uint64> NumTimersFired{0};
	std::atomic<uint64> NumRafCallbacksRun{0};
	std::atomic<uint64> NumJobsExecuted{0};
	std::atomic<uint64> NumCollections{0};
	std::atomic<uint64> NumErrors{0};
	std::atomic<uint64> NumWatchdogTrips{0};

	//~ Owning-thread-only bookkeeping; plain members on purpose (see the class
	//~ comment's counter-pattern note -- these have no cross-thread readers).
	int64 GCStepBytes = 0;
	int64 BytesAtLastCollection = 0;
	bool bOomFallbackPending = false;
	double LastCollectionPauseMs = 0.0;
	uint64 LastCollectionHeapBytes = 0;

	//~ Watchdog state, written by FVaCuusJsEntryGuard on the owning thread and
	//~ read by InterruptThunk ON THE SAME THREAD (the handler runs inside JS
	//~ execution, i.e. inside the guarded entry). Plain members are correct.
	double WatchdogSeconds = 0.0;
	double WatchdogDeadlineSeconds = 0.0;
	int32 WatchdogEntryDepth = 0;
	bool bWatchdogTripped = false;
};

/**
 * RAII for one host -> JS entry boundary (spec 3.3): arms the watchdog deadline
 * on entry, and on exit -- if the interrupt handler tripped inside -- logs the
 * entry's source name and makes the engine's uncatchable "interrupted"
 * InternalError (JS_ThrowInterrupted, quickjs.c:8215-8219) catchable-pending
 * again via JS_ResetUncatchableError (quickjs.h:829-833), so the NEXT entry
 * starts clean and THIS entry's caller can consume the exception normally.
 *
 * USAGE CONTRACT: the guard wraps the JS call ONLY -- close the scope before
 * consuming the exception, because the reset needs the exception still pending:
 *
 *   JSValue Ret;
 *   {
 *       FVaCuusJsEntryGuard Guard(Runtime, Ctx, TEXT("hud_logic.js"));
 *       Ret = JS_Eval(...);
 *   }
 *   if (JS_IsException(Ret)) { Runtime.ReportException(Ctx, ...); }
 *
 * NESTED ENTRIES ARM NOTHING: only the outermost guard sets the deadline, so a
 * C++ callback that re-enters JS (event dispatch inside the pump, Task 3 on)
 * cannot extend a script's budget by chaining entries -- the deadline the
 * outermost boundary armed is the one that holds.
 */
struct FVaCuusJsEntryGuard
{
	FVaCuusJsEntryGuard(FVaCuusJsRuntime& InRuntime, JSContext* InCtx, const TCHAR* InSourceName);
	~FVaCuusJsEntryGuard();

	FVaCuusJsEntryGuard(const FVaCuusJsEntryGuard&) = delete;
	FVaCuusJsEntryGuard& operator=(const FVaCuusJsEntryGuard&) = delete;

private:
	FVaCuusJsRuntime& Runtime;
	JSContext* Ctx;
	const TCHAR* SourceName;
};
