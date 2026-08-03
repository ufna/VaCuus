# macOS pass — the plan: prep, run order, and where it will bite

## 1. What this pass is for

This is the **first execution of VaCuus on Apple silicon and Metal, ever**. No line of this
plugin has been compiled by Apple clang, no shader of it by Apple's metal front-end, and two
whole code paths — HiDPI layout and the platform IME bridge — have never run anywhere, because
Linux offscreen has no backing-scale factor and no `ITextInputMethodSystem`. The pass fills the
**macOS Metal** columns of `2026-08-vacuus-manual-matrix.md` and
`2026-08-vacuus-perf-passport.md` §11, and closes the macOS half of the quickjs export claim.
It **cannot** stand in for the Win64 rows (disk literal, TSF IME, the MSVC `/experimental:c11atomics`
question) — those are a different platform's evidence and stay open. Rule, unchanged from the
Linux column: run the same commands, read the same assertions, and a row that cannot run gets
its reason recorded, never silently dropped.

---

## 2. Prep the machine

### Start these before the session — they are downloads, not work

| Item | Cost | Notes |
|---|---|---|
| Xcode 26.1.x (App Store) | ~20–40 GB installed *(inferred — check the listing)* | Full `Xcode.app`. Command Line Tools alone is a hard fail. |
| UE **5.8.1** via Epic Launcher | ~60–80 GB *(inferred)* | Must be **5.8.1 exactly** — `Engine/Build/Build.version` here is 5.8.1. |

### P0 — five minutes, before anything else

Two questions gate the whole trip. Answer them first.

```bash
uname -m                            # must print arm64
sysctl -n sysctl.proc_translated    # must print 0 (not a Rosetta shell)
sw_vers -productVersion             # ≥ 14.0 required; ≥ 15.0 wanted (see feature level)
```

