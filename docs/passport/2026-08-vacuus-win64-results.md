# Win64/D3D12 pass — what was executed, what it found, what it could not reach

**What this is.** The first execution of VaCuus on Windows and Direct3D 12, ever. It ran on
2026-08-03 over SSH from the Linux dev box onto the owner's desktop. It fills what it honestly
can of the **Win64 D3D12** column of `2026-08-vacuus-manual-matrix.md` and records, by name and
with a reason, everything it could not run — of which there is more than on the macOS leg, for
one specific reason given in §7.

**The commit under test:** `fc38ced67db11eb1d30145ad16676ef07c8ce367` (branch `master`, "docs:
the macOS/Metal pass, executed"). **Five fixes were made during the pass** and are committed on
top of it; each is named below with its SHA. Note that `master` also gained `094f5a8` and
`b779983` from another session while this ran — **neither was in the tree that was tested**, and
`094f5a8` ("the harness stops failing on the engine's own Mac IME error") touches the automation
harness, so the suite numbers below are from a tree *without* it.

**The machine and the engine.**

| | |
|---|---|
| Host | `DESKTOP-590NICV` — Windows 11 10.0.26200.8875, AMD64, 8 physical / 16 logical cores, 15.9 GB RAM |
| GPU | **NVIDIA GeForce RTX 2080 SUPER**, 7987 MB VRAM, driver 591.86 (2026-01-20), DirectX Agility SDK runtime present |
| Toolchain | Visual Studio Community 2022 17.14.37516.0, MSVC 14.44.35207, Windows SDK 10.0.26100.0 |
| Engine | **UE 5.8.1 Launcher/Installed build** at `C:\Program Files\Epic Games\UE_5.8` — changelist **56057345**, `InstalledBuild.txt` present. Same changelist as the Linux and macOS passes, so the comparison is honest. |
| Host project | `C:\VaCuusWin64Test\VcHost` — a raw copy of `Engine\Templates\TP_ThirdPerson` (C++), stock module and target names kept (`TP_ThirdPerson`, target `TP_ThirdPersonEditor`); only the `.uproject` renamed. Plugin installed as **real files** at `Plugins\VaCuus`. The three plugin-scoped config lines from VcHost's own `DefaultGame.ini`/`DefaultEditor.ini` were mirrored in. |

**Feature level that bound: `SM6`, on real hardware.** Not inferred — `LogRHI: RHI D3D12 with
Feature Level SM6 is supported and will be used`, `LogD3D12RHI: Display: Creating D3D12 RHI with
Max Feature Level SM6`, on an adapter reporting `Max supported Feature Level 12_2, shader model
6.7, binding tier 3, wave ops supported, atomic64 supported`. **This is the first time VaCuus has
run at SM6 anywhere** — the Mac leg was `SP_METAL_SM5` throughout because M1 Pro is
`GPUFamilyApple7`. It is also, per §7, the venue where VaCuus's own draw does not survive.

---

## 1. How the tree got there, and how that was verified

`git-lfs` is installed here, but the transfer was still an rsync-equivalent from the Linux tree
(tar over `scp`) rather than a clone, for the same reason the macOS pass gave: it sidesteps the
unsmudged-pointer failure class entirely. Excluded: `.git`, `.beads`, `Binaries`, `Intermediate`,
`node_modules`, `Saved`, `DerivedDataCache`.

Verified on the Windows side after extraction:

- `git lfs ls-files` on the Linux side lists **36** paths, 35 marked `*` and exactly one `-`; the
  single `-` is `Tools/scan-fixture/planted_lfs_pointer.txt`, the deliberate fixture.
- **All 36 files were MD5-compared file-by-file across the two machines: 36 checked, 0 mismatches,
  0 missing.**
- `Content/DevUI/fonts/LatoLatin-Regular.ttf` is 148,540 bytes beginning `00 01 00 00` — the
  TrueType magic, not ASCII.
- Sampled `.uasset` files begin `c1 83 2a 9e` (`0x9E2A83C1`, the UE package magic).
- A recursive scan for `git-lfs.github.com` over every file under 500 bytes hit **exactly one
  path**: `Tools\scan-fixture\planted_lfs_pointer.txt`. Nothing under `Content/`.

---

## 2. The blocker that had nothing to do with VaCuus, and the shim

**No editor target could produce a makefile on this machine, with or without the plugin.**

```
Unable to instantiate module 'SwarmInterface': Could not find NetFxSDK install dir; this will
prevent SwarmInterface from installing.  Install a version of .NET Framework SDK at 4.6.0 or higher.
(referenced via TP_ThirdPersonEditor -> Launch.Build.cs -> ... -> UnrealEd.Build.cs)
```

`SwarmInterface.Build.cs:29-34` throws outright when `Target.WindowsPlatform.NetFxSdkDir` is null
on Win64, and `UnrealEd` depends on it. **The control that settles authorship: a stock
`TP_ThirdPerson` project with no VaCuus plugin at all fails identically**, in 3.4 seconds. This is
a machine prerequisite gap, not a plugin defect.

The .NET Framework SDK is genuinely absent here, checked four ways (all with correct quoting —
the first attempt was void because the cmd shell ate the quotes around paths with spaces):
no `C:\Program Files (x86)\Windows Kits\NETFXSDK`, no `Microsoft SDKs\NETFXSDK` registry key in
either registry view, no `.NETFramework` reference-assemblies directory, and **no `mscoree.h` or
`mscoree.lib` anywhere on the volume**. Only the .NET *runtime* (v4.0.30319) is installed.

**The owner's Visual Studio was deliberately not modified.** Instead the pass used a sandboxed
AutoSDK shim: `MicrosoftPlatformSDK.cs:357-367` probes
`$(UE_SDKS_ROOT)/HostWin64/Win64/Windows Kits/NETFXSDK/<4.6.2|4.6.1|4.6>/Include/um/mscoree.h`
(`UEBuildPlatformSDK.cs:1150` supplies the `HostWin64` part), so that path was created under
`C:\VaCuusWin64Test\autosdk` with a stub header, and `UE_SDKS_ROOT` was set **per build process
only** — never in the user or machine environment.

**The shim is inert, and that is demonstrated rather than asserted.** This Installed engine ships
`Engine\Binaries\Win64\UnrealEditor-SwarmInterface.dll` (102,328 bytes) prebuilt, so UBT never
runs a compile action for that module and the stub's include/lib paths are never read. Proof:
the DLL's `LastWriteTimeUtc` is `2026-08-03T11:13:32.8254914Z` **both** in the baseline taken
before any build and after all six builds, and no build log contains SwarmInterface as an action —
only the original failure line from before the shim existed. The stub header is also `#error`-ed,
so had it ever actually been included the build would have failed loudly rather than quietly.

**Owner action:** install the .NET Framework SDK properly (VS component
`Microsoft.Net.Component.4.6.2.SDK` or `4.8.SDK`) and the shim becomes unnecessary. Everything
this pass created lives under `C:\VaCuusWin64Test` and deletes with it.

> **DONE 2026-08-04.** The owner installed it; `NETFXSDK\4.6.2` is real, editor targets generate
> makefiles with no `UE_SDKS_ROOT` at all, and **the shim has been deleted**. Everything in this
> section is history — do not re-create the shim. The verification, including the cached-makefile
> trap that nearly produced a false PASS, is in §12.5. Bead `akj.10.9` closed.

---

## 3. Build findings

**Six findings, five fixed, and not one of them was a missing include.** All six are invisible on
clang by construction; every one is a first-Windows-contact defect that had been in the tree for
milestones.

A structural fact underpins three of them and is worth stating on its own, because it silently
falsified two existing `Build.cs` comments: **`UEBuildModuleCPP.cs:2669` is
`Result.bWarningsAsErrors |= Rules.bWarningsAsErrors`.** It is an OR. A module can turn
warnings-as-errors ON and can *never* turn it OFF, so `bWarningsAsErrors = false` in
`VaCuusRml.Build.cs:14` and `VaCuusJs.Build.cs:17` has been inert since it was written. MSVC ran
`/W4 /WX` over vendored RmlUi and quickjs alike. (`/WX` itself comes from
`VCToolChain.cs:1667-1669`.) Also worth knowing for the future: `AppendCLArguments_CompileWarnings`
consults `CppCompileWarningSettings` **only for clang** (`VCToolChain.cs:1672-1685`), so every
named `WarningLevel` knob in that struct is a no-op under MSVC.

### B1 — 106 × `C4800` from vendored headers. Fixed in `42fbdc2`.

`RmlUi/Core/DataVariable.h:25` and `DataModelHandle.h:22` (`explicit operator bool() const
{ return definition; }`, a pointer-to-bool conversion), 43 TUs each, plus `quickjs.h:856`
(`JS_ToBoolean` feeding an `int` into a `bool` parameter), 20 TUs.

Two different fixes because the two cases differ. RmlUi's `Include/` is now a **system** include
path: `VCToolChain.cs:281-289` turns that into `/external:I` plus `/external:W0` on MSVC and
`ClangToolChain.cs:649` into `-isystem` elsewhere, so nothing is emitted inside those headers for
`/WX` to promote, while our own code keeps every warning. quickjs could not use the same trick —
`ModuleRules.cs:1281` offers only `PublicSystemIncludePaths` and quickjs's path is deliberately
private so that a wrong include fails at compile time — so there is now one wrapper header,
`VaCuusQuickJs.h`, that push/disable/pops the single warning, and all seven including TUs go
through it.

### B2 — 9 × `C4702` from vendored `quickjs.c`. Fixed in `42fbdc2`.

Unreachable code at `quickjs.c:665` (`strv`), `:3074` (`JS_AtomGetKind`), `:23606`
(`find_var_htab`), `:52215` (`get_first_weak_ref`). These are MSVC **back-end** warnings, so no
include-path flag reaches them — the pragma has to be in the TU. It went into the four relay `.c`
files and into `gen_relays.sh`, so a re-vendor keeps it. The vendored `.c` is not edited and our
own C keeps its warnings.

### B3 — 23 × `C4005`, and a module built at the wrong Windows API level. Fixed in `42fbdc2`.

`VaCuusJs.Build.cs` forced `_WIN32_WINNT=0x0601`. UBT already writes `0x0A00` into the module's
generated `Definitions.h:74`; ours landed at `:100` and won. So VaCuusJs was compiling **the
engine's headers at a Windows 7 API level while every other module in the build saw Windows 10** —
a genuine ODR/configuration hazard that only announced itself as a macro-redefinition warning.
Upstream wanted a floor and the engine's value clears it, so the line is gone.

### B4 — 21 × `C2280`: `FVaCuusModelLayout`'s copy-assignment was always ill-formed. Fixed in `e767f4a`.

The header has always *claimed* the layout is move-only. Nothing enforced it: the copy constructor
was implicitly deleted, but the copy **assignment** operator was implicitly declared and viable,
because `TArray`'s copy-assignment is an ordinary member of a class template — it exists for every
element type and only fails when its body is instantiated.

clang never instantiated it. `Layout = FVaCuusModelLayout(...)`
(`VaCuusDataArrayTest.cpp:91`, `VaCuusDataVariableTest.cpp:127`) binds an rvalue, move assignment
wins overload resolution, and the copy branch is never odr-used. MSVC defines the implicit
copy-assignment eagerly, instantiated it, and got `TArray<FVaCuusModelArrayDesc>::operator=(const
TArray&)` → `CopyToEmpty` → `MemoryOps.h:121` placement copy-construct → deleted function.

The copy pair is now deleted and the move pair declared out of line. That this cannot break the
other platforms is not a hope: a copy ever really performed would have failed to compile there
too. **Linux was rebuilt green afterwards.** A first attempt put `VACUUS_API` on the new members
and earned 44 × `C2487` — the class is already `class VACUUS_API FVaCuusModelLayout` and MSVC
rejects the repetition where clang accepts it silently.

### B5 — `LNK2019`: RmlUi puts a dll-interface on header-only class templates. Fixed in `38e1318` (vendored patch #2).

```
VaCuusJsDom.cpp.obj : error LNK2019: unresolved external symbol "__declspec(dllimport) public:
__cdecl Rml::ObserverPtr<class Rml::Element>::ObserverPtr<class Rml::Element>(void)"
referenced in function "FVaCuusJsViewContext::WrapElement(class Rml::Element *)"
```

`Header.h:8-21` makes `RMLUICORE_API` dllexport while building the library and dllimport for
consumers. On a **class template** that is a category error: dllimport tells MSVC every
instantiation's members live in another DLL, so a consumer stops instantiating locally and emits
`__imp_` references that resolve only if the library happened to instantiate and export that exact
specialization. RmlUi's Core uses `Rml::Element` constantly but never default-constructs an
`ObserverPtr<Element>`, so that one constructor was never exported.

Four templates carry the macro and all four have the same latent defect — `ObserverPtr`,
`EnableObserverPtr` (`ObserverPtr.h:42,113`), `UniqueRenderResource` (`:14`), `Releaser`
(`Traits.h:35`). **All four are entirely header-inline** — no `.cpp` in `Source/` defines a member
of any of them and there is no explicit instantiation of any of them, both checked — so none needs
a dll-interface at all. The non-template API their bodies call (`ObserverPtrBlock`,
`AllocateObserverPtrBlock`, `DeallocateObserverPtrBlockIfEmpty`, `ObserverPtr.h:10-15`) keeps the
macro and stays exported, so the observer **state** remains single-instance in `VaCuusRml.dll`;
only inline code is duplicated per module, which is the normal model for a header-only template.

Seen both ways, which is this repository's standard: reverted, VaCuusJs fails to link; applied,
all five DLLs link. Recorded in `VENDORED_TAG.txt` with that verification. A **monolithic** Win64
target cannot exercise this — `RMLUI_STATIC_LIB` makes the macro empty there — which is worth
knowing because it means the game-target build would not have caught it.

### B6 — the UTF-8 test literal was not the bytes it claimed. Fixed in `3b6bb44`.

Found by the suite, not the build; see §5.

### What compiled clean, which is itself a data point

- The final build is **`Result: Succeeded`, zero errors, zero warnings.** Six builds were needed to
  get there; the last full one took 340 s.
- All five modules link: `UnrealEditor-{VaCuus,VaCuusRml,VaCuusJs,VaCuusRender,VaCuusEditor}.dll`.
- The vendored **quickjs C compiled under MSVC with no diagnostic except B2's unreachable-code**.

---

## 4. The `/experimental:c11atomics` question, answered — and the answer is not "no flag needed"

`VaCuusJs.Build.cs:44-48` has held this open since M4. **The flag is not needed, but not because
MSVC accepted the atomics.**

`quickjs.c:73` guards the entire feature:

```c
#if !defined(__TINYC__) && !defined(EMSCRIPTEN) && !defined(__wasi__) && !__STDC_NO_ATOMICS__ && !defined(__DJGPP)
#include "quickjs-c-atomics.h"
#define CONFIG_ATOMICS
#endif
```

MSVC defines `__STDC_NO_ATOMICS__` unless `/experimental:c11atomics` is passed, so the guard is
false, `CONFIG_ATOMICS` is never defined, and `quickjs.c:60921-61409` — the `_Atomic` /
`atomic_fetch_*` / `atomic_load` code that would have demanded the flag — is preprocessed away.
**It compiles because the feature is gone.**

The consequence is a shipped platform difference, not a build detail: **the JavaScript `Atomics`
global exists on Linux and macOS and does not exist on Win64.** Adding the flag for MSVC would
restore parity. It is written into the Build.cs comment as an owner call rather than silently
defaulted, because it opts a shipped module into an explicitly *experimental* MSVC switch to enable
a builtin that no VaCuus document or test currently uses.

### Confirmed at runtime, 2026-08-03 evening — and the split is narrower and sharper than predicted

The check this section could not reach has now run. A temporary `console.log` probe in
`RefHud/refhud_logic.js` (inserted, run, reverted; the tree is clean) on a live console-session
`vacuus.RefHud` build printed:

```
LogVaCuusJS: Display: ATOMICS-PROBE: typeof Atomics=undefined typeof SharedArrayBuffer=function typeof WeakRef=function
```

**`Atomics` is `undefined` on Win64 — the preprocessor inference was right.** But
`SharedArrayBuffer` is **`function`**, i.e. present. That is the part no one predicted, and it makes
the buyer-facing hazard worse rather than better: the idiomatic feature test for this capability is
`typeof SharedArrayBuffer !== 'undefined'`, and on Win64 that test **passes while `Atomics` is still
missing**. A script that gates on `SharedArrayBuffer` and then calls `Atomics.load` gets a
`TypeError` on Windows and works everywhere else. Whatever the owner decides about the flag, that
sentence belongs in the buyer docs, because feature-detecting correctly here is not obvious.

(The `Linux`/`macOS` half of the claim remains the preprocessor inference plus those platforms'
absence of `__STDC_NO_ATOMICS__` — no probe was run there in this pass.)

---

## 5. The automation suite

Command: `UnrealEditor-Cmd.exe <proj> -ExecCmds="Automation RunTests VaCuus, Quit," -unattended
-nullrhi -nosplash`. Counts read from the project's own `Saved\Logs\VcHost.log`, never stdout.
`Quit` never fired in either run; both processes were ended by `taskkill /PID <pid>` on a PID this
pass started itself.

| Run | Tree | Result |
|---|---|---|
| 1 | `fc38ced` + B1–B5 | `Automation Test Queue Empty 197 tests performed.` — **195 Success, 2 Fail** |
| 2 | + B6 (`3b6bb44`) | `Automation Test Queue Empty 197 tests performed.` — **196 Success, 1 Fail** |

**197 tests selected and performed — exactly the Linux and macOS baseline.** No test failed to
register and no Win64-only test appeared or vanished. The four `-nullrhi` self-skips behaved as
designed and are named in the log: `VaCuus.Render.Composite.LinearOutputGPU`,
`VaCuus.Render.Upload.AsyncPayload`, `VaCuus.Render.Upload.Cost`, `VaCuus.World.MipContentGPU`.

**The key-map count holds.** `VaCuusInputMap.cpp:209`'s `ensureMsgf(Map.Num() == 111)` — the
observable the macOS Backspace defect forced into existence — **did not fire on Windows**. So no
`FKey` in `BuildKeyMap` collapses onto another here, and the macOS fix's guard is confirmed on a
second platform. (`EKeys::Platform_Delete` is `EKeys::Delete` generically per
`GenericPlatformInput.h:37-40`, and it is no longer in the map at all.)

### `VaCuus.Model.Binding` — a real defect, found and fixed. `3b6bb44`.

```
Expected 'an FUtf8String survives as UTF-8' to be "café", but it was "cafÃ©".
[VaCuusDataVariableTest.cpp(516)]
```

The source wrote `UTF8TEXT("caf\xC3\xA9")`. That is **not portable inside a `u8""` literal**: MSVC
treats a `\x` escape there as a *code point* and re-encodes it as UTF-8 rather than emitting it as
a raw code unit. MSVC says so itself — `warning C5321`, "nonstandard extension used: encoding
'\x..' as a multi-byte utf-8 character", which this build raised twice and which is now gone. So
`\xC3\xA9` became the UTF-8 encodings of U+00C3 and U+00A9: four bytes where two were meant. clang
takes the same escapes as raw code units, which is why Linux and macOS always passed.

The literal is now an explicit `UTF8CHAR` array, which has one meaning on every compiler — and is
what a test asserting *byte* preservation should have used from the start. **Exactly one
occurrence of this pattern exists in the plugin and it is this test; no production code writes a
`\x` escape into a `u8` literal**, so nothing shipped was affected. Seen to fail and then to pass
with nothing else changed.

### `VaCuus.Render.Texture.UnsizedDrain` — over-tight assertion, NOT fixed here. Owner call.

```
Expected 'The sized view recorded exactly one frame' to be 1, but it was 2.
[VaCuusUnsizedDrainTest.cpp(264)]
```

**It failed identically in both runs, so it is deterministic on this machine, not jitter.**

Every substantive assertion of the bead passed in the same run: an unsized view records **0**
frames, its finished decode is drained anyway, and the first sized frame publishes **exactly one**
buffer, at the right size, carrying the real payload rather than the 1×1 placeholder. What failed
is the final count.

The mechanism explains it. `RunFrames` (`VaCuusUnsizedDrainTest.cpp:91-104`) calls
`UIThread.Trigger()` then `WaitForFrameCount(Before + 1, 5.0)` — it waits for the count to reach
**at least** `Before + 1`. It is a floor, not an exact pulse. The assertion at `:263` reads it as
exact. On a 16-core Windows box the UI thread gets two frames in before the waiter observes;
Linux and macOS happen to land on one. This is the same family as CLAUDE.md's recorded hazard that
`vacuus.M1HUD.AutoShot N` fires after `max(N,3)` *recorded* frames because the counter is a floor.

**Recommended fix, deliberately not applied**: assert `>= 1` recorded frames while keeping the
published-buffer count exact at 1, since the published count is the user-visible property and the
one the bead is about. Not applied here for the same reason the macOS pass withheld its two-line
harness fix: it perturbs a baseline this pass cannot re-measure from the far side. Verify on Linux,
then land. Note that the extra recorded frame was correctly **withheld** by the publish gate — no
extra work crossed to the render thread.

---

## 6. The `dumpbin` export gate — the Win64 twin of `Tools/api_export_check.sh`

`Tools/api_export_check.sh` is ELF-only and says so; bead `dgl` is the reason it exists at all.
The Win64 twin was written for this pass and run against the built DLLs. **It passes, including
its own self-test.**

```
== supported surface (must be reachable from a buyer's C++) ==
  ok    VaCuusRender/UVaCuusWidget: 27 exported member references
  ok    VaCuusRender/UVaCuusWorldComponent: 53 exported member references
  ok    VaCuus/UVaCuusView: 66 exported member references
  ok    VaCuus/UVaCuusSubsystem: 39 exported member references
  ok    VaCuus/UVaCuusStyleSet: 14 exported member references
== internals (must stay unreachable) ==
  ok    VaCuusRender/FVaCuusRmlDocumentHost: 0 exported members
== self-test: absent class correctly reports 0 (the FAIL path works) ==
  VaCuus: 385 exported symbols       VaCuusRender: 143
  VaCuusJs: 2                        VaCuusRml: 1746        VaCuusEditor: 2
== quickjs containment ==
  exported JS_* symbols: 0 (expected 0)
RESULT: clean -- the supported C++ surface survives binary delivery on Win64.
```

The mechanism differs from Linux and the check says so: on Linux `-fvisibility-ms-compat` lets a
class register while exporting zero linkable members; on Windows nothing is exported without the
module's `_API` macro, and UHT stamps that macro on the generated registrar regardless of what the
class declares — so the *failure shape* is identical and the check is meaningful. **Vendored patch
#1 (hidden quickjs visibility) is now confirmed on PE/COFF as well as ELF and Mach-O** — a third
binary format.

The script lives in this pass's scratch, not in `Tools/`: it should be committed as
`Tools/api_export_check_win64.ps1` once someone decides whether the repo wants PowerShell in
`Tools/`. **Owner call.**

---

## 7. The finding that stopped the matrix: VaCuus's own PSO does not create on D3D12

**This is the most important open item of the pass, and it is not fixed.**

Every `-game` session — windowed *and* offscreen, on every row attempted — dies within 20–40 s:

```
LogRHI: Error: Failed to create graphics pipeline, hashes: Vertex: B4A05A77C9004166,
        Pixel: 68E301A0F9A03DF2, Pipeline: 1D835C18C2A07F58.
LogRHI: Error: Vertex: <unknown>   Pixel: <unknown>
LogWindows: Error: appError called: Fatal error:
        [File:...\Runtime\RHI\Private\PipelineStateCache.cpp] [Line: 712]
        Shader compilation failures are Fatal.
```

**It is a PSO-creation failure, not an HLSL compile failure** — and that distinction is
load-bearing, because the engine's message says "shader compilation" and it is not one.
`LogShaderCompilers: Error` appears **zero** times in every log of this pass, and no VaCuus `.usf`
is named anywhere. The dumped pipeline description is what points at us:

```
InputLayout[0] = { "ATTRIBUTE", 0, 0x10, 0, 0,  0x0, 0 }
InputLayout[1] = { "ATTRIBUTE", 1, 0x1C, 0, 8,  0x0, 0 }
InputLayout[2] = { "ATTRIBUTE", 2, 0x10, 0, 12, 0x0, 0 }
NumRenderTargets = 1   RTVFormats[0] = 0x57   PrimitiveTopologyType = 0x3
```

Three attributes at byte offsets **0, 8, 12** is exactly the RmlUi vertex — position (2×float),
colour (4×byte), tex-coord (2×float), 20 bytes — which is VaCuus's recorder output and nothing
else in the frame.

**What is verified vs. inferred.** Verified: the fatal is real, reproducible on every row, kills
the process, and no shader-compiler error accompanies it. Verified: the engine itself renders
fine — see the screenshot below. **Inferred, not proven:** that the failing PSO is VaCuus's, from
the vertex layout matching. The shader names print as `<unknown>`, so the identification rests on
the input layout alone. **Root cause was not found**, and finding it is the first thing the next
Win64 session should do.

### The one screenshot, read by eye

`row7` (`vacuus.M5World 1, …`) produced a 1920×1080 PNG before the crash. What is actually in it:
the ThirdPerson template level under D3D12 — blue sky with scattered cumulus, the grey mannequin
seen from behind standing on a bright flat-green floor disc **casting a real shadow**, tan brick
perimeter walls, a grey paved ring, green blocks left, right and centre. Lighting, shadows and sky
are all correct; the flat green is the template's unbuilt material, normal for an uncooked project.

**There is no VaCuus UI anywhere in the frame** — no world-space quad, no HUD, no panel. That is
consistent with the same session's
`InputSmoke: no interactive rects; is the document published yet? Aborting`. So: the engine's
rendering is healthy on this GPU and VaCuus's own draw is what does not survive.

### The two things that did get through — and the first of them was an artifact of this pass's venue

> **CORRECTED 2026-08-03 evening. Read the correction under the box before quoting this finding.**

**Glass takes the DIRECT-SRV route on D3D12 — the first time that branch has ever executed.**
Logged in rows 8 and 11 before the crash:

```
Exp-GLASS-BACKBUFFER-SRV: glass samples the Slate output DIRECTLY as an SRV
  (texture ShaderResource=yes, vacuus.GlassBackbufferSRV=1)
```

The macOS pass recorded the direct-SRV route as **structurally unreachable on Metal** — the Slate
output texture there carries no `TexCreate_ShaderResource`, so no cvar value could select it, and
`VaCuusSlateElement.cpp:463-467`'s own comment predicted exactly that. On D3D12 the flag is
present, `bDirectSRV` resolves true, and the route runs. Linux/Vulkan and macOS/Metal have only
ever exercised the bounded-copy fallback. **The pixels were not seen** — the crash took the
session before a glass screenshot landed — so this is a route-selection result, not a visual one.

**CORRECTION.** That result is real but it is **not a Win64 result — it is a `-RenderOffscreen`
result**, and this pass had no choice but to run offscreen (§9). The A/B, same machine, same commit,
same command, only the offscreen switch differing:

| Venue | Log line |
|---|---|
| `-game -windowed` (real D3D12 swapchain) | `glass samples the Slate output through a bounded copy pass (texture ShaderResource=no, vacuus.GlassBackbufferSRV=1)` |
| `-game -RenderOffscreen` | `glass samples the Slate output DIRECTLY as an SRV (texture ShaderResource=yes, vacuus.GlassBackbufferSRV=1)` |

A real swapchain back buffer is not created shader-resource-able; an offscreen render target is. So
`bDirectSRV` resolving true here was a property of the **render target this pass was forced into**,
not of D3D12. **The shipped Win64 glass path is the bounded copy pass — the same route as Linux and
macOS** — and the "first on any platform" claim should be retired. What survives is narrower and
still worth having: the direct-SRV branch is *reachable* on D3D12 (it is structurally unreachable on
Metal), so it is not dead code on Windows, and anything that renders through an offscreen target
there will take it.

The pixels have since been seen, windowed, on a console session: rounded and square panels blur the
scene live, the control panel does not, and the idle economy holds at 0 published / 1,075 recorded
while Glass samples every frame. Matrix row 8 is a **PASS**.

`vacuus.M5World.InputSmoke` ran and failed `1 of 1` with "no interactive rects; is the document
published yet?" — a frame-0 timing artifact of firing the command before the view publishes (the
same class as the macOS `AutoShot 10`-before-materials-compile lesson), **not** an input finding.

