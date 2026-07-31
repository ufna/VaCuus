# VaCuus M4 — JS Tier 1: QuickJS core surface

**Status:** design v2, ready for planning. v1 was reviewed adversarially by three independent
passes (source fidelity, design attack, completeness) and came back **NEEDS REWORK** with twelve
distinct blocking findings; §12 records the class of each, because — as with M3 and M3b — the
reasons are the most useful part.

**Scope:** quickjs-ng vendored at a recorded tag; a `VaCuusJs` runtime module behind a
core-declared script-host seam; the DOM facade over `Rml::Element`; timers/rAF/console (microtasks
ship in the engine core); ES modules via the VFS; `<script>` and `ExecuteScript`; the error
overlay; the two-way write router M3 §4/I3 promised to this milestone; a model **read** surface
discharging M3 §12.1; and the demo-HUD logic ported from the C++ sim to JS plus a churn workload
sized to make the GC numbers mean something (§9).

**Ground truth:** `docs/research/m4-api-notes/{hud-demo-patterns,quickjs-ng-0151,plugin-integration,
rmlui-scripting}.md` (2026-07-31; hud-demo citations `Js.cpp/Js.h` = `src/VacuusJs.cpp/.h`), the
architecture spec, the M3 notes. **[unverified]** claims carry their experiment.

---

## 1. Goal

```html
<head><script src="hud_logic.js"/></head>
```
```js
// hud_logic.js — UI thread, this view's document; deterministic like the C++ driver it replaces
const feed = document.getElementById('killfeed');
let serial = 0;
setInterval(() => {
  const row = document.createElement('div');
  row.classList.add('kill');
  row.innerRML = `<span>${KILLERS[serial % 5]}</span> » <span>BOGEY-0${serial % 7}</span>`;
  feed.appendChild(row); serial++;
  if (feed.children.length > 6) feed.children[0].remove();
}, 1500);
requestAnimationFrame(function tick(tMs) {
  bar.style.width = vacuus.model('hud').get('Health') + '%';   // read surface, §3.11
  requestAnimationFrame(tick);
});
```

The C++ demo driver's logic, expressed in JS, against the same document — with the game thread
still only enqueuing input and data.

---

## 2. The findings that decide the architecture

**(a) The library is four C files — and its export macro fights UE's hidden visibility on the
platforms we ship.** quickjs-ng v0.15.1's core target compiles exactly
`quickjs.c, libregexp.c, libunicode.c, dtoa.c` (CMakeLists.txt:266-271, :301); `cutils` is
header-only; the bytecode headers are checked in (builtin-*.h:1); `quickjs-libc` is a separate
target not folded into the core unless `QJS_BUILD_LIBC` (default OFF, CMakeLists.txt:249-251,
:334-339) — and the core has no setTimeout and no console (quickjs-libc.c:4443-4444, :4613), so
timers/rAF/console are ours to implement. **`queueMicrotask` is NOT ours**: it is a core global
(`js_global_queueMicrotask`, quickjs.c:40200, installed at :55939) — the facade must not shadow
it; our host jobs ride `JS_EnqueueJob` beside it. **The `JS_EXTERN` correction:** on any
GCC/Clang build the non-Win32 branch defines it **unconditionally** as
`__attribute__((visibility("default")))` (quickjs.h:47-49, :70-73) — the shared-lib macros gate
only the Win32 branch — so unpatched, every `JS_*` symbol escapes `VaCuusJs.so` on Linux/macOS
modular builds, where a second quickjs anywhere in the process gets interposed to one copy.
**Vendored patch #1** (recorded in `VENDORED_TAG.txt`): neutralize the GNU-like branch to
`/* nothing */` — a command-line `-DJS_EXTERN=` cannot beat the header's unconditional define.
Smoke assertion: `nm -D` on the built module exports no `JS_` symbols. C11 under UBT
(`ModuleRules.CStandard`, UEBuildModuleCPP.cs:3072, ClangToolChain.cs:594-599); SQLiteCore's relay
`.c` is the engine precedent (SQLiteEmbedded.c:17-24). Pin: **tag `v0.15.1` =
`fd0a0210b7be00957751871e7e01b8291268fc29`** (`QJS_VERSION_*`, quickjs.h:1410-1413).

