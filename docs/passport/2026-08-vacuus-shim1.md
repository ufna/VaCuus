# Experiment SHIM-1 — the 5.6/5.7 compat build (owner hardware)

**What this is** (M6 spec §2(f)/§3.3; research `docs/research/m6-api-notes/buildplugin-fab.md`
§3): the engine-version port, written as an experiment because it CANNOT run on the dev
machine — no 5.6/5.7 SDK exists here, and the project conventions forbid speculative
`#if` guards asserting header shapes nobody here has read. The seam is already in place:
every drift-suspect engine call goes through
`Source/VaCuusRender/Private/VaCuusEngineCompat.h` (four hotspots, each documenting what
5.8 does with opened citations and what to CHECK on the older engine). Ground truth for
"zero guards exist today": grepping all five modules for
`ENGINE_MINOR_VERSION|ENGINE_MAJOR_VERSION|UE_VERSION|VERSION_NEWER_THAN|VERSION_OLDER_THAN`
returns only the compat header's own comments.

**The claim this experiment buys**: "`RunUAT BuildPlugin` passes on 3 engine versions"
(arch spec §14's M6 accept line). On this machine only the 5.8-Linux leg ran (see
`buildplugin-fab-dryrun.md`); 5.6 and 5.7 — and everything Win64 — are exactly this page.

## Procedure, per engine (5.6.x, then 5.7.x)

1. **Clean clone, no node_modules** (the esbuild ELF must never enter the copy):

   ```
   git clone <repo> VaCuus-shim && cd VaCuus-shim && git lfs pull
   # verify: find . -type d -name node_modules   -> only Tools/scan-fixture/node_modules
   # verify: file Content/Bundles/DevUIBundle.uasset -> "Unreal Engine package", NOT a ~130-byte LFS pointer
   ```

2. **BuildPlugin, strict, from that engine** (the same shape as the 5.8 dry-run;
   `-StrictIncludes` = no PCH, no unity — BuildPluginCommand.Automation.cs:133-137 —
   because Fab recompiles on compilers we do not run):

   ```
   <UE_5.x>/Engine/Build/BatchFiles/RunUAT.bat BuildPlugin ^
     -Plugin=<abs>/VaCuus-shim/VaCuus.uplugin ^
     -Package=<abs-outside-plugin-and-engine>/Package ^
     -TargetPlatforms=Win64 -StrictIncludes
   ```

   Three legs run per invocation: host editor (Development) + Win64 game
   (Development AND Shipping) — BuildPluginCommand.Automation.cs:297,309-310. On a
   Windows host, Linux legs are silently dropped, never attempted (:505-517) — a pass
   there says nothing about Linux, and vice versa.

3. **Every compile break lands in the seam, not at the call site**: fix inside
   `VaCuusEngineCompat.h` under `#if UE_VERSION_OLDER_THAN(5, 8, 0)` (or the tightest
   version test that matches what the 5.6/5.7 header actually says — open it), re-run
   step 2. When 5.6 and 5.7 both pass, re-run once against 5.8 to prove the guards
   did not regress the primary engine.

4. **Scan + smoke**: `bash Tools/fab_scan.sh <Package-dir>` must self-test-fail on the
   fixture then report CLEAN; then drop the packaged plugin into a blank project of that
   engine version and load a DevUI document (the setup.md happy path).

## Expected failure shapes (what a break looks like, per hotspot)

These are the four places the research ranked likeliest to differ; each error should
point INTO the compat header or into one of its four call-site files. A break anywhere
else is a finding the research missed — record it on the bead, then still fix it in the
seam if it is version drift.

| # | Hotspot | Expected error shape on 5.6/5.7 | Seam fix |
|---|---|---|---|
| 1 | `ICustomSlateElement::FDrawPassInputs` field set (RenderingCommon.h:945-955 on 5.8) | `'FDrawPassInputs' has no member named 'OutputTexture'` (or ElementsOffset / SceneViewRect / bOutputIsHDRDisplay) in `VaCuusEngineCompat.h`; or `Draw_RenderThread ... marked 'override', but does not override` if the virtual's shape itself moved | repoint the four accessors (old-name candidates on the 5.5-deprecated `FSlateCustomDrawParams`, :963-972: ViewOffset/ViewRect/bIsHDR); if the struct moved/renamed, edit the `FVaCuusDrawPassInputs` alias |
| 2 | `RegisterInputPreProcessor(processor, FInputPreprocessorRegistrationKey)` (SlateApplication.h:1568/:222 on 5.8) | `no matching function for call to 'FSlateApplication::RegisterInputPreProcessor'` or `'FInputPreprocessorRegistrationKey' was not declared` in `VaCuusEngineCompat.h` | fall back to the `EInputPreProcessorType` overload (:1560 on 5.8) keeping PreGame; if even the enum is absent, the plain overload (:1544) — and RECORD the lost before-Game ordering guarantee |
| 3 | `FMaterialShader::SetParameters(..., const FSceneInterface*)` (MaterialShader.h:88-92 on 5.8) | `no matching function ... SetParameters` with only the `const FSceneView&` candidate listed, in `VaCuusEngineCompat.h` | fabricate the minimal view the pass's own view UB already mimics (SlateRHIRenderingPolicy.cpp:706-756 recipe), inside the seam |
| 4 | `FSlateDrawElement::MakeCustom` declaring header (DrawElementTypes.h:303 on 5.8) | none expected — the compat header includes umbrella `DrawElements.h`, which predates the split; a surprise would be a changed MakeCustom signature | one-line fix in `VaCuusCompat::MakeCustomDrawElement` |

**Also expected on Win64, unrelated to the seam** (research §1 risk table): the quickjs
`/experimental:c11atomics` decision (VaCuusJs.Build.cs:47-50) — a Win64 compile of the
vendored C may demand it; that is bead akj.6.19's sibling handoff item, not a compat-seam
entry.

## Success shape / report-back

Per engine: the UAT tail `BUILD SUCCESSFUL`, the three target/config pairs listed in the
log, `fab_scan` self-test-fail-then-CLEAN, the blank-project document load. Report: the
diff of `VaCuusEngineCompat.h` (the entire port should live there), any off-seam breaks,
and the 5.8 re-run confirming no regression.