**If `uname -m` is `x86_64` (and not Rosetta): STOP.** UE 5.8 has *removed* Mac rendering on
Intel — `MetalDevice.cpp:202-209` opens a modal ("Rendering support for Intel based Mac has been
removed in this version of the engine.") and calls `RequestExit`; `MetalRHI.cpp:408` re-asserts it
as a hard `check()`. `GPUFamilyApple7` is M1 and newer. There is no workaround and no flag; the
macOS column needs a different Mac.

Second: **does the Launcher currently offer 5.8.1 for macOS?** If it only offers 5.8.0 or 5.9,
the cheap path collapses and the engine decision below reopens. Check it before starting any
download.

### The engine decision: Launcher binary, not a source build

**Take the Launcher install.** Verified, not assumed: an Installed engine *does* compile
source-only plugins on Mac — `AppleToolChainSettings.cs:167-179` branches on
`Unreal.IsEngineInstalled()` to enforce a minimum Xcode for installed engines (dead code if
installed builds never compiled), and `Mac/Build.sh:11` skips only the UBT/ShaderCompileWorker
*self*-build when `InstalledBuild.txt` exists — it still runs UBT at line 35. VaCuus needs
nothing from the engine but headers and libs: five modules entirely under `Plugins/`, zero engine
patches. The alternative costs a clone + `Setup.sh` + `GenerateProjectFiles.sh` + a full engine
compile; the Linux equivalent tree here measures **223 GB** (`.git` alone 80 GB) and was built on
a 32-thread box. Wall-clock for a MacBook source build is genuinely unknown — no number exists in
this repo and none was invented. Budget a full day if you are forced onto that path.

Whichever engine flavour is used, **name it in every Method sentence** — a 5.8.0 or 5.9 install
makes this a different experiment.

### Ordered checklist

1. **P0 above.** Stop on Intel.
2. **Xcode.** Install, launch once to accept the license and let it install components, then:
   ```bash
   sudo xcode-select -s /Applications/Xcode.app
   xcode-select -p        # → /Applications/Xcode.app/Contents/Developer
   xcodebuild -version    # want 26.1.x
   ```
   Window is `MinVersion 15.2.0` … `MaxVersion 26.9.0` inclusive, `MainVersion 26.1.1`
   (`Apple_SDK.json:3-5`; max is inclusive per `UEBuildPlatformSDK.cs:328`). A CommandLineTools
   selection throws a `BuildException` at `AppleToolChainSettings.cs:155-161` **and** makes
   version detection return null (`ApplePlatformSDK.cs:102-110` reads
   `$(xcode-select -p)/../Info.plist`).
3. **git + git-lfs, before cloning.**
   ```bash
   brew install git git-lfs && git lfs install
   ```
4. **UE 5.8.1** via the Launcher. No `Setup.sh`, no `GenerateProjectFiles`, no engine compile.
5. **Host project.** A fresh C++ template project is enough — VcHost is itself a stock template
   with five source files and nothing the matrix depends on. **Name it `VcHost`** so every
   recorded command and every `Saved/Logs/VcHost.log` path transfers verbatim. Set the default
   map to something **empty**, not `OpenWorld` — the first Metal shader compile is DDC-cold and
   OpenWorld makes it far longer than it needs to be.
6. **Clone the plugin — clone, do not copy, do not symlink.**
   ```bash
   git clone git@github.com:ufna/VaCuus.git <Project>/Plugins/VaCuus
   cd <Project>/Plugins/VaCuus && git lfs pull
   ```
   ~5.4 MiB pack + 17.8 MB LFS. A copy of `/w/Unreal/VaCuus` drags untracked Linux `Binaries/`
   and `Intermediate/`; a clone is clean by `.gitignore:2-4`. A symlink hits the same UBA
   `cross-process rename-while-open` abort as on Linux — UBA is on by default
   (`BuildConfiguration.cs:55`) and ships a Mac agent. One tree on the Mac, in `Plugins/`; the
   VaCuus/VcHost split is a Linux-dev-box artifact and the plugin's own second-working-tree
   detector exists because two checkouts are silently wrong.
7. **LFS gate — do not skip this one.** A missing smudge fails as a *Warning*, not an error:
   the 129-byte pointer still exists on disk so `ResolveExistingDocument` succeeds
   (`VaCuusEngine.cpp:140-141`), `Rml::LoadFontFace` then fails on a text file, and the code takes
   the `:145-147` Warning path. The HUD renders **with no text**, ~54k "No font face defined"
   warnings per suite run (bead `VaCuus-akj.21`), and rows 1, 2, 3, 4, 6, 9, 11 are silently void;
   the Spike materials void row 10, `M_VaCuusWorldPanel` row 7, `DevUIBundle.uasset` row 15.
   ```bash
   git lfs ls-files            # 36 lines: 35 with '*', exactly ONE '-'
   file Content/DevUI/fonts/LatoLatin-Regular.ttf   # must say TrueType, not ASCII text
   ```
   The single permitted `-` is `Tools/scan-fixture/planted_lfs_pointer.txt` — a deliberately
   committed pointer fixture (`Tools/fab_scan.sh:22-23,119-126,170`); `fab_scan.sh` reporting it
   is that script's self-test, not a failure.
8. **Project config**, per `docs/buyer/setup.md` §3:
   - `DefaultGame.ini`: `+DirectoriesToAlwaysCook=(Path="/Game/Bundles")` and
     `[VaCuus] BundleAssetPath=…` — needed for row 15.
   - `DefaultEngine.ini`, before packaging:
     `[/Script/MacTargetPlatform.XcodeProjectSettings] bMacSignToRunLocally=True` and
     `[/Script/MacTargetPlatform.MacTargetSettings] TargetArchitecture=MacTargetArchitectureHost`
     (see block 10).
   - `r.DefaultBackBufferPixelFormat=3` is row 12's A/B — added *during* the session and reverted
     after.
9. **First open.** UBT compiles the five plugin modules; then a first-run Metal shader compile.
   Let it finish before timing anything.
10. **Smoke:** the console knows `vacuus.M2Demo` (`setup.md` §1).
11. **Record the bound feature level.** Mac SM6 needs macOS ≥ 15.0 **and** `GPUFamilyApple8`
    (M2+) — `MetalRHI.cpp:255-267`. Otherwise `:421-427` logs `SM6 is enabled but is not
    supported on this system, falling back to SM5` and `:433-441` pins `SP_METAL_SM5`. Stock
    defaults already target both (`BaseEngine.ini:3434-3435`), so nothing to configure — but grep
    the log for that Warning and write down which level bound. **An SM5 run and an SM6 run are not
    the same cell** for rows 8 and 12.

**Nothing else installs.** dotnet is bundled and arch-selected (`Mac/SetupDotnet.sh:46-58`); the
Metal shader converter and ShaderConductor ship in-tree (`MetalShaderConverter.build.cs:88-96`,
`MetalShaderFormat.Build.cs:28-46`); FreeType2 — our only engine third-party — has Mac static libs
(`FreeType2.Build.cs:82-89`). No Vulkan/Metal SDK, no Homebrew toolchain, no separate Apple
downloads.

---

## 3. The session

**Venue translation — eight facts that rewrite every copied command.** Write these down before
using any recipe from CLAUDE.md or the matrix.

| | |
|---|---|
| a | **No `UnrealEditor-Cmd` on Mac.** The editor is `Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor` (`CommandletUtils.cs:495`). Run the inner binary directly — `open` loses stdout. |
| b | **`-ExecCmds` needs a TRAILING COMMA.** The value parse uses `bShouldStopOnSeparator=false` (`ParseExecCommands.cpp:63`), so the value swallows later arguments and the launcher's appended `-game` becomes part of the last command. Write `-ExecCmds="vacuus.RefHud,"`. (Splits on commas, never semicolons.) |
| c | **There is no offscreen mode on Mac at all.** `FNullPlatformApplicationMisc::IsUsingNullApplication()` is `#if PLATFORM_WINDOWS \|\| PLATFORM_LINUX` (`NullPlatformApplicationMisc.cpp:17-23`) and no Mac file references `-RenderOffScreen`. Windowed is the only mode — drop the flag. |
| d | **Keep `-ForceRes -resx=1920 -resy=1080`.** Platform-neutral (`GameEngine.cpp:414`); without it the engine clamps to a "convenient windowed resolution" (`:416-440`) and every perf row stops being comparable. |
| e | **Screenshots land in `Saved/Screenshots/MacEditor/`** for Dev `-game` (`Paths.cpp:553-556` + `MacPlatformProperties.h:52-69`); packaged shots land inside the `.app` — use `-SaveToUserDir`. |
| f | Kill editors **by PID** (`pgrep -a UnrealEditor`), never `pkill -f`. |
| g | Read counts and PerfLog windows from `Saved/Logs/VcHost.log`, never stdout. |
| h | `VaCuus.uplugin` has no `SupportedTargetPlatforms` and no module allow-list — all five modules build for Mac by default; nothing to enable. |

**Conflict resolved (c).** The code-risk assessment suggested running rows 3/4/6 headless
"to reproduce Linux exactly"; the machine-prep assessment flagged it as unverified. The execution
assessment read the source: there is no null application on Mac. **Windowed it is** — which is why
block 6 exists.

### Run order

| # | Block | Est. | Must produce |
|---|-------|------|--------------|
| 0 | Preflight / venue translation | 20–30 m | the eight rewritten command forms, written down before use |
| 1 | Build `VcHostEditor Mac Development` | 20–45 m | `Result: Succeeded`; any vendored-C clang diagnostic |
| 2 | First editor launch → Metal shader compile | 10–30 m | zero `LogShaderCompilers: Error` naming `VaCuus*.usf` |
| 3 | Automation suite, `-nullrhi` | 10–20 m | **189** `Result={Success}`, 0 non-Success, **and the one IME branch line** |
| 4 | Automation, real Metal RHI, GPU pair | 5–10 m | 2/2 Success, no `SKIPPED under NullRHI` |
| 5 | Matrix rows 1, 2, 6, 7, 8, 9, 10, 11, 14 — windowed | 60–75 m | 9 screenshots read by eye, the node count, InputSmoke 16/16, 9 clean teardown tails |
| 6 | Matrix rows 3, 4 — with the coordinate correction | 15–20 m | rect list, `covered=yes`, hover/typed shots, the `handled` line, IME status **registered=yes** |
| 7 | Dev perf soaks (100 s + 35 s + 25 s + M2) | 25–35 m | PerfLog windows for passport rows 1, 2, 2a, 2b, 3, 4, 7, 8 |
| 8 | Row 12: ini A/B `-game`, then editor PIE | 25–35 m | both `elements texture is … -> …` lines + A/B by eye, twice |
| 9 | Row 5 IME + row 13 live reload — **human at the keyboard** | 40–60 m | composition in-field, candidate placement verdict + the Slate differential; PIE repaint + the shadowing Warning naming the path |
| 10 | Package Development, then Shipping | 60–150 m | 2× `BUILD SUCCESSFUL`, staging-shape check, bundle hash vs Linux's `adcb1da0…` |
| 11 | Shipping soaks + row 15 + RAM A/B | 45–60 m | Shipping PerfLog windows, M==0 on both VFS lines, gate screenshots, median RAM delta + band |
| 12 | Write-up | 30–45 m | filled cells **with Method**, venue notes, every non-executable item recorded with its reason |

**Total ≈ 6–9 h, realistically two sittings.** Blocks 0–8 are one sitting and stop cleanly before
packaging. **Only block 9 and block 8's PIE leg need a human at the keyboard** — block 5's gamepad
row does not, despite appearances (`NavShot` synthesizes the FKey sequence through
`FSlateApplication` on `OnBeginFrame`, `VaCuusRender.cpp:1814-1856`). Block 10 is the one that can
blow the estimate.

### Block detail, where it differs from Linux

**1 — Build.** `Engine/Build/BatchFiles/Mac/Build.sh VcHostEditor Mac Development
-project=/abs/VcHost.uproject`. Editor targets build **host architecture only**
(`EditorDefaultArchitecture=MacTargetArchitectureHost`, `BaseEngine.ini:3441`) — the universal
trap is in packaging, not here. There are **zero x86 intrinsics** in the vendored quickjs-ng or
RmlUi trees, and the only platform conditional in `VaCuusJs.Build.cs:41-48` is a Win64 block, so
clang-on-Mac takes the same path as clang-on-Linux. Record the outcome either way: "Apple clang
compiled the vendored quickjs unmodified" is a real new data point.

**2 — Metal shader compile.** Four global-shader files (`VaCuusUI/Blur/Gradient/Material.usf`),
none platform-gated: the only `ShouldCompilePermutation` in the tree gates on
`MaterialDomain == MD_UI` (`VaCuusMaterialDraw.h:63-65, :98-100`) with no shader-platform filter.
The Mac profile targets **two** platforms (`SF_METAL_SM5` + `SF_METAL_SM6`) at
`MetalLanguageVersion=7`, so every permutation compiles twice through a front-end that has never
seen this HLSL. An error here is a **real finding**, not a venue artifact, and it blocks every
visual row.

**3 — The suite: 189 tests.** `Automation RunTests VaCuus` does a plain substring match
(`AutomationCommandline.cpp:133-135`), selecting 189 of the 191 registered tests at HEAD; the last
recorded Linux run was 186/186 at an older HEAD, +3 being the two lobby-demo tests and the
world-panel mip test. Two tests self-skip loudly under `-nullrhi` by design
(`VaCuus.Render.Composite.LinearOutputGPU`, `VaCuus.World.MipContentGPU`) — that is block 4's job.

**3b — the highest-value automated result of the trip.** `VaCuus.Input.TextEntry` does not
hardcode Linux; it asserts **relatively**: `bRegistered == !bPlatformImeAbsent`
(`VaCuusTextEntryTest.cpp:433-436`). On Linux both sides take the "absent" branch and registration
is never exercised. On Mac `FMacApplication` returns a live `FMacTextInputMethodSystem`, so the
test flips branch and becomes the first automated proof that **registration actually happens**.
Read the one-line observable first (`VaCuusTextInput.cpp:966-975`): either
`IME: this platform exposes no ITextInputMethodSystem …` (Warning) or `IME: platform
ITextInputMethodSystem present; composition is available` (Log). That line decides whether the run
exercised the new path at all. A failure of this assertion is a **real bug** in the registration
plumbing, not a venue artifact.

**5 — the scripted rows.** Session shape:
`…/UnrealEditor.app/Contents/MacOS/UnrealEditor /abs/VcHost.uproject -game -ForceRes -resx=1920
-resy=1080 -ExecCmds="<row command>,"`. End each with SIGTERM to collect row 14 too. One timing
note: `AutoShot N` fires after N *recorded* frames; windowed at 60 Hz vsync, `AutoShot 1200`
is ~20 s, not the ~5 s it was at Linux's offscreen 227 fps.

**6 — rows 3 and 4, the coordinate trap.** On Linux the window sat at the desktop origin and
"view pixels" and "Slate absolute pixels" were the same numbers. On Mac they are not, and the
plugin does not compensate: `HoverShot`/`TypeShot` feed their `x y` straight to
`FSlateApplication::SetCursorPos` and into a synthesized `FPointerEvent` at that **absolute**
position (`VaCuusRender.cpp:1357-1397`), while the widget converts back with
`AbsoluteToLocal(...) * Geometry.Scale` (`SVaCuusWidget.cpp:381-388`). The delta is the window
origin plus the title bar. Procedure: (1) move the window to the top-left of the main display, or
run fullscreen; (2) run `vacuus.M2Demo.Rects 5` first — its output is in **view pixels** and is
window-position-immune; (3) `vacuus.M2Demo.Hit 120 115 6` is also pure view-pixel space
(`:1311-1343`) so it answers correctly regardless of placement; (4) for `HoverShot`/`TypeShot`,
add the window origin. Built-in self-check: `MoveMouseTo` logs `…the event handled somewhere on
the bubble path (Slate does not say by whom) | unhandled (it fell through)` (`:1394-1397`) —
`unhandled` means fix the offset and re-run, not record a FAIL. The **`unhandled` token is the
decision and is unchanged**; the handled branch stopped naming a culprit in bead
`VaCuus-akj.6.41` because it never could (SViewport is our ancestor, so a pass-through move also
reports handled). For "did the UI actually take it", use a command that CLICKS —
`vacuus.LobbyDemo.Click` or `vacuus.M1HUD.TypeShot` — which now prints `the press was taken by
THE UI (VaCuus captured the mouse) | THE GAME (VaCuus declined…)` from Slate's mouse capture,
the same attribution `vacuus.M2Demo.Drag` has always had. Note `FMacCursor::SetPosition` warps the **real system cursor** via
`CGWarpMouseCursorPosition` and defeats Apple's 0.25 s post-warp suppression
(`MacCursor.cpp:491-506`): the pointer jumps on the desktop and the game window must be frontmost.
Row 4's own evidence line must read **absent=no, registered=yes** — the opposite of the Linux
record.

**7 — Dev soaks.** In order: RefHud 100 s (`vacuus.M1HUD.PerfLog 1, vacuus.RefHud,`) → passport
rows 1, 2a, 2b, 3, 4, 7, 8; static idle 35 s (`vacuus.M1HUD` + PerfLog) → row 4's confirmed idle
venue and row 8's idle gate; glass idle 25 s (`vacuus.M5Glass` + PerfLog) → row 8's glass line;
M2 demo ~60 s → row 2's typical-scale figures. **A number without a Method is not a filled cell**
in this document's convention: name venue, duration, frame count, window selection (exclude the
boot window from steady figures), and which scopes were summed. If the row-2 soak is skipped,
write "not separately measured" — never leave the cell blank.

