# VaCuus M5 — Web-DX Tier 2 + the flagship renderer

**Status:** design v2, ready for planning. v1 was reviewed adversarially by three independent
passes and came back **NEEDS REWORK** with twelve blocking findings across two reviewers (the
citations pass approved); §12 records the classes, because — the project's habit — the reasons
are the most useful part.

**Scope:** four tracks. **(P)** `@vacuus/preact` + `@vacuus/cli` — TSX over the M4 facade, esbuild
watch riding M2 live reload, typings, sourcemaps, **the localization hook M4 deferred here**.
**(G)** scene backdrop-filter glass, LDR, composited per engine frame. **(S)** the
shader-decorator pipeline: gradients + builtin shaders guaranteed, the UMaterial tier behind the
2-day gate with a declared fallback — **whose GO price now includes the source-verified freeze
remedy**. **(W)** `UVaCuusWorldComponent`. Acceptance (arch §14/M5): TSX HUD via CLI in PIE; LDR
glass demo; world-space panel interactive via raycast in PIE; material gate decision recorded.

**Ground truth:** `docs/research/m5-api-notes/*.md` (2026-07-31). **[unverified]** claims carry
their experiment. **Five** architecture-spec corrections are recorded in §10.

---

## 1. Goal

```tsx
// hud.tsx — built by `vacuus build`, hot-reloaded by the M2 watcher via `vacuus dev`
import { render } from '@vacuus/preact';
function Killfeed() {
  const [rows, setRows] = useState<Row[]>([]);
  useEffect(() => vacuus.onKill(k => setRows(r => [...r.slice(-5), k])), []);
  return <div class="killfeed">{rows.map(r => <div key={r.id}>{r.killer} » {r.victim}</div>)}</div>;
}
render(<Killfeed/>, document.getElementById('mount'));
```
```css
.panel { decorator: shader(glass-panel); backdrop-filter: blur(12px); }
```
Plus the same HUD on a quad in the world, clickable by raycast.

---

## 2. The findings that decide the architecture

**(a) Glass must be composited per engine frame — replay-baked glass freezes.** Replay runs only
on publish; the idle gate makes publishes ~never on a static HUD; the scene moves every frame. So
the recorded backdrop sequence is distilled into a **glass list** stored on the Slate element,
persisting across idle frames like the RT, and `Draw_RenderThread` runs downsample → blur →
masked glass draw into `OutputTexture` every engine frame, before the existing UI composite
(backdrop-glass.md §5; restore-the-bug Exp-GLASS-IDLE-FREEZE). **The list is recorded in VIEW
space and mapped through the live transform every engine frame**: offset = `DestRect.Min +
ElementsOffset`, scale = `DestRect.Size / ViewSize` applied to regions, mask geometry and σ,
clamped to `Inputs.SceneViewRect` in PIE — window coordinates are never pre-baked
(the existing composite's own mapping, VaCuusSlateElement.cpp:108-136; DestRect.Min is nonzero
in PIE). **The list-replacement rule is an invariant with a test**: every published buffer
replaces the list wholesale — a distiller that early-outs on glass-free buffers leaves the last
panel's blur running forever after removal or unload; §7 carries the inverted pixel-variance
restore-the-bug for exactly that. **The mask is clip-mask geometry, not analytic SDF params**:
corner radii cross the render interface only as compiled geometry (ElementUtilities.cpp:164-169)
— the recorded data supports the geometry-mask branch; an analytic-SDF fast path would need a
recorder side-channel and is not v1.

**(b) `OutputTexture` verified: the tonemapped scene is there in LDR game and PIE — and the copy
is avoidable.** (Unchanged from v1; the chain held under citation review.) One bilinear half-res
downsample pass is simultaneously the copy and the downsample (the engine's own Slate blur binds
the same texture as an SRV mid-frame — SlateRHIRenderingPolicy.cpp:1718-1738,
SlatePostProcessor.cpp:784-832). Exp-GLASS-SCENE-CONTENT / Exp-GLASS-BACKBUFFER-SRV cover the
two [inference] legs; fallback = the engine's own copy-pass shape. HDR composite ⇒ no scene in
the elements texture and no in-band discriminator ⇒ **LDR-only**, Exp-GLASS-HDR-DETECT.

