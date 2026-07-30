# VaCuus v1 — Architecture Design Spec

Date: 2026-07-29 · Status: DRAFT v2 (adversarial self-review applied: 35 findings resolved)
Owner: ufna · Decision record: `docs/research/2026-07-29-webui-middleware.md` (DECISION block)

## 1. Overview

VaCuus is an Unreal Engine plugin providing Gameface-class HTML/web game UI for PC
(Win64 / Linux / macOS), built on **RmlUi 6.x + QuickJS-ng** with a custom render backend
over UE RHI. Sold on Fab (Personal/Professional tiers).

**Core promise:** the UI never stalls the game or render thread. All document, style,
layout, script, and paint-command work runs on a dedicated UI thread; the game thread only
enqueues input/data; the render thread only replays pre-built command lists.

**Product promises** (evidence basis in parentheses; budgets formalized in §11):
- ≤0.5 ms/frame UI-thread cost for the reference HUD (measured: 0.07–0.12 ms
  `Context::Update` + ~0.15 ms command generation on the research bench workload).
- ~20–32 MB RAM budget for a typical UI (measured: ~20 MB incl. fonts).
- <10 MB added to a shipped build (RmlUi 3.8 MB + QuickJS ~1 MB + plugin code).
- Authoring: TSX + hooks + npm toolchain (`@vacuus/preact`), or pure C++/Blueprint data
  binding with zero JS.
- Flagship renderer feature (proven): **backdrop-filter glass over the 3D scene** (LDR
  path; RmlUi ships backdrop-filter natively, scene capture design in §5).
- Flagship renderer feature (**feasibility-gated at M5**): UE Materials as CSS shader
  decorators; guaranteed baseline regardless of gate outcome: built-in shader decorator
  set (gradients, blur, masks) — see §5 and risk table §13.

**Non-goals for v1 (explicit):**
- Not a browser: no arbitrary-website rendering, no `react-dom`, no networking stack in
  the UI runtime (no fetch/XHR/WebSocket in v1), no video element.
