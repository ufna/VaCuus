# TASK B — RunUAT BuildPlugin, the Fab dry-run, and engine-version shims

All engine citations are UE 5.8.1 source at `/w/Unreal/UnrealEngine`; all were opened and read. `BPC` = `Engine/Source/Programs/AutomationTool/Scripts/BuildPluginCommand.Automation.cs` (593 lines, read in full). Plugin root = `/w/Unreal/VcHost/Plugins/VaCuus`.

## 1. BuildPlugin end to end

### It is NOT host-project-free — it synthesizes one

- `CreateHostProject` writes a minimal `HostProject.uproject` (`{"FileVersion":3,"Plugins":[{"Name":"VaCuus","Enabled":true}]}`) under `<Package>/HostProject/` (BPC:124-125, 177-183), then **copies the ENTIRE plugin tree** into `HostProject/Plugins/VaCuus` via `ThreadedCopyFiles` and deletes only `Intermediate` from the copy (BPC:186-188). For us that copy includes `.git`, `.beads`, `docs/`, stale `Binaries/`, and `Web/node_modules` (~esbuild ELF) — filtered out of the final package later, but paid for in copy time/disk. Run the dry-run from a clean clone without `node_modules`.
- The host project is deleted afterwards unless `-NoDeleteHostProject` (BPC:165-168) — that flag is the dry-run debugging tool.
- Guards: `-Package` dir must not contain the plugin and must be outside the engine tree (BPC:87-94); it is wiped first (BPC:96-100). A placeholder `Config/FilterPlugin.ini` is auto-created if absent (BPC:107-121) — ours exists.

### What it compiles

- **Host platforms** (default: the current host, BPC:548-553): target `UnrealEditor`, config `Development` (BPC:297).
- **Target platforms**: target `UnrealGame`, configs `Development` **and `Shipping`** (BPC:309-310). Shipping-game is the config the daily editor loop never touches — it is the likeliest novel failure (only the M5 packaged gate has exercised it).
- Per-module gate is `ModuleDescriptor.IsCompiledInConfiguration` (BPC:381); `Type: Editor` modules compile **only** for `TargetType.Editor` (UnrealBuildTool/Configuration/Descriptors/ModuleDescriptor.cs:798-800). So **VaCuusEditor's `UnrealEd`/`DirectoryWatcher` deps (VaCuusEditor.Build.cs:24,37) cannot fail the game targets** — the module is simply absent from them. All four Runtime modules compile in all three legs.
- UBT is invoked per leg as `-plugin=<uplugin> -noubtmakefiles -manifest=<xml> -nohotreload` (BPC:402); build products are harvested from the manifests (BPC:316-321).
- **Foreign-plugin compile environment** (the deltas vs. our project build):
  - plugin modules get `bPrecompile=true; bUsePrecompiled=false` (UEBuildTarget.cs:6855-6859);
  - for monolithic (game) targets: `bDisableLinking=true` and **`bUseSharedPCHs=false`** (UEBuildTarget.cs:1474-1481) — no executable is linked, object files + a `.precompiled` manifest are the product (UEBuildModuleCPP.cs:1193-1207);
  - manifest build products are pruned to files under the plugin dir (UEBuildTarget.cs:2255-2258), and makefile outputs pruned to the plugin's own modules (UEBuildTarget.cs:3181-3204).

### What lands in the package

Default filter (BPC:461-469): the `.uplugin` + every manifest build product by exact path + `/Binaries/ThirdParty/...` + `/Resources/...` + `/Content/...` + `/Intermediate/Build/.../Inc/...` (UHT headers) + `/Shaders/...` + `/Source/...`, minus `/Tests/...`. Then `Config/FilterPlugin.ini` rules (BPC:472, section read at FileFilter.cs:237-255; a leading `-` makes a rule an exclude, FileFilter.cs:127-134), then `FilterPlugin<Platform>.ini` per target platform (BPC:475-479), then excludes for `EpicInternal/CarefullyRedist/LimitedAccess/NotForLicensees/NoRedist` at any depth (BPC:482-485; names from UnrealBuildTool/System/RestrictedFolders.cs:188-208). [inference from FileFilter.cs:591, rule-number comparison in node search] the latest-added matching rule wins, so ini rules override the defaults.

A leading `/` anchors a pattern to the plugin root (FileFilter.cs:322-330) — so `/Tests/...` (BPC:469) excludes only a root-level `Tests/`; **our `Source/*/Private/Tests/` ships as source** (noise, not a violation).

