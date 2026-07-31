// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// For FVaCuusJsListenerKey: a TMap member needs its key COMPLETE at class
// definition (the key's hash/traits instantiate with the map). No cycle: the
// listener header only forward-declares this context.
#include "VaCuusJsEventListener.h"
#include "VaCuusJsRuntime.h"

namespace Rml
{
class Element;
class ElementDocument;
class Event;
}
struct FVaCuusJsElementHandle;

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
 *  - the DOM facade's per-view state (M4 Task 4, VaCuusJsDom.cpp): the element
 *    identity cache (WrapperCache), the per-context wrapper prototypes
 *    (JS_SetClassProto keeps them on the JSContext), and the compiled style
 *    proxy factory. The `document` global becomes a real Document wrapper via
 *    BindDocument -- called by tests through the host's test-only entry now,
 *    by OnDocumentReady in M4 Task 6.
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

	/** For the listener machinery (entry guards, counters, error sink); module-private like everything here. */
	FVaCuusJsRuntime& GetRuntime() const { return Runtime; }

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

	//~ ---- The DOM facade (M4 Task 4; implementation in VaCuusJsDom.cpp) ----

	/**
	 * Points the `document` global at Document (wrapped through the identity
	 * cache), or back at null. Production wiring is M4 Task 6's OnDocumentReady;
	 * until then the caller is FVaCuusJsScriptHost::BindDocumentForTest. UI
	 * thread in production -- this wraps, and wrapping allocates an ObserverPtr
	 * block from RmlUi's un-mutexed global pool (ObserverPtr.cpp:6-24).
	 */
	void BindDocument(Rml::ElementDocument* Document);

	/**
	 * The identity cache's front door: one wrapper per live element, `===`
	 * across lookups. Returns an OWNED ref (caller frees or returns to JS);
	 * the cache itself keeps a BORROWED one (VaCuusJsDomHandle.h has the
	 * retention story). Null in, JS_NULL out. Populated lazily on first wrap,
	 * NEVER from OnElementCreate -- clones bypass Factory::InstanceElement and
	 * would miss a hook-populated map (Element.cpp:214-225).
	 */
	JSValue WrapElement(Rml::Element* Element);

	/**
	 * The process-global OnElementDestroy hook's per-view probe (spec 2(g)),
	 * called by FVaCuusJsScriptHost for EVERY element the process destroys:
	 * erases the dying element's cache entry, if this view ever wrapped it.
	 * Misses are one TMap lookup -- free. Runs inside ~Element (Element.cpp:99),
	 * so no JS is executing on this thread right now, though the call may sit
	 * BELOW a facade thunk that triggered the destruction (innerRML, remove).
	 */
	void OnRmlElementDestroyed(Rml::Element* Element);

	/** Finalizer-side cache erase; called only while bInCache was still set. */
	void RemoveCacheEntry(Rml::Element* RawKey);

	/**
	 * TEST OBSERVABILITY (the cache-hygiene invariant needs an observable, the
	 * M2 lesson): live cache entries. Not a counter -- the exact current size,
	 * so a test can assert it returns to a recorded baseline.
	 */
	int32 GetWrapperCacheSize() const { return WrapperCache.Num(); }

	//~ ---- Events (M4 Task 5; implementation in VaCuusJsEvents.cpp) ----

	/**
	 * The event object, fresh per listener invocation (spec 3.9 -- cheap, no
	 * identity requirement): an event-class instance whose opaque holds the raw
	 * Rml::Event* for the stop methods, plus plain data properties -- type,
	 * target/currentTarget (through the identity cache, so `ev.target ===
	 * getElementById(...)` is free), eventPhase, `params`, and the DOM-ish
	 * parameter aliases. The CALLER owns invalidation: null the handle's Event
	 * pointer before the Rml::Event leaves scope (ProcessEvent does both).
	 */
	JSValue BuildEventObject(Rml::Event& Event);

	//~ The listener registry's containers are private; the listener class and
	//~ the thunks maintain them through this narrow surface.
	void RegisterListener(const FVaCuusJsListenerKey& Key, FVaCuusJsEventListener* Listener);
	void UnregisterListener(const FVaCuusJsListenerKey& Key);
	void AdoptAttributeListener(FVaCuusJsEventListener* Listener);
	void DropAttributeListener(FVaCuusJsEventListener* Listener);

	/** TEST OBSERVABILITY: live registrations owned by this context (both kinds), an exact gauge like the cache size. */
	int32 GetLiveListenerCount() const { return ListenerRegistry.Num() + AttributeListeners.Num(); }

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

	//~ ---- DOM facade internals (VaCuusJsDom.cpp) ----

	/** Builds the Element and Document prototypes and hands them to JS_SetClassProto. */
	void InstallDomPrototypes();

	/**
	 * The wrapper behind This if it is one of ours, else null. Class-id checked
	 * through JS_GetOpaque (quickjs.h:1045, null on mismatch) for BOTH classes --
	 * a script may .call() a facade method on any object, and a foreign `this`
	 * must read as a dead handle, not as UB.
	 */
	FVaCuusJsElementHandle* GetHandle(JSValueConst Value) const;

	/** GetHandle + the dead-check: the live element, or null. EVERY method opens with this. */
	Rml::Element* GetLiveElement(JSValueConst Value) const;

	//~ Document prototype (createElement / getElementById / body).
	static JSValue DocCreateElementThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv);
	static JSValue DocGetElementByIdThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv);
	static JSValue DocBodyGetterThunk(JSContext* Ctx, JSValueConst This);

	//~ Element prototype: tree surgery.
	static JSValue InsertThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv, int Magic);
	static JSValue RemoveChildThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv);
	static JSValue RemoveThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv);

	//~ Element prototype: queries and attributes (magic-dispatched families).
	static JSValue QueryThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv, int Magic);
	static JSValue AttributeThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv, int Magic);

	//~ Element prototype: properties.
	static JSValue StringGetterThunk(JSContext* Ctx, JSValueConst This, int Magic);
	static JSValue StringSetterThunk(JSContext* Ctx, JSValueConst This, JSValueConst Value, int Magic);
	static JSValue ParentNodeGetterThunk(JSContext* Ctx, JSValueConst This);
	static JSValue ChildrenGetterThunk(JSContext* Ctx, JSValueConst This);
	static JSValue ClassListGetterThunk(JSContext* Ctx, JSValueConst This);
	static JSValue StyleGetterThunk(JSContext* Ctx, JSValueConst This);

	//~ Bound helpers (JSCFunctionData, quickjs.h:453): FuncData[0] is the
	//~ element wrapper the classList object / style proxy was minted from.
	static JSValue ClassListOpThunk(
		JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv, int Magic, JSValueConst* FuncData);
	static JSValue StyleOpThunk(
		JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv, int Magic, JSValueConst* FuncData);

	//~ ---- Event internals (VaCuusJsEvents.cpp) ----

	/** The event-class prototype (stop methods); called by InstallDomPrototypes. */
	void InstallEventPrototype();

	//~ Element prototype: the listener surface.
	static JSValue AddEventListenerThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv);
	static JSValue RemoveEventListenerThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv);
	static JSValue DispatchEventThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv);

	//~ Event prototype (stopPropagation / stopImmediatePropagation / preventDefault).
	static JSValue EventOpThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv, int Magic);

	FVaCuusJsRuntime& Runtime;
	JSContext* Ctx = nullptr;
	const uint32 ViewId;

	/**
	 * THE IDENTITY CACHE (spec 3.9): raw Element* -> wrapper JSValue, the value
	 * BORROWED -- deliberately no dup, so the cache pins nothing and a wrapper
	 * lives exactly as long as JS references it. Identity without retention:
	 * lookups dup on the way out, and the entry leaves by whichever dies first
	 * -- the element (OnRmlElementDestroyed, by raw pointer) or the wrapper
	 * (the finalizer, via the bInCache handshake VaCuusJsDomHandle.h explains).
	 * The destructor neuters every surviving entry's back-pointer BEFORE
	 * JS_FreeContext, whose finalizer sweep must not reach into a dying map.
	 */
	TMap<Rml::Element*, JSValue> WrapperCache;

	/**
	 * The compiled style-proxy factory (a JS function value, VaCuusJsDom.cpp) --
	 * a member, NOT a global: a script that clobbers globals cannot break the
	 * style getter. JS_UNDEFINED until InstallDomPrototypes compiles it; freed
	 * in the destructor before JS_FreeContext.
	 */
	JSValue StyleFactory = JS_UNDEFINED;

	/**
	 * THE LISTENER REGISTRY (spec 2(g)): every live addEventListener
	 * registration, keyed for removeEventListener's exact-match semantics
	 * (FVaCuusJsListenerKey). Values are raw pointers, NOT owned here in the
	 * unique-ptr sense -- the listener self-deletes in OnDetach on the two
	 * RmlUi-driven death orders and leaves the map on its way out; the ONE path
	 * where this map outlives its entries' JS state is the destructor's neuter
	 * walk (order (3)), which frees each entry's ref, marks the shell, and
	 * empties the map, leaving the allocation for RmlUi's later detach.
	 */
	TMap<FVaCuusJsListenerKey, FVaCuusJsEventListener*> ListenerRegistry;

	/**
	 * on*-attribute listeners that RESOLVED into this context (compiled their
	 * snippet here, hold a ref into this heap). Joined at first fire, not at
	 * instancing -- before resolution they sit in the host's unresolved set,
	 * because no view is known yet (FVaCuusJsEventListener's class comment).
	 * Same neuter treatment as the registry, minus the key.
	 */
	TSet<FVaCuusJsEventListener*> AttributeListeners;

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
