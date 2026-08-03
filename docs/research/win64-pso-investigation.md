# Win64/D3D12 PSO fatal — a decision tree for the next session at the machine

**Bead:** `VaCuus-xa5`. **Prior art:** `docs/passport/2026-08-vacuus-win64-results.md` §7 (commit `31b0628`).
**Status of this page:** desk work only. Nothing here was run on Windows; nothing in the plugin was
edited. It exists so that the owner's runs at the physical console are decisive rather than
exploratory.

**Every claim carries a marker.**

- **READ** — I opened the cited source (or measured it with a tool on this box) and it says this.
- **INFERRED** — follows from READ facts plus a rule I could not execute here.
- **UNKNOWN** — genuinely open; only his run settles it.

**A calibration fact first, because it makes every citation below usable.** The passport's fatal
quotes `PipelineStateCache.cpp` `[Line: 712]`. On this Linux tree,
`Engine/Source/Runtime/RHI/Private/PipelineStateCache.cpp:712` is exactly
`UE_LOGF(LogRHI, Fatal, "Shader compilation failures are Fatal.");` — **READ**. The two engine trees
agree line-for-line, so the `file:line` citations in this document can be opened on his machine at
`C:\Program Files\Epic Games\UE_5.8\Engine\Source\...` and will land on the quoted code.

---

## 0. Before any run: three answers are already sitting in the logs from the last pass

The passport quoted four lines out of a dump that is roughly thirty. **READ** — the rest is in
`Saved\Logs\VcHost.log` right now.

### 0.1 The HRESULT. This is the single most valuable line and it costs nothing.

`WindowsD3D12PipelineState.cpp:733-767` is `CreatePipelineStateFromStream`. When no pipeline library
is in use it takes the `else` branch and logs the raw `HRESULT`:

```
Failed to create pipeline state with combined hash <NAME>, error <hr>.
```

(`WindowsD3D12PipelineState.cpp:762`, category `LogD3D12RHI`, verbosity `Error`.) **READ.**

**And no pipeline library is in use on a default install** — **READ**: `bUseAPILibaries` is set from
`D3D12.PSO.DriverOptimizedDiskCache` (`WindowsD3D12PipelineState.cpp:510-512`), whose default is `0`
(`:40-48`), and no `.ini` under `Engine/Config` overrides it (grepped; zero hits). So the `else`
branch is the live one and **that line is in his log**.

**Search the existing log for `Failed to create pipeline state with combined hash`.** What the value
means:

| `error` | Reading |
|---|---|
| `80070057` (`E_INVALIDARG`) | The desc or the shader pair is invalid. This is what hypothesis **H1** predicts. |
| `887a0005` (`DXGI_ERROR_DEVICE_REMOVED`) / `887a0006` (`_HUNG`) | Not a validation problem at all — a TDR/driver crash during PSO compile. Different bead entirely. |
| `8007000e` (`E_OUTOFMEMORY`) | VRAM/heap exhaustion. Would reframe everything. |

The engine itself distinguishes these two families at `WindowsD3D12PipelineState.cpp:804-810` /
`:822-828`, where the desc is always dumped and then `DEVICE_REMOVED`/`DEVICE_HUNG` are escalated
through `VERIFYD3D12RESULT_EX`.

### 0.2 The rest of the PSO desc, which he has and did not quote

`DumpGraphicsPSO` (`WindowsD3D12PipelineState.cpp:629-714`) prints, in this order and all under
`LogD3D12RHI: Warning:` — **READ**:

`AlphaToCoverageEnable`, `IndependentBlendEnable`, `RenderTarget[0] = { … }` (blend), `SampleMask`,
`FillMode`, `CullMode`, `FrontCounterClockwise`, depth bias trio, `DepthClipEnable`,
`MultisampleEnable`, `AntialiasedLineEnable`, `ForcedSampleCount`, `ConservativeRaster`,
`DepthEnable`, `DepthWriteMask`, `DepthFunc`, `StencilEnable`, stencil masks, `FrontFace`/`BackFace`,
`InputLayout.NumElements` + the elements, `IBStripCutValue`, `PrimitiveTopologyType`,
`NumRenderTargets`, `RTVFormats[…]`, **`DSVFormat`**, **`SampleDesc = { Count, Quality }`**,
`NodeMask`, **`Flags`**.

Three of those settle three hypotheses without a rerun:

- **`SampleDesc = { 1, 0 }`?** Anything else is an MSAA mismatch against a non-MSAA RT (**H5b**).
- **`DSVFormat = 0x0`?** Anything else with `DepthEnable = 0` is worth a second look (**H5c**).
- **`RenderTarget[0]` blend fields** — should decode to `BlendEnable=1`, `SrcBlend=2` (`ONE`),
  `DestBlend=6` (`INV_SRC_ALPHA`), `BlendOp=1` (`ADD`), same for alpha,
  `RenderTargetWriteMask=0xF`. That is `TStaticBlendState<CW_RGBA, BO_Add, BF_One,
  BF_InverseSourceAlpha, BO_Add, BF_One, BF_InverseSourceAlpha>` from
  `VaCuusReplayRenderer.cpp:420-421` — **a second, independent attribution signal beyond the vertex
  layout**, because that exact blend is ours.

### 0.3 The RHI-level dump, which also prints render-target formats

`PipelineStateCache.cpp:693-701` prints `Render Targets: (N)`, one `0x%x` per format, then
`Depth Stencil Format:` and one more. **READ.** Those are **`EPixelFormat`** values, not DXGI:
`PF_B8G8R8A8 = 2`, `PF_Unknown = 0` (`Core/Public/PixelFormat.h:18-20`). So the expected pair for
either of our render targets is `0x2` and `0x0`.

