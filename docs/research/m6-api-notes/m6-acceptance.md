# M6 Task 8 — the acceptance walk (2026-08-02)

**What this is** (plan 8.1–8.3, spec §5): the packaged gates rerun on the bundle at the
milestone's final HEAD, the remaining gates, and the six acceptance lines walked with named
evidence. HEAD: `333e2ec` (`fix: the scan fails closed, the seam routes every site`), branch
`m6-productization`, tree clean. Machine: the passport's (7950X3D, Linux, Vulkan, UE 5.8.1
source, host `/w/Unreal/VcHost`). Every packaged run below is headless
`-RenderOffscreen -ForceRes -resx=1920 -resy=1080`; every screenshot was read by eye and what
was seen is written next to it.

## 1. The packaged gates, rerun ON THE BUNDLE at this HEAD (plan 8.1)

Code changed since Task 5's stage (333e2ec touched `VaCuusEngineCompat.h` /
`VaCuusMaterialDraw.cpp`), so both configs were repackaged with Task 3/5's exact recipe —
`RunUAT.sh BuildCookRun -project=/w/Unreal/VcHost/VcHost.uproject -platform=Linux
-clientconfig=<cfg> -build -cook -stage -pak`, `VaCuus.Build.cs` touched first (the
stale-receipt trap, M5 T9) — **Development first, then Shipping** (the standing user
directive). Both UAT runs: `BUILD SUCCESSFUL`, ExitCode=0. Both cooks packed the identical
bundle — determinism observed again across configs and processes:

```
LogVaCuus: Display: Bundle '/VaCuus/Bundles/DevUIBundle.DevUIBundle': packed 24 file(s),
461881 bytes, hash adcb1da0b34dffdb071d3f9db02fd780eceb1f4e700eae66ed79966ed8015017
(0 shadowed duplicate(s), 3 test fixture(s) excluded)
```

Staging shape, verified from `Manifest_UFSFiles_Linux.txt`: Development stages the loose DevUI
tree BESIDE the bundle (28 `DevUI` entries — the dev-loop layout; bundle-first precedence is
exactly what M==0 then proves), Shipping stages **only** `DevUIBundle.uasset` (1 entry — the
`RuntimeDependencies` Shipping gate held). All four runs mounted the same bundle and logged the
Linux resident path by its own line, hash matching the cook:

```
LogVaCuus: Mounted bundle '/VaCuus/Bundles/DevUIBundle.DevUIBundle': 24 entries, 461881 bytes,
resident buffer (FPlatformProperties::SupportsMemoryMappedFiles() is false on this platform),
hash adcb1da0b34dffdb071d3f9db02fd780eceb1f4e700eae66ed79966ed8015017
```

Per run — the two M==0 teardown lines verbatim (counts differ per workload; the per-bundle
`ServedOpens` reconciles as opens + script reads, the single-session venue the counter comment
names), the JS-error grep, the warning inventory, and the screenshot as read:

| Config / workload | VFS teardown lines (verbatim counts) | ServedOpens | `LogVaCuusJS: Error` | LogVaCuus Warnings |
|---|---|---|---|---|
| Development `-VaCuusM5Demo` | `5 open(s) served by mounted bundles, 0 by loose roots` · `2 script read(s) served by mounted bundles, 0 by loose roots` | 7 = 5+2 | 0 | 1 — the documented Linux no-`ITextInputMethodSystem` line only |
| Development `-VaCuusRefHud` | `4 open(s) … 0 by loose roots` · `1 script read(s) … 0 by loose roots` | 5 = 4+1 | 0 | same single documented line |
| Shipping `-VaCuusM5Demo` | `5 open(s) … 0 by loose roots` · `2 script read(s) … 0 by loose roots` | 7 = 5+2 | 0 | same single documented line |
| Shipping `-VaCuusRefHud` | `4 open(s) … 0 by loose roots` · `1 script read(s) … 0 by loose roots` | 5 = 4+1 | 0 | same single documented line |

**M==0 on BOTH lines in all four runs.** Both teardown lines present in every log ⇒ the file
interface destructed (clean RmlUi session teardown); every run ended `UI thread stopping on an
in-band shutdown command` → `RmlUi shut down` → recorder destroyed with **zero unpublished NEW
resources** (released-side traffic only) → `Bundle mount table: 1 record(s) destroyed at module
shutdown` → `RequestExit(... ReturnCode=143)` → `Log file closed`. No crash, no callstack, in
any log. Shipping boot line confirmed the Shipping constants live (`JS runtime created: cap=16
MB, stack=256 KB, watchdog=50 ms`); Development runs show `watchdog=250 ms`, as designed. M5
demo runs: `translation: published table v1 (2 entries)`, `model 'hud' bound` on both views;
RefHud runs: `model 'refhud' bound … 9 of 9 top-level variable(s)`.

**The screenshots (t+8 s gate shot, each read by eye):**

