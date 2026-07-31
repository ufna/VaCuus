# VaCuus M4 — JS Tier 1: QuickJS core surface

**Status:** design v1, for adversarial review before planning.

**Scope:** quickjs-ng vendored at a recorded tag; a `VaCuusJs` runtime module behind a core-declared
script-host seam; the DOM facade over `Rml::Element`; timers/rAF/microtasks/console; ES modules via
the VFS; `<script>` and `ExecuteScript`; the error overlay; the two-way write router M3 §4/I3
promised to this milestone; and the demo-HUD logic ported from the C++ sim to JS with parity and
budgets including GC pauses (architecture spec §7, §14/M4).

**Ground truth:** `docs/research/m4-api-notes/{hud-demo-patterns,quickjs-ng-0151,plugin-integration,
rmlui-scripting}.md` (2026-07-31, written against the code on this disk), plus the architecture
spec and the M3 notes. Claims not supported by read source are **[unverified]** and carry the
experiment that settles them.

---

## 1. Goal

```html
<head><script src="hud_logic.js"/></head>
```
```js
// hud_logic.js — runs on the UI thread, against this view's document
const feed = document.getElementById('killfeed');
vacuus.onModelEvent = null; // gameplay flows in through data binding, unchanged
setInterval(() => {
  const row = document.createElement('div');
  row.classList.add('kill');
  row.innerRML = `<span>${pick(names)}</span> » <span>${pick(names)}</span>`;
  feed.appendChild(row);
  if (feed.children.length > 6) feed.children[0].remove();
}, 1500);
requestAnimationFrame(function tick(tMs) { /* bars, damage numbers */ requestAnimationFrame(tick); });
```

The C++ demo driver's logic, expressed in JS, against the same document — with the game thread
still only enqueuing input and data.

---

## 2. The findings that decide the architecture

**(a) The library is four C files and nothing else.** quickjs-ng v0.15.1's core target compiles
exactly `quickjs.c, libregexp.c, libunicode.c, dtoa.c` (CMakeLists.txt:266-271, :301); `cutils` is
header-only (152 static-inline functions, no .c); the bytecode headers are checked in
(builtin-*.h:1); `quickjs-libc` is a separate, default-OFF target (CMakeLists.txt:251, :335-339) —
**and the core has no setTimeout and no console** (both live in libc, quickjs-libc.c:4443-4444,
:4613). Excluding libc is not a restriction we impose; it is the library's own embedder shape, and
it means every global in §5 is ours to implement — which the spec always assumed. C11
(CMakeLists.txt:9-11); UBT compiles `.c` natively with `ModuleRules.CStandard`
(UEBuildModuleCPP.cs:3072, ClangToolChain.cs:594-599), and the engine's own vendored-C precedent is
SQLiteCore's relay `.c` (SQLiteEmbedded.c:17-24). Recorded pin: **tag `v0.15.1` =
`fd0a0210b7be00957751871e7e01b8291268fc29`**; `QJS_VERSION_*` at quickjs.h:1410-1413.

**(b) The frame-controlled GC design holds — with one sharp caveat.** The only implicit GC trigger
in the engine is object allocation (`js_trigger_gc`'s sole call site is `JS_NewObjectFromShape`,
quickjs.c:5748), and `JS_SetGCThreshold(rt, -1)` disables it ("use -1 to disable automatic GC",
quickjs.c:2110). That leaves `JS_RunGC` at our frame point as the only collector — exactly the
architecture spec's design. The caveat: allocation failure at the memory cap does **not** collect
and retry — `js_malloc_rt` returns NULL immediately (quickjs.c:1645-1647) and the context throws
InternalError "out of memory" (quickjs.c:8130-8135). So the pump owns an on-OOM fallback: when an
eval/callback returns the OOM error, run one `JS_RunGC` and log; the *next* allocation gets the
freed space. **[unverified]** end-to-end — experiment E5 (m4-api-notes/quickjs-ng-0151.md).

