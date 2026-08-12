# VaCuus manual platform matrix — Linux Vulkan and Win64 D3D12 executed, macOS handoff

**What this is** (M6 spec §3.2, acceptance line 6): the enumerated interactive checklist,
executed once by hand per platform. The **Linux Vulkan** column was executed 2026-08-02 on
this machine (UE 5.8.1 source build, headless `-game -RenderOffscreen -ForceRes 1920×1080`,
plugin at branch `m6-productization`); every visual row's screenshot was read by a human eye
and what was seen is written next to it. The **Win64 D3D12** column was executed 2026-08-03
on the owner's desktop from its **physical console session**, at commit `6b82e4a`: **13 of 15
rows pass**, and the two that do not are named with what each still needs — row 5 wants a
human at the keyboard with an IME, and row 13 wants an interactive editor PIE with a mid-run
file edit. Neither is "the session died". **Row 13 has since been executed on the Linux
column** (2026-08-06, `-vacuusproof` plus a scripted edit, with a pixel pair and a new test
for its shadow leg — see the row's note); the same run is available on Win64 and needs
nothing that platform has to supply.

**The macOS Metal column below is STALE, and the cells lie by omission.** They still read
`owner hw`, but the macOS pass ran on 2026-08-03 and answered 12 of the 15 rows — the record is
`2026-08-vacuus-macos-results.md` §"the matrix", which was written as a separate document and
never merged back into this table. Read that document, not this column, until the merge lands
(bead `VaCuus-cob`). Two things to carry when you do: every macOS visual row is an **SM5** row
(M1 Pro is `GPUFamilyApple7`, so the SM6 clause in `MetalRHI.cpp:255-268` never binds), and rows
5, 13 and 15 are NOT RUN there for reasons that are named.

A row that could not be executed is marked with its reason — it is owed on every platform, not
silently dropped.

**Read the Win64 column against its machine, not against Linux's.** The two columns are
different hardware and the desktop is ~2.8× slower on this workload; the section "The machine,
stated once" at the bottom gives the ratio, the two controls that establish it, and the
experiment that refuted the easy alternative explanation.

**Session recipe** (Dev build; Shipping uses the launch flags instead of `-ExecCmds`):

```
UnrealEditor <proj>.uproject -game -RenderOffscreen -ForceRes -resx=1920 -resy=1080 \
  -ExecCmds="<row command>"        # -ExecCmds splits on COMMAS
# Screenshots land in Saved/Screenshots/<Platform>/; logs in Saved/Logs/VcHost.log.
# On Win64/macOS, drop -RenderOffscreen and run windowed: half these rows exist to be
# seen on a real desktop (IME especially).
```

| # | Row | Command / action | Assertion | Linux Vulkan 2026-08-02 | Win64 D3D12 2026-08-03 | macOS Metal |
|---|-----|------------------|-----------|--------------------------|-------------|-------------|
| 1 | Screen-space HUD composite | `vacuus.RefHud, vacuus.M1HUD.AutoShot 1200` | Full 1,732-node HUD renders: two 24-row boards, minimap blips, killfeed, plate, glow on ammo/objective | PASS (soak shot read; see below) | **PASS** (soak shot read; see below) | owner hw |
| 2 | Steady-state node count | `vacuus.RefHud.Count 12` | Logged count ∈ [1650, 1850] | PASS — 1732 | **PASS — 1732** | owner hw |
| 3 | Mouse hit-test + hover | `vacuus.M2Demo, vacuus.M2Demo.Rects 5, vacuus.M2Demo.Hit 120 115 6, vacuus.M1HUD.HoverShot 120 115` | Hit answers Handled inside the button, rect list non-empty, hover state visible in the shot | PASS (Handled + :hover seen) | **PASS** (6 rects, Handled, `:hover` seen) | owner hw |
| 4 | Keyboard text entry | `vacuus.M2Demo, vacuus.M1HUD.TypeShot 710 113 vacuus` | Typed text appears in the `<input>`; caret visible | PASS ("vacuus" in field) | **PASS** ("vacuus" in field) — and the IME line is the **`present`** branch, note C | owner hw |
| 5 | IME composition | interactive: focus the input, compose via platform IME (ibus/fcitx; Win64 bead akj.6.19) | Composition string + candidate window over the field | PARTIAL — note A | **NOT EXECUTED — needs a human at the keyboard.** Precondition now established, note C; bead akj.6.19 | owner hw |
| 6 | Gamepad spatial nav | `vacuus.M2Demo, vacuus.M1HUD.NavShot Gamepad_DPad_Down Gamepad_DPad_Right` | Focus ring lands on the expected control after the traversal | PASS (focus on BRAVO) | **PASS** (focus on BRAVO) | owner hw |
| 7 | World-space panel + raycast input | `vacuus.M5World 1, vacuus.M5World.InputSmoke` (then `vacuus.WorldDemo.Shot`) | InputSmoke "all N assertion(s) passed"; quad visible in shot | PASS — 16/16 assertions | **PASS — 16/16 assertions** | owner hw |
| 8 | Glass (backdrop blur) over scene | `vacuus.M5Glass, vacuus.M1HUD.PerfLog 1, vacuus.M1HUD.HoverShot` | Blurred scene through the panel; idle windows publish≈0 while Glass samples every frame | PASS (blur live, idle 100%) | **PASS** (blur live, idle 100%) — **and it takes the COPY path, not the direct SRV; note D** | owner hw |
| 9 | Gradient + builtin decorators | `vacuus.M5Deco, vacuus.M1HUD.AutoShot 10` | Linear/radial/conic fills + glass-panel builtin render as in the M5 gallery | PASS (all six cells) | **PASS** (all six cells) | owner hw |
| 10 | Material decorators (UMaterial in UI pass) | `vacuus.M5MatSpike, vacuus.M1HUD.AutoShot 10` | Spike cells draw their materials; refused cells named in log | PASS (five materials drawn) | **PASS** (five materials drawn) | owner hw |
| 11 | M5 acceptance demo (TSX + translation + glass + world quad) | `vacuus.M5Demo, vacuus.M1HUD.AutoShot 10, vacuus.M5Glass.Shot 8` | Both beats read as the M5 T9 proofs; zero `LogVaCuusJS: Error` | PASS (0 JS errors, beats match proofs) | **PASS** (0 JS errors, beats match proofs) | owner hw |
| 12 | **PF_FloatRGBA composite permutation** (spec §3.2) | set `r.DefaultBackBufferPixelFormat=3` in DefaultEngine.ini `[/Script/Engine.RendererSettings]`, run row 1; then revert and run row 1 again | Log line `VaCuus composite: elements texture is FloatRGBA -> LinearOutput` (GPixelFormats names carry no `PF_` prefix — engine Misc/PixelFormat.cpp:44); HUD colors match the default-format run by eye (no washed-out ~2.2× brightening) | PASS (LinearOutput line + A/B by eye) | **PASS** (LinearOutput line + A/B by eye) — the `-game` leg; the editor-PIE leg is still owed, note E | owner hw |
| 13 | Live reload (editor watcher) | interactive editor PIE: edit a loaded .rml/.rcss on disk, watch the view reload; runtime half: `vacuus.ReloadUI` | Document reloads without restart; watcher Warning if a bundle shadows the edit | **PASS** 2026-08-06 — PIE + scripted mid-run edit, pixel pair; shadow leg now a test — note B | **NOT EXECUTED** — the same run is available here (`-vacuusproof`); nothing about it needs this platform | owner hw |
| 14 | Demo-suite toggles + clean teardown | each row's session end (SIGTERM) | Teardown tail: UI thread stopped in-band, RmlUi shut down, zero unpublished NEW resources | PASS (all sessions) | **PASS (all 10 gracefully-closed sessions)** | owner hw |
| 15 | Shipping ignition flags | packaged Shipping: `-VaCuusRefHud` / `-VaCuusM5Demo` (+`-VaCuusPerfLog`, `-VaCuusMemProbe`) | Demo boots with no console; gate screenshot at t+8 s | PASS (bundle-mounted Shipping) | **PASS** (cooked + staged Shipping, gate shot at t+8 s) — note F | owner hw |
| 16 | Drag'n'drop (clone ghost, typed slots) | `vacuus.M1HUD.AutoShot 400, vacuus.DragDemo, vacuus.M2Demo.Drag 83 129 463 129 12 1.0` | Rifle leaves the stash and sits in the WEAPON slot; status line reads `MOVED IT-RIFLE -> EQ-WEAPON`; log line says the press was taken by THE UI with 12 of 12 moves handled (capture held across the inter-panel gap). Automation shadow: `VaCuus.Js.DragDrop` drives the same document through the input path, mouse and touch | PASS 2026-08-13 (shot read: rifle in WEAPON, status MOVED; "press was taken by THE UI ... 12 of 12 move(s) handled") | not executed — the command needs nothing this platform lacks | not executed |

**Note A (IME, Linux headless):** offscreen Linux has no `ITextInputMethodSystem`.

**Note B (live reload, headless):** the watcher lives in `VaCuusEditor` (editor-only module).

**Note C (IME, Win64 — the precondition, not the row).** The 2026-08-03 SSH pass logged the
`GetTextInputMethodSystem() returned null` branch and correctly warned that it was a **headless
artifact**, not a statement about Windows. On a real console session the other branch is taken, and
row 4's session logged both halves of it:

> `IME: platform ITextInputMethodSystem present; composition is available`
> `TypeShot: IME bridge built=yes, platform system absent=no, registered=yes, context active=yes`

That is the Windows TSF path reaching `registered` and `context active` for the first time. It is
the **precondition** for row 5, not row 5 itself: composing through a real IME and reading the
candidate window still needs a human at the keyboard with an IME installed. Bead `akj.6.19` stays
open on exactly that remainder, and it is now the only thing standing between it and closure.

**Note D (glass, Win64 — the SSH pass's headline was an offscreen artifact).** That pass reported
`ShaderResource=yes`, "direct-SRV path taken: a first on any platform". It is not what a real
Windows session does. Same machine, same commit, same command, only `-RenderOffscreen` differing:

| Venue | Log line |
|---|---|
| **windowed** (real D3D12 swapchain) | `glass samples the Slate output through a bounded copy pass (texture ShaderResource=no …)` |
| `-RenderOffscreen` | `glass samples the Slate output DIRECTLY as an SRV (texture ShaderResource=yes …)` |

A real swapchain back buffer is not created shader-resource-able, so **the shipped Win64 path is the
bounded copy pass**, and the "first on any platform" claim describes the offscreen render target the
SSH pass was forced to use. Corrected in `2026-08-vacuus-win64-results.md` §7/§8. The row still
PASSes — the blur is live and the idle economy holds — but through the other route.

**Note E (row 12, Win64):** the `-game` leg ran here (ini forced, permutation line read, A/B by eye,
ini reverted). Spec §3.2 also names an **editor-PIE** composite check; that leg rides with row 13,
which is the other interactive-editor row still owed on every platform.

**Note F (row 15, Win64 — and a trap in reading it).** `BuildCookRun -platform=Win64
-clientconfig=Shipping -build -cook -stage -pak`, then the staged
`TP_ThirdPerson.exe -VaCuusRefHud -VaCuusPerfLog -VaCuusMemProbe -windowed`. The gate screenshot at
t+8 s, read by eye: the full 1,732-node HUD from the cooked bundle with no console — both 24-row
boards populated, killfeed down the right edge, minimap blips and sweep, plate UFNA-01 HP 28.681 /
MP 55.425, compass, "HOLD THE LINE // WAVE 00", ammo **"23 / 120"** in the warm glow, three rows of
damage numbers, buff bar, settings panel. PASS. The scene *behind* the HUD is an unstreamed top-down
view rather than the Dev run's third-person camera — a host-project content artifact of this stock
template package, not a UI result, and the row asserts the UI.

**Two Win64-specific traps this row cost, worth knowing before repeating it:**

1. **A staged Shipping build does not write under the staged tree.** Its `Saved/` is
   `%LOCALAPPDATA%\<Project>\Saved` (`FPlatformProcess::UserSettingsDir`), so screenshots and logs
   are not where the Dev runs put them.
2. **A stock `Game` target writes no log at all in Shipping, `-log` or not.** `TP_ThirdPersonTarget`
   does not set `bUseLoggingInShipping`, so every `UE_LOG` is compiled out and the run is silent —
   the gate screenshot is the only evidence such a build can produce. Anything that needs to *read*
   a Shipping log on Win64 (the bundle-mount line, the PerfLog windows, the MemProbe samples) needs
   a target with `bUseLoggingInShipping = true`, which the Linux passport's host project evidently
   had. That is why the cooked-Shipping-Win64 perf column and the memory-mapped bundle observation
   are handled separately rather than harvested from this run.

## What was seen, row by row (Linux Vulkan)

**Row 1 — screen-space HUD.** Session: the R1 perf soak (100 s). Screenshot at 1,200 recorded
frames, read by eye: both 24-row boards populated (ALFA-RAPTOR-01 … BRVO-HAVOC-24) with distinct
K/D/A/score/ping columns and green ping meters; killfeed right edge full (orange killer names,
weapon labels, red HS pills on the serial-deterministic rows); minimap bottom-right with the
red/blue blip cloud + sweep + gold north marker; 18-slot buff bar with mid-sweep shades; player
plate "UFNA-01", HP 9.847 / MP 31.346 (mid-sweep values — the pipeline is live, not stuck);
compass with N centered; "HOLD THE LINE // WAVE 01"; ammo "20 / 120" in the warm glow; three
rows of damage numbers; crosshair + ability bar + settings panel + "VaCuus RefHud" overlay.
One cosmetic finding: the objective line overlaps the scoreboard headers at 1920×1080 (both
top-center); recorded, not a defect gate. PASS.

**Row 2 — steady-state count.** `vacuus.RefHud.Count 12` printed
`NodeCount: view 1 document 'RefHud/refhud.rml': 1732 nodes` — inside [1650, 1850], equal to the
published arithmetic and to the automation twin (`VaCuus.RefHud.Count`, same 1732 both phases). PASS.

**Row 3 — mouse hit-test + hover.** `vacuus.M2Demo.Rects 5` listed 6 interactive rects (3 buttons,
the wheel list, its scrollbar, and the TextInput field (511,95)-(909,132)); the `vacuus-passthrough`
panel was correctly ABSENT from the list. `vacuus.M2Demo.Hit 120 115 6` answered
`covered=yes focusable=yes … a press there would be answered Handled`. The HoverShot screenshot,
read by eye: the pointer parked inside ALPHA and ALPHA renders in the amber `:hover` state while
BRAVO/CHARLIE stay dark. PASS.

**Row 4 — keyboard text entry.** `vacuus.M1HUD.TypeShot 710 113 vacuus`: the screenshot shows
"vacuus" sitting in the text field with the cyan focus ring on it. The IME status line printed:
`IME bridge built=yes, platform system absent=yes, registered=no, context active=no — so the text
went through OnKeyChar -> ProcessTextInput`. PASS (typing); the IME half is row 5.

**Row 5 — IME.** PARTIAL by venue: headless Linux has no `ITextInputMethodSystem`, so composition
cannot be exercised offscreen — the TypeShot line above is the honest record of exactly that
(bridge built, platform system absent). The M2 milestone verified ibus composition interactively
on this machine; the packaged/matrix re-check with a live IME is an interactive-desktop task and
the Win64 leg is bead akj.6.19. NOT EXECUTABLE HEADLESS — reason recorded, not dropped.

**Row 6 — gamepad spatial nav.** `vacuus.M1HUD.NavShot Gamepad_DPad_Down Gamepad_DPad_Right`
(`navigation config overridden: yes`): the screenshot shows the magenta focus ring on **BRAVO** —
Down enters the grid at ALPHA, Right lands on BRAVO, exactly the expected traversal. PASS.

**Row 7 — world-space + raycast.** `vacuus.M5World 1, vacuus.M5World.InputSmoke 5`:
**all 16 assertions passed** — snapshot carries exactly the button's rect; raycast click fired
OnModelWrite once; consumed click never reached game input; occlusion (a Slate overlay wins,
processor defers, nothing consumed); pass-through (game heard exactly one press); hover raised
mouseover; MouseLeave cleared RmlUi's hover chain. Measured: occlusion query 0.47 µs avg
(max 2.79), WS-STALE-RAY re-trace 2.91 µs avg (max 34.43), 500 samples each. The screenshot shows
the quad in-scene running the JS demo with **Ammo 29** — the raycast click's routed write
(30 − 1) visible in pixels. PASS.

**Row 8 — glass.** `vacuus.M5Glass` + PerfLog + HoverShot: the screenshot shows the ROUNDED
blur panel (mountains smeared behind it, soft corners), the SQUARE blur panel (hard corners,
same blur), and the CONTROL panel with the same fill and NO blur — the terrain reads sharp
through it. PerfLog: published=0 / 3,363 recorded (100.0% idle) while Glass sampled every
engine frame at 0.011 ms avg — the composite-time glass economy in one log line. PASS.

**Row 9 — decorators.** `vacuus.M5Deco`: all six cells correct by eye — linear 90° red→blue,
repeating 45° gold/black hazard stripes, radial white-core→deep-blue rim, conic full hue wheel,
builtin `shader(glass-panel)` translucent fill + border glow, control plain fill. PASS.

**Row 10 — material decorators.** `vacuus.M5MatSpike`: TRANSLUCENT (brown, text over),
ADDITIVE (cyan), OPAQUE (replaces the cell box), MID base (textured, game-side params), and
TIME-ANIMATED (gradient bars mid-motion) all drew their materials; the control cell stayed a
plain fill. PASS.

**Row 11 — M5 acceptance demo.** Zero `LogVaCuusJS: Error`; `translation: published table v1
(2 entries)`; `model 'hud' bound` on both views. Beat-2 screenshot (t+8) read identically to the
M5 T9 proof `m5demo_beat2.png`: TSX HUD with Health **59** (the model-fed sweep), five translated
killfeed rows ("Rasp » Vex" — the arrow table), glass panel blurring the clouds, and the world
quad to the right running the same document. PASS.

**Row 12 — PF_FloatRGBA composite permutation.** With `r.DefaultBackBufferPixelFormat=3` the
session logged `VaCuus composite: elements texture is FloatRGBA -> LinearOutput (sRGB->linear
decode, gamma 1.0 target) permutation`; the control run (default format) logged
`A2B10G10R10 -> pass-through`. The two RefHud screenshots compared by eye: opaque surfaces
identical (dark panel blues, gold ammo glow, red HP bar, white text) — none of the ~2.2×
global brightening a missed decode produces (the GPU test's numeric twin: 0.502 raw vs 0.216
decoded). One expected, correct difference: semi-transparent rows over the very bright sun
region read slightly lighter on the float target — linear-space alpha blending vs gamma-space
blending, the same property the engine's own translucent Slate has on linear targets. PASS.
Numeric backstop: `VaCuus.Render.Composite.LinearOutputGPU` (real-RHI readback, both
permutations, seen to fail with the decode removed and pass restored). Ini reverted after
the session. Venue substitution, recorded per spec §2(f): spec §3.2 / plan 5.1 name an
editor-PIE composite check; what ran here is the ini-forced FloatRGBA-backbuffer `-game`
session plus the real-RHI readback test — mechanism-identical (the permutation keys off the
actual target format, not the viewport kind) — and the interactive editor-PIE leg rides the
owner-hardware pass of this row.

**Row 13 — live reload. EXECUTED 2026-08-06 on Linux Vulkan, and the premise that it could not
be was wrong.** This row said "not expressible in a frame-0 `-ExecCmds` headless run" for
three days. What is true is narrower: a frame-0 command line cannot BY ITSELF edit a file
half a minute later. The plugin already ships the harness that closes that gap —
`Proof.LiveReload.PIE`, whose own header says it needs "a HUMAN **OR A SCRIPT** to edit a file
from outside this process" — so the row needed a second process, not a person.

What was run, and what it produced:

```bash
UnrealEditor VcHost.uproject -RenderOffscreen -ForceRes -resx=1920 -resy=1080 -nosplash \
  -vacuusproof -testexit="Automation Test Queue Empty" \
  -ExecCmds="Automation RunTests Proof.LiveReload.PIE,"
# ... a shell loop watches the log for VACUUS_PROOF_EDIT_NOW, then edits
# Plugins/VaCuus/Content/DevUI/m1_hud.rcss: #hp-fill #FF0000 -> #00FFFF
```

  - `Live reload flushed 1 changed path(s) after 190 ms and reloaded 1 view(s): …/m1_hud.rcss`
    — a real inotify event, the debounce, the reload, in a live PIE session;
  - `Proof.LiveReload.PIE` **Success** (it asserts both screenshots exist, so a harness that
    started PIE and did nothing else would go red);
  - the pixel pair in `Saved/VaCuusProof/`: exactly **1,080 pixels in a 90×12 block at
    (128..217, 189..200)** are `(255,0,0)` in the before shot and `(0,255,255)` in the after
    one. That block is `#hp-fill`. The rest of the frame differs too, because the 3D scene
    behind the HUD keeps moving — which is why the assertion is on the red→cyan transition
    and not on a whole-frame diff.

**Steps 4-5 — the bundle shadow warning — are now a test rather than a manual step.**
`VaCuus.LiveReload.BundleShadow` mounts a transient bundle containing the changed path and
asserts ONE Warning names the shadow, the file, the bundle and the remedy, with a control run
(nothing mounted → no such line). Watched to fail with the warning deleted. It needs no
watcher and no view: the shadow check is a string test between a watched root and the mount
table, so nothing about it wanted a human.

**What genuinely still wants a person, and it is now a small claim:** somebody looking at a
real windowed editor rather than at two PNGs. Every mechanical link in this row — event,
debounce, reload dispatch, repaint, shadow warning — is asserted by a test or by the pixel
pair above.

Standing evidence unchanged: the M2 T10 proof session
(`docs/research/proofs/m2-t10-live-reload/`), the automation reload suite
(`VaCuusReloadTest.cpp`), `VaCuus.LiveReload.WatcherEvent` for the real-inotify link, and the
runtime half (`vacuus.ReloadUI`).

**Row 14 — teardown.** Every session above ended by SIGTERM with the same tail: UI thread stopped
by an in-band shutdown command, VFS teardown serving totals printed, RmlUi shut down, recorder
destroyed with **zero unpublished NEW resources** (only released-side traffic). PASS.

**Row 15 — Shipping ignition flags.** Packaged Shipping (bundle-mounted): `-VaCuusRefHud
-VaCuusPerfLog` booted the reference HUD from the cooked bundle with no console —
`Mounted bundle '/VaCuus/Bundles/DevUIBundle.DevUIBundle': 24 entries, 461881 bytes, resident
buffer`, `vacuus.M1HUD.PerfLog=1` set by flag, gate screenshot at t+8 s. The screenshot, read by
eye: the full 1,732-node HUD identical in composition to the Dev run — boards populated with the
stat beats visibly landed (ALFA-HAVOC-05 at K 29 = seed 28 + one bump), killfeed full, blips
live, ammo "14 / 120" in the glow, damage numbers, HP 60.056 mid-sweep. `-VaCuusM5Demo` and
`-VaCuusMemProbe` exercised in their own sessions (see the passport). PASS.

## What was seen, row by row (Win64 D3D12)

**Venue.** 2026-08-03 evening, on the **physical console session** of the owner's desktop
(`SESSIONNAME=Console`, state Active) — which is the whole reason this column exists, because the
morning's SSH pass could not open a window at all (bead `VaCuus-5fg`). Same machine and engine as
that pass: RTX 2080 SUPER, driver 591.86, UE 5.8.1 Installed CL 56057345, `SM6` bound on real
hardware. Commit under test `6b82e4a`, i.e. **with** the PSO fix `b4f12e1` that the morning pass
died without. Sessions ran `-game -windowed -resx=1920 -resy=1080 -ForceRes`, closed by
`CloseMainWindow()` so a teardown tail exists to read, driven by `run-row.ps1` (kept with the run
artifacts). **Zero `Fatal error`, zero `graphics pipeline` failures, zero `D3D12 ERROR` across all
sessions** — the fatal that voided 13 of 15 cells in the morning is gone.

**Row 1 — screen-space HUD.** 100 s soak, screenshot at 1,200 recorded frames, read by eye: both
24-row boards populated (ALFA-RAPTOR-01 … BRVO-HAVOC-24) with distinct K/D/A/score/ping columns and
green ping meters; killfeed down the right edge (orange killer names, weapon labels, red HS pills);
minimap bottom-right with the red/blue blip cloud, sweep arc and gold north marker; 18-slot buff bar
with mid-sweep shades; player plate "UFNA-01", HP 61.107 / MP 74.308 (mid-sweep — the pipeline is
live, not stuck); compass with N centred; "HOLD THE LINE // WAVE 03"; ammo "25 / 90"; three rows of
damage numbers; crosshair, ability bar, settings panel and the "VaCuus RefHud" overlay.
The glow assertion was checked at 3× magnification rather than by squinting at 1080p: the amber
halo around the ammo glyphs is plainly there, so `font-effect: glow` composites correctly on D3D12.
**The same cosmetic finding as Linux reproduces**: the objective line overlaps the scoreboard
headers at 1920×1080. Recorded, not a defect gate — and now known to be platform-independent. PASS.

**Row 2 — steady-state count.** `NodeCount: view 1 document 'RefHud/refhud.rml': 1732 nodes` —
inside [1650, 1850] and **exactly the Linux figure and the published arithmetic**. PASS.

**Row 3 — mouse hit-test + hover.** `vacuus.M2Demo.Rects 5` listed 6 interactive rects — 3 buttons,
the wheel list, its scrollbar, and the TextInput field at (511,95)-(909,132), all identical to
Linux; the `vacuus-passthrough` panel was correctly ABSENT. `vacuus.M2Demo.Hit 120 115 6` answered
`covered=yes focusable=yes textInput=no -- so a press there would be answered Handled`. The
HoverShot, read by eye: ALPHA renders in the amber `:hover` state while BRAVO and CHARLIE stay
dark. PASS.

**Row 4 — keyboard text entry.** `vacuus.M1HUD.TypeShot 710 113 vacuus`: the screenshot shows
"vacuus" in the field with the cyan focus ring. `6 UTF-16 unit(s) forwarded to the focused widget`.
The IME status line is the `present` branch — see note C, which is the part that matters. PASS
(typing); the IME half is row 5 and is still owed.

**Row 6 — gamepad spatial nav.** The screenshot shows the magenta focus ring on **BRAVO** — Down
enters the grid at ALPHA, Right lands on BRAVO, the same traversal Linux produced. PASS.

**Row 7 — world-space + raycast.** `vacuus.M5World.InputSmoke 5`: **all 16 assertions passed** —
snapshot carries exactly the button's rect; raycast click fired OnModelWrite once; consumed click
never reached game input; occlusion (Slate overlay wins, processor defers, nothing consumed);
pass-through (game heard exactly one press); hover raised mouseover; MouseLeave cleared RmlUi's
hover chain. Processor counters `consumed=4 deferred-to-Slate=4 passed-to-game=3 leaves=2`.
Measured on this hardware: occlusion query **2.18 µs avg** (max 57.20), WS-STALE-RAY re-trace
**5.85 µs avg** (max 213.50), 500 samples each — same shape as Linux (0.47 / 2.91 µs), larger by
about the machine ratio below. This row was the morning pass's `FAIL, venue`; it is now a pass.

**Row 8 — glass.** The screenshot: the ROUNDED blur panel (scene smeared behind it, soft corners),
the SQUARE blur panel (hard corners, same blur), and the CONTROL panel with the same fill and NO
blur — wall and terrain read sharp through it. PerfLog: **0 published / 1,075 recorded (100.0%
idle)** while Glass sampled every engine frame at 0.057 ms avg — the composite-time glass economy,
reproduced on D3D12. The route it takes is **not** the one the morning pass reported: see note D.
PASS.

**Row 9 — decorators.** All six cells correct by eye: linear 90° red→blue, repeating 45° gold/black
hazard stripes, radial white-core→deep-blue rim, conic full hue wheel, builtin `shader(glass-panel)`
translucent fill + border glow, control plain fill. PASS.

**Row 10 — material decorators.** TRANSLUCENT (brown, text over), ADDITIVE (cyan), OPAQUE (green,
replaces the cell box), MID base (textured, game-side params) and TIME-ANIMATED (gradient bars
mid-motion) all drew their materials; the control cell stayed a plain fill. PASS.

**Row 11 — M5 acceptance demo.** **Zero `LogVaCuusJS: Error`**; `translation: published table v1
(2 entries)`; `model 'hud' bound` on both views. Beat-2 screenshot reads as the M5 T9 proof: TSX HUD
with Health **58** (the model-fed sweep), five translated killfeed rows ("Rasp » Vex" — the arrow
table), and the world quad to the right running the same document. PASS.

**Row 12 — PF_FloatRGBA composite permutation.** With `r.DefaultBackBufferPixelFormat=3` the session
logged `elements texture is FloatRGBA -> LinearOutput (sRGB->linear decode, gamma 1.0 target)
permutation`; the control runs logged `A2B10G10R10 -> pass-through (display-gamma target)`. The two
RefHud screenshots compared by eye: opaque surfaces identical — dark panel blues, the amber ammo
glow, red HP bar, white text — with **none of the ~2.2× global brightening a missed decode
produces**. The one expected difference is present and is the same one Linux recorded: semi-
transparent panels over the bright sky read lighter on the float target (linear-space alpha blending
vs gamma-space), most visible on the killfeed rows and scoreboard against the sky. The ini was
reverted immediately after the run and re-checked absent. PASS for the `-game` leg; note E.

**Row 14 — teardown.** Every one of the ten gracefully-closed sessions ended with the same tail:
UI thread stopped by an in-band shutdown command, VFS teardown serving totals printed, RmlUi shut
down, recorder destroyed with **zero unpublished NEW resources** (only released-side traffic), exit
code 0. This is the row the morning pass could not fill at all, because every session there ended in
`appError` rather than a clean exit. PASS.

### The machine, stated once so the numbers can be compared honestly

This column's hardware is **not** the Linux column's hardware, and the difference is large enough
that it has to be named. On the same 1,732-node RefHud workload at the same **908.3 draws/frame**,
Linux ran at 227 fps and this desktop runs at **80.8 fps** — a 2.81× ratio. Every CPU-side per-frame
figure in the perf passport's Win64 column is larger than the Linux one by close to that same
factor. Two controls keep this from being guesswork:

- **VaCuus is not what limits this machine.** Compared like with like, both windowed: the static M1
  HUD — publishing nothing, UI thread effectively free — runs at **80.5 fps**, and the full
  1,732-node reference HUD at **76.1 fps**. The whole reference workload costs about **5%** of frame
  rate; the ~80 fps ceiling belongs to the venue, not to the UI plugin.
- **It is not a frame-rate artifact either** (`Exp-FPS-LAW`, below and in the passport). The obvious
  explanation — that a time-driven HUD does more work per frame when frames are longer — was tested
  and **refuted**: capping to 30 fps left Update+Record at 2.93 ms against 3.00 ms at 80 fps. The
  per-frame cost is frame-rate independent, so the Win64/Linux per-frame gap is real work, and it
  tracks the machine.