### 0.4 What is *not* in his log, and why

`DumpShaderAsm` (`WindowsD3D12PipelineState.cpp:588-627`) loads
`Engine/Binaries/ThirdParty/Windows/DirectX/x64/d3dcompiler_47.dll` and calls `D3DDisassemble` on
each stage's bytecode, appending the disassembly to the same dump. **READ** — the code path exists
and is unconditional in non-shipping builds (`D3D12RHI_USE_D3DDISASSEMBLE` defaults to `1`,
`:25-26`). **UNKNOWN** whether it produced anything here: `D3DDisassemble` from `d3dcompiler_47`
disassembles DXBC, and an SM6 shader is DXIL; if the call fails the code silently appends nothing
(`:617-624` — the `SUCCEEDED` guard has no else). **Ask him to say whether the dump was followed by
shader assembly text.** If it was, the VS disassembly names its own input/output signature and
attribution is settled on the spot with no rerun at all.

---

## 1. The attribution test — one run, and it is the first one

**The question:** is the failing PSO ours? The passport is honest that this is inferred from the
input layout alone (§7). Two things I could check here make that inference much stronger, and one
run makes it a fact.

### 1.1 What I could settle here without his machine

**The dumped input layout cannot be produced by any other vertex declaration in this build.**
**READ**, three ways:

1. The mapping is exact. `D3D12VertexDeclaration.cpp:38` maps `VET_Float2 → DXGI_FORMAT_R32G32_FLOAT`
   (`= 16 = 0x10`), `:44` maps `VET_UByte4N → DXGI_FORMAT_R8G8B8A8_UNORM` (`= 28 = 0x1C`), and
   `:109` stamps every semantic name as the literal `"ATTRIBUTE"`. The dumped
   `{0x10 @0, 0x1C @8, 0x10 @12}` is therefore *precisely* `VaCuusUIShaders.cpp:78-81`
   (`VET_Float2` attr 0 @0, `VET_UByte4N` attr 1 @8, `VET_Float2` attr 2 @12).
2. `VET_UByte4N` is nearly unused in the engine. A grep of all of `Engine/Source` (`*.cpp`, `*.h`)
   returns exactly four *user* sites: `SkeletalRenderGPUSkin.cpp:2193` and `:2201` (bone weights),
   `GPUSkinVertexFactory.cpp:963` and `:967` (attributes **4** and **15**, offset **4**). All of
   `Engine/Plugins` returns exactly one: `HairCardsDatas.h:40`. **Not one of them is attribute 1 at
   offset 8 in a three-element layout.**
3. Slate — the obvious suspect, since it is the other thing drawing UI — is not it.
   `SlateShaders.cpp:83-95` builds **six** elements and uses `VET_Color` (which maps to
   `DXGI_FORMAT_B8G8R8A8_UNORM`, `D3D12VertexDeclaration.cpp:45`), not `VET_UByte4N`.
   And the engine's screen-pass declaration is `FFilterVertex` — `float4` @0 plus `float2` @16,
   two elements (`CommonRenderResources.h:29-34`) — so **the composite, the blur and the glass
   downsample cannot be the failing PSO**, whatever else is true.

So: **INFERRED, with the inference now much stronger than "offsets coincide"** — the failing PSO
uses `GVaCuusVertexDeclaration`. That narrows it to exactly three of ours: the replay UI/Gradient
pipelines (`VaCuusReplayRenderer.cpp:424`), the glass draw (`VaCuusSlateElement.cpp:589`) and the
material draw (`VaCuusMaterialDraw.cpp:248`).

### 1.2 The run that settles it: a `-game` session that never creates a view

**READ** — nothing in the plugin brings a document up on its own. Every demo is an
`FAutoConsoleCommand` (`VaCuusRender.cpp:2028-2039`, `:2154-2187`, `:2326-2332`), and the only two
command-line hooks are `-VaCuusRefHud` (`:2227`) and `-VaCuusM5Demo` (`:2312`). With neither flag and
no `-ExecCmds`, `UVaCuusSubsystem` initializes and (per `Config/DefaultGame.ini:17-22`) may mount the
bundle, but no `Rml::Context` renders, no command buffer publishes, and no VaCuus PSO is ever
created.

**Run A** (§5.1) is that session. Outcomes:

| Outcome | Meaning | Next |
|---|---|---|
| Survives ≥ 90 s, no `LogRHI: Error` | **The failing PSO is ours.** | Go to §4 rung 1. |
| Dies with the same `Vertex:`/`Pixel:` hashes | **Not ours** — the bead changes shape and becomes an engine/driver bead. | Run A′, then hand back to Epic-land. |
| Dies with *different* hashes | Two separate defects. Record both. | Split the bead. |

**Run A′** is the control that removes all doubt if A dies: the same `-game` command against
`C:\VaCuusControl` (the no-plugin ThirdPerson project the last pass already created, passport §2).
If A′ dies too, VaCuus is not in the picture at all.

**Cheapness note:** A is the cheapest decisive run available, and it is decisive in *both*
directions. Do it before anything else that needs a rebuild.

---

## 2. Turning `<unknown>` into a name

### 2.1 Why the name is missing, and the one cvar that fixes it

**READ**, the whole chain:

- `RHIResources.h:704-708` — `RHI_INCLUDE_SHADER_DEBUG_DATA` is `1` in any non-Shipping/non-Test
  build, so the `Debug.ShaderName` field **exists** in this Development build.
- `RHIResources.h:859-864` — `GetShaderName()` returns `TEXT("<unknown>")` when that string is
  *empty*. The passport's `<unknown>` means the field is present and empty, not that the build
  stripped it.