**8 — row 12.** The Mac device profile does **not** override `r.DefaultBackBufferPixelFormat`
(`BaseDeviceProfiles.ini:1486-1493`), whose default is 4 = A2B10G10R10 (`SceneTextures.cpp:71-79`),
so the control run is *expected* to log the same `A2B10G10R10 -> pass-through` as Linux. If Metal
returns something else that is data, not failure — the row asserts the **format→permutation
pairing**. Steps 1–3 are `-game`; only the PIE leg needs a human, and it is the leg the Linux
column explicitly deferred to this pass. Revert the ini before block 10.

**10 — packaging, the block that can eat the day.**
- **Xcode is mandatory here, not optional.** On a Mac host UE 5.8 always drives packaging through
  a generated stub Xcode project and `xcodebuild` (`AppleExports.cs:45-65, :229-271`).
- **Signing will stop you without a team.** Defaults are `bUseAutomaticCodeSigning=true`,
  `bMacSignToRunLocally=false`, empty `CodeSigningTeam` (`BaseEngine.ini:3451-3455`). The one-line
  fix with no Apple Developer account: `bMacSignToRunLocally=True`, which writes
  `CODE_SIGN_IDENTITY = -` (ad-hoc) into the xcconfig (`XcodeProject.cs:2274-2290`). Ad-hoc is
  enough to run the package on the same Mac and for every soak in block 11.