**(c) Thread legality is a stack anchor, not an identity check.** quickjs core has zero
thread-identity code (grep verified); the single thread-sensitive datum is `stack_top`, captured at
`JS_NewRuntime2` (quickjs.c:2019) and consulted by the native-stack overflow check
(quickjs.c:1952-1957). "Created on thread A, used on thread B" is legal with `JS_UpdateStackTop`
on B (quickjs.h:501-503) — which is precisely the inline/commandlet mode's shape. And the UI
thread's stack is a **written-down M4 constraint**: 512 KB chosen with the comment "QuickJS lands
on this thread in M4, so the platform default is not obviously enough"
(VaCuusUIThread.cpp:42-50). §4 raises it and budgets the JS stack under it.

**(d) The seam does not exist yet, but its template does.** `IVaCuusScriptHost` appears nowhere in
code (grep; the name lives only in the architecture spec). The in-tree template is
`IVaCuusDocumentHost` — core declares the contract, the dependent module implements it, and the
comment calls the seam deliberate (VaCuusDocumentHost.h:22-27) — plus the one true
register-before-boot API, `FVaCuusEngine::SetRenderInterface`, refused after `Initialize()`
(VaCuusEngine.cpp:230-242). M4 declares `IVaCuusScriptHost` in core, implements it in `VaCuusJs`,
and the frame loop skips the pump when no host is registered — the spec's JS-off configuration
for free (architecture spec §3:79-82).

