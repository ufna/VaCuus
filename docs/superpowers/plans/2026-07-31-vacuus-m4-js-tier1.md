# VaCuus M4 — JS Tier 1: Implementation Plan

> **For agentic workers:** execute task-by-task with a fresh subagent per task and a two-stage
> review (spec compliance, then code quality) after each. Steps use `- [ ]` for tracking.

**Goal:** quickjs-ng on the UI thread behind the script-host seam; the Tier-1 DOM/globals
surface; documents, modules, errors; the write router and the model read surface; the demo port
with parity and the GC numbers.

**Spec:** `docs/superpowers/specs/2026-07-31-vacuus-m4-js-tier1.md` (v2, after a three-lens
adversarial review returned NEEDS REWORK on v1 with twelve blocking findings — §12 lists them;
several of the tests below are their restore-the-bug proofs).
**Ground truth:** `docs/research/m4-api-notes/*.md`. quickjs-ng sources to vendor sit in the
session scratchpad clone (tag v0.15.1, commit fd0a0210…); if absent, re-clone at that exact tag.

**Environment:** work in `/w/Unreal/VcHost/Plugins/VaCuus` on branch `m4-js`. Build/test commands
and the dev-loop hazards are in `CLAUDE.md` — read before the first build. Baseline: **80 tests**,
all green. Implementers do **not** run `bd`; the controller tracks issues.

**Non-negotiables, from the spec:**
- All JS on the UI thread: creation, evals, callbacks, finalizers, GC (§4). Inline mode refreshes
  `JS_UpdateStackTop` per `RunFrameInline` entry.
- The seam skips cleanly when no host is registered — M3 configurations byte-identical; the
  M3a/M3b suites keep passing untouched at every task boundary.
- Script execution is **host-ordered** (§2(f)) — never from `Plugin::OnDocumentLoad`.
- Every JS entry boundary arms the watchdog and pairs the uncatchable reset (§3.3).
- Listener registry + neuter protocol for all **three** death orders (§2(g)).
- Leak observables are host-side counters (`check`, Development-real) — never the library's
  NDEBUG-dead assert (§5).
- Comments cite source as `file:line`, every cited line opened.

---

### Task 1: Vendor quickjs-ng + the VaCuusJs module skeleton

- [ ] **1.1** `Source/ThirdParty/quickjs-ng/`: the four .c, all project headers, checked-in
      `builtin-*.h`, `LICENSE`, `VENDORED_TAG.txt` (tag + commit + patch log). **Patch #1**:
      neutralize the GNU-like `JS_EXTERN` branch (spec §2(a)) — record the diff in the tag file.
- [ ] **1.2** `VaCuusJs` module: Build.cs per spec §3.2 (VaCuusRml preamble, `CStandard = C11`,
      relay `.c` files in `Private/Gen/`, the platform defines); `LogVaCuusJS` declare/define
      pair; module added to the `.uplugin` (Runtime, after VaCuus).
- [ ] **1.3** Vendoring smoke test (automation): `JS_NewRuntime`/eval `1+1`/free; version string
      equals `0.15.1`; `typeof queueMicrotask === 'function'` (core builtin — the test guards
      against future shadowing); the E3 BigInt probe recorded in the test's log output.
- [ ] **1.4** Build-time `nm -D` check on Linux modular builds: no `JS_` exports (a small
      post-build script or a test-time assertion — pick what UBT allows; document the choice).
- [ ] **1.5** Commit: `feat: vendor quickjs-ng v0.15.1 — four files, one patch, no exports`

### Task 2: Runtime core + the seam + the pump phase

- [ ] **2.1** `IVaCuusScriptHost` in core (spec §3.1) + `FVaCuusModule::SetScriptHostFactory`
      (refused after thread boot — the SetRenderInterface shape); the RunFrame pump phase between
      DataApply and the record loop, ungated, skipped when no host; `JsPump`/`JsGC` scopes +
      PerfLog names **declared first** (the DataApply playbook); `CollectGarbage` call at end of
      RunFrame.
- [ ] **2.2** `FVaCuusJsRuntime`: `JS_NewRuntime2` over FMemory hooks + atomic live-byte counter
      + `LLM_SCOPE`; `JS_SetMemoryLimit` (cvar); `JS_SetMaxStackSize(256KB)`;
      `JS_SetGCThreshold(-1)`; watchdog handler + the per-entry arm/reset helper (spec §3.3);
      rejection tracker registered; fired-counters (timers/rAF/jobs/collections/errors).
