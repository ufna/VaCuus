# Performance guide — the budgets, the numbers, and the idioms that hit them

Everything here is measured, and you can re-run all of it: the workload is the
plugin's own reference HUD (`vacuus.RefHud` — 1,732 nodes at declared steady state,
every driver family live), the instrument is the built-in perf logger
(`vacuus.M1HUD.PerfLog 1`; in packaged Shipping the plugin-parsed launch flags
`-VaCuusRefHud -VaCuusPerfLog`). The full evidence with methods per row is
`docs/passport/2026-08-vacuus-perf-passport.md`; this page is what to DO with it.

**Measurement venue for every number below:** 7950X3D, Linux, Vulkan, offscreen
1920×1080, UE 5.8.1, 2026-08-02 — except the typical-scale UI row's 0.052/0.023,
which are the 2026-07-30 M2-era editor-venue figures (the passport's row 2 marks
the venue). Two columns: Dev (`UnrealEditor -game`) and
cooked-Shipping-Linux (staged, bundle-mounted). Win64/macOS columns are an
owner-hardware handoff (`owner-handoff.md`), never guessed.

## The budget table, measured

| Metric | Budget | Dev | Shipping | Verdict |
|---|---|---|---|---|
| Game-thread cost/frame | ≤ 0.10 ms | 0.012 avg | 0.008–0.009 avg | PASS, ~8–11× headroom |
| UI-thread Update+Record — typical HUD scale | ≤ 0.50 ms | 0.052 avg (M1 HUD) / 0.023 (M2 demo) | bounded above by the reference row | PASS, ~10–20× headroom |
| UI-thread Update+Record — reference worst case (1,732 live nodes) | ≤ 1.2 ms steady avg (re-baselined 2026-08-02) | 1.077 avg | 1.050 avg | **PASS, ~14% avg headroom — see below** |
| JS drivers (pump), informational | — | 0.295 avg | 0.33 avg | blips + killfeed + damage timers |
| Render-thread replay | ≤ 0.50 ms | 0.403 avg / 0.589 p99 | 0.179–0.199 avg | PASS in Shipping (~1.7× at p99); Dev p99 marginal |
| Composite-only (idle UI) | ≤ 0.05 ms | 0.003 avg | 0.002 avg | PASS, ~12–25× |
| Added RAM (incl. JS heap ≤ 16 MB cap) | ≤ 32 MB | bounds only (editor venue) | **+14.3 MB** (A/B median) | PASS, ~2.2× |
| Added disk (Win64 Shipping) | ≤ 10 MB | — | **+3.22 MiB** (Linux proxy) | PASS on the proxy; Win64 literal = owner hw |
| Game-thread hitch on document load | 0 > 1 ms | max 0.018 ms | max 0.038 ms | PASS both venues |

The game-thread row is the product's core promise holding at full scale: at 1,732
nodes with every driver running, the game thread pays ~0.01 ms/frame — queue writes
and a snapshot read, nothing else. The costs live on the UI thread, where they
belong.

## The reference row, and how to stay inside 0.50

The original single 0.50 ms UI-thread budget did not survive the full reference
workload on this machine: steady Update+Record is ~1.05 ms in Shipping (Update alone
~0.65 ms with every driver live). The passport printed the number first, and the
owner then split the row with the reasoning recorded (passport, "The re-baseline",
2026-08-02): **0.50 ms stands at typical HUD scale; the 1,732-node worst case
budgets 1.2 ms steady avg on its own row.** The levers below are how YOUR document
sits inside 0.50 at your scale — they are **document-side** and they are yours:

