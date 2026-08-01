# M5 Task 6 — world-space: WS-GAMMA and WS-COPY-COST evidence

All captures: headless `-game -RenderOffscreen`, 1920x1080, Vulkan SM6.
`vacuus.WorldDemo` places a `UVaCuusWorldComponent` quad (document `DevUI/m1_hud.rml`)
in front of the camera; `vacuus.M1HUD` composites the same document screen-space as
the parity reference. The preset under test is `/VaCuus/M_VaCuusWorldPanel`
(BLEND_AlphaComposite, unlit, responsive AA, two-sided; authored by
`author_world_panel_material.py` in this directory).

## WS-GAMMA — decode-or-not, settled by screenshot parity

The question (worldspace-cli.md section 4): the view RT holds display-encoded
pixels (the screen composite writes them post-tonemap with no conversion,
VaCuusSlateElement.cpp:186-190; the RT carries no TexCreate_SRGB,
VaCuusReplayRenderer.cpp:170-178), but a translucent world material's emissive
enters PRE-tonemap linear scene color. The preset therefore carries a runtime A/B
knob: `VaCuusDecodeSRGB` lerps raw RGB against pow(RGB, 2.2) before Emissive.

- `ws_gamma_decode_on.png` — `VaCuusDecodeSRGB 1`: the panel next to the
  screen-space HUD.
- `ws_gamma_decode_off.png` — `VaCuusDecodeSRGB 0`: same framing, same beat.

**Decision: DECODE (`VaCuusDecodeSRGB` default = 1), by eyeball parity.** With the
decode, the world panel's colors track the screen composite: the player plate's
dark navy, the saturated hotbar cyan/green/amber/pink, the scoreboard's dark rows.
Without it, the panel is uniformly washed out — pale gray-blue plates, pastel
hotbar — the classic double-encode (display-encoded pixels tonemapped as if
linear). The authored asset already defaults to 1; no re-author needed. Honest
caveats, carried on the preset: the decode is pow(2.2) (approximate, not piecewise
sRGB), applied to premultiplied RGB while alpha stays linear (imperfect on
partially transparent pixels), and the tonemapper is not the identity, so parity
is close, not pixel-exact — the panel legitimately participates in scene exposure.

One dev-loop trap this experiment ate: the FIRST A/B run photographed an invisible
panel — the preset's shaders were still async-compiling on their first-ever
in-game use (cold DDC), and a translucent mesh simply skips drawing until its
material is ready. Screenshot runs on a cold DDC must wait out (or precede with) a
warm-up run.

## WS-COPY-COST — the copy per published frame at 1024x1024

Method: `vacuus.WorldDemo 1024 1024`, `vacuus.M1HUD.PerfLog 1`; the PerfLog's
`WorldCopy (RT)` line is the observable — its per-window sample COUNT is the copy
count (one sample per issued copy, FVaCuusWorldSink::CopyToDestination), its
avg/p99 the per-copy render-thread cost. Cross-checked against
`vacuus.WorldDemo.Stats` (the sink's cumulative arrival/copy/skip counters).

- **Idle case** (`m1_hud.rml`, static, 900x450 panel + screen HUD, ~38 s):
  `WorldCopy [win] ... (0)` in every 5-second window; sink totals **1 buffer
  arrived, 1 copy, 0 skips for the whole session** — the panel cost exactly its
  initial publish and nothing after (`published=0 skipped=2262 (100.0% idle)` per
  window). The exact-zero half is also asserted headlessly in
  `VaCuus.World.ComponentLifecycle` (20 idle UI frames, arrival and copy counters
  byte-equal before/after).
- **Animated case** (`m4_demo.rml` — rAF-driven JS HUD — on a 1024x1024 panel,
  ~38 s): **7601 buffers arrived, 7600 copied, 0 extent skips** (~200 copies/s ==
  the UI thread's publish rate for a per-frame-animating document). Per copy at
  1024x1024: **avg 0.001 ms, p50 0.001, p99 0.017, max 0.064 ms** render-thread
  (7616 samples) — the ≤0.05 ms budget row holds with ~50x headroom at avg, and
  even max is within 1.3x. The GPU-side cost of a 4 MB B8G8R8A8 copy is bandwidth
  noise at these rates. Verdict: the `FRHITextureReference` repoint stays the
  unpromoted v1.x escape, exactly as spec 2(g) reserves it — WS-COPY-COST found
  no evidence to promote it on.
- **Resize race** (the real-RHI `VaCuus.World.ResizeRace` run): churn totals
  **7 arrivals, 7 copies, 6 extent skips** — every publish that lost the race to
  its slot update SKIPPED (never a stale-pointer copy), and every skip was healed
  by the slot update's own repaint from the persistent OutputRT, which is why
  copies still equal arrivals.

The material-decorator caveat is documented on the component
(VaCuusWorldComponent.h): a live material decorator forces republish clamped to
engine rate (M5 Task 5b), so such a panel pays one copy per engine frame. The m4
soak above is the harsher case — UI-thread-rate publishes — and still lands at
~0.2 ms/s of render-thread copy time.
