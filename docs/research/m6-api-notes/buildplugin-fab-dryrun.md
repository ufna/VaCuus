# M6 Task 6 — the Fab dry-run, run for real (2026-08-02)

Machine: this one (Linux, UE 5.8.1 source at `/w/Unreal/UnrealEngine`). Companion to
`buildplugin-fab.md` (the research); this page records what actually happened, in the
bundle-cook-experiments.md style: commands, verbatim outputs, and errata against the
research where running found them. The compat-seam half of the task lives in
`Source/VaCuusRender/Private/VaCuusEngineCompat.h` (the four reroutes) and
`docs/passport/2026-08-vacuus-shim1.md` (the owner-hardware experiment); review
round 1 narrowed hotspot 3 to the scene-free SetParameters overload alone — the
batched scratch/commit pair it sits in is used as seven pairs across three files,
was rated stable by the research, and wrapping it only where convenient had made
the seam's one-file promise false (the header and SHIM-1 row 3 carry the
correction).

## 1. FilterPlugin.ini — the three rules and their reasons

`Config/FilterPlugin.ini` (previously deliberately empty — its Task 14 comment stands,
updated): rules land ON TOP of BuildPlugin's default filter and the latest matching rule
wins (BuildPluginCommand.Automation.cs:459-472; exclude syntax FileFilter.cs:125-134;
root anchoring FileFilter.cs:322-337 — all re-opened this task).

| Rule | Why |
|---|---|
| `/Web/...` | no default rule includes Web at all (BPC:459-472); the arch spec mandates Web ships source-only |
| `-/Web/node_modules/...` + `-/Web/.../node_modules/...` | the esbuild ELF (`Web/node_modules/@esbuild/linux-x64/bin/esbuild`) is a hard stop under the `.exe`/`.msi`-class ban; second rule guards nested layouts |
| `-/Source/ThirdParty/RmlUi/Backends/...` | never-compiled upstream demo code; the tree's only `.at(` sites (akj.6.9's evidence scope); per-backend licenses for unshipped code |

Deliberate NON-exclusions, each a decision not an oversight:

- **`Source/*/Private/Tests/` ships** — module test source rides `/Source/...`; the
  default `/Tests/...` exclude anchors to the plugin ROOT only (FileFilter.cs:322-337),
  and stripping test source would hand buyers a suite they cannot run.
- **`Content/DevUI/Tests/` ships** — the shipped automation tests load these fixtures;
  excluding them makes the buyer's first `Automation RunTests VaCuus` red. The place
  Tests/ must never appear is the COOKED BUNDLE a buyer's game ships, and that is
  enforced in the packer itself (VaCuusBundle.cpp:265-270, `IsExcludedTestPath`) — see
  §5 on the marker-only bundle asset.
- **`gen_relays.sh` ×2 ship** — the documented re-vendor procedure (invoked as
  `bash .../gen_relays.sh`); exec bits stripped instead (§2).

## 2. Exec-bit hygiene — found and fixed

Full-tree sweep (`find . -type f -perm /111` + `git ls-files -s | awk '$1=="100755"'`):

| File(s) | Verdict |
|---|---|
| `Source/VaCuusJs/gen_relays.sh`, `Source/VaCuusRml/gen_relays.sh` | **stripped to 644** — ship as source; docs invoke them via `bash` |
| `Web/packages/cli/bin/vacuus.mjs` | **stripped to 644** — npm chmods `bin` targets itself on install, and Web/README.md invokes it via `node` |
| `.beads/hooks/*` (6 files) | kept — git hooks need the bit; repo-only, no filter rule reaches `.beads` |
| `Binaries/Linux/*.so` (5) | kept — real ELF build artifacts, gitignored, and BuildPlugin packages only exact-path manifest products (no blanket `/Binaries/...` rule, BPC:459-472) |
| `Tools/fab_scan.sh` (new) | executable on purpose — repo tooling under `Tools/`, which no filter rule includes |
| `Tools/scan-fixture/planted_execbit.txt` (new) | committed 100755 ON PURPOSE — it is the EXECBIT plant |