- `D3D12Shaders.cpp:36-38` — the field is filled from `ShaderCode.FindOptionalData(FShaderCodeName::Key)`.
- `D3DShaderCompiler.inl:561-564` — that optional block is written **only** if the compile
  environment carries `CFLAG_ExtraShaderData`.
- `ShaderCompiler.cpp:3654-3657` — that flag is added only if `ShouldEnableExtraShaderData()`.
- `ShaderCore.cpp:978-982` — which reads `r.Shaders.ExtraData`.
- `ShaderCompiler.cpp:450-457` — `r.Shaders.ExtraData` **defaults to `0`** and is `ECVF_ReadOnly`.
- `ConsoleVariables.ini:88-89` ships it commented out with the comment *"When this is enabled,
  ShaderName field of FRHIShader will be populated (Development and Debug builds only)"*.

**So: `r.Shaders.ExtraData=1` is the answer, and it is the only one.** There is no runtime cvar, no
`-flag`, and no post-hoc lookup table that maps a printed shader hash back to a name — **READ**: the
hash printed at `PipelineStateCache.cpp:669` is `FShaderHash`, and nothing in the tree indexes it by
name. Shader symbols (`r.Shaders.Symbols=1`) do **not** substitute: the `.pdb`/`.dxil` files DXC
emits are named from `PdbName`, which DXC derives from the shader hash
(`D3DShaderCompilerDXC.cpp:760-789`), so they carry the same hash you already have.

**Cost, honestly.** `ECVF_ReadOnly` means it must be set before startup, and it changes the shader
compile environment — so the DDC key changes and **the affected shaders recompile once**. Expect a
slow first launch. The engine's own comment warns it "can add bloat to compiled shaders and can
prevent shaders from being deduplicated" (`ShaderCompiler.cpp:454-455`).

**Where to set it.** `ShaderCore.cpp:672-680` (`GetBoolForPlatform`, which `IsEnabled` calls) reads
the global cvar *first* and only then falls back to an `.ini` `[ShaderCompiler]` section. Set it in
the **project**, not in the owner's engine install:

```ini
; C:\VaCuusWin64Test\VcHost\Config\DefaultEngine.ini  — append
[ShaderCompiler]
r.Shaders.ExtraData=1
```

**What he gets.** `ShaderCompilerCore.h:298-312` — for a global shader the name is
`VirtualSourceFilePath + "|" + EntryPointName`. So the two `<unknown>` lines become, if H1 is right:

```
LogRHI: Error: Vertex: /Plugin/VaCuus/Private/VaCuusUI.usf|MainVS
LogRHI: Error: Pixel: /Plugin/VaCuus/Private/VaCuusUI.usf|MainPS
```

and if it is the gradient or the glass pipeline instead, the pixel line reads
`…/VaCuusGradient.usf|MainGradientPS` or `…/VaCuusBlur.usf|MainGlassPS`. **That one line separates
three of the four hypotheses in §3 in a single run.**

### 2.2 `-d3ddebug`: what it actually enables, what it costs, where the output lands

**READ**, `D3D12RHI.cpp:988-1009` (`SetupD3D12Debug`): the switches are parsed into the cvar
`r.D3D12.EnableD3DDebug` (`D3D12Adapter.cpp:66-74`), which is `ECVF_ReadOnly` — so **the command
line is the way to set it**, not the console.

| Switch | Cvar value | Effect |
|---|---|---|
| `-d3ddebug` (aliases `-d3debug`, `-dxdebug`) | 1 | Debug layer on; **errors** logged. `D3D12RHI.cpp:991-995` |
| `-d3dlogwarnings` | 2 | Errors **and warnings** logged. `:998-1000` |
| `-d3dbreakonwarning` | 3 | Plus break on both. `:1002-1004` |
| `-d3dcontinueonerrors` | 4 | Debug layer on, **no break on error**. `:1006-1008` |
| `-gpuvalidation` / `-d3d12gpuvalidation` | — | GPU-based validation; **implies the debug layer** (`D3D12Adapter.cpp:513-515`) and force-enables draw events (`:531-532`). |

**Where the output lands — READ.** `D3D12Adapter.cpp:989-1010` registers `D3D12MessageCallBack` on
`ID3D12InfoQueue1` and mutes the OS debug output, so messages come to us and only us. The callback
goes to `LogD3D12Message` (`:254-288`), which prints:

```
LogD3D12RHI: Error:   [D3DDebug] <the debug layer's own sentence>      (:270)
LogD3D12RHI: Warning: [D3DDebug] <…>                                   (:276)
```

**in the ordinary project log** — `C:\VaCuusWin64Test\VcHost\Saved\Logs\VcHost.log`. So the grep is
simply `[D3DDebug]`.

**Three traps, all READ:**

1. **`-d3ddebug` is a hard fatal if the Graphics Tools feature is not installed.**
   `D3D12Adapter.cpp:536-539`: if `D3D12GetDebugInterface` fails, the engine calls
   `UE_LOGF(LogD3D12RHI, Fatal, "Failed to create D3D debug interface, error %x. … Please install
   the Graphics Tools for Windows…")`. If the first `-d3ddebug` run dies *instantly* with that
   message, install the optional feature (Settings → System → Optional features → *Graphics Tools*)
   and rerun. It is not a new bug.
2. **Warnings are suppressed by default.** `D3D12Adapter.cpp:217` sets
   `GDebugLayerMinimumSeverity = D3D12_MESSAGE_SEVERITY_ERROR`, and `:1002` only raises it to
   `WARNING` when `bLogWarnings` (i.e. cvar ≥ 2). A PSO-creation complaint that the layer files as a
   *warning* would be invisible under plain `-d3ddebug`. **Use `-d3dlogwarnings`, which is a
   superset of `-d3ddebug` and costs nothing extra.**