---

## 8. The matrix, Win64 D3D12 column

> **SUPERSEDED 2026-08-03 evening.** Every `NOT RUN` below was downstream of the PSO fatal (§7) or
> of this pass's inability to open a window (§9). Both causes are gone — `b4f12e1` fixed the fatal,
> and a console session supplies the venue — and the column was re-run at commit `6b82e4a`:
> **12 of 15 rows PASS**, the three remaining ones are named with what each still needs, and none of
> them is "the session died". The live column is
> `2026-08-vacuus-manual-matrix.md`. The table below is kept as the record of what the SSH pass
> could see, not as the current state.

| # | Row | Win64 D3D12 (SSH pass, superseded) | Evidence / reason |
|---|-----|-------------|-------------------|
| 1 | Screen-space HUD composite | **NOT RUN** | session dies on the PSO fatal, §7 |
| 2 | Steady-state node count | **NOT RUN** | same; `vacuus.RefHud.Count` never reached |
| 3 | Mouse hit-test + hover | **NOT RUN** | same |
| 4 | Keyboard text entry | **NOT RUN** | same |
| 5 | IME composition | **NOT RUN** — and see below | the automated half did not move either, unlike on macOS |
| 6 | Gamepad spatial nav | **NOT RUN** | same as row 1 |
| 7 | World-space panel + raycast | **FAIL, venue** — `InputSmoke 1 of 1 FAILED`, "no interactive rects; is the document published yet?" — fired before publication, then the PSO fatal. Screenshot shows the level with no VaCuus quad. |
| 8 | Glass (backdrop blur) | **PARTIAL — route proven, pixels not seen.** `ShaderResource=yes`, direct-SRV path taken: a first on any platform. §7. |
| 9 | Gradient + builtin decorators | **NOT RUN** | same as row 1 |
| 10 | Material decorators | **NOT RUN** | same as row 1 |
| 11 | M5 acceptance demo | **NOT RUN** | reached the glass line, then died |
| 12 | PF_FloatRGBA composite permutation | **NOT RUN** | needs a surviving composite; also the editor-PIE leg this column was to own |
| 13 | Live reload (editor watcher) | **NOT RUN** | needs an interactive editor PIE session and a mid-run file edit; same reason as Linux and macOS, compounded by §9 |
| 14 | Demo-suite toggles + clean teardown | **NOT RUN** | every session ended in `appError`, not a clean exit, so there is no teardown tail to read |
| 15 | Shipping ignition flags | **NOT RUN** | packaging not attempted; §9 |