Finally the packaged `.uplugin` is rewritten: `bEnabledByDefault` cleared, `bInstalled=true`, `EngineVersion="5.8.0"` unless `-Unversioned` (BPC:434-445).

### Linux-only reality (`-TargetPlatforms=Linux`)

Default target set = registered code-project platforms; Mac is removed on a non-Mac host (BPC:500-503), **Win64 is removed on a non-Windows host** (BPC:505-508), Linux/LinuxArm64 survive on Linux (BPC:510-517). `-TargetPlatforms` then intersects — a platform not already in the list is silently dropped, never an error (BPC:520-537). Same logic for `-HostPlatforms` with a warning (BPC:558-589). **So a Linux run builds Linux editor-Dev + Linux game-Dev + game-Shipping; nothing Win64 is attempted, required, or warned about.** A "passes on 3 engine versions × Win64" claim can only be made on owner hardware.

### What fails it

Compile errors in any of the three legs (Shipping-game above all: `bBuildDeveloperTools=false` there, BPC:375); a module named in the `.uplugin` with no source; a dependency plugin outside `Engine/Plugins` not passed via `-Dependencies` (BPC:62-72). Recommended hygiene flag: `-StrictIncludes` (no PCH, no unity — BPC:133-137), since Fab recompiles on compilers we do not run.

### Plugin-specific risk spots

| Spot | Finding |
|---|---|
| Vendored C (quickjs relays) | Effectively **no env delta**: VaCuusJs.Build.cs:13-26 already sets `NoPCHs`, `bUseUnity=false`, `CStandard=C11`, `bWarningsAsErrors=false` — the foreign-plugin "shared PCHs off" change (UEBuildTarget.cs:1480) is a no-op for it. Same preamble in VaCuusRml.Build.cs:11-17. Residual: Win64 quickjs needs the `/experimental:c11atomics` decision (VaCuusJs.Build.cs:47-50) — owner hardware. |
| `Shaders/` | Rides via `/Shaders/...` (BPC:467). Consumer mapping needs **no** BuildPlugin help: `FVaCuusRenderModule::StartupModule` resolves `IPluginManager::FindPlugin("VaCuus")->GetBaseDir()/Shaders` and calls `AddShaderSourceDirectoryMapping("/Plugin/VaCuus", …)` (VaCuusRender.cpp:2112-2115), at `PostConfigInit` (VaCuus.uplugin, VaCuusRender entry) — install-location independent, before global shader compilation. |
| `Content/` | Rides via `/Content/...` (BPC:465) — DevUI loose files, spike `.uasset`s, fonts all ship. |
| `Web/` | **Not in any default rule** (BPC:461-469 has no `/Web/`) — today's package has NO Web at all, while spec §2 mandates Web ships source-only (arch spec:50-53). M6 must add to `[FilterPlugin]`: `/Web/...` then `-/Web/node_modules/...` (exclusion syntax FileFilter.cs:127-134). Without the exclude, the esbuild **ELF executable** ships (verified: `file` says `ELF 64-bit LSB executable` at `Web/node_modules/@esbuild/linux-x64/bin/esbuild`). |
| Committed `.uasset` = LFS | All Content binaries are LFS-tracked (`git lfs ls-files`: 7 uassets + ttf + png + icon); this clone is smudged (`file` reports real `Unreal Engine package`). A checkout without LFS would package ~130-byte pointer files **silently** — the dry-run must scan for the pointer signature (below). |
| Stale `Binaries/Linux/*.so` | Copied into HostProject but **not packaged**: only exact-path manifest build products plus `/Binaries/ThirdParty/...` are included — no blanket `/Binaries/...` rule (BPC:461-463). |

## 2. The no-executables scan

Verified rules on record: source-shipping mandate 4.3.6.1.a and `.exe`/`.msi` ban 4.3.6.1.e; Web source-only, no node_modules, no tool binaries; the M6 dry-run includes a no-executables scan of the zip (arch spec:48-56, :402, :441-442). Fab's full current rulebook is not on disk — anything past §2's recorded rules is a dry-run confirmation item (spec:450).

**Scan shape** (run over the BuildPlugin `-Package` output before zipping):

```bash
PKG=<package-dir>
find "$PKG" -type f \( -name '*.exe' -o -name '*.msi' -o -name '*.dll' -o -name '*.dylib' -o -name '*.so' -o -name '*.bat' -o -name '*.cmd' \)   # extension blacklist; .so legitimate only under Binaries/ThirdParty, which we don't use
find "$PKG" -type f -perm /111                                     # executable bits
grep -rlI '^#!' "$PKG"                                             # shebang scripts
find "$PKG" -type d -name node_modules                             # must be empty
grep -rl 'git-lfs.github.com/spec/v1' "$PKG"                       # LFS pointers that never smudged — must be empty
```