- **Notarization is not part of `BuildCookRun`** and is not needed for this pass. It exists only
  as a BuildGraph task over a `.dmg` (`NotarizeTask.cs:103, :151-152`) and needs a paid account.
  Without it the `.app` cannot be handed to another Mac without a Gatekeeper right-click-Open.
  That blocks distribution, not a single row here.
- **Pin the architecture.** *Conflict:* one assessment reads `UEBuildMac.cs:217-227` as expanding
  to universal only in distribution/build-machine mode; the other reads
  `MacPlatform.Automation.cs:28-51` as taking `TargetArchitecture` straight from the ini, whose
  shipped default is `MacTargetArchitectureUniversal` (`BaseEngine.ini:3439`). **Call: set
  `TargetArchitecture=MacTargetArchitectureHost` explicitly.** Both readings agree that pinning
  works, it costs one ini line, and it removes a fork that would otherwise double the compile and
  make any disk figure non-comparable. Record which setting produced the package.
- **The bundle layout moves the evidence.** Output is `<Staged>/Mac/VcHost.app` with the project
  tree under `Contents/UE/`, so `Saved/` lands *inside* the bundle. Pass **`-SaveToUserDir`**
  (→ `~/Library/Application Support/Epic/VcHost/Saved/`) — cleaner, and the habit a notarized app
  would require anyway.
