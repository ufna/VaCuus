# VaCuus-4ik — the clip-mask stencil pass

Before/after for the stencil pass added to `FVaCuusReplayRenderer::ReplayCommands`. "Before"
is the same binary with the pass disabled by two lines in the build clone — `BufferUsesClipMask`
forced to `return false` (so no stencil is attached) and the `RenderToClipMask` case returning
immediately — which is exactly the code path that shipped before this bead. Both edits were
reverted afterwards; the clone diffs clean against the source tree.

Venue: 7950X3D / RTX 3090 / Linux / Vulkan / UE 5.8.1, 2026-08-06, offscreen 1920×1080,
`vacuus.M2Demo` with a temporary probe in the clone's `Content/DevUI/m2_demo.rcss`.

## Files

| File | What it shows |
|---|---|
| `cmp_transform.png` | **the headline.** `#p-list { transform: scale(0.92) }` — an ancestor of `#list-clip` |
| `before_transform.png` / `after_transform.png` | the same two, full frame |
| `cmp_corner.png` | `border-radius: 14px` on `#list-clip`, bottom-left corner at 4× zoom |
| `before_border_radius.png` / `after_border_radius.png` | the same two, full frame |

## The transform case — the one the bead is named for

`cmp_transform.png`. On the left, with the mask skipped, the list renders **completely
unclipped**: rows 09 through 17 spill out of a 176px-tall container and paint over the terrain,
past the panel, down the screen. On the right the same document clips at row 08, mid-glyph,
exactly where the container ends.

That is the whole defect in one image, and the mechanism is both halves of
`ElementUtilities::ApplyActiveClipRegions`:

- `:174-175` — `if (transform) disable_scissor_clipping = true;`, unconditional. A transformed
  element's geometry may project anywhere, so a screen-space rectangle can no longer describe it.
- `:162-169` — a clip **mask** is pushed instead.

Skip the mask and there is nothing left. No Error, no Warning, no log line.

## The border-radius case — and a correction

`cmp_corner.png`. The difference is **one rounded corner**, and an exact per-pixel diff over the
list container's region says so: **11.7 differing pixels in a 10×8 box at the bottom-left corner**,
and nothing anywhere else in the container.

**This corrects a claim that was in the bead brief, in `gotchas.md` #8a and in both demo sheets'
headers**: that `border-radius` on a clip container leaves it *unclipped*, the same way a
transform does. It does not, and the vendored source says why in a comment three lines below the
one everybody was citing:

```
// If we only have border-radius then we add this element to the scissor region as well as the
// clip mask. This may help with e.g. culling text render calls. However, when we have a
// transform, the element cannot be added to the scissor region since its geometry may be
// projected entirely elsewhere.
if (transform)
    disable_scissor_clipping = true;
```
— `Source/ThirdParty/RmlUi/Source/Core/ElementUtilities.cpp:171-175`

So with `border-radius` and no transform, the scissor **stays on**. The box still clips
rectangularly; only the corner arcs go unclipped, because only they need the mask. The failure
was real but it was cosmetic, and confined to four corners — not "the list renders over the rest
of the screen", which is what the demo sheets claimed and what a reader would have budgeted for.

Both demo sheet headers and `gotchas.md` #8a now say the accurate thing.

## Cost

Reference HUD (`vacuus.RefHud`, 1,732 nodes, publishing 100% by design), 65 s per row,
`vacuus.M1HUD.PerfLog 1`, same venue and day:

| Path | `ViewSampleCount` | stencil per view @1080p | draws/frame | UI Record avg | RT Replay avg / p99 | fps | publish |
|---|---|---|---|---|---|---|---|
| scissor (no mask) | 1 | **none** | 908.3 | 0.527 ms | 0.442 / 0.743 ms | 207.1 | 100% |
| scissor (no mask) | 4 | **none** | 908.3 | 0.519 ms | 0.442 / 0.766 ms | 208.4 | 100% |
| clip mask | 1 | **7.91 MiB** | 991.1 | 0.654 ms | 0.452 / 0.724 ms | 219.4 | 100% |
| clip mask | 4 | **31.64 MiB** | 991.6 | 0.657 ms | 0.449 / 0.699 ms | 216.0 | 100% |

**Read the rows for what they are.** The two "clip mask" rows are not a clean A/B: forcing the
reference HUD onto the mask path meant changing the *document* (`border-radius` on both clip
containers plus `transform: scale(0.95)` on `body`), and that is worth +83 draws/frame of rounded
corner geometry on its own. So:

- **`Record` +0.13 ms is mostly not this pass.** It is RmlUi building rounded clip geometry and
  a mask list on the UI thread — work the recorder then writes down. The stencil pass itself does
  nothing on the UI thread.
- **`Replay` is the number that isolates the pass, and it barely moves**: +0.010 ms avg (+2.3%),
  with p99 *lower* than the scissor rows at both sample counts. The mask draws are stencil-only
  (`CW_NONE`) and the reference document rebuilds its mask a handful of times per frame.
- **fps is not a signal here.** It went *up*, because `scale(0.95)` shrinks the fill area.
- **The publish ratio is untouched**, which is the property that matters most: the idle gate sits
  upstream of all of this, so a static HUD that clips still replays approximately never.

**Memory is the honest cost**, exactly as it was for MSAA: `N × 7.91 MiB` at 1080p, *per view*,
for as long as the view lives. It is allocated **lazily** — a document that never records a
`RenderToClipMask` never allocates it, which is every document that shipped before this bead, and
`VaCuus.Render.ClipMask.LazyStencilAllocation` asserts exactly that.

## Restore-the-bug

Beyond the two screenshots above, each GPU test in
`Source/VaCuusRender/Private/Tests/VaCuusClipMaskTest.cpp` carries its own negative control in the
same test body — the identical buffer with the two mask commands removed, which must fail the clip
assertion. `SurvivesTransform` additionally pins the discriminator at x = 48, which is inside the
*scaled* mask and outside the unscaled one, so "the transform was ignored" and "the mask was
dropped" fail differently and neither can pass by accident.