**Row 5 (IME) deserves its own note, because the one line this pass did produce is misleading if
quoted alone.** The `-nullrhi -unattended` suite logged:

> `IME: this platform exposes no ITextInputMethodSystem (GetTextInputMethodSystem() returned
> null), so composition is unavailable … Only FWindowsApplication and FMacApplication implement
> it; FLinuxApplication does not.`

That is **an artifact of the headless run**, not a statement about Windows: with `-nullrhi
-unattended` there is no real Slate application window, so `FSlateApplication::
GetTextInputMethodSystem()` returns null even on a platform that implements it. The macOS pass got
the `present` branch because it could run a real windowed session; this pass could not (§9). So
bead **`akj.6.19` did not move**, and the Windows TSF leg remains exactly as owed as it was.

**Confirmed 2026-08-03 evening.** The warning above was right. On a console session the other branch
is taken, and the matrix row-4 run logged both halves:

```
IME: platform ITextInputMethodSystem present; composition is available
TypeShot: IME bridge built=yes, platform system absent=no, registered=yes, context active=yes
```

So the Windows TSF path reaches `registered` and `context active`, and typing routes
`OnKeyChar -> ProcessTextInput` as designed. That is the **precondition** for row 5, not row 5:
`akj.6.19` is specifically about a stale-shadow window after a *document reload*, and exercising it
needs a real IME and a human at the keyboard. The bead moves from "blocked on venue" to "blocked on
a human", which is a materially different place to be.

