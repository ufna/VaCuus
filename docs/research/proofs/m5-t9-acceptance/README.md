# M5 Task 9 — the acceptance demo, photographed

`vacuus.M5Demo` (spec §8), headless `-game -RenderOffscreen -ForceRes 1920×1080` Vulkan:

```
-ExecCmds="vacuus.M5Demo, vacuus.M1HUD.AutoShot 10, vacuus.M5Glass.Shot 8, vacuus.M1HUD.PerfLog 1"
```

- **m5demo_beat1.png** (AutoShot, ~t+0.5 s): the TSX HUD top-left — title over the
  linear-gradient strip, `Health 97` (the model-fed sweep, not the JS fallback), the
  gradient bar, the killfeed panel blurring the clouds behind it
  (`backdrop-filter: blur(12px)` + `shader(glass-panel)`), rows "Vex » Kilo" /
  "Moth » Rasp" — the TRANSLATION TABLE's arrow substituted around user-data params,
  where the identity string says "downed".
- **m5demo_beat2.png** (~t+8 s): health swept to 59, five translated rows, the camera
  heading visibly changed (different backdrop through the same glass — composite-time
  glass, spec §2(a)), and the world quad 16° right running the SAME document
  (`UVaCuusWorldComponent`, model-fed bar, Simulate button raycast-clickable).

The same session's PerfLog windows fill the spec §6 measured column (Glass avg 0.011 ms
/engine frame with two blur panels; the `vacuus.M5Glass` idle companion run:
published=0, 100.0% idle, Glass still sampling every frame). SIGTERM teardown of this
session: quad down (2445 arrivals = 2445 copies, 0 skips), HUD down, input mode
restored, UI thread stopped in-band, RmlUi shut down, zero unpublished NEW resources.

## The packaged gates (plan 9.3, user directive 2026-08-01)

**Development** (`BuildCookRun -platform=Linux -clientconfig=Development -build -cook
-stage -pak`, `VaCuus.Build.cs` touched first — the stale-receipt trap): UFS manifest
carries all 4 `M5Hud/*` files, 24 DevUI entries, `M_VaCuusWorldPanel` + 6 Spike
material `.uasset`s, the LatoLatin font. The staged binary ran the demo headless from
cooked paks: bundle resolved and booted (0 `LogVaCuusJS: Error`), `model 'hud' bound`
on both views (lowercase — akj.23), `translation: published table v1 (2 entries)`,
world quad up, Glass sampling every engine frame (avg 0.011 ms), SIGTERM → the same
clean teardown tail as the editor run. **m5demo_staged_dev_beat2.png** reads
identically to the editor beat 2.

**Shipping** (same, `-clientconfig=Shipping`), findings all recorded:
1. **`-ExecCmds` is COMPILED OUT of Shipping** — UnrealEngine.cpp:2543 wraps the
   :2552 `QueueDeferredCommands` in `#if !(UE_BUILD_SHIPPING) || ENABLE_PGO_PROFILE`
   — so the first Shipping run booted to an empty scene: no demo, no way to start
   one. Fix shipped in-milestone: the `-VaCuusM5Demo` launch flag (VaCuusRender.cpp,
   the Shipping-ignition comment), parsed by the plugin itself on the first map load.
2. **Stock Shipping compiles logging out entirely** — the HOST opted in
   (`VcHost.Target.cs`: `bUseLoggingInShipping = true` + `TargetBuildEnvironment.
   Unique`), or the gate would have been an exit code and nothing else.
3. **With logging on, Log-verbosity lines still print in Shipping** (the whole VaCuus
   teardown tail is Log) — the "Verbose/Log stripped" expectation was half right:
   Verbose stays silent (default runtime floor), Log survives. The one Display boot
   line (`VaCuus M5 acceptance demo: …`) remains the right guarantee for hosts that
   filter Log at runtime. The dev overlay is compile-gated out
   (VaCuusJsScriptHost.cpp `#if !UE_BUILD_SHIPPING` at :30/:44/:896-1055).
4. **Shipping's Saved tree moves to the user dir** — `~/.config/Epic/VcHost/Saved/`
   holds the log and screenshots, not the staged tree.
5. **watchdog=50 ms confirmed live**: `JS runtime created: cap=16 MB, stack=256 KB,
   watchdog=50 ms` (the Shipping default constant, VaCuusJsRuntime.cpp:26-30).
6. **The gate itself: PASS.** The demo ran all tracks from cooked paks, 0 JS errors,
   the one warning is the known Linux no-ITextInputMethodSystem line,
   **m5demo_staged_shipping.png** shows the full HUD + translated rows + glass +
   world quad, and SIGTERM produced the identical clean teardown (UI thread in-band
   stop, RmlUi shut down, zero unpublished NEW resources). No crash — no P1 beads.
