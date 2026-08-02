# Performance guide — the budgets, the numbers, and the idioms that hit them

Everything here is measured, and you can re-run all of it: the workload is the
plugin's own reference HUD (`vacuus.RefHud` — 1,732 nodes at declared steady state,
every driver family live), the instrument is the built-in perf logger
(`vacuus.M1HUD.PerfLog 1`; in packaged Shipping the plugin-parsed launch flags
`-VaCuusRefHud -VaCuusPerfLog`). The full evidence with methods per row is
`docs/passport/2026-08-vacuus-perf-passport.md`; this page is what to DO with it.

**Measurement venue for every number below:** 7950X3D, Linux, Vulkan, offscreen
1920×1080, UE 5.8.1, 2026-08-02. Two columns: Dev (`UnrealEditor -game`) and
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

**A unit-bearing data-style binding never goes idle.** The data-style compare reads
the unit-less variant of the old value, so a value carrying its unit never compares
equal — `SetProperty` fires every Update and the surface re-publishes every frame
even when nothing visibly changed (vendored DataViewDefault.cpp:168-170 with
Property.h:41-45; bead akj.17, deliberately not patched in the vendored tree).
Bind unit-less numbers and keep units in the stylesheet. Check yourself with the
PerfLog publish ratio: an "idle" HUD publishing 100% has one of these somewhere.

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
32 MB: the per-view RT is 7.91 MiB at 1080p and scales with YOUR view size. Added
disk, Linux Shipping proxy: **+3.22 MiB** total (binary 2.96 MiB + cooked bundle +
shaders), itemized in the passport; the 10 MB budget row's own platform (Win64) is
an owner-hardware literal.