3. **One PSO-creation message is on the deny list.** `D3D12Adapter.cpp:866` filters
   `D3D12_MESSAGE_ID_CREATEGRAPHICSPIPELINESTATE_RENDERTARGETVIEW_NOT_SET`. Nothing else in the
   `CREATEGRAPHICSPIPELINESTATE_*` family is filtered — checked the whole `DenyIds` list,
   `:862-970`. So a signature-linkage error, an input-layout error or a root-signature error will
   all print.

**Cost.** The debug layer is a large CPU slowdown (every API call is validated) and adds startup
time; `-gpuvalidation` is dramatically worse again — it instruments shaders and also turns on draw
events (`:532`). **Recommendation: `-d3dlogwarnings` first. Reach for `-gpuvalidation` only if the
layer says nothing at all**, because GBV is about *execution* correctness (out-of-bounds
descriptors, uninitialised reads), and a PSO that never gets created never executes. For this bead
it is the wrong instrument, and I would skip it unless §3's hypotheses are all dead.

**What to copy back.** Every `[D3DDebug]` line between the last `LogVaCuus` line and
`appError`, plus the whole `Failed to create graphics PSO with combined hash …` block, plus the
`Failed to create pipeline state with combined hash …, error …` line. Whole lines, with their
timestamps.

---

## 3. Ranked hypotheses

### H1 — VS↔PS signature register mismatch. **Rank 1, by a wide margin.**

**The mechanism.** D3D links the vertex stage's output signature to the pixel stage's input
signature by semantic *and by the register the compiler assigned to it*. If the same semantic sits at
different register indices in the two stages, the pair does not link and `CreateGraphicsPipelineState`
refuses. SPIR-V and Metal do not fail here: the UE cross-compilers assign matching locations to both
stages from one rule, so the same source builds and draws on Vulkan and Metal.

**What I measured on this box — READ, not inferred.** I built a small harness against the DXC that
ships with the engine (`Engine/Binaries/ThirdParty/ShaderConductor/Linux/x86_64-unknown-linux-gnu/libdxcompiler.so`),
compiled each entry point's signature at `vs_6_6`/`ps_6_6`, and disassembled it. Source and method
in the appendix. Registers, as DXC assigns them:

| Pipeline | Stage | Signature, register by register | Link? |
|---|---|---|---|
| **Replay UI** | `VaCuusUI.usf:9-15` `MainVS` **out** | `SV_Position@0`, `TEXCOORD1@1`, `TEXCOORD0@2` | |
| | `VaCuusUI.usf:26-29` `MainPS` **in** | `TEXCOORD1@`**`0`**, `TEXCOORD0@`**`1`** | **NO** |
| **Replay Gradient** | same VS | `SV_Position@0`, `TEXCOORD1@1`, `TEXCOORD0@2` | |
| | `VaCuusGradient.usf:97-99` `MainGradientPS` **in** | `TEXCOORD1@`**`0`**, `TEXCOORD0@`**`1`** | **NO** |
| **Glass draw** | same VS | `SV_Position@0`, `TEXCOORD1@1`, `TEXCOORD0@2` | |
| | `VaCuusBlur.usf:94-97` `MainGlassPS` **in** | `SV_Position@0`, `TEXCOORD1@1`, `TEXCOORD0@2` | yes |
| **Material draw** | `VaCuusMaterial.usf:39-45` `MainVS` **out** | `COLOR0@0`, `TEXCOORD0@1`, `SV_Position@2` | |
| | `VaCuusMaterial.usf:52-56` `MainPS` **in** | `COLOR0@0`, `TEXCOORD0@1`, `SV_Position@2` | yes |
| **Composite / blur** | `ScreenPass.usf:12-19` `ScreenPassVS` **out** | `TEXCOORD0@0`, …, `SV_Position` last | |
| | `VaCuusUI.usf:56-58` / `VaCuusBlur.usf:52-53` | `TEXCOORD0@0` | yes |

**The cause of the shift is not "descending semantic index". It is that `MainVS` declares
`SV_POSITION` *first* while `MainPS` does not declare it at all** — so every user interpolant lands
one register higher in the VS than in the PS.

**Why I believe D3D rejects it — INFERRED, and here is the induction.** The engine never writes this
shape. **READ, from a sample rather than a census**: grepping the one textual form
`out float4 … : SV_POSITION` across `Engine/Shaders` gives 22 entry points that declare it *before*
other outputs and 68 that declare it last. Every one of the four pairs I opened realigns — the
SV_POSITION-first vertex shaders pair with pixel shaders that **also** declare `SV_POSITION` first:

- `ComputeGenerateMips.usf:79` `MainVS(… , SV_POSITION, TEXCOORD0)` ↔ `:87`
  `MainPS(float4 : SV_POSITION, float2 : TEXCOORD0, …)`.
- `LandscapeLayersPS.usf:11` `CopyTextureVS(… , SV_POSITION, TEXCOORD0)` ↔ `:18`
  `CopyTexturePS(float4 : SV_POSITION, noperspective float2 : TEXCOORD0, …)`.
- Slate goes further and shares one struct between the stages, `SV_POSITION` first in both:
  `SlateShaderCommon.ush:35-53`, consumed as `VertexToPixelInterpolants` by both
  `SlateVertexShader.usf:7` and `SlateElementPixelShader.usf`.
- The SV_POSITION-last majority pairs with pixel shaders that omit it — `ScreenPass.usf:12-19` being
  the one we ourselves pair against for the composite.

