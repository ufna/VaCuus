# HUD-demo pattern inventory (the proven QuickJS↔RmlUi prototype)

Base: `/w/Unreal/VaCuus/docs/research/hud-demo/`. Cited as `main.cpp` = `src/main.cpp`, `Js.cpp` = `src/VacuusJs.cpp`, `Js.h` = `src/VacuusJs.h`, `hud.js` = `data/hud.js`. The "657 LOC" = 618 (`Js.cpp`) + 39 (`Js.h`). Every cited line was opened and read; derivations from cited lines are marked **[derived]**, uncited engine behavior **[inference]**.

## 1. Runtime/context lifecycle

**Creation order (main.cpp):** `Backend::Initialize` (main.cpp:102) → `Rml::SetSystemInterface`/`SetRenderInterface`/`Rml::Initialise` (main.cpp:108–110) → `Rml::CreateContext` (main.cpp:112) → fonts (main.cpp:120–124) → `LoadDocument` + `Show` (main.cpp:126–134) → **then** `VacuusJs::Initialize(context, document, auto_exit_seconds)` (main.cpp:136) → `VacuusJs::EvalFile("hud.js")` (main.cpp:143). JS comes up **after** RmlUi is fully alive and the document is already shown; the script binds against an existing tree.

**Inside Initialize (Js.cpp:429–472):** `JS_NewRuntime` (Js.cpp:434), `JS_NewContext` (Js.cpp:437), element class registration (Js.cpp:441–446), then globals: `vacuus` object (Js.cpp:450–453), `console` (Js.cpp:455–459), timers/rAF (Js.cpp:461–466), injected constant `AUTO_EXIT_SECONDS` (Js.cpp:468).

**Memory limit: NONE.** No `JS_SetMemoryLimit`, `JS_SetGCThreshold`, or `JS_SetMaxStackSize` anywhere (grep over all three sources hits only `JS_Eval`, Js.cpp:517). The spec's 16 MB cap (spec 2026-07-29-vacuus-architecture-design.md:241–242) is **unprototyped** — M4 adds it fresh. *Experiment "cap-fit": run the reference HUD under `JS_SetMemoryLimit(16MB)` and record peak via `JS_ComputeMemoryUsage`.*

**Teardown order (main.cpp:190–198)** — this is the spec's shutdown clause verbatim origin (Js.h:16–18: "Call AFTER the RmlUi document tree has been destroyed (doc->Close() + context->Update()) and BEFORE Rml::Shutdown()"):
1. `document->Close()` then `context->Update()` **while JS is alive** (main.cpp:194–195) — comment: "event listeners self-release their JS callbacks on detach" (main.cpp:192–193). [inference] the `Update()` is what actually destroys the closed document, which is why it must precede JS teardown.
2. `VacuusJs::Shutdown()` (main.cpp:196): frees timer fns (Js.cpp:478–480), rAF fns (Js.cpp:481–483), any never-detached listeners via `FreeAtShutdown()` + `delete` (Js.cpp:486–491, comment 484–485: "should be none if the document tree was destroyed first"), then `JS_FreeContext`/`JS_FreeRuntime` (Js.cpp:493–494).
3. `Rml::Shutdown()` (main.cpp:197).

Process exit code = 2 if any JS exception was ever dumped (`g_error_count`, Js.cpp:30/49; main.cpp:190, 200–201) — errors are a first-class smoke-test observable.

## 2. Element facade