**(e) One runtime per process, one `JSContext` per view — a deliberate amendment to the
architecture spec's "per subsystem".** The UI thread has no per-subsystem structure at all: hosts
and models are keyed by process-unique `ViewId` (VaCuusUIThread.h:315, :341), commands carry only
`ViewId` (VaCuusUIQueues.h:116), and multi-PIE is "N subsystems, 1 thread, N views"
(VaCuusUIThread.h:26-33). A per-subsystem runtime would need new command plumbing and a subsystem
teardown protocol that today does not exist (plugin-integration.md §2), and Tier 1 has nothing that
needs subsystem-level sharing: a script binds a *view's* document. So: **one `JSRuntime` per
process** (created lazily on the UI thread at the first script-bearing view, destroyed in the
Exit split of §7; the 16 MB cap is per-runtime and therefore process-wide — stricter than the
spec's wording, not weaker), and **one `JSContext` per view** — created on demand, destroyed with
the view, and **torn down and rebuilt on document replace**, which gives live reload
browser-refresh semantics: scripts re-run against the new document; timers, listeners and module
instances die with the old context. The M3 rule "the model survives reload, do nothing" is
untouched — models are not JS state.

**(f) RmlUi's script surface is head-only `<script>`, a virtual pair, and a plugin bus.** There is
no script node handler; `XMLNodeHandlerHead` collects inline text and `src` into
`DocumentHeader::scripts` and `ProcessHeader` calls the **virtual**
`ElementDocument::LoadInlineScript/LoadExternalScript` — mid-parse, before `<body>` exists and
before the document joins the context (XMLNodeHandlerHead.cpp:84-91, :126-129, :98-110;
ElementDocument.cpp:217-228, base impls empty :461-463). A `<script>` in `<body>` reaches nothing.
So: a facade `ElementDocument` subclass (via a document instancer) **captures** scripts at
`LoadInlineScript` time and **defers execution** to `Plugin::OnDocumentLoad` — which fires after
instancing+append and before the document's `load` event (Context.cpp:299-300) — in document
order, externals resolved through the VFS. The head-only limitation is documented, not fought.

**(g) The facade's lifetime story is already sound in RmlUi.** `Element` derives
`EnableObserverPtr` (Element.h:47); a dead handle reads null but `operator->` does not check
(ObserverPtr.h:77-86) — the facade tests every access, the demo's "dead handles return null, never
throw" philosophy (hud-demo Js.cpp:1-3). Detached elements hold no dangling document/context
pointer (Element.cpp:1492, :2124-2143) — create-then-append is sound. Listener detach fires in
**two different orders** by death path: destroy→detach on direct destruction (Element.cpp:99,
:112), detach-then-destroy on document unload (Context.cpp:1565-1567) — the JS ref release must
tolerate both (rmlui-scripting.md §3, experiment E2). `Element::Clone` bypasses
`OnElementCreate` (Element.cpp:214-225), so the identity cache populates **lazily on wrap**, keyed
on the raw `Element*`, erased in `OnElementDestroy` (during which ObserverPtrs still read alive —
key on the pointer, not on observer death). And every ObserverPtr operation mutates an un-mutexed
process-global pool (ObserverPtr.cpp:6-24): **wrapper finalization must run on the UI thread**,
which the controlled-frame-point GC already guarantees — asserted, not assumed.

---

## 3. Architecture

```
VaCuusJs (new Runtime module)
├─ ThirdParty vendored: Source/ThirdParty/quickjs-ng (4 .c + headers, VENDORED_TAG.txt)
├─ FVaCuusJsRuntime      one per process, UI-thread-owned: JSRuntime, malloc hooks over FMemory,
│                        16MB cap, GC threshold -1, interrupt watchdog, rejection tracker
├─ FVaCuusJsViewContext  one per view: JSContext, globals (console, timers, rAF, vacuus.*),
│                        module loader over the VFS, document binding, error overlay state
├─ FVaCuusJsElementCache identity map Element* -> JSValue wrapper (per view context)
├─ FVaCuusJsScriptHost   implements IVaCuusScriptHost (declared in VaCuus core)
└─ FVaCuusJsDocument     ElementDocument subclass capturing <script> via LoadInlineScript
```

### 3.1 The seam: `IVaCuusScriptHost`

Declared in core (`VaCuus/Public/`), following `IVaCuusDocumentHost`'s pattern verbatim. Surface
(UI thread unless noted):

- `OnViewAdded(ViewId) / OnViewRemoved(ViewId)` — context lifecycle.
- `OnDocumentLoaded(ViewId, Rml::ElementDocument*)` / `OnDocumentClosing(ViewId)` — script run
  and context recycle points (§3.4).
- `PumpFrame(double NowSeconds)` — rAF → timers → jobs, §3.5.
- `CollectGarbage(reason)` — the controlled point, §3.6.
- `ExecuteScript(ViewId, Source, SourceName)` — the command's landing.
- `Shutdown()` — runtime death, called at the §7 split point.

Registered via `FVaCuusModule::SetScriptHostFactory(...)` from `FVaCuusJsModule::StartupModule`,
refused after the UI thread boots (the `SetRenderInterface` shape, VaCuusEngine.cpp:230-242). No
host registered → every call site skips — the module-absent configuration. Runtime kill switch:
`vacuus.Js.Enable` cvar (default 1), checked once at first-runtime-creation (the `vacuus.IdleGate`
read pattern, VaCuusRecordingRenderInterface.cpp:42-52); flipping it after the runtime exists is
documented as a no-op.

### 3.2 Vendoring and the module build

`Source/ThirdParty/quickjs-ng/` carries the four .c files, all project headers, the checked-in
`builtin-*.h`, `LICENSE`, and `VENDORED_TAG.txt` (`v0.15.1` + commit). `VaCuusJs.Build.cs` clones
the VaCuusRml preamble (NoPCHs, no unity, warnings relaxed, no exceptions) plus
`CStandard = CStandardVersion.C11`; compilation via relay `.c` files in `Private/Gen/` (the
VaCuusRml relay pattern; SQLiteCore proves the `.c` relay compiles). Defines: `_GNU_SOURCE`
(CMakeLists.txt:278), Win64 adds `WIN32_LEAN_AND_MEAN` + `_WIN32_WINNT=0x0601` (:279-282), and
**neither** shared-lib macro, so `JS_EXTERN` degrades to nothing (quickjs.h:62-76) — the C symbols
stay module-internal; nothing crosses the module boundary as C (the host interface is the
boundary).

### 3.3 The runtime: memory, stack, watchdog

- **Malloc hooks over `FMemory`**: `JS_NewRuntime2` with the five-hook `JSMallocFunctions`
  (quickjs.h:451-457) forwarding to `FMemory::Malloc/Free/Realloc` (+`QuantizeSize` for
  usable-size). The runtime's own accounting enforces the cap (`JS_SetMemoryLimit(16MB)`,
  overridable by cvar `vacuus.Js.MemoryLimitMB`); UE's allocator sees every byte for LLM/stats.
  At the cap: InternalError, never abort (quickjs.c:1645-1647, :8130-8135) — plus the §2(b) on-OOM
  GC fallback.