Four out of four, plus VaCuus's own three later-written pairs (glass, material, composite) all
obeying it, reads as a rule rather than a coincidence. **It is still induction.** What I cannot do
here is watch D3D12 refuse the mismatched pair — that needs his machine.

**What the debug layer will print if H1 is true.** A `STATE_CREATION ERROR` from
`ID3D12Device::CreateGraphicsPipelineState` naming *linkage* and *mismatched hardware registers* for
semantic `TEXCOORD` — e.g.

```
LogD3D12RHI: Error: [D3DDebug] D3D12 ERROR: ID3D12Device::CreateGraphicsPipelineState:
  Vertex Shader - Pixel Shader linkage error: Signatures between stages are incompatible.
  Semantic 'TEXCOORD' is defined for mismatched hardware registers between the output stage
  and input stage. [ STATE_CREATION ERROR #… ]
```

**Do not treat the exact wording as gospel** — it is the D3D debug layer's, not ours, and I have not
seen it emitted. The load-bearing words to look for are **linkage**, **signature**, **TEXCOORD**.

**Cheapest confirmation:** §5.2 (`-d3dlogwarnings` + `r.Shaders.ExtraData=1` + `vacuus.M1HUD`). One
run gives both the message and the shader names.

**Cheapest kill:** the same run printing a debug-layer error about something *else entirely* (input
layout, root signature, RTV format), or printing nothing at all.

> #### ⚠ H1 has a candidate fix already in the working tree, and **it does not fix H1**
>
> **READ** — `/w/Unreal/VcHost/Plugins/VaCuus` has **uncommitted** modifications to
> `Shaders/Private/VaCuusUI.usf`, `VaCuusGradient.usf` and `VaCuusBlur.usf` (`git status`; also
> `Source/VaCuusRender/Private/VaCuusWorldDemo.cpp`, `VaCuusWorldInputProcessor.{cpp,h}`). They
> reorder the interpolants into ascending semantic index in **both** stages and add a comment above
> `MainVS` asserting this bead's cause, complete with a quoted debug-layer message. **That message is
> currently unverified** — the passport is explicit that root cause was not found (§7), and no run
> in the pass had `-d3ddebug` on.
>
> **And the edit is a no-op with respect to the register shift. Measured, READ:**
>
> | | VS out | PS in | link? |
> |---|---|---|---|
> | shipped | `SVPos@0, TEX1@1, TEX0@2` | `TEX1@0, TEX0@1` | **NO** |
> | edited | `SVPos@0, TEX0@1, TEX1@2` | `TEX0@0, TEX1@1` | **still NO** |
>
> Swapping the order **symmetrically in both stages** preserves the off-by-one, because the shift is
> caused by `SV_POSITION` being in the VS signature and not the PS's. **If the owner builds the
> working tree and still crashes, that is not evidence against H1.** Either revert those three files
> before the diagnostic runs, or use the run to read the *message*, not the outcome.
>
> If H1 confirms, the two shapes that actually realign the registers (either is sufficient; pick one
> and apply it to all three pipelines):
> - **(a)** add `in float4 SvPosition : SV_POSITION` as the **first** input of `MainPS` and
>   `MainGradientPS` — matches what `MainGlassPS` already does and leaves `MainVS` alone; or
> - **(b)** move `out float4 OutPosition : SV_POSITION` to **last** in `MainVS` (the
>   `ScreenPass.usf:12-19` idiom, which `VaCuusMaterial.usf:39-45` already follows) and drop
>   `SV_POSITION` from `MainGlassPS`.
>
> Not applied here: this page is read-only, and the fix wants the restore-the-bug proof on the
> machine that can see it fail.

### H2 — the mid-pass PSO switch. **Rank 2, but it is probably H1 wearing a hat.**

**Mechanism.** `VaCuusReplayRenderer.cpp:454-469` rebinds a second pipeline inside the same render
pass, changing only the pixel shader (UI ↔ Gradient). Both of those pairs are register-mismatched by
the H1 table, so a document with gradients has *two* bad PSOs; a plain document has one.

**Tell.** The pixel-shader name in §2.1's output. `MainPS` → the UI pipeline is the culprit and the
switch is irrelevant. `MainGradientPS` → the *first* PSO created was the gradient one, which is only
possible if the buffer's first draw is a `DrawShader`.

**Confirm/kill.** §4 rung 1 versus rung 2. If `vacuus.M1HUD` (no gradients, no glass, no materials)
dies, the mid-pass switch is not involved at all.

**Settled here?** Partly — **READ**: `SetGraphicsPipelineState` inside a render pass is ordinary
engine practice and the glass draw does the same (`VaCuusSlateElement.cpp:583-593`), so "you may not
switch mid-pass" is not a D3D12 rule. Low prior.

### H3 — input layout vs VS input signature under DXC/SM6. **Rank 3.**

**Mechanism.** D3D12 requires every element the VS input signature reads to exist in the input
layout, matched by semantic name and index.

**What I settled here — READ.** The VS input signature at SM6 is exactly
`ATTRIBUTE0@0, ATTRIBUTE1@1, ATTRIBUTE2@2` (measured; appendix), and
`D3D12VertexDeclaration.cpp:109` names every layout element `"ATTRIBUTE"` with
`SemanticIndex = Element.AttributeIndex`. **They match.** DXC does not rename or drop them.

**Tell if it were true anyway.** A debug-layer line naming the *input assembler* and a specific
`SemanticName/Index`, e.g. *"the declared input layout for the Input Assembler does not contain a
matching element"*.

**Kill.** Already mostly killed statically. Confirm from `InputLayout.NumElements = 3` in the dump he
has.

### H4 — `VET_UByte4N` → DXGI format. **Rank 4. Settled here; it is fine.**