---

## 9. What could not be run, and why — recorded, not dropped

> **Status 2026-08-03 evening**, after the console-session pass at commit `6b82e4a`:
>
> | Item below | Now |
> |---|---|
> | Every windowed row | **RUN.** `SESSIONNAME=Console` is the venue; bead `VaCuus-5fg` answered. |
> | All visual rows (PSO fatal) | **RUN.** Zero pipeline failures across ~12 sessions. |
> | Row 5 / bead `akj.6.19` | Precondition established (TSF `present`, `registered`, `context active`); still needs a human + IME. |
> | Row 15, disk literal, memory-mapped bundle | **STILL OWED** — all three need the Win64 cook, bead `akj.10.3`. |
> | The `Atomics` runtime confirmation | **RUN.** `typeof Atomics=undefined`, and `SharedArrayBuffer=function` — see §4. |
> | Perf passport §11 Win64 column | **MEASURED.** Rows 1, 2a, 2b, 3, 4, 7, 8 on the Dev leg. |
> | Monolithic / game-target build | see §12 |
> | `BuildPlugin -StrictIncludes` | see §12 |
> | Editor PIE (rows 12, 13) | Row 12's `-game` leg ran; **the editor-PIE leg and row 13 are still owed.** |

| Item | Why not |
|---|---|
| **Every windowed row** | **Windows SSH session isolation.** A GUI process launched from a non-interactive OpenSSH session has no interactive desktop; `-game -windowed` crashed at startup (exit 3) before engine init, with zero VaCuus lines. Falling back to `-RenderOffscreen` got the sessions to boot and reach VaCuus code — which is how §7's findings exist at all — but offscreen cannot produce the desktop-level rows the matrix wants. **A future Win64 pass needs a session on the physical console, or a scheduled task in the console session.** This is the single biggest venue difference from the macOS leg, where the console user and the SSH user were the same account and real windows opened. |
| **All visual rows** | The PSO fatal, §7. Even with a console session these would not have completed. |
| **Row 5 / bead `akj.6.19`** | Needs a human at the keyboard **and** a real window. Neither was available. |
| **Row 15, the disk-budget literal, and the memory-mapped bundle line** | **Packaging was not attempted.** Two `BuildCookRun -platform=Win64 -clientconfig=Shipping` runs were the plan; the pass spent its budget on six build iterations to get the plugin to compile and link at all, and a cook cannot be trusted while `-game` dies on a fatal. |
| **The memory-mapped bundle line specifically** | Still owed, and still Win64-only. What *is* now established from source rather than guessed: the cook-time predicate `ETargetPlatformFeatures::MemoryMappedFiles` resolves to `TPlatformProperties::SupportsMemoryMappedFiles()` (`TargetPlatformBase.h:493-494`), which is `true` on Windows (`WindowsPlatformProperties.h:76-79`) and `false` generically (`GenericPlatformProperties.h:258-261`), so a Win64 cook **will** stamp `BULKDATA_MemoryMappedPayload` at `VaCuusBundle.cpp:484-486`. **A trap for whoever runs it:** if the runtime mapping then fails for any reason (pak alignment, compression), `VaCuusBundleMount.cpp:89-94` prints `resident buffer (FPlatformProperties::SupportsMemoryMappedFiles() is false on this platform)` — an explanation that is **factually wrong on Windows**. Read `bMemoryMapped`, not the sentence. That message should probably be split; **owner call.** |
| **The `Atomics` runtime confirmation** | §4's conclusion is from the preprocessor guard and MSVC's documented `__STDC_NO_ATOMICS__`, not from a live `typeof Atomics`. No session survived long enough. |
| **Perf passport §11 Win64 column** | Not filled. Nothing measured on a surviving session. |
| **Monolithic / game-target build** | Not attempted. Worth noting it would **not** have caught B5: `RMLUI_STATIC_LIB` makes the macro empty in monolithic builds. |
| **`BuildPlugin -StrictIncludes` for Win64** | Not run. |
| **Editor PIE (rows 12, 13)** | Needs the console session, same as the windowed rows. |

