// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// For FVaCuusJsListenerKey: a TMap member needs its key COMPLETE at class
// definition (the key's hash/traits instantiate with the map). No cycle: the
// listener header only forward-declares this context.
#include "VaCuusJsEventListener.h"
#include "VaCuusJsRuntime.h"
#include "VaCuusJsValue.h"

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
	 * Calls the function at FunctionPath (dotted, resolved from globalThis) with Args, and
	 * NEVER BUILDS A LINE OF JAVASCRIPT to do it (M6, VaCuus-asv). Implementation in
	 * VaCuusJsHostApi.cpp, beside the FVaCuusJsValue marshaller it shares with `vacuus.emit`.
	 *
	 * THAT IS THE WHOLE POINT, and it is a security property rather than a convenience. The
	 * idiom this replaces was `ExecuteScript(FString::Printf(TEXT("... vacuus.onFreeze(%s);"),
	 * Arg))`, which is string interpolation into a live interpreter: a game-supplied argument
	 * containing a quote does not break the call, it CONTINUES it. Here the arguments never
	 * touch a parser -- each becomes a JSValue through FromHostValue and is handed to JS_Call
	 * as data.
	 *
	 * `this` is the OWNER of the last path segment, so `vacuus.onFreeze` sees `this === vacuus`
	 * exactly as `vacuus.onFreeze(x)` written in a script would. A single-segment path gets
	 * globalThis.
	 *
	 * A MISSING FUNCTION IS NOT AN ERROR HERE, because the guard it replaces was not one
	 * either: the old idiom's `typeof ... === 'function'` test existed precisely so a document
	 * that never registered the callback (or failed to load) would not turn a console command
	 * into a JS exception. Same contract, one Warning naming the path, no throw.
	 */
	void CallFunction(const FString& FunctionPath, TArrayView<const FVaCuusJsValue> Args);

	/**
	 * The module ENTRY point (M4 Task 7, spec 3.7; implementation in
	 * VaCuusJsModules.cpp): compiles Source as an ES module named ModuleName
	 * (JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY, quickjs.h:434, :440-444
	 * -- static imports resolve INSIDE this compile, quickjs.c:37395-37404, each
	 * through the loader thunk below), stamps import.meta.url = ModuleName, then
	 * evaluates (JS_EvalFunction, quickjs.h:1244 -> quickjs.c:37274-37287).
	 *
	 * Returns the module promise, OWNED by the caller -- module eval always
	 * returns js_dup(m->promise) (quickjs.c:31553-31554, :31589); a module's
	 * RUNTIME throw is NOT an eval exception, it rejects that promise
	 * (quickjs.c:31571-31575). Compile/resolve failures ARE eval exceptions
	 * (quickjs.c:37411-37416): reported to the sink here, JS_UNDEFINED returned.
	 * The caller (FVaCuusJsScriptHost::EvalModule) owns the drain-then-inspect
	 * protocol -- the promise-state reading is meaningless before a job drain,
	 * and the bounded drain machinery lives on the host (spec 3.5).
	 */
	JSValue EvalModuleToPromise(const FString& Source, const FString& ModuleName);

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
	 * cache), or back at null. Production callers (M4 Task 6): OnDocumentReady
	 * for a document with scripts, EnsureViewContext for a context materialized
	 * after its view's document was already current; tests keep
	 * FVaCuusJsScriptHost::BindDocumentForTest. UI thread in production -- this
	 * wraps, and wrapping allocates an ObserverPtr block from RmlUi's un-mutexed
	 * global pool (ObserverPtr.cpp:6-24).
	 */
	void BindDocument(Rml::ElementDocument* Document);

	/**
	 * The Tier 1 unload surface (M4 Task 6): invokes `vacuus.onUnload` if a
	 * script assigned a function to it, under the entry guard, exceptions to the
	 * sink, counted on the runtime (GetNumUnloadCallbacksRun). Called by the
	 * script host at Close() time -- document replace, explicit close, view
	 * removal, and both shutdown paths -- always BEFORE the tree's deferred free
	 * and BEFORE this context dies, so the callback still sees its DOM.
	 *
	 * DELIBERATELY NOT A DOM EVENT: RmlUi dispatches its own `unload` into the
	 * closing tree inside Context::UnloadDocument (Context.cpp:339-341), so a
	 * JS-visible DOM unload would either duplicate that dispatch or require
	 * hooking into it mid-teardown -- and on the view-removal path there is no
	 * Close() at all, only tree destruction inside the host's Shutdown(). One
	 * callback slot on the `vacuus` host object (InstallGlobals), invoked by the
	 * host at a moment IT controls, is the smallest surface that fires
	 * identically on every death path; Task 9 grows the same object
	 * (vacuus.model, vacuus.log).
	 */
	void DispatchUnload();

	/**
	 * Invokes `vacuus.onLanguageChanged(tag)` if a script assigned a function to it — the same
	 * read-at-fire, guard-wrapped, exception-to-the-sink shape as DispatchUnload, counted on the
	 * runtime (GetNumLanguageCallbacksRun).
	 *
	 * WHAT IT IS FOR: markup written `{{ t.key }}` re-translates by itself, but a UI that builds
	 * its own strings in JS — a killfeed line, anything needing `translate(key, params)` — has
	 * no other way to learn that the language moved. Tag is the pusher's label, uninterpreted.
	 *
	 * NOT A DOM EVENT, for DispatchUnload's reason restated: the table is process-wide and has
	 * no element to dispatch against, so a callback slot on the host object is the smallest
	 * surface that fires identically in every view.
	 */
	void DispatchLanguageChanged(const FString& Tag);

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

	/**
	 * The document BindDocument last bound, or null -- the `vacuus.view` getter's
	 * route to the context's dimensions (Document -> GetContext() -> GetDimensions()).
	 * A MEMBER rather than a read of the `document` global, so a script that clobbers
	 * that global cannot make the view report someone else's size.
	 */
	Rml::ElementDocument* GetCurrentDocument() const { return CurrentDocument; }

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

	/**
	 * THE ONE DOOR TO THE CONTEXT OPAQUE -- every thunk reads `this` through
	 * here, never through a bare JS_GetContextOpaque. Null is a REAL answer: the
	 * destructor nulls the opaque before JS_FreeContext, and the JSContext
	 * allocation can outlive that call -- a pending job's argv holds dup'd
	 * values (JS_EnqueueJob, quickjs.c:2146-2154) whose function realms keep the
	 * context refcount above zero past JS_FreeContext's decrement
	 * (quickjs.c:2681-2682) -- so a removed view's pinned job can still call
	 * setTimeout (or touch a stashed wrapper) against this very JSContext from a
	 * surviving view's drain segment. Each caller answers null with its own
	 * dead shape (undefined/null/false/empty), the dead-handle house rule; a
	 * check(non-null) here would pass on the DANGLING pointer, not catch it.
	 */
	static FVaCuusJsViewContext* GetSelfOrNull(JSContext* InCtx)
	{
		return static_cast<FVaCuusJsViewContext*>(JS_GetContextOpaque(InCtx));
	}

	//~ The global-function thunks. `this` rides JS_GetContextOpaque
	//~ (quickjs.h:524-525), set to the owning FVaCuusJsViewContext before any
	//~ script can run -- and read back ONLY through GetSelfOrNull (see its
	//~ comment for why null is a live case, not a bug). Signatures per
	//~ JSCFunctionType (quickjs.h:1272-1286).
	static JSValue ConsoleThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv, int Magic);
	static JSValue SetTimerThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv, int Magic);
	static JSValue ClearTimerThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv);
	static JSValue RequestRafThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv);
	static JSValue CancelRafThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv);

	//~ ---- The vacuus.* host API (M4 Task 9, spec 3.11; implementation in
	//~ VaCuusJsHostApi.cpp). Installed onto the `vacuus` object by InstallGlobals;
	//~ everything below routes through GetSelfOrNull like every thunk, and reaches
	//~ core through VaCuusGameBridge -- reads from the UI shadow, writes NEVER
	//~ (vacuus.model has no set; writes are the router's, spec 3.10).

	/** Populates Vacuus with emit/model/stats and the `view` getter. Called by InstallGlobals; Vacuus is borrowed. */
	void InstallHostApi(JSValue Vacuus);

	/** vacuus.emit(name, payload): the payload's own enumerable string properties, flat, to the game queue. */
	static JSValue EmitThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv);

	/** vacuus.model(name): mints {name, get} with the name bound into get's FuncData. */
	static JSValue ModelThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv);

	/** The bound get(path); FuncData[0] is the model-name string. */
	static JSValue ModelGetThunk(
		JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv, int Magic, JSValueConst* FuncData);

	/** The vacuus.view getter: {id, width, height} computed at read time. */
	static JSValue ViewGetterThunk(JSContext* Ctx, JSValueConst This);

	/** vacuus.stats(): {updateMs, renderMs, fps} from the always-on last-sample store (FVaCuusPerfLog). */
	static JSValue StatsThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv);

	/**
	 * vacuus.translate(key, params?) (M5 Task 8, spec §2(l)): synchronous lookup in
	 * the installed FVaCuusTranslationRegistry snapshot — identity on any miss —
	 * then `{name}` placeholder substitution from params' own enumerable string
	 * properties (bool/number/string values, the emit conversion contract). The
	 * no-table case logs ONE latched Verbose per context (bTranslateNoTableWarned).
	 */
	static JSValue TranslateThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv);

	//~ ---- ES modules (M4 Task 7; implementation in VaCuusJsModules.cpp) ----

	/**
	 * The JSModuleNormalizeFunc (quickjs.h:1156-1158; "return the module
	 * specifier (allocated with js_malloc()) or NULL if exception",
	 * :1154-1155). Specifiers arrive AS WRITTEN IN JS SOURCE -- no head-handler
	 * pipe-encoding ever reaches here; that transform exists only for <script
	 * src> attributes (XMLNodeHandlerHead.cpp:14-19), and module entries have it
	 * undone by RunCapturedScripts before any module name is minted. Relative
	 * ('./x', '../y') resolves against the IMPORTING module's directory (BaseName
	 * is the importer's own canonical name -- the engine passes m->module_name,
	 * i.e. whatever the importer was COMPILED as); bare and vfs://-prefixed
	 * specifiers are root-relative. Result: "vfs://<canonical>", the per-context
	 * cache key (VaCuusJsModules.h). Routed through GetSelfOrNull like every
	 * thunk; a dead context refuses the resolution (exception + NULL).
	 */
	static char* ModuleNormalizeThunk(JSContext* Ctx, const char* BaseName, const char* Name, void* Opaque);

	/**
	 * The JSModuleLoaderFunc (quickjs.h:1164-1165): strips vfs://, resolves
	 * through the ordered DevUI roots (ResolveExistingDocument), reads via the
	 * pak-transparent IPlatformFile path (VaCuusJsScriptSource), compiles with
	 * COMPILE_ONLY and returns the JSModuleDef*. A read miss is our OWN Error
	 * naming what was probed, plus a thrown ReferenceError in the engine's own
	 * wording (quickjs.c:30044) that surfaces at the importer -- both
	 * diagnostics, spec 3.7. NULL on any failure, exception pending.
	 */
	static JSModuleDef* ModuleLoaderThunk(JSContext* Ctx, const char* ModuleName, void* Opaque);

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

	//~ Document prototype (createElement / createElementNS / createTextNode /
	//~ getElementById / body). createElement and createElementNS share one
	//~ magic-dispatched thunk: the NS spelling is the SAME operation with the tag
	//~ one argument later -- preact 10.29.7 creates every element through it
	//~ (src/diff/index.js:465-469), observed in E-P1 as the mount-blocking gap.
	static JSValue DocCreateElementThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv, int Magic);
	static JSValue DocCreateTextNodeThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv);
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
	static JSValue ChildListGetterThunk(JSContext* Ctx, JSValueConst This, int Magic);
	static JSValue ClassListGetterThunk(JSContext* Ctx, JSValueConst This);
	static JSValue StyleGetterThunk(JSContext* Ctx, JSValueConst This);

	//~ Element prototype: node discrimination and traversal (M5 Task 1, the
	//~ preact gaps G2/G4): one magic family for the four sibling/child steps,
	//~ plus nodeType, the attributes snapshot, and the text-node value pair.
	static JSValue NodeStepGetterThunk(JSContext* Ctx, JSValueConst This, int Magic);
	static JSValue NodeTypeGetterThunk(JSContext* Ctx, JSValueConst This);
	static JSValue AttributesGetterThunk(JSContext* Ctx, JSValueConst This);
	static JSValue TextDataGetterThunk(JSContext* Ctx, JSValueConst This);
	static JSValue TextDataSetterThunk(JSContext* Ctx, JSValueConst This, JSValueConst Value);

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

	/** See GetCurrentDocument(). Written only by BindDocument; the pointer's liveness is BindDocument's caller contract. */
	Rml::ElementDocument* CurrentDocument = nullptr;

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

	/**
	 * The `vacuus.translate` no-table refusal latch (spec §2(l)'s "one latched
	 * Verbose line"). PER CONTEXT, not process-wide, deliberately: it dies with the
	 * context (so every fresh session/test starts armed), and the line it gates
	 * names this view. Never re-arms once a table exists — TranslateThunk checks
	 * the snapshot first, so the latch is only ever consulted before the first
	 * table of the process arrives.
	 */
	bool bTranslateNoTableWarned = false;

	//~ Test-only knobs, forwarded from FVaCuusJsScriptHost::FParams (see there).
	const bool bTestRelaxTimerCutoff;
	const int32 TestTimerPassHardStop;
};
