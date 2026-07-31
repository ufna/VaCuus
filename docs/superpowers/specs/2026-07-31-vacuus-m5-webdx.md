# VaCuus M5 — Web-DX Tier 2 + the flagship renderer

**Status:** design v1, for adversarial review before planning.

**Scope:** four tracks. **(P)** `@vacuus/preact` + `@vacuus/cli` — TSX over the M4 facade, esbuild
watch riding M2 live reload, typings, sourcemaps. **(G)** scene backdrop-filter glass, LDR,
composited per engine frame. **(S)** the shader-decorator pipeline: gradients + builtin shaders as
the guaranteed tier, the UMaterial tier behind the 2-day gate with a declared fallback. **(W)**
`UVaCuusWorldComponent` — the per-view RT on a world quad, raycast input through the existing
enqueue+snapshot path. Acceptance (arch spec §14/M5): TSX HUD via CLI in PIE; LDR glass demo;
world-space panel interactive via raycast in PIE; material gate decision recorded.

**Ground truth:** `docs/research/m5-api-notes/{preact-contract,backdrop-glass,material-decorators,
worldspace-cli}.md` (2026-07-31, written against the code on this disk). **[unverified]** claims
carry their experiment. Four architecture-spec corrections are recorded in §10 — each was found by
opening the cited source, and two change the shape of the work.

---

## 1. Goal

```tsx
// hud.tsx — built by `vacuus dev`, hot-reloaded by the M2 watcher
import { render } from '@vacuus/preact';
function Killfeed() {
  const [rows, setRows] = useState<Row[]>([]);
  useEffect(() => vacuus.onKill(k => setRows(r => [...r.slice(-5), k])), []);
  return <div class="killfeed">{rows.map(r => <div key={r.id}>{r.killer} » {r.victim}</div>)}</div>;
}
render(<Killfeed/>, document.getElementById('mount'));
```
```css
.panel { decorator: shader(glass-panel);        /* builtin — guaranteed tier */
         backdrop-filter: blur(12px); }         /* the scene shows through, blurred, live */
```
Plus the same HUD on a quad in the world, clickable by raycast.

---

## 2. The findings that decide the architecture

