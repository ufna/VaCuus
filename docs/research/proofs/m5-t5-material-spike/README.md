# M5 Task 5 — the material spike's screenshot evidence

All captures: headless `-game -RenderOffscreen`, 1920×1080, Vulkan SM6, document
`DevUI/m5_matspike.rml` (`vacuus.M5MatSpike`), materials `/VaCuus/Spike/*` registered via
`vacuus.MatSpike.Add/.MID` with `vacuus.MaterialDecorators 1`. RMSE = ImageMagick
`compare -metric RMSE`, normalized. The decision these support is recorded in the M5 spec
§3.3 stage-2 outcome note.

## Run A — blend matrix + MID + animation, forced republish ON (editor `-game`)

- `runA_blend_matrix_beat1.png` — the day-2 (a) matrix at t=12 s: Translucent (orange,
  text legible through it), Additive (blue glow, dest text survives at full strength),
  Opaque (green slab, text replaced), and the MID cell — all four MD_UI materials drawn
  by the replay pass over RmlUi text, premultiplied compositing holding with the RT's
  single One/InvSrcAlpha blend state (mode mapping is in `VaCuusMaterial.usf`).
- `runA_mid_beat1.png` / `runA_mid_beat2.png` — t=12 s vs t=15.5 s with `vacuus.MatSpike.MID`
  driving `SpikeScalar` + `SpikeTex` every game frame: region RMSE **11.5%**
  (control cell in the same pair: **0.13%**). The proxy picks up per-frame MID changes in
  our pass; the game-thread cost measured avg 3.5 µs, max 47 µs per frame.
- `runA_anim_beat1.png` / `runA_anim_beat2.png` — the Time-driven stripe material
  (`M_VaCuusSpike_Anim`) between the same beats: RMSE **17.9%**, phase visibly advanced —
  Time animation flows because forced republish re-runs the replay (and so the material
  evaluation) every recorded frame.

## Run B — the freeze, forced republish OFF (`vacuus.MaterialForcedRepublish 0`)

- `runB_frozen_beat1.png` / `runB_frozen_beat2.png` — the SAME animated material, beats
  5 s apart: RMSE **0.18%**, equal to the control cell's floor (0.17%) — pixel-frozen.
  PerfLog for the same window: published=0, skipped=100%. This is spec §2(f)'s
  source-verified fact observed: the composite cannot re-evaluate a material, so between
  publishes the RT holds the last evaluation.

## Run S — the monolithic gate (packaged Linux Development, cooked paks)

- `runS_staged_blend_matrix.png` — the same matrix rendered by the **packaged** VcHost
  binary from cooked content: the `FVaCuusMaterialVS/PS` permutations exist in the
  cooked `VULKAN_SM6 … Game` shader maps (no `HasEditorOnlyData`), zero shader misses
  logged. The load-bearing GO fact.
- `runS_staged_anim_beat1.png` / `runS_staged_anim_beat2.png` — beats 3.5 s apart in the
  packaged game: RMSE **18.0%**, animating.

Note for reproduction: the spike materials are referenced by nothing, so cooking them
requires the host project's `DefaultGame.ini`:
`[/Script/UnrealEd.ProjectPackagingSettings] +DirectoriesToAlwaysCook=(Path="/VaCuus/Spike")`.
The assets themselves were authored once by an editor python run (see the Task 5 report);
runtime-constructed UMaterials cannot compile outside the editor.