- [ ] **2.3** `GVaCuusUIThreadStackSize` → 2 MB (update the comment's promise); inline-mode
      `JS_UpdateStackTop` per entry.
- [ ] **2.4** GC point: trigger on the live-byte delta (cvar step), OOM fallback; heap +
      collections on the PerfLog window line.
- [ ] **2.5** Tests: no implicit collections while allocating hard (collections counter 0, bytes
      climbing); OOM at a tiny cap → InternalError + fallback counted (E5); watchdog trips in
      ExecuteScript-shaped eval **(restore: disable → timeout)**; deep recursion → RangeError
      (stack-headroom); the FMemory-delta observable for the malloc hooks.
- [ ] **2.6** Commit: `feat: FVaCuusJsRuntime behind IVaCuusScriptHost — capped, watched, counted`

### Task 3: Timers, rAF, console, the bounded drain

- [ ] **3.1** Per-view `FVaCuusJsViewContext` (created on demand; `document === null` before a
      document); console.* → `UE_LOG(LogVaCuusJS)`; timers (frame-start cutoff, interval re-arm
      from fire time, clear APIs); rAF (swap-out list, `GetElapsedTime()` ms timestamp,
      cancelAnimationFrame); job drain to the per-pump cap (cvar, Error at the cap).
- [ ] **3.2** Tests: rAF next-frame semantics; 0 ms self-rearm **(restore: drop cutoff →
      timeout)**; self-requeuing microtask **(restore: drop cap → timeout)**; promise-in-rAF
      resumes after this frame's timers (ordering pinned); sibling survives a throw; cancel APIs;
      fired-counter deltas exact.
- [ ] **3.3** Commit: `feat: the pump — rAF, timers, a bounded drain, and counters that fire`

### Task 4: The DOM facade — elements

- [ ] **4.1** Wrapper class (ObserverPtr opaque, finalizer), per-view identity cache (lazy wrap,
      `OnElementDestroy` pointer-probe erase across views); `document.createElement` (lowercased),
      `getElementById`, `querySelector(All)`/`closest` (deviations documented in comments),
      `appendChild`/`insertBefore`/`remove`/`removeChild` (ElementPtr ownership per spec §3.9),
      `id`/`tagName`/`parentNode`/`children`, `innerRML` get/set, attributes, classList (never
      the class attribute), style proxy (copy-at-once get).
- [ ] **4.2** Tests: identity `===`; dead-handle nulls; `createElement("DIV")` matches `div` RCSS
      **(restore: drop lowercase → style absent)**; classList vs `GetAttribute("class")`
      staleness observed; style get-copies; querySelector deviations; innerRML set kills handles
      synchronously; two-view isolation (each context sees only its document).
- [ ] **4.3** Commit: `feat: the element facade — one wrapper per element, dead handles read null`

### Task 5: Events

- [ ] **5.1** Listener registry (per view, module-owned) + `JsEventListener` (ref release on
      detach, neuter protocol); `add/removeEventListener` with capture; the event object
      (type/target/currentTarget/phase/parameters/stop*/preventDefault→stopPropagation);
      `dispatchEvent`; the `on*` attribute instancer (Factory hook) compiling against the view
      context.
- [ ] **5.2** Tests: all **three** death orders release refs — counters as observable
      **(restore: release in one hook only → other paths' counters climb)**; capture flag; a
      synthesized gamepad event (M2's path) reaches a JS listener; keydown parameters; `onclick`
      attribute fires; dispatchEvent round-trip.
- [ ] **5.3** Commit: `feat: events — one registry, three death orders, zero leaked refs`

### Task 6: Documents, ExecuteScript, reload

- [ ] **6.1** `FVaCuusJsDocument` + document instancer (script capture at `LoadInlineScript`/
      `LoadExternalScript` with source lines); `IVaCuusScriptHost::OnDocumentReady` invoked from
      `AdoptDocument` **after** old-close and `Show()` (the §2(f) order); `OnDocumentClosing`
      dispatches unload JS at `Close()` time; context recycle on replace; watchdog trip
      mid-scripts skips the rest with the counted Error; missing `<script src>` → named Error,
      later scripts run.
- [ ] **6.2** `ExecuteScript`: command kind + enqueue + `UVaCuusView::ExecuteScript`
      (BlueprintCallable); unknown-view drop at Error; FIFO-after-LoadDocument.
- [ ] **6.3** Tests: head scripts run against a shown body; body `<script>` inert;
      **reload recycles** — old timer never fires, scripts re-ran fresh **(restore: run scripts
      from Plugin::OnDocumentLoad → reload leaves JS dead — v1's design bug, observed)**;
      `document === null` pre-document; unload JS observed before close.
- [ ] **6.4** Commit: `feat: documents — captured scripts, host-ordered execution, honest reload`

### Task 7: Modules and the VFS

- [ ] **7.1** Module loader (normalize strips `vfs://`, resolves relative-to-importer then
      document roots, reads via `IPlatformFile`); TLA refusal (pending promise after drain →
      named Error); missing import → loader NULL + our Error; module cache dies with the context;
      live-reload watch extensions gain `.js`/`.mjs`.
- [ ] **7.2** Tests: import chain; `vfs://` stripped; TLA refused (E1); module throw → rejection
      tracker; missing import both Errors; a `.js` edit triggers the reload path (editor test,
      the M2 watcher harness).
- [ ] **7.3** Commit: `feat: ES modules over the VFS — vfs:// stripped, TLA refused`

### Task 8: Errors — overlay and tracker

- [ ] **8.1** Error routing (message + stack + source) to `LogVaCuusJS`; the dev overlay element
      (cvar, non-shipping) with the **retract path** on `is_handled=true` re-fires; the total
      error counter.
- [ ] **8.2** Tests: overlay shows an uncaught throw; a later-handled rejection retracts
      (overlay state observed); every M4 test that expects no errors asserts the counter.
- [ ] **8.3** Commit: `feat: errors that show themselves — and retract when handled`

### Task 9: The write router and the read surface

- [ ] **9.1** Core seam on `FVaCuusScalarDefinition::Set` (ViewId stamping at BindModel-drain,
      definition→model back-pointer, DiagnosticPath→(Model, Path) trim); bounded game-thread
      queue + drain in `UVaCuusSubsystem::Tick` → `OnModelWrite`/`OnJsEvent`; **the
      revert-dirty** (force-dirty the touched top-level name from the shadow on the next apply);
      zero-handler Warning; refusal-log rewording with a router present; `vacuus.emit`.
- [ ] **9.2** `vacuus.model(name).get(path)` over the UI shadow (per-kind projections, array
      length/index); `vacuus.view`, `vacuus.stats()`.
- [ ] **9.3** Tests: checkbox → `OnModelWrite` payload; **control reverts** while the model is
      unchanged **(restore: drop the revert-dirty → checkbox stays toggled — v1's divergence)**;
      shadow byte-identical + element reads; router-absent ⇒ M3 byte-identical (the M3a/M3b
      suites are the proof); overflow drop diagnostic; read surface per kind; unknown model/path.
- [ ] **9.4** Commit: `feat: writes route to the game, reads come from the shadow`

### Task 10: The demo port, the churn workload, the numbers

- [ ] **10.1** `m4_demo.rml` + `hud_logic.js` per spec §9 (serial-deterministic); `vacuus.M4Demo`
      + `Freeze`; AutoShot screenshots at computable beats vs the C++ driver's states.
- [ ] **10.2** The churn workload: the 200-row killfeed fixture document driven from JS at M3b's
      rates — the GC population.
- [ ] **10.3** Measure §7's table: idle pump; steady pump; **the combined row** (p99 of
      JsPump+JsGC+Update+Record ≤ 0.50 ms, collections in population); GC pause p50/p99; heap;
      facade op costs; the idle row via fired-counters (exact). Report via AddInfo + UE_LOG; the
      controller writes numbers into spec §7.
- [ ] **10.4** Commit: `feat: the demo speaks JavaScript — and the numbers are in`

### Task 11: Acceptance

- [ ] **11.1** Full suite green (baseline 80 + all new), counts from the log.
- [ ] **11.2** Monolithic `VcHost` build.
- [ ] **11.3** Teardown runs: graceful SIGTERM with live JS (unload observed, counters zero) and
      hard-stop; ENABLE_DUMPS leak check in the test config.
- [ ] **11.4** Spec §7 numbers recorded; parity screenshots eyeballed; arch-spec amendment
      cross-checked.
- [ ] **11.5** Merge `m4-js` → master; close beads.

---

## Acceptance

1. The demo HUD runs its logic in JS with parity screenshots, while gameplay data still flows
   through binding.
2. Every §8 spec test exists and has been seen to fail where marked.
3. The combined UI-frame gate holds with GC in the population; the GC numbers are recorded.
4. Idle is exact zeros across all counters, JS included.
5. M3 configurations are byte-identical with no script host registered; the M3a/M3b suites never
   broke.