- **Storage:** `struct ElementHandle { Rml::ObserverPtr<Rml::Element> el; }` (Js.cpp:93–95), heap-allocated per JS object, attached as class-id opaque: `JS_NewObjectClass(g_ctx, g_element_class_id)` + `JS_SetOpaque(obj, new ElementHandle{el->GetObserverPtr()})` (Js.cpp:114–121). Class registered with `JS_NewClassID`/`JS_NewClass` and `JSClassDef{"VacuusElement", ElementFinalizer}` (Js.cpp:97–106, 441–442); methods live on a shared prototype via `JS_SetClassProto` (Js.cpp:443–446).
- **Dead-handle detection:** every access goes through `GetElement(this_val)` = `JS_GetOpaque` → `handle->el.get()` (Js.cpp:108–112); ObserverPtr yields nullptr after element death and each method then returns `JS_FALSE`/`JS_NULL` (e.g. Js.cpp:174–176, 192–194). Design stated in the file header: "a handle whose element got removed simply goes 'dead': methods return false/null" (Js.cpp:1–3). GC finalizer deletes only the handle, never the element (Js.cpp:97–100).
- **Identity: one JS object per WRAP CALL, not per element.** `WrapElement` unconditionally news an object (Js.cpp:114–121); two `getElementById('x')` calls give `===`-unequal handles. No cache, no back-pointer from `Rml::Element`. hud.js works because it caches handles itself (hud.js:22, 28–29, 80, 117). A production facade wanting DOM-like identity needs a per-element cache — unprototyped.
- **Exhaustive method list** (Js.cpp:267–278): `isValid`, `setInnerRML`, `setAttribute`, `getAttribute`, `setProperty`, `removeProperty`, `setClass`, `appendRML`, `remove`, `addEventListener`. **No properties/getters at all** — no `parentNode`, `children`, `id`, `innerRML` getter.
- **Traversal:** essentially none. `appendRML(rml)` parses into a temp div and moves children over, returning the **last** appended element wrapped (Js.cpp:229–243); `remove()` uses `GetParentNode()` internally but never exposes it, relying on the discarded `RemoveChild` return: "returned ElementPtr destroys the element" (Js.cpp:245–255). `vacuus.createElementIn(parentId, tag[, innerRML])` appends into an id-addressed parent and returns the wrap (Js.cpp:291–304). All creation is create-and-immediately-attach; detached nodes never exist in JS.

## 3. Events

- **Listener:** `class JsEventListener final : public Rml::EventListener` (Js.cpp:128–162), one per `addEventListener` call, `new`ed with a `JS_DupValue`'d function ref (Js.cpp:262) and registered in a global `std::unordered_set` for shutdown accounting (Js.cpp:125–126, 130–131).
- **Ref lifecycle:** the JS fn is freed in `OnDetach`, which then `delete this` (Js.cpp:147–152) — element removal/destruction is what releases the ref; nothing else does. `FreeAtShutdown` covers the never-attached/never-detached remainder (Js.cpp:154–158, 486–490).
- **Capture/bubble: NOT supported.** `el->AddEventListener(type, listener)` with no phase argument (Js.cpp:263) — [inference] RmlUi defaults `in_capture_phase=false`, so bubble-phase only. No `removeEventListener` exists at all (method list Js.cpp:267–278).
- **Event object** (built fresh per dispatch, Js.cpp:137–144): exactly `type` (string), `targetId` (string, "" if no target), `target` (freshly wrapped element). No `currentTarget`, no `stopPropagation`/`preventDefault`, no mouse coords, no key data. hud.js's two click handlers (hud.js:133, 182) ignore the event entirely.
- **[derived] Latent hazard the demo never hit:** if a JS handler removes its own element, `RemoveChild`'s returned `ElementPtr` destroys it at statement end (Js.cpp:253) → detach → `OnDetach` → `delete this` while `ProcessEvent` is on the stack. hud.js only removes elements from timer callbacks (hud.js:67, 106), never from event handlers. *Experiment "self-removal-during-dispatch": click handler calls `ev.target.remove()`; run under ASan.*

## 4. Timers / rAF