- Keep the recorded traps: touch `VaCuus.Build.cs` before packaging (stale receipt), and package
  **Development first, then Shipping**.
- Staging shape: Development stages the loose DevUI tree beside the bundle; **Shipping must stage
  only `DevUIBundle.uasset`** (Linux: 28 entries vs 1).

**11 — Shipping column.** Run the staged inner binary with the ignition flags (`-VaCuusRefHud`,
`-VaCuusM5Demo`, `-VaCuusPerfLog`, `-VaCuusMemProbe`, `-VaCuusLobbyDemo`) — all `FParse::Param`
switches, no console needed. **The RAM row's Method must change on Mac, and this is a real
methodological difference, not boilerplate:** `FPlatformMemory::GetStats().UsedPhysical` is
`ri_phys_footprint` from `proc_pid_rusage` on Apple (`ApplePlatformMemory.cpp:423`), where the
Linux figure it is being compared to is `VmRSS` from `/proc/self/status`
(`UnixPlatformMemory.cpp:917`). `phys_footprint` counts compressed and certain wired/IOKit
memory that RSS does not — say so on the row. Consequence: the Linux P0 cross-check sampled the
plugin-disabled build externally *because* the external field matched the in-process one; **there
is no Mac command that reads `phys_footprint` from outside** (`ps -o rss` is not it), so that leg
either changes instrument or is recorded as not reproduced.