**Today's hits** (inventory ran: `find … -name '*.sh' -o -name '*.py' -o -perm -u+x`):

- **Would ride today's package** (under `/Source/...`): `Source/ThirdParty/RmlUi/Backends/RmlUi_SDL_GPU/compile_shaders.py`, `Source/ThirdParty/RmlUi/Backends/RmlUi_Vulkan/compile_shaders.py`, `Source/VaCuusJs/gen_relays.sh`, `Source/VaCuusRml/gen_relays.sh` (both `#!/usr/bin/env bash`, exec bit set). Text scripts, not `.exe`/`.msi` — review friction rather than a recorded-rule violation. Options: `-/Source/ThirdParty/RmlUi/Backends/...` in FilterPlugin.ini (the Backends dir is upstream demo backends we never compile — [inference] from the module's include list, VaCuusRml.Build.cs:49-53, which names only Source/Core paths), and either exclude `gen_relays.sh` or keep it (it is the documented re-vendor procedure) with the exec bit stripped.
- **Would ride once `/Web/...` is added**: `Web/node_modules/**/esbuild` (real ELF — hard stop), `Web/packages/cli/bin/vacuus.mjs` (`#!/usr/bin/env node` + exec bit — content is fine, bit/shebang trips a naive scan; whitelist `.mjs` shebangs or drop the bit).
- **Repo-only, never packaged** (but present if anyone zips the tree by hand): `.beads/hooks/*` (executable), `docs/research/proofs/m5-t6-worldspace/author_world_panel_material.py`, editor `.so`s under `Binaries/`, link scripts under `Intermediate/`.

## 3. 5.6/5.7 shims

**Ground truth: zero version guards exist today** — grep for `ENGINE_MINOR_VERSION|ENGINE_MAJOR_VERSION|UE_VERSION|VERSION_NEWER_THAN|VERSION_OLDER_THAN` over all five modules returns no files.

Engine-API surface ranked by drift likelihood (each verified against the 5.8 header we compile against; 5.6/5.7 forms are unverifiable on this machine and are so marked):

1. **`ICustomSlateElement::Draw_RenderThread(FRDGBuilder&, const FDrawPassInputs&)`** — current API at RenderingCommon.h:937-958; we override exactly it (VaCuusSlateElement.h:73). The header still carries `UE_DEPRECATED(5.4) DrawRenderThread(FRHICommandListImmediate&, const void*)` and `UE_DEPRECATED(5.5) FSlateCustomDrawParams` (RenderingCommon.h:963-975), so the RDG shape predates 5.6 — but `FDrawPassInputs` is a plain struct whose fields (`UsedSlatePostBuffers`, `bOutputIsHDRDisplay`, RenderingCommon.h:945-955) can differ per version with only a compile error to show it. Our glass path reads them (VaCuusSlateElement.h:62). **Top shim candidate.**
2. **`FSlateApplication::RegisterInputPreProcessor(processor, const FInputPreprocessorRegistrationKey&)`** — SlateApplication.h:1568 (struct at :222), called at VaCuusWorldInputProcessor.cpp:134. Four overloads coexist (:1544-1568); the Key struct is the newest of them. [inference] likeliest 5.6 gap; the fallback to the plain overload (:1544) is a one-line shim.
3. **`FMaterialShader::SetParameters(FRHIBatchedShaderParameters&, proxy, material, const FSceneInterface*)`** — MaterialShader.h:88-92, called at VaCuusMaterialDraw.h:133; plus `RHICmdList.GetScratchShaderParameters()` (RHICommandList.h:996) / `SetBatchedShaderParameters` (VaCuusMaterialDraw.cpp:279-293). No deprecation churn visible near them in 5.8; [inference] batched-parameters API predates 5.6, but the `FSceneInterface*` overload's presence there is unconfirmed.
4. **`FSlateDrawElement::MakeCustom`** — declared in **DrawElementTypes.h:303** (not DrawElements.h — grep of DrawElements.h finds no MakeCustom), called at SVaCuusWidget.cpp:299. [inference] header split is recent; 5.6/5.7 may want the old include — include-path shim.
5. **RDG utils** — `AddDrawScreenPass`/`AddCopyTexturePass`/`RegisterExternalTexture`/`GraphBuilder.AddPass` (VaCuusSlateElement.cpp:120,170,201,273,407,425). Stable family; `FScreenPassViewInfo` ctor drift possible.
6. **Stable by construction:** `ITextInputMethodContext/System` (4.x-era, VaCuusTextInput.h:14,32); `FScriptArrayHelper` (Core); `IInputProcessor` virtuals; quickjs/RmlUi are vendored. The SlateRHIRenderingPolicy/UnrealEngine.cpp/EditorEngine.cpp lines we **cite in comments but never call** (e.g. VaCuusRender.cpp:1983, VaCuusEditor.Build.cs:23) can only go stale as prose — no shim, but the M6 docs pass should re-pin those line numbers per engine version.