- **Structures:** `struct Timer { int64_t id; double deadline; double interval; JSValue fn; bool dead; }` in a flat `std::vector` (Js.cpp:32–39); ids monotonic from 1 (Js.cpp:40); rAF is a plain `std::vector<JSValue> g_raf_pending` "callbacks for the NEXT frame" (Js.cpp:42).
- **Pump location:** `VacuusJs::OnFrame(now)` called in the main loop **before** `context->Update()` (main.cpp:161–164); contract stated in Js.h:23–25. This is exactly the spec's "Pump JS … before Context::Update()" step.
- **Ordering within a frame (Js.cpp:525–580):** (1) rAF — swap-out so callbacks registered during the run land next frame (Js.cpp:531–542); (2) timers; (3) `JS_ExecutePendingJob` drained to empty, exceptions dumped (Js.cpp:568–579). Note rAF runs **before** timers, the opposite of browser scheduling ([inference] browsers run rAF after the task queue, just before render).
- **rAF timestamp:** one shared `JS_NewFloat64(now_seconds * 1000.0)` — milliseconds, host steady-clock since app start, same value for every callback that frame (Js.cpp:535–541). rAF returns `undefined` — **no handle, no cancelAnimationFrame** (Js.cpp:418–424).
- **Timer semantics:** ms arg defaults 0, negatives clamped to 0 (Js.cpp:382–386); non-function throws TypeError (Js.cpp:380–381); no extra-args forwarding (`CallVoid(fn, 0, nullptr)`, Js.cpp:554). Index loop because "callbacks may append new timers" (Js.cpp:544–545). Intervals reschedule from `g_now` **before** the call (Js.cpp:549–550) → drift, no catch-up bursts. One-shots marked dead before the call; fn is dup'd first because "callback may clear itself" (Js.cpp:552–553). Dead entries swept and freed after the pass (Js.cpp:557–566).
- **`clearTimeout`/`clearInterval` are the same function** (Js.cpp:405–416, registered Js.cpp:463–464): linear scan, sets `dead=true`; unknown id is a no-op — hud.js relies on `clearTimeout(0)` being harmless (`let hmTimer = 0` then `clearTimeout(hmTimer)`, hud.js:81, 86).
- **[derived] Hazard:** a timer registered with 0 ms during the timer pass is appended to the vector being index-iterated with `deadline = g_now`, and the skip test is `deadline > g_now` (Js.cpp:390, 547) — so it **fires in the same frame**, and a 0 ms timer that re-registers itself livelocks `OnFrame` forever. The production pump needs a per-frame cutoff (snapshot length or "due before frame start"). *Experiment "zero-ms-cascade": `(function f(){ setTimeout(f, 0); })()` — demo pump hangs; M4 pump must not.*

## 5. console.*

Only `log`/`warn`/`error` (Js.cpp:455–458). All route through `PrintArgs`: prefix, then `JS_ToCString` of each arg space-joined, newline, `fflush` (Js.cpp:324–337). Prefixes/streams: `[js] `→stdout, `[js-warn] `→stderr, `[js-err] `→stderr (Js.cpp:362–373); the separate `vacuus.log` uses `[vacuus] `→stdout (Js.cpp:339–342). No printf-style format specifiers, no object inspection ([inference] plain objects print as `[object Object]` via `JS_ToCString`).

## 6. Modules / script loading

`EvalFile`: whole-file fread, then `JS_Eval(g_ctx, src, len, path, JS_EVAL_TYPE_GLOBAL)` (Js.cpp:501–523) — **classic global script, not a module**; path is passed only for stack traces. No `JS_SetModuleLoaderFunc`, no import machinery anywhere (grep confirms). hud.js compensates with `'use strict'` (hud.js:3) and IIFEs (hud.js:47, 71, 109). The spec's ES-modules-via-VFS (spec:253–254) is **entirely greenfield**.

## 7. GC

**Zero explicit GC.** No `JS_RunGC`, no threshold tuning, no measurement (grep over all sources: only hit is `JS_Eval`, Js.cpp:517). All collection was implicit/allocation-driven, and 144 fps was achieved that way — but the demo churns objects heavily (fresh event objects per dispatch Js.cpp:137, fresh wraps per access Js.cpp:118, damage/killfeed rows via timers hud.js:59, 98), so the spec's controlled-frame-point GC with pause accounting (spec:243–246) has **no reference numbers**. *Experiment "gc-pause": run the HUD with `JS_RunGC` forced each frame after the pump vs. never, log pause with the same EMA machinery main.cpp:181–187.*

## 8. vacuus.* host surface

Registered functions (Js.cpp:350–357): `getElementById(id)` (Js.cpp:283–288), `createElementIn(parentId, tag[, innerRML])` (Js.cpp:291–304), `stats()` → `{updateMs, renderMs, fps}` fed from C++ EMAs (Js.cpp:306–313; `SetStats` Js.cpp:601–606; measured/fed at main.cpp:160, 181–187), `contextSize()` → `{w,h}` (Js.cpp:315–322), `log`, `exit()` → sets flag polled by the main loop (Js.cpp:344–348; main.cpp:153–154). Plus one **JS-assigned data property**: `vacuus.onKey` — host fetches and calls it per key (Js.cpp:582–599), fed by a key callback that maps only `1–4`/`Escape`/`Space` and **never consumes** ("let the context see the event too", main.cpp:25–47). Plus injected global `AUTO_EXIT_SECONDS` (Js.cpp:468). **No data-model access, no view/document management, no localization** — all spec §7 vacuus.* items beyond stats/key-callback (spec:255–256) are unprototyped.

