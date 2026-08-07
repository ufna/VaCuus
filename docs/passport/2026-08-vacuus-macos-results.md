# macOS/Metal pass — what was executed, what it found, what it could not reach

> **Superseded numbers, not superseded findings (2026-08-07).** This document records the
> 2026-08-03 pass on commit `f5eba06` and stays as written — it is the record of that run. The
> build/suite figures in it have since moved; the current ones are in
> `2026-08-07-cross-platform-reverify.md`, which re-ran the three legs on `0e1f470`. Two things
> that pass reported as blocked are also no longer true: the machine now carries a real
> **git clone** (`~/VaCuus` plus a build clone under `VcHost/Plugins/`) instead of a tar copy,
> and the lobby demo's "`chrome.rml` is not served by any DevUI root" refusal was resolved by
> moving the demo into the VaCuusDemo project, where its content already lived.

**What this is.** The first execution of VaCuus on Apple silicon and Metal, ever. It ran on
2026-08-03 against the plan in `2026-08-vacuus-macos-plan.md`, over SSH from the Linux dev box
onto the owner's MacBook. It fills the **macOS Metal** column of `2026-08-vacuus-manual-matrix.md`
where it legitimately can and records, by name and with a reason, everything it could not run.

**The commit under test:** `f5eba0634af2e60146790de155ad149b2182c26b` (branch `master`,
"perf: the upload leaves the blocking path"). Two fixes were made *during* the pass and are
committed on top of it; both are named below with their SHAs, and the rows after each fix ran
against the fixed tree. `master` also gained two commits from another session while this ran
(`17c134e`, `6052dbd`); both touch only `Tools/mac_bootstrap.sh`, a script this pass never
executed, so `git diff --stat f5eba06..HEAD -- Source/ Content/ Config/ Shaders/` is exactly the
two fixes and nothing else.

**The machine and the engine.**

| | |
|---|---|
| Host | va-macbook — Apple M1 Pro (16 GPU cores), 10 CPU cores, 16 GB, macOS **26.5.2** (25F84), arm64, `sysctl.proc_translated` = 0 |
| Display | Built-in Liquid Retina XDR, 3456 × 2234 device px (1728 × 1117 points), main and only display |
| Toolchain | **Xcode 26.0** (17A324), full `Xcode.app` selected |
| Engine | **UE 5.8.1 Launcher/Installed build** at `/Users/Shared/Epic Games/UE_5.8` — `Engine/Build/Build.version` Major 5 / Minor 8 / Patch 1, changelist 56057345, `InstalledBuild.txt` present, log confirms `LogInit: Installed Engine Build: 1` |
| Host project | `~/VaCuusMacTest/VcHost` — a raw copy of `Engine/Templates/TP_ThirdPerson` (C++), stock module and target names kept (`TP_ThirdPerson`, target `TP_ThirdPersonEditor`); only the `.uproject` renamed to `VcHost.uproject` so log paths read as the Linux recipes do. Plugin installed as **real files** at `Plugins/VaCuus`, never a symlink. |

**Feature level that bound: `SM5` / `SP_METAL_SM5`.** Not SM6, and this is not a configuration
mistake — it is the hardware. `MetalRHI.cpp:255-268` computes SM6 support as
`@available(macOS 15.0) && MTLDevice->supportsFamily(MTL::GPUFamilyApple8)`; this machine satisfies
the OS clause with room to spare and fails the GPU one, because M1 Pro is `GPUFamilyApple7`.
The engine's own message is misleading and should not be quoted without this note: at
`MetalRHI.cpp:421-431` it picks its wording from `GRHIAdapterName.Contains("M1")`, so what the log
actually prints is

```
LogMetal: Warning: To use SM6 on this system, please ensure you are running Mac OS 15. Falling back to SM5
```

on a machine running macOS 26.5.2. **Every visual row below is an SM5 row.**

What *is* proven about Metal shader compilation: the plugin's four `.usf` global-shader files
compiled and ran at **`SF_METAL_SM5`**, and the proof is pixels rather than a log line — the glass
blur, all six decorator gradients and all five material decorators drew correctly (rows 8, 9, 10),
and **`LogShaderCompilers: Error` appears zero times in every session of this pass**. Plan risk 5
(`VaCuusMaterial.usf` meeting the Metal front-end) did not materialise at SM5.

What is **not** proven: anything at SM6. The engine created an `Intermediate/ShaderAutogen/METAL_SM6/`
tree beside the SM5 one, and the `-nullrhi` session's CsvProfiler metadata reads
`shaderplatform="METAL_SM6"` — but that is NullRHI reporting the target platform's maximum, not a
compile of our permutations, and no VaCuus `.usf` is named against SM6 anywhere in the logs. Both
"our HLSL survives Apple's Metal front-end at SM6" and "VaCuus renders correctly under Metal SM6"
remain open and need an M2-or-later Mac, or a cook targeting SM6.

---

## 1. How the tree got there, and how that was verified

`git-lfs` is not installed on the Mac and there is no Homebrew, so the plan's clone-and-`lfs pull`
step was replaced by an **rsync from the Linux tree**, where every LFS object is materialised.
That sidesteps the plan's largest silent-failure class (§2.7: an unsmudged pointer fails as a
*Warning*, the HUD renders textless, and seven rows are quietly void).

Excluded from the transfer: `.git`, `.beads`, `Binaries`, `Intermediate`, `node_modules`, `Saved`.

Verification, run on the Mac after transfer:

- `git lfs ls-files` on the Linux side lists **36** paths, 35 marked `*` and exactly one `-`;
  the single `-` is `Tools/scan-fixture/planted_lfs_pointer.txt`, the deliberate fixture.
- The **aggregate MD5 of all 36 files matches byte-for-byte across the two machines**
  (`aa898de5231f18918bae26aa17d864e7` both sides).