**Honest strategy.** Do not write speculative `#if` guards now: with no 5.6/5.7 headers on disk every guard would assert an API shape nobody here has read — precisely the comment-rot failure the project conventions forbid. What M6 does on this machine: (a) create an empty `VaCuusEngineCompat.h` seam and route the four hotspot call sites through it (they already live in exactly four files); (b) write the owner-hardware checklist as a named experiment — **Experiment SHIM-1:** per engine (5.6.x, 5.7.x): `RunUAT BuildPlugin -Plugin=…/VaCuus.uplugin -Package=<out> -TargetPlatforms=Win64 -StrictIncludes`; every compile break lands as a `#if UE_VERSION_OLDER_THAN(…)` branch in the compat header, then re-run all three engines. The M6 accept line requires exactly this (arch spec:439).

## 4. The buyer-docs inventory (every scattered "document this" note found)

1. **Style-set cooking**: a style set referenced only by path needs `+DirectoriesToAlwaysCook=(Path="/VaCuus/Spike")` in the host's DefaultGame.ini — the note is explicitly labeled "the M6 buyer-docs note" (VaCuusStyleSet.h:31-40).
2. **DevUI staging + the stale-receipt trap**: RuntimeDependencies wildcards freeze into the .target receipt at makefile generation; adding a document does not invalidate the makefile — touch `VaCuus.Build.cs` or `-Rebuild` (VaCuus.Build.cs:83-109); the extension list must track the live-reload watcher's (:110-114); Tests fixtures ride along; **all replaced by UVaCuusBundle in M6** (VaCuus.Build.cs:115-116; arch spec:330-334).
3. **Shipping ignition**: `-ExecCmds` is compiled out of Shipping (comment citing UnrealEngine.cpp:2543, at VaCuusRender.cpp:1981-1992); `-VaCuusM5Demo` is the surviving opt-in, armed on first map load, one screenshot at t+8s (VaCuusRender.cpp:1994-2008); the one Display-verbosity boot line is the observability contract (VaCuusRender.cpp:942-947).
4. **Shipping logging**: host must set `bUseLoggingInShipping=true` + `TargetBuildEnvironment.Unique`; Verbose silent, Log survives; Shipping's Saved tree moves to `~/.config/Epic/<Project>/Saved/` (docs/research/proofs/m5-t9-acceptance/README.md:38-52; m5-webdx spec:343).
5. **Web workspace**: `npm install` once per checkout, exact pins; the TSX-error trap (failed dev build writes no bundle → engine silently keeps last good UI); committed bundles carry provenance JSON; the preact MIT banner is a license requirement, not style (Web/README.md:9-21, :25, :37-40).
6. **DevUI ordered roots (D19)**, plugin content first (VaCuusFileInterface.cpp:63, VaCuusContentPaths.cpp:22).
7. **Live reload is editor-only**: DirectoryWatcher is pumped only by UEditorEngine::Tick; a packaged game never receives an event (VaCuusEditor.Build.cs:19-24).
8. **Win64 IME/TSF hand-off** is an M6 matrix item; Linux has no ITextInputMethodSystem at all (VaCuusTextEntryTest.cpp:288-300).
9. **quickjs vendoring**: the `nm -D … | grep ' JS_'`-must-be-empty export check and re-vendor procedure (VaCuusJs.Build.cs:52-56); Win64 c11atomics revisit (:47-50).
10. **Open items to close at the dry-run**: `CanContainContent` stays true — confirm (arch spec:447); third-party disclosure form mechanics (arch spec:53-54, :450).

**Handoff checklist (owner hardware, Linux cannot do these):** SHIM-1 per 5.6/5.7 (§3); `BuildPlugin -TargetPlatforms=Win64` on Windows + macOS host for Mac (BPC:500-508 make both host-locked); Win64 D3D12 / macOS Metal matrix incl. TSF IME; Win64 quickjs c11atomics decision; §11 gates on cooked Win64 Shipping from the bundle (arch spec:439-442).
