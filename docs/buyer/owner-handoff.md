# Owner-hardware handoff — everything this machine could not run

This page enumerates every item the M6 milestone owes to hardware that does not
exist on the dev machine (no Win64, no macOS, no 5.6/5.7 SDK), each with its exact
command and where the result lands. Nothing here was guessed; every cell these items
fill is currently marked "owner hw" in the evidence docs. The rule throughout:
run the SAME commands, read the SAME assertions, fill the cells — a row that cannot
run gets its reason recorded, never silently dropped.

## 1. The manual matrix — Win64 D3D12 and macOS Metal columns

**Win64 half tracked as bead `akj.10.1`, and it is blocked on `5fg`, not on rendering:** the PSO
fatal that stopped the 2026-08-03 pass is fixed (`xa5`, `b4f12e1`), so what remains is a session on
the physical console — an OpenSSH session on Windows has no interactive desktop and a windowed UE
process dies before engine init.

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

**Win64 half tracked as bead `akj.10.2`** (blocked on `5fg` for the Dev soak; the staged Shipping
leg may not be — establishing that is part of the bead).

`docs/passport/2026-08-vacuus-perf-passport.md` has Dev + cooked-Shipping-Linux
numbers on every row; Win64 D3D12 and macOS Metal columns are open. Same soaks:
`vacuus.RefHud` + `vacuus.M1HUD.PerfLog 1` (Dev), staged Shipping with
`-VaCuusRefHud -VaCuusPerfLog`. Read windows from the log, not stdout.

## 3. The Win64 disk literal (the budget row's own platform)

**Tracked as bead `akj.10.3`, which also carries the memory-mapped bundle line** — same cook, and
Win64 is the only platform where that branch executes. Needs no interactive desktop.

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

**Tracked as bead `akj.10.4`. Half-answered by the 2026-08-03 Win64 pass, and the answer moved the
question:** MSVC compiles the module clean *without* the flag, because the vendored source takes the
`__STDC_NO_ATOMICS__` branch and the atomics code is preprocessed away — so the open item is no
longer a build question but a behavioural one, the JavaScript `Atomics` global being absent on Win64
and present on Linux and macOS (`VaCuusJs.Build.cs:56-75` now carries the full argument). Owner
decision 2026-08-03: **do not add the flag yet** — sessions survive on Win64 now, so confirm with a
live `typeof Atomics` first and decide against an observation rather than a deduction.

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
3b. **If a channel ever gets PRECOMPILED binaries** (Fab's is source-only, so this is
   the "some day" step): re-run BuildPlugin with `FilterPlugin.ini`'s `-/Binaries/...`
   rule commented out, then `bash Tools/api_export_check.sh <out>`. It `nm -D`s the
   delivered `.so`s and fails if a class the docs tell buyers to use has zero exported
   members. This is not paranoia — it is exactly how VaCuus-dgl shipped: `UVaCuusWidget`
   and `UVaCuusWorldComponent` registered fine (Blueprint worked) and exported **0**
   members each, so no buyer's C++ could link either one, and nothing in this repo
   could see it because every in-tree consumer compiles the plugin from source. The
   check is Linux-only; a Win64 drop needs the same thing against `dumpbin /exports`.
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

## 8. Owner decisions (both made 2026-08-02; kept here as the record)

- **The UI-row breach at reference scale**: **DECIDED 2026-08-02 — route B, the
  row split** (passport, "The re-baseline"): ≤0.50 ms stands at typical HUD scale
  (M2 figures, 10–20× inside); the 1,732-node reference worst case budgets
  ≤1.2 ms steady avg (measured 1.05 — ~14% headroom). Reasoning recorded per
  spec §2(g); the buyer's document-side levers stay in the perf-guide.
- **GPU in or out of the 32 MB RAM row** (passport row 5): **DECIDED 2026-08-02 —
  out, its own line**, per the recorded recommendation (7.91 MiB @1080p is a
  view-size choice, not plugin behavior); the row now states the exclusion.

## 9. What the Win64 machine itself needs before any of §1–§3 can run

Added 2026-08-03 after the first Win64 pass, because two of its findings are properties
of the machine rather than of the work, and both will otherwise be rediscovered from
scratch by whoever sits down next.

- **No interactive desktop over SSH** — bead `5fg`. A GUI process launched from a
  non-interactive OpenSSH session on Windows has no desktop: `-game -windowed` died at
  startup (exit 3) before engine init, with zero VaCuus lines. `-RenderOffscreen` boots
  and is how the pass produced findings at all, but it cannot produce the rows that exist
  to be seen. Options, none tried: the physical console, a scheduled task set to run in
  the console session, PsExec `-i`, or RDP (which creates a real interactive session).
  **This blocks §1 and most of §2; it does not block §3 or the builds in bead
  `akj.10.8`.**
- **No .NET Framework SDK** — bead `akj.10.9`. `SwarmInterface.Build.cs:29-34` throws when
  `NetFxSdkDir` is null on Win64 and `UnrealEd` depends on it, so **no editor target can
  produce a makefile** — a stock template project with no VaCuus fails identically in
  3.4 s, which is what settles authorship. The pass worked around it with a sandboxed
  AutoSDK stub under `C:\VaCuusWin64Test\autosdk` and `UE_SDKS_ROOT` set per build process
  only; that stub dies with the scratch tree and has to be re-created otherwise. Installing
  VS component `Microsoft.Net.Component.4.6.2.SDK` retires the workaround.

Full record of that pass, including the five build fixes it landed and the 13 matrix rows
it could not reach: `docs/passport/2026-08-vacuus-win64-results.md` (see its §11
disposition block for where each open item now lives).
