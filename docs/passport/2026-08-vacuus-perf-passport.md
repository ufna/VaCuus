# VaCuus performance passport — the §11 table, filled

**What this is** (M6 spec §3.2, arch spec §11): every budget row measured on the reference
workload, with the number AND the method that produced it. Two measured columns: **Dev** (the
UE 5.8.1 Development editor binary run `-game`) and **cooked-Shipping-Linux** (the packaged
Shipping build, bundle-mounted, M==0 asserted). Win64 D3D12 and macOS Metal columns are owner
hardware — never guessed; their checklist is `2026-08-vacuus-manual-matrix.md` beside this file.

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
| 2 | UI-thread Update + Record/frame | ≤ 0.50 ms | **1.077 ms avg / 1.672 ms p99-sum** (Update 0.659/1.003 + Record 0.418/0.669) — **BREACH, ~2.2× over** | **1.050 ms avg / ~1.67 p99-sum** (steady windows: Update 0.630–0.653 avg over the 19 steady windows, median 0.642, p99 1.01–1.03 + Record 0.394–0.404/0.64–0.66) — **the same breach in the row's own venue** | **FAIL at reference scale** (see "The breach" below) | Dev: R1 soak; Shipping: `-VaCuusRefHud -VaCuusPerfLog` soak, steady windows (the boot window runs hot — first-window Update avg 1.40 while Vulkan PSOs and IoStore warm — and is excluded from the steady figure but included in row 7's spike accounting). Update+Record summed per the arch row's definition; JS drivers ride separately in JsPump (row 2b). |
| 2b | JS drivers (pump)/frame — informational, no budget row | — | 0.295 ms avg / 0.460 p99 (blips + killfeed + damage timers) | 0.326–0.331 avg / 0.478–0.487 p99 | — | Same soaks, JsPump scope. Isolation: Exp-BLIP-DRIVER (below). |
| 3 | Render-thread replay (re-replay frames) | ≤ 0.50 ms @1080p | **0.403 ms avg / 0.589 ms p99** @ 908.3 draws/frame — avg PASS, **p99 1.18× over** | **0.179–0.199 ms avg / 0.292–0.298 p99** @ 908.3 draws/frame — **PASS, ~1.7× headroom at p99** | Dev-marginal / **Shipping PASS** | Same soaks, Replay scope; every frame re-replays on this workload (publishes 100%) — the row's worst case by construction. The Dev p99 breach is consistent with Development-RHI overhead (validation, uninstrumented debug paths) [inference — not isolated]; the shipped configuration is the row's venue and passes. |
| 4 | Composite-only frames (idle UI) | ≤ 0.05 ms | **0.003 ms avg / 0.007 p99 / 0.079 max** | **0.002 avg / 0.003–0.004 p99** | PASS (≈12–25×) | Same soaks, Composite scope (graph-build cost; execution under the RDG event). Confirmed idle venue: the static-HUD idle session (row 8b) where composite is the entire per-frame render cost. |
| 5 | Added RAM (reference HUD, incl. JS heap ≤ 16 MB cap) | ≤ 32 MB | +36 MB ± ~15 (bounds, not measures — editor venue; see "RAM" below) | **+14.3 MB (median matched-point delta; band +11…+19)** | **PASS (Shipping, ~2.2× headroom)** | §2(i)'s design: two identical boots, UI on vs off, `FPlatformMemory::GetStats().UsedPhysical` at matched quiesced checkpoints (`-VaCuusMemProbe`, 5 s cadence, t=60–90 s). VFS path in this venue: **Linux resident-buffer** (memory-mapping is Win64-only; the bundle payload is resident in BOTH runs, so the delta attributes the HUD). Dev-only cross-check: the fixed-formula symmetric quantized ledger = +3.85 MiB GMalloc-visible (below). GMalloc canary green. GPU excluded from this row (owner decided 2026-08-02) and reported as its own line below — 32 MB is CPU-side footprint, not total. |
| 6 | Added disk (Win64 shipping) | ≤ 10 MB | not separately measured, by method (see "Disk" below: Dev staging deliberately stages the loose DevUI tree — a Dev delta measures the dev-loop layout, not the product) | **+3,377,065 B (3.22 MiB) — Linux proxy**, itemized below; P0's binary verified plugin-free by string content | **PASS on the Linux proxy (3.1× headroom); Win64 literal = owner hardware** | A/B staged-bytes delta (non-debug): `BuildCookRun -platform=Linux -clientconfig=Shipping -build -cook -stage -pak` with the plugin enabled vs disabled in VcHost.uproject (plus the three plugin-scoped config lines disabled for P0); itemized from the staged tree + `UnrealPak -List`. **Win64 literal: owner hardware** — this is the Linux proxy the research demanded be marked as such. |
| 7 | Frame-drop on document load | 0 GT hitches > 1 ms | **PASS — GT maxima in the load window: GameTick 0.015 / SlateTick 0.012 / OnPaint 0.018 ms.** UI-thread build spike (not a GT hitch, reported by the arch row's own demand): DrainCommands max **10.6 ms** = parse + first layout + **document-ready JS seeding** (52 killfeed rows × 8 nodes via innerRML + 64 blips + 24 pool spans ≈ 600 nodes built in ONE JsPump at ready); first Update **3.5 ms** (data-for clone build); first Record **5.8 ms** incl. **~4.2 ms effect-glyph generation** (Exp-GLYPH-WARMUP: 5.81 with glow vs 1.65 without, steady-state Record identical) | **PASS — GT maxima in the load window: GameTick 0.029 / SlateTick 0.038 / OnPaint 0.004 ms.** UI spike: DrainCommands max **11.2 ms** (same load event); first-window Update max **13.5 ms** / Record max **11.8 ms** — larger than Dev because Vulkan PSO creation and IoStore first-touch land in the same window; settles to steady by window 2 | PASS (GT, both venues); UI spike documented | Dev: R1 boot window vs R2 control (identical run, `font-effect: glow` stripped from refhud.rcss). Shipping: ship-perf boot window. The document-ready seeding is the measured event — the boot IS the warm-up (the HUD boots at declared steady state), and it lands on the UI thread where the game-thread hitch gate structurally cannot see it; that is the architecture doing its job, and the spike numbers are printed here so nobody mistakes "no GT hitch" for "free". |
| 8 | Idle-gate publish ratio (static content) | — (idle economy) | RefHud: **published 100%** of 22,727 frames (animates every frame, by design). Static M1 HUD: **1 publish / 6,910 recorded (100.0% idle windows)**, Replay ran ONCE (1.49 ms, atlas+first upload), composite 0.004 avg is the whole per-frame render cost. Glass idle: **0 publishes / 3,363 recorded** while Glass sampled every engine frame (0.011 avg) | RefHud: published 100% of 22,275 (same by design) | PASS (the gate exists and bites where content is static) | Dev sessions: `vacuus.M1HUD + PerfLog` 35 s; `vacuus.M5Glass + PerfLog` 25 s; Shipping: the RefHud soak's own publish line. |

### The breach (row 2), stated plainly

The 0.50 ms UI row does not survive contact with the full 1,732-node reference workload on this
machine — in Development AND in the row's own venue, cooked Shipping (steady Update+Record
1.05 ms avg there): Update alone averages ~0.65 ms with every driver running. The spec
predicted the risk ("the churn breach already lives in this budget's regime",
refhud-passport.md §3) and pre-assigned the consequence: **breach routes are document-side**
(fewer per-frame animated surfaces, smaller standing DOM), and **REF-COUNT yields to the gates**
(spec §2(g)) — the budgets are gates, the 1,732 count is a marketing claim made checkable. What
this passport does NOT do is quietly re-baseline the budget; the number is printed, the owner
decides: shrink the reference workload, or re-baseline the row with the reasoning recorded.
Attribution from the same soak: the per-frame animated surfaces dominate (rAF blip transforms +
18 keyframe sweeps + per-frame scalar bindings land in Update's 0.66 avg; the sparse scoreboard
beats are invisible above p99 — see Exp-REF-SCALE). Steady-state Record 0.42 avg scales with the
908 draws/frame command stream.

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

- Win64 D3D12 §11 columns + the disk row's Win64 literal (`BuildCookRun -platform=Win64
  -clientconfig=Shipping`, A/B as above).
- macOS Metal §11 columns.
- Win64 IME re-check (bead akj.6.19).
- The matrix checklist: `2026-08-vacuus-manual-matrix.md` (same commands per row).
- ~~The GPU-in-or-out owner decision on row 5~~ — decided 2026-08-02: out, its own line (recorded above).
- 5.6/5.7 matrix builds (SHIM-1).
