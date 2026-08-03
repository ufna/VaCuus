# VaCuus performance passport — the §11 table, filled

**What this is** (M6 spec §3.2, arch spec §11): every budget row measured on the reference
workload, with the number AND the method that produced it. Two measured columns: **Dev** (the
UE 5.8.1 Development editor binary run `-game`) and **cooked-Shipping-Linux** (the packaged
Shipping build, bundle-mounted, M==0 asserted). A third, **Win64 D3D12 Dev**, was measured
2026-08-03 on the owner's desktop and has its own section below the table — it is a **different
machine**, so it is kept out of the main table rather than dropped into a column that would invite
an apples-to-apples reading it does not support. The macOS Metal column is still owner hardware —
never guessed; the checklist is `2026-08-vacuus-manual-matrix.md` beside this file.

**The workload:** `Content/DevUI/RefHud/refhud.rml` — 1,732 nodes at declared steady state
(asserted ∈ [1650, 1850] by `VaCuus.RefHud.Count` and the `vacuus.RefHud.Count` field door),
all driver families live: two 24-row × 8-binding scoreboard arrays (sparse C++ binding, one-panel
bump every 2 s), per-frame model scalars (HP/MP sweep, ammo, objective), 64 minimap blips on a JS
rAF writing ONE `transform` per blip per frame, killfeed churn via JS createElement (1 row/1.5 s,
52-row standing history), damage pool on JS timers (0.9 s beat, 24 recycled spans), 18 buff
sweeps on RCSS keyframes alone, and two `font-effect: glow` surfaces (ammo, objective) so the
effect-glyph pathology is present, not dodged.

**Machine:** 7950X3D, Linux, Vulkan, offscreen 1920×1080 (`-RenderOffscreen -ForceRes`).
**Date:** 2026-08-02, branch `m6-productization`.

---

## The §11 table