**Erratum vs the research note**: `buildplugin-fab.md` §2 lists the two Backends
`compile_shaders.py` as exec-bit-set; in this tree both are 644 and start with
`import sys` (no shebang either). Moot regardless — Backends is excluded.

## 3. The scan + its fixtures — seen to fail, both ways, and seen to REFUSE

`Tools/fab_scan.sh`: SIX classes (EXTENSION, EXECBIT, SHEBANG, ELF_MAGIC,
NODE_MODULES, LFS_POINTER — spec §2(e)'s five, plus the review round's ELF-magic
class: an ELF with a harmless name, no bit and no shebang evaded all five originals,
and that renamed-esbuild shape is the scan's whole charter), a three-entry shebang
whitelist (the two `gen_relays.sh` + `vacuus.mjs`, reasons in the script), and TWO
mandatory self-tests per invocation: `Tools/scan-fixture/` (one plant per class,
must report exactly the six) and `Tools/scan-fixture-whitelist/` (a shebang at
exactly a whitelisted path must be forgiven while its near-miss neighbour is
reported — the whitelist branch is seen to both admit and refuse, closing review
round 1's untested-branch gap).

**Fail-closed (review round 1's blocker, demonstrated by the reviewer):** the
original scan blessed an empty directory (CLEAN exit 0) and a tree whose unreadable
subdir hid a `.so` (find's Permission-denied went to the screen and nowhere else).
Now: the target must carry a root `*.uplugin` (every BuildPlugin package does —
BPC:433-445 rewrites and saves it there), and every find/head stderr is collected —
any read failure ABORTS the scan (exit 2). Both refusals re-demonstrated, verbatim:

```
--- REFUSAL LEG A: empty dir ---
SCAN ABORTED: no *.uplugin at .../scratchpad/empty-dir — not a BuildPlugin package root
exit=2
--- REFUSAL LEG B: unreadable subdir hiding a .so ---
[self-tests 1+2 pass as below]
== fab_scan: scanning .../scratchpad/denied-tree ==
SCAN ABORTED: find could not read the tree (target tree):
find: '.../denied-tree/locked': Permission denied   [x5 — one per check pass]
exit=2
```

**The fail leg** (a package-shaped tree — the fixture plus a dummy `Demo.uplugin` —
carrying all six plants), verbatim:

```
== fab_scan self-test 1: scanning the planted fixture (must report exactly the 6 plants) ==
ELF_MAGIC	planted_elf.bin
EXECBIT	planted_execbit.txt
EXTENSION	planted.exe
LFS_POINTER	planted_lfs_pointer.txt
NODE_MODULES	node_modules
SHEBANG	planted_shebang.sh
== fab_scan self-test 2: whitelist mode (must forgive the whitelisted path, report the near-miss) ==
SHEBANG	Source/VaCuusJs/gen_relays_nearmiss.sh
== self-tests OK: the scan has been seen to fail, exactly as planted ==

== fab_scan: scanning .../scratchpad/planted-package ==
ELF_MAGIC	planted_elf.bin
EXECBIT	planted_execbit.txt
EXTENSION	planted.exe
LFS_POINTER	planted_lfs_pointer.txt
NODE_MODULES	node_modules
SHEBANG	planted_shebang.sh
SCAN: FAIL (6 violation(s))
exit=1
```

And the self-test's own restore-the-bug — `planted.exe` removed (round-1 run, then
five classes), the self-test refused to bless anything, expected-vs-got printed,
exit 2; restored. Known limits are STATED in the script header rather than
discovered: symlinks not followed (UAT-materialized trees contain none), and
Git-for-Windows noacl mounts make `-perm /111` refuse everything (fail-closed;
SHIM-1 step 4 says run the scan under WSL/Linux).

The green leg over the real package output is in §7.

## 4. Third-party disclosure list

Every entry points at a real in-tree license file THE PACKAGE INCLUDES (spec §3.3);
each file was opened this task and its license identified from its own text.

| Component | License | In-tree pointer (ships via) | Compiled? |
|---|---|---|---|
| RmlUi 6.x (vendored fork) | MIT (CodePoint/Shift Technology + contributors) | `Source/ThirdParty/RmlUi/LICENSE.txt` (`/Source/...`) | yes |
| robin_hood hashing (inside RmlUi Core) | MIT (Martin Ankerl) | `Source/ThirdParty/RmlUi/Include/RmlUi/Core/Containers/LICENSE.txt` (`/Source/...`) | yes (header) |
| itlib flat_map/flat_set (inside RmlUi Core) | MIT (Chobolabs / Borislav Stanimirov) | same Containers/LICENSE.txt — **verified present, the spec's "verify in-tree" discharged** | yes (header) |
| RmlUi Debugger embedded fonts (Courier Prime Code) | OFL 1.1 | `Source/ThirdParty/RmlUi/Source/Debugger/LICENSE.txt` (`/Source/...`) | no (zero Debugger relay TUs in `VaCuusRml/Private/Gen/`) — ships as source only |
| quickjs-ng | MIT (Bellard, Gordon, Noordhuis, Ibarra Corretgé) | `Source/ThirdParty/quickjs-ng/LICENSE` (`/Source/...`) | yes |
| preact 10.29.7 (patched, `@vacuus/preact`) | MIT (Jason Miller) | `Web/packages/preact-vacuus/PREACT-LICENSE` (`/Web/...`); the committed `Content/DevUI/M5Hud/hud_bundle.js` opens with the MIT banner the CLI prepends | n/a (JS) |
| LatoLatin-Regular.ttf | OFL 1.1 | **`Content/DevUI/fonts/OFL.txt` — ADDED this task** (`/Content/...`): copyright line taken verbatim from the font's own name table ("Copyright (c) 2011-2015 by tyPoland Lukasz Dziedzic ... with Reserved Font Name \"Lato\""), license body the canonical OFL 1.1 text (byte source: the vendored Debugger LICENSE.txt:16-99, same canonical text) | n/a (asset) |
| FreeType | FTL (attribution) | **not in the package** — engine-provided (`AddEngineThirdPartyPrivateStaticDependencies(Target, "FreeType2")`, VaCuusRml.Build.cs:69); the engine ships its own attribution | via engine |
| HarfBuzz | — | **NOT USED**: no build rule, no source reference anywhere outside the arch spec's dependency table — that table row is stale, flagged for Task 7's §15 amendment | no |

Excluded WITH their code (no disclosure entry needed): the Backends per-backend
licenses (`RmlUi_DirectX/LICENSE.txt`, `RmlUi_Vulkan/LICENSE.txt`,
`RmlUi_SDL_GPU/SDL_shadercross/LICENSE.txt`) leave the package with
`-/Source/ThirdParty/RmlUi/Backends/...`.

## 5. The bundle asset in the package is a marker, not a payload

`Content/Bundles/DevUIBundle.uasset` ships at **1,202 bytes**: the editor save
serializes the SourceNote marker only (spec §2(a) — payload+index serialize exclusively
under `Ar.IsCooking()`), so no DevUI content — and in particular no `Tests/` fixture —
can leak through the shipped asset. A buyer's cooked game packs its own payload, and
that pack excludes Tests at enumeration time (VaCuusBundle.cpp:265-270).

## 6. BuildPlugin -StrictIncludes — run 1 FAILED, and that was the point

Command (clean rsync clone of the work tree at `/w/Unreal/FabDryRun/VaCuus` — no `.git`,
no `.beads`, no `Binaries/Intermediate`, `Web/node_modules` verified absent, all 8
`.uasset` verified real packages by `file`, not LFS pointers):

```
RunUAT.sh BuildPlugin -Plugin=/w/Unreal/FabDryRun/VaCuus/VaCuus.uplugin \
  -Package=/w/Unreal/FabDryRun/Package -TargetPlatforms=Linux -StrictIncludes
```

Run 1: the editor leg died at 6905/6910 actions after 37 m —
`Result: Failed (OtherCompilationError)`, ExitCode=6. Every error was a **missing
include that PCH/unity had been silently supplying** (`-StrictIncludes` = `-NoPCH
-NoSharedPCH -DisableUnity`, BuildPluginCommand.Automation.cs:133-137) — the exact
failure class the research predicted for Fab's foreign compile environments. All fixed
in the work tree with real includes (never suppressed); the full-log unique list and
the root-cause grouping:

| Error (unique, from UBA-UnrealEditor-Linux-Development.txt) | Root cause | Fix (work tree) |
|---|---|---|
| `VaCuusModelLayoutTestTypes.h:103: field has incomplete type 'FAnsiString'` (+3 uses in VaCuusModelSamplerTest.cpp) | CoreMinimal only forward-declares it (ContainersFwd.h:24) | `#include "Containers/AnsiString.h"` (+`Utf8String.h` for the sibling member) in the types header; AnsiString.h in the sampler test |
| `no matching function for call to 'NewObject'` ×6 (VaCuusModelApiTest:72, VaCuusModelLayoutTest:369/426/568, VaCuusModelViewTest:113, VaCuusRender.cpp:1055) | `GetTransientPackage()` returns `UPackage*` and UObjectGlobals.h only forward-declares UPackage — the derived-to-base conversion needs the complete type | `#include "UObject/Package.h"` in all four TUs |
| `VaCuusRmlDocumentHost.h:163: use of undeclared identifier 'EMouseCursor'` + the UCFS_FChecker noise at cpp:581 + `TUniquePtr<FVaCuusRmlDocumentHost>` → `TUniquePtr<IVaCuusDocumentHost>` failures in 3 test TUs | one missing `GenericPlatform/ICursor.h`; the rest is clang error-recovery cascade from the broken member declaration | the one include in the host header |
| `VaCuusWorldInputProcessor.h:310: unknown type name 'FHitResult'` + MakeShared/TSharedPtr conversion cascade at cpp:126/133/158 | missing `Engine/HitResult.h`; cascade again | the one include in the processor header |
| `VaCuusSlateRoutingTest.cpp:1009/1178/1286: incomplete type 'FVaCuusInputEvent'` | test TU never included the event header | `#include "VaCuusInputEvent.h"` |
| `Shader.h:903: incomplete type 'FViewUniformShaderParameters'` (instantiated at VaCuusMaterialDraw.h:74) | the header's inline `GetUniformBufferParameter<FViewUniformShaderParameters>()` needs the complete struct (SceneView.h:1157); MaterialShader.h:18 only forward-declares it, and the .cpp included SceneView.h AFTER its own header — unity/PCH had been masking the order | `#include "SceneView.h"` in VaCuusMaterialDraw.h |

The cascades are worth recording: TWO of the scariest-looking errors
(`SharedPointerInternals.h:885 UpdateWeakReferenceInternal`, the TUniquePtr
derived-to-base refusals) were clang refusing conversions on classes it had marked
invalid after ONE bad member declaration — the fix in both cases was a single include,
not anything about shared-pointer machinery.

After the fixes: editor target rebuilt (Succeeded), full suite re-run (results below),
clone refreshed via the same rsync, run 2 relaunched.

## 7. Runs 2 and 3 — three legs green, and one more filter finding

**Run 2** (after the §6 fixes): `BUILD SUCCESSFUL`, ExitCode=0, **2 m 53 s total** (UBA
cache warm; the three legs' UBT times 70.4 s / 54.5 s / 46.3 s for editor-Dev /
game-Dev / game-Shipping). The packaged `.uplugin` rewrite matched the research
(BPC:434-445): `"EngineVersion": "5.8.0"`, `"Installed": true`.

**Run 2's inventory found the filter's last gap**: the package carried
`Binaries/Linux/libUnrealEditor-VaCuus*.so/.sym/.debug` (manifest build products are
included by exact path, BPC:462) and 674 `Intermediate/Build/.../Inc` UHT files
(BPC:466). Right for a precompiled drop; wrong for the Fab source-only submission
(mandate 4.3.6.1.a), where the scan itself would — correctly — refuse the `.so`s.
Fix: `-/Binaries/...` and `-/Intermediate/...` in FilterPlugin.ini (rules land after
the build-product rules; latest match wins), so the package IS the upload shape and
the three compile legs remain the compilability proof.

**Run 3** (final filter): `BUILD SUCCESSFUL`, ExitCode=0, 2 m 31 s. Package: **976
files, 13 MB**. The hardened scan over it (post-review: six classes, two self-tests,
fail-closed), verbatim:

```
== fab_scan self-test 1: scanning the planted fixture (must report exactly the 6 plants) ==
ELF_MAGIC	planted_elf.bin
EXECBIT	planted_execbit.txt
EXTENSION	planted.exe
LFS_POINTER	planted_lfs_pointer.txt
NODE_MODULES	node_modules
SHEBANG	planted_shebang.sh
== fab_scan self-test 2: whitelist mode (must forgive the whitelisted path, report the near-miss) ==
SHEBANG	Source/VaCuusJs/gen_relays_nearmiss.sh
== self-tests OK: the scan has been seen to fail, exactly as planted ==

== fab_scan: scanning /w/Unreal/FabDryRun/Package ==
SCAN: CLEAN
```

Inventory: **`Tools/fab_inventory.sh` (committed post-review so the count is
reproducible, not prose) → `pass=34 fail=0`** against the same package — Web/
present source-only with `node_modules` absent; Backends absent with the compiled
subtree intact; all license pointers present; the 1.2 KB marker bundle asset
present; `Tools/`, `docs/`, `CLAUDE.md`/`AGENTS.md`, root `Tests/`, `Binaries/`,
`Intermediate/` all absent; `Source/*/Private/Tests` present (decision, §1);
`Web/.gitignore` present ON PURPOSE (its row in the checklist carries the reason).
The scan/fixture/checklist changes live under `Tools/`, which is never packaged —
the run-3 package needed no rebuild for them, and the FilterPlugin edit in the same
review round was comment-only (`:460`→`:462`).

## 8. Side effect worth its own warning: BuildPlugin rewrites engine module manifests

After the BuildPlugin runs, the next editor launch DIED at startup:
`Assertion failed: !bIsRunningPlatform ... "Cannot have more than one Running
Platform"` (TargetPlatformManagerModule.cpp:1070). Cause, established from
timestamps: `Engine/Binaries/Linux/LinuxArm64/` held STALE (April-30)
`libUnrealEditor-LinuxArm64TargetPlatform*` modules the current editor never loaded —
until the HostProject editor leg rewrote that directory's `UnrealEditor.modules` with
the CURRENT BuildId (mtime = the run), which made the module manager load the
ABI-stale .so trio and trip the assert. Fix: delete the three stale module binaries +
that manifest; the suite then ran green. **Dev-loop rule this earns: after a
BuildPlugin dry-run against a source engine, expect `Engine/Binaries/**/
UnrealEditor.modules` manifests to have been rewritten — a stale platform module
directory becomes live again.** (Cousin of the "program targets go stale" note; same
tree, same class.)

## 9. What SHIM-1 inherits

The 5.8-Linux legs are done here; `docs/passport/2026-08-vacuus-shim1.md` carries the
5.6/5.7 (and all Win64) procedure with per-hotspot expected failure shapes, keyed to
`Source/VaCuusRender/Private/VaCuusEngineCompat.h`. Fab's UNRECORDED rules (anything
past 4.3.6.1.a/e) remain a dry-run-confirms item at upload time — new findings land in
the disclosure draft (§4), per the spec's risk table.