**(a) Glass must be composited per engine frame — replay-baked glass freezes.** Replay runs only
when a buffer was published, and the idle gate exists precisely so a static HUD publishes ~nothing
(2 of 13,074 frames measured in M2). The scene changes every frame regardless: bake the blurred
scene into the per-view RT at replay time and an idle HUD shows a **frozen backdrop over a moving
scene**. Design: the recorded backdrop sequence is distilled into a **glass list** (region +
rounded-rect/clip params + σ) stored on the Slate element beside `DestRect`, persisting across
idle frames like the RT itself; every engine frame `Draw_RenderThread` runs downsample → separable
blur → masked glass draw into `OutputTexture`, then the existing UI composite on top. Idle economy
intact; backdrop live at engine rate. Documented v1 limit (already the arch spec's scope): glass
blurs the scene + lower Slate content, not same-context UI beneath the element
(backdrop-glass.md §5, restore-the-bug Exp-GLASS-IDLE-FREEZE).

**(b) `OutputTexture` verified: the tonemapped scene is there in LDR game and PIE — and the copy
is avoidable.** In `-game` LDR the elements texture *is* the backbuffer holding the scene; in PIE
the scene quad is flushed as a primitive batch before custom drawers run
(SlateRHIRenderingPolicy.cpp:1678-1707, SlateRHIRenderer.cpp:972-1013). The engine's own Slate
blur samples the same texture directly as an SRV mid-frame (SlateRHIRenderingPolicy.cpp:1718-1738,
SlatePostProcessor.cpp:784-832), so **one bilinear half-res downsample pass is simultaneously the
copy and the downsample**. Two [inference]-tagged ordering claims and the Linux/Vulkan
swapchain-as-SRV question carry experiments (Exp-GLASS-SCENE-CONTENT, Exp-GLASS-BACKBUFFER-SRV;
fallback = the engine's own `AddCopyTexturePass` shape). Under HDR composite the elements texture
contains **no scene** and `bOutputIsHDRDisplay` cannot detect it — glass is **LDR-only**, with
Exp-GLASS-HDR-DETECT finding the discriminator or a game-thread mirror disabling it.

**(c) The blur chain does not exist — the arch spec's "ported from the GL3 backend" is future
work.** The tree ships exactly one shader file (`VaCuusUI.usf`); the GL3 chain is vendored
*reference source* only. What glass needs built: `VaCuusBlur.usf` (separable gaussian, the engine's
downsample→blur→upsample scheme with corner-radius masking as the structural template — it lives
in a Private folder and must be mimicked, not linked) and `VaCuusGlassPS` (blurred-RT sample +
rounded-rect SDF mask, `SrcAlpha/InvSrcAlpha` so edges lerp blurred-over-sharp). Gamma-neutral by
construction: glass samples and writes the same display-encoded texture — the engine's own Slate
blur does no linearization either (Exp-GLASS-GAMMA-BLUR settles by image).

**(d) RmlUi's backdrop is its own base layer — the scene must be injected under it, and the
recorder's gap is exactly seven virtuals.** `backdrop-filter` composites layer 0 → temp → 0 with
the filter list (ElementEffects.cpp:247-281); layer 0 is the per-view RT, transparent where UI
hasn't drawn — RmlUi has no concept of the 3D scene. Glass needs `CompileFilter`/`ReleaseFilter` +
`PushLayer`/`CompositeLayers`/`PopLayer` recorded (plus `EnableClipMask`/`RenderToClipMask` for
rounded corners); border-radius always routes through the clip mask
(ElementUtilities.cpp:157-186). The command buffer grows new command types + a `NewFilters`/
`ReleasedFilters` resource pair — **the member-count tripwires and `HasResourceTraffic()` must be
extended and the hash image must gain the new fields**, or the idle gate silently withholds
changed frames; the buffer header names this exact scenario (VaCuusCommandBuffer.h:191-241 — the
M3 lesson enforced by shape).

**(e) `decorator: shader(...)` already exists upstream — the guaranteed tier is plumbing, not
parsing.** RmlUi 6 registers a `shader` decorator instancer by default (Factory.cpp:91, :202); the
registry key arrives verbatim as one string. The gradient family (`linear/radial/conic-gradient`)
crosses the same three virtuals (`CompileShader`/`RenderShader`/`ReleaseShader`) with plain-data
dictionaries — today all of them **draw nothing with one warning** (the default returns a zero
handle). The recorder/replayer shader plumbing — new resource pair `NewShaders`/`ReleasedShaders`,
a `DrawShader` command (the first mid-pass PSO switch), the same tripwire/hash discipline as (d) —
**ships regardless of the material gate**, because gradients need it (material-decorators.md §2).

**(f) The UMaterial tier is feasible without Slate — the spike protocol is written.** Slate's own
material-shader types are module-private, but the *mechanism* is not: a plugin can declare its own
`FMaterialShader` pair gated on `MaterialDomain == MD_UI` against a plugin `.usf`
(TextureGraph is the engine precedent — FxMaterial_DrawMaterial.h:27-97, its `.usf` shows the
whole recipe), bind per-draw via `FMaterialShader::SetParameters` (batched, `FSceneView`-free),
and fabricate the view uniform buffer the way Slate itself does
(`SetupCommonViewUniformBufferParameters`, SlateRHIRenderingPolicy.cpp:706-756). The wrong path is
named so nobody proposes it: `DrawTileMesh` feeds mesh passes that filter out MD_UI. Honest
constraints the spike verifies: premultiply-on-output over our RT contract; the shader-map cost
(two new permutations per UI material per project); no scene textures (registry rejects with a
named error); VT-sampling materials unsupported. **GO/FALLBACK criteria and the day-1/day-2 build
list are in material-decorators.md §6** — including the idle-gate finding the decision must
record: a time-animated material may freeze if replay is withheld; whether the composite rerun
masks that is a spike observable, and if not, the fix's cost belongs in the GO price.

**(g) The RT bridge for world-space is copy-on-publish — and the idle gate makes it nearly
free.** The per-view RT is a raw RHI texture recreated on resize; materials bind `UTexture`s
through `FRHITextureReference` (MaterialUniformExpressions.cpp:1708-1729). Nothing drives replay
for a world view (only `SVaCuusWidget::OnPaint` submits the element today) — so the host's
publish targets become an `IVaCuusFrameSink` interface (the Slate element implements it
unchanged; the world component provides `FVaCuusWorldSink`), the world sink replays
arrival-driven (the arch spec §4 model verbatim) and `CopyTexture`s into a component-owned
`UTextureRenderTarget2D` — one GPU copy **per published frame**, i.e. ~never on an idle HUD. The
zero-copy `FRHITextureReference` repoint is the recorded v1.x optimization (per-update RHI-thread
fence + Epic deprecation intent — the M2 debt note), promoted only if WS-COPY-COST shows the copy
(worldspace-cli.md §2).

**(h) World-space input is twelve lines of borrowed math plus the machinery M2 already built.**
`UWidgetComponent::GetLocalHitLocation` (hit → component space → pixel space, because the quad's
world units are pixels) is cloned verbatim for a plane; delivery is NOT UMG's virtual-window
path but ours: one process-wide `IInputProcessor` (the M2 research note's skeleton), per pointer
event deproject → hit component → local pixels → the existing `SendInput`/`EnqueueInput`, consume
verdict = the interactive snapshot (`Contains(Pixel)`), capture = the pressed-buttons latch, and
a mandatory `MouseLeave` when the ray leaves (or `:hover` sticks). Keys ride the same path;
**IME-less by decision D17**; pointer-only meets the acceptance line. Inherited, accepted skew:
snapshot one UI frame stale + transform staleness on moving actors (WS-STALE-RAY decides whether
button events re-trace).

**(i) The world material preset: `BLEND_AlphaComposite`, responsive AA — and TSR honestly
ignores it.** AlphaComposite's blend state is exactly the RT's premultiplied contract
(`One/InvSrcAlpha`, TranslucentRendering.cpp:2474-2475). `bEnableResponsiveAA` works under TAA
(stencil bit → history cut, verified end-to-end) and does **nothing under TSR** (no stencil
consumption in TemporalSuperResolution.cpp; `bHasPixelAnimation` is opaque-only) — the docs say
post-AA compositing or FXAA for heavy world-space UI, which is the arch spec's own framing.
New finding for the preset: the RT holds display-gamma pixels but a translucent material writes
into pre-tonemap linear scene color — the preset needs an sRGB→linear decode (imperfect on
premultiplied data; noted) or panels read washed out; `UWidgetComponent` acknowledges the same
seam with `bApplyGammaCorrection`. WS-GAMMA settles by screenshot parity.

**(j) The facade is fifteen gaps away from Preact — most trivial, one moderate, two fork-side.**
The gap table (preact-contract.md §3): **G1 text nodes** is the one moderate item —
`createTextNode` + `data`/`nodeValue`, buildable over RmlUi's `#text` elements (the facade's
`children` filter stays; `childNodes` arrives unfiltered); **trivial**: `nextSibling`/`firstChild`
family (RmlUi has all four), `childNodes`, `nodeType` (1/3/9 — discriminators exist), `localName`;
**fork-side patches** in `@vacuus/preact`: event-name case (`onClick` → the lowercase RmlUi
names) and `className` → `setAttribute('class')` (which the facade already applies immediately —
the classList trap does not bite the attribute path); **facade-side**: style camelCase→kebab
mapping in the style proxy. `insertBefore` with a non-child ref appends (RmlUi's fallback) where
DOM throws — documented deviation. Expandos work (wrappers are plain JS objects + the identity
cache — Preact's `_listeners`/`__k` attach naturally). Scheduling: a `setState` from an input
handler commits in the same frame's job drain before `Context::Update`; a `setState` from a
listener RmlUi fires *inside* Update waits one frame — a documented line, inherent to the pump
placement. The fork pins `options.debounceRendering`/`options.requestAnimationFrame` explicitly.
Seven experiments (E-P1..E-P7) gate the fork work on observed Preact behavior, not memory.

**(k) Reload is already correct for Preact — re-mount by module top-level.** The M4 recycle
destroys the whole JSContext; no JS survives, so no vdom pointer dangles. The CLI template's entry
must call `render()` at module scope — the only thing the recycle re-runs. Component state is
lost by design; the continuity story is M3 data models. The real hazard is in-session: C++ tree
surgery (or `innerRML` on a Preact-owned subtree) leaves Preact holding dead wrappers, silently —
E-P6 decides whether a refused-op counter + overlay line is worth shipping.

**(l) The CLI writes into `Content/DevUI` — the arch spec's `Content/UI` was never built — and
watch → reload works today with zero plugin changes.** The M2 watcher roots are the DevUI pair
and the extension list already covers `js|mjs` (M4 added them); a bundle written elsewhere
produces no event, no reload, no error — the CLI docs carry the two traps (roots, debounce).
Typings are a **hand-written `vacuus.d.ts` + a conformance automation test** that walks the real
globals/prototypes both ways (~40 entries; generation has no type information to harvest).
Sourcemaps v1: esbuild `--sourcemap=inline`, the overlay prints raw generated positions, the CLI
ships `vacuus symbolicate <stack>` for offline mapping — in-engine mapping deferred, matching the
arch spec's own Tier-boundary. Workspace: `Web/packages/{preact-vacuus,cli}`, source-only, no
`node_modules`, no binaries (the Fab constraints are hard requirements; M6's no-executables scan
covers the zip).

---

## 3. Architecture

### 3.1 Track P — `@vacuus/preact` + `@vacuus/cli`

- **Facade completion** (VaCuusJs): G1 text nodes (`document.createTextNode`, `nodeValue`/`data`
  set, `textContent` on elements if E-P3 shows it is load-bearing), G2-G4 traversal/discrimination
  getters, `localName`, style-proxy camelCase→kebab (numbers get `px` only if E-P4 shows Preact
  emits bare numbers — else refuse loudly), `replaceChild` if E-P1 shows first-render needs it.
  Every addition follows the wrapper rules: dead ⇒ null, identity cached, UI thread.
- **`@vacuus/preact`**: vendored preact + compat, patched per E-P2/E-P5 observations (event-name
  case, className path, options pinned). Ships with the undom-subset conformance test the arch
  spec §12 planned, run in-engine via the M4 rig.
- **`@vacuus/cli`**: `create` (template: TSX entry rendering at module scope into a dedicated
  empty mount element — per E-P1; base stylesheet; the RCSS-gotchas linter rules from the arch
  spec risk table), `dev` (esbuild watch → `<Project>/Content/DevUI/<app>/`), `build`,
  `symbolicate`. Typings + the conformance test (l).
- Node is unavailable in this dev loop — the CLI is authored as source + a **smoke script the
  controller runs where node exists**; in-engine tests cover everything that runs in-engine
  (the facade, the conformance walk, a prebuilt bundle checked into test content — built once,
  committed, so the suite never needs npm).

### 3.2 Track G — glass

Recorder: the seven virtuals of (d), recording `{type, sigma}` filters and layer ops; buffer:
new commands + `NewFilters`/`ReleasedFilters` + tripwires/hash. Replayer v1 skips layer commands
except glass extraction. Slate element: the glass list (set per published buffer, used every
engine frame), pooled half-res RT, passes downsample → blur ping-pong → masked glass draw before
the existing composite. `vacuus.M5Glass` demo: the M4 HUD over a moving scene with a glass panel,
AutoShot screenshots; the Exp-GLASS-* experiments run as part of the track (SRV probe, idle
freeze restore-the-bug, HDR detect, gamma A/B).

### 3.3 Track S — shader decorators

Stage 1 (guaranteed): recorder/replayer shader plumbing (e) + a gradient PS implementing
`linear/radial/conic-gradient` + a small builtin set behind `shader(<name>)` (`glass-panel` etc.).
Stage 2 (gated): the 2-day spike per material-decorators.md §6 — day 1 draws one MD_UI material
in the replay pass via plugin material-shader types + synthetic view UB; day 2 verifies the
blend-mode matrix over text, per-frame MID parameters without game-thread hitching, the
idle-gate interaction (recorded either way), Vulkan + monolithic. **GO** ⇒ `UVaCuusStyleSet`
(game-thread resolution, immutable snapshot to the UI thread through the command queue,
render-thread proxy mirror with deferred release — the thread handoff of
material-decorators.md §4). **FALLBACK** ⇒ builtin tier only, marketing note, plumbing kept so
the tier returns in v1.x without a format break. Either way the decision is recorded with the
blend screenshots, the idle finding, and the measured uniform-expression cost.

### 3.4 Track W — world-space

`UVaCuusWorldComponent` in **VaCuusRender** (the arch spec's `VaCuusUMG` module never existed):
`UMeshComponent` + quad proxy + `UBodySetup` box cloned from `UWidgetComponent`; view created via
the existing `CreateView` with `DrawSize` passed up front (no first-tick resize dance); the
`IVaCuusFrameSink` refactor (g); copy-on-publish into the component's `UTextureRenderTarget2D`;
`M_VaCuusWorldPanel` preset per (i) with the WS-GAMMA decode decision; input per (h) — the
processor owned by the existing `UVaCuusWorldSubsystem` (today an explicit placeholder), refcount
register/unregister, snapshot-consume, capture latch, MouseLeave. Demo: the HUD on a quad in the
M5 map, clicked by raycast in PIE.

## 4. Threading

Nothing new in kind. All RmlUi/JS on the UI thread; glass and the material tier run render-thread
passes fed by recorded data; the style-set snapshot crosses via the command queue; the world
component's game-thread surface is component lifecycle + input math. The two cross-thread
additions — the style-set snapshot and the frame-sink publish — both follow existing seams.

## 5. Diagnostics

Glass: `stat vacuus` gains GlassMs (the per-engine-frame blur+draw cost); a glass list dump on
`vacuus.DumpModel`'s sibling command. Decorators: unknown registry key → RmlUi's per-element
warning (free) + our named log listing known keys; scene-texture material refused with a named
error. World: the processor logs a latched line when a hit component has no live view. CLI: the
conformance test IS the diagnostic for typings drift.

## 6. Budgets

| | Target | Note |
|---|---|---|
| Glass per engine frame (1 panel, half-res blur, 1080p) | ≤0.15 ms RT | measured on the demo; the idle HUD must keep publishing ~nothing (glass is composite-time) |
| Gradient/builtin shader draw | no measurable Record/Replay regression | the M3b harness rows rerun |
| Material draw (if GO) | UpdateUniformExpressionCache + draw ≤0.10 ms/material/frame | the spike's number |
| World copy per published frame (1024²) | ≤0.05 ms RT, ~0 when idle | WS-COPY-COST |
| Preact HUD steady state | JsPump ≤0.30 ms holds with the TSX HUD | the M4 row re-measured on the port |
| Idle, all tracks live | the M3b/M4 exact-zero gates still hold | glass composite excepted by design — it is engine-frame work, not publish work; stated in the table |

## 7. Testing

Restore-the-bug where marked; the M3a/M3b/M4 suites stay green throughout.

- **Facade gaps**: per-gap tests (text node create/write/read; traversal; nodeType; camelCase
  style); the Preact conformance subset in-engine against the committed bundle; E-P1..E-P7 run
  and their observations folded back (each experiment's result recorded in the fork/facade
  commit that acts on it).
- **Glass**: recorder virtuals record what RmlUi sends (unit); tripwires fire on unhashed fields
  **(restore: drop a hash field → the idle gate withholds a changed frame — observed)**;
  Exp-GLASS-IDLE-FREEZE both outcomes; the demo screenshot shows blurred scene through the panel
  (eyeballed + a pixel-variance assertion: the glass region differs from both the sharp scene
  and flat color).
- **Decorators**: gradients render (screenshot vs RmlUi reference expectations); unknown key
  warns; if GO — the blend matrix, MID parameter flow, style-set snapshot thread discipline;
  if FALLBACK — the decision doc + the builtin set renders.
- **World**: local-hit math unit-tested against known transforms; PIE functional test: click a
  button on the quad → the M4 write router fires (the full stack); MouseLeave un-hovers;
  snapshot pass-through (click outside interactive rects reaches the game); WS-GAMMA parity
  screenshot.
- **CLI** (where node exists, controller-run): create → dev → edit → reload roundtrip; the
  committed-bundle test covers the engine side hermetically.

## 8. The demo — acceptance

`vacuus.M5Demo`: the TSX HUD (built by the CLI, committed as a bundle) in PIE with a glass panel
over the moving scene, gradient + builtin-shader decorators visible, and the same HUD on a world
quad the player clicks. AutoShot screenshots; the material gate decision recorded in this spec's
§3.3 outcome note.

## 9. Risks

| Risk | Mitigation |
|---|---|
| Preact needs more DOM than the gap table found | E-P1's logging-proxy cold mount runs FIRST; the gap table is a hypothesis until it passes |
| Swapchain-as-SRV fails on Vulkan | Exp-GLASS-BACKBUFFER-SRV; fallback copy pass is the engine's own shape |
| Material gate fails | the fallback is declared, budgeted, and the plumbing ships regardless |
| TSR ghosting disappoints | documented honestly (i); post-AA/FXAA guidance; not a cure |
| The copy-on-publish RT bridge shows in profiles | WS-COPY-COST; the repoint is pre-verified as the v1.x escape |
| CLI unbuildable in this dev loop | the committed-bundle strategy keeps the engine suite hermetic; node-side smoke is controller-run |

## 10. Architecture-spec corrections found by this research

1. **"The M1 spike ported the GL3 blur chain" is false** — one shader file exists; the chain is
   unbuilt (backdrop-glass.md §3). Arch §5's rendering claims read as shipped; they are plans.
2. **Replay-baked glass contradicts the M2 idle gate** — the arch spec's §5 wording implies
   baking; composite-time is the correct design (2(a)).
3. **`Content/UI/**` was never built** — the watcher roots are `Content/DevUI` (both trees);
   the CLI targets what exists; arch §9 needs the path amended.
4. **The `VaCuusUMG` module does not exist** — the UMG/Slate surface lives in VaCuusRender;
   `UVaCuusWorldComponent` lands there (arch §3's module map is aspirational).

## 11. Out of scope

HDR/pre-tonemap glass (v1.x per arch spec); in-page UI-over-UI backdrop (needs the full layer
stack in the replayer — v1.x); in-engine sourcemap mapping; cylinder/screen-space world modes;
VR interaction rays (the processor seam accepts them later); CSS Grid; the filter trio for
element `filter:`/masks beyond what glass needs (SaveLayerAsTexture etc. — explicitly not
smuggled in with the gate).