## 9. hud.js — what Tier 1 actually needs vs. what the demo never had

**Used by the 245-LOC HUD (the real priority list):**

| API | hud.js sites |
|---|---|
| `getElementById` (via `el()`) | hud.js:22, 28–29, 42, 45, 80, 117, 179–182, 196 |
| `setClass` — the workhorse | hud.js:36, 42, 45, 83–87, 102, 105, 107, 119, 125–127, 141–147, 179–180 |
| `setProperty` (left/top/width/height only) | hud.js:32–33, 64–65, 142, 149, 170–171 |
| `setInnerRML` | hud.js:34–35, 143, 150, 196–199 |
| `createElementIn` + `setAttribute('class', …)` | hud.js:59–61, 98–101, 163–164 |
| `remove` | hud.js:67, 106 |
| `addEventListener('click')` | hud.js:133, 182 |
| `setTimeout` / `setInterval` / `clearTimeout` | e.g. hud.js:48, 45, 86, 194 |
| `requestAnimationFrame` | hud.js:85, 102, 145, 217, 220 |
| `stats` / `contextSize` / `log` / `exit` / `onKey` | hud.js:195, 21, 245, 242, 185–191 |

Bound but **never called** by hud.js: `isValid`, `getAttribute`, `removeProperty`, `appendRML` (bindings Js.cpp:267–278). Key idiom to preserve: **class-toggle-across-frames animation restart** — clear class, re-add inside next rAF (hud.js:44–45 + 83–87, hud.js:102, hud.js:144–146); Tier 1's rAF must keep "queued during pump → next frame" semantics (Js.cpp:531–534) or every retrigger breaks.

**Gap list — spec §7 Tier 1 items (spec:248–258) the demo NEVER implemented:** `querySelector(All)` (only `getElementById`, Js.cpp:283–288) · `classList` object (only `setClass`, Js.cpp:218–225) · style proxy (only string `setProperty`/`removeProperty`, Js.cpp:201–216) · detached element creation & `insertBefore` (attach-on-create only, Js.cpp:291–304) · capture phase, `removeEventListener`, `stopPropagation`/`preventDefault` (Js.cpp:257–265, 137–144) · `innerRML` getter (Js.cpp:172–179) · parent/child traversal · `queueMicrotask` (only implicit Promise jobs, Js.cpp:568–579; whether quickjs-ng ships it natively is unverified — *experiment "qjs-microtask": eval `typeof queueMicrotask` on the pinned tag*) · ES modules / `vfs://` (Js.cpp:517) · `console.*`→UE_LOG (stdio only, Js.cpp:324–337) · error overlay (stderr + exit code only, Js.cpp:47–67, main.cpp:200–201) · 16 MB cap · controlled GC · data-model access · gamepad/localization · `cancelAnimationFrame` (Js.cpp:418–424) · element identity (`===`) across lookups (Js.cpp:114–121).

## 10. Recorded gotchas (comments worth carrying into M4)

1. Shutdown ordering contract — Js.h:16–18 and main.cpp:192–193 ("event listeners self-release their JS callbacks on detach") — the origin of spec §7's shutdown clause; note it requires an extra `context->Update()` after `Close()` (main.cpp:195).
2. Dead-handle philosophy — Js.cpp:1–3: methods on dead handles return false/null, never throw.
3. Defensive listener sweep at shutdown "should be none if the document tree was destroyed first. Free their JS functions anyway" — Js.cpp:484–485.
4. rAF re-entrancy: swap the pending list so callbacks queued during the run go to the *next* frame — Js.cpp:531–534.
5. Timer re-entrancy pair: index loop because "callbacks may append new timers" (Js.cpp:544) and dup-before-call because "callback may clear itself" (Js.cpp:553).
6. `remove()` semantics ride on `RemoveChild`'s discarded return: "returned ElementPtr destroys the element" — Js.cpp:253.
7. Input pass-through: the key hook "never consume[s]; let the context see the event too" — main.cpp:46 — JS observes input without stealing it from RmlUi.
8. Pump contract in one line: "fires requestAnimationFrame callbacks, due timers and pending JS jobs. Call once per frame BEFORE Context::Update()" — Js.h:23–25 — matching the spec's frame-loop step exactly, including its rAF→timers→jobs order.