---

## 10. Disk

The guardrail was: check before and after every heavy step, stop below 15 GB free.

| Point | Free |
|---|---|
| Before anything | **103.17 GiB** (110,776,123,392 bytes) |
| After project setup + plugin transfer | 103.08 GiB |
| Low-water mark (during the largest build) | **94.35 GiB** |
| At the end of the pass | **99.91 GiB** |

The low-water mark was **94.35 GiB — never within 79 GiB of the stop line.** Total consumed by the
pass is roughly 3 GiB: one editor build's `Intermediate` and `Binaries` (the five DLLs plus ~254 MB
of PDBs), plus per-row logs and screenshots.

**Nothing of the owner's was deleted or modified.** Everything created lives under
`C:\VaCuusWin64Test` (project, plugin, AutoSDK shim, logs, rows) plus `C:\VaCuusControl` (the
no-plugin control project) and the usual per-user UBT logs. Visual Studio, the engine install and
the registry were not touched — see §2. No process this pass did not start was ever killed, and
every kill was by PID. No editor process is left running.

---

## 11. What the owner has to decide

1. **The PSO fatal (§7) is a release blocker for Win64 and has no root cause yet.** Nothing
   visual can be signed off on this platform until it is found. It is the first thing for the
   next session, and it wants a debugger on the physical machine, not SSH.
2. **Land the five fixes.** `42fbdc2`, `e767f4a`, `38e1318`, `3b6bb44` (and B6 inside `3b6bb44`).
   None is pushed. **B4 and B5 are latent on every platform** — `FVaCuusModelLayout`'s
   ill-formed copy-assignment and RmlUi's dll-interface-on-a-template are defects that Linux and
   macOS merely fail to *diagnose*. B3 (the Windows 7 API level) affected only Win64 builds.
3. **`UnsizedDrain`'s assertion is tighter than its mechanism** (§5). The two-line fix is written
   out and deliberately not applied; verify on Linux, then land.
4. **`/experimental:c11atomics`** (§4): accept that Win64 ships without the JavaScript `Atomics`
   global, or add the experimental MSVC flag for parity with Linux and macOS.
5. **The machine is missing a documented UE prerequisite** (§2). Installing
   `Microsoft.Net.Component.4.6.2.SDK` removes the need for the shim. The owner's VS was
   deliberately left alone.