- **Stack**: `GVaCuusUIThreadStackSize` rises 512 KB → **2 MB** (the constant's own comment
  anticipated this, VaCuusUIThread.cpp:42-50), and `JS_SetMaxStackSize(256 KB)` keeps the
  interpreter's native recursion budget far under it, leaving RmlUi's own recursion room below.
  quickjs's default JS stack is 1 MB (quickjs.h:423-425) — larger than the *old thread stack*,
  which is exactly the class of quiet catastrophe the raise removes. Inline mode calls
  `JS_UpdateStackTop` at each thread handoff (§2(c)). **[unverified]** headroom — the
  stack-headroom experiment (plugin-integration.md §1.2): deep JS recursion must die as a JS
  RangeError, never the guard page.
- **Watchdog**: `JS_SetInterruptHandler` (quickjs.h:1139-1141; polled every 10k interpreter ops,
  quickjs.c:479, :8234-8241) against a per-pump deadline (`vacuus.Js.WatchdogMs`, default 250 dev /
  0=off shipping). A trip is an **uncatchable** InternalError (quickjs.c:8215-8218) — logged with
  the script name; the pump resets via `JS_ResetUncatchableError` and continues the frame. A hung
  script costs one budget overrun, never a hung UI thread.

### 3.4 Contexts, documents, scripts

- A view's `JSContext` is created on demand (first script, first ExecuteScript) via
  `JS_NewContext` (intrinsics list at quickjs.c:2533-2557; BigInt's absence from it is settled by
  experiment E3 before the context recipe freezes).
- **Document replace recycles the context** (§2(e)): `OnDocumentClosing` frees it (timers,
  listeners, modules, wrappers die — every JS-held `ElementPtr` finalizes while RmlUi is alive),
  `OnDocumentLoaded` builds a fresh one and runs the new document's scripts. Unload-event JS runs
  before the close, satisfied by dispatch-at-`Close()`-time (plugin-integration.md §4).
- `<script>` capture per §2(f): `FVaCuusJsDocument` (document instancer registered by the host)
  stores inline text + src paths with source lines; execution at `OnDocumentLoad`, document order,
  externals read through the VFS resolution path (§3.7). Scripts are classic scripts
  (`JS_EVAL_TYPE_GLOBAL`); modules are opt-in via `<script type="module">` → module eval (§3.7).
- `ExecuteScript`: new command kind + `EnqueueExecuteScript` + `UVaCuusView::ExecuteScript`
  (BlueprintCallable), source in the command `Payload` (the LoadDocumentMemory precedent,
  VaCuusUIQueues.h:50-51), unknown-view drop at **Error** like BindModel — its loss is equally
  invisible downstream (VaCuusUIThread.cpp:999-1020).

### 3.5 The pump

A new ungated RunFrame phase between DataApply and the record loop (the marker argument at
VaCuusUIThread.cpp:873-877 applies verbatim; ungated because sizeless-but-alive UMG views must
pump — the DataApply precedent, VaCuusUIThread.cpp:885-897). Order within the pump, from the
demo's proven contract (hud-demo Js.h:23-25): **rAF → timers → `JS_ExecutePendingJob` to
exhaustion** (one job per call, loop while >0 — quickjs.c:2173-2202).

