# VaCuus M5 — Web-DX Tier 2 + flagship renderer: Implementation Plan

> **For agentic workers:** fresh subagent per task, two-stage review after each pair. `- [ ]`
> tracks steps.

**Spec:** `docs/superpowers/specs/2026-07-31-vacuus-m5-webdx.md` (v2 after a three-lens review;
§12 lists what v1 got wrong — several tasks below carry its restore-the-bug proofs).
**Ground truth:** `docs/research/m5-api-notes/*.md`.
**Environment:** `/w/Unreal/VcHost/Plugins/VaCuus`, branch `m5-webdx`. CLAUDE.md hazards apply.
Baseline: **132 tests**, all green. Implementers do not run `bd`.
**Environment correction to the spec:** node v26 + npm ARE available on this machine — E-P
experiments and bundle builds run in-loop; the committed-bundle + provenance-manifest strategy
(spec §2(l)) still ships so other machines/CI stay hermetic.

**Non-negotiables:** every RmlUi/JS call on the UI thread; the M3/M3b/M4 suites green at every
task boundary; tripwire discipline for every buffer change; no `rmlui_dynamic_cast` across
modules; comments cite source, every cited line opened.

---

### Task 1: E-P experiments + facade completion (Track P first half)

- [ ] **1.1** Build a stock-preact probe bundle (npx esbuild, preact from npm) into test content;
      run E-P1 (logging-proxy cold mount), E-P2 (event case + class path), E-P4 (style writes),
      E-P5 (keyed reorder needs nextSibling?) against the CURRENT facade in the M4 rig; record
      every observation in the report (the invalidation protocol: contradictions → spec errata).
- [ ] **1.2** Land the gaps the observations confirm: G1 text nodes (`createTextNode`,
      `nodeValue`/`data`, `textContent` if E-P3 shows need) **with the binding-scanner bypass**
      — set text directly on ElementText, never through RML re-parse; G2 traversal
      (`firstChild/lastChild/nextSibling/previousSibling`), G3 `childNodes` (unfiltered), G4
      `nodeType` (1/3/9), `localName`; style camelCase→kebab (+`px` per E-P4); `replaceChild`
      per E-P1.
- [ ] **1.3** Tests: per-gap; **brace injection** — a text node written `{{Health}}` renders
      literal braces **(restore: route through RML parse → the binding evaluates — observed)**;
      E-P3 (counter text updates) against the landed G1.
- [ ] **1.4** Commit: `feat: the facade Preact needs — text nodes that cannot be injected`

### Task 2: Glass — recorder, buffer, tripwires (spec §2(d))