6. **Windows SSH cannot run the interactive half of this matrix** (§9). A console session is
   required before rows 1–6, 9–15 or bead `akj.6.19` can be attempted again.
7. **Two small housekeeping calls**: where the `dumpbin` export check should live (§6), and
   whether the bundle mount's "resident buffer" message should stop asserting a reason that is
   wrong on Windows (§9).

### Disposition, 2026-08-03 evening — what happened to each of the seven

Added after the pass, deliberately below the original list rather than inside it: §1–§10 record
what was seen on the day and do not get rewritten. Every item now has a bead, so this block is a
signpost and not a second tracker.

> **Written in the afternoon; §12 is the evening, and it overtakes items 4, 5, 6 and half of 7.**
> Read §12 for those. In short: the venue question (item 6) was **answered** — the physical console
> session, no PsExec and no RDP — and with it 13 of 15 matrix rows and the passport's Win64 column
> landed; `Atomics` (item 4) was **measured live** and the split accepted, with the sharper finding
> that `SharedArrayBuffer` is present while `Atomics` is not; the NetFxSDK gap (item 5) is still
> open but its blast radius grew rather than shrank — it blocks packaging too, because
> `BuildCookRun` builds the editor target. Item 7's `dumpbin` half went a third way: the script was
> not in the pass's scratch at all, so it was rewritten and re-earned its green (§12.6).

1. **RESOLVED — and it was not a driver, a permission or a stale cache.** Bead `xa5`, fixed by
   `b4f12e1`. DXC numbers signature registers positionally and `SV_Position` occupies one in the
   vertex output signature, so a pixel stage that spells out its own semantics and omits it starts
   one register low; D3D12 refuses the pipeline and `PipelineStateCache.cpp:712` makes that Fatal.
   Fix is the engine's own idiom (`SlateShaderCommon.ush:35-47`): one shared interpolant struct in
   `Shaders/Private/VaCuusUIInterpolants.ush`, included by all four stages. Zero pipeline failures
   on the RTX 2080 SUPER afterwards, HUD on screen, Linux 198/198. **Neither Vulkan nor Metal could
   have caught it by construction** — both map `SV_Position` to a builtin that consumes no numbered
   varying. The 13 `NOT RUN` rows of §8 are now blocked only on the venue (item 6), tracked as
   `akj.10.1`.
2. **RESOLVED.** All five are ancestors of `master`: `42fbdc2`, `e767f4a`, `38e1318`, `3b6bb44`,
   plus `b4f12e1` from item 1.
3. **RESOLVED.** Bead `akj.10.5`. Landed as a *range*, not the two-line `>= 1` this section
   sketched: `recorded >= 1` **and** `recorded <= frames-run-since-the-resize + 1`, so the
   anti-retroactive property the original `== 1` was written for survives. The extra frame's real
   source turned out to be one level up from §5's account — `Enqueue()` triggers the wake event
   itself (`VaCuusUIThread.cpp:800-801`), so the resize wakes the worker before `RunFrames` samples
   anything. The `+1` is the one frame that can be recorded-but-not-yet-counted
   (`VaCuusRmlDocumentHost.cpp:576` bumps inside the frame, `VaCuusUIThread.cpp:979-980` after it),
   which is why the two loads are read in that order. Restore-the-bug both ways on Linux: a
   fabricated retroactive record fails the ceiling and leaves the publish assertion passing exactly
   as Win64 saw it; removing the record bump fails the floor. Suite 198/198.
   **Confirmed on this machine the same evening**, which is the venue that matters because this is
   where it failed twice: `DESKTOP-590NICV`, editor rebuilt at `6b82e4a` (325 actions, 670 s,
   Succeeded), suite `198 tests performed`, **198 Success, 0 Fail** — the first all-green Win64
   run there has ever been, against §5's 196/1. The export gate re-run on those fresh DLLs is
   also clean and reports the same counts as §6, so nothing in `master` since the pass moved the
   supported surface.
4. **DEFERRED, by owner decision, until it can be measured.** Bead `akj.10.4`. The flag is not
   added: §4's answer is deduced from the preprocessor guard, and sessions now survive long enough
   to evaluate `typeof Atomics` on Win64 and decide against an observation instead.
5. **OPEN.** Bead `akj.10.9`.
6. **OPEN, and it is now the top of the Win64 critical path** — with item 1 fixed, the console
   session is the only thing between here and the 13 rows. Bead `5fg`; the rows are `akj.10.1`, the
   passport column `akj.10.2`, the IME row `akj.6.19`.
7. **The `dumpbin` check: DECIDED** — it goes in as-is at `Tools/api_export_check_win64.ps1`, so the
   remaining work is retrieving it from this pass's scratch on the Win64 box before that tree is
   deleted, not writing it (bead `akj.10.7`). **The bundle-mount message: RESOLVED** by `825969f`,
   hours after this pass — `VaCuusBundleMount.cpp` now branches on the predicate and the
   supported-but-unmapped case says so. That commit also corrects a premise this document repeats
   in §9: "memory mapping is Win64-only" is false. Win64, Android and iOS all answer true; only
   Linux and Mac inherit false from `FGenericPlatformProperties`
   (`GenericPlatformProperties.h:258-261`). Bead `akj.10.6`, closed.

Not on the original list because they were recorded in §9 as "could not run" rather than as
decisions, but they are owed and now tracked: the Win64 cook — disk literal and the memory-mapped
bundle line — is `akj.10.3`, and the monolithic game target plus `BuildPlugin -StrictIncludes` is
`akj.10.8`. **`akj.10.8` needs no interactive desktop**, so it is the one piece of this backlog that
an SSH session can finish today.

---

## 12. The console-session pass, 2026-08-03 evening

**What changed:** nothing about the machine, and two things about the situation. `b4f12e1` fixed the
PSO fatal of §7, and the work ran from the **physical console session** instead of over SSH — which
is the whole content of bead `VaCuus-5fg`. Commit under test `6b82e4a`. Everything below is on the
same host, GPU, driver and engine as the morning pass, so the two are comparable.

### 12.1 The venue question, answered once so it is not rediscovered (bead `5fg`)

The morning pass's diagnosis was right, and its options list can be closed on the first entry:
**run from the console session**. Concretely, what worked:

```
SESSIONNAME=Console, state Active   # `query session` shows it; [Environment]::UserInteractive is True
"…\UnrealEditor.exe" <abs>.uproject -game -windowed -resx=1920 -resy=1080 -ForceRes -ExecCmds="…,"
```

No PsExec, no scheduled task, no RDP, no autologon — those were the fallbacks and none was needed.
The one thing that *does* have to be right is that the shell issuing the launch is itself in the
console session; an OpenSSH session on Windows is a different session with no interactive desktop,
and the process dies before engine init with exit 3 and zero VaCuus lines.

Two operational lessons, both learned by doing it wrong first:

- **Close the window, do not kill the process.** `Stop-Process -Force` produces no teardown tail,
  which is matrix row 14's entire evidence. `Process.CloseMainWindow()` and then waiting gives
  exit 0 and the full tail. (Offscreen runs have no main window and still need the kill — which is
  why the offscreen glass A/B below has no tail.)
- **`-ExecCmds` still swallows everything after it and still splits on commas.** The runner appends
  the trailing comma itself rather than trusting each invocation to remember.

The driver used for every row is kept beside the artifacts as `run-row.ps1`; each run leaves its
log, screenshots and a `run.json` under `runs/<tag>/`.

### 12.2 The matrix column: 13 of 15 (beads `akj.10.1`, `akj.10.2`)

Filled in `2026-08-vacuus-manual-matrix.md`, which now carries the row-by-row reading. Rows 1, 2, 3,
4, 6, 7, 8, 9, 10, 11, 12, 14 and 15 **PASS** — 13 of 15. Only two remain: row 5 needs a human with
an IME, and row 13 needs an interactive editor PIE with a mid-run file edit. **Zero `Fatal error`,
zero `graphics pipeline` failures, zero `D3D12 ERROR` across roughly a dozen sessions.**