- rAF: swap-out list so callbacks registered during the run land next frame (the demo's
  re-entrancy rule, Js.cpp:531-534); timestamp = `GetSystemInterface()->GetElapsedTime()` sampled
  once at pump top, in ms — the same clock RmlUi's animations advance on (Clock.cpp:7-14,
  Element.cpp:2838-2849), so JS motion and RCSS motion share time. `cancelAnimationFrame` exists
  (the demo lacked it — gap list).
- Timers: due = `deadline <= now` with **frame-start cutoff** — a 0 ms timer registered during the
  pump runs next frame, closing the demo's documented livelock (`(function f(){setTimeout(f,0)})()`
  hangs the demo's pump — hud-demo-patterns.md §4 hazard). Interval re-arm from fire time.
- Microtasks: `queueMicrotask` → `JS_EnqueueJob` (quickjs.h:1199-1201); promise jobs ride the same
  drain.
- Per-callback exception handling: `JS_GetException` (returns-and-clears, quickjs.h:818), read
  `stack`, route to §3.8. One callback's throw never skips its siblings.

Scopes `JsPump` and `JsGC` are declared in `EScope`/stats **before the phase lands** — the
DataApply declare-first playbook (VaCuusStats.h:39-45), with the positional-enum name guard
(VaCuusStats.cpp:48-49).

### 3.6 GC: the controlled point

`JS_SetGCThreshold(rt, -1)` at runtime birth (§2(b)). `CollectGarbage` runs at **end of RunFrame,
after the record loop** — the pause lands inside the frame the counter names but after this
frame's output is published (plugin-integration.md §1.1 candidate (a)). Trigger heuristic: heap
grew ≥ `vacuus.Js.GCStepKB` (default 512) since the last collection, or an OOM fallback fired this
frame. The pause is measured under the `JsGC` scope; heap bytes (via `JS_ComputeMemoryUsage`,
sampled at collection only — it walks the heap) and collections-per-window join the PerfLog
window line (the `AddUIFrame` counter template, VaCuusStats.h:140-149).

### 3.7 Modules and the VFS