- **Dev M5 demo**: "VaCuus M5 // TSX HUD" over the gradient strip, Health **60** with the
  red→green model-fed bar, the Kill Feed glass panel visibly blurring the clouds behind it with
  the Simulate button and five TRANSLATED rows ("Rasp » Vex", "Unto » Kilo", "Vex » Moth",
  "Kilo » Rasp", "Moth » Unto" — the arrow table), and the world quad mid-right running the
  SAME document (own title bar, health bar, five-row killfeed). Reads as the M5 T9 proof.
- **Dev RefHud**: the full 1,732-node HUD — UFNA-01 plate HP **60.101** / MP 0.063 (mid-sweep,
  the pipeline live), both 24-row boards (ALFA-RAPTOR-01…BRVO-HAVOC-24) with K/D/A/score/ping
  and the 2 s stat beats visibly landed, killfeed right edge full (orange killers, weapon
  labels, red HS pills), compass N centered, 18-slot buff bar, minimap blip cloud + "VaCuus
  RefHud" overlay, ammo **14 / 120** in the warm glow, three rows of damage numbers, crosshair,
  ability bar, SYSTEM // SETTINGS panel. The known cosmetic objective/scoreboard-header overlap
  (matrix row 1's recorded finding) is present, as recorded — not a gate.
- **Ship M5 demo**: reads identically to the Dev M5 shot (Health 60, same five translated
  rows, glass over clouds, world quad live).
- **Ship RefHud**: same full-HUD composition as the Dev RefHud shot — HP **59.961** / MP 0.024,
  ammo **15 / 120**, boards populated with beats landed, killfeed/blips/damage numbers live.

## 2. The remaining gates (plan 8.2)

**Full automation suite at this HEAD: 186/186.** Editor target rebuilt first (`Target is up to
date` — the binaries ARE 333e2ec). Counted from `/w/Unreal/VcHost/Saved/Logs/VcHost.log` with
its tail intact (the `RequestExit` pair closes the file): 186 `Test Completed. Result={Success}`,
0 non-Success results, `**** TEST COMPLETE. EXIT CODE: 0 ****`.

**Monolithic game build at this HEAD:** `Build.sh VcHost Linux Development` → `Result:
Succeeded` (67 s; relinked the 333e2ec render-module change).

**SIGTERM teardown, all tracks live** (the packaged Development RefHud run above: UI thread
pumping the 1,732-node document, render thread replaying every frame, JS drivers running —
blips rAF + killfeed churn + damage timers, bundle mounted): SIGTERM at ~t+30 s of steady
state. The tail, verbatim from the packaged game's own log
(`Saved/StagedBuilds/Linux/VcHost/Saved/Logs/VcHost.log`):

```
[2026.08.02-01.40.59:973][934]LogVaCuus: UI thread stopping on an in-band shutdown command (0 view(s) closed, 0 queued command(s) dropped behind it)
[2026.08.02-01.40.59:974][934]LogVaCuus: VaCuus VFS teardown: 4 open(s) served by mounted bundles, 0 by loose roots
[2026.08.02-01.40.59:974][934]LogVaCuus: VaCuus VFS teardown: 1 script read(s) served by mounted bundles, 0 by loose roots
[2026.08.02-01.40.59:974][934]LogVaCuus:   bundle '/VaCuus/Bundles/DevUIBundle.DevUIBundle' served 5 open(s)
[2026.08.02-01.40.59:974][934]LogVaCuus: RmlUi shut down
[2026.08.02-01.40.59:974][934]LogVaCuus: Recorder destroyed with unpublished resource traffic (new: 0 geometry, 0 textures, 0 filters, 0 shaders; released: 968 geometry, 13 textures, 0 filters, 0 shaders) — dropped
[2026.08.02-01.40.59:974][934]LogVaCuus: UI thread exit: released 2 cached model definition set(s)
[2026.08.02-01.40.59:974][934]LogVaCuus: Bundle mount table: 1 record(s) destroyed at module shutdown
[2026.08.02-01.40.59:974][934]LogVaCuus: VaCuus runtime module shut down
[2026.08.02-01.40.59:978][934]LogChaosDD: Chaos Debug Draw Shutdown
[2026.08.02-01.40.59:978][934]LogNFORDenoise: NFORDenoise function shutting down
[2026.08.02-01.40.59:978][934]RenderDocPlugin: plugin has been unloaded.
[2026.08.02-01.40.59:979][934]LogPakFile: Destroying PakPlatformFile
[2026.08.02-01.41.00:244][934]LogExit: Exiting.
[2026.08.02-01.41.00:244][934]LogInit: Tearing down SDL.
[2026.08.02-01.41.00:244][934]LogCore: FUnixPlatformMisc::RequestExit(bForce=false, ReturnCode=143)
[2026.08.02-01.41.00:247][934]Log file closed, 08/02/26 04:41:00
```

No SIGSEGV, no `Fatal error`, no `Assertion failed`, no callstack (the only grep hits are the
engine's benign boot-time `LogStreaming … UniqueCallstack(-1)` Display lines). The Shipping
RefHud run's SIGTERM tail is line-for-line the same shape through `Log file closed`.

Post-UAT hazard check (the Task-6 finding, TargetPlatformManagerModule.cpp:1070): no
`Engine/Binaries/**/UnrealEditor.modules` manifest was rewritten by either BuildCookRun leg —
verified by mtime before any further editor use.

## 3. Spec §5 walked — six lines, each with named evidence

| # | The line (spec §5, condensed) | Verdict | Evidence, named |
|---|---|---|---|
| 1 | M5 demo + reference HUD boot from a **cooked bundle** in packaged Development AND Shipping (Linux), zero JS errors, clean teardown, **bundle-served M==0 asserted**, the Linux resident path asserted | **PASS** | §1 of this doc: four packaged runs at HEAD `333e2ec`, M==0 on BOTH teardown lines in all four (verbatim above), the resident-buffer mount line in all four, `LogVaCuusJS: Error` count 0 in all four, clean SIGTERM teardown tails; screenshots read by eye. First pass at Task 5 recorded in the passport's "Cooked-Shipping gate" section — this rerun supersedes it at the merge HEAD. |
| 2 | The passport filled: every §11 row Dev + cooked-Shipping-Linux (or named handoff), Method column complete; PF_FloatRGBA's PIE check green | **PASS** | `docs/passport/2026-08-vacuus-perf-passport.md` (commit cd0e846): rows 1–8 with both venue columns and a Method cell per row; the row-2 breach printed and routed to the owner, not re-baselined *(post-acceptance 2026-08-02: the owner chose route B — the row split, ≤0.50 at typical scale + ≤1.2 steady avg at reference scale; the passport's "The re-baseline" section records it)*; Win64/macOS cells marked owner-hw and enumerated in `docs/buyer/owner-handoff.md`. PF_FloatRGBA: `docs/passport/2026-08-vacuus-manual-matrix.md` row 12 **PASS** — `FloatRGBA -> LinearOutput` line + A/B by eye + the `VaCuus.Render.Composite.LinearOutputGPU` readback test (seen to fail with the decode removed); **venue substitution recorded** per spec §2(f) in the row-12 note: the ini-forced FloatRGBA-backbuffer `-game` session stands in for the editor-PIE leg (mechanism-identical — the permutation keys off the actual target format), and the interactive PIE leg rides the owner-hardware pass. |
| 3 | `BuildPlugin -StrictIncludes` green on 5.8 Linux, three legs; the scan clean **after its fixture run reports exactly the planted violations**; the zip inventory as designed; every disclosure entry points at an in-tree license file | **PASS** | `docs/research/m6-api-notes/buildplugin-fab-dryrun.md`: §6 run 1 FAILED under `-StrictIncludes` (the point — a real missing include), §7 runs 2–3 `BUILD SUCCESSFUL` ExitCode=0, three legs (editor-Dev 70.4 s / game-Dev 54.5 s / game-Shipping 46.3 s); the hardened scan's verbatim transcript: self-test 1 reports exactly the 6 plants (incl. ELF_MAGIC), self-test 2 forgives the whitelisted path and refuses the near-miss, then `SCAN: CLEAN` on the 976-file / 13 MB package; inventory `Tools/fab_inventory.sh` → `pass=34 fail=0`; disclosure list §4 — every entry with its in-tree license file, HarfBuzz explicitly not declared (arch spec :48 amendment). Commits 6419869 + 333e2ec (scan fail-closed rework). |
| 4 | The docs exist; every gotcha cites its source; the arch amendments landed | **PASS** | `docs/buyer/`: setup.md (mount predicate table + cook-inclusion rule), gotchas.md (19 entries, each symptom → cause with the proving `file:line` → what to do), perf-guide.md, rcss-matrix.md (keyed to VENDORED_SHA), owner-handoff.md — commit 0e1abd7. Arch-spec amendments dated 2026-08-02 inline in `docs/superpowers/specs/2026-07-29-vacuus-architecture-design.md`: §9 (:343), §11 (:394), §14 (:488), §15 (:512), plus the §2 disclosure note (:48). |
| 5 | The sweep per §3.5; the disposition table complete, every bead updated | **PASS** (bd execution = controller's, per plan 8.3) | `docs/research/m6-api-notes/p2-sweep.md`: all five P2/P3 premises re-verified against opened source before fixing; fixes landed (commits 0eb0033, a96ecb4). `docs/research/m6-api-notes/bead-dispositions.md`: 19 rows — 13 closed-in-M6 with commit evidence, 1 wontfix-documented (akj.6.17, stock-engine behavior + gotchas #17), 1 owner-hardware (akj.6.19), 4 carried with recorded reasons. The `bd` side executes from that table by the controller. |
| 6 | The Linux-Vulkan manual matrix checklist executed and recorded; the handoff enumerates every owner-hardware item with its exact command | **PASS** | `docs/passport/2026-08-vacuus-manual-matrix.md`: 15 rows executed 2026-08-02, 13 PASS with per-row "what was seen" prose, 2 PARTIAL with the venue reason recorded (row 5 IME — headless Linux has no `ITextInputMethodSystem`; row 13 live reload — editor-only watcher), nothing silently dropped. `docs/buyer/owner-handoff.md`: 8 sections, every owner item with its exact command (matrix columns, passport columns, Win64 disk literal, akj.6.19 IME, c11atomics, SHIM-1, the Fab upload, the two owed decisions). |

**All six lines PASS. M6 accepts.**