1. **Fewer per-frame animated surfaces.** Attribution from the soak: the per-frame
   movers dominate (64 rAF blip transforms + 18 keyframe sweeps + per-frame scalar
   bindings land in Update's ~0.65 avg). The sparse scoreboard beats are invisible
   above p99. Animate less every frame and the row comes back fast.
2. **Smaller standing DOM.** Steady-state Record (~0.40 ms) scales with the ~908
   draws/frame command stream. A 52-row killfeed scrollback under `overflow: hidden`
   still records.

The reference HUD deliberately refuses both routes — it exists to stress the row.
A HUD that animates a fraction of its surface per frame (the normal case) sits far
inside the budget: the static-HUD idle numbers below are the other end of the same
axis.

## The laws worth designing against

**The binding law: ~0.53 µs per re-evaluated binding.** M3b measured 0.42257 ms for
one changed row's re-evaluation at 200 rows × 4 bindings = 800 bindings. Cost scales
with bindings × rows in the dirty scope, not with what changed.

**Therefore: split independent arrays (the two-array scoreboard).** The reference
HUD's scoreboard is TWO top-level 24-row × 8-binding arrays, one per team panel,
with independent dirty scopes — verified exact: a one-panel change re-evaluates 192
bindings (192 gets, that panel only), measured +0.084/+0.120 ms on the bump frame
against the ~0.10 ms prediction (passport, Exp-REF-SCALE). One 48-row array would
pay both panels for either's change. Split along your update boundaries.

**The blip idiom: one `transform` write, not `left`+`top`.** Isolated on the 64-blip
driver: transform pump 0.235 ms mean / 0.318 p99 vs left+top 0.379 / 0.675
(Exp-BLIP-DRIVER). Two reasons: each facade style write costs ~2.4 µs, and transform
does not force layout. This is the shipped idiom for anything that moves every frame.

**A unit-bearing data-style binding goes idle — because we patched RmlUi for it.**
Upstream's data-style compare reads the unit-LESS variant of the old value
(`Property::Get<String>()`, Property.h:41-45, where the unit is a separate member),
so a value carrying its unit never compared equal and `SetProperty` fired on every
dirty of the bound variable — every dirty, not every change: one field of a nested
struct or one row of a `data-for` array re-evaluates every binding under that name.
Since a percentage health bar cannot be written without its unit, "bind unit-less
numbers" was advice nobody could take, our own reference HUD included. The vendored
copy now compares `Property::ToString()` (Source/ThirdParty/RmlUi/VENDORED_TAG.txt,
patch #1; bead akj.17), pinned by VaCuus.Model.View.DataStyleIdle: 100 frames of a
dirtied model whose bound values stand still, **0 published** — and **100 of 100
published** with the patch reverted.

**What that cost, exactly, and it is not the same for every property.** A no-op
re-write always costs a restyle, and a whole-document relayout if the property
affects layout — UI-thread time, on the budget the passport measures. It costs a
PUBLISHED frame on top of that only for border and background properties
(`border-*-width`, `border-radius`, `box-shadow`, `background-color`, `opacity`),
whose geometry is re-made with no equality check. A layout-only binding such as
`width` is absorbed by the gate: the box comes out the same, and RmlUi reuses a text
element's compiled geometry when the mesh compares equal. Measured both ways — the
same test bound to `width` alone publishes 0 of 100 with the defect still in.

The compare is now against the property's own spelling, so **write the spelling it
uses**. Numbers and units round-trip (`%.3f` with trailing zeros trimmed, then the
unit — `120px`, `27.9px`, `42%`, and any expression that builds one, which is what
the reference HUD's `Health + '%'` and `row.Ping * 0.3 + 'px'` do). Three things
still re-set on every dirty, exactly as they did before: **colours other than
lowercase `#rrggbb`** (the stored `Colourb` spells itself that way, so `#FF8000`,
`rgba(255,128,0,255)` and named colours all miss — prefer `data-class`);
**shorthands** (`margin`, `padding`, `border` leave no local property under their
own name, so there is nothing to compare against — bind longhands); and literal
spellings a float cannot reproduce (`120.50px`, `1.23456px`). Check yourself with
the PerfLog publish ratio: an "idle" HUD publishing 100% still means something is
re-writing the DOM every frame.

**The quantisation law: round a continuous value to the resolution it is DISPLAYED at,
before you let it dirty anything.** Writing conditionally — the advice above — is
necessary and **not sufficient**, and this is the case that proves it: the write was
conditional, and the condition was true on every single frame. Ship heat in the 2d6 demo
cooled *proportionally*, so it changed every simulation step by some fraction of nothing,
so its change counter bumped every step, so the gated reader re-read its whole domain
every frame, and ~250 elements re-rendered to move a 155-pixel bar by a hundredth of a
pixel. Measured on a UI that looked completely idle: **2.9 % of frames withheld**, i.e.
it published 97 of every 100. Rounding the value to the gauge's own pixel — one
part in 155 — before it reached the counter took that to **93.9 % withheld** and dropped
the JS pump from **6.386 ms to 0.584 ms**, with no visible difference of any kind, because
a hundredth of a pixel was never on screen to begin with.

```js
// 155px bar: 1/155 is the finest change a viewer could ever see.
const shown = Math.round(heat * 155) / 155;
if (shown !== last) { last = shown; model.heat = shown; }   // now the condition is usually false
```

The quantum is the display, not the datum: a 155 px bar needs 1/155, a percentage label
needs 1/100, a two-decimal readout needs 1/100, an icon that is on or off needs 1 bit.
Pick it from what the pixel or the glyph can actually resolve and round there, at the
boundary where the value enters the UI — not inside the simulation, which should keep its
float. This pairs directly with the binding law above: the cost of getting it wrong is
bindings × rows in the dirty scope at ~0.53 µs each, plus the whole render and publish of
a frame that looks identical to the last one, and it is paid for a change no one can see.
A UI that looks idle and publishes 100 % (`vacuus.M1HUD.PerfLog 1`) usually has exactly
one of these in it.

**Font-effect glyphs are a load cost — pay them at load.** `font-effect: glow` added
~4.2 ms to the reference HUD's FIRST Record (5.81 vs 1.65 ms A/B) and nothing to
steady state (0.402 vs 0.395). The spike lands on the UI thread before first publish;
the game-thread load window stayed clean (max 0.018 ms). The document load IS the
warm-up — so load documents with their effects in them, and do not introduce a new
effect font mid-match if you can avoid it (Exp-GLYPH-WARMUP; the 32.5 ms research
figure is the pathology ceiling on large glyph sets).

## The idle gate — what a static UI costs

Publication is withheld when a frame's content hash is unchanged and no resource
traffic occurred. Measured: the static M1 HUD published **1 frame in 6,910** (replay
ran once, 1.49 ms with atlas+upload; after that the entire per-frame render cost is
the 0.003–0.004 ms composite). The glass demo published **0 of 3,363** recorded
frames while sampling the moving scene every engine frame at 0.011 ms — backdrop
blur is composite-time work, deliberately outside the publish gate. The reference
HUD publishes 100% by design (it animates every frame); your pause menu should look
like the M1 number, and `vacuus.M1HUD.PerfLog 1` prints the ratio so you can check.

## Geometry antialiasing: `vacuus.ViewSampleCount`, off by default

Most of a VaCuus frame is antialiased already and costs nothing extra for it: RmlUi's
layout boxes are axis-aligned so their edges land on pixel boundaries, glyphs come out of
an antialiased font atlas, and gradient decorators antialias their own stop edges
analytically in the pixel shader. What is left is **polygon edges** — `border-radius`
arcs, which RmlUi tessellates into a fan, and anything under a non-axis-aligned
`transform`. On a single-sampled target those are a hard staircase, which is what a small
dial ring reads as "broken pixels".

`vacuus.ViewSampleCount` (1 = default, 2, 4, 8) turns on MSAA for the per-view render
target. The draws land in a multisampled companion and the render pass's own store action
resolves them back into the same single-sampled RT — so **nothing downstream changes**:
the Slate composite, the world-panel copy, the mip chain and every screen coordinate are
untouched. Values that are not powers of two round down; the platform maximum still
applies, and the granted count is logged once per view.

**What it buys, counted.** A `border-radius` disc and a rotated bar, measured as pixels
that are neither the fill colour nor the background (`docs/research/proofs/3tg-view-msaa`):

| Shape | `ViewSampleCount 1` | `ViewSampleCount 4` |
|---|---|---|
| `border-radius` disc, 206×206 box | 0 blended pixels | 432 |
| rotated quads, 168×173 box | 0 blended pixels | 819 |
| **axis-aligned boxes (control)** | **0** | **0** |
| **text (control)** | byte-identical | byte-identical |
| **gradient fill (control)** | byte-identical | byte-identical |

Zero is not "few" — a single-sampled rasterizer produces a step function, so no pixel is
ever partially covered. The three controls are the other half of the claim: MSAA multiplies
COVERAGE samples and leaves texture sampling alone, so it cannot soften text or a fill.

**What it costs.** Reference HUD (1,732 nodes, 908 draws/frame, publishing 100% by design),
Dev venue, offscreen 1920×1080, 60 s per row, `vacuus.M1HUD.PerfLog 1`, 2026-08-06:

| `vacuus.ViewSampleCount` | Extra GPU memory per view @1080p | UI Record avg | RT Replay avg / p99 | fps | publish ratio |
|---|---|---|---|---|---|
| 1 (default) | — | 0.429 ms | 0.411 / 0.594 ms | 226.8 | 100% |
| 2 | **+15.82 MiB** | 0.433 ms | 0.408 / 0.594 ms | 226.1 | 100% |
| 4 | **+31.64 MiB** | 0.439 ms | 0.420 / 0.606 ms | 224.5 | 100% |
| 8 | **+63.28 MiB** | 0.435 ms | 0.546 / 0.733 ms | 223.0 | 100% |

Read that table for what it is. **Memory is the honest cost and it is large**: the
multisampled target is `N × 7.91 MiB` at 1080p and it ADDS to the 7.91 MiB RT, which has to
stay because nothing can sample a multisampled texture. At 4× a fullscreen view goes from
7.91 to 39.55 MiB, and that is *per view* — a stack of three fullscreen views pays it three
times. Scale it by your own view size, not by 1080p.

The CPU rows barely move, and they barely move for a reason rather than because MSAA is
free: `Record` is UI-thread command recording and `Replay` is render-thread command
*recording*, and multisampling costs neither — it costs GPU fill and bandwidth. The fps
column is a whole-pipeline number on a run that is CPU-bound at ~225, so it bounds the cost
rather than isolating it. Up to 4× the cost stays invisible here (Replay within noise of the
1× row, fps −1%). **At 8× it stops
being invisible**: Replay avg rises 33% (0.411 → 0.546 ms) as the render thread starts
waiting on the RHI. Treat 8× as the row that needs your own project's GPU profiler before
you ship it, and 2× or 4× as the ones a HUD can afford. The **publish ratio is untouched at
every count** — the idle gate is upstream of all of this, so a static HUD still replays
approximately never and pays the memory and nothing else.

**Turning it off gives the memory back** on the next replayed frame (the release is
asserted by `VaCuus.Render.MSAA.OutputRTUnchanged`), so this is a live quality setting, not
a boot-time one.

One caveat worth stating: the resolve is a box filter over display-encoded premultiplied
pixels. For the case this exists for — a shape's coverage against what is behind it — that
is exactly right, because a premultiplied edge stores `coverage × encode(C)` and the average
of those IS the correct value for the averaged coverage. Where it is the standard MSAA
approximation is an edge between two different opaque colours, which resolves to the encoded
average rather than the encoded value of the linear average. Every downsample of this target
has that property; it is not specific to MSAA.

## World panels: mips ride the same gate

A world panel's render target carries a full mip chain by default (`bGenerateMips`
on `UVaCuusWorldComponent`), regenerated right after the copy on every PUBLISHED
frame — so a panel that shrinks on screen samples a filtered far mip instead of
strobing, and the cost obeys the same idle economics as the copy itself: ~zero on a
static document, once per engine frame with a live material decorator. The price is
one `FGenerateMips` pass per publish (its own `WorldMips` PerfLog line, printed next
to the `WorldCopy` it follows) and ~33% more RT memory. Turn it off — per component,
`SetGenerateMips(false)` at runtime — for a panel that never minifies (pinned
near-1:1 on screen), where the chain buys nothing and the memory is real.

`vacuus.ViewSampleCount` applies to world panels too, and that is deliberate rather than
incidental: because MSAA resolves into the panel's own single-sampled RT at its own
`DrawSize`, the copy's extent guard, its format check and the mip chain all see exactly what
they saw before. (A supersample would not have this property — the sink SKIPS its copy on
any extent mismatch, so an oversized RT would freeze the panel.) The memory multiplier is
the panel's own size, not the screen's: a 1024×1024 panel is 4 MiB, so 4× adds 16.

## Load behavior at scale

Loading the 1,732-node HUD produced **zero game-thread hitches** in both venues.
The honest cost lands on the UI thread, printed so nobody mistakes "no GT hitch" for
"free": DrainCommands max 10.6 ms Dev / 11.2 Shipping (parse + first layout +
document-ready JS seeding of ~600 nodes in one pump), first Update 3.5 ms
(data-for clone build), first Record 5.8 ms incl. the glyph warm-up. In Shipping the
first window also carries Vulkan PSO creation and IoStore first-touch (Update max
13.5 ms in window 1), settling by window 2. If you must photograph frame one,
know what it contains.

## RAM and disk, briefly

Added RAM at reference scale, cooked Shipping, A/B two-run delta at matched quiesced
checkpoints: **+14.3 MB median** (band +11…+19) against the 32 MB gate — with the JS
heap capped at 16 MiB (`JS_SetMemoryLimit`; the reference HUD's steady churn never
even triggered a collection in 95 s). GPU is reported separately, not inside the
32 MB: the per-view RT is 7.91 MiB at 1080p and scales with YOUR view size, and
`vacuus.ViewSampleCount` multiplies that (see the geometry-AA section — 4× is +31.64 MiB
per view on top). Added
disk, Linux Shipping proxy: **+3.22 MiB** total (binary 2.96 MiB + cooked bundle +
shaders), itemized in the passport; the 10 MB budget row's own platform (Win64) is
an owner-hardware literal.