`JS_SetModuleLoaderFunc` — the classic normalize/loader split survives in ng (quickjs.h:1173-1175);
attributes tier not needed. Specifiers: relative paths resolve against the importing module's
directory, then the document roots; the `vfs://` prefix is **stripped before resolution** because
`FPaths::IsRelative` would otherwise glue it under a root (plugin-integration.md §3). Files read
through the same `IPlatformFile` path documents use (pak-transparent, VaCuus.Build.cs:55-60).
Module eval returns a promise in ng (quickjs.c:31553-31557); after the pump's job drain, a still-
pending module promise means top-level await on a host event — **logged and refused** (one Error
naming the module; TLA is not part of Tier 1's contract) — grounded by experiment E1. A module
runtime throw rejects the promise instead of raising (quickjs.c:31573-31574) — surfaced through
the rejection tracker, not `JS_Eval`'s return. Module cache lives in the per-view context and dies
with it on reload (§2(e)); live reload's watch extensions gain `.js`/`.mjs`
(VaCuusLiveReload.cpp:20-26 is rml/rcss-only today — script edits currently reload **nothing**).

### 3.8 Errors: log, overlay, rejection tracking

- Every surfaced exception → `UE_LOG(LogVaCuusJS, Error)` with message + `stack` (a real string
  property, quickjs.c:7766) + script/source name. `LogVaCuusJS` follows the `LogVaCuus`
  declare/define pair in VaCuusJs's public header (plugin-integration.md §7).
- **Dev overlay** (non-shipping): a host-owned overlay element appended to the view's document
  root (absolutely positioned, `vacuus.Js.Overlay` cvar default 1 in dev), showing the last N
  errors. Uncaught promise rejections enter it via `JS_SetHostPromiseRejectionTracker` — and are
  **retracted** when the tracker re-fires with `is_handled=true` (a later-attached handler,
  quickjs.c:55160-55170) — the overlay needs the retract path or it lies (quickjs-ng-0151.md §8).
- The demo's exit-code-on-error observable becomes: total JS error count exposed on the host,
  asserted zero in every M4 test that doesn't expect one.

### 3.9 The DOM facade

Hardened from the demo per the gap list (hud-demo-patterns.md §9), spec §7 Tier 1 surface:

- **Identity**: one wrapper per element — cache keyed on raw `Element*`, populated lazily on wrap
  (clones bypass `OnElementCreate`, §2(g)), erased in `OnElementDestroy`. Two `getElementById`
  calls are `===`-equal; the demo's per-call wrapping is the named defect this fixes.
- **Handles**: `ObserverPtr` in an opaque (the demo's `ElementHandle` shape, class-id +
  finalizer); dead handle ⇒ methods return null/false/undefined, never throw.
- **Surface**: `document.createElement` (tag **lowercased by the facade** — uppercase tags
  silently miss RCSS with asserts compiled out, rmlui-scripting.md §2 + E1) / `getElementById` /
  `querySelector(All)` / `closest` (documented deviations: self never matches; `closest` starts at
  the parent — Element.cpp:1544-1546, :1077-1090); `appendChild` / `insertBefore` / `remove` /
  `removeChild` (ownership per `ElementPtr` move semantics; a JS-held detached element owns its
  `ElementPtr` in the wrapper, freed by finalizer or transferred on append); `id`, `tagName`,
  `parentNode`, `children`, `innerRML` get/set (set destroys replaced children synchronously —
  their handles die, listeners detach before the call returns, rmlui-scripting.md §6);
  `getAttribute`/`setAttribute`/`removeAttribute`; **classList** (`add/remove/toggle/contains`)
  over `SetClass`/`IsClassSet`/`GetClassNames` — never the `class` attribute, which `SetClass`
  leaves stale (Element.cpp:258-276, the trap in rmlui-scripting.md §5); **style proxy**
  (string-based: get = `GetProperty(name)?->ToString()` copied at once — the returned pointer is
  invalidated by any following call, Element.h:187-193; set = `SetProperty(name, value)`; remove).
- **Events**: `addEventListener(type, fn, capture?)` / `removeEventListener` over the real capture
  flag (Element.h:471-482); one `JsEventListener` per registration releasing its ref in
  `OnDetach` and tolerating **both** death orders (§2(g)); event object: `type`, `target`,
  `currentTarget`, `phase`, parameters (mouse/key data via `GetParameter`), `stopPropagation` /
  `stopImmediatePropagation`; `preventDefault` **maps to stopPropagation** and the docs say so —
  RmlUi runs default actions only while the event propagates; there is no separate default-action
  veto (EventDispatcher.cpp:173-185). `dispatchEvent` for custom events (auto-registration
  semantics per EventSpecification.cpp:125-133; the global-id-slot note documented).
- The `on*` attribute path (`onclick="…"`): the global `EventListenerInstancer` hook
  (Factory.cpp:549-556) — registered by the host, compiling the snippet against the view context.

### 3.10 The write router — two-way binding lands here (M3's promissory note)

M3 §4/I3 froze `Set()` to a refusal and named M4 as where writes become legal "routed to a
delegate on the game thread instead of scribbling on a buffer". Implementation: the refusing
`FVaCuusScalarDefinition::Set` gains a registered **write router** (a core seam, set by nobody in
M3 configurations — behavior unchanged there). When VaCuusJs (or any host code) registers one:
`Set(address, value)` marshals `(ViewId, dotted path, value as string/number/bool)` into a
game-thread queue drained by `UVaCuusSubsystem::Tick`, surfacing as
`UVaCuusView::OnModelWrite(FName Model, FString Path, FVaCuusJsValue Value)` (dynamic multicast).
**The shadow is never written** — I3 stands; the value round-trips through gameplay:
handler → game state → `UpdateModel` → the normal pipeline. `Set` returns false still (RmlUi
skips its own `DirtyVariable` on false — DataModel per M3 notes — so no phantom re-render of a
value that did not change). `data-value`/`data-checked`/`data-event` assignments thereby become
functional without a single new RmlUi call. JS additionally gets `vacuus.emit(name, payload)` —
the same queue, no model attached — the generic JS→game channel; `UVaCuusView::OnJsEvent`.

### 3.11 `vacuus.*` host API (Tier 1)

`vacuus.view` (id, size), `vacuus.stats()` (the demo's shape fed from real scopes),
`vacuus.emit(name, payload)` (§3.10), `vacuus.log(...)` (alias of console), localization hook
deferred to M5 with the CLI work (documented). The arch-spec's vague "data-model access" is
scoped for Tier 1 to what documents already have — binding expressions — plus the write router;
a JS object mirror of models is deliberately absent (M3 built the one-way pipeline so JS would
not need a second data path).

## 4. Threading

Everything JS runs on the UI thread: runtime creation, every eval, every callback, every
finalizer (§2(g) — ObserverPtr pool), GC. Asserted at every host entry point with the existing
`IsInUIThread` pattern. Inline mode: `JS_UpdateStackTop` at each handoff (§2(c)). Game-thread
surface: enqueue-only (`ExecuteScript`, the write-router drain, config cvars).

## 5. Shutdown (§7's ordering, made true on every path)

Graceful: DrainCommands' in-band Shutdown already closes documents while the loop lives
(VaCuusUIThread.cpp:950-953) — JS is alive for unload events; the runtime dies at the top of
`Exit()` before the host loop. Hard-stop: `Exit()` step 1 splits — `CloseDocument()` over hosts →
script host `Shutdown()` (runtime death; all JS-held `ElementPtr`s finalize while instancers
live — Element.cpp:2170-2171 needs them) → host `Shutdown()` loop (context removal) →
`Engine.Shutdown()` (the split argument and the RemoveView precedent:
plugin-integration.md §1.3(c)). `JS_FreeRuntime` asserts an empty GC list in debug
(quickjs.c:2348) — a leaked context is loud exactly where we want it.

## 6. Diagnostics

`vacuus.Js.DumpHeap` (console): `JS_ComputeMemoryUsage` summary per runtime + per-view context
count + wrapper-cache sizes + timer/rAF/listener counts. The §3.8 error counter. Stats: `JsPump`
and `JsGC` scopes, heap bytes + GCs-per-window on the PerfLog line. Every refusal named: TLA
module, watchdog trip, OOM fallback, ExecuteScript on a dead view, script on a JS-disabled build.

## 7. Budgets

| | Target | Note |
|---|---|---|
| JsPump, idle (no timers due, no rAF) | ≤0.02 ms | the empty-pump floor; joins the idle gates |
| JsPump, demo-port steady state | ≤0.30 ms | inside the §11 UI budget of 0.5 ms with Update+Record beside it |
| JsGC pause, demo-port, per collection | ≤0.50 ms | **the number this milestone exists to take** — arch spec: "counts against the UI-thread budget"; measured p50/p99 across a soak |
| Heap, demo-port steady state | well under 16 MB | `JS_ComputeMemoryUsage` at each GC |
| Facade op costs (wrap, getElementById, setProperty) | measure, no target | per-op µs table for the docs |
| Idle three layers with a JS-bearing idle document | 0 published / 0 applied / 0 evaluated, and 0 timers fired | extends M3b's exact gates |
| Parity | demo-port renders the same states as the C++ driver | screenshot comparison at deterministic beats |

## 8. Testing

Restore-the-bug where marked; every refusal observed.

- **Vendoring smoke**: eval `1+1`, `typeof queueMicrotask`, `typeof 1n` (E3 settles the context
  recipe), version string equals the recorded tag.
- **Pump**: rAF next-frame semantics (the demo's class-toggle idiom depends on it —
  hud-demo-patterns.md §9); 0 ms self-rearming timer does NOT livelock **(restore: remove the
  frame-start cutoff, watch the test hang → timeout-fail)**; interval re-arm; cancel both timer
  kinds + cancelAnimationFrame; microtask runs before next frame's rAF; callback exception does
  not skip siblings.
- **GC**: threshold disabled ⇒ no implicit GC (allocate hard, assert zero collections between
  frame points); OOM at a tiny test cap → InternalError + fallback GC ran (E5); pause measured.
- **Watchdog**: `while(true)` trips, uncatchable, thread alive, next frame normal **(restore:
  disable the handler, watch the test time out)**.
- **Stack**: deep recursion → RangeError, not a crash (the stack-headroom experiment as a test).
- **Facade**: identity (`getElementById` twice ⇒ `===`); dead handle after `remove()` ⇒ null
  returns, no throw; createElement("DIV") matches RCSS `div` rules **(restore: drop the
  lowercase, watch the style not apply — E1)**; classList vs stale class attribute (assert via
  `GetClassNames` path); style proxy get-copies; querySelector deviations pinned; innerRML set
  kills child handles + detaches listeners synchronously (E3 of rmlui-scripting.md); both
  listener-death orders release the JS ref — heap-count observable **(restore: release in only
  one hook, watch the other path's count grow — E2)**.
- **Documents**: head script runs at OnDocumentLoad against a full body; body `<script>`
  documented-inert (observed); reload recycles the context (timer from the old document never
  fires after reload; scripts re-ran); ExecuteScript ordering after LoadDocument (FIFO
  guarantee); unknown-view ExecuteScript drops at Error.
- **Modules**: import chain via VFS; `vfs://` stripped; TLA module refused with the named Error
  (E1); module throw surfaces through the rejection tracker; rejection later handled ⇒ overlay
  retracts.
- **Write router**: `data-event` assignment reaches `OnModelWrite` on the game thread with the
  right payload; **the shadow is byte-identical after** (I3's test, now with the router on);
  no router registered ⇒ M3 behavior verbatim (the M3a/M3b suites keep passing untouched).
- **Shutdown**: both paths (graceful SIGTERM run + hard-stop) leak nothing — `JS_FreeRuntime`'s
  debug assert is the observable; unload event observed before close.
- **Demo parity + budgets**: §7 table.

## 9. The demo port — acceptance

`m4_demo.rml` + `hud_logic.js`: the M3 demo document driven by JS instead of `PumpDemoModel` —
bars via rAF + style proxy, killfeed via interval + createElement/remove, damage numbers via
timers, stance via classList, the write-refusal button now landing in `OnModelWrite` and echoed
back through `UpdateModel`. `vacuus.M4Demo` toggles it; `Freeze` freezes the JS clock inputs the
same way. AutoShot screenshots at deterministic beats compared against the C++ driver's states.
Gameplay-fed fields (health from the game) keep flowing through data binding **unchanged** — the
demo shows both paths coexisting, which is the product's actual shape.

## 10. Risks

| Risk | Mitigation |
|---|---|
| GC pause blows the budget at real heap sizes | §7 measures on the port; step-GC knob; worst case the heuristic runs more often on smaller heaps |
| Stack undersized for JS-over-RmlUi recursion | 2 MB + 256 KB JS limit + the headroom test |
| Runaway script hangs the UI thread | watchdog, uncatchable, tested |
| Wrapper leaks keep elements' blocks alive | identity cache + OnElementDestroy erase + both-orders listener test + FreeRuntime's debug assert |
| Reload semantics surprise (JS state loss) | browser-refresh semantics documented loudly; models surviving is the continuity story |
| Per-process runtime deviates from arch spec | deliberate amendment with reasons (§2(e)); revisit only if a real isolation need appears |
| quickjs-ng upstream drift | pinned tag + VENDORED_TAG.txt + the vendoring inventory note |

## 11. Out of scope

`@vacuus/preact`, CLI, TSX, sourcemaps (M5); CDP/breakpoints (v2 per arch spec §7); TLA support;
fetch/XHR/WebSocket (non-goals); JS object mirror of data models (§3.11); localization hook (M5);
BigInt decision is settled by E3, not deferred.