- [ ] **2.1** The seven virtuals recorded (blur-only CompileFilter; every other filter type →
      handle 0 + the named supported-types log); PushLayer/CompositeLayers/PopLayer +
      clip-mask commands; `NewFilters`/`ReleasedFilters`; member counts, `HasResourceTraffic()`,
      hash (variable-length filter records per the header's provision).
- [ ] **2.2** Replayer: skip rules (clip-mask outside glass; layer ops pass-through) — behavior
      today preserved for non-glass documents, asserted.
- [ ] **2.3** Tests: recorder units per virtual; the tripwire restore-the-bug (drop a hash field
      → a changed frame withheld — observed); non-blur refusal observed; M3b idle rows still
      exact.
- [ ] **2.4** Commit: `feat: the recorder learns layers and filters — blur only, loudly`

### Task 3: Glass — the composite-time pipeline + demo

- [ ] **3.1** `VaCuusBlur.usf` + `VaCuusGlassPS`; the glass-list distiller (wholesale
      replacement per publish); the per-element pooled half-res RT; the engine-frame passes
      (downsample sampling OutputTexture → blur ping-pong → geometry-masked glass draw) before
      the existing composite; **the coordinate mapping** (DestRect.Min + ElementsOffset offset,
      DestRect/ViewSize scale on regions, mask geometry and σ, SceneViewRect clamp).
- [ ] **3.2** Exp-GLASS-SCENE-CONTENT, -BACKBUFFER-SRV (fallback copy if Vulkan refuses),
      -HDR-DETECT (disable glass), -GAMMA-BLUR (A/B image). GlassMs stat.
- [ ] **3.3** Tests: presence pixel-variance; **removal** (show → publish → remove/unload →
      publish → region matches sharp scene) **(restore: distiller skips glass-free buffers →
      stale blur — observed)**; PIE-shaped mapping (nonzero DestRect.Min); idle-freeze
      restore-the-bug both outcomes; `vacuus.M5Glass` demo + AutoShot.
- [ ] **3.4** Commit: `feat: glass — the scene shows through, blurred, live`

### Task 4: Decorators stage 1 — shader plumbing + gradients (spec §2(e))

- [ ] **4.1** `NewShaders`/`ReleasedShaders` + `DrawShader` (first mid-pass PSO switch) + the
      tripwire/hash discipline; gradient PS (linear/radial/conic) + a builtin set behind
      `shader(<name>)` incl. `glass-panel`; unknown-key named log.
- [ ] **4.2** Tests: gradients vs reference screenshots; unknown key; the PerfLog soak method
      for the Record/Replay budget row (decorators present vs absent — the probe harness cannot
      see Record/Replay).
- [ ] **4.3** Commit: `feat: gradients and builtin shaders — the guaranteed tier`

### Task 5: Decorators stage 2 — the material spike + the gate decision

- [ ] **5.1** Timeboxed per material-decorators.md §6 with the spec §2(f) corrections: day 1
      one MD_UI material drawn in the replay pass (plugin FMaterialShader pair, synthetic view
      UB); day 2 the contract (blend matrix over text, MID params per frame, **the freeze
      remedy priced** — forced-republish vs composite-time draw, measured; monolithic `-game`
      — load-bearing; Vulkan).
- [ ] **5.2** The gate decision recorded in spec §3.3's outcome note with screenshots + numbers.
      GO ⇒ `UVaCuusStyleSet` + snapshot version-counter discipline + registry; FALLBACK ⇒
      builtin tier + marketing note, plumbing kept.
- [ ] **5.3** Commit: `feat|spike: the material gate — <GO|FALLBACK>, priced and recorded`

### Task 6: World-space — sink refactor, component, RT bridge

- [ ] **6.1** `IVaCuusFrameSink` extraction (Slate element implements unchanged);
      `UVaCuusWorldComponent` (VaCuusRender): quad proxy + body setup cloned, `DrawSize` up
      front, **zero-size named refusal at OnRegister**; `FVaCuusWorldSink` arrival-driven replay
      + copy-on-publish with **the destination-slot discipline** (game-thread-enqueued slot
      updates FIFO with re-init; extent guard; self-healing skip).
- [ ] **6.2** `M_VaCuusWorldPanel` preset (AlphaComposite, responsive AA, WS-GAMMA decode A/B,
      two-sided switch).
- [ ] **6.3** Tests: zero-size refusal; resize-race (rapid DrawSize churn under publish load —
      no crash, self-heals); WS-COPY-COST measured.
- [ ] **6.4** Commit: `feat: the HUD on a quad — one copy per publish, none when idle`

### Task 7: World-space input + demo

- [ ] **7.1** `FVaCuusWorldInputProcessor` on `UVaCuusWorldSubsystem` (refcount register): **the
      occlusion rule** (no Slate capture; widget path under cursor terminates at the game
      viewport), deproject → hit → `GetLocalHitLocation` math → SendInput; snapshot consume;
      capture latch; MouseLeave; WS-STALE-RAY decision.
- [ ] **7.2** Tests: PIE click → the M4 write router fires (full stack); **occlusion** (Slate
      widget over the panel wins); **pass-through** (test-pawn click counter increments on a
      miss); MouseLeave un-hovers; WS-GAMMA parity screenshot.
- [ ] **7.3** Commit: `feat: raycast input that defers to Slate`

### Task 8: `@vacuus/preact`, the CLI, typings, localization (Track P second half)

- [ ] **8.1** `Web/packages/{preact-vacuus,cli}` (source-only, Fab rules): the fork patches per
      E-P observations (event case; options pinned); `create`/`dev`/`build`/`symbolicate`; base
      stylesheet + linter rules; **the typings manifest pipeline** (extract from `vacuus.d.ts`,
      commit beside test content); **the localization hook** (`vacuus.translate` + handler seam
      + template routing + no-handler Verbose).
- [ ] **8.2** Node-side smoke (runs in-loop here): linter fixtures (bad fires / clean silent);
      manifest staleness check; the E-P6/E-P7 experiments (desync observability; reload
      re-mount) against the fork.
- [ ] **8.3** The committed demo bundle + provenance manifest; the in-engine conformance test
      (both-ways walk vs the manifest) + provenance skip-warning behavior; JsPump re-measured
      on the TSX HUD.
- [ ] **8.4** Commit: `feat: @vacuus/preact and the CLI — TSX over the facade, honestly typed`

### Task 9: The M5 demo, budgets, acceptance, merge

- [ ] **9.1** `vacuus.M5Demo` per spec §8 (TSX HUD + glass + decorators + world quad +
      localized string); AutoShot; the §6 table measured and written into the spec.
- [ ] **9.2** Full suite green; monolithic build; SIGTERM teardown with all tracks live.
- [ ] **9.3** Arch-spec amendments (§10's five corrections) committed.
- [ ] **9.4** Merge `m5-webdx` → master; close beads.

---

## Acceptance

1. TSX HUD via the CLI runs in PIE (committed bundle with recorded provenance).
2. The glass demo shows the moving scene blurred through a panel — and stops showing it when
   the panel goes.
3. A world-space panel is clickable by raycast, defers to overlaid Slate, and passes misses
   through to the game.
4. The material gate decision is recorded with numbers either way.
5. Every §7 spec test exists and has been seen to fail where marked; the M3a/M3b/M4 suites
   never broke.