**Three Mac-first observables to capture deliberately** — nothing else will surface them:
`Exp-GLASS-BACKBUFFER-SRV` (block 5, row 8 — see risk 4); the bundle mount line, which should read
`resident buffer (FPlatformProperties::SupportsMemoryMappedFiles() is false on this platform)`
exactly as on Linux (`VaCuusBundleMount.cpp:93`; `MacPlatformProperties.h` has no override); and
the cook's bundle hash, expected to match Linux's `adcb1da0b34dffdb071d3f9db02fd780eceb1f4e700eae66ed79966ed8015017`
at 461881 bytes — identical is a free cross-platform determinism proof, different is a finding
worth chasing before anything else in the packaging block.

---

## 4. Where it will probably break, and what that looks like

Ranked. **The first two have literally never executed anywhere** — everything below them has run
on Linux and is only meeting a new compiler or RHI.

| # | Risk | What you will SEE | Cheapest observation |
|---|------|-------------------|----------------------|
| 1 | **HiDPI: the view lays out in DEVICE pixels** | The HUD is **crisp but half physical size** — tiny text, tiny buttons, huge margins. NOT blurry, NOT mis-aimed. | Compare the logged ViewSize to the window's point size. 2× ⇒ confirmed, before a single screenshot. |
| 2 | **IME: the whole `ITextInputMethodContext` runs for the first time** | Composition duplicated, inserted at the wrong offset, committed twice — or the field stops accepting characters after one composition. | Row 5, but with a **Latin** keyboard first: hold `e` for the accent picker, or `Option-e` then `e`. Marked text, same never-run path, no CJK IME needed. |
| 2b | IME shadow staleness *within one game frame* | Type slowly → correct. Type fast → characters land at the wrong offset or the composition duplicates. | Same field, same text, twice: ~2 chars/sec vs as fast as you can. A divergence is this and nothing else. |
| 3 | IME candidate window misplaced ~2× on Retina | Candidate window drawn at roughly double the correct screen offset, usually off the bottom-right. | **Differential test:** compose into a native Slate text box (the UE console, `~`) in the same session. Misplaced for both ⇒ engine, record as a venue note; misplaced only over RmlUi ⇒ ours. |
| 4 | Glass takes the **copy fallback**, not direct SRV | Glass region shows garbage/black/last-frame content, or a Mac-only per-frame allocation shows as a frame-time regression. | Row 8's latched log line already names the route. Then force the other route once with `vacuus.GlassBackbufferSRV 0` to prove both work on Metal. |
| 5 | `VaCuusMaterial.usf` meets the Metal compiler | `LogShaderCompilers: Error` naming `FVaCuusMaterialPS` at startup, MD_UI materials going purple; or `DrawShader: no FVaCuusMaterialVS/PS pair` spam and decorators drawing nothing. | Block 2's log grep, then row 10. |
| 6 | Metal/AppKit intercepts keys **before** Slate once the IME context is active | After the first composition, plain typing stops working — or double-inserts. | **Row 4 → row 5 → row 4 again**, one session. The second row-4 run is the whole test; it has no Linux equivalent. |
| 7 | `half` is a real 16-bit float on Mac (`bSupportsRealTypes=RuntimeGuaranteed`) and the material gamma encode uses it | Visible **banding** in smooth material-decorator gradients, or a slight hue shift vs the Linux shot — worst in dark tones. | Row 10, diffed against the Linux row-10 shot at the same resolution. |
| 8 | World-panel mips forced onto the **raster** path (PF_B8G8R8A8 is not in Metal's typed-UAV list) | Minified world panels show stale or black far mips, or a fringe at glancing angles. The compute-branch `ensureMsgf` does **not** fire. | Row 7 with `bGenerateMips` on, camera walking away; compare the `WorldMips` PerfLog scope shape to Linux. |
| 9 | Composite output rect is the one **unclamped** viewport/scissor in the render path; Metal is stricter than Vulkan | Hard crash or a `setScissorRect:` assertion **during a window resize or a monitor switch** — transitions only, which is why it survived Linux. | Run the whole matrix once with `MTL_DEBUG_LAYER=1` in the environment. One env var turns every latent bounds/usage violation into a named abort. Highest value per keystroke on this list. |
| 10 | Cross-module `rmlui_dynamic_cast` | Every cast answers null: carets park at end-of-text, `GetSelection` returns false, **nothing logs an error**. | `VaCuus.Rml.CrossModuleCast` (`VaCuusRmlCastTest.cpp:223`), which exists to make this a named failure. Must run in an **editor** build — monolithic links statically and masks the class. |
| 11 | Retina ↔ non-Retina display move (no Linux equivalent) | A stretched frame or two (by design), then a correct relayout — or a permanently wrong size. On the lobby, the layout **mode** flips: 1280×800 points is 2560×1600 px (fluid) on Retina and 1280×800 px (letterboxed to the 1920×1080 design) externally. | `vacuus.LobbyDemo` windowed, drag between displays, watch the logged ViewSize and whether the design box letterboxes. Re-run any coordinate row afterwards — `CachedInputGeometry` only refreshes in Tick. |
| 12 | quickjs Darwin branches under bare `-std=c11` | `implicit declaration of function 'pthread_cond_timedwait_relative_np'`, or missing `malloc_size`. | Block 1. Fix is a Mac-only `PrivateDefinitions.Add("_DARWIN_C_SOURCE")` beside the Win64 block at `VaCuusJs.Build.cs:39-48` — **not** a source patch. Mechanism inferred (no macOS SDK here to read `sys/cdefs.h`); the `-std=c11` and `_GNU_SOURCE` facts are read. |

**Ruled out — do not spend time here.** Clicks are **not** mis-aimed: `ToViewPixels` is the exact
inverse of the layout scale, so hit-testing, snapshot rects and the composite rect agree at any
scale factor; if clicks *are* mis-aimed the cause is a stale `CachedInputGeometry` after a DPI
change (risk 11), and that is a distinct bug. Surrogate-pair handling is identical —
`PLATFORM_TCHAR_IS_CHAR16` is 1 on Mac (`MacPlatform.h:46`) exactly as on Unix. The Metal drawable
*is* sampleable/blittable (`setFramebufferOnly:NO`, `MetalViewport.cpp:259`), so risk 4's copy path
is legal. Vertex-attribute offsets 0/8/12 at stride 20 are all 4-byte aligned, which is what Metal
requires. Every AppKit read of our IME context is wrapped in a blocking `GameThreadCall`, and
`VaCuusTextInput.cpp` contains no blocking primitives at all — a deadlock there would be the worst
possible Mac outcome and the code is structurally immune. RmlUi's vendored macOS branches reduce to
one `RMLUI_BREAK` redefinition in a path that never compiles (`RMLUI_DEBUG` is never defined), and
`Backends/` — the only tree with real platform code — is never compiled at all.

**Conflict resolved (risk 1 vs the coordinate literals).** Both assessments agree Slate absolute
space on Mac *is* device pixels. They disagree on the practical consequence: one predicts every
literal matrix coordinate lands elsewhere; the other says Retina does not double the numbers
because the whole chain is consistently in device pixels. **Both are right about their own claim** —
the physical *size* of the UI is wrong (risk 1), while the coordinate *space* is self-consistent
(block 6). The arbiter is not argument, it is the log: read the ViewSize, then run
`vacuus.M2Demo.Rects 5`. If the TextInput field is no longer near `(511,95)-(909,132)` as the Linux
column records, the literals are invalid for that window and must be recomputed.

**One asymmetry between the two Mac columns, which has no Linux analogue.** The packaged game's
Info.plist sets `NSHighResolutionCapable = false` while the **editor's** sets it `true`
(`Info.Template.plist:10-11` vs `Info-Editor.Template.plist:43-44`), and
`IsHighDPIModeEnabled()` is `bIsHighResolutionCapable && IsHighDPIAwarenessEnabled()`
(`MacPlatformApplicationMisc.h:35`). So the **Dev column runs Retina-aware and the Shipping column
runs at 1× upscaled by the WindowServer**: risk 1 shows in Dev and hides in Shipping, and the
packaged HUD will look **softer** on a Retina panel at the same 1920×1080 RT and the same perf
numbers. Record it as a venue property so nobody reports it as a rendering regression.

---

## 5. What this pass cannot cover — recorded, not dropped

1. **Passport row 6's Win64 disk literal.** Another platform's row by definition. A Mac disk delta
   is a *second proxy*, not a substitute, and is only meaningful with `TargetArchitecture=Host`
   pinned. If measured at all, mark it as such. (And note: `VcHostNoVc.uproject` in the Linux tree
   has no `Plugins` array at all and `VaCuus.uplugin` is `EnabledByDefault: true` — as written that
   file does **not** disable the plugin. The A/B needs the explicit disabled stanza plus the three
   plugin-scoped config lines commented out.)
2. **Win64 IME re-check (bead akj.6.19)** — TSF-specific. A Mac IME pass is *additional* evidence,
   never a substitute. Keep the bead open; open a Mac sibling line for the Mac result.
3. **The quickjs `/experimental:c11atomics` question** — MSVC vs clang-cl only. Apple clang
   compiling the vendored C proves nothing about it; the `VaCuusJs.Build.cs:41-48` comment stands.
4. **Any headless-with-RHI row.** Impossible on Mac by construction. The Linux column's
   `-RenderOffscreen` venue cannot be reproduced — every visual row's venue note must say
   "windowed, real Metal RHI".
5. **SHIM-1 (5.6/5.7 matrix builds)** — only if those engines are installed. Note the recorded
   asymmetry: a Mac-host `BuildPlugin` **silently drops** the Win64/Linux legs
   (`BuildPluginCommand.Automation.cs:499-508`), so a green Mac run says nothing about the others.
   Conversely, the Mac `BuildPlugin -StrictIncludes` leg **can only be run here**. If there is
   appetite: `RunUAT.sh BuildPlugin -Plugin=<abs>/VaCuus.uplugin -Package=<abs-outside>
   -TargetPlatforms=Mac -StrictIncludes`, ~40 min, expect it to fail once and treat that as the
   point. Linux's first strict run died at 6,905/6,910 actions after 37 minutes on ten added
   includes; Mac's wave is a *header-set* difference (`Mac/MacPlatform*.h` instead of
   `Unix/UnixPlatform*.h`), not a repeat of that one, and should be small.