- No CSS Grid (matches Gameface's flex-only market bar; candidate upstream contribution).
- No BiDi/complex-script shaping in v1.0 (HarfBuzz backend planned v1.x).
- No console platforms in v1 (design must not preclude them: interpreter-only JS,
  no subprocesses, no runtime codegen).
- Pre-tonemap/HDR-linear composition path deferred to v1.x (§5); v1 glass is LDR.

## 2. Third-party & licensing

| Component | Version policy | License | Vendoring |
|---|---|---|---|
| RmlUi | pin 6.x release; fork `ufna/RmlUi` tracking upstream | MIT | `Source/ThirdParty/RmlUi` (source) |
| quickjs-ng | pin latest tagged release (0.15.1 as of 2026-06; record exact tag at M4) | MIT | `Source/ThirdParty/QuickJS` (source) |
| FreeType | UE's bundled FreeType (engine module dependency) | FTL (attribution shipped) | engine dependency |
| HarfBuzz (v1.x) | UE's bundled | MIT-like | engine dependency |
| Preact (patched) | `@vacuus/preact` npm fork | MIT | `Web/` (source only) |

Fab compliance (rules verified in research: source-shipping mandate 4.3.6.1.a, `.exe`/
`.msi` ban 4.3.6.1.e, license stack, size caps): all UE-facing modules ship as source
(mandatory); ThirdParty is MIT source — no closed binaries, no executables. **`Web/`
ships source-only** (package.json + sources; no `node_modules`, no tool binaries —
esbuild/preact are fetched from npm on the buyer's machine); the M6 packaging dry-run
includes a no-executables scan of the zip. Third-party disclosure procedure (declaration
form): confirm exact mechanism during the M6 dry-run. NOTICE files preserved for all
components. Plugin's own license: Fab Standard License on Fab copies (per-seat);
direct-sales license TBD by owner (open decision, non-blocking).

Bus-factor mitigation: maintain the RmlUi fork with upstream tracking; sponsor the
upstream maintainer; upstream fixes we make (candidates: CSS Grid, box-shadow animation).

## 3. Module map

```
VaCuus.uplugin
├─ VaCuus         (Runtime)  UI thread, contexts, VFS, fonts, data binding, public API
├─ VaCuusRender   (Runtime)  Rml::RenderInterface impl, command lists, RHI replay,
│                            shaders/materials, scene composition
├─ VaCuusJs       (Runtime)  QuickJS runtime, DOM facade bindings, timers/rAF, console
├─ VaCuusUMG      (Runtime)  UVaCuusWidget/SVaCuusWidget, UVaCuusWorldComponent,
│                            input routing, IME, gamepad/spatial nav
├─ VaCuusEditor   (Editor)   asset import/cook, file watcher + live reload, dev panel
└─ Web/                      npm workspace: @vacuus/preact, @vacuus/cli (esbuild template,
                             TSX, sourcemaps, error overlay), TS typings for the JS surface
```

Dependency rule: `VaCuus` has no dependency on the other runtime modules.
`VaCuusJs`/`VaCuusRender`/`VaCuusUMG` register implementations of core-defined interfaces
at module startup (`IVaCuusScriptHost`, RenderInterface factory, input sink); frame-loop
steps with no registered implementation are skipped (e.g., JS-off configuration).
JS can be **disabled at runtime** in v1 (QuickJS never initialized via config flag);
build-level stripping (removing the module) is documented as a manual `.uplugin` edit and
formalized in v1.x.

## 4. Threading model

**Ownership.** One `FVaCuusUIThread` (FRunnable) per `UVaCuusSubsystem`
(UGameInstanceSubsystem — one per PIE instance / game instance). The UI thread exclusively
owns: all `Rml::Context`s, all documents, the QuickJS `JSRuntime/JSContext`, font engine,
and the data-model registry. **No RmlUi or QuickJS API is ever called from any other
thread** (both libraries are single-thread-affine; enforced by `check(IsInUIThread())`
wrappers in debug).

**Game thread → UI thread** (all non-blocking):
- Input event ring buffer (mouse/key/touch/gamepad/IME composition events, timestamped).
- Data-binding snapshots — single protocol, defined in §6, referenced here.
- Command queue: LoadDocument/Unload/Show/Hide/Resize/SetVisibility/ExecuteScript/Reload.

**UI thread → game thread:** alongside each published command buffer, the UI thread
publishes an **interactive-region snapshot** (screen rects of interactive elements +
pass-through regions). This is a *required* component: Slate demands a synchronous
Handled/Unhandled decision on the game thread (§8), and this stale-by-one-UI-frame
snapshot is what answers it.

**UI thread frame loop.** The UI thread runs its own loop; default mode: triggered once
per game frame (triggers arriving while a UI frame is in flight are coalesced); optional
fixed-rate mode with configurable max Hz.
1. Drain command queue; drain input ring → `Context::Process*`.
2. Consume data snapshots (§6) → `DirtyVariable` per changed field.
3. Pump JS: `requestAnimationFrame`, expired timers, `JS_ExecutePendingJob`.
4. `Context::Update()`.
5. `Context::Render()` into a **command recorder** (API-agnostic commands + geometry/
   texture handles; no RHI calls here).
6. Publish the completed command buffer + interactive-region snapshot (triple-buffered;
   publish = atomic pointer swap; refines the decision record's "double-buffered" wording
   — the third buffer removes the only remaining producer/consumer contention point).

**Render thread — RT-based replay model (single source of truth, also §5):** replay
always renders into a **persistent per-view offscreen render target**. The Slate custom
element / world-space material composites that RT every engine frame (one blit/sample).
Arrival of a new command buffer triggers re-replay into the RT; otherwise the RT is
reused as-is — an idle UI costs one composite, zero replay. The render thread **never
waits** on the UI thread. Re-replay of the *same* buffer happens only on RT loss/resize.

**Resource lifetime.** Compiled geometry and textures are handle-based. Creation requests
flow UI→render thread with the command buffer; releases are deferred until the last
command buffer referencing the handle has been replayed (generation counter per buffer).

**Shutdown** (order verified by the HUD demo): stop accepting commands → drain UI thread
→ close/unload all documents (**while JS is still alive** — element detach releases JS
event-listener refs) → destroy the JS runtime → destroy Rml contexts / `Rml::Shutdown` →
flush render-side handles. Resize is a queued command (re-layout on UI thread; RT resize
on render thread at next replay).

**Explicit contrast with Gameface:** no end-of-`World::Tick` fence; input and model sync
are legal at any point of the game frame; the game thread's total cost is queue writes.

## 5. Rendering

**RenderInterface mapping (UI thread, record-only):**
- `CompileGeometry` → persistent VB/IB handle (created on render thread, reused across
  frames — RmlUi retains compiled geometry; measured 479 draws / 0 compiles at steady
  state).
- `RenderGeometry(handle, translation, texture)` → draw command.
- Scissor/clip-mask/transform → state commands. Layers (`PushLayer/PopLayer/
  CompositeLayers`, filters, `SaveLayerAsTexture`) → offscreen RT pool commands.
- `LoadTexture`: RmlUi's contract is synchronous for dimensions (out-parameter needed for
  layout) — the image **header is probed synchronously** on the UI thread; pixel decode
  runs on a task and upload lands via `RHIAsyncCreateTexture2D`. Draws referencing a
  not-yet-ready texture render fully transparent until the upload completes.

**Replay (render thread):** UE global shaders ported from RmlUi's reference GL3 backend
(color/texture, linear/radial/conic gradient, blur chain for filter/box-shadow/backdrop,
mask). Premultiplied-alpha blend states throughout (`BF_One, BF_InverseSourceAlpha`).
Formats: geometry textures `PF_B8G8R8A8`; per-view output RT and layer pool
`PF_B8G8R8A8` in the display-gamma path, float/`RGB10A2`-class formats reserved for the
deferred linear-HDR path (v1.x).

**Composition paths:**
1. **Screen-space HUD/menus (v1 default):** the per-view RT (see §4 replay model) is
   composited by `ICustomSlateElement` (RDG `Draw_RenderThread`) submitted from
   `SVaCuusWidget::OnPaint` via `FSlateDrawElement::MakeCustom` — after tonemap,
   display-gamma space. The RDG API exists in all supported engine versions (5.6–5.8);
   no pre-RDG legacy path is built.
2. **World-space (v1):** the same per-view RT sampled by a translucent material on
   `UVaCuusWorldComponent`; input via raycast → texture-space coords. TAA/TSR ghosting
   risk documented (§13): shipped material presets use responsive-AA flags; docs
   recommend post-AA compositing or FXAA for heavy world-space UI.
3. **Pre-tonemap linear-HDR overlay: deferred to v1.x.** The shader permutation seams
   (gamma-neutral outputs) are kept in v1 code, but no HDR-linear path ships or is
   tested in v1.0.

**Gamma:** v1 ships the display-gamma LDR path plus the `PF_FloatRGBA` editor/PIE
composition permutation (gamma 1.0) — both required for correct editor rendering.

**Shader decorators → UE Materials (feasibility-gated flagship):**
`decorator: shader("<name-or-asset-path>")` resolves via a registry: built-in names →
our global shaders; `/Game/...` paths → `UMaterialInterface` (UI material domain) drawn
as quads inside the replay pass with RCSS/data-binding-fed parameters. Materials are
declared in a `UVaCuusStyleSet` asset (cooked, preloaded — no sync loads on the UI
thread). **Gate: M5 opens with a 2-day spike; fallback if it fails: built-in shader set
only for v1.0, and the §1 promise is downgraded accordingly (marketing consequence noted
in §13).**

**Backdrop-filter over the 3D scene (v1, LDR):** for root-level elements with
`backdrop-filter`, the composite step captures scene color **from
`FDrawPassInputs::OutputTexture`** (an RDG copy taken before the UI draws — at Slate
custom-element time the output texture already contains the tonemapped scene). The copy
is downsampled + blurred (separable, half-res) into a pooled RT sampled by panel
background shaders. Note (verified against 5.8 source): `ESlatePostRT` post buffers are
NOT readable from a custom Slate element's inputs — the engine's post-buffer mechanism
requires the `SPostBufferUpdate` widget pattern; we therefore use the OutputTexture-copy
approach and do not depend on Slate post buffers. In-page backdrop between UI layers uses
RmlUi's native layer compositing (proven in the demo).

**Idle cost model:** UI publishes nothing → render thread reuses the per-view RT →
per-frame cost is one composite of the RT. Dirty-region partial replay is a v1.x
optimization.

## 6. Data binding

- `FVaCuusModelBuilder`: walks UPROPERTY reflection of a USTRUCT/UObject and constructs a
  matching Rml data model (scalars, FString/FText, TArray, nested structs; unsupported
  property types produce a build-time log listing, never silent).
- **Snapshot protocol (the single definition; §4 refers here).** Single-writer (game
  thread) / single-reader (UI thread) per model. Two value slots + dirty bitset. Game
  thread writes the back slot (POD memcpy + string/array copy for dirty fields only) and
  publishes atomically (slot index + version counter). Repeated updates before
  consumption coalesce: dirty bits OR-accumulate into the unpublished slot. UI thread
  consumes at frame-loop step 2, calling `DirtyVariable` only for set bits (measured
  RmlUi cost: 0.7 µs/variable).
- API: `UVaCuusView::UpdateModel(Struct)` (copy + diff → dirty bits) or fine-grained
  `MarkDirty(FieldPath)`. Blueprint nodes mirror C++ (Gameface-style ergonomics:
  `CreateModelFromStruct`, `UpdateWholeModel`, batched sync once per frame).
- JS sees the same models through RmlUi data-binding expressions; JS-driven VDOM (Tier 2)
  and data models coexist but don't compose on the same subtree (documented rule, from
  RmlUi's own constraints).

## 7. JS runtime & web-DX

**Engine:** quickjs-ng (pinned), one `JSRuntime`+`JSContext` per subsystem, living on the
UI thread. Interpreter-only (console-friendly). Default JS memory cap **16 MB**
(configurable; chosen to fit inside the §11 RAM gate — the cap is a hard ceiling, the
reference HUD is expected well under it). GC: quickjs-ng collection is stop-the-world —
a full GC is triggered at a controlled point of the frame loop by threshold/idle
heuristics, and its pause **counts against the UI-thread budget** (measured on the
reference HUD).

**Tier 1 — core surface** (v1; hardened from the demo's 657-LOC prototype):
- DOM facade over `Rml::Element` (via `Rml::ObserverPtr` — dead handles are safe):
  create/append/insert/remove, attributes, classList, style proxy, `querySelector(All)`,
  events with capture/bubble, `innerRML`.
- Globals: `setTimeout/setInterval/clearTimeout/clearInterval`, `requestAnimationFrame`,
  `queueMicrotask`, `console.*` → `UE_LOG(LogVaCuusJS)`, ES modules via the VFS
  (`vfs://` scheme).
- `vacuus.*` host API: view/document management, data-model access, key/gamepad
  callbacks, perf stats, localization hook.
- Error handling: exceptions → on-screen dev overlay (dev builds) + log with sourcemap
  resolution (Tier 2 supplies maps).

**Tier 2 — web-DX layer** (v1):
- `@vacuus/preact`: patched Preact + `preact/compat` running against the DOM facade
  (undom contract; precedent: OneJS/Unity — Preact over a retained-mode UI toolkit on
  QuickJS).
- `@vacuus/cli`: project template — TSX, esbuild bundling, TS typings for `vacuus.*`,
  sourcemaps, dev error overlay, watch mode wired to the editor's live-reload (§9).
- Positioning language (docs/marketing): "React-style components + game CSS" — never
  "runs your website".

**Debugging v1:** console + error overlay + RmlUi's built-in visual debugger/inspector
(element tree, box model, data-model inspector). CDP/DAP bridge for breakpoints is v2
(research flag: quickjs-ng CDP adapters unverified).

## 8. Input, IME, gamepad

- `SVaCuusWidget` implements the FReply route: mouse (capture on drag), keyboard (focus),
  touch. The synchronous Handled/Unhandled decision on the game thread is made against
  the UI thread's published **interactive-region snapshot** (§4) — stale by at most one
  UI frame; events are then timestamp-queued to the UI thread where authoritative RmlUi
  hit-testing runs. Cursor shape comes from the same snapshot.
- Per-element pass-through: elements marked `data-vacuus-passthrough` (and the document
  body by default) contribute pass-through regions to the snapshot — input over them
  falls through to the game. Gameface-parity ergonomics without a full-screen
  input-stealing widget.
- World-space: `IInputProcessor` forwards when a `UVaCuusWorldComponent` is under the
  interaction ray.
- Gamepad: buttons/axes forwarded as events; navigation via RmlUi's native spatial nav
  (`nav-*` RCSS properties, 6.0); optional `FAnalogCursor` mode.
- IME: `ITextInputMethodContext` implementation following the engine's CEF handler
  pattern (caret rect from RmlUi selection, composition events queued to UI thread).

## 9. Content pipeline

**Dual mode:**
- **Dev (editor/PIE):** loose files under `<Project>/Content/UI/**` (rml/rcss/js/ttf/png),
  directory watcher → live document reload (built in **M2**); `@vacuus/cli watch`
  rebuilds TSX bundles into the same tree (M5).
- **Shipping:** `UVaCuusBundle` asset — path-indexed archive (bulk data) built from the
  UI source tree at cook time by `VaCuusEditor`; VFS reads memory-mapped bundle entries.
  Built and gated in **M6** ("reference HUD runs from cooked bundle in a shipping
  build"). Guarantees cook correctness without loose-file staging rules in the consuming
  project.

Fonts: registered per style set; loaded on UI thread at init. Font-**effect** glyph
generation (glow/outline) is a measured spike risk (up to 32.5 ms for large glyph sets):
v1 mitigations — effect-glyph warm-up during document load (before the view is shown) +
CLI linter warning on effect-heavy styles; fully async glyph generation is v1.x (§13).
Textures: png/dds via UE image wrappers on a task, uploaded async.

## 10. Public API surface (game-facing)

- `UVaCuusSubsystem` (GameInstance): view factory, global config, stats.
- `UVaCuusView` (UObject handle): document lifecycle, model API, script eval, visibility,
  input policy. Owns nothing thread-affine — a proxy that posts commands.
- `UVaCuusWidget` (UMG) / `SVaCuusWidget` (Slate) — screen-space.
- `UVaCuusWorldComponent` — world-space quad/mesh UI.
- `stat vacuus`: UI-thread update ms, render-record ms, replay ms, buffer age, JS heap,
  model sync counts.

**Engine-neutrality stance (explicit):** public headers expose no RmlUi types (all Rml
usage is module-private), so the API is engine-neutral *in shape*; however v1 makes **no
hard abstraction layer** for alternative engines. If the deferred Servo tier
(VaCuus-akj.3) is ever built, it gets its own view class rather than constraining v1's
design. (Resolves the research §5 "engine-agnostic API" recommendation: adopted at the
header-hygiene level, rejected as an abstraction requirement.)

## 11. Performance budgets

**Reference HUD (the workload all gates are measured on):** a port of the research bench
workload — ~1,750 elements: 64 rAF-animated minimap blips, 18 keyframe-animated buff
icons, 24-row scoreboard, killfeed, animated bars — plus the demo's ability bar and
damage numbers. Exists from **M1 onward** (static subset) and is completed by M3;
budgets are asserted from M3.

| Metric | Budget | Status |
|---|---|---|
| Game-thread cost (input+snapshots) | ≤0.10 ms/frame | gate — **measured M2: 0.004 ms typical, 0.011 ms p99-sum @1080p (2026-07-30)**. Three scopes now cover the whole game-thread path: `GameTick` (subsystem tick — snapshot poll + UI-thread pulse), `SlateTick` (per view), `Input` (per input *event*, whole handler incl. the snapshot scan). 60 s soaks, both documents. **Honest exception: 1 frame in 13,073** on the M1 run had a 0.182 ms `GameTick` sample (and one 0.132 ms), both in fully idle windows with no VaCuus activity — they read as OS scheduling blips rather than VaCuus work, but they are above the gate and are recorded rather than smoothed. 25× headroom in the typical case |
| UI-thread Update + command record (reference HUD) | ≤0.50 ms/frame | gate — **measured M2: 0.052 ms avg / 0.113 ms p99 (M1 HUD), 0.023 ms avg / 0.060 ms p99 (M2 demo) @1080p (2026-07-30)**, Update+Record summed, 13k frames each. Note this is the M1/M2 subset, not the full reference HUD (~10× the element count, animated), so the ~4× headroom is not the final margin |
| Render-thread replay (re-replay frames) | ≤0.50 ms @1080p | gate — **measured M1: 0.03 ms avg / 0.07 ms p99 @1080p, 97 draws (2026-07-29)**. M1 static HUD subset, 100 s `-RenderOffscreen` soak, 15,324 replays, Linux Vulkan; steady-state max ≤0.37 ms, single 1.48 ms outlier on the first replay (font-atlas + image upload). Budget kept at 0.50 ms: ~7x p99 headroom for the full reference HUD (~10x the M1 element count, animated) |
| Composite-only frames (idle UI) | ≤0.05 ms | gate — **measured M2 on genuinely idle frames: 0.004 ms avg / 0.010–0.013 ms p99 (2026-07-30)**. M1 could only measure the composite *section* because it re-recorded and re-published every frame; M2's idle short-circuit makes true idle frames exist, and on a static HUD **13,072 of 13,074 frames published nothing at all** (M2 demo: 13,496 of 13,571, 99.4% idle). On those frames `Replay` is not called — it produces **zero samples**, not a small cost — so the composite is the entire per-frame render-thread cost of an idle UI. 12× under gate |
| Added RAM (reference HUD, incl. JS heap ≤16 MB cap) | ≤32 MB | gate |
| Added disk (Win64 shipping) | ≤10 MB | gate |
| Frame-drop on document load (async path) | 0 hitches >1 ms on game thread | gate — **measured M2 (2026-07-30)**: game-thread max across the load window 0.033 ms; window fps 209.1 vs 208.3–234.2 steady. Separately stress-tested with a deliberately pathological image (6000×6000, 144 MB decoded, 150 KB on disk so the read and the dimension probe stay trivial): the UI thread's `Update` max was 0.095 ms, **indistinguishable from steady state**. The cost moved to the render thread as expected — one 36.6 ms `UpdateTexture2D`, never recurring — because M2 made the *decode* async and deliberately left the *upload* synchronous (`VaCuus-akj.6.25`, gated on a measurement: ~1 ms of render thread per 4 MB) |

## 12. Testing

- Headless automation: `-RenderOffscreen` runs of UI thread + replay with RHI readback;
  golden-image comparisons for the two shipped composition paths (screen-space,
  world-space).
- Unit: model builder reflection matrix; JS facade conformance (subset of undom tests);
  command-buffer lifetime fuzzing (create/destroy churn); snapshot-protocol stress
  (writer/reader race harness).
- Perf CI: reference HUD budget assertions via `stat vacuus` counters (fail on
  regression).
- Manual matrix per release: Win64 D3D12, Linux Vulkan, macOS Metal; three latest UE
  majors (5.6–5.8 at time of writing).

## 13. Risks & mitigations

| Risk | Mitigation |
|---|---|
| UE API churn (RDG/Slate/RHI drift between majors) | version shims; support window = 3 latest majors; CI matrix |
| RmlUi single-maintainer | vendored fork + sponsorship + upstream contributions |
| UMaterial-in-UI-pass feasibility | M5 spike, declared fallback = built-in shaders only; **marketing consequence**: §1 gated promise is dropped from store copy if the gate fails |
| RmlUi measured perf pathologies (font-effect glyph gen up to 32.5 ms; bare attribute selectors ~2 ms; layout-thrash docs 5–16 ms) | effect-glyph warm-up at load (§9); CLI linter rules (bare attribute selectors, thrash patterns); docs perf guide; async glyph gen v1.x |
| RCSS gotchas surprise web devs (no UA stylesheet — `div` is inline; box-shadow not animatable; tween parse strictness; positioned-ancestor rules) | `@vacuus/cli` ships a base stylesheet + linter rules + "web-dev gotchas" docs page (seeded from demo findings) |
| TAA/TSR ghosting on world-space UI | responsive-AA material presets; docs recommend post-AA compositing/FXAA (same guidance class as Gameface's) |
| JS debugging expectations (breakpoints) | honest docs; CDP bridge on v2 roadmap; error overlay + inspector in v1 |
| PIE multi-instance | one subsystem+UI thread per GameInstance by construction; automation test with 2 PIE clients |
| Fab review friction | package per verified Fab rules; no-executables scan in M6; precedents documented in research |
| Grid absence hurts adoption | flex-first templates in CLI; evaluate upstream Grid contribution post-v1 |
| Win64/macOS validation hardware | required for M6 matrix — tracked as open item §15 |

## 14. Milestones

- **M1 — Render spike (VaCuus-akj.2):** command recorder + RHI replay into per-view RT +
  `ICustomSlateElement` composite in UE 5.8 PIE; static subset of the reference HUD.
  *Accept:* HUD renders in PIE at visual parity with the GL3 demo; zero game-thread waits
  (Unreal Insights capture attached); **replay ms measured and §11 replay budget
  re-baselined from data**.
- **M2 — UI thread + input + UMG widget + live reload:** full threading model (queues,
  snapshot publish, shutdown order per §4), input routing incl. interactive-region
  snapshot & pass-through, resize; editor file watcher + live document reload.
  *Accept:* interaction demo driven by mouse+keyboard+**gamepad spatial nav traversal**;
  **IME composition verified in a text input** (Linux ibus/fcitx in dev; Win64 re-checked
  at M6); thread-affinity checks green; no game-thread hitches on document load.
- **M3 — Data binding:** reflection model builder, snapshot protocol (§6), BP nodes;
  reference HUD completed and driven from a USTRUCT at 60 Hz.
  *Accept:* §11 gates (except provisional replay row) pass on the reference HUD.
- **M4 — JS Tier 1:** QuickJS on UI thread (vendored, exact tag recorded), DOM facade,
  timers/rAF, console, VFS modules, error overlay.
  *Accept:* reference-HUD logic ported from C++ sim to JS; parity + §11 budgets hold
  incl. GC pauses.
- **M5 — Web-DX Tier 2 + flagship renderer:** `@vacuus/preact` + CLI template (watch →
  M2 live reload); material-decorator spike → impl or declared fallback; scene
  backdrop-filter (OutputTexture-copy design, §5); world-space component.
  *Accept:* TSX HUD sample built via CLI runs in PIE; LDR glass-over-scene demo;
  **world-space panel interactive via raycast in PIE**; material-decorator gate decision
  recorded.
- **M6 — Productization:** reference HUD project; perf passport (final §11 table
  published); docs (gotchas, supported-RCSS matrix, perf guide); `UVaCuusBundle` cook
  path; Fab packaging dry-run; 5.6/5.7 shims.
  *Accept:* `RunUAT BuildPlugin` passes on 3 engine versions; **§11 gates pass on a
  cooked Win64 shipping build of the reference HUD (run from the cooked bundle)**;
  manual matrix pass on Win64 D3D12 / Linux Vulkan / macOS Metal; no-executables scan
  clean.

## 15. Open items (non-blocking, owner decisions)

- Plugin license for direct (non-Fab) distribution; pricing tiers.
- `CanContainContent` stays true (style sets/bundles are assets) — confirm at M6.
- Public API prefix (`VaCuus`/`Vc`) — confirm before M2 freezes headers.
- Win64 (D3D12) and macOS (Metal) validation hardware for M6 matrix.
- Third-party declaration mechanics on Fab (form details) — confirm at M6 dry-run.