Two results changed what this document previously claimed, and both are corrections rather than
additions — see §4 (Atomics, confirmed live and *narrower* than predicted) and §7's boxed correction
(glass takes the bounded copy pass on a real swapchain; the direct-SRV "first on any platform" was
an artifact of this pass's own offscreen venue).

The perf passport's Win64 D3D12 column is measured and lives in
`2026-08-vacuus-perf-passport.md`. The one thing to carry back here: **this desktop is ~2.8× slower
than the Linux passport machine on the identical workload** — the ratio is established by two
controls rather than assumed — and row 2a's per-frame budget is breached 2.5× on that basis. The
tempting alternative explanation, that a slower frame rate inflates per-frame cost, was tested at a
30 fps cap and **refuted**.

### 12.3 Monolithic game target: builds, and it found what the modular leg cannot (bead `akj.10.8`)

```
Build.bat TP_ThirdPerson Win64 Development -project=C:\VaCuusWin64Test\VcHost\VcHost.uproject
  -> Result: Succeeded, 888 s, 358 actions
  -> C:\VaCuusWin64Test\VcHost\Binaries\Win64\TP_ThirdPerson.exe, 337,195,008 B
```

**It needed no AutoSDK shim.** §2's `NetFxSDK` blocker is an *editor-target* problem: `SwarmInterface`
is reached via `UnrealEd`, and a game target does not depend on `UnrealEd`. `UE_SDKS_ROOT` was unset
in the machine, user and process environments for this build and the makefile generated anyway. That
narrows bead `akj.10.9` usefully — see §12.5.

**One warning, and it is exactly the kind of thing this leg exists to find:**

```
…\ThirdParty\RmlUi\Source\Core\Layout\ContainerBox.cpp(141) : warning C4756: overflow in constant arithmetic
```

`ContainerBox.cpp:141` is `available_space.y = HUGE_VALF;`. The overflow is deliberate and lives in
Microsoft's own header: `corecrt_math.h:90-99` defines `_HUGE_ENUF` as `1e+300` with the comment
`_HUGE_ENUF*_HUGE_ENUF must overflow`, `INFINITY` as `((float)(_HUGE_ENUF))`, and `HUGE_VALF` as
`((float)INFINITY)` — narrowing 1e+300 to `float` overflows on purpose, which is how the header
manufactures infinity. The diagnostic is therefore benign.

**What is not benign is that the modular leg cannot see it.** The same TU
(`relay_Core_Layout_ContainerBox.cpp`) is compiled by both targets — 190 `relay_Core_*` actions in
each — and the modular editor build emits nothing for it. Its `/experimental:log` SARIF sidecar for
that TU contains **zero results**, so this is not a log-capture artifact. The cause was isolated
rather than inferred: the only substantive difference between the two response files is
**`/fp:precise` (editor) versus `/fp:fast` (game)**, and two lines reproduce it exactly —

```cpp
#include <math.h>
float f() { return HUGE_VALF; }
```

```
cl /c /W4 /fp:precise  -> clean
cl /c /W4 /fp:fast     -> warning C4756
```

**The consequence to remember:** `bWarningsAsErrors` is an OR (`UEBuildModuleCPP.cs:2669`, §3), so
the day anything upstream turns `/WX` on for a game target this becomes a build stopper — on Windows
only, in vendored code, from a flag nobody set deliberately. It is recorded rather than silenced,
because silencing it in the module would also hide any *real* constant overflow.

This is B5's mirror image, and the reason the bead insisted on both shapes: B5 (RmlUi's
dll-interface on header-only templates) **cannot** fire monolithically because `RMLUI_STATIC_LIB`
empties the macro; C4756 **cannot** fire modularly because the editor target compiles `/fp:precise`.
Neither build shape substitutes for the other, in either direction.

### 12.4 `BuildPlugin -StrictIncludes` for Win64

Not yet run — queued behind the cook, which claims the machine. Command:

```
RunUAT.bat BuildPlugin -Plugin=<clone>\VaCuus.uplugin -Package=<out> -TargetPlatforms=Win64 -StrictIncludes
```

### 12.5 The .NET Framework SDK, narrowed (bead `akj.10.9`)

Re-checked 2026-08-03 evening, four ways, all still negative: no `NETFXSDK` under either Windows
Kits path, no `Microsoft SDKs\NETFXSDK` key in either registry view, no `.NETFramework` reference
assemblies, no `mscoree.h` anywhere. `UE_SDKS_ROOT` is set in no scope (machine and user both empty,
re-verified). The shim from the morning pass still sits at `C:\VaCuusWin64Test\autosdk`, still with
its `#error`-ed stub header, and is still needed.

**How much it blocks, established by tripping over it rather than by reasoning.** The first draft of
this section said the SDK gap "blocks the dev loop, not the product", on the strength of §12.3's
game target building without the shim. **That was wrong, and the P0 cook proved it within five
seconds:**

```
Creating makefile for TP_ThirdPersonEditor (.uproject file is newer)
Unable to instantiate module 'SwarmInterface': Could not find NetFxSDK install dir …
  (referenced via TP_ThirdPersonEditor -> Launch.Build.cs -> … -> Engine.Build.cs)
Result: Failed (RulesError)          # AutomationTool exiting with ExitCode=8
```

`BuildCookRun` builds the **editor** target in order to run the cook, so packaging inherits the
editor's dependency on `SwarmInterface` the moment UBT has to evaluate module rules again. The P1
cook succeeded only because the editor makefile happened to still be valid from an earlier editor
build; disabling the plugin in the `.uproject` invalidated it, and the very next cook failed. Any
`.uproject` or `Build.cs` edit does the same, and a clean machine's first cook has no valid makefile
at all.

**Corrected blast radius: this blocks editor builds AND packaging. The only thing that escapes it is
a direct game-target build** (`Build.bat TP_ThirdPerson …`), which does not depend on `UnrealEd`.
Until it was fixed, every cook on this machine needed `UE_SDKS_ROOT` pointed at the shim **for the
build process only** — which is how both cooks in §12.7 were run, with the machine and user scopes
verified empty before and after.

#### RESOLVED 2026-08-04 — the owner installed the SDK, and the shim is gone

