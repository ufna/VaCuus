# VaCuus-3tg — the per-view render target's opt-in MSAA

Before/after for `vacuus.ViewSampleCount`, the knob added in
`Source/VaCuusRender/Private/VaCuusReplayRenderer.cpp`. Both shots are the same document
at the same resolution, minutes apart, with nothing but the cvar changed:

```
/w/Unreal/UnrealEngine/Engine/Binaries/Linux/UnrealEditor /w/Unreal/VcHost/VcHost.uproject \
  -game -RenderOffscreen -ForceRes -resx=1920 -resy=1080 \
  -ExecCmds="vacuus.ViewSampleCount 1, vacuus.M5Deco, vacuus.M1HUD.AutoShot 10,"
```

The document was a temporary probe living in the VcHost clone's
`Content/DevUI/m5_deco.{rml,rcss}` — the same slot sw1's probe used — reverted with
`git checkout` afterwards. Six cells: three carrying the geometry this bead is about, and
three CONTROLS that must not move a byte.

Venue: 7950X3D / RTX 3090 / Linux / Vulkan / UE 5.8.1, 2026-08-06, offscreen 1920×1080.

## Files

| File | What it shows |
|---|---|
| `before.png` / `after.png` | the whole six-cell probe at 1× and 4× |
| `cmp_dial.png` | the reported artefact — the demo's dial ring at 192px, 4× zoom |
| `cmp_dial64.png` | the same ring at its shipped 64px, 8× zoom. This is the "broken-pixel" report |
| `cmp_ticks.png` | four rotated quads — what the demo's ring became when it was rebuilt out of 24 rotated elements |
| `cmp_disc.png` | a plain `border-radius` disc with no decorator anywhere near it |

## Measured, per region (exact per-pixel diff of the two shots)

| Region | pixels changed | max channel delta |
|---|---|---|
| dial, 192px + 64px (border-radius arcs) | 548 / 144000 (0.38%) | 119 |
| rotated quads | 819 / 144000 (0.57%) | 114 |
| disc, border-radius 50% | 432 / 144000 (0.30%) | 107 |
| **axis-aligned boxes (CONTROL)** | **0 / 144000** | **0** |
| **text (CONTROL)** | **0 / 144000** | **0** |
| **gradient, sw1 analytic AA (CONTROL)** | **0 / 144000** | **0** |

The three controls changing by exactly zero bytes are the point, and each one rules out a
different way this could have been the wrong fix:

- **Axis-aligned boxes.** A coverage-sampled rasterizer must leave an edge that lands on a
  pixel boundary exactly as crisp as it found it. It does.
- **Text.** This is the measurement that decided against supersampling the view, which was
  the other candidate on the bead. Glyphs come out of an already-antialiased font atlas, so
  there is nothing for antialiasing to add — and a supersample would have RESAMPLED them
  (bilinear magnify by N composed with the N:1 downsample is a [0.125, 0.75, 0.125] kernel
  at N=2), softening the one thing here that was already right. MSAA multiplies coverage
  samples and leaves texture sampling alone, so the bytes are identical.
- **Gradient.** sw1's screen-space stop AA is derivative-based, and the interior of a
  gradient fill is one triangle pair with no interior edges. Zero confirms the two fixes
  are disjoint rather than overlapping.

## Counted the sw1 way: pixels that are neither foreground nor background

The sharper form of the same evidence, on the two cells whose fill is a single flat colour,
measured over the shape's own bounding box so the panel titles cannot contribute:

| Shape | 1× | 4× |
|---|---|---|
| disc `#E8402A` on `#12161C`, 206×206 box | **0** | **432** |
| rotated quads `#7FE3FF` on `#12161C`, 168×173 box | **0** | **819** |
| axis-aligned bars `#FFC820` (CONTROL), 406×46 box | 0 | 0 |
| axis-aligned bars `#34C759` (CONTROL), 406×46 box | 0 | 0 |

A single-sampled rasterizer produces a step function, so the count of partially covered
pixels is not "few", it is exactly zero. That zero IS the defect.

## The same number, in the suite

`VaCuus.Render.MSAA.TessellatedDiscEdgeAA` measures it without a screenshot: a 48px-radius
triangle fan (RmlUi's own corner construction) replayed through the production
`FVaCuusReplayRenderer::Replay`, read back, partially-covered pixels counted.

```
VaCuus.Render.MSAA.TessellatedDiscEdgeAA: partially covered pixels 0 at 1x, 200 at 4x
  (r=48, 128 px target)
```

Restore-the-bug, same run venue: with `bResolve` in `ReplayCommands` forced false — the
multisampled target still created, but the pass binding `OutputRT` with `Clear_Store`,
which is exactly the pre-3tg pass — the run reported `0 at 1x, 0 at 4x` and failed on

```
Expected 'multisampled: the boundary is softened (0 partial pixels, expected > 75)' to be true.
```

The file's other two tests passed throughout that run, which is the right split: the target
was still being created and `OutputRT` was still untouched, only the resolve was gone.

## Hit-testing did not move

The knob changes no coordinate anywhere — the view size, the interactive-region snapshot,
`SVaCuusWidget::ToViewPixels` and the IME rects are all untouched, because MSAA resolves
into the same single-sampled `OutputRT` at the same extent. Measured rather than argued: the
identical script at 1× and at 4× on `vacuus.M1HUD`,

```
-ExecCmds="vacuus.ViewSampleCount N, vacuus.M1HUD, vacuus.M2Demo.Rects 6,
           vacuus.M2Demo.Hit 868 1026 7, vacuus.M2Demo.Hit 125 987 7,
           vacuus.M2Demo.Hit 868 900 7, vacuus.M2Demo.Hit 500 500 7,
           vacuus.M2Demo.Drag 868 1026 868 1026 1 8,
           vacuus.M2Demo.Drag 500 500 500 500 1 10,"
```

produced 13 log lines each, **identical except the snapshot's `generation=` counter** (how
many UI frames had elapsed when the dump ran — not a coordinate):

```
Rects: view 1 'm1_hud.rml' viewSize=1920x1080 rects=6 cursor=1 ...
Rects:   [ 0] (  16,1012)-( 218,1062) centre ( 117,1037) Interactive|Focusable
Rects:   [ 1] (  16, 970)-( 234,1004) centre ( 125, 987) Interactive|Focusable|TextInput
Rects:   [ 2] ( 841, 999)-( 895,1053) centre ( 868,1026) Interactive|Focusable
Rects:   [ 3] ( 903, 999)-( 957,1053) centre ( 930,1026) Interactive|Focusable
Rects:   [ 4] ( 965, 999)-(1019,1053) centre ( 992,1026) Interactive|Focusable
Rects:   [ 5] (1027, 999)-(1081,1053) centre (1054,1026) Interactive|Focusable
Hit (868,1026): covered=yes focusable=yes textInput=no  -- Handled (the UI takes it)
Hit (125,987):  covered=yes focusable=yes textInput=yes -- Handled (the UI takes it)
Hit (868,900):  covered=no  focusable=no  textInput=no  -- Unhandled (it reaches the game)
Hit (500,500):  covered=no  focusable=no  textInput=no  -- Unhandled (it reaches the game)
Dragged (868, 1026) -> ... the press was taken by THE UI (VaCuus captured the mouse)
Dragged (500, 500)  -> ... the press was taken by THE GAME (VaCuus declined, ...)
```

Both halves matter. A click landing on a button proves the rects did not shift; a click on
empty space still reaching the game proves they did not GROW — which is the failure a
coordinate scale would produce and a coverage-only check would miss.
