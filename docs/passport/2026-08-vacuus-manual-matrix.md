# VaCuus manual platform matrix — Linux Vulkan executed, Win64/macOS handoff

**What this is** (M6 spec §3.2, acceptance line 6): the enumerated interactive checklist,
executed once by hand per platform. The **Linux Vulkan** column was executed 2026-08-02 on
this machine (UE 5.8.1 source build, headless `-game -RenderOffscreen -ForceRes 1920×1080`,
plugin at branch `m6-productization`); every visual row's screenshot was read by a human eye
and what was seen is written next to it. The **Win64 D3D12** and **macOS Metal** columns are
the owner-hardware handoff: run the SAME commands, read the SAME assertions, fill the cells.
A row that could not be executed headless is marked with its reason — it is owed on every
platform, not silently dropped.

**Session recipe** (Dev build; Shipping uses the launch flags instead of `-ExecCmds`):

```
UnrealEditor <proj>.uproject -game -RenderOffscreen -ForceRes -resx=1920 -resy=1080 \
  -ExecCmds="<row command>"        # -ExecCmds splits on COMMAS
# Screenshots land in Saved/Screenshots/<Platform>/; logs in Saved/Logs/VcHost.log.
# On Win64/macOS, drop -RenderOffscreen and run windowed: half these rows exist to be
# seen on a real desktop (IME especially).
```

| # | Row | Command / action | Assertion | Linux Vulkan 2026-08-02 | Win64 D3D12 | macOS Metal |
|---|-----|------------------|-----------|--------------------------|-------------|-------------|
| 1 | Screen-space HUD composite | `vacuus.RefHud, vacuus.M1HUD.AutoShot 1200` | Full 1,732-node HUD renders: two 24-row boards, minimap blips, killfeed, plate, glow on ammo/objective | PASS (soak shot read; see below) | owner hw | owner hw |
| 2 | Steady-state node count | `vacuus.RefHud.Count 12` | Logged count ∈ [1650, 1850] | PASS — 1732 | owner hw | owner hw |
| 3 | Mouse hit-test + hover | `vacuus.M2Demo, vacuus.M2Demo.Rects 5, vacuus.M2Demo.Hit 120 115 6, vacuus.M1HUD.HoverShot 120 115` | Hit answers Handled inside the button, rect list non-empty, hover state visible in the shot | PASS (Handled + :hover seen) | owner hw | owner hw |
| 4 | Keyboard text entry | `vacuus.M2Demo, vacuus.M1HUD.TypeShot 710 113 vacuus` | Typed text appears in the `<input>`; caret visible | PASS ("vacuus" in field) | owner hw | owner hw |
| 5 | IME composition | interactive: focus the input, compose via platform IME (ibus/fcitx; Win64 bead akj.6.19) | Composition string + candidate window over the field | PARTIAL — note A | owner hw (akj.6.19) | owner hw |
| 6 | Gamepad spatial nav | `vacuus.M2Demo, vacuus.M1HUD.NavShot Gamepad_DPad_Down Gamepad_DPad_Right` | Focus ring lands on the expected control after the traversal | PASS (focus on BRAVO) | owner hw | owner hw |
| 7 | World-space panel + raycast input | `vacuus.M5World 1, vacuus.M5World.InputSmoke` (then `vacuus.WorldDemo.Shot`) | InputSmoke "all N assertion(s) passed"; quad visible in shot | PASS — 16/16 assertions | owner hw | owner hw |
| 8 | Glass (backdrop blur) over scene | `vacuus.M5Glass, vacuus.M1HUD.PerfLog 1, vacuus.M1HUD.HoverShot` | Blurred scene through the panel; idle windows publish≈0 while Glass samples every frame | PASS (blur live, idle 100%) | owner hw | owner hw |
| 9 | Gradient + builtin decorators | `vacuus.M5Deco, vacuus.M1HUD.AutoShot 10` | Linear/radial/conic fills + glass-panel builtin render as in the M5 gallery | PASS (all six cells) | owner hw | owner hw |
| 10 | Material decorators (UMaterial in UI pass) | `vacuus.M5MatSpike, vacuus.M1HUD.AutoShot 10` | Spike cells draw their materials; refused cells named in log | PASS (five materials drawn) | owner hw | owner hw |
| 11 | M5 acceptance demo (TSX + translation + glass + world quad) | `vacuus.M5Demo, vacuus.M1HUD.AutoShot 10, vacuus.M5Glass.Shot 8` | Both beats read as the M5 T9 proofs; zero `LogVaCuusJS: Error` | PASS (0 JS errors, beats match proofs) | owner hw | owner hw |
| 12 | **PF_FloatRGBA composite permutation** (spec §3.2) | set `r.DefaultBackBufferPixelFormat=3` in DefaultEngine.ini `[/Script/Engine.RendererSettings]`, run row 1; then revert and run row 1 again | Log line `VaCuus composite: elements texture is FloatRGBA -> LinearOutput` (GPixelFormats names carry no `PF_` prefix — engine Misc/PixelFormat.cpp:44); HUD colors match the default-format run by eye (no washed-out ~2.2× brightening) | PASS (LinearOutput line + A/B by eye) | owner hw | owner hw |
| 13 | Live reload (editor watcher) | interactive editor PIE: edit a loaded .rml/.rcss on disk, watch the view reload; runtime half: `vacuus.ReloadUI` | Document reloads without restart; watcher Warning if a bundle shadows the edit | PARTIAL — note B | owner hw | owner hw |
| 14 | Demo-suite toggles + clean teardown | each row's session end (SIGTERM) | Teardown tail: UI thread stopped in-band, RmlUi shut down, zero unpublished NEW resources | PASS (all sessions) | owner hw | owner hw |
| 15 | Shipping ignition flags | packaged Shipping: `-VaCuusRefHud` / `-VaCuusM5Demo` (+`-VaCuusPerfLog`, `-VaCuusMemProbe`) | Demo boots with no console; gate screenshot at t+8 s | PASS (bundle-mounted Shipping) | owner hw | owner hw |

**Note A (IME, Linux headless):** offscreen Linux has no `ITextInputMethodSystem`.

**Note B (live reload, headless):** the watcher lives in `VaCuusEditor` (editor-only module).

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

**Row 13 — live reload.** PARTIAL by venue: the file watcher lives in `VaCuusEditor`
(editor-only module) and needs an interactive editor session plus a mid-run file edit —
not expressible in a frame-0 `-ExecCmds` headless run. Standing evidence: the M2 T10 proof
session (`docs/research/proofs/m2-t10-live-reload/`) and the automation reload suite
(`VaCuusReloadTest.cpp`), plus the runtime half (`vacuus.ReloadUI`, moved into the runtime
module by the M6 sweep). The interactive editor pass is owed on each platform page with the
same steps: PIE with a loaded document → edit the .rcss on disk → watch the view repaint →
mount a bundle → edit again → assert the shadowing Warning names the path. NOT EXECUTABLE
HEADLESS — reason recorded.

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