**READ.** `D3D12VertexDeclaration.cpp:44` gives `DXGI_FORMAT_R8G8B8A8_UNORM` = `0x1C`, which is what
the dump shows. The shader declares `float4 InColor : ATTRIBUTE1` (`VaCuusUI.usf:11`); a UNORM
vertex format feeding a `float4` input is the normal IA→VS conversion. `AlignedByteOffset = 8` is
4-byte aligned, as D3D12 requires. **No defect.** Keep it on the list only so nobody re-derives it.

### H5 — render-target state. **Rank 5. Three sub-cases, all readable from the dump he has.**

- **H5a, RTV format.** `RTVFormats[0] = 0x57` = `DXGI_FORMAT_B8G8R8A8_UNORM`, which is exactly
  `PF_B8G8R8A8` from `VaCuusReplayRenderer.cpp:226`. Legal as an RTV; not sRGB-typed, so no
  typed/typeless conflict. **READ; no defect.** Note it does *not* discriminate replay from glass —
  a default SDR Slate elements texture is the same format.
- **H5b, sample count.** `SampleDesc.Count` must be `1`. **UNKNOWN** until he reads the line.
- **H5c, depth.** `DSVFormat` must be `0x0` given `DepthEnable = 0`
  (`VaCuusReplayRenderer.cpp:423`). **UNKNOWN** until he reads the line.
- The composite's `FLinearOutput` permutation is **not** in play: **READ**, that PSO's input layout
  is `FFilterVertex` (`CommonRenderResources.h:29-34`), two elements, not three, so it cannot be the
  dumped PSO.

### H6 — root signature / bindless. **Rank 6.**

**Mechanism.** UE derives the D3D12 root signature from the bound shaders' resource counts; a
mismatch between the signature and what the bytecode binds is an `E_INVALIDARG` at PSO creation.
SM6 is the first shader model this code has ever run at, so a binding-model difference is a fair
suspicion.

**What I settled here — READ.** `BaseWindowsEngine.ini:54-55` sets
`[ShaderPlatformConfig PCD3D_SM6] BindlessConfiguration=RayTracing`. Bindless is therefore **not**
enabled for our raster global shaders on this platform, which removes the most obvious version of
this hypothesis. **Low prior, but not zero** — the root-signature *layout* for SM6 still differs
from SM5 and I cannot exercise it here.