- `file Content/DevUI/fonts/LatoLatin-Regular.ttf` → `TrueType Font data, 17 tables …`, not ASCII.
- `grep -rl git-lfs.github.com` over the transferred tree hits only the scan fixture and three
  files that mention the string in prose (`Tools/fab_scan.sh`, `Tools/mac_bootstrap.sh`,
  `docs/research/m6-api-notes/buildplugin-fab.md`). Nothing under `Content/`.
- Confirmed again at runtime, which is the check that actually matters:
  `LogVaCuus: [Rml] Loaded font face 'LatoLatin' [regular] from 'fonts/LatoLatin-Regular.ttf'.`
  and zero "No font face defined" warnings in any session.

---

## 2. Build findings

Two findings, one of each kind: a link failure that stopped the build, and a shipped behavioural
defect the build was clean about. **Neither was a missing include** — the class of defect the plan
predicted (§4 risk 12, and the `-StrictIncludes` wave Linux saw) did not appear at all.

### Finding B1 — `VaCuusRml` fails to link on an Installed engine: libpng is missing

**Fixed in `03a3fdf`** ("fix: VaCuusRml links libpng itself, because an Installed engine drops
FreeType2's request").

The first build died at `Link [Apple] libUnrealEditor-VaCuusRml.dylib` with ~23 undefined symbols:

```
Undefined symbols for architecture arm64:
  "_png_create_info_struct", referenced from:
      _Load_SBit_Png in libfreetype.a[arm64][35](sfnt.c.o)
  … 22 more _png_* …
ld: symbol(s) not found for architecture arm64
```

**Mechanism, read rather than guessed.** `FreeType2.Build.cs:66-68` already asks for `zlib` and
`UElibPNG` on its own behalf ("FreeType needs these to deal with bitmap fonts") — but through
`AddEngineThirdPartyPrivateStaticDependencies`, and `ModuleRules.cs:1601-1607` gates that helper's
entire body on `if (!bUsePrecompiled || target.LinkType == TargetLinkType.Monolithic)`. A
Launcher/Installed engine marks every engine module `bUsePrecompiled`, so in a modular editor
target FreeType2's own two lines evaporate and `libfreetype.a` arrives with its libpng references
unresolved. The engine's own consumers never notice: they are shipped prebuilt and never re-link.

**Why Linux never saw it — two independent reasons, both worth knowing.** The dev box runs a
*source* engine, where `bUsePrecompiled` is false, the helper runs, and we inherited the
dependency transitively. And `ld.lld` tolerates an unresolved symbol in a shared object where
macOS's `ld` refuses outright. Evidence for both halves: the shipped `libfreetype.a` references
`png_*` on **both** platforms (`nm -u` finds them in the Linux archive too), and the existing
Linux `libUnrealEditor-VaCuusRml.so` **defines** 387 png symbols — i.e. UElibPNG was statically
linked into it. Note the shipped `ftoption.h:276` leaves `FT_CONFIG_OPTION_USE_PNG` commented out;
the header does not describe how Epic built the archives, so the archive is the authority.

**Reach beyond macOS.** This is an *Installed-engine* defect that macOS merely exposed first. Any
customer on a Launcher engine is in the same configuration. Fix is three lines in
`VaCuusRml.Build.cs`: name `zlib` and `UElibPNG` beside `FreeType2` from **our** module, where
`bUsePrecompiled` is false and the calls always run. After the fix,
`libUnrealEditor-VaCuusRml.dylib` defines 427 `_png_*` symbols and has zero undefined ones.

### Finding B2 — Backspace was forward-delete on macOS

**Fixed in `49bcad0`** ("fix: Backspace was forward-delete on macOS, because Platform_Delete is
not a key"). This one is a **shipped, user-visible defect**, not a build break; it was found by the
automation suite's first Mac run and is reported here because the pass is what surfaced it.

`VaCuus.Input.KeyMap` failed with
`Expected 'BackSpace maps to its RmlUi key identifier' to be 69, but it was 99` — `KI_BACK = 69`,
`KI_DELETE = 99` (`RmlUi/Core/Input.h:92,128`).

`EKeys::Platform_Delete` is not a physical key. It is "whichever key this platform's text
shortcuts mean by delete": `InputCoreTypes.cpp:166` initialises it from
`FPlatformInput::GetPlatformDeleteKey()`, which is `EKeys::Delete` generically
(`GenericPlatformInput.h:37-40`) and **`EKeys::BackSpace` on macOS**
(`MacPlatformInput.cpp:86-97` — "The legacy behavior is that Platform Delete is Backspace",
default `true`, overridable via `[MacInput] bPlatformDeleteIsBackspace`).

So `BuildKeyMap`'s `Platform_Delete → KI_DELETE` line was, on macOS, `BackSpace → KI_DELETE`, and
`TMap::Add` **replaces**: it silently overwrote the `KI_BACK` entry set two dozen lines earlier.
Every Backspace in every RmlUi text field on macOS arrived at the document as forward-delete. Off
macOS the same line duplicated the `EKeys::Delete` entry directly above it, so it never added
anything correct on any platform and was removed rather than guarded. Mac's real forward-delete
key (fn-Delete) is unaffected — Slate reports it as `EKeys::Delete`, still mapped.

**Seen to fail, then seen to pass**, which is this repository's proof standard: `VaCuus.Input.KeyMap`
failed on the Mac run before the fix and passed on the Mac run after it, with nothing else changed.

The fix also adds the observable this class of bug had none of. A duplicate `FKey` leaves no trace
anywhere — no warning, no return value — except a finished map one entry short, so `BuildKeyMap`
now ensures its own final count (111). Neither a compiler nor a test on one platform can see an
`FKey` that is an alias of a *different* `FKey` somewhere else; the count can, on every platform,
at first use, before any test runs. It was calibrated on this machine: the ensure did not fire on
the post-fix run.

### What compiled clean, which is itself a data point

- **Vendored quickjs-ng compiled unmodified by Apple clang**: `relay_quickjs.c`, `relay_libregexp.c`,
  `relay_libunicode.c`, `relay_dtoa.c` all built with **zero warnings and zero errors**. The plan's
  risk 12 (`pthread_cond_timedwait_relative_np` / `malloc_size` under bare `-std=c11`, needing a
  Mac-only `_DARWIN_C_SOURCE`) **did not materialise**; no `VaCuusJs.Build.cs` change was needed.
- The whole 367-action build produced **zero compiler warnings**.
- All five modules link: `libUnrealEditor-{VaCuusRml,VaCuus,VaCuusJs,VaCuusRender,VaCuusEditor}.dylib`.
- **The Mach-O export gate passes** (plan §6): `nm -gU libUnrealEditor-VaCuusJs.dylib | grep ' _JS_'`
  is empty, while `nm` finds 247 `_JS_*` symbols in the binary as non-exported. 88 exported symbols
  total. Vendored patch #1 (hidden visibility on the quickjs API) is now confirmed on Mach-O as
  well as ELF — a claim `VENDORED_TAG.txt` previously only half-supported.
  *Correction to the plan:* §6 writes the path as `Binaries/Mac/UnrealEditor-VaCuusJs.dylib`.
  On Mac the file is `libUnrealEditor-VaCuusJs.dylib`; without the `lib` prefix `nm` finds no file
  and the grep is trivially, falsely empty.

Build wall-clock, for the record: **365 s** for the full cold build (the run that ended in B1),
**52 s** to relink after B1's fix, **11 s** for B2's one-TU rebuild. `Result: Succeeded`.

---

## 3. Venue translation — what actually had to change, including three the plan did not have

The plan's eight venue facts (§3 a–h) held, with these corrections and additions:

| | |
|---|---|
| **Log location** (new) | The plan and CLAUDE.md both say `Saved/Logs/VcHost.log`. **On macOS that file does not exist.** Editor-target sessions log to `~/Library/Logs/Unreal Engine/VcHostEditor/VcHost.log`; `-game` sessions log to `~/Library/Logs/VcHost/VcHost.log`. Every number in this document was read from those, never from stdout. |
| **Screenshots** | Land where the plan says: `<Project>/Saved/Screenshots/MacEditor/ScreenShotNNNNN.png`. |
| **`-ExecCmds` trailing comma** | Confirmed necessary and used throughout. |
| **No offscreen mode** | Confirmed. Every visual row here ran **windowed against a real Metal RHI**. |
| **GUI over SSH** | Works. The console user is the same account as the SSH login, so the editor binary launched from a non-interactive SSH session reaches the WindowServer and opens a real window. `open -a` was not needed. `screencapture` does **not** work (no screen-recording grant), so no desktop-level capture was possible. |
| **Kill by PID** | Confirmed, using `pgrep -x UnrealEditor` (BSD `pgrep` has no `-a`). |
| **The machine was not exclusively ours** (new) | The owner was logged in at the console and interacted with at least one session — a window was dragged and resized mid-run and extra `vacuus.*` demo commands were typed into a running game's console. Where this perturbed a measurement it is called out on the row. It is also why every scripted row below fires its evidence within the first few seconds. |

---

## 4. The automation suite

Command (venue-translated): the editor binary run directly, `-ExecCmds="Automation RunTests VaCuus, Quit,"
-unattended -nullrhi -nosplash`. Counts read from `~/Library/Logs/Unreal Engine/VcHostEditor/VcHost.log`.

| Run | Tree | Result |
|---|---|---|
| 1 | `f5eba06` + B1 fix | `Automation Test Queue Empty 197 tests performed.` — **193 Success, 4 Fail** |
| 2 | + B2 fix (`49bcad0`) | `Automation Test Queue Empty 197 tests performed.` — **194 Success, 3 Fail** |

**197 tests selected and performed — exactly the Linux baseline of 197.** No test failed to
register, and no Mac-only test appeared or vanished. (The plan predicted 189 at an older HEAD;
197 is what both platforms select now.)

The four self-skips under `-nullrhi` behaved as designed and are named in the log:
`VaCuus.Render.Composite.LinearOutputGPU`, `VaCuus.Render.Upload.AsyncPayload`,
`VaCuus.Render.Upload.Cost`, `VaCuus.World.MipContentGPU`.

### The IME result — the highest-value automated outcome of the trip

`LogVaCuus: IME: platform ITextInputMethodSystem present; composition is available`

That is the **Log** branch of `VaCuusTextInput.cpp:966-975`, and it is the first time it has ever
been taken. On Linux both sides of `VaCuus.Input.TextEntry`'s relative assertion
(`TestEqual("A context is only REGISTERED where a platform system exists", ImeStatus.bRegistered,
!bPlatformImeAbsent)`, `VaCuusTextEntryTest.cpp:327-328` — the plan's `:433-436` has drifted and now
lands in the `GetSelectionRange` block) collapse into the
"absent" branch and registration is never exercised. On this machine `FSlateApplication` returns a
live `FMacTextInputMethodSystem`, the test took the other branch, **and the assertion held** —
registration demonstrably happens. The same line appears in every `-game` session too.

### The three remaining failures, judged

**`VaCuus.Input.SlateRouting` and `VaCuus.Input.TextEntry` — venue, not defect, but the Mac suite
is red because of it.**

Neither test's own assertions failed. Both were failed by the automation framework promoting an
*engine* log line to a test error:

```
LogMacTextInputMethodSystem: Error: Activating a context failed when its window couldn't be found.
```

`FMacTextInputMethodSystem::ActivateContext` (`MacTextInputMethodSystem.cpp:164-190`) requires
`Context->GetWindow()` to yield an `FCocoaWindow` whose `openGLView` is an `FCocoaTextView`; an
automation test builds a widget that lives in no real window, so it logs `Error` and returns. Our
side is behaving correctly — `FVaCuusImeHandler::ActivateContext` activates because focus arrived,
and `FVaCuusTextInputMethodContext::GetWindow()` returns `Surface.NativeWindow`, which is populated
from a real `SWindow` in a real session and is legitimately null in the harness. On Linux the
platform system is absent, so `ActivateContext` is never called and the message cannot occur.

Note the evidence that this is registration *working*, not failing: the same run's TextEntry
`AddInfo` reads `Platform ITextInputMethodSystem: present.`, and the caret rect it published came
out correct — `GetTextBounds caret rect: pos (215.0, 150.0) size (1.0, 17.0) in Slate absolute px`
against a field box of `Min=(200,150) Max=(350,180)`.

**Recommended fix, deliberately NOT applied here — this needs an owner call.** Two lines,
`AddExpectedMessage(TEXT("Activating a context failed when its window"), ELogVerbosity::Error,
EAutomationExpectedMessageFlags::Contains, /*Occurrences=*/-1)` in each test.
`AutomationTest.h:1777-1779` documents `Occurrences < 0` as "occurrences of this message will be
silently ignored", which is exactly right and needs no `#if PLATFORM_MAC` — on Linux the message
never occurs and a negative count does not demand it. It was not applied because its correctness
rests on a semantic this pass could only verify in 5.8.1, and because it perturbs the Linux
197/197 baseline, which cannot be re-run from the Mac. Verify on Linux, then land.

**`VaCuus.Js.Cost.CombinedChurn` — venue (machine), with the numbers to show it.**

The gate is `CombinedP99 < 5.0` ms, described in `VaCuusJsCostTest.cpp:729-731` as "the gate at
10x (machine-jitter-proof; the NUMBER is the deliverable)". Two runs, same binary, same machine,
20 minutes apart:

| Run | p50 | **p99** | max | pump mean | update mean | gc mean |
|---|---|---|---|---|---|---|
| 1 | 1.006 | **7.764** | 18.144 | 0.092 | 1.496 | 0.004 |
| 2 | 1.419 | **5.100** | 10.562 | 0.053 | 1.591 | 0.005 |

p99 moved by 52% between two runs of identical code while p50 moved by 41% and the means barely
moved — that is scheduling jitter on a 10-core laptop running a 197-test suite (and, in run 1,
sharing the machine with its owner), not a slower code path. Run 2 misses the gate by 2%. The
honest reading: **the 10× margin is not jitter-proof against this class of machine**, and the
deliverable — the number — is recorded here. No code change is proposed; the owner may want to
decide whether the gate should be venue-aware.

---

## 5. The matrix, macOS Metal column

Session shape for every row below:

```
"/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor" \
  /Users/ufna/VaCuusMacTest/VcHost/VcHost.uproject -game -windowed -ForceRes -resx=1920 -resy=1080 \
  -nosplash -ExecCmds="<row command>,"
```

**Venue: windowed, real Metal RHI, `SP_METAL_SM5`, 1920×1080, UE 5.8.1 Installed, macOS 26.5.2,
M1 Pro.** Every screenshot below was read by eye by the agent that ran the pass; what follows is
what was seen, not what was expected.

| # | Row | macOS Metal (SM5) | Evidence |
|---|-----|-------------------|----------|
| 1 | Screen-space HUD composite | **PASS** | screenshot read below |
| 2 | Steady-state node count | **PASS — 1732** | `NodeCount: view 1 document 'RefHud/refhud.rml': 1732 nodes` — identical to Linux, inside [1650,1850] |
| 3 | Mouse hit-test + hover | **PASS** | 6 rects at the Linux coordinates; `Hit (120,115): covered=yes focusable=yes`; ALPHA in `:hover` after the origin correction |
| 4 | Keyboard text entry | **PASS**, after the window-origin correction | "vacuus" in the field with a caret; `IME bridge built=yes, platform system absent=no, registered=yes, context active=yes` |
| 5 | IME composition | **NOT RUN — needs a human at the keyboard.** But the half that can be automated moved: registration is now proven (§4). | `IME: platform ITextInputMethodSystem present; composition is available` |
| 6 | Gamepad spatial nav | **PASS** | focus ring on BRAVO |
| 7 | World-space panel + raycast | **PASS — 16/16 assertions** | `vacuus.M5World.InputSmoke: all 16 assertion(s) passed`; quad shows Ammo 29 |
| 8 | Glass (backdrop blur) | **PASS, via the bounded-copy route** | `Exp-GLASS-BACKBUFFER-SRV: … through a bounded copy pass (texture ShaderResource=no …)`; `published=0 skipped=227 (100.0% idle)` |
| 9 | Gradient + builtin decorators | **PASS — all six cells** | screenshot read below |
| 10 | Material decorators | **PASS** on a re-run with a later beat | all five materials draw; see below |
| 11 | M5 acceptance demo | **PASS — zero `LogVaCuusJS: Error`** | `translation: published table v1 (2 entries)`, `model 'hud' bound` on both views, two beats read below |
| 12 | PF_FloatRGBA composite permutation | **PASS — both permutation lines, A/B read by eye** | control `A2B10G10R10 -> pass-through`; forced `FloatRGBA -> LinearOutput (sRGB->linear decode, gamma 1.0 target)` |
| 13 | Live reload (editor watcher) | **NOT RUN** — needs an interactive editor PIE session and a mid-run file edit; the watcher lives in the editor-only `VaCuusEditor` module. Same reason as the Linux column. |
| 14 | Demo-suite toggles + clean teardown | **PASS — but not via SIGTERM.** See the venue note below. | full teardown tail, zero unpublished NEW resources |
| 15 | Shipping ignition flags | **NOT RUN** — packaging was not attempted; see §7. |

### Row 1 — screen-space HUD. PASS.

`vacuus.RefHud, vacuus.RefHud.Count 12, vacuus.M1HUD.AutoShot 600`. Read by eye at 600 recorded
frames: both 24-row boards populated (ALFA-RAPTOR-01 … ALFA-NOMAD-24, BRVO-VIPER-01 …
BRVO-HAVOC-24) with distinct K/D/A/SCORE/PING columns and green ping meters; killfeed down the
right edge, 15 rows, orange killer names with weapon labels (DMR, knife, railgun, SMG) and red HS
pills; minimap bottom-right with the red/blue blip cloud, the green sweep arc and the gold north
marker; player plate "UFNA-01" with level badge 17, HP 73.769 on a red bar and MP 33.606 on a blue
one (mid-sweep — the pipeline is live, not frozen); two rows of buff swatches with their countdown
labels; compass strip with N centred; "HOLD THE LINE // WAVE 04"; three rows of damage numbers;
crosshair; four-slot ability bar; the SYSTEM // SETTINGS panel with its sliders and RESUME [ESC];
ammo in the warm glow. **The same cosmetic finding the Linux column recorded reproduces exactly:
the objective line overlaps the scoreboard headers** — both are top-centre. Recorded, not a gate.

Nothing on this HUD is missing, mis-coloured, mis-placed or untextured relative to the Linux shot.
This screenshot was taken at 1728×1084 rather than 1920×1080 because the owner resized the window
mid-run; every other row's screenshot is 1920×1080.

### Row 3 — mouse hit-test. PASS, and it settles the plan's coordinate question.

```
Rects: view 1 'm2_demo.rml' generation=123 viewSize=1920x1080 rects=6 cursor=1 wantsKeyboard=no …
  [0] (  51,  95)-( 173, 142) Interactive|Focusable
  [1] ( 181,  95)-( 303, 142) Interactive|Focusable
  [2] ( 311,  95)-( 433, 142) Interactive|Focusable
  [3] (  51, 285)-( 471, 464) Interactive
  [4] ( 456, 286)-( 470, 463) Interactive
  [5] ( 511,  95)-( 909, 132) Interactive|Focusable|TextInput
Hit (120,115): covered=yes focusable=yes textInput=no -- so a press there would be answered Handled
```

Six rects: three buttons, the wheel list, its scrollbar, the TextInput field. The
`vacuus-passthrough` panel is correctly ABSENT. **The TextInput field is at (511,95)-(909,132) —
the Linux column's literal, to the pixel.** The plan (§4, "Conflict resolved") named this as the
arbiter for whether the matrix's coordinate literals survive on Retina. They do.

The hover half needed the coordinate correction below. With it: the screenshot shows the pointer
parked inside ALPHA, ALPHA rendering in the amber `:hover` fill with dark text, and BRAVO and
CHARLIE staying dark — the Linux description, unchanged.

### Rows 3 and 4 — the window-origin correction, measured rather than assumed

The plan (block 6) predicted this and prescribed "move the window to the top-left of the main
display … for `HoverShot`/`TypeShot`, add the window origin". Both were done. All four runs below
passed `-WinX=0 -WinY=0`:

| Click at desktop | Attribution |
|---|---|
| (710, 113) — the raw view coordinate | `taken by THE GAME (VaCuus declined, so it bubbled past us to SViewport)` |
| (710, 141) — +28 | `taken by THE GAME` |
| (1478, 690) — the centred-window guess | `taken by THE GAME` |
| **(710, 169) — +56** | **`taken by THE UI (VaCuus captured the mouse)`** |

**The correction on this machine is `+0` in X and `+56` px in Y** — nothing horizontal, and
vertically the height of one 28-point title bar at a 2× backing scale. Two negative controls make
it a measurement rather than a lucky hit: `+28` is too little, and the centred-window guess (which
is where the window would sit had `-WinX/-WinY` been ignored) also misses, so the flags did move
it. Note the row-3 `Hit` command and the `Rects` listing needed
none of this — they are pure view-pixel space and answered correctly at the raw coordinates, which
is what makes them the right thing to run first.

Also worth recording about the self-check: `MoveMouseTo`'s `handled` token reported **handled** for
the *missed* hover at (120,115) as well as the successful one at (120,171). That is not a bug — the
code says so at `VaCuusRender.cpp:1384-1388` ("SViewport is our ancestor, so 'handled' has always
included the case where the UI never saw it") — but it means **the `handled`/`unhandled` token is
not a usable coordinate self-check on macOS**, where a mis-aimed move still lands inside the game
window. The token that *does* discriminate is the press attribution from
`TypeShot`/`LobbyDemo.Click`/`M2Demo.Drag`. The plan's block-6 procedure should be amended to say so.

### Row 4 — keyboard text entry. PASS, with the plan's demanded evidence line and one better.

At the corrected coordinate, `vacuus.M1HUD.TypeShot 710 169 vacuus`: the screenshot shows
**"vacuus" sitting in the text field with the caret after the final "s"** and the cyan focus ring
on the field. The evidence line:

```
IME bridge built=yes, platform system absent=no, registered=yes, context active=yes
  -- so the text above went through OnKeyChar -> ProcessTextInput
```

The plan demanded this row read **`absent=no, registered=yes`** — the opposite of the Linux
record's `absent=yes, registered=no`. It does. `context active=yes` is beyond what was asked: the
platform IME context was not merely registered but **activated for the focused field**, on a real
window, which is the precondition every part of row 5 depends on.

### Row 6 — gamepad spatial nav. PASS.

`vacuus.M1HUD.NavShot Gamepad_DPad_Down Gamepad_DPad_Right`, with
`navigation config overridden: yes` and both keys reported handled on the focus path. The
screenshot shows the M2 interaction demo with the magenta focus fill on **BRAVO** while ALPHA and
CHARLIE stay dark — Down enters the grid at ALPHA, Right lands on BRAVO. Exactly the Linux
traversal. The rest of the document is correct too: the TEXT INPUT panel, the WHEEL-SCROLL LIST
with items 01-07 and its scrollbar, and the PASS-THROUGH panel with its pruned child.

### Row 7 — world-space panel. PASS, 16/16.

Every assertion the Linux column lists passed, in the same order: snapshot carries exactly the
button's rect; the raycast click fired OnModelWrite exactly once; the processor consumed it; the
consumed click never reached game input; occlusion (a Slate overlay wins, the processor defers and
consumes nothing, no write reaches the router); pass-through (the game heard exactly one press,
no write on a miss); hover raised mouseover on re-entry; MouseLeave cleared RmlUi's hover chain.

Measured on Metal: occlusion query **0.88 µs avg (max 12.29)**, WS-STALE-RAY re-trace
**1.98 µs avg (max 34.46)**, 500 samples each. Linux recorded 0.47/2.79 and 2.91/34.43 — the same
order of magnitude, with the re-trace slightly cheaper here and the occlusion query slightly
dearer.

The screenshot: the quad stands in-scene running the M4 JavaScript demo, all six panels legible —
GAME-FED SCALARS (313 updates published, CallSign WORLD-1, **Ammo 29**, Stance `Standing` styled by
JS classList, Zone Quad), the bound-bar/rAF-bar pair, the nested struct (Target.Designation M5-T7,
Target.Distance 220), the routed-write button, a 6-row JS killfeed with HS pills, and two JS-timer
damage numbers. **Ammo 29** is the raycast click's routed write (30 − 1) visible in pixels, exactly
as the Linux column recorded it. The panel's small text is slightly ghosted at this minification —
expected for a quad this size, and no worse than the Linux equivalent.

### Row 8 — glass. PASS, on the fallback route, and the plan's risk-4 experiment does not exist here.

Read by eye: the ROUNDED panel smears the clouds and the horizon behind it into soft bands while
the same clouds outside the panel stay sharp; the SQUARE panel does the same with hard corners;
the CONTROL panel with the identical fill leaves the horizon a **hard line** and the floor tiles
individually distinct. The three-way comparison the row exists for is unambiguous.

Route: **bounded copy pass**, both times it was asked.

```
Exp-GLASS-BACKBUFFER-SRV: glass samples the Slate output through a bounded copy pass
  (texture ShaderResource=no, vacuus.GlassBackbufferSRV=1)
```

**Correction to plan §4 risk 4.** The plan says "force the other route once with
`vacuus.GlassBackbufferSRV 0` to prove both work on Metal". That is inverted: the copy route is
already the one Metal takes, and the cvar cannot select the other. `VaCuusSlateElement.cpp:468-469`
computes `bDirectSRV = bOutputSampleable && cvar != 0`, where `bOutputSampleable` reads
`TexCreate_ShaderResource` off the live output-texture desc — and on this venue that flag is
**absent**, so no cvar value can reach the SRV path. Running with the cvar at 0 was done anyway and
logged `ShaderResource=no, vacuus.GlassBackbufferSRV=0`, same route, same correct picture. **The
direct-SRV route is therefore untested on Metal**, and the plugin's own comment at `:463-467`
already predicted precisely this ("an RHI whose swapchain image cannot be sampled does not create
it ShaderResource").

Idle economy, matching Linux's shape: `published=0 skipped=227 (100.0% idle)` over the session
while the camera kept panning behind the glass.

### Row 9 — decorators. PASS, all six.

Linear 90° red→blue left-to-right; repeating linear 45° gold/black hazard stripes; radial
white-core into deep-blue rim; conic full hue wheel about the centre; builtin
`shader(glass-panel)` as a translucent fill with a pale border glow and the checkerboard visible
through it; control cell a plain dark fill. **No banding** in the linear or the radial gradient —
plan risk 7 (`half` being a real 16-bit float on Mac, showing as banding in smooth gradients) is
**not visible at this scale on these gradients**; a numeric diff against the Linux shot was not
performed, so this is an eye verdict, not a measurement.

### Row 10 — material decorators. PASS, on the second attempt.

**First attempt, recorded because it is a venue lesson.** `AutoShot 10` fires about one second
after boot, and on a DDC-cold Mac that is long before an MD_UI material has a Metal shader. The
resulting image has the engine's compile-progress overlay legible in the top-left and all five
material cells drawing their **plain fallback fill**, indistinguishable from the CONTROL cell. Not
a Metal failure — a timing artifact of the row command. Worth noting that the fallback is a plain
fill, not purple and not garbage.

**Second attempt, `AutoShot 900`**, and every cell is correct by eye: TRANSLUCENT a warm brown with
the sky faintly through it, ADDITIVE a bright cyan brightening the clouds behind it, OPAQUE a solid
green that replaces the cell box entirely, MID base the textured pebble material with its game-side
params, TIME-ANIMATED a set of vertical blue/red/magenta gradient bars caught mid-motion, and the
CONTROL cell a plain dark fill with no material. That is the Linux column's list, item for item.
No `LogShaderCompilers: Error` naming `FVaCuusMaterialPS`, no `DrawShader: no FVaCuusMaterialVS/PS
pair` spam — **plan risk 5 did not materialise.** No banding in the TIME-ANIMATED gradients either,
which is the only place risk 7 (`half` being a real 16-bit float on Metal) would have shown here.

### Row 11 — M5 acceptance demo. PASS.

Zero `LogVaCuusJS: Error`. `VaCuus translation: published table v1 (2 entries)`;
`model 'hud' bound … 10 of 10 top-level variable(s)` on **both** the screen-space and the
world-space view. Two beats read by eye:

- **Beat 1**: screen HUD "VaCuus M5 // TSX HUD", Health **90** on the red→green gradient bar, Kill
  Feed with the Simulate button and 2 rows ("Vex » Kilo", "Moth » Rasp"); the world quad to the
  right runs the same document with its own health bar and its own 2 rows.
- **Beat 2** (t+8): Health **52**, Kill Feed grown to 5 rows ("Moth » Rasp", "Rasp » Vex",
  "Unto » Kilo", "Vex » Moth", "Kilo » Rasp"); the quad follows.

The `»` in every row is the translation table's substitution, and the two beats differing in both
Health and row count is the model sweep and the JS killfeed driver both being alive. Structurally
this reads identically to the Linux beat-2 proof; the background scene differs only because the
host project is the ThirdPerson template rather than VcHost's own level.

### Row 12 — PF_FloatRGBA composite permutation. PASS.

Control run (stock ini): `VaCuus composite: elements texture is A2B10G10R10 -> pass-through
(display-gamma target) permutation` — the same format and the same permutation Linux logged, so
the Mac device profile does not override `r.DefaultBackBufferPixelFormat` (as
`BaseDeviceProfiles.ini` predicted).

Forced run (`[/Script/Engine.RendererSettings] r.DefaultBackBufferPixelFormat=3`):
`elements texture is FloatRGBA -> LinearOutput (sRGB->linear decode, gamma 1.0 target) permutation`.

The two RefHud screenshots landed on the same data beat (same scoreboard numbers, same ammo
26 / 120, same damage numbers), which makes the A/B unusually clean. By eye: **opaque surfaces are
identical** — dark panel blues, the gold ammo glow, the red HP bar, the blue MP bar, white text,
green ping meters, red HS pills. **None of the ~2.2× global brightening a missed decode produces.**
One expected, correct difference, the same one the Linux column recorded and slightly broader here:
**the semi-transparent surfaces read lighter on the float target** — the killfeed panel is
noticeably paler and its weapon labels lower-contrast, the scoreboard panel is a shade lighter, and
the minimap disc lighter with less saturated blips. That is linear-space alpha blending against
gamma-space blending, and the Mac's whole killfeed sits over bright sky where Linux's equivalent
sat over one bright sun region.

The ini was reverted after the run; `grep -c DefaultBackBufferPixelFormat DefaultEngine.ini` = 0.

### Row 14 — clean teardown. PASS, and a venue fact worth more than the row.

**On macOS, SIGTERM does not produce the teardown tail.** Every SIGTERM-ended session in this pass
ends its log with

```
LogCore: Engine exit requested (reason: Mac GracefulTerminationHandler)
Log file closed, …
```

and **zero** VaCuus teardown lines — `grep -c "RmlUi shut down"` is 0 in row3, row8 and row12a
alike. The Linux column collected this row from exactly such a SIGTERM. The working route on macOS
is the console `quit`, and with it the tail is complete:

```
UI thread stopping on an in-band shutdown command (0 view(s) closed, 0 queued command(s) dropped behind it)
VaCuus VFS teardown: 0 open(s) served by mounted bundles, 4 by loose roots
VaCuus VFS teardown: 0 script read(s) served by mounted bundles, 1 by loose roots
RmlUi shut down
UI thread exit: released 2 cached model definition set(s)
VaCuus runtime module shut down
```

and no `Recorder destroyed with unpublished resource traffic` line, whose absence is the row's
"zero unpublished NEW resources" condition (`VaCuusRecordingRenderInterface.cpp:200-208` only prints
it when there is traffic). This is the same class of hazard as CLAUDE.md's "`ShutdownModule` never
runs in an `Automation RunTests …; Quit` run" and belongs beside it.

---

## 6. HiDPI — the plan's number-one risk, measured

The plan's risk 1 was "**the view lays out in DEVICE pixels**: the HUD is crisp but half physical
size", with a stated cheapest observation: *compare the logged ViewSize to the window's point size;
2× ⇒ confirmed, before a single screenshot.* That measurement was taken.

**The ratio is 1×, not 2×, and the whole chain agrees with itself.** With
`-ForceRes -resx=1920 -resy=1080`:

- `LogVaCuus: Created view 1 (1920x1080)` and `View 1 size now 1920x1080` — the requested size,
  honoured exactly, and stable for the length of every undisturbed session.
- `LogCore: Display: Tracing Screenshot "ScreenShot00000" taken with size: 1920 x 1080` — the
  backbuffer is the same number, so the composite target and the view are the same size.
- In the one session the owner resized by hand, the view followed to **1728×1084** — and
  1728 × 1117 points is exactly this panel's point size (3456 × 2234 device px ÷ 2), minus a menu
  bar. A full-screen window measures 1728 wide here, not 3456.

So Slate hands VaCuus a rectangle in the same units the window is measured in, VaCuus lays out
1:1 in those units, and no scale factor is applied or missing anywhere in the plugin. The
consequence on a 2× panel is that the *whole frame* — the 3D scene as much as the UI — is produced
at that resolution and scaled by the compositor; the UI is not half-size, not mis-aimed, and not
distinguishable from the engine's own output in sharpness. **Risk 1 did not occur in this venue.**

The corroborating evidence is row 3: `vacuus.M2Demo.Rects 5` reports `viewSize=1920x1080` and puts
the TextInput field at `(511,95)-(909,132)`, the Linux literal to the pixel. Under a 2× layout
error those numbers would be somewhere else.

**What this does NOT close.** Only the `-game` venue was measured. `IsHighDPIModeEnabled()` is
`bIsHighResolutionCapable && IsHighDPIAwarenessEnabled()` (`MacPlatformApplicationMisc.h:35`), both
of which are true for the editor bundle (`NSHighResolutionCapable` in the plist;
`EnableHighDPIAwareness` defaults to 1, `GenericPlatformApplicationMisc.cpp:25`), so an **editor /
PIE** window is a different measurement and was not taken. Nor was a **Retina ↔ non-Retina display
move** (plan risk 11) — this machine has one display. Both remain open.

---

## 7. What could not be run, and why — recorded, not dropped

| Item | Why not |
|---|---|
| **Row 5 — IME composition** | Needs a human at the keyboard composing into the field on the physical machine. Everything automatable about it *did* move: registration is proven (§4), and row 4's own evidence line now reads `absent=no, registered=yes` — the opposite of the Linux record and exactly what the plan asked this row to produce. The composition, candidate-window placement, the fast-vs-slow typing divergence (risk 2b) and the row 4 → row 5 → row 4 re-check (risk 6) are all still owed. |
| **Row 13 — live reload** | Needs an interactive editor PIE session plus a mid-run file edit; the watcher is in the editor-only `VaCuusEditor` module. Same reason the Linux column gave. macOS additionally uses FSEvents rather than inotify, so its timing has never been characterised against ours. |
| **Row 15 — Shipping ignition flags, and all of blocks 10–11** | **Packaging was not attempted, on the disk guardrail.** The pass started with 47 GB free and the instruction to stop below 10 GB; a Development *and* a Shipping cook on top of the editor build was not a safe bet against that budget, and the value of the remaining rows was higher. Nothing about signing, staging shape or the bundle hash was tested. |
| **Perf passport §11 macOS columns** | Not filled. The soaks need an undisturbed machine, and this one was shared with its owner mid-session (§3). What exists is fragmentary and is deliberately *not* promoted into passport cells: row 8's `published=0 skipped=227 (100.0% idle)`, and windowed frame rates of 20–25 fps that are not comparable to Linux's 227 fps offscreen figure and should not be quoted as if they were. |
| **Glass direct-SRV route** | Structurally unreachable on this venue — the Slate output texture carries no `TexCreate_ShaderResource`, so no cvar value selects it. See row 8. |
| **Anything at Metal SM6** | Needs an M2-or-later Mac (`GPUFamilyApple8`), or at minimum a cook targeting `SF_METAL_SM6`. Neither the rendering nor the *shader compile* was exercised at SM6 — see the correction at the top of this document. |
| **`MTL_DEBUG_LAYER=1` sweep (plan risk 9, "highest value per keystroke")** | Not run. It should be — it is one environment variable and it is the only thing that would have turned the composite rect's unclamped scissor into a named abort during a resize. The owner did resize a window mid-session and nothing crashed, which is weak evidence at best. |
| **Rows 16/17 candidates — lobby demo** | `vacuus.LobbyDemo` refused: `'chrome.rml' is not served by any DevUI root`. That is **not a Mac finding**: `chrome.rml` is host-project content and it exists in neither the plugin tree nor VcHost on Linux either. The refusal is the designed behaviour that `VaCuusLobbyDemoTest.cpp:42,53` asserts. Needs a host project that ships lobby chrome; then it is worth running on both platforms. |
| **`vacuus.WorldDemo.Mips` (row 17 candidate, plan risk 8)** | Not run — the world-panel mip chain on Metal's raster path is untested. Cheap to add next time. |
| **`Proof.LiveReload.PIE`** | Not selected by `Automation RunTests VaCuus` (its path has no "VaCuus" substring), as the plan noted. Not named explicitly either. |
| **`BuildPlugin -StrictIncludes` for Mac** | Not run. The plan notes this leg *can only be run on a Mac*, ~40 min, and expects a small header-set wave. It remains the single highest-value unexecuted item after the IME row. |
| **The `/experimental:c11atomics` question, the Win64 disk literal, the Win64 TSF IME re-check** | Another platform's evidence by definition. Unchanged. |

---

## 8. Disk

The hard guardrail was: check before and after every heavy step, stop below 10 GB free.

| Point | Free |
|---|---|
| Before anything | **47 GB** |
| After the cold editor build | 42 GB |
| Low-water mark (first DDC-cold Metal shader compile) | **39 GB** |
| At the end of the pass | **42 GB** |

The low-water mark was 39 GB — never within 29 GB of the stop line, and the ~5 GB consumed is
mostly one editor build plus the Metal shader DDC. Space came back afterwards because each row run
deletes the previous run's `Saved/Screenshots`. Net: **`~/VaCuusMacTest` is 4.0 GB** — project,
plugin, five dylibs, `Intermediate`, and per-row logs and screenshots.

**Nothing of the owner's was deleted.** Everything this pass created lives under `~/VaCuusMacTest`
and in the engine's usual per-user caches (`~/Library/Logs/VcHost`,
`~/Library/Logs/Unreal Engine/VcHostEditor`, `~/Library/Application Support/Epic`). No editor
process is left running. The screenshots read for this document are on the Mac at
`~/VaCuusMacTest/rows/<row>/ScreenShot00000.png`; they are deliberately **not** committed here —
the repo is not where 20 MB of PNGs belong, and the row text is what carries the verdict.

---

## 9. What the owner has to decide

1. **Land the two fixes.** `03a3fdf` (libpng / Installed engine) and `49bcad0` (Backspace). The
   first is a **release blocker for anyone on a Launcher engine on any platform**, not just macOS.
   Neither is pushed.
2. **The Mac automation suite is red at 194/197 and two of those three are a two-line fix**
   (§4). The fix is written out but deliberately not applied, because its correctness could not be
   re-verified against the Linux 197/197 baseline from here. Verify on Linux, then land.
3. **`VaCuus.Js.Cost.CombinedChurn`'s gate is not jitter-proof on a laptop** — two runs of the same
   binary straddled it (7.76 and 5.10 ms p99 against a 5.0 gate) while p50 and the means barely
   moved. Either the gate becomes venue-aware or the test is excluded from laptop runs; the number
   is recorded either way.
4. **Packaging and the whole Shipping column are still owed on macOS**, and so is the interactive
   IME row. Both want a session with the machine to itself. The IME row is the one that has never
   run anywhere and is the largest remaining unknown in the product.
5. **Metal SM6 needs different hardware.** Every visual result here is SM5.
