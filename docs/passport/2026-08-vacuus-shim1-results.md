# SHIM-1 results — the UE 5.6 leg (Linux, 2026-08-10)

First execution of Experiment SHIM-1 (`2026-08-vacuus-shim1.md`), against **5.6.1**
(`/w/Unreal/UE5.6`, `Build.version` MajorVersion 5 / MinorVersion 6 / PatchVersion 1) with
**5.8.1** (`/w/Unreal/UnrealEngine`) as the control. Bead `VaCuus-ys2`.

**Outcome: the plugin builds and passes on 5.6.1, and 5.8.1 is unregressed.**

| | 5.6.1 | 5.8.1 (re-run after the port) |
|---|---|---|
| Editor target (`VcHostEditor Linux Development`) | Succeeded, 0 errors | Succeeded, 0 errors |
| Warnings from plugin sources | 0 | 0 |
| `Automation RunTests VaCuus` | **227 performed, 227 Success, 0 Fail** | **227 performed, 227 Success, 0 Fail** |

Host projects: `/w/Unreal/VcHost56` (new, 5.6) and `/w/Unreal/VcHost` (existing, 5.8). Not
run on this leg: `RunUAT BuildPlugin -StrictIncludes`, Win64, and 5.7 — see *What this
leg does not claim*.

## The experiment's central prediction was wrong, and that is the main result

SHIM-1 ranked four hotspots as likeliest to drift and routed them through
`VaCuusRender/Private/VaCuusEngineCompat.h` so that "a version port is an edit to THIS
file". **All four are byte-identical in 5.6.1 and 5.8.1** — checked by opening both
headers before the first compile:

| Hotspot | 5.6.1 | Verdict |
|---|---|---|
| 1 `ICustomSlateElement::FDrawPassInputs` | `RenderingCommon.h:910-920`, same 8 fields in the same order as 5.8's `:945-955` | no change |
| 2 `RegisterInputPreProcessor(.., FInputPreprocessorRegistrationKey)` | `SlateApplication.h:1526`; struct at `:222`; `EInputPreProcessorType::PreGame` at `:189-208` | no change |
| 3 `FMaterialShader::SetParameters(.., const FSceneInterface*)` | present in 5.6 `MaterialShader.h` | no change |
| 4 `FSlateDrawElement::MakeCustom` | `DrawElementTypes.h:289`, same signature; umbrella `DrawElements.h` includes it | no change |

Every real break was somewhere else, and four of the seven were **outside VaCuusRender
entirely**. The one-file promise could not hold as written; it now holds as a pair —
render-path drift in that header, everything else in the new
`Source/VaCuus/Public/VaCuusCoreCompat.h`, which cross-references it.

The lesson is narrower than "the research was wrong": the four hotspots were ranked by
*visible churn near them in 5.8* (deprecations, header splits). That is a good proxy for
"this area is moving" and a bad one for "this call breaks", because a deprecation is
precisely the engine promising the old spelling still compiles.

## What actually broke (7 findings)

Three were found statically before the first build, by diffing the plugin's 478 `#include`s
and its identifier set against both engines' full public-header corpora (12,044 vs 13,027
headers). Four needed the compiler or the test run.

| # | What | 5.6 | 5.8 | Fix, and why that form |
|---|---|---|---|---|
| 1 | `IPooledRenderTarget` | `RendererInterface.h:489` | split into `PooledRenderTarget.h:433`, still pulled by `RendererInterface.h:21` | include the **umbrella** — works on both, no guard (`VaCuusWorldSink.h/.cpp`) |
| 2 | `UE::Cook::FCookDependencyContext` | defined in `Cooker/CookDependency.h:29` | forward-declared there (`:22`), defined in the split-out `CookDependencyContext.h`; **no** header present in both pulls it | `#if __has_include(...)` — tests the file's presence, which is the fact actually checked, instead of guessing which release split it (`VaCuusBundle.cpp`) |
| 3 | `UE_FORCEINLINE_HINT` | absent from all of `Runtime/Core` | `HAL/Platform.h:766-776`, `FORCEINLINE` unless the target opts into `inline` | `#ifndef` fallback to its own 5.8 default; inert where the engine defines it (`VaCuusModelSampler.cpp`) |
| 4 | `UMaterialInterface::GetMaterialResource` / `GetRelevance_Concurrent` | keyed by `ERHIFeatureLevel::Type` only (`MaterialInterface.h:603/609`, `:761`) | keyed by `EShaderPlatform` (`:841/849`, `:1011`); the feature-level forms survive as `UE_DEPRECATED(5.7)` **and** `final` | the only genuine version guard, `UE_VERSION_OLDER_THAN(5, 7, 0)` — and 5.7 is the boundary the 5.8 deprecation message itself names (`VaCuusCoreCompat.h`, two call sites in two modules) |
| 5 | `FTickableGameObject(ETickableTickType)` | no such constructor; `FTickableGameObject()` queues unconditionally (`Tickable.cpp:130-136`) | `Tickable.h:158`; constructing with `Never` skips registration entirely (`Tickable.cpp:135-144`) | `VACUUS_TICKABLE_STARTING_TYPE_ARG`. **No behaviour change**: `GetTickableTickType()` already answers `Never` until `bInitialized`, which is what 5.6 asks for at the next tick |
| 6 | `FIntProperty` / `FStructProperty` / `FArrayProperty` constructors | `EObjectFlags` is **required** (`UnrealType.h:2179`, forwarded to `FField::FlagsPrivate`) | `FlagsPrivate` deleted; two-argument form added and the three-argument one `UE_DEPRECATED(5.8, "... remove that parameter")` | overload ranking (`NewProperty<T>`), because *no single spelling is warning-free*. Six sites, `VaCuusModelLayoutTest.cpp` |
| 7 | `UStringTable` auto-registration | registers **every non-CDO** table (`StringTable.cpp:356-360`) | registers only `IsAsset() && ShouldAutoRegister(...)` (`:401`) | ask the registry before registering by hand, and unregister only what we registered (`VaCuusTranslationFoundationTest.cpp`) |