6. **`Proof.LiveReload.PIE`** — its path has no "VaCuus" substring, so `Automation RunTests VaCuus`
   does not select it. Name it explicitly if wanted; it needs a PIE-capable editor session.
7. **Live reload semantics beyond "it exists".** Our two `#if PLATFORM_LINUX` assertions in
   `VaCuusLiveReloadTest.cpp:280-282` (the inotify missing-directory pitfall) compile out on Mac —
   the test still runs and still prints its `AddInfo`, it just proves less. macOS is FSEvents
   (`DirectoryWatchRequestMac.cpp:66-81`, 0.2 s coalescing latency), so row 13's timing differs;
   nobody has read the Mac watcher's semantics against ours.

**Two staleness items to record rather than inherit.** The matrix is 15 rows written 2026-08-02;
HEAD has since added the **lobby demo** (`vacuus.LobbyDemo`, `.Backdrop`, `.Click`, `.Rects`,
`.Shot`, `.Stats`, the `-VaCuusLobbyDemo` Shipping flag, two automation tests) and the
**world-panel mip chain / off-switch** (`vacuus.WorldDemo.Mips`). Neither is in the command
column. Recommendation: run them as **rows 16/17 candidates on the macOS page**, marked
"not in the Linux column — Mac-first". That is honest; quietly extending the matrix or quietly
omitting them is not.