**(c) The blur chain does not exist and gets built** (`VaCuusBlur.usf`, `VaCuusGlassPS`);
gamma-neutral by construction; Exp-GLASS-GAMMA-BLUR decides by image. Scaling note: the pooled
half-res RT and the blur chain are **per Slate element** — N glass-bearing views in one window
run N chains; the budget row states the per-view cost and the multiplier.

**(d) Seven recorder virtuals + buffer tripwires** (CompileFilter/ReleaseFilter,
PushLayer/CompositeLayers/PopLayer, EnableClipMask/RenderToClipMask): new command types, the
`NewFilters`/`ReleasedFilters` resource pair, member counts, `HasResourceTraffic()`, and the hash
— **CompositeLayers' variable-length filter list rides the header's own variable-length-record
provision** (VaCuusCommandBuffer.h:406-411), not a fixed hash-image field. **The filter-type
contract:** RmlUi ships ten filter instancers; v1 compiles **blur only** — every other type
returns handle 0 (RmlUi's own per-element warning fires, the same discipline as unknown
decorator keys) plus one named VaCuus log listing supported types; glass extraction consumes
blur entries only; a backdrop list with no blur produces no glass entry. The replayer **skips**
clip-mask commands outside glass extraction in v1 (today's behavior for ordinary rounded-corner
clipping preserved; documented).

**(e) `decorator: shader(...)` exists upstream; the shader plumbing ships regardless of the
gate** (`NewShaders`/`ReleasedShaders`, `DrawShader` — the first mid-pass PSO switch — same
tripwire discipline). Gradient dictionaries are plain data.