The owner installed the VS component; `NETFXSDK\4.6.2` is now real, checked the same four ways this
document used to prove it absent: registry key
`HKLM:\SOFTWARE\WOW6432Node\Microsoft\Microsoft SDKs\NETFXSDK\4.6.2` carrying
`KitsInstallationFolder=C:\Program Files (x86)\Windows Kits\NETFXSDK\4.6.2\`; the directory itself;
a real `Include\um\mscoree.h`; and `Lib\um\{x64,x86,arm}\mscoree.lib`. (The `.NETFramework`
reference-assemblies folder is still absent — that is the *targeting pack*, a different component,
and `MicrosoftPlatformSDK.cs:357-367` does not look for it.)

**The acceptance criterion, run as written, with the same command that used to fail.** The shim was
not merely unreferenced but *renamed off disk* and then deleted outright, so it could not
participate even by accident, and `UE_SDKS_ROOT` was empty in process, machine and user scopes with
no pre-existing makefile:

| | before the install | after |
|---|---|---|
| `Build.bat TP_ThirdPersonEditor Win64 Development` | `Unable to instantiate module 'SwarmInterface': Could not find NetFxSDK install dir` → **`Result: Failed (RulesError)`, exit 8**, ~3 s | `Creating makefile for TP_ThirdPersonEditor (no existing makefile)` → **`Result: Succeeded`, exit 0**, 2.5–3.0 s, `Makefile.bin` 6,076,274 B written |

One variable changed and the failure went with it. Confirmed twice, the second time with the shim
already deleted rather than merely renamed.

**`C:\VaCuusWin64Test\autosdk` no longer exists.** That was the point of removing it: the shim was
the standing trap this bead was really about — a future session would otherwise have found the
recipe in this document and re-created a workaround for a problem that no longer exists. Editor
builds and cooks on this machine now need no `UE_SDKS_ROOT` at all.

**A trap worth keeping from the verification itself**, because it nearly produced a false PASS: the
first re-test returned `Result: Succeeded` while the SDK was *still missing*. UBT had reused a
**cached makefile** and never evaluated module rules. The `RulesError` only appears when the
makefile has to be regenerated — which is exactly why the P1 cook passed and the P0 cook died the
moment the `.uproject` changed. **Delete `Makefile.bin` before testing anything about module
rules**, or the cache will tell you a broken thing works. Same family as §12.7's stale-receipt and
stale-`Target.cs`-mtime traps.

### 12.6 The export gate, re-run (bead `akj.10.7`)

`Tools/api_export_check_win64.ps1` is committed (`6b82e4a`), so the "does Tools/ take PowerShell"
question is closed. Re-run at this commit against the Win64 editor build: **clean** — 5 supported
classes reachable (`UVaCuusWidget` 27 exported member refs, `UVaCuusWorldComponent` 53,
`UVaCuusView` 65, `UVaCuusSubsystem` 39, `UVaCuusStyleSet` 14), `FVaCuusRmlDocumentHost` at 0, the
absent-class self-test reporting 0 so the FAIL path is known to work, exported `JS_*` symbols 0.
`docs/buyer/owner-handoff.md` step 3b now names both gates instead of calling the check Linux-only.

### 12.7 The Win64 cook (bead `akj.10.3`) — both halves closed

**(1) The disk literal.** Two `BuildCookRun -platform=Win64 -clientconfig=Shipping -build -cook
-stage -pak` packages, P1 = plugin enabled, P0 = plugin disabled in the `.uproject` plus the three
plugin-scoped config lines commented out:

| | P1 (plugin on) | P0 (plugin off) | Delta |
|---|---|---|---|
| Staged bytes, non-debug (`.pdb` excluded) | 473,562,978 B | 469,947,474 B | **+3,615,504 B (3.45 MiB)** |
| `TP_ThirdPerson-Win64-Shipping.exe` | 169,001,472 B | 166,074,368 B | +2,927,104 B |
| `VcHost-Windows.ucas` | 224,444,416 B | 223,771,520 B | +672,896 B |
| `VcHost-Windows.pak` | 11,243,420 B | 11,236,996 B | +6,424 B |
| `VcHost-Windows.utoc` | 213,163 B | 210,422 B | +2,741 B |
| `global.ucas` | 3,226,992 B | 3,221,584 B | +5,408 B |

**+3.45 MiB against a 10 MB budget**, itemization closing to 931 bytes of staged manifest text, and
identical non-debug file counts (29) in both legs — nothing appears or disappears as a whole file,
so the delta is entirely growth inside the binary and the containers. That is what a bundle-mounted
plugin with no loose staged content is supposed to look like. **Passport row 6's Linux proxy
(3.22 MiB) turns out to have been a good one — 7% low, same shape.** Full itemization in the
passport.

**(2) The memory-mapped bundle line, observed taken:**

```
Mounted bundle '/VaCuus/Bundles/DevUIBundle.DevUIBundle': 24 entries, 465513 bytes,
  memory-mapped region, hash da75027b...
```

`bMemoryMapped` is **true** — the branch at `VaCuusBundleMount.cpp:212` that only Win64, Android and
iOS can reach. Reproduced across two consecutive runs with the identical hash and byte count. Set
beside the Linux Shipping line for the same bundle — `24 entries, 461881 bytes, resident buffer` —
this is the platform split the source predicted, now seen from both sides. (Payload sizes differ
because the two platforms cook the payload separately; entry count matches.)

Note also that `825969f`'s fix is doing its job: the message now reports the property it read rather
than a platform it assumed, so this line can be trusted on Windows. The warning in bead `akj.10.3`
about the fallback sentence being wrong on Win64 predates that commit.

**Venue substitution, recorded per spec §2(f).** The observation was made on a cooked, paked, staged
**Development** build rather than Shipping, because on this engine a Shipping run cannot be read at
all — see trap 3 below. It is mechanism-preserving for this particular question: `bMemoryMapped` is
set from whether the load returned a mapped region, which depends on the **cooked payload's**
`BULKDATA_MemoryMappedPayload` stamp and the iostore/pak loader, neither of which is a function of
the compile configuration. Both legs read the same cooked content. What the substitution does not
cover is Shipping-only codegen, and nothing here reads that.

**A cross-check that fell out of it:** the packaged run's second pass measured 79.7 fps with UI
Update 3.556 / Record 2.205 ms — within noise of the editor `-game` soak's 80.8 fps and
3.001 ms combined. The packaged build performs like the editor build on this workload, which is
worth knowing because the passport's Win64 column was measured in the editor venue.

**First-run warm-up is real and large, and it would have been easy to publish by mistake.** The
*first* run off a fresh cook measured **46.6 fps, Update 13.0 / Record 9.0 ms** — about 4× the
steady figures above, on the same binary and the same content. Nothing was wrong; the cold PSO and
shader caches simply dominate the first pass. Any Win64 number taken from a just-cooked package
without a warm-up run is wrong by roughly that factor.

**Four Win64-specific traps, each of which cost a failed run:**

1. **The cook needs the AutoSDK shim.** Exit code 8 on the first P0 attempt — §12.5. `BuildCookRun`
   builds the editor target, and disabling the plugin in the `.uproject` invalidated the editor
   makefile, so UBT re-evaluated module rules and hit `SwarmInterface`.
2. **Orphaned staged processes lock the staging directory.** Exit code 102,
   `Error_FailedToDeleteStagingDirectory`, on `dbghelp.dll`. Cause: the staged **root** `.exe` is a
   launcher stub that re-launches the real binary out of `<staged>\VcHost\Binaries\Win64\`, so
   killing the PID `Start-Process` returns leaves the *game* running. Two such orphans from earlier
   row-15 runs were still alive fifteen minutes later. Reap by executable **path** after every
   staged run — never by name pattern, which is the same trap CLAUDE.md already records for `pkill`.
3. **A Shipping run on an Installed engine cannot be read at all.** A stock `Game` target compiles
   every `UE_LOG` out of Shipping, `-log` or not. Setting `bUseLoggingInShipping = true` is refused:
   `TP_ThirdPerson modifies the values of properties … not allowed, as TP_ThirdPerson has build
   products in common with UnrealGame`, and the remedy UBT names —
   `BuildEnvironment = TargetBuildEnvironment.Unique` — requires rebuilding engine modules, which an
   Installed build cannot do. `-clientconfig=Test` is not an escape either: *"Targets cannot be
   built in the Test configuration with this engine distribution."* Hence the Development
   substitution above.
4. **A stale target receipt silently stages nothing.** After the failed `bUseLoggingInShipping`
   builds, `TP_ThirdPerson.target` was left as a *partial* receipt — `Launch` empty, **0 build
   products** — and UAT dutifully staged content with no executable, without warning. Rebuilding did
   not fix it ("Target is up to date" writes no receipt); the receipt file has to be **deleted** and
   the target rebuilt. Two related timestamp traps in the same area: restoring a `Target.cs` with
   `Copy-Item` from a backup restores its **original mtime**, so UBT keeps using the rules assembly
   it compiled from the edited file and the failure repeats verbatim until the file is touched.

**And one venue difference that is not a trap but will waste ten minutes if unknown:** a staged
**Shipping** build writes its `Saved/` to `%LOCALAPPDATA%\<Project>\Saved`, while a staged
**Development** build writes it under the staged tree at `<staged>\VcHost\Saved\`. Screenshots and
logs are in different places depending on the configuration.