---

## 6. After the session

**Cells that get filled:**

| Where | What |
|---|---|
| `2026-08-vacuus-manual-matrix.md`, macOS Metal column | 15 rows, plus a "what was seen" paragraph per visual row in the same shape as the Linux section. Rows 16/17 appended as Mac-first candidates. |
| `2026-08-vacuus-perf-passport.md` §11 | macOS Metal Dev and cooked-Shipping-Mac halves of rows 1, 2, 2a, 2b, 3, 4, 7, 8; row 5's RAM delta **with the `phys_footprint` vs `VmRSS` note**; row 6 only if measured, marked as a second proxy. |
| Same file, "Owner-hardware handoff" | strike the macOS lines that closed; leave the Win64 ones. |
| `docs/buyer/owner-handoff.md` | §1 and §2 macOS items closed; §4/§5 Win64 items untouched. |
| `VaCuusJs.Build.cs:50-54` | the Mach-O export check beside the Linux one: `nm -gU Binaries/Mac/UnrealEditor-VaCuusJs.dylib \| grep ' _JS_'` must be empty (note the Mach-O leading underscore, and `.dylib` not `.so`). Confirms vendored patch #1 on macOS — a claim `VENDORED_TAG.txt` currently only half-supports. |

**Beads to file** (one per, with the log line or screenshot as evidence): the HiDPI layout scale if
risk 1 confirms; any IME defect, separated into *ours* vs the engine's candidate-window placement
by the differential test; the Metal shader-compile result; the glass route Mac takes; any
`MTL_DEBUG_LAYER` abort; the cook-hash comparison if it differs. Also a Mac sibling line on
`akj.6.19` for the Mac IME result — that bead's own leg is Win64 and stays open.

**And record every venue fact**, not just the pass/fail: which engine flavour, which feature level
bound (SM5 or SM6), which architecture packaged, which display the windowed rows ran on. This
document's whole convention is that a number without a method is not a filled cell.
