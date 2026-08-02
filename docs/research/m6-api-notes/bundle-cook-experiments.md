# M6 Task 3 — the cook experiments, run for real (2026-08-01)

Machine: this one (Linux, UE 5.8.1 source at `/w/Unreal/UnrealEngine`, host `/w/Unreal/VcHost`).
Method: `UnrealEditor-Cmd VcHost.uproject -run=Cook -TargetPlatform=Linux [-CookIncremental]
[-legacyiterative]`, one cook per row, outcomes read from the cook log's
`LogVaCuus: Bundle ... packed` line and `LogCookStats` (`NumPackagesSaved` /
`NumPackagesIncrementallySkipped`). The asset: `/VaCuus/Bundles/DevUIBundle`
(created headlessly by `vacuus.Bundle.CreateAsset`), whose PreSave packs the loose
DevUI tree and whose `OnCookEvent` registers the `VaCuusBundleTree`
`FCookDependency::Function` tree-hash.

## Two errata against `bundle-cook.md` — found only by running

1. **Incremental cook is NOT default-on in stock 5.8.** The code default is true
   (`bool bDefaultIncremental = true;`, CookOnTheFlyServer.cpp:10544-10548 — as the
   research read), but the SHIPPED config overrides it:
   `Engine/Config/BaseEditor.ini:393 CookIncrementalDefaultIncremental=false`. A stock
   cook without `-CookIncremental` logs `FULL COOK: Incremental Cooks are disabled by
   default in Editor.ini:[CookSettings]:CookIncrementalDefaultIncremental=false` and
   recooks everything. Every incremental row below passed `-CookIncremental`.
2. **Incremental skipping is class-gated, and project plugins are OUTSIDE the gate.**
   `IsIncrementalCookEnabled` (TargetDomainUtils.cpp:30) requires every imported class
   on an allowlist; stock config allowlists engine script packages only
   (`BaseEditor.ini:475 +IncrementalClassScriptPackageAllowList=Allow,<EngineRoot>`).
   With no opt-in, the bundle package is `IncrementallyModified: IncrementalCookDisabled`
   (diagnosed via `-CookIncrementallyModifiedDiagnostics` →
   `Saved/Cooked/Linux/VcHost/Metadata/ModifiedCookedPackages.txt`) and REPACKS ON
   EVERY COOK — safe (never stale), just never skipped. The host opted in with
   `VcHost/Config/DefaultEditor.ini`:
   `[CookSettings] +IncrementalClassAllowList=/Script/VaCuus.VaCuusBundle` — honest for
   this class because its cooked bytes are fully determined by dependencies it
   declares (the tree-hash). **This is a buyer-facing setup.md line** (with the
   cook-inclusion `DirectoriesToAlwaysCook` rule it sits next to).

## Exp-COOK-FILEDEP — ZenStore ON (`bUseZenStore=True`, the 5.8 default)

| Cook | Tree state | Outcome (verbatim signals) |
|---|---|---|
| baseline (incremental) | unchanged | `packed 21 file(s), 429251 bytes, hash 1ec520a1c26987e80929f585d4d775be9e6c03d94f0a3fefcf06df95f0eafe3f (0 shadowed duplicate(s), 3 test fixture(s) excluded)` |
| control (incremental, NO change) | unchanged | **no pack line**; `NumPackagesIncrementallySkipped=586`, `NumPackagesSaved=0` — the bundle skipped |
| **edit** (append comment to `m1_hud.rcss`, `.uasset` untouched) | 1 file edited | `packed 21 file(s), 429315 bytes, hash 7ef8b9c5…` ; `NumPackagesSaved=1`, `NumPackagesIncrementallySkipped=585` — **exactly the bundle recooked** |

Verdict: **PASS.** The `FCookDependency::Function` tree-hash invalidates the package on
a loose-file edit the AssetRegistry knows nothing about, and ONLY that package.

## Exp-COOK-ADDFILE — ZenStore ON

| Cook | Tree state | Outcome |
|---|---|---|
| **add** `exp_addfile_probe.rcss` | 1 file added | `packed 22 file(s), 429379 bytes, hash a7c45519…`; `NumPackagesSaved=1`, skipped=585 |
| **delete** the probe + revert the edit (below, "restore") | back to original 21 | repacked; hash returned to **`1ec520a1…` exactly** — the pack is a pure function of the tree, across processes and days |

Verdict: **PASS** for add AND delete — the reason a tree-hash beats per-file `File()`
deps, which cannot see a file that did not exist when they were declared.

## Exp-COOK-FILEDEP — ZenStore OFF (`bUseZenStore=False` in ProjectPackagingSettings)

| Cook | Flags | Outcome (verbatim) |
|---|---|---|
| Z1 | `-CookIncremental` | `INCREMENTAL COOK DEPENDENCIES: Disabled. … the cooker is not using ZenStore, and we do not yet support storage of Incremental Cook dependency data with loose cooked packages. Falling back to legacy iterative cook mode.` then `FULL COOK: … recooking all packages` — `NumPackagesSaved=593`, bundle repacked |
| Z2a | `-legacyiterative` | `Keeping 586. Recooking 0.` — everything skipped, bundle not repacked (tree unchanged; fine) |
| Z2b | `-legacyiterative`, **a packed file DELETED from the tree** | `Keeping 586. Recooking 0.`, `NumPackagesSaved=0` — **the bundle was NOT repacked: the cooked bundle still contains the deleted file. STALE, silently.** |

Verdict: the legacy path **cannot honor the Function dependency at all** (the cooker
says so itself — no storage for dependency data without ZenStore). The DEFAULT
behavior without ZenStore is a full cook every time — always correct, never
incremental. The hazard is exactly one configuration: **ZenStore off + explicit
`-legacyiterative` ships a stale bundle with no warning.** Buyer doc line
(gotchas.md, Track D): with `bUseZenStore=False`, do not pass `-legacyiterative`
on a project that cooks UI bundles; the safe default full cook is what you get
without it. (This is the spec's "fallback always-recook" arriving as the engine's
own behavior rather than our code.)

## Determinism, observed twice more (beyond the double-pack test)

- The editor test suite's pack-on-demand mount (a different process, different code
  path into the same pack) reported the identical hash `1ec520a1…` for the identical
  tree — logged by `Mounted bundle '<PackedOnDemand>' … hash 1ec520a1…`.
- The restore cook returned the cooked hash to `1ec520a1…` after edit+add+delete
  round-tripped the tree back to its original state.