**(f) The material tier is feasible without Slate — and the freeze is a fact, not a spike
observable.** The mechanism: plugin-declared `FMaterialShader` pair gated on MD_UI against a
plugin `.usf` + `FMaterialShader::SetParameters` + the fabricated view UB (Slate's own recipe).
TextureGraph proves the plugin-shader mechanism but its permutations are editor-only
(`HasEditorOnlyData` gate) — **the runtime permutation is proven only by Slate's in-engine MD_UI
types, so the spike's monolithic `-game` check is load-bearing for GO, not a formality.**
**Source-verified before the spike:** the composite pass samples the RT and writes OutputTexture
— it cannot re-evaluate a material — and the RT is written only in the publish-gated replay
branch (VaCuusSlateElement.cpp:56-89, :113-140). **A time-animated MD_UI material freezes
between publishes.** GO therefore names and prices its remedy up front — forced republish while
a material decorator is live (reopening the idle row for material documents) or a composite-time
material draw — and the spike measures the remedy's cost. The blend contract: our RT is
premultiplied `One/InvSrcAlpha`; the material PS premultiplies on output. Scene-texture/VT
materials refused with a named error.

**(g) The world RT bridge is copy-on-publish with an explicit lifetime discipline.** The sink
holds **one `FTextureRHIRef` destination slot, updated only by a render command the game thread
enqueues after every `UTextureRenderTarget2D` (re)init** — FIFO with the resource recreation it
follows; publishes arrive via render commands from the UI thread; the per-publish copy is
guarded by extent equality and **skipped on mismatch, self-healing on the first matching
publish** (the same accepted transient the screen path documents for resize). One GPU copy per
published frame ⇒ ~never when idle; the `FRHITextureReference` repoint stays the recorded v1.x
escape (WS-COPY-COST promotes it only on evidence).

**(h) World input defers to Slate — the processor consumes only what nothing else claims.** The
`IInputProcessor` runs before all Slate routing, so an unconditional consume would starve any
UMG/Slate widget stacked over the viewport (the engine's own world-widget input is an
`ICustomHitTestPath` *inside* viewport hit-testing for exactly this reason). **The occlusion
rule:** the processor traces and consumes only when (a) no Slate widget holds mouse capture and
(b) the widget path under the cursor terminates at the game viewport (an `FSlateApplication`
query) — i.e. nothing interactive overlays the point. Then: deproject → hit component → the
`GetLocalHitLocation` math (cloned verbatim for a plane) → `SendInput` with synthesized coords →
consume verdict = the interactive snapshot (`Contains(Pixel)`), capture latch, mandatory
`MouseLeave`. Pointer-only; IME-less by D17. WS-STALE-RAY decides button-event re-trace.
**Zero/degenerate `DrawSize` is a named refusal at `OnRegister`** (log + no view; the replayer
keeps no RT for degenerate sizes and the world path has no first-tick heal by design).

**(i) The world material preset:** `BLEND_AlphaComposite` (color blend `One/InvSrcAlpha` — the
RT's contract; the alpha half is `Zero/InvSrcAlpha`, a stated one-channel difference that is
holdout bookkeeping, not compositing), unlit, `bEnableResponsiveAA` (works under TAA, **ignored
by TSR** — documented honestly, post-AA/FXAA guidance), the WS-GAMMA decode decision, two-sided
switch. One preset asset replaces UMG's six.

**(j) The facade is fifteen gaps away from Preact** — G1 text nodes moderate, traversal/
discrimination trivial, event-name case fork-side; style camelCase→kebab facade-side.
*(Corrected by E-P observation, 2026-08-01: v2's "className is likely a no-op" was wrong —
stock preact 10.29.7 emits `setAttribute("className", …)` outside SVG, silently losing the
class; the className→class rename exists only in its SVG branch. The fork renames it, or the
facade grows a `className` accessor. Also observed: `createElementNS` is preact's ONLY
element-creation call and mount adoption walks `dom.attributes` — both landed as facade gaps
the table missed; `replaceChild` is never called and was dropped; preact appends `px` itself.
Full record: m5-api-notes/ep-observations.md.)* **The brace hazard is a real injection
class**: RmlUi's text instancer auto-tags brace-bearing text with `data-text` (Factory.cpp
:343-392) — a Preact app rendering user strings containing `{{` via text nodes or
`dangerouslySetInnerHTML` gets data-binding evaluation of user data. **G1's text-node write path
must bypass the binding scanner** (set text on the ElementText directly, never through RML
re-parse), and `dangerouslySetInnerHTML` documents the hazard; E-P3 verifies the bypass.
Scheduling and expandos as v1 stated. E-P1..E-P7 gate the fork work; **the invalidation
protocol**: an experiment result that contradicts a §2 claim gets a spec errata commit (this
project's standing mechanism), not just a fork commit — the experiment's outcome is recorded
either way.

**(k) Reload is re-mount by module top-level** (unchanged; the in-session desync hazard E-P6).

**(l) The CLI targets `Content/DevUI`; typings flow through a committed manifest; sourcemap
resolution is honestly rescoped.** Watch → reload works today (roots + `js|mjs` extensions).
**Typings pipeline (the conformance test's provenance):** the node-side smoke script extracts an
entry manifest (JSON) from `vacuus.d.ts` and the manifest is **committed beside the test
content**; the in-engine conformance test walks the real globals/prototypes against the manifest
**both ways** (typed-but-absent fails, present-but-untyped fails); the smoke script fails when
the committed manifest is stale against the `.d.ts`. **Sourcemaps: this is arch-correction #5,
not a match** — arch §7 Tier 1 promised "log with sourcemap resolution (Tier 2 supplies maps)";
v1 ships maps (inline) + raw generated positions in the overlay + offline `vacuus symbolicate`;
**in-engine resolution moves to v1.x** and the arch spec is amended. **The committed-bundle
staleness protocol:** a provenance manifest beside the bundle (preact version, facade-manifest
hash, build command); the engine suite asserts the provenance matches the current facade
manifest and **skips-with-a-named-warning** (not fails) on mismatch, so a facade change gives
the controller a rebuild instruction instead of an undiagnosable red. **TSX build errors in
watch mode reach only the terminal** — no bundle written ⇒ no reload ⇒ the engine silently keeps
the last good UI; the CLI prints the loud failure and the docs name the trap. **The
localization hook (M4's documented deferral) lands here**: `vacuus.translate(key, params?)` —
UI-thread callback into a game-registered handler (the write-router seam pattern), default =
identity; the CLI template routes user-visible strings through it; RmlUi's own
`TranslateString` path already exists for RML text (the M2 system interface) — the JS hook is
the missing half. Refusal shape: no handler ⇒ identity + one latched Verbose line.

## 3. Architecture

### 3.1 Track P — facade completion, `@vacuus/preact`, `@vacuus/cli`

Facade: G1 text nodes **with the binding-scanner bypass** (j), G2-G4 traversal, `localName`,
style camelCase→kebab (bare numbers per E-P4), `replaceChild` per E-P1. Fork: event-name case +
pinned options; the undom-subset conformance run in-engine against the committed bundle.
CLI: `create` (template rendering at module scope into a dedicated empty mount; base stylesheet;
linter rules), `dev`, `build`, `symbolicate`; the typings manifest pipeline (l); the
localization hook (l). **Node-side smoke** (controller-run): linter fixtures — one known-bad
per rule asserting it fires, one clean asserting silence; the demo bundle built on the base
stylesheet so §8's screenshots double as its test; the manifest staleness check.

### 3.2 Track G — glass

As v1 (recorder virtuals, buffer + tripwires + variable-length filter records, replayer skip
rules, the Slate-element glass pipeline, pooled per-element half-res RT) with (a)'s coordinate
mapping, (d)'s blur-only filter contract, and the list-replacement test. `stat vacuus` gains
GlassMs. Demo `vacuus.M5Glass` + the four Exp-GLASS experiments.

### 3.3 Track S — decorators

Stage 1 (guaranteed): shader plumbing + gradient PS + builtin set. Stage 2 (gated): the spike
per material-decorators.md §6 **with the (f) corrections** — the freeze remedy priced in GO, the
monolithic check load-bearing, the editor-only TextureGraph caveat noted. GO ⇒ `UVaCuusStyleSet`
with **publish-by-replacement snapshots carrying a version counter the UI thread asserts
monotonic** (immutability made observable); FALLBACK ⇒ builtin tier only + marketing note +
plumbing kept. Decision recorded with the blend screenshots, the freeze-remedy cost, and the
uniform-expression measurement.

> **Stage-2 outcome (2026-08-01, the Task 5 spike): GO.** Every material-decorators.md §6
> criterion met, the remedy implemented and priced. Mechanism as designed: plugin
> `FVaCuusMaterialVS/PS : FMaterialShader` against `Shaders/Private/VaCuusMaterial.usf`,
> `ShouldCompilePermutation = (MaterialDomain == MD_UI)` with **no editor-only flag**; one
> synthetic view UB per replay pass (the Slate recipe + its noise/scene-scale patches);
> the Slate proxy walk; one additional PSO per material shader map; draws injected after
> the replayed commands — **no recorder or buffer-format change**, as Task 4 predicted.
> Spike surface: `vacuus.MaterialDecorators` (default 0), `vacuus.MaterialForcedRepublish`
> (the remedy, default 1), `vacuus.MatSpike.Add/.MID/.Clear`, demo `vacuus.M5MatSpike`,
> test `VaCuus.Render.Decorator.MaterialSpike`. Criteria, measured (1920×1080 Vulkan SM6,
> headless, screenshots in `docs/research/proofs/m5-t5-material-spike/`):
>
> - **Blend matrix over text** — Translucent/Additive/Opaque MD_UI materials over RmlUi
>   text; every mode mapped IN the PS onto the RT's single premultiplied One/InvSrcAlpha
>   state (additive = alpha-0 output, Slate's own trick; emissive sRGB-encoded so material
>   colours match RCSS colours on the display-referred RT): `runA_blend_matrix_beat1.png` —
>   text legible through translucent/additive, replaced under opaque, no fringes.
> - **MID params, per game frame** — scalar+texture through an MID picked up in our pass
>   with no Slate draw anywhere; game-thread cost **avg 3.5 µs, max 47 µs/frame**; region
>   RMSE 11.5% between beats vs 0.13% control (`runA_mid_beat*.png`).
> - **The freeze, observed then remedied** — remedy off: a Time-driven material is pixel-
>   frozen across 5 s (anim-region RMSE **0.18% = the control floor**, `runB_frozen_*`);
>   remedy on: **17.9%**, animating (`runA_anim_beat*.png`). Confirms §2(f) exactly.
> - **The remedy, priced** — forced republish while a material is live: published goes
>   0% → 100% of recorded frames (~237/s in the soak, scales with UI-thread rate);
>   Record (UI) **0.007 ms/frame, unchanged from the idle baseline**; Replay (RT)
>   **0.024 ms/frame** including the per-pass view UB and
>   `UpdateUniformExpressionCacheIfNeeded` over five live materials — ≈1.7 ms/s UI +
>   5.7 ms/s RT at 237 fps. The un-implemented alternative (composite-time material draw)
>   would run one glass-shaped pass per engine frame (composite scope measured 0.003 ms)
>   but is z-order-correct only for stack-end decorators — rejected for v1; the production
>   task should clamp republish to engine-frame rate, not adopt it.
> - **Monolithic `-game`, the load-bearing check** — `BuildCookRun` Linux Development:
>   the five spike materials compiled shader maps for `VULKAN_SM6 … Game` (no
>   `HasEditorOnlyData`), and the packaged binary loaded them from cooked paks and
>   rendered the same matrix + animation (staged anim-beat RMSE 18.0%), **zero shader
>   misses** (`runS_staged_*`). The TextureGraph editor-only caveat does not apply to our
>   permutation.
>
> Findings the production task inherits: (1) while a material's shader map is still
> async-compiling, the whole proxy chain (default UI material included) can be
> pair-less for the first frames — observed at frame 2, self-healed; the registry should
> pre-warm or accept the named-log transient. (2) Runtime-constructed `UMaterial`s cannot
> compile outside the editor — the spike's `.uasset`s are authored once by an editor
> python script (Task 5 report) and committed under `Content/Spike/`; cooking them needs
> the host project's `DirectoriesToAlwaysCook=/VaCuus/Spike` (they are referenced by
> nothing). (3) Scene-texture/VT refusal remains registry-validation work — the spike
> hard-disables scene textures in the `.usf` only. The production shape (`UVaCuusStyleSet`,
> monotonic snapshots, per-view republish flag driven by the recorder's compiled-shader
> table) is the follow-up task, deliberately not built here.

### 3.4 Track W — world-space

As v1 (component in VaCuusRender, quad proxy + body setup cloned, `IVaCuusFrameSink` refactor,
copy-on-publish per (g), preset per (i), input per (h) incl. the occlusion rule and the
zero-size refusal). Pass-through's observable is named: a test pawn's click counter — the
processor returns false ⇒ Slate routes to the viewport ⇒ the pawn hears it.

## 4. Threading

As v1. The two cross-thread additions (style-set snapshot, frame-sink publish) follow existing
seams; the world destination-slot discipline is (g).

## 5. Diagnostics

As v1 plus: the filter-type refusal log (d); the zero-DrawSize refusal (h); the provenance
skip-warning (l); the localization no-handler line (l).

## 6. Budgets

| | Target | Machinery |
|---|---|---|
| Glass per engine frame, per glass-bearing view (1 panel, half-res, 1080p) | ≤0.15 ms RT | GlassMs scope; N views = N× stated |
| Gradient/builtin decorator cost | no regression in Record (UI) / Replay (RT) | **the headless demo-session PerfLog windows** (the M2/M4 soak method), decorators present vs absent — the probe harness structurally cannot see Record/Replay |
| Material draw (GO only; FALLBACK vacates this row and the gradient row is the shipped tier's entire budget) | remedy + draw ≤0.10 ms/material/frame | the spike's numbers |
| World copy per published frame (1024²) | ≤0.05 ms RT, ~0 idle | WS-COPY-COST |
| Preact HUD steady state | JsPump ≤0.30 ms holds | the M4 row re-measured on the port |
| Idle, all tracks | the M3b/M4 exact-zero gates hold; glass composite excepted by design (engine-frame work, not publish work) | the existing suites + a glass-bearing idle run |

## 7. Testing

Restore-the-bug where marked; M3a/M3b/M4 suites green throughout.

- **Facade**: per-gap tests; **the brace-injection test** — a text node written with `{{Health}}`
  renders the literal braces, never the binding **(restore: route the write through RML parse →
  the binding evaluates — observed)**; conformance vs the committed manifest both ways;
  E-P1..E-P7 with the invalidation protocol.
- **Glass**: recorder units; tripwires **(restore: drop a hash field → withheld change)**;
  Exp-GLASS-IDLE-FREEZE both outcomes; the presence pixel-variance assertion; **the removal
  test** — show glass, publish, remove the panel / unload, publish, assert the former region
  matches the sharp scene **(restore: distiller skips glass-free buffers → stale blur forever)**;
  the coordinate mapping under a nonzero DestRect.Min (PIE-shaped) asserted.
- **Decorators**: gradients vs reference screenshots; unknown key; **non-blur filter refusal
  observed**; GO/FALLBACK per §3.3 incl. the snapshot version-counter assertion.
- **World**: hit-math units; PIE click-through-to-write-router; **occlusion** — a Slate widget
  over the panel wins, the processor does not consume; **pass-through** — the test pawn's
  counter increments on a miss; zero-size refusal; MouseLeave; WS-GAMMA parity.
- **CLI** (node-side, controller-run): linter fixtures; manifest staleness; roundtrip.
  In-engine: the committed bundle + provenance skip-warning behavior.

## 8. The demo — acceptance

`vacuus.M5Demo`: the TSX HUD (committed bundle **with recorded provenance**: built by
`vacuus build` over the committed TSX source) in PIE with a glass panel over the moving scene,
gradient + builtin decorators visible, the same HUD on a world quad clicked by raycast, a
localized string routed through the hook. AutoShot screenshots; the material gate outcome
recorded here.

## 9. Risks

As v1, plus: the occlusion query's cost per pointer event (measure; cache per frame like the
engine's hit tester); the manifest pipeline adds a controller step (the skip-warning keeps the
suite green-with-signal rather than red).

## 10. Architecture-spec corrections

1. The GL3 blur chain was never ported (one shader file exists).
2. Replay-baked glass contradicts the M2 idle gate; composite-time is correct.
3. `Content/UI/**` was never built; the watcher roots are `Content/DevUI`.
4. The `VaCuusUMG` module does not exist; the surface lives in VaCuusRender.
5. **Tier-1's "log with sourcemap resolution" is rescoped**: v1 = inline maps + raw positions +
   offline symbolicate; in-engine resolution v1.x.

## 11. Out of scope

As v1 (HDR glass, in-page UI-over-UI backdrop, in-engine sourcemap mapping, cylinder/VR,
CSS Grid, the full filter/mask tier) plus: analytic-SDF glass masks (geometry-mask branch only);
per-element non-blur backdrop filters (refused, (d)); the `PF_FloatRGBA` composite permutation —
**the pre-existing M1 gap no milestone has claimed; assigned to M6's productization matrix**
(recorded on the M6 bead rather than absorbed here).

## 12. What v1 got wrong, and why it matters

1. **Glass regions without a coordinate-space rule** — view-space data drawn at window
   coordinates works fullscreen and breaks in PIE, where DestRect.Min ≠ 0. The mapping is now
   the design, with a PIE-shaped test.
2. **An input processor that consumes before Slate routes starves overlaid widgets** — the
   engine's own world-widget input lives *inside* viewport hit-testing for this reason. The
   occlusion rule is now the contract.
3. **"Whether the composite masks the material freeze is a spike observable" was answerable by
   reading two functions** — the composite cannot re-evaluate a material; the freeze is a fact;
   the spike prices the remedy. A question you can settle from source is not an experiment.
4. **Copy-on-publish without a destination-lifetime rule** raced game-thread RT re-inits against
   UI-thread-enqueued copies. The slot + FIFO + extent-guard discipline is now stated.
5. **A twice-deferred promise (localization) silently vanished** — the exact scope-drop class
   the project's correction discipline exists to prevent. It lands in Track P.
6. **A budget row named machinery that structurally cannot measure it** (the probe harness has
   Record=0 by construction). The PerfLog soak is the named method.
7. **"Matching the arch spec's Tier boundary" dressed a rescope as a match** — resolution was
   the Tier-1 deliverable. Now correction #5, amended honestly.
8. **Invariants without observables, again**: the glass-removal rule, the conformance-test
   provenance, the linter rules, zero DrawSize, non-blur filters — each now has a named test or
   refusal. The M2 lesson holds: an invariant with no observable rots.
9. Smaller, same species: the SDF mask had no data source (geometry branch is the design); the
   brace-injection hazard was unmentioned (now a restore-the-bug test); `className` was budgeted
   as a patch it likely doesn't need; the committed bundle had no staleness protocol.