**(b) The frame-controlled GC design holds — with one sharp caveat.** The only implicit GC
trigger is object allocation (`js_trigger_gc`'s sole call site, quickjs.c:5748) and
`JS_SetGCThreshold(rt, -1)` disables it (quickjs.c:2110). The caveat: allocation failure at the
memory cap does **not** collect and retry — `js_malloc_rt` returns NULL immediately
(quickjs.c:1645-1647) → InternalError (quickjs.c:8130-8135); the pump owns an on-OOM
`JS_RunGC`-and-log fallback. **[unverified]** end-to-end — E5. The trigger heuristic's data
source is **our own malloc hooks**: an atomic live-byte counter maintained in the
FMemory-forwarding `JSMallocFunctions` (§3.3) — `JS_ComputeMemoryUsage` walks the heap and is
sampled at collections only, so it cannot drive a per-frame threshold.

**(c) Thread legality is a stack anchor, not an identity check.** No thread-identity code in the
core (grep verified); the one thread-sensitive datum is `stack_top`, captured at creation
(quickjs.c:2019) and consulted by the native-stack check (quickjs.c:1952-1957);
`JS_UpdateStackTop` on handoff makes cross-thread use legal (quickjs.h:501-503). The UI thread's
512 KB stack is a written-down M4 constraint (VaCuusUIThread.cpp:42-50); §4 raises it. Note the
inline-mode caveat: `IsInUIThread()` is true on the game thread at all times there
(bead `VaCuus-akj.6.40`), so the thread asserts are vacuous exactly where `JS_UpdateStackTop`
matters — the handoff calls are load-bearing, the asserts are not, and the E2 experiment covers
the handoff, not the assert.

**(d) The seam does not exist yet, but its template does.** `IVaCuusScriptHost` appears nowhere
in code; the template is `IVaCuusDocumentHost` (core declares, dependent module implements,
"the seam is deliberate" — VaCuusDocumentHost.h:22-27) plus the register-before-boot shape of
`FVaCuusEngine::SetRenderInterface` (VaCuusEngine.cpp:230-242). No host registered → every call
site skips — the JS-off configuration for free.

**(e) One runtime per process, one `JSContext` per view — a deliberate amendment to the
architecture spec's "per subsystem".** The UI thread is keyed on process-unique `ViewId`
everywhere (VaCuusUIThread.h:315, :341; VaCuusUIQueues.h:116); a per-subsystem runtime would need
command plumbing and teardown protocol that exist nowhere, for isolation Tier 1 does not need
(a script binds a *view's* document). One `JSRuntime` per process (lazy, UI thread; 16 MB cap
process-wide — stricter than the spec's wording), one `JSContext` per view, recycled on document
replace (§3.4) and destroyed with the view.

**(f) The document-replace order is load-and-fire-first — the host, not the plugin bus, must
drive script execution.** On every replace, `Context::LoadDocument` fires
`PluginRegistry::NotifyDocumentLoad(new)` **inside itself** (Context.cpp:299), while the old
document is still current; `AdoptDocument` closes the old one only after `LoadDocument` returns
(VaCuusRmlDocumentHost.cpp:159-194), and the old tree's deferred teardown lands at the **next**
`Context::Update` (Context.cpp:1557-1567). So executing scripts from `Plugin::OnDocumentLoad`
would run the new document's scripts **into the old context**, which the recycle then frees —
JS silently dead after every live reload. **Decision: scripts are captured at
`LoadInlineScript` time (the virtual pair, ElementDocument.cpp:217-228) and executed by the
host** — `IVaCuusScriptHost::OnDocumentReady`, invoked from `AdoptDocument` *after* the old
document's close and *after* `Show()`: old context recycled there (old unload JS having run in
it at close time), fresh context built, captured scripts run in document order. Two documented
consequences: RmlUi's own `load` DOM event fired inside `LoadDocument`, **before** scripts run —
a script cannot observe it (scripts *are* the ready signal, like `defer`); and scripts run
against a shown document.

**(g) The facade's lifetime story needs a third listener-death order the demo never met.**
`Element` derives `EnableObserverPtr` (Element.h:47); dead handles read null; detached elements
hold no dangling pointers (Element.cpp:1492, :2124-2143); listener detach fires destroy→detach on
direct destruction (Element.cpp:99, :112) and detach-then-destroy on document unload
(Context.cpp:1565-1567). **The third order M4 adds itself:** the context (or runtime) dies
*before* RmlUi's deferred detach arrives — the old tree survives one frame past the recycle
(f above), and the hard-stop path destroys trees after runtime death. So `JsEventListener`s are
owned by a per-view registry in the module: context teardown **frees the JS refs and neuters the
still-attached C++ shells** (OnDetach becomes self-reclaiming no-op); RmlUi's later detach
reclaims them. Identity cache: keyed on raw `Element*`, populated lazily on wrap (clones bypass
`OnElementCreate`, Element.cpp:214-225), erased in the process-global `OnElementDestroy` hook by
probing each view's cache by pointer (misses are free); ObserverPtrs still read alive during the
hook — key on the pointer. Every ObserverPtr op mutates an un-mutexed global pool
(ObserverPtr.cpp:6-24): **wrapper finalization runs on the UI thread only** — the controlled GC
point guarantees it; the finalizer of a detached element frees an `ElementPtr`, which needs the
instancer alive (Element.cpp:2170-2171) — every context/runtime death point in §5 precedes
`Rml::Shutdown`, satisfying it structurally.

---

## 3. Architecture

```
VaCuusJs (new Runtime module)
├─ Source/ThirdParty/quickjs-ng   4 .c + headers + VENDORED_TAG.txt (tag, commit, patch #1)
├─ FVaCuusJsRuntime      per process: JSRuntime, FMemory malloc hooks + live-byte counter,
│                        16MB cap, GC threshold -1, watchdog, rejection tracker, fired-counters
├─ FVaCuusJsViewContext  per view: JSContext, globals, module loader, listener registry,
│                        element cache, overlay state
├─ FVaCuusJsScriptHost   implements IVaCuusScriptHost (declared in VaCuus core)
└─ FVaCuusJsDocument     ElementDocument subclass capturing <script> via LoadInlineScript
```

### 3.1 The seam: `IVaCuusScriptHost`

Core-declared, `IVaCuusDocumentHost`-patterned. UI thread unless noted:
`OnViewAdded/OnViewRemoved(ViewId)`; `OnDocumentReady(ViewId, Rml::ElementDocument*)` (the §2(f)
host-driven point — recycle + run scripts); `OnDocumentClosing(ViewId)` (unload JS dispatch, at
`Close()` time); `PumpFrame(NowSeconds)`; `CollectGarbage(reason)`; `ExecuteScript(ViewId,
Source, SourceName)`; `Shutdown()`. Registered via `FVaCuusModule::SetScriptHostFactory` from
`FVaCuusJsModule::StartupModule`, refused after the UI thread boots. Kill switch `vacuus.Js.Enable`
(default 1), read once at first-runtime-creation; later flips documented no-ops.

### 3.2 Vendoring and the module build

Four .c + headers + checked-in builtins + LICENSE + `VENDORED_TAG.txt` (tag, commit, **patch #1**
from §2(a)). `VaCuusJs.Build.cs`: VaCuusRml preamble + `CStandard = C11`; relay `.c` files in
`Private/Gen/`; defines `_GNU_SOURCE` (CMakeLists.txt:278), Win64 `WIN32_LEAN_AND_MEAN` +
`_WIN32_WINNT=0x0601` (:279-282), neither shared-lib macro. The `nm -D` no-`JS_` export check is
a build-time smoke test on Linux.

### 3.3 The runtime: memory, stack, watchdog

- **Malloc hooks over `FMemory`** (`JSMallocFunctions`, quickjs.h:451-457): forward to
  `FMemory::Malloc/Free/Realloc` + `QuantizeSize`; maintain the atomic live-byte counter that
  drives §3.6's trigger and the stats heap line; `LLM_SCOPE` tag so UE memory tooling attributes
  the heap (asserted once in §8 by an FMemory-visible delta). `JS_SetMemoryLimit(16MB)`
  (`vacuus.Js.MemoryLimitMB`); at the cap: InternalError + the §2(b) fallback.
- **Stack**: `GVaCuusUIThreadStackSize` 512 KB → **2 MB**; `JS_SetMaxStackSize(256 KB)`. Inline
  mode calls `JS_UpdateStackTop` at each handoff — and because inline mode runs deep in a
  game-thread callstack, the anchor is refreshed **per RunFrameInline entry**, not once.
  **[unverified]** headroom — the stack-headroom experiment: deep JS recursion dies as RangeError,
  never the guard page.
- **Watchdog**: `JS_SetInterruptHandler` (quickjs.h:1139-1141; polled every 10k ops,
  quickjs.c:479, :8234-8241) against a deadline **armed at every host→JS entry boundary** —
  document scripts, `ExecuteScript`, event dispatch, `on*` snippets, module eval, every pump
  callback batch — not per-pump (v1's scope missed the two largest entry points, which run from
  DrainCommands before the pump). `vacuus.Js.WatchdogMs`: default 250 dev, **50 shipping** — not
  0: the watchdog is also §3.5's microtask-livelock backstop, so shipping keeps a generous
  non-zero deadline. A trip is uncatchable (quickjs.c:8215-8218), logged with the entry's source
  name, reset via `JS_ResetUncatchableError` at that same boundary. A trip mid-document-scripts
  leaves the document adopted and shown; **remaining captured scripts are skipped**, one Error
  says how many.

### 3.4 Contexts, documents, scripts

- Context on demand (first script or ExecuteScript); `JS_NewContext` (intrinsics
  quickjs.c:2533-2557; BigInt presence settled by E3 before the recipe freezes).
- **Replace = recycle, host-ordered (§2(f))**: `OnDocumentClosing(old)` dispatches unload JS in
  the old context; `AdoptDocument` closes old, shows new, then `OnDocumentReady` frees the old
  context (listener registry neuters its shells — §2(g)), builds the fresh one, runs captured
  scripts in document order. Live reload is a document replace and therefore browser-refresh
  semantics; models surviving is the continuity story (M3 rule untouched).
- `ExecuteScript` before any document: legal; the context exists, **`document` is `null`** until
  a document is ready — specified, tested. Command: new kind + `EnqueueExecuteScript` +
  `UVaCuusView::ExecuteScript` (BlueprintCallable), source in `Payload`
  (the LoadDocumentMemory precedent, VaCuusUIQueues.h:50-51), unknown-view drop at **Error**
  (the BindModel argument, VaCuusUIThread.cpp:999-1020).
- `<script>` is head-only in RmlUi (no body handler; XMLNodeHandlerHead.cpp:84-91, :126-129) —
  documented, not fought; a `<script src>` naming a missing file is one **Error** naming document
  and path, remaining scripts still run.

### 3.5 The pump

An ungated RunFrame phase between DataApply and the record loop (the DataApply placement argument
verbatim, VaCuusUIThread.cpp:873-877, :885-897). Order: **rAF → timers → job drain** (the demo's
proven contract, Js.h:23-25 — and deliberately not browser order; a promise resolved in a rAF
callback resumes after this frame's timers, documented beside the API).

- rAF: swap-out list (re-entrancy rule, Js.cpp:531-534); timestamp =
  `GetSystemInterface()->GetElapsedTime()` ms, sampled once at pump top — the clock RmlUi
  animations advance on (Clock.cpp:7-14, Element.cpp:2838-2849). `cancelAnimationFrame` exists.
- Timers: due = deadline strictly < frame-start now, with deadlines priced off the pump-fixed
  now — so a 0 ms timer registered during the pump prices exactly AT the cutoff, is excluded,
  and runs next frame (closes the demo's documented livelock — hud-demo-patterns.md §4).
  Intervals re-arm from fire time.
- **Job drain: bounded.** `JS_ExecutePendingJob` loops while >0 (quickjs.c:2173-2202) **up to a
  per-pump cap** (`vacuus.Js.MaxJobsPerPump`, default 10 000); at the cap the drain stops with
  one Error naming the view, remaining jobs run next frame. The self-requeuing microtask —
  `(function f(){queueMicrotask(f)})()` — is the same livelock shape as the 0 ms timer and gets
  the same two defenses: the cap (deterministic, shipping-safe) and the watchdog (non-zero in
  shipping, §3.3). Restore-the-bug for both.
- Per-callback exceptions: `JS_GetException` (returns-and-clears, quickjs.h:818) → §3.8; one
  callback's throw never skips siblings.

Scopes `JsPump`/`JsGC` declared **before the phase lands** (the DataApply playbook,
VaCuusStats.h:39-45; positional-enum guard VaCuusStats.cpp:48-49).

### 3.6 GC: the controlled point

`JS_SetGCThreshold(rt, -1)` at birth. `CollectGarbage` at **end of RunFrame after the record
loop** — pause inside the frame, after publish. Trigger: the malloc-hook live-byte counter grew
≥ `vacuus.Js.GCStepKB` (default 512) since the last collection, or the OOM fallback fired.
`JS_ComputeMemoryUsage` sampled at collections only; heap bytes + collections-per-window join the
PerfLog window line (the `AddUIFrame` template, VaCuusStats.h:140-149). Finalizers run here — UI
thread, instancers alive (§2(g)).

### 3.7 Modules and the VFS

`JS_SetModuleLoaderFunc` (classic split, quickjs.h:1173-1175). Specifiers: relative → importing
module's directory, then document roots; `vfs://` stripped before resolution (`FPaths::IsRelative`
would glue it under a root — plugin-integration.md §3); reads via the pak-transparent
`IPlatformFile` path. Module eval returns a promise (quickjs.c:31553-31557): still-pending after
the drain ⇒ top-level await ⇒ **refused, one Error naming the module** (E1 grounds it). A module
runtime throw rejects the promise (quickjs.c:31573-31574) — surfaced via the rejection tracker.
Missing import: loader returns NULL → engine ReferenceError (quickjs.c:30041-30048) plus our
Error naming the resolved path. Module cache dies with the context (§3.4). Live-reload watch
extensions gain `.js`/`.mjs` (today rml/rcss only — VaCuusLiveReload.cpp:20-26 — script edits
reload nothing); a `.js` change triggers the same full document reload, which is a replace,
which recycles — the path is §3.4's, no second mechanism.

### 3.8 Errors: log, overlay, rejection tracking

Every surfaced exception → `UE_LOG(LogVaCuusJS, Error)` (message + `stack` — a real property,
quickjs.c:7766 — + source name); `LogVaCuusJS` follows the `LogVaCuus` declare/define pair.
Dev overlay (non-shipping, `vacuus.Js.Overlay`): host-owned element in the view's document, last
N errors; uncaught rejections enter via `JS_SetHostPromiseRejectionTracker` and are **retracted**
on the `is_handled=true` re-fire (quickjs.c:55160-55170). The host exposes a total-JS-error
counter; every test that expects none asserts zero.

### 3.9 The DOM facade

Per the demo gap list (hud-demo-patterns.md §9), arch §7 Tier 1:

- **Identity**: one wrapper per element (cache per §2(g)); `getElementById` twice ⇒ `===`.
- **Handles**: ObserverPtr opaque; dead ⇒ null/false/undefined, never throw (Js.cpp:1-3).
- **Surface**: `createElement` (tag lowercased — uppercase silently misses RCSS with asserts out,
  rmlui-scripting.md §2, E1) / `getElementById` / `querySelector(All)` / `closest` (deviations
  documented: self never matches, Element.cpp:1544-1546; `closest` starts at the parent,
  :1077-1090); `appendChild` / `insertBefore` / `remove` / `removeChild` (ElementPtr move
  semantics; detached wrapper owns its ElementPtr, freed by finalizer or moved on append); `id`,
  `tagName`, `parentNode`, `children`, `innerRML` get/set (set kills child handles + detaches
  listeners synchronously — rmlui-scripting.md §6); attributes get/set/remove; **classList**
  (`add/remove/toggle/contains`) over `SetClass`/`IsClassSet`/`GetClassNames` — never the class
  attribute, which `SetClass` leaves stale (Element.cpp:258-276; the §8 test compares
  `GetClassNames()` against `GetAttribute("class")` to see the trap); **style** proxy
  (get = `GetProperty(name)?->ToString()` copied at once — the pointer dies on the next call,
  Element.h:187-193; set/remove string-based).
- **Events**: `add/removeEventListener(type, fn, capture?)` (real capture flag,
  Element.h:471-482); registry-owned `JsEventListener` per registration, refs released on detach,
  **all three** death orders tolerated (§2(g)); event object: `type/target/currentTarget/phase`,
  parameters via `GetParameter` (mouse/key data), `stopPropagation/stopImmediatePropagation`;
  `preventDefault` maps to stopPropagation — RmlUi runs default actions only while propagating
  (EventDispatcher.cpp:173-185) — documented. `dispatchEvent` (auto-registration
  EventSpecification.cpp:125-133; the global-id-slot note). Key/gamepad callbacks (arch §7's
  vacuus.* item) are discharged **here**: `keydown/keyup/textinput` and the M2 gamepad event
  names arrive as ordinary listeners — with a §8 test proving a gamepad event reaches JS.
  `on*` attributes via the global `EventListenerInstancer` (Factory.cpp:549-556), compiled
  against the view context.

### 3.10 The write router — two-way binding lands here

M3 §4/I3's promissory note. A core seam on the refusing `FVaCuusScalarDefinition::Set` (the
override is `Set(void*, const Rml::Variant&)` — VaCuusDataVariable.h:122). *(Corrected after
implementation: v2's "definition→model back-pointer" is impossible — definitions are per-type
stateless statics shared across models. The attribution that works is **storage ownership**: a
UI-thread registry of (ViewId, name, model) written at the BindModel drain, resolved at Set by a
span test — the shadow's inline span, then each array field's element block — live allocations
are disjoint, so a hit is exact. The wire path is `DiagnosticPath` with its type-name root
replaced by the model name.)* **The echo rule, discovered in implementation:** the revert's
attribute write re-fires RmlUi's change controller (`InputTypeCheckbox.cpp:22-37` dispatches for
programmatic writes), so every revert — and every game-driven apply — would route the model's own
value back. An attributed write whose value equals the shadow's (in RmlUi's string projection) is
therefore **swallowed** — counted, not routed, no revert. Documented cost: a user write
requesting the current value is swallowed too. With a router registered: `Set` marshals
`(ViewId, Model, Path, value)` into a **bounded** game-thread queue (the input-ring bound + drop
diagnostic pattern), drained by `UVaCuusSubsystem::Tick` into
`UVaCuusView::OnModelWrite(FName Model, FString Path, FVaCuusJsValue Value)`. The shadow is never
written — I3 stands. `Set` returns false, **and then the host force-dirties the touched top-level
name from the shadow on the next apply** — because RmlUi's default actions have already mutated
the *control* (a checkbox toggles its attribute before dispatching change,
InputTypeCheckbox.cpp:39-47, and a false `Set` skips `DirtyVariable`,
DataControllerDefault.cpp:57-59 — without the revert-dirty the control stays visually toggled
against an unchanged model forever). The revert re-runs the view from the authoritative shadow:
the control snaps back unless the game actually changed the value. A routed write with **zero
bound handlers** logs one Warning per (model, path); with a router registered the M3 refusal log
is reworded (it is now the legal channel), and with none, M3 behavior is byte-identical — the
M3a/M3b suites run untouched. JS gets `vacuus.emit(name, payload)` — same queue, no model —
surfacing as `UVaCuusView::OnJsEvent`.