**Tell.** A debug-layer line naming the **root signature** ("the root signature does not match the
shader", a mismatched descriptor range, or a missing register space).

**Kill.** If §5.2's run prints a linkage message instead, this is dead.

### H7 — it is not ours at all. **Rank 7, and §1.2 is its test.**

Kept on the list because the passport was right to keep it. §1.1 makes it unlikely; Run A makes it
false or makes it the whole bead.

---

## 4. The bisect ladder

Ordered so each rung eliminates the most. **Every rung is a separate `-game` session.** Run each for
90 s or until it dies, whichever is first.

| Rung | What runs | Pipelines it can create | Pass means | Fail means |
|---|---|---|---|---|
| **0** | nothing (Run A, §5.1) | none of ours | the fatal is ours → rung 1 | not ours → H7; run A′ |
| **1** | `vacuus.M1HUD` | **UI only** | the plain UI pair is clean; H1 is wrong about `MainPS` → rung 2 | **the minimum repro.** H1's primary prediction. Stop and read the names. |
| **2** | `vacuus.M5Deco` | UI + **Gradient** | gradients clean → rung 3 | the Gradient pair. H1's second prediction; `MainGradientPS` in the name line |
| **2b** | `vacuus.M5Deco plain` | UI only | control for rung 2 — the decorator-free twin of the same document. A pass here with a fail at rung 2 pins it on the gradient PS, not the document | |
| **3** | `vacuus.M5Glass` | UI + **Glass** + blur + composite | glass clean (H1 predicts this) → rung 4 | the glass pair, or the blur/composite; the name line says which |
| **3b** | `vacuus.M5Glass` + `vacuus.GlassBackbufferSRV 0` | as 3, but the bounded-copy route | if 3 fails and 3b passes, it is the **direct-SRV route** — the D3D12-only path (passport §7), never exercised on Linux or macOS | |
| **4** | `vacuus.M5MatSpike` | UI + **Material** | the material pair clean (H1 predicts this) → rung 5 | the material pair; `VaCuusMaterial.usf` in the name line |
| **4b** | `vacuus.M5MatSpike` + `vacuus.MaterialDecorators 0` | UI only (style keys refuse at compile, `VaCuusMaterialDraw.cpp:43-48`) | control for rung 4 | |
| **5** | `vacuus.M5Demo` | everything at once | the acceptance demo lives → the matrix reopens | tells you nothing rungs 1–4 did not |

**`vacuus.IdleGate 0` is not a rung — it is a modifier.**
`VaCuusRecordingRenderInterface.cpp:66` declares the kill switch and `:1452-1453` is where it gates
publication on an unchanged content hash; a static HUD publishes ~1 frame per 13,000 recorded.
If a rung is suspiciously quiet (no fatal *and* no visible UI), set it to `0` **before** the demo
toggle to force a publish every frame and prove the replay path really ran. It does not change PSO
creation — the first published frame creates every PSO the buffer needs.

**Two ordering rules, both learned the expensive way (CLAUDE.md, passport §7):**

- Set every cvar **before** the demo toggle in `-ExecCmds`, in the same comma list.
  `SVaCuusWidget.cpp:27` says so for `AutoShot` and it is true for all of them.
- `vacuus.M5World.InputSmoke` fired at frame 0 reports *"no interactive rects; is the document
  published yet?"* — that is a timing artifact, not an input finding (passport §7). Do not read it as
  a result.

---

## 5. The exact command lines

Windowed, from a desktop session. All of these assume:

```
project : C:\VaCuusWin64Test\VcHost\VcHost.uproject
editor  : "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe"
log     : C:\VaCuusWin64Test\VcHost\Saved\Logs\VcHost.log
```

**The `-ExecCmds` comma rule, with its source.** `ParseExecCommands.cpp:57-66` calls
`FParse::Value(..., /*bShouldStopOnSeparator*/ false, ...)` — so an unquoted value **runs to the end
of the command line** and every switch after it is swallowed into the last command. The splitter
(`:11-54`) breaks on `,` and appends the final fragment separately (`:48-51`), so **ending the value
with a comma makes the swallowed tail its own throwaway command instead of corrupting the last real
one.** Every `-ExecCmds` below ends with a comma. Keep it that way.

**Read the log, not the console** (CLAUDE.md): `UnrealTraceServer` interleaving clobbers stdout tails.

### 5.1 Run A — the attribution test

```bat
"C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe" ^
  C:\VaCuusWin64Test\VcHost\VcHost.uproject ^
  -game -windowed -ResX=1600 -ResY=900 -nosplash -log
```

No `-ExecCmds`, no `-VaCuusM5Demo`, no `-VaCuusRefHud`. Leave it up for 90 s, then close the window.

**Bring back:** whether it survived; and if not, the `Failed to create graphics pipeline, hashes:`
line verbatim (the hashes matter — compare them with the passport's `Vertex: B4A05A77C9004166,
Pixel: 68E301A0F9A03DF2`).

**Run A′, only if A died:**

```bat
"C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe" ^
  C:\VaCuusControl\<Control>.uproject -game -windowed -ResX=1600 -ResY=900 -nosplash -log
```

### 5.2 Run B — the diagnostic run. **This is the one that names the shader.**

First, one edit, once (§2.1):

```ini
; C:\VaCuusWin64Test\VcHost\Config\DefaultEngine.ini
[ShaderCompiler]
r.Shaders.ExtraData=1
```

Then:

```bat
"C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe" ^
  C:\VaCuusWin64Test\VcHost\VcHost.uproject ^
  -game -windowed -ResX=1600 -ResY=900 -nosplash -log ^
  -d3dlogwarnings ^
  -LogCmds="LogVaCuus Verbose, LogD3D12RHI Verbose" ^
  -ExecCmds="vacuus.M1HUD,"
```

The first launch after the ini edit recompiles shaders — expect it to be slow, and do not read the
delay as a hang.

**Bring back, in this order:**

1. `LogRHI: Error: Vertex: …` and `LogRHI: Error: Pixel: …` — **the names**.
2. Every `LogD3D12RHI: Error: [D3DDebug]` and `LogD3D12RHI: Warning: [D3DDebug]` line in the
   60 seconds before `appError`.
3. `LogD3D12RHI: Error: Failed to create pipeline state with combined hash …, error ….`
4. The whole `Failed to create graphics PSO with combined hash …` block — all of it, including
   `SampleDesc`, `DSVFormat`, `Flags`, and any shader assembly that follows it.
5. The last five `LogVaCuus` lines before the fatal.
6. `LogD3D12RHI: Log: InitD3DDevice: -D3DDebug = on -D3D12GPUValidation = off`
   (`D3D12Adapter.cpp:544`) — proof the layer was actually on. **If this line says `off`, the run
   proves nothing.**

### 5.3 Runs C… — the ladder

Same shape as 5.2, changing only the `-ExecCmds` value. Keep `-d3dlogwarnings` on throughout; it is
what makes a pass at each rung mean something.

```bat
:: rung 2   gradients
  -ExecCmds="vacuus.M5Deco,"
:: rung 2b  the decorator-free control
  -ExecCmds="vacuus.M5Deco plain,"
:: rung 3   glass
  -ExecCmds="vacuus.M5Glass,"
:: rung 3b  glass through the bounded-copy route
  -ExecCmds="vacuus.GlassBackbufferSRV 0, vacuus.M5Glass,"
:: rung 4   materials
  -ExecCmds="vacuus.M5MatSpike,"
:: rung 4b  materials refused at compile — the control for rung 4
  -ExecCmds="vacuus.MaterialDecorators 0, vacuus.M5MatSpike,"
:: rung 5   the acceptance demo
  -ExecCmds="vacuus.M5Demo,"
:: modifier: force a publish every frame if a rung is suspiciously quiet
  -ExecCmds="vacuus.IdleGate 0, vacuus.M1HUD,"
```

If a rung **survives**, photograph it before moving on:
`-ExecCmds="vacuus.M1HUD.AutoShot 10, vacuus.M5Glass,"` — cvar first, and remember `AutoShot N` fires
after `max(N,3)` **recorded** frames, which is a floor, not a count (CLAUDE.md).

### 5.4 Housekeeping for every run

- **No editor may be running while anything builds.** `tasklist | findstr UnrealEditor`; kill by PID.
- If a session wedges, `taskkill /PID <pid>` — by PID, never by pattern.
- Every session appends to the same `VcHost.log`. Copy it aside between runs, or the ladder's rungs
  become one indistinguishable file.

---

## 6. What I could settle here, and what I could not

**Settled on this box, no Windows needed:**

| | Result |
|---|---|
| The dumped input layout is `GVaCuusVertexDeclaration` and nothing else in this build can produce it | **READ** — `D3D12VertexDeclaration.cpp:38,44,109`; `VET_UByte4N` grep over `Engine/Source` + `Engine/Plugins`; `SlateShaders.cpp:83-95` |
| The composite / blur / glass-downsample PSOs are **not** the failing one | **READ** — they use `FFilterVertex`, two elements (`CommonRenderResources.h:29-34`) |
| Nothing in the plugin creates a view without a console command or `-VaCuusM5Demo`/`-VaCuusRefHud` | **READ** — `VaCuusRender.cpp:2227, 2312`, and every demo is an `FAutoConsoleCommand` |
| The HRESULT of the failed `CreatePipelineState` **is already in his log** | **READ** — `WindowsD3D12PipelineState.cpp:762` reachable because `D3D12.PSO.DriverOptimizedDiskCache` defaults to `0` (`:40-48, :510-512`), unoverridden in every shipped `.ini` |
| `r.Shaders.ExtraData=1` is the only lever that resolves `<unknown>`, and there is no offline hash→name map | **READ** — `RHIResources.h:704-708, 859-864`; `D3D12Shaders.cpp:38`; `D3DShaderCompiler.inl:561-564`; `ShaderCompiler.cpp:450-457, 3654-3657`; `ShaderCore.cpp:978-982` |
| The debug layer's output lands in the project log as `[D3DDebug]`, and warnings are hidden unless `-d3dlogwarnings` | **READ** — `D3D12Adapter.cpp:217, 254-288, 989-1010` |
| `-d3ddebug` is a **fatal** without the Graphics Tools feature | **READ** — `D3D12Adapter.cpp:536-539` |
| H3 (input-layout ↔ VS input signature) — the semantics match at SM6 | **READ** — measured signature is `ATTRIBUTE0/1/2 @ registers 0/1/2` |
| H4 (`VET_UByte4N` → DXGI) — correct and legal | **READ** — `D3D12VertexDeclaration.cpp:44` |
| H6 (bindless) — not enabled for raster shaders on `PCD3D_SM6` | **READ** — `BaseWindowsEngine.ini:54-55` |
| **The register shift in the UI and Gradient pairs** | **READ** — measured with the engine's own DXC; table in §3/H1 |
| **The uncommitted working-tree "fix" does not remove that shift** | **READ** — measured both variants |
| Every engine VS/PS pair I opened realigns its registers; I found no counter-example | **READ, but a sample, not a census** — I grepped one textual form (`out float4 … : SV_POSITION`) across `Engine/Shaders`, got 22 SV_POSITION-first and 68 SV_POSITION-last hits, and opened four pairs: `ComputeGenerateMips.usf:79/87`, `LandscapeLayersPS.usf:11/18`, `SlateShaderCommon.ush:35-53` (shared struct, `SlateVertexShader.usf:7`), `ScreenPass.usf:12-19`. Struct-returning entry points are not covered by that grep. |

**Not settled here, and no amount of reading will settle it:**

- **Whether D3D12 actually refuses on the register shift.** The rule is INFERRED from the engine's
  own unbroken convention and from the D3D debug layer having a message for exactly this. Only the
  layer's own sentence, on his box, proves it. → Run B.
- **Whether the failing PSO is ours at all.** → Run A.
- **The HRESULT, `SampleDesc`, `DSVFormat`, `Flags`.** They are in a file I cannot open. → §0.
- **Whether `DumpShaderAsm` produces anything for DXIL.** → §0.4.
- **Everything downstream.** Thirteen matrix rows, the perf passport's Win64 column, the `Atomics`
  runtime check and bead `akj.6.19` are all blocked behind a session that survives.

---

## Appendix — how the register table was measured

Not a claim, a measurement, so that anyone can redo or refute it.

The engine ships a Linux DXC: `Engine/Binaries/ThirdParty/ShaderConductor/Linux/x86_64-unknown-linux-gnu/libdxcompiler.so`,
with headers at `Engine/Source/ThirdParty/ShaderConductor/ShaderConductor/External/DirectXShaderCompiler/include/dxc/`.
A ~110-line harness `dlopen`s it, compiles each entry point at `vs_6_6`/`ps_6_6` through
`IDxcCompiler3::Compile`, disassembles the result with `IDxcCompiler3::Disassemble`, and prints the
`; Input signature:` / `; Output signature:` comment blocks DXC emits — which carry the `Register`
column directly.

```
g++ -std=c++17 -fms-extensions \
  -I<engine>/Engine/Source/ThirdParty/ShaderConductor/ShaderConductor/External/DirectXShaderCompiler/include \
  sigdump.cpp -o sigdump -ldl
./sigdump <engine>/Engine/Binaries/ThirdParty/ShaderConductor/Linux/x86_64-unknown-linux-gnu/libdxcompiler.so
```

The entry points were transcribed from the committed `.usf` sources with their bodies reduced to
whatever keeps each input live (an unread input is dropped from the signature, which would have
falsified the measurement). Two engine-idiom controls were compiled alongside — a `ScreenPassVS`-shaped
VS and its PS — and they came out register-aligned, which is the harness's own sanity check.

Raw output, the two rows that matter:

```
MainVS   (SV_POSITION, TEXCOORD1, TEXCOORD0 — as shipped)   vs_6_6
  Output:  SV_Position 0 → register 0     TEXCOORD 1 → register 1     TEXCOORD 0 → register 2
MainPS   (TEXCOORD1, TEXCOORD0 — as shipped)                ps_6_6
  Input:   TEXCOORD 1 → register 0        TEXCOORD 0 → register 1
```

The harness emits a `DXIL signing library (dxil.dll,libdxil.so) not found` warning on Linux. That is
expected and irrelevant here: signing affects whether the D3D12 runtime will *load* the bytecode, not
what the signature tables say. It is not evidence about the Windows build, where the compile runs
through the shipped `dxcompiler.dll`/`dxil.dll` pair.
