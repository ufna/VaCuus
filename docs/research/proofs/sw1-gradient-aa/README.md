# VaCuus-sw1 — gradient decorators, screen-space antialiasing

Before/after for the fix in `Shaders/Private/VaCuusGradient.usf`. Both shots are the
same document at the same resolution, taken minutes apart with nothing but the `.usf`
swapped:

```
/w/Unreal/UnrealEngine/Engine/Binaries/Linux/UnrealEditor /w/Unreal/VcHost/VcHost.uproject \
  -game -RenderOffscreen -ForceRes -resx=1920 -resy=1080 \
  -ExecCmds="vacuus.M5Deco, vacuus.M1HUD.AutoShot 10,"
```

The document was a temporary probe living in the VcHost clone's
`Content/DevUI/m5_deco.{rml,rcss}`, reverted with `git checkout` afterwards. Its eight
cells are the eight cases the fix has to get right at once; the two labelled CONTROL are
the ones that must not move at all.

## Files

| File | What it shows |
|---|---|
| `before.png` / `after.png` | the whole 1920x1080 probe |
| `cmp_ring.png` | the reported case — a 12-segment `repeating-conic` ring at 360px |
| `cmp_dial.png` | the 2d6 demo's own `.dial-ring` / `.dial-face` (15 hard stops), verbatim, at 192px and at its shipped 64px |
| `cmp_seam.png` | two non-closing conic discs, seam at 0deg (vertical) and 35deg (diagonal) |
| `cmp_seam35_zoom.png` | the diagonal seam at 16x — the case a naive `fwidth(T)` would have banded |
| `cmp_hatch48.png`, `cmp_hatch12.png` | `repeating-linear-gradient` at 45deg, 48px and 12px periods |
| `cmp_hatchfine.png` | the same at 4px and 2px — bands at and past a pixel |

## Measured, per region (exact per-pixel diff of the two shots)

| Region | pixels changed | max channel delta |
|---|---|---|
| segmented conic ring | 1478 / 129600 (1.1%) | 103 |
| the demo's dial, 192px + 64px | 1158 / 67200 (1.7%) | 64 |
| **two-stop linear 45deg (CONTROL)** | **0 / 144000** | **0** |
| **radial (CONTROL)** | **0 / 144000** | **0** |
| conic seam disc, `from 0deg` | 16 / 129600 | 46 |
| hatch 48px | 8481 / 144000 (5.9%) | 109 |
| hatch 12px | 32628 / 144000 (22.7%) | 119 |
| hatch 4px | 47016 / 70400 (66.8%) | 118 |
| hatch 2px | 70400 / 70400 (100%) | 128 |

The two controls changing by exactly zero bytes is the point: the widening is a
mathematical no-op for any stop pair already more than a pixel apart, and this is that
claim measured rather than argued.

The 16 pixels on the `from 0deg` seam disc are all within 8 rows of the disc's centre, on
the two columns flanking the seam, where the angular rate really is large (1/(2*pi*r) with
r = 1..8). Everywhere else on that seam: zero. The seam there is exactly vertical and a
one-pixel ramp centred on a pixel boundary is sampled at its two endpoints, so a correct
screen-space AA leaves an axis-aligned edge exactly as crisp as it found it.

The 100% figure for the 2px hatch is the sub-pixel-band path: at 45 degrees a 2px period
is 0.7 periods per pixel, past Nyquist, and the shader fades toward the period's mean
colour instead of aliasing. Compare the two halves of `cmp_hatchfine.png`.

## Two scanlines, in numbers

A 45-degree `repeating-linear-gradient` boundary, row 700 of the probe:

```
before: ... 16, 16, 16, |255, 255, 255 ...     <- a step
after : ... 16, 16, 16, | 49, 214, 255 ...     <- a ramp, ~1px (sqrt(2) on a diagonal)
```

The 35-degree conic seam at three radii (r = 30, 60, 85 px), across the wrap:

```
before r=60: (32,48,255) (32,48,255) | (255,48,32) (255,48,32)
after  r=60: (32,48,255) (62,48,225) | (235,48,51) (255,48,32)
```

## What did NOT change, and is not this bead's business

The circle outlines in `cmp_ring.png` and `cmp_dial.png` are staircased in BOTH shots.
Those are `border-radius` arcs — tessellated polygon geometry drawn into a single-sampled
render target, not gradient fill. Assessed and split out as its own bead (VaCuus-3tg).