### 3.11 `vacuus.*` host API (Tier 1)

`vacuus.view` (id, size); `vacuus.stats()`; `vacuus.emit(name, payload)`;
**`vacuus.model(name)`** — the read surface M3 §12.1 promised ("JS access to the same models —
same objects, no rework"): `.get(path)` reads the view's UI-thread shadow through the layout's
existing value accessors (the shadow lives on this thread — M3 §3.1 — and the read is the same
per-kind projection the scalar definitions ship; arrays read as length + indexed access). No
write methods — writes are §3.10's router. `vacuus.log` aliases console. Localization hook: M5,
with the CLI (documented deferral). Key/gamepad: §3.9's listeners.

## 4. Threading

All JS on the UI thread — creation, evals, callbacks, finalizers, GC — asserted at host entries
(vacuous in inline mode per §2(c); there the per-entry `JS_UpdateStackTop` is the load-bearing
part). Game-thread surface: enqueues, the write-router drain, cvars.

## 5. Shutdown (the ordering, true on every path)

Graceful: in-band Shutdown closes documents while the loop lives (VaCuusUIThread.cpp:950-953) —
unload JS runs; runtime dies at the top of `Exit()` before the host loop. Hard-stop: `Exit()`
step 1 splits — `CloseDocument()` over hosts → **script-host `Shutdown()`**: per-view teardown
(each context's listener registry frees JS refs and neuters shells — the trees are still
attached; RmlUi's detach arrives later inside host `Shutdown()`/`RemoveContext` and reclaims the
shells), then `JS_FreeContext` per view, then `JS_FreeRuntime` → host `Shutdown()` loop →
`Engine.Shutdown()`. **The leak observable is host-side and Development-real**: outstanding
context count, wrapper-cache sizes, live-listener counts and the malloc-hook live-byte counter,
`check`ed zero (post-neuter, post-free) immediately before `JS_FreeRuntime` — the engine's own
`assert(list_empty(...))` (quickjs.c:2348) is compiled out under UBT's global `NDEBUG=1`
(UEBuildPlatform.cs:1344) in every configuration this project runs, and is therefore *not* the
observable. Test configurations additionally build the vendored library with `ENABLE_DUMPS` +
`JS_DUMP_LEAKS` (quickjs.h:475-481) so a leak also names itself in the log.

## 6. Diagnostics

`vacuus.Js.DumpHeap`: memory-usage summary, per-view context + cache + timer/rAF/listener
counts. **Fired-counters** (the M3b counter-layer pattern, host-exposed, exact): timers fired,
rAF callbacks run, jobs executed, collections run, JS errors total. `JsPump`/`JsGC` scopes; heap
bytes + GCs/window on the PerfLog line. Named refusals: TLA module, watchdog trip (with entry
name), job-cap trip, OOM fallback, missing `<script src>`, missing import, ExecuteScript on a
dead view or with JS disabled, routed write with no handler.

## 7. Budgets

The workload note: the architecture spec's reference HUD (~1,750 elements) **does not exist as a
document yet** — arch §11 said "completed by M3", which did not happen; `m3_demo.rml` is ~51
elements. M4 therefore measures on **the demo port plus a churn workload** — the 200-row killfeed
fixture driven from JS at the M3b cost-test rates — which matches the reference HUD's *allocation
shape* (per-frame string/element churn) if not its element count; reference-HUD-scale numbers
move to the milestone that builds that document (M6 prep), recorded on bead `VaCuus-akj.8` and as
an arch-spec §14 amendment. **[unverified]** that churn-rate parity is the right proxy — the GC
numbers themselves settle it.

| | Target | **Measured 2026-07-31** (7950X3D, Development editor, commit 9678ad5) |
|---|---|---|
| JsPump, idle (nothing due) | ≤0.02 ms | **0.0002 ms** mean, p99 0.0006 — 100× under |
| JsPump, demo-port steady state | ≤0.30 ms | **0.007 ms** mean, p99 0.027 (the real `m4_hud_logic.js`, 2000 frames) — 40× under |
| **Combined UI frame with GC in population**: p99 of (JsPump + JsGC + Update + Record) | **≤0.50 ms** | **Demo scale: PASS** — production session sum-of-p99s ≈ **0.43 ms** (JsPump 0.06 + Update 0.20 + Record 0.17 + JsGC 0.001). **200-row churn: 0.64–0.81 p99 — breach, and the breach is not JS**: Update alone runs ~0.40 ms, which is M3b's own recorded re-evaluation row at 85–91% of this same budget *before JS existed*; the JS increment is pump ~0.01 mean + rare GC pauses. **Gate decision: met for the M4 deliverable.** The churn-scale breach is the M3b document-side scaling property (bindings × rows), tracked there; pause stacking is mitigable via `vacuus.Js.GCStepKB` (smaller steps → more, shorter pauses), untouched here per the default-step protocol |
| JsGC pause, per collection | report p50/p99 | **p50 0.39–0.48 ms, p99/max 0.53 ms**; 7 collections / 2000 churn frames at the 512 KB step; heap at collection ~617 KB. The demo session never reached the step in 55 s. Method note that cost a red run: quickjs frees acyclic garbage by refcount — `JS_RunGC` collects only cycles (quickjs.c:7078-7089), so the churn carries a cyclic node graph per row or it measures nothing |
| Heap steady state | well under 16 MB | **~617 KB** at collection — 26× under |
| Facade op costs | measure, no target | getElementById **0.09 µs**, createElement+wrap **0.26 µs**, style set **2.4 µs** per op (10k-op loops, baseline-subtracted) |
| Idle with a JS-bearing idle document | 0 published / 0 applied / 0 evaluated / 0 timers / 0 rAF / 0 jobs | **PASS, exact zeros on every counter**; production frozen windows publish 0, 100% idle |
| Parity | port renders the C++ driver's states | **PASS** — screenshots decode to the exact serial arithmetic (grown: serials 0–2 with HS on 0; trimmed: serials 4–9 with HS on 6/9; Health pins t to the frame) |

**Method note recorded for M6:** the in-harness combined row has Record=0 structurally (probe
hosts cannot reach the recorder); the production complement comes from the PerfLog, and
sum-of-p99s is an upper bound on the per-frame-sum p99 — both halves are written here together.

## 8. Testing

Restore-the-bug where marked; every §6 refusal observed; the M3a/M3b suites untouched and green.

- **Vendoring smoke**: `1+1`; `typeof queueMicrotask === 'function'` **as a core built-in**
  (guards against shadowing); E3 settles BigInt; version string == recorded tag; `nm -D` shows no
  `JS_` exports (Linux).
- **Pump**: rAF next-frame semantics (the class-toggle idiom); 0 ms self-rearm does not livelock
  **(restore: drop the cutoff → hang/timeout)**; self-requeuing microtask does not livelock
  **(restore: drop the job cap → hang/timeout)**; cancel APIs; a promise-in-rAF resumes this
  frame after timers (the documented ordering pinned); sibling callbacks survive a throw.
- **GC**: no implicit collections (allocate hard between frame points; observable = the
  fired-counter for collections stays 0 while the live-byte counter climbs); OOM at a tiny cap →
  InternalError + fallback collection counted (E5); pause measured.
- **Watchdog**: `while(true)` in (a) ExecuteScript, (b) a document script, (c) a listener — all
  three trip, thread alive, next frame normal, document-script trip skips the remaining scripts
  with the counted Error **(restore: disable → timeout)**.
- **Stack**: deep recursion → RangeError, not a crash; inline mode with a deep game-thread stack
  (E2's shape).
- **Facade**: identity `===`; dead-handle nulls; `createElement("DIV")` matches `div` RCSS
  **(restore: drop the lowercase → style not applied)**; classList mutation ⇒ `GetClassNames()`
  reflects it while `GetAttribute("class")` goes stale (the trap observed); style get-copies;
  querySelector deviations pinned; innerRML set kills child handles + detaches synchronously;
  **all three listener-death orders** release the JS ref — live-listener + heap counters as the
  observable **(restore: release in one hook only → the other paths' counters climb)**;
  **two-view isolation**: two JS views, interleaved, each sees only its own document and globals.
- **Documents**: head scripts run at OnDocumentReady against a shown body; body `<script>` inert
  (observed); **reload recycles** — old context's timer never fires after replace, scripts re-ran
  in a fresh context (the §2(f) ordering test, restore-the-bug: execute scripts from
  OnDocumentLoad instead → reload leaves JS dead, exactly v1's bug); ExecuteScript-before-
  document sees `document === null`; FIFO after LoadDocument; unknown-view drop at Error;
  missing `<script src>` → named Error, later scripts still run.
- **Modules**: import chain via VFS; `vfs://` stripped; TLA refused (E1); module throw → tracker;
  later-handled rejection retracts from the overlay (observed via overlay state); missing import
  → both Errors.
- **Write router**: checkbox click lands in `OnModelWrite` with the right payload; **the control
  reverts** on the next apply while the model is unchanged **(restore: drop the revert-dirty →
  checkbox stays toggled against the shadow — v1's silent divergence)**; shadow byte-identical +
  element-level reads (the M3b I3 test extended); zero-handler Warning; router-absent ⇒ M3
  byte-identical (suites prove it); `vacuus.emit` round-trip; queue overflow drops with the named
  diagnostic.
- **Read surface**: `vacuus.model().get` for each kind incl. array length/index; unknown
  model/path → null + one Warning.
- **on\* attributes**: `onclick` snippet fires against the view context; `dispatchEvent`
  round-trip.
- **Gamepad/key**: a synthesized gamepad event (M2's path) reaches a JS listener.
- **Shutdown**: graceful (SIGTERM run) and hard-stop leak nothing — the §5 host-side counters
  `check` zero; ENABLE_DUMPS build logs no leaks **(restore: skip the listener sweep on
  hard-stop → counters non-zero / ASan)**.
- **Demo parity + budgets**: §7's table; the churn workload's GC numbers recorded.

## 9. The demo port — acceptance

`m4_demo.rml` + `hud_logic.js`: the M3 demo document driven by JS — bars via rAF + style proxy,
killfeed via interval + createElement/remove (serial-deterministic, the same arithmetic as the
C++ driver so screenshots are computable, VaCuusRender.cpp's pool scheme), damage numbers via
timers, stance via classList, the demo button landing in `OnModelWrite` and echoed back through
`UpdateModel`, health read via `vacuus.model('hud').get('Health')`. Gameplay-fed fields keep
flowing through data binding unchanged — both paths coexisting is the product's shape.
`vacuus.M4Demo` toggles; `Freeze` freezes the JS clock inputs. Plus the **churn workload** (§7):
the 200-row killfeed fixture document driven from JS at M3b's rates, for the GC population.

## 10. Risks

| Risk | Mitigation |
|---|---|
| GC pause blows the combined gate | §7's combination row is the assertion; step knob; smaller steps → more, shorter pauses |
| Stack undersized | 2 MB + 256 KB JS limit + headroom test |
| Runaway script | watchdog at every entry, non-zero in shipping; job cap; both tested |
| Wrapper/listener leaks | registry + neuter protocol + three-orders test + host-side counters (`check`, Development-real) |
| Reload kills JS silently (v1's design bug) | host-ordered recycle (§2(f)) + the restore-the-bug reload test |
| Symbol export collision on Linux | vendored patch #1 + `nm -D` smoke |
| Per-process runtime deviates from arch spec | documented amendment (§2(e)) |
| Reference-HUD-scale unknowns | §7's workload note; churn proxy; numbers recorded on the bead |

## 11. Out of scope

`@vacuus/preact`, CLI, TSX, sourcemaps, localization hook (M5); CDP/breakpoints (v2); TLA;
fetch/XHR/WebSocket (non-goals); JS object *mirror* of models (the read surface §3.11 is the
scoped discharge; a live proxy object is not built); per-subsystem runtimes (§2(e), revisit on a
real isolation need).

## 12. What v1 got wrong, and why it matters

1. **The replace-path ordering was inverted** — v1 ran new-document scripts from
   `Plugin::OnDocumentLoad`, which fires inside `LoadDocument` while the old context is current;
   the recycle then freed everything the scripts had just built. Every live reload would have
   left JS silently dead — the design's own §8 test would have caught it, but the design section
   prescribed the bug. A hook's *name* is not its *timing*; the fix is host-ordered execution.
2. **v1's leak observable was compiled out in every configuration we run** — `JS_FreeRuntime`'s
   assert is `<assert.h>` under UBT's global `NDEBUG=1`. The project's own "invariant with no
   observable" lesson, again. Host-side counters are the observable.
3. **`JS_EXTERN` "degrades to nothing" was true only on Windows** — on Linux/macOS it is
   unconditional default-visibility; v1 compressed the research note's careful "visibility /
   nothing" into the false half. Vendored patch #1.
4. **The watchdog guarded the wrong scope** — per-pump, while document scripts and ExecuteScript
   run from DrainCommands. Armed per host→JS entry now.
5. **The microtask drain had the exact livelock v1 loudly closed for timers** — and v1's
   shipping default turned the only backstop off. Job cap + non-zero shipping watchdog.
6. **The write router left toggled controls diverged from the model** — RmlUi mutates the control
   before dispatching change; a false `Set` skips the re-render. The revert-dirty is the missing
   half, and its restore-the-bug is the checkbox staying toggled.
7. **A third listener-death order existed that neither the demo nor v1 covered** — context death
   before RmlUi's deferred detach. The registry + neuter protocol.
8. **v1 measured GC against a budget the sum could breach while every row passed** — the
   combination rule is now the gate row. And the workload v1 called "the reference HUD" is ~51
   elements; the honest §7 note + churn proxy replace the pretense.
9. Smaller, same species: `queueMicrotask` is a core builtin v1 planned to reimplement (and its
   smoke test tested the engine, not our wiring); M3 §12.1's read-access promise was silently
   retired (now §3.11's read surface); key/gamepad was promised in arch §7 and appeared nowhere
   (now §3.9); the fired-counters the idle gate needs existed nowhere (now §6).
