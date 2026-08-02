# Owner-hardware handoff — everything this machine could not run

This page enumerates every item the M6 milestone owes to hardware that does not
exist on the dev machine (no Win64, no macOS, no 5.6/5.7 SDK), each with its exact
command and where the result lands. Nothing here was guessed; every cell these items
fill is currently marked "owner hw" in the evidence docs. The rule throughout:
run the SAME commands, read the SAME assertions, fill the cells — a row that cannot
run gets its reason recorded, never silently dropped.

## 1. The manual matrix — Win64 D3D12 and macOS Metal columns

The checklist is `docs/passport/2026-08-vacuus-manual-matrix.md`: 15 rows, executed
once by hand per platform; the Linux Vulkan column is filled and shows what each
PASS looks like. Per-platform notes:

- Run **windowed**, not `-RenderOffscreen` — half the rows exist to be seen on a
  real desktop (IME especially). The session recipe is at the top of the matrix doc.
- Row 5 (IME) is the akj.6.19 item below.
- Row 12 (PF_FloatRGBA) carries the **PIE-composite venue note**: on Linux the check
  ran as an ini-forced FloatRGBA-backbuffer `-game` session plus the real-RHI
  readback test — mechanism-identical, since the permutation keys off the actual
  target format, not the viewport kind. The interactive editor-PIE leg rides THIS
  pass: on owner hardware, run row 12 in an actual editor PIE session
  (`r.DefaultBackBufferPixelFormat=3` — the editor case the arch spec names at
  :194-195), read the `FloatRGBA -> LinearOutput` line and the A/B colors by eye.
- Row 13 (live reload) needs the interactive editor: edit a loaded `.rcss` mid-PIE,
  watch the repaint, then mount a bundle, edit again, and assert the shadowing
  Warning names the path.
- Rows 1–11, 14, 15 run exactly as written in the matrix's command column.

## 2. The perf passport — Win64/macOS §11 columns

`docs/passport/2026-08-vacuus-perf-passport.md` has Dev + cooked-Shipping-Linux
numbers on every row; Win64 D3D12 and macOS Metal columns are open. Same soaks:
`vacuus.RefHud` + `vacuus.M1HUD.PerfLog 1` (Dev), staged Shipping with
`-VaCuusRefHud -VaCuusPerfLog`. Read windows from the log, not stdout.

## 3. The Win64 disk literal (the budget row's own platform)

Passport row 6 currently holds the Linux proxy (+3.22 MiB, marked as proxy —
the research demanded the marking). The literal:

```
BuildCookRun -platform=Win64 -clientconfig=Shipping -build -cook -stage -pak
```

twice — plugin enabled vs disabled in the `.uproject` (plus the three plugin-scoped
config lines commented out for the disabled leg, per the passport's Disk method) —
then the staged-bytes delta excluding debug files, itemized from the staged tree +
`UnrealPak -List`. Fills the row's "Win64 literal: owner hardware" cell against the
10 MB budget.

## 4. Win64 IME re-check — bead akj.6.19

M2 verified IME composition interactively on Linux (ibus); the M2 accept line
deferred the Win64 leg to M6, and M6's machine cannot run it. Matrix row 5,
windowed: focus the text input (`vacuus.M2Demo`), compose via the Windows IME
(Japanese/Chinese), assert the composition string renders in-field and the candidate
window tracks the caret rect. Record the result on the bead.

## 5. The quickjs `/experimental:c11atomics` decision

`Source/VaCuusJs/VaCuusJs.Build.cs:44-48`: upstream quickjs-ng demands
`/experimental:c11atomics` under MSVC (their CMakeLists.txt:128); clang-cl and MSVC
differ here, and no Win64 build of the module has ever run. On the first Win64
compile leg (SHIM-1 or the matrix build): if MSVC refuses the vendored C, add the
flag for MSVC only and record it in the Build.cs comment; if clang-cl compiles
clean, record that instead. This is akj.6.19's sibling handoff item, not a
compat-seam entry.

## 6. SHIM-1 — the 5.6/5.7 compat builds

`docs/passport/2026-08-vacuus-shim1.md` is the complete procedure: clean clone →
`RunUAT BuildPlugin -StrictIncludes` per engine → every break fixed inside
`VaCuusEngineCompat.h` (the four hotspots with expected failure shapes per engine) →
5.8 re-run to prove no regression → scan + blank-project smoke. On this machine only
the 5.8-Linux legs ran (three legs green, `buildplugin-fab-dryrun.md` §7). Note the
platform asymmetry: a Windows-host BuildPlugin silently drops Linux legs and vice
versa — a pass on one says nothing about the other.

## 7. The Fab upload

The package already IS the upload shape (976 files, 13 MB — dry-run §7, run 3, after
the `-/Binaries/...` `-/Intermediate/...` filter fix). Steps:

1. Clean clone (no `.git`, no `.beads`, no `node_modules` — verify the esbuild ELF
   is absent; `.uasset` files real packages, not LFS pointers).
2. `RunUAT BuildPlugin -Plugin=<clone>/VaCuus.uplugin -Package=<out> -StrictIncludes`
   with the platforms Fab requires.
3. `bash Tools/fab_scan.sh <out>` — it must FAIL its planted fixture first, then
   report CLEAN on the package.
4. Upload with the third-party disclosure list from
   `docs/research/m6-api-notes/buildplugin-fab-dryrun.md` §4 (every entry points at
   an in-tree license file the package includes; HarfBuzz is NOT USED — do not
   declare it).
5. Fab's unrecorded rules (anything past the verified 4.3.6.1.a/e) are a
   confirm-at-upload item; new findings land in the disclosure draft.
6. **Dev-machine hazard if re-running BuildPlugin against a source engine:** the
   HostProject editor leg rewrites `Engine/Binaries/**/UnrealEditor.modules`
   manifests and can resurrect stale platform modules — next editor launch dies on
   `!bIsRunningPlatform` (TargetPlatformManagerModule.cpp:1070). Fix: delete the
   stale `.so`s + that manifest (CLAUDE.md dev-loop hazards; dry-run §8).

## 8. Decisions owed (not hardware, but owner calls)

- **The UI-row breach at reference scale** (passport, "The breach"): 1.05 ms
  steady vs the 0.50 budget at 1,732 always-animated nodes. Two routes, both
  document-side: shrink the reference workload (REF-COUNT yields to the gates, by
  spec), or re-baseline the row with the reasoning recorded. The passport prints
  the number and deliberately does not choose.
- **GPU in or out of the 32 MB RAM row** (passport row 5): **DECIDED 2026-08-02 —
  out, its own line**, per the recorded recommendation (7.91 MiB @1080p is a
  view-size choice, not plugin behavior); the row now states the exclusion.