| # | Metric | Budget | Dev | cooked-Shipping-Linux | Verdict | Method |
|---|--------|--------|-----|----------------------|---------|--------|
| 1 | Game-thread cost/frame (input + snapshots + paint) | ≤ 0.10 ms | **0.012 ms avg / ~0.021 ms p99-sum** (GameTick 0.004 + SlateTick 0.001 + OnPaint 0.001 + ModelSample 0.006; Input 0 events) | **0.008–0.009 ms avg / ~0.017 p99-sum** (GameTick 0.003 + SlateTick 0.001 + OnPaint 0.000 + ModelSample 0.004 + Input 0.000 × ~790 ev/win) | PASS (≈8–11× headroom) | Dev: 100 s PerfLog soak (R1): `-game -RenderOffscreen -ForceRes -resx=1920 -resy=1080 -ExecCmds="vacuus.M1HUD.PerfLog 1, vacuus.RefHud, …"`, 22,727 frames @227 fps. Shipping: staged build, `-VaCuusRefHud -VaCuusPerfLog`, 95 s, 22,292 frames @234.6 fps, steady windows. Per-frame sum of the once-per-frame GT scopes incl. the new OnPaint (bead akj.6.38). The five rare-EVENT handlers (OnMouseEnter/Leave/CaptureLost, OnFocusReceived/Lost) carry no scope — arch:369's stated unmeasured remainder of rare-event cost, outside every per-frame sum. |
| 2 | UI-thread Update + Record/frame — typical HUD scale | ≤ 0.50 ms | **0.052 ms avg / 0.113 p99 (M1 HUD), 0.023 avg / 0.060 p99 (M2 demo)** — M2-era editor venue @1080p, 13k frames each | not separately measured at typical scale — bounded above by row 2a's Shipping figure | PASS (≈10–20× headroom at the measured scale) | The M2 soak pair (arch §11 row 2's original figures; venue stated there). The 0.50 budget stands at this scale; the reference worst case moved to its own row 2a by the owner's route-B decision (2026-08-02) — see "The re-baseline" below. |
| 2a | UI-thread Update + Record/frame — reference worst case (1,732 always-animated nodes) | ≤ 1.2 ms steady avg (re-baselined 2026-08-02, route B) | **1.077 ms avg / 1.672 ms p99-sum** (Update 0.659/1.003 + Record 0.418/0.669) | **1.050 ms avg / ~1.67 p99-sum** (steady windows: Update 0.630–0.653 avg over the 19 steady windows, median 0.642, p99 1.01–1.03 + Record 0.394–0.404/0.64–0.66) | **PASS (Shipping steady avg, ~14% headroom; an avg budget — p99 printed, not gated)** | Dev: R1 soak; Shipping: `-VaCuusRefHud -VaCuusPerfLog` soak, steady windows (the boot window runs hot — first-window Update avg 1.40 while Vulkan PSOs and IoStore warm — and is excluded from the steady figure but included in row 7's spike accounting). Update+Record summed per the arch row's definition; JS drivers ride separately in JsPump (row 2b). The original 0.50 budget was breached ~2.1× at this scale — the history and the reasoning are in "The re-baseline" below. |
| 2b | JS drivers (pump)/frame — informational, no budget row | — | 0.295 ms avg / 0.460 p99 (blips + killfeed + damage timers) | 0.326–0.331 avg / 0.478–0.487 p99 | — | Same soaks, JsPump scope. Isolation: Exp-BLIP-DRIVER (below). |
| 3 | Render-thread replay (re-replay frames) | ≤ 0.50 ms @1080p | **0.403 ms avg / 0.589 ms p99** @ 908.3 draws/frame — avg PASS, **p99 1.18× over** | **0.179–0.199 ms avg / 0.292–0.298 p99** @ 908.3 draws/frame — **PASS, ~1.7× headroom at p99** | Dev-marginal / **Shipping PASS** | Same soaks, Replay scope; every frame re-replays on this workload (publishes 100%) — the row's worst case by construction. The Dev p99 breach is consistent with Development-RHI overhead (validation, uninstrumented debug paths) [inference — not isolated]; the shipped configuration is the row's venue and passes. |
| 4 | Composite-only frames (idle UI) | ≤ 0.05 ms | **0.003 ms avg / 0.007 p99 / 0.079 max** | **0.002 avg / 0.003–0.004 p99** | PASS (≈12–25×) | Same soaks, Composite scope (graph-build cost; execution under the RDG event). Confirmed idle venue: the static-HUD idle session (row 8b) where composite is the entire per-frame render cost. |
| 5 | Added RAM (reference HUD, incl. JS heap ≤ 16 MB cap) | ≤ 32 MB | +36 MB ± ~15 (bounds, not measures — editor venue; see "RAM" below) | **+14.3 MB (median matched-point delta; band +11…+19)** | **PASS (Shipping, ~2.2× headroom)** | §2(i)'s design: two identical boots, UI on vs off, `FPlatformMemory::GetStats().UsedPhysical` at matched quiesced checkpoints (`-VaCuusMemProbe`, 5 s cadence, t=60–90 s). VFS path in this venue: **Linux resident-buffer** (memory-mapping is Win64-only; the bundle payload is resident in BOTH runs, so the delta attributes the HUD). Dev-only cross-check: the fixed-formula symmetric quantized ledger = +3.85 MiB GMalloc-visible (below). GMalloc canary green. GPU excluded from this row (owner decided 2026-08-02) and reported as its own line below — 32 MB is CPU-side footprint, not total. |
| 6 | Added disk (Win64 shipping) | ≤ 10 MB | not separately measured, by method (see "Disk" below: Dev staging deliberately stages the loose DevUI tree — a Dev delta measures the dev-loop layout, not the product) | **+3,377,065 B (3.22 MiB) — Linux proxy**, itemized below; P0's binary verified plugin-free by string content | **PASS — Win64 literal +3.45 MiB (2.9× headroom), measured 2026-08-03; Linux proxy 3.22 MiB agrees to 7%** | A/B staged-bytes delta (non-debug): `BuildCookRun -platform=Linux -clientconfig=Shipping -build -cook -stage -pak` with the plugin enabled vs disabled in VcHost.uproject (plus the three plugin-scoped config lines disabled for P0); itemized from the staged tree + `UnrealPak -List`. **Win64 literal, 2026-08-03: +3,615,504 B (3.45 MiB)** from the same A/B on `-platform=Win64` — itemized under "Disk (row 6)". The Linux figure remains recorded as the proxy it was; it is now a proxy with its own literal beside it. |
| 7 | Frame-drop on document load | 0 GT hitches > 1 ms | **PASS — GT maxima in the load window: GameTick 0.015 / SlateTick 0.012 / OnPaint 0.018 ms.** UI-thread build spike (not a GT hitch, reported by the arch row's own demand): DrainCommands max **10.6 ms** = parse + first layout + **document-ready JS seeding** (52 killfeed rows × 8 nodes via innerRML + 64 blips + 24 pool spans ≈ 600 nodes built in ONE JsPump at ready); first Update **3.5 ms** (data-for clone build); first Record **5.8 ms** incl. **~4.2 ms effect-glyph generation** (Exp-GLYPH-WARMUP: 5.81 with glow vs 1.65 without, steady-state Record identical) | **PASS — GT maxima in the load window: GameTick 0.029 / SlateTick 0.038 / OnPaint 0.004 ms.** UI spike: DrainCommands max **11.2 ms** (same load event); first-window Update max **13.5 ms** / Record max **11.8 ms** — larger than Dev because Vulkan PSO creation and IoStore first-touch land in the same window; settles to steady by window 2 | PASS (GT, both venues); UI spike documented | Dev: R1 boot window vs R2 control (identical run, `font-effect: glow` stripped from refhud.rcss). Shipping: ship-perf boot window. The document-ready seeding is the measured event — the boot IS the warm-up (the HUD boots at declared steady state), and it lands on the UI thread where the game-thread hitch gate structurally cannot see it; that is the architecture doing its job, and the spike numbers are printed here so nobody mistakes "no GT hitch" for "free". |
| 8 | Idle-gate publish ratio (static content) | — (idle economy) | RefHud: **published 100%** of 22,727 frames (animates every frame, by design). Static M1 HUD: **1 publish / 6,910 recorded (100.0% idle windows)**, Replay ran ONCE (1.49 ms, atlas+first upload), composite 0.004 avg is the whole per-frame render cost. Glass idle: **0 publishes / 3,363 recorded** while Glass sampled every engine frame (0.011 avg) | RefHud: published 100% of 22,275 (same by design) | PASS (the gate exists and bites where content is static) | Dev sessions: `vacuus.M1HUD + PerfLog` 35 s; `vacuus.M5Glass + PerfLog` 25 s; Shipping: the RefHud soak's own publish line. |

### The Win64 D3D12 column, measured 2026-08-03

**Venue.** Owner's desktop, **physical console session** — the morning's SSH pass could not open a
window (bead `VaCuus-5fg`) and could not measure anything at all, because every session died on the
PSO fatal in ~20 s. That fatal is fixed (`b4f12e1`), so this is the first Win64 measurement that
exists. **i9-9900K (8C/16T) + RTX 2080 SUPER**, driver 591.86, Windows 11 26200.8875, UE 5.8.1
Installed CL 56057345, `SM6` on real hardware, commit `6b82e4a`. Method matched to the Dev column
above: `-game -RenderOffscreen -ForceRes -resx=1920 -resy=1080`, 100 s soak,
`vacuus.RefHud + vacuus.M1HUD.PerfLog 1`. **6,880 frames @ 80.8 fps, 908.3 draws/frame** — the draw
count is identical to Linux's, so the workload is the same one.

| # | Metric | Budget | **Win64 D3D12 Dev** | vs Linux Dev | Verdict |
|---|--------|--------|---------------------|--------------|---------|
| 1 | Game-thread cost/frame | ≤ 0.10 ms | **0.069 ms avg** (GameTick 0.014 + SlateTick 0.006 + OnPaint 0.003 + ModelSample 0.042 + Input 0.004); p99-sum 0.144 | 0.012 avg | **PASS on avg** (1.45× headroom); p99-sum over — see below |
| 2a | UI-thread Update + Record/frame, reference worst case | ≤ 1.2 ms avg | **3.001 ms avg** (Update 1.919 + Record 1.082); p99-sum 8.58 | 1.077 avg | **BREACH, 2.5×** — attributed below |
| 2b | JS drivers (pump)/frame | — | 0.719 avg / 1.391 p99 | 0.295 avg | informational |
| 3 | Render-thread replay | ≤ 0.50 ms @1080p | **0.560 ms avg / 0.950 p99** @ 908.3 draws/frame | 0.403 avg / 0.589 p99 | avg **1.12× over** in Dev — the same Dev-marginal shape Linux has; the row's shipped venue is cooked Shipping and is not yet measured here |
| 4 | Composite-only frames (idle UI) | ≤ 0.05 ms | **0.012 ms avg / 0.030 p99** | 0.003 avg | **PASS** (≈4×) |
| 7 | Frame-drop on document load | 0 GT hitches > 1 ms | **PASS — whole-run GT maxima: GameTick 0.178 / SlateTick 0.140 / OnPaint 0.112 ms.** (Whole-run, not load-window-only: a stronger bound than the row asks for, since the load window is a subset — every GT scope stayed under 1 ms for the entire 100 s.) UI-thread build spike, also whole-run maxima and dominated by the load event: DrainCommands **17.1 ms**, Update **33.8 ms**, Record **31.1 ms** | GT max 0.018; DrainCommands 10.6 | **PASS (GT gate)**; UI spike ~1.6–3× the Linux one, tracking the machine |
| 8 | Idle-gate publish ratio | — | RefHud published **100%** of 6,880 (animates every frame, by design). Static M1 HUD: **0 publishes / 2,015 recorded (100.0% idle)**, Replay ran **ONCE** (1.881 ms, atlas + first upload), composite 0.015 avg is the whole per-frame render cost. Glass idle: **0 publishes / 1,075 recorded** while Glass sampled every frame (0.057 avg) | 1/6,910; 0/3,363 | **PASS — the gate bites identically on D3D12** |

Rows 5 (RAM) and 6 (disk) need the Win64 cook and stay with bead `akj.10.3`. Row 2 (typical scale)
was not separately measured on either platform.

**Attribution: the machine, not the platform, and this was tested rather than assumed.**
The Linux Dev column is a **7950X3D**; this is an **i9-9900K** from 2018. On the identical workload
(same node count, same 908.3 draws/frame) Linux runs 227 fps and this desktop runs 80.8 — a **2.81×**
ratio. Update+Record is 3.001 vs 1.077 ms — a **2.79×** ratio. Those agree to within 1%, and every
other CPU-side scope lands in the same 3–7× band. Two controls:

- **VaCuus does not set this machine's ceiling.** Compared like with like — both windowed — the
  static M1 HUD (0 publishes, UI thread effectively free) runs at **80.5 fps** and the full
  1,732-node reference HUD at **76.1 fps**: the entire reference workload costs about **5%** of
  frame rate. The ~80 fps ceiling belongs to the venue, not to the plugin. That is the
  architecture's claim holding on Windows — the expensive UI work is off the game thread, so
  tripling the node count barely moves the frame rate.
- **Exp-FPS-LAW — the tempting explanation, refuted.** A time-driven HUD *ought* to do more work per
  frame when frames are longer, which would make the per-frame breach an artifact of 80 fps rather
  than real work. Predicted at a 30 fps cap: Update+Record ≈ 8.1 ms. **Measured: 2.93 ms** (Update
  1.951 + Record 0.977) against 3.00 ms at 80 fps, and JsPump equally flat (0.782 vs 0.719). The
  drivers are per-frame, not per-second. **Prediction refuted; the per-frame cost is frame-rate
  independent and the Win64 figure is real work on a slower machine.**

**So what does the row 2a breach mean?** It is a genuine miss of a per-frame budget on this
hardware, and it is recorded as a breach, not explained away. What it is *not* is a regression or a
platform defect: the same binary, workload and node count cost 2.79× more per frame on a CPU that is
2.81× slower at this workload overall. In absolute terms the UI thread spends ~3.9 ms of a 12.4 ms
frame here and the game thread still sees 0.069 ms. **The owner call this raises** is whether the
1.2 ms row 2a budget is a per-frame figure at all, or should be stated against a named reference
machine the way row 6's disk literal is stated against a named platform — because as written, the
row silently encodes the 7950X3D. Flagged, not decided.

**Windowed vs offscreen, since this column could finally run both:** the same RefHud soak totals
**76.1 fps windowed** against **80.8 offscreen** — about 6% for the present path. Offscreen is used
for the numbers above to match the Linux method; the visual matrix rows ran windowed. One thing does
change with the venue and it is not a perf figure: the glass back-buffer route (matrix note D).

### The re-baseline (row 2a), decided 2026-08-02 — the section formerly titled "The breach"

The original 0.50 ms UI row did not survive contact with the full 1,732-node reference workload
on this machine — in Development AND in the row's own venue, cooked Shipping (steady
Update+Record 1.05 ms avg there): Update alone averages ~0.65 ms with every driver running. The
spec predicted the risk ("the churn breach already lives in this budget's regime",
refhud-passport.md §3) and pre-assigned the consequence: breach routes are document-side, and
REF-COUNT yields to the gates (spec §2(g)). This passport printed the number without quietly
re-baselining, and the owner then decided — **route B, split the row**: the 0.50 ms budget
stands at typical HUD scale (row 2, measured 10–20× inside), and the reference worst case
carries its own ≤1.2 ms steady-avg budget (row 2a, measured 1.05 — ~14% headroom).

Reasoning, recorded per spec §2(g): (1) the reference HUD exists to be the honest worst case —
shrinking it to fit the old budget would destroy the thing it proves; (2) the cost lives on the
dedicated UI thread — 1.05 ms is ~6% of that thread's 16.6 ms frame, and the game thread pays
row 1's ~0.01 ms regardless; (3) the 0.50 figure was set at M2 scale, when the arch table itself
recorded that the reference margin was "not the final margin", and no external claim had been
published against it.

Attribution from the same soak (unchanged by the decision): the per-frame animated surfaces
dominate (rAF blip transforms + 18 keyframe sweeps + per-frame scalar bindings land in Update's
0.66 avg; the sparse scoreboard beats are invisible above p99 — see Exp-REF-SCALE). Steady-state
Record 0.42 avg scales with the 908 draws/frame command stream. The document-side levers (fewer
per-frame movers, smaller standing DOM) remain the buyer's path to sit inside 0.50 at their own
scale — perf-guide, "The reference row, and how to stay inside 0.50".

### RAM (row 5), all three layers

- **Dev A/B two-run delta** (editor `-game`, matched `-VaCuusMemProbe` schedules, 95 s runs,
  quiesced t=60–90 s): RefHud-on median UsedPhysical 3,090.7 MB (mean 3,091.9); no-UI median
  3,054.9 MB (mean 3,056.1) ⇒ **delta ≈ +35.8 MB (median) / +35.9 (mean)** — but the no-UI
  baseline itself drifted +18 MB across its own window, so the honest Dev statement is
  **+36 MB ± ~15 MB against a ~3.1 GB editor baseline**: this column bounds rather than
  measures (the venue carries editor bookkeeping the product never ships); the Shipping
  column is the primary form (spec §2(i)).
- **Cooked-Shipping-Linux A/B (primary)**: same staged Shipping build, same map, 95 s runs,
  `-VaCuusRefHud -VaCuusMemProbe` vs `-VaCuusMemProbe`, UsedPhysical at matched 5 s checkpoints,
  quiesced t=60–90 s. Matched-point deltas: +33.0 (t=60 — a GC-sawtooth boundary both runs share
  at ~t=62; outlier), then +18.9, +13.3, +14.3, +14.2, +11.1, +15.3 MB ⇒
  **Added RAM ≈ +14.3 MB (median matched-point delta; band +11…+19)** against a ~2.03 GB
  Shipping baseline. Medians-of-windows agree: 2,048.0 − 2,033.8 = **+14.2 MB**.
  **PASS — ~2.2× headroom under the 32 MB gate.** VFS path in this venue: Linux
  resident-buffer (the mounted bundle's 461,881 B payload is resident in BOTH runs — auto-mount
  happens at subsystem init regardless of UI, so the delta attributes the HUD, not the bundle;
  the bundle+module baseline is the P0 comparison under Disk below).
- **Dev-only proxy cross-check (fixed formula)**: `VaCuus.RefHud.MemProxy` — symmetric
  quantized ledger over GMalloc (add `GetAllocationSize` after alloc, subtract before free,
  realloc = sub-old+add-new): RefHud boot → settled steady state = **+4,042,024 B (3.85 MiB)**
  live quantized bytes (251,606 mallocs / 14,158 reallocs / 242,127 frees; **0 size-lookup
  failures** — the exactness bit held); still window +1,008 B over 60 frames (steady state is
  real). Venue: probe host, 1280×720, no view RT — CPU side only.
- **GMalloc canary** (the research's [inference], verified): a context created and destroyed by
  vendored RmlUi code moves the ledger both directions through GMalloc — `Rml::CreateContext`
  allocated (mallocs>0, bytes>0) and `RemoveContext` freed (frees>0, bytes<0) under the proxy.
- **Plugin baseline without UI** (modules + mounted bundle + subsystem, no view): P1's no-UI
  run vs the P0 (plugin-disabled) package's run — P0 sampled externally via `/proc/<pid>/status
  VmRSS`, the very field `FPlatformMemory::GetStats().UsedPhysical` reads on this platform
  (UnixPlatformMemory.cpp:916-917), because `-VaCuusMemProbe` is plugin-parsed and P0 has no
  plugin. P0 late-run 2,043.7–2,055.4 MB vs P1-no-UI 2,022–2,041 MB at similar run age:
  **indistinguishable from run-to-run noise (≤ ~10 MB either way)** — consistent with a
  0.44 MiB resident bundle and idle modules; no number is claimed beyond the bound.
- **JS sub-row**: cap enforced at 16 MiB (`JS_SetMemoryLimit`; boot line `cap=16 MB … boot
  heap=23360 bytes`). Zero collections triggered in the 100 s Dev soak AND the 95 s Shipping
  soak (PerfLog: `JsGC runs=0`, GC step 512 KB) — the HUD's steady churn stays under the step
  budget; heap-at-collection therefore unsampled here (M4's demo-scale figure: ~617 KB at
  collection).
- **GPU, its own line (owner decided 2026-08-02: out of the 32 MB row)**: per-view
  output RT is PF_B8G8R8A8 at view size (VaCuusReplayRenderer.cpp:170-178) = **7.91 MiB @
  1920×1080**; plus UI texture uploads (font/effect-glyph atlases, images) and — only when glass
  is on screen — the pooled half-res pair. The texture inventory is not instrumented (no
  byte counter on the replayer's texture map); the RT figure is arithmetic from the opened
  code, not a measurement. **Recommendation: keep GPU out of the 32 MB CPU row and publish it
  as its own line** — at 1080p the RT alone is a quarter of the budget, and its size is the
  host's view-size choice, not plugin behavior. Counting it in would make the row a resolution
  statement; keeping it out is stated on the row so nobody reads 32 MB as total footprint.
  Owner decided 2026-08-02: out, per the recommendation.

### Disk (row 6), itemized

Two `BuildCookRun -platform=Linux -clientconfig=Shipping -build -cook -stage -pak` packages of
VcHost: P1 = plugin enabled (the shipped story: bundle-mounted, no loose DevUI staged — the
Shipping `RuntimeDependencies` gate held), P0 = plugin disabled (`"Plugins":[{"Name":"VaCuus",
"Enabled":false}]` in the .uproject, plus the three plugin-scoped config lines commented out so
the cooker does not chase `/VaCuus/...` into a plugin that is not there). Staged bytes exclude
`.debug`/`.sym` (a shipped game does not carry them; the with-debug totals are recorded in the
session notes).

| Item | P1 (plugin on) | P0 (plugin off) | Delta |
|---|---|---|---|
| **Staged bytes (non-debug)** | 509,110,520 B | 505,733,455 B | **+3,377,065 B (3.22 MiB)** |
| Monolithic binary | 193,854,000 B | 190,750,216 B | +3,103,784 B (2.96 MiB) |
| VcHost-Linux.ucas (iostore) | 237,261,680 B | 236,999,264 B | +262,416 B |
| VcHost-Linux.pak | 10,512,226 B | 10,509,295 B | +2,931 B |
| VcHost-Linux.utoc + global.ucas + rest | — | — | +7,934 B |

**Added disk (Linux Shipping proxy) = 3.22 MiB — 3.1× under the 10 MB budget.** The itemization
closes: binary 2.96 MiB (all five runtime modules + vendored RmlUi + quickjs-ng, monolithic and
dead-stripped — the Dev editor's 8.8 MB of dynamic .so is the unstripped signpost, not the
product) + 272 KB of containers (the 161.5 KB cooked bundle + ~18 KB materials + ~70 KB shader
archive growth + uplugin/ini) ≈ the 3,377,065 measured, within 1 KB of manifests. P0's binary
verified plugin-free by content, not assumption: zero "VaCuus" strings in the P0 binary vs 98
in P1 (and P0's own run ignored `-VaCuusMemProbe` entirely — no parser present).

Inside the P1 containers (from `UnrealPak <utoc> -List`): **DevUIBundle.uasset 161,515 B in the
container** (the whole 24-entry UI tree; 461,881 B mounted payload — compression accounts for
the difference), M_VaCuusWorldPanel 2,409 B, six Spike materials ≈ 15.9 KB,
`VaCuus.uplugin` 896 B + `FilterPlugin.ini` 18 B (pak), ShaderArchive-Global 220,516 B +
ShaderArchive-VcHost 50,404 B (the VaCuus global-shader permutations live inside the global
archive; the A/B delta attributes them). **Win64 shipping literal (the row's own platform):
owner hardware** — this Linux figure is the proxy the research demanded be marked as such.

Dev column for this row: **not separately measured, by method** — Development staging
deliberately stages the loose DevUI tree beside the bundle (the `RuntimeDependencies` globs
gate on `Target.Configuration != Shipping`), so a Development staged delta measures the
dev-loop file layout, not the product. The Shipping A/B is the row's own venue.

#### The Win64 literal, measured 2026-08-03 — the row's own platform, no longer a proxy

Same method, on Win64: two `BuildCookRun -platform=Win64 -clientconfig=Shipping -build -cook -stage
-pak` packages, P1 = plugin enabled, P0 = plugin disabled in the `.uproject` **plus** the three
plugin-scoped config lines commented out (`+DirectoriesToAlwaysCook` ×2 and `BundleAssetPath` in
`DefaultGame.ini`, `+IncrementalClassAllowList` in `DefaultEditor.ini`). Staged bytes exclude
`.pdb` — the Windows debug artifact, and a large one here: 259 MB of it, which is why the
with-debug totals (P1 732,712,802 B / P0 697,246,802 B) are recorded but not used.

| Item | P1 (plugin on) | P0 (plugin off) | Delta |
|---|---|---|---|
| **Staged bytes (non-debug)** | 473,562,978 B | 469,947,474 B | **+3,615,504 B (3.45 MiB)** |
| Monolithic binary (`TP_ThirdPerson-Win64-Shipping.exe`) | 169,001,472 B | 166,074,368 B | +2,927,104 B (2.79 MiB) |
| `VcHost-Windows.ucas` (iostore) | 224,444,416 B | 223,771,520 B | +672,896 B |
| `VcHost-Windows.pak` | 11,243,420 B | 11,236,996 B | +6,424 B |
| `VcHost-Windows.utoc` | 213,163 B | 210,422 B | +2,741 B |
| `global.ucas` | 3,226,992 B | 3,221,584 B | +5,408 B |
| `global.utoc` | 794 B | 794 B | 0 |

**Added disk (Win64 Shipping, literal) = 3,615,504 B = 3.45 MiB — 2.9× under the 10 MB budget.**
The itemization closes to **931 bytes** (the staged manifest text files), the same order of
agreement the Linux itemization reached. File counts are identical at 29 non-debug files in both
legs, so nothing was added or dropped as a whole file — the delta is entirely growth inside the
binary and the containers, which is what a bundle-mounted plugin with no loose staged content
should look like.

**The Linux proxy was a good proxy, and now it is possible to say so with a number instead of a
hope:** 3.22 MiB predicted, **3.45 MiB measured** — 7% apart, same shape (binary dominates at
~2.8–3.0 MiB, containers a few hundred KB). The research demanded row 6 be marked as a proxy until
the literal existed; it exists.

### The named experiments

| Experiment | Predicted | Measured | Verdict |
|---|---|---|---|
| Exp-REF-COUNT | 1,732 (published arithmetic) ∈ [1650, 1850] | **1,732**, automation (twice, 40 sim frames apart) AND the field door (`vacuus.RefHud.Count 12` in the live soak) | PASS, exact |
| Exp-REF-SCALE (dirty scope) | one-panel bump re-evaluates only that panel: 24 rows × 8 bindings = 192 | **192 gets / 1 size / 192 childs** per panel; both-panel = exact sum 384/2/384; plate scalar 2/0/0 | PASS, exact |
| Exp-REF-SCALE (soak half) | ~0.53 µs/binding law ⇒ one-panel change ≈ **~0.10 ms** | Controlled venue (DirtyScope rig, probe host 1280×720, blips live): bump-frame `Context::Update()` **0.683 / 0.719 ms** (alpha/bravo) vs still median 0.599 ⇒ **extra 0.084 / 0.120 ms** — the two panels bracket the 0.10 prediction. Field soak: a 2 s beat is 2–3 frames per 1,090 — beat frames vanish above p99 into the animation tail (window p99 0.94–1.03), i.e. the beat cost sits below the full-motion soak's noise floor, which is the two-array topology doing its job | **PASS — prediction verified** |
| Exp-BLIP-DRIVER (isolation, probe host, 500 frames) | transform single-write ≈ half of left+top (1 vs 2 facade ops) | transform pump **0.235 ms mean / 0.318 p99**; left+top **0.379 / 0.675** (this session; Task 4's run: 0.255/0.438 vs 0.355/0.552) | PASS — single transform write is the shipped idiom |
| Exp-BLIP production figure | — | JsPump 0.295 ms avg (Dev soak) / 0.326–0.331 avg (Shipping soak) — blips + killfeed + damage together | recorded |
| Exp-GLYPH-WARMUP | font-effect glyph gen is a measured 32.5 ms pathology class; the architecture must keep it off the game thread | effect-glyph generation ≈ **4.2 ms, UI thread, first Record only** (5.81 vs 1.65 A/B); GT load window clean (max 0.018 ms); steady-state Record unchanged (0.402 vs 0.395) | PASS — the spike exists, lands off the game thread, before first publish |
| Exp-RAM-DELTA | — | Shipping A/B +14.3 MB median (primary); Dev A/B +36±15 (bounds); proxy ledger +3.85 MiB; canary green | PASS |
| Exp-DISK-DELTA | — | +3.22 MiB Linux Shipping proxy, itemization closes to within 1 KB (see Disk) | PASS (proxy); Win64 literal owner hw |

### Cooked-Shipping gate (M6 acceptance line 1)

**GREEN, both workloads, from the cooked bundle** (staged Shipping, 2026-08-02):

- Mount: `Mounted bundle '/VaCuus/Bundles/DevUIBundle.DevUIBundle': 24 entries, 461881 bytes,
  resident buffer (FPlatformProperties::SupportsMemoryMappedFiles() is false on this platform)` —
  **the Linux resident path asserted by its own log line.**
- RefHud soak (`-VaCuusRefHud -VaCuusPerfLog`): teardown `4 open(s) served by mounted bundles,
  **0 by loose roots**` AND `1 script read(s) served by mounted bundles, **0 by loose roots**` —
  **M==0 on BOTH lines**; zero `LogVaCuusJS: Error`; clean teardown (UI thread in-band stop,
  zero unpublished NEW resources); gate screenshot at t+8 read by eye (full HUD, beats landed).
- M5 demo (`-VaCuusM5Demo`): `5 open(s) … 0 by loose roots` + `2 script read(s) … 0 by loose
  roots`; translation table published; both views bound; zero JS errors; the t+8 screenshot reads
  identically to the M5 T9 proof (TSX HUD Health 60, translated rows, glass, world quad).

### Owner-hardware handoff (never guessed)

- ~~Win64 D3D12 §11 columns~~ — **measured 2026-08-03**, own section above. Rows 1, 2a, 2b, 3, 4, 7
  and 8 filled on the Dev leg; the cooked-Shipping-Win64 leg is still owed and rides with the cook.
- The disk row's Win64 literal (`BuildCookRun -platform=Win64 -clientconfig=Shipping`, A/B as
  above) — still owed, bead `akj.10.3`. Row 5 (RAM) on Win64 rides with the same cook.
- macOS Metal §11 columns.
- Win64 IME re-check (bead akj.6.19) — the **precondition** is now established on a real console
  session (`ITextInputMethodSystem present`, bridge `registered=yes, context active=yes`); what
  remains is composing through a real IME with a human at the keyboard.
- The matrix checklist: `2026-08-vacuus-manual-matrix.md` (same commands per row).
- ~~The GPU-in-or-out owner decision on row 5~~ — decided 2026-08-02: out, its own line (recorded above).
- 5.6/5.7 matrix builds (SHIM-1).