Finding 7 is the one that a compile could not have caught: it is a hard `checkf` inside
`FStringTableRegistry::RegisterStringTable` ("String table ID '%s' is already in use!",
`StringTableRegistry.cpp:71`) that **killed the whole automation run** after 214 tests. The
registry function is byte-identical in both engines — the difference is entirely in the
caller, which is why nothing in the API diff pointed at it.

Findings 6 and 7 are test-only. Production code never registers a string table; it only
asks (`VaCuusSubsystem.cpp:320`), which was already the portable form.

**The first attempt at finding 6 was wrong and the control caught it.** Passing the flags
argument compiles on 5.6 *and* 5.8, so the 5.6 build went green — and the 5.8 re-run
produced six `-Wdeprecated-declarations` warnings whose text ends "otherwise your project
will no longer compile". A port verified only on the new engine would have shipped that.

## The other half of the port: content

Independent of any C++, and a hard stop on its own. All 8 committed `.uasset` files carried
`FileVersionUE5 = 1018` (5.8's `IMPORT_TYPE_HIERARCHIES`). 5.6's ceiling is **1017**
(`AUTOMATIC_VERSION`, `ObjectVersion.h`), and `FPackageFileSummary::IsFileVersionTooNew`
(`PackageFileSummary.h:345-347`, reached from `LinkerLoad.cpp:1596`) refuses such a package
outright. **The refusal is silent** — no version line is logged; the package simply never
appears, and the first symptom is `Failed to find object`.

Packages are forward-compatible only, so fixtures must be authored on the **oldest**
supported engine. Done, and now reproducible:

- `M_VaCuusWorldPanel` — the authoring script was already committed (`m5-t6-worldspace/`);
  re-ran it on 5.6.
- the six `/VaCuus/Spike/*` — **had no committed script**; the graphs lived only inside the
  `.uasset` files. Written now as `m5-t5-material-spike/author_spike_materials.py`, by
  reading the 5.8 assets back through `MaterialEditingLibrary`.
- `DevUIBundle.uasset` — regenerated headlessly with `vacuus.Bundle.CreateAsset`.

All 8 now carry `FileVersionUE5 = 1017` and load on both engines; the 5.8 suite passing is
the forward-compatibility check.

### Evidence for both halves

**The spike script reproduces the fixtures.** Authored into `/Game/SpikeGen` on 5.8 and
re-dumped: **zero** structural differences across all six — material properties, expression
classes, expression properties, links, and material-property inputs. Negative control: a
copy perturbed in two places (stripe count `4.0 -> 5.0`, translucent alpha `0.55 -> 0.56`)
is reported with exactly those two differences and no others. The method was itself
controlled by re-dumping `M_VaCuusWorldPanel`, whose authoring script is committed: the
read-back reproduced its ten nodes and every link.

**The version ceiling is real, not inferred.** With the 5.6-authored `M_VaCuusSpike_Opaque`
in place, all eight assets load on 5.6. Drop the 5.8-saved copy of that one file back in and
re-run in the same process: that asset alone is refused while its six 1017 siblings load.
Restore, and it loads again.

## What this leg does not claim

- **`RunUAT BuildPlugin -StrictIncludes` has not been run on 5.6.** This leg is a project
  build. The acceptance line on bead `VaCuus-93v` is still unmet for 5.6 until the packaged
  no-PCH/no-unity build runs.
- **Win64 and macOS on 5.6**: not attempted. Linux only.
- **5.7**: no SDK on this machine. The two `UE_VERSION_OLDER_THAN(5, 7, 0)` guards are
  written from a 5.6-vs-5.8 pair, and finding 4's boundary is cited from 5.8's own
  deprecation text while finding 5's is not — it is the plugin's best information. Neither
  can fail silently: a wrong boundary is a compile error at the guarded call site.
- **Behaviour differences accepted, not fixed**: on 5.6 the subsystem is enqueued with the
  tickable registry at construction rather than at `Initialize()` (finding 5). Its queue
  takes a lock (`Tickable.cpp:9-14`), and what ticks is unchanged.
- The `_ElementProp` inner-property path and the other five `NewProperty<T>` sites are
  covered by the layout tests that already existed; no new test was added for the seam
  itself, because on any one engine only one branch of it exists.
