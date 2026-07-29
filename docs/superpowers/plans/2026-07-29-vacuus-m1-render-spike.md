# VaCuus M1 — Render Spike Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps
> use checkbox (`- [ ]`) syntax for tracking.

**Goal:** RmlUi renders a static game-HUD document inside UE 5.8 PIE through a recorded
command buffer replayed on the render thread into a persistent per-view RT, composited by
an RDG `ICustomSlateElement` — with replay cost measured and the spec §11 replay budget
re-baselined.

**Architecture:** RmlUi Core is vendored as source and compiled by a UE ThirdParty-style
module. A record-only `Rml::RenderInterface` captures draw commands + CPU geometry/texture
data into a command buffer; a replay renderer creates RHI resources lazily and draws with
two global shaders into a pooled RT; a Slate custom element composites that RT. In M1,
`Context::Update/Render` run on the game thread (temporary — the dedicated UI thread is
M2 scope); the M1 acceptance is about the render path: no render-thread blocking waits,
no `FlushRenderingCommands` in steady state.

**Tech Stack:** UE 5.8.1 (source build at `/w/Unreal/UnrealEngine`), RmlUi master
`0ae381e` (benchmarked baseline), UE global shaders (USF), RDG, Slate.

**Beads:** this plan implements `VaCuus-akj.2`. Claim it before starting:
`bd update VaCuus-akj.2 --claim`.

**References (read before coding, all local):**
- RmlUi API ground truth: `Source/ThirdParty/RmlUi/Include/RmlUi/Core/RenderInterface.h`
  (after Task 2) — verify exact signatures; they are restated below from the benchmarked
  checkout but the header wins.
- RDG Slate element API: `/w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Rendering/RenderingCommon.h`
  (`ICustomSlateElement`, `FDrawPassInputs`, ~line 937).
- Reference RHI backend to mimic: `https://github.com/Noesis/UnrealPlugin` →
  `Source/NoesisRuntime/Private/Render/NoesisRenderDevice.cpp` (patterns only — do NOT
  copy code, its license is not ours).
- Working GL3 semantics reference: `docs/research/hud-demo/` +
  `Source/ThirdParty/RmlUi/Backends/RmlUi_Renderer_GL3.cpp` (vendored in Task 1).

**Known machine pitfalls (from project memory):**
- **Never symlink the plugin into a host project** — UBA breaks on symlinked plugins.
  Use a git clone inside the host project (Task 0).
- Throwaway host projects keep stock template naming conventions; do not rename modules.

---

## File structure (end state of M1)

```
VaCuus/  (plugin repo)
├─ Source/
│  ├─ ThirdParty/RmlUi/            # Task 1: vendored source @0ae381e (Include/, Source/, Backends/ ref only, LICENSE)
│  ├─ VaCuusRml/                   # Task 2: UE module compiling RmlUi Core
│  │  ├─ VaCuusRml.Build.cs
│  │  └─ Private/VaCuusRmlModule.cpp
│  ├─ VaCuus/                      # exists; Task 3 adds:
│  │  ├─ Public/VaCuusEngine.h     # Rml boot singleton (M1-scope lifecycle)
│  │  ├─ Private/VaCuusEngine.cpp
│  │  ├─ Private/VaCuusSystemInterface.h/.cpp
│  │  ├─ Private/VaCuusFileInterface.h/.cpp
│  │  └─ Private/Tests/VaCuusBootTest.cpp
│  ├─ VaCuusRender/                # Tasks 4-8
│  │  ├─ VaCuusRender.Build.cs
│  │  ├─ Public/VcCommandBuffer.h
│  │  ├─ Public/VcRecordingRenderInterface.h
│  │  ├─ Private/VcRecordingRenderInterface.cpp
│  │  ├─ Private/VcReplayRenderer.h/.cpp
│  │  ├─ Private/VcUIShaders.h/.cpp
│  │  ├─ Private/VcSlateElement.h/.cpp
│  │  ├─ Private/SVaCuusHUDWidget.h/.cpp
│  │  ├─ Private/VaCuusRenderModule.cpp   # console command + shader dir mapping
│  │  └─ Private/Tests/VcRecorderTest.cpp
├─ Shaders/Private/VaCuusUI.usf    # Task 6
└─ Content/DevUI/m1_hud.rml|.rcss  # Task 9 (dev-time loose files)
```

---

### Task 0: Host project + plugin worktree

**Files:** none in repo (environment setup).

- [x] **Step 0.1: Create the host project.** In UnrealEditor (source build), create a
  **Blank C++** project named `VcHost` at `/w/Unreal/VcHost`. Close the editor.
- [x] **Step 0.2: Clone the plugin into the host (no symlinks!):**

```bash
mkdir -p /w/Unreal/VcHost/Plugins
git clone /w/Unreal/VaCuus /w/Unreal/VcHost/Plugins/VaCuus
cd /w/Unreal/VcHost/Plugins/VaCuus
git switch -c m1-render-spike
```

All M1 work happens in this clone on branch `m1-render-spike`; push back with
`git push origin m1-render-spike` (origin = `/w/Unreal/VaCuus`; pushing a non-checked-out
branch is allowed).

- [x] **Step 0.3: Baseline build check:**

```bash
cd /w/Unreal/UnrealEngine
./Engine/Build/BatchFiles/Linux/Build.sh VcHostEditor Linux Development \
  -project=/w/Unreal/VcHost/VcHost.uproject
```

Expected: builds clean, including existing `VaCuus`/`VaCuusEditor` modules.

---

### Task 1: Vendor RmlUi @ benchmarked SHA

**Files:** Create: `Source/ThirdParty/RmlUi/**`

- [x] **Step 1.1:**

```bash
cd /tmp && git clone https://github.com/mikke89/RmlUi.git rmlui-vendor
cd rmlui-vendor && git checkout 0ae381e00d7426762bb5ed897973366358b16642
DEST=/w/Unreal/VcHost/Plugins/VaCuus/Source/ThirdParty/RmlUi
mkdir -p $DEST
cp -r Include Source Backends LICENSE.txt readme.md $DEST/
rm -rf $DEST/Source/Lua $DEST/Source/SVG $DEST/Source/Lottie
echo "0ae381e00d7426762bb5ed897973366358b16642" > $DEST/VENDORED_SHA.txt
```

(`Backends/` is kept as reference material only — never compiled.)

- [x] **Step 1.2: Commit:** `git add Source/ThirdParty && git commit -m "vendor: RmlUi @0ae381e (benchmarked baseline)"`

---

### Task 2: VaCuusRml module — compile RmlUi Core under UBT

**Files:** Create: `Source/VaCuusRml/VaCuusRml.Build.cs`,
`Source/VaCuusRml/Private/VaCuusRmlModule.cpp`; Modify: `VaCuus.uplugin`.

- [x] **Step 2.1: Build.cs:**

```csharp
using UnrealBuildTool;
using System.IO;

public class VaCuusRml : ModuleRules
{
    public VaCuusRml(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.NoPCHs;
        bUseUnity = false;                       // third-party sources, keep TU boundaries
        UndefinedIdentifierWarningLevel = WarningLevel.Off;
        bEnableExceptions = false;
        CppStandard = CppStandardVersion.Cpp20;

        string RmlRoot = Path.Combine(ModuleDirectory, "../ThirdParty/RmlUi");
        PublicIncludePaths.Add(Path.Combine(RmlRoot, "Include"));

        PublicDefinitions.Add("RMLUI_STATIC_LIB=1");
        PublicDefinitions.Add("RMLUI_CUSTOM_RTTI=1");   // UE builds with -fno-rtti

        PrivateDependencyModuleNames.AddRange(new[] { "Core" });
        AddEngineThirdPartyPrivateStaticDependencies(Target, "FreeType2");

        // Compile RmlUi Core directly (Source/Core, Core/Elements, Core/Layout,
        // Core/FontEngineDefault). UBT picks up all .cpp under the module dir, so the
        // sources are added via ConditionalAddModuleDirectory-equivalent: keep RmlUi
        // sources OUTSIDE this module dir and add them explicitly:
        foreach (string Dir in new[] { "Source/Core", "Source/Core/Elements",
                 "Source/Core/Layout", "Source/Core/FontEngineDefault" })
        {
            string Abs = Path.Combine(RmlRoot, Dir);
            if (Directory.Exists(Abs))
                PrivateIncludePaths.Add(Abs);
        }
    }
}
```

**Important mechanism note:** UBT only compiles sources inside the module directory. The
straightforward approach: create relay TUs — a small script generates
`Source/VaCuusRml/Private/Gen/relay_<name>.cpp` files, each `#include`-ing one vendored
RmlUi `.cpp`. Write the generator once:

- [x] **Step 2.2: Relay generator** `Source/VaCuusRml/gen_relays.sh`:

```bash
#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"; RML="$ROOT/../ThirdParty/RmlUi/Source"
OUT="$ROOT/Private/Gen"; rm -rf "$OUT"; mkdir -p "$OUT"
find "$RML/Core" -name '*.cpp' | sort | while read -r f; do
  rel="$(realpath --relative-to="$OUT" "$f")"
  base="${f#"$RML"/}"; base="${base%.cpp}"; name="$(echo "$base" | tr '/' '_')"
  printf '#include "%s"\n' "$rel" > "$OUT/relay_${name}.cpp"
done
echo "generated $(ls "$OUT" | wc -l) relay TUs"
```

Run it: `bash Source/VaCuusRml/gen_relays.sh`. Commit the generated files (they change
only when the vendored SHA changes).

- [x] **Step 2.3: Module cpp** (`Private/VaCuusRmlModule.cpp`):

```cpp
#include "Modules/ModuleManager.h"
IMPLEMENT_MODULE(FDefaultModuleImpl, VaCuusRml)
```

- [x] **Step 2.4:** Add `{"Name":"VaCuusRml","Type":"Runtime","LoadingPhase":"PreDefault"}`
  to `VaCuus.uplugin` Modules array.
- [x] **Step 2.5: Build.** Expected: RmlUi compiles. Likely friction to resolve here (this
  is the task's real work): missing `<ft2build.h>` include path (check the engine's
  `FreeType2` public includes), `RMLUI_CUSTOM_RTTI` requiring `Rml::Detail` macro use in
  a couple of TUs, shadow/sign warnings (suppress via `bWarningsAsErrors = false` for
  this module only if needed).
- [x] **Step 2.6: Commit:** `git commit -am "feat: VaCuusRml module compiles vendored RmlUi Core"`

---

### Task 3: Rml boot — system/file interfaces + headless boot test

**Files:** Create: `Source/VaCuus/Private/VaCuusSystemInterface.h/.cpp`,
`Source/VaCuus/Private/VaCuusFileInterface.h/.cpp`, `Source/VaCuus/Public/VaCuusEngine.h`,
`Source/VaCuus/Private/VaCuusEngine.cpp`, `Source/VaCuus/Private/Tests/VaCuusBootTest.cpp`;
Modify: `Source/VaCuus/VaCuus.Build.cs` (add `VaCuusRml` to PrivateDependencyModuleNames).

- [x] **Step 3.1: Write the failing test** (`VaCuusBootTest.cpp`):

```cpp
#include "Misc/AutomationTest.h"
#include "VaCuusEngine.h"
#include <RmlUi/Core.h>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusBootTest, "VaCuus.Core.Boot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusBootTest::RunTest(const FString& Parameters)
{
    FVaCuusEngine& Engine = FVaCuusEngine::Get();
    TestTrue(TEXT("Initialized"), Engine.Initialize());

    Rml::Context* Ctx = Rml::CreateContext("boot_test", Rml::Vector2i(1280, 720));
    TestNotNull(TEXT("Context"), Ctx);

    Rml::ElementDocument* Doc = Ctx->LoadDocumentFromMemory(
        R"(<rml><head><style>body{display:block;width:100px;}</style></head>)"
        R"(<body><div id="probe">hello</div></body></rml>)");
    TestNotNull(TEXT("Document"), Doc);
    Doc->Show();
    Ctx->Update();
    TestNotNull(TEXT("Probe element"), Doc->GetElementById("probe"));

    Rml::RemoveContext("boot_test");
    Engine.Shutdown();
    return true;
}
```

- [x] **Step 3.2: Run to verify failure** (compile error: no `FVaCuusEngine`):

```bash
./Engine/Build/BatchFiles/Linux/Build.sh VcHostEditor Linux Development -project=/w/Unreal/VcHost/VcHost.uproject
```

- [x] **Step 3.3: Implement.** `VaCuusSystemInterface` (`Rml::SystemInterface`):
  `GetElapsedTime()` → `FPlatformTime::Seconds() - StartTime`; `LogMessage(type, msg)` →
  `UE_LOG(LogVaCuus, ...)` mapped by type, return true. `VaCuusFileInterface`
  (`Rml::FileInterface`): Open/Close/Read/Seek/Tell/Length over `IFileHandle*`
  (`FPlatformFileManager::Get().GetPlatformFile().OpenRead`), with a configurable root
  list — M1 roots: `<Project>/Content/DevUI/` and absolute paths. `FVaCuusEngine`
  (singleton): owns both interfaces + a **null render interface stub for boot tests**
  (RmlUi requires a render interface at Initialise; provide
  `FVcNullRenderInterface : Rml::RenderInterface` with no-op CompileGeometry returning
  handle 1, etc. — lives in VaCuus for test use); `Initialize()` = set interfaces +
  `Rml::Initialise()` + `LoadFontFace` for a default font shipped at
  `Content/DevUI/fonts/LatoLatin-Regular.ttf` (copy from vendored
  `ThirdParty/RmlUi/Samples/assets` — wait, Samples were not vendored; copy the font from
  `/tmp/.../scratchpad/bench/RmlUi/Samples/assets/` or any TTF; commit it);
  `Shutdown()` = `Rml::Shutdown()`. Idempotent (ref-count Init calls).
- [x] **Step 3.4: Run test:**

```bash
/w/Unreal/UnrealEngine/Engine/Binaries/Linux/UnrealEditor-Cmd \
  /w/Unreal/VcHost/VcHost.uproject -ExecCmds="Automation RunTests VaCuus.Core.Boot; Quit" \
  -unattended -nullrhi -nosplash -log 2>&1 | grep -E "VaCuus.Core.Boot|Result"
```

Expected: `Test Completed. Result={Passed}`.
- [x] **Step 3.5: Commit:** `git commit -am "feat: Rml boot lifecycle + system/file interfaces (VaCuus.Core.Boot green)"`

---

### Task 4: Command buffer + resource tables (pure CPU, TDD)

**Files:** Create: `Source/VaCuusRender/VaCuusRender.Build.cs` (deps: Core, CoreUObject,
Engine, RHI, RenderCore, Renderer, SlateCore, Slate, Projects, VaCuus, VaCuusRml),
`Source/VaCuusRender/Public/VcCommandBuffer.h`,
`Source/VaCuusRender/Private/Tests/VcRecorderTest.cpp`. Modify: `VaCuus.uplugin`
(add `VaCuusRender`, `LoadingPhase: PostConfigInit` for shader mapping).

- [x] **Step 4.1: Define the command model** (`VcCommandBuffer.h`):

```cpp
#pragma once
#include "CoreMinimal.h"

using FVcGeometryHandle = uint32;   // index+1 into geometry table; 0 = invalid
using FVcTextureHandle  = uint32;

enum class EVcCommandType : uint8 { DrawGeometry, SetScissor, DisableScissor, SetTransform };

struct FVcCommand
{
    EVcCommandType Type;
    FVcGeometryHandle Geometry = 0;
    FVcTextureHandle  Texture  = 0;    // 0 = untextured
    FVector2f Translation = FVector2f::ZeroVector;
    FIntRect  Scissor;
    FMatrix44f Transform = FMatrix44f::Identity;
};

struct FVcVertex { FVector2f Position; FColor Color; FVector2f UV; };   // matches Rml::Vertex layout

struct FVcGeometryData { TArray<FVcVertex> Vertices; TArray<int32> Indices; };
struct FVcTextureData  { FIntPoint Size = FIntPoint::ZeroValue; TArray<uint8> RGBA; };

struct FVcCommandBuffer
{
    uint64 Generation = 0;
    FIntPoint ViewSize = FIntPoint::ZeroValue;
    TArray<FVcCommand> Commands;
    // Resources first referenced by this buffer travel with it (UI->RT ownership handoff):
    TMap<FVcGeometryHandle, FVcGeometryData> NewGeometry;
    TMap<FVcTextureHandle,  FVcTextureData>  NewTextures;
    TArray<FVcGeometryHandle> ReleasedGeometry;   // release AFTER this buffer retires
    TArray<FVcTextureHandle>  ReleasedTextures;
};
```

- [x] **Step 4.2: Failing tests** (`VcRecorderTest.cpp`) — three
  `IMPLEMENT_SIMPLE_AUTOMATION_TEST` cases under `VaCuus.Render.Recorder`:
  (a) `CompileGeometry` returns distinct non-zero handles and stashes vertex copies into
  `NewGeometry` of the *next published* buffer; (b) `ReleaseGeometry` before publish moves
  the handle to `ReleasedGeometry` of the same buffer (create+release same frame is
  legal); (c) publishing produces monotonically increasing `Generation` and a fresh
  command array (double publish without new commands → empty buffer, not stale copy).
  Write against the `FVcRecordingRenderInterface` API from Task 5 (it will not compile
  yet — that is the failing state).
- [x] **Step 4.3: Build → expect compile failure** (missing recorder class).
- [x] **Step 4.4: Commit the test + header:** `git commit -am "test: recorder contract (red)"`

---

### Task 5: Recording RenderInterface

**Files:** Create: `Source/VaCuusRender/Public/VcRecordingRenderInterface.h`,
`Private/VcRecordingRenderInterface.cpp`.

- [x] **Step 5.1: Implement** (verify signatures against vendored `RenderInterface.h`;
  the 6.x API is compiled-geometry based):

```cpp
#pragma once
#include "VcCommandBuffer.h"
#include <RmlUi/Core/RenderInterface.h>

class VACUUSRENDER_API FVcRecordingRenderInterface : public Rml::RenderInterface
{
public:
    // --- Rml::RenderInterface ---
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> Vertices,
                                                Rml::Span<const int> Indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle Handle, Rml::Vector2f Translation,
                        Rml::TextureHandle Texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle Handle) override;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& OutDimensions,
                                   const Rml::String& Source) override;   // sync header+decode in M1
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> SourceData,
                                       Rml::Vector2i Dimensions) override;
    void ReleaseTexture(Rml::TextureHandle Handle) override;

    void EnableScissorRegion(bool bEnable) override;
    void SetScissorRegion(Rml::Rectanglei Region) override;
    void SetTransform(const Rml::Matrix4f* Transform) override;

    // --- VaCuus ---
    void BeginFrame(FIntPoint ViewSize);
    TUniquePtr<FVcCommandBuffer> EndFrameAndPublish();   // moves the accumulated buffer out

private:
    uint32 NextGeometryHandle = 1, NextTextureHandle = 1;
    uint64 Generation = 0;
    TUniquePtr<FVcCommandBuffer> Pending;
};
```

Implementation notes: `LoadTexture` in M1 decodes synchronously via `IImageWrapperModule`
(PNG) — dimensions out-param filled from the decoded size (spec's async path is M2+;
record the deviation in code comment referencing spec §5). `GenerateTexture` copies the
RGBA span (font glyphs arrive this way). Rml vertex layout (`position: Vector2f`,
`colour: ColourbPremultiplied` = 4 bytes RGBA, `tex_coord: Vector2f`) is memcpy-compatible
with `FVcVertex` — add `static_assert(sizeof(Rml::Vertex) == sizeof(FVcVertex))`.

- [x] **Step 5.2: Run recorder tests:**

```bash
UnrealEditor-Cmd VcHost.uproject -ExecCmds="Automation RunTests VaCuus.Render.Recorder; Quit" -unattended -nullrhi
```

Expected: 3/3 pass.
- [x] **Step 5.3: Integration probe test** (add to VcRecorderTest.cpp): boot Rml with the
  recorder installed (replacing the null interface via `FVaCuusEngine::SetRenderInterface`
  — add that setter), load a 20-element document, `Update`+`Render`, assert
  `Commands.Num() > 0` and `NewTextures.Num() >= 1` (font atlas). Run; expect pass.
- [x] **Step 5.4: Commit:** `git commit -am "feat: recording RenderInterface (recorder tests green)"`

---

### Task 6: Global shaders + plugin shader directory

**Files:** Create: `Shaders/Private/VaCuusUI.usf`,
`Source/VaCuusRender/Private/VcUIShaders.h/.cpp`,
`Source/VaCuusRender/Private/VaCuusRenderModule.cpp`.

- [x] **Step 6.1: USF:**

```hlsl
#include "/Engine/Public/Platform.ush"

float4x4 Projection;

void MainVS(in float2 InPosition : ATTRIBUTE0, in float4 InColor : ATTRIBUTE1,
            in float2 InUV : ATTRIBUTE2, out float4 OutPosition : SV_POSITION,
            out float4 OutColor : TEXCOORD1, out float2 OutUV : TEXCOORD0)
{
    OutPosition = mul(float4(InPosition, 0, 1), Projection);
    OutColor = InColor;          // premultiplied
    OutUV = InUV;
}

Texture2D UITexture; SamplerState UISampler;
uint bUseTexture;

void MainPS(in float4 SvPosition : SV_POSITION, in float4 Color : TEXCOORD1,
            in float2 UV : TEXCOORD0, out float4 OutColor : SV_Target0)
{
    float4 Tex = bUseTexture ? UITexture.Sample(UISampler, UV) : float4(1,1,1,1);
    OutColor = Color * Tex;      // premultiplied * premultiplied
}
```

- [x] **Step 6.2: Shader classes** (`VcUIShaders.h`): `FVcUIVS`/`FVcUIPS` :
  `FGlobalShader` with `SHADER_PARAMETER` for Projection / texture / sampler /
  `bUseTexture`, `IMPLEMENT_GLOBAL_SHADER(FVcUIVS, "/Plugin/VaCuus/Private/VaCuusUI.usf", "MainVS", SF_Vertex)`
  (and PS). Vertex declaration `FVcVertexDeclaration`
  (`FVertexDeclarationElementList`: Float2 @0, Color @8, Float2 @12; stride 20).
- [x] **Step 6.3: Module startup** (`VaCuusRenderModule.cpp`):

```cpp
void FVaCuusRenderModule::StartupModule()
{
    const FString ShaderDir = FPaths::Combine(
        IPluginManager::Get().FindPlugin(TEXT("VaCuus"))->GetBaseDir(), TEXT("Shaders"));
    AddShaderSourceDirectoryMapping(TEXT("/Plugin/VaCuus"), ShaderDir);
}
```

- [x] **Step 6.4: Build; run editor once** — shader compiles at startup (watch log for
  `LogShaderCompilers` errors). **Commit.**

---

### Task 7: Replay renderer + persistent RT

**Files:** Create: `Source/VaCuusRender/Private/VcReplayRenderer.h/.cpp`.

- [x] **Step 7.1: Implement `FVcReplayRenderer`** (render-thread only):

```cpp
class FVcReplayRenderer
{
public:
    // Called on render thread. Consumes NewGeometry/NewTextures (creates RHI resources),
    // replays Commands into the persistent RT (recreating it on ViewSize change),
    // then processes Released* lists.
    void Replay(FRHICommandList& RHICmdList, const FVcCommandBuffer& Buffer);
    FTextureRHIRef GetOutputRT() const { return OutputRT; }
private:
    struct FGeo { FBufferRHIRef VB, IB; int32 NumIndices = 0; };
    TMap<FVcGeometryHandle, FGeo> Geometry;
    TMap<FVcTextureHandle, FTextureRHIRef> Textures;
    FTextureRHIRef OutputRT;   // PF_B8G8R8A8, TexCreate_RenderTargetable|ShaderResource
    uint64 LastReplayedGeneration = 0;
};
```

Replay body: begin render pass on `OutputRT` (`ERenderTargetActions::Clear_Store`,
clear to transparent black); set `FVcUIVS/PS` bound via `SetGraphicsPipelineState`
(premultiplied blend `TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha,
BO_Add, BF_One, BF_InverseSourceAlpha>`, no depth); ortho projection
(`FMatrix44f::Ortho`-style 0..W, 0..H, y-down); per command: `SetScissorRect` /
draw (`SetStreamSource` + `DrawIndexedPrimitive`). Geometry upload:
`RHICmdList.CreateBuffer` + `LockBuffer/UnlockBuffer` (static usage). Texture upload:
`RHICreateTexture(FRHITextureCreateDesc::Create2D(...))` + `RHICmdList.UpdateTexture2D`.
Transform command: multiply into projection (Rml supplies column-major `Matrix4f` — add
`static_assert` + transpose at the boundary; verify visually in Task 9).

- [x] **Step 7.2: Build clean. Commit** (no test yet — validated visually in Task 9 and
  by the golden capture in Task 10).

---

### Task 8: Slate element + widget + console command

**Files:** Create: `Source/VaCuusRender/Private/VcSlateElement.h/.cpp`,
`Private/SVaCuusHUDWidget.h/.cpp`; Modify: `Private/VaCuusRenderModule.cpp`.

- [x] **Step 8.1: `FVcSlateElement : ICustomSlateElement`.** Game thread stores per-paint
  params (widget rect in window space, latest buffer ptr) via a render-command-enqueued
  setter (Noesis pattern). Render thread:

```cpp
void FVcSlateElement::Draw_RenderThread(FRDGBuilder& GraphBuilder, const FDrawPassInputs& Inputs)
{
    if (PendingBuffer) { Replayer.Replay(GraphBuilder.RHICmdList, *PendingBuffer); PendingBuffer = nullptr; }
    if (!Replayer.GetOutputRT()) return;
    // Composite the RT into Inputs.OutputTexture at WidgetRect with premultiplied blend:
    FRDGTexture* Output = Inputs.OutputTexture;
    FRDGTextureRef UIRT = RegisterExternalTexture(GraphBuilder, Replayer.GetOutputRT(), TEXT("VaCuusUI"));
    AddDrawTexturePass(GraphBuilder, ShaderMap, UIRT, Output, WidgetRectParams /* dest rect */);
}
```

(`AddDrawTexturePass` variant with dest-rect exists in `RenderGraphUtils`/ScreenPass —
check `/w/Unreal/UnrealEngine/Engine/Source/Runtime/Renderer/Public/ScreenPass.h` and use
the simplest fitting helper; if none blends premultiplied, add a tiny composite PS to
`VaCuusUI.usf` (`MainCompositePS`) and a `GraphBuilder.AddPass` drawing a quad.)

- [x] **Step 8.2: `SVaCuusHUDWidget : SLeafWidget`.** `OnPaint`: push widget geometry to
  the element, `FSlateDrawElement::MakeCustom(OutDrawElements, LayerId, Element)`;
  `ComputeDesiredSize` = fill. `Tick` (M1 only, game thread): `Context::Update()` +
  `Context::Render()` via recorder → `EndFrameAndPublish` → enqueue buffer to element.
- [x] **Step 8.3: Console command** in module cpp:

```cpp
static FAutoConsoleCommand GVcM1HUD(TEXT("vacuus.M1HUD"),
    TEXT("Toggle the M1 test HUD overlay"), FConsoleCommandDelegate::CreateStatic(&ToggleM1HUD));
```

`ToggleM1HUD`: `FVaCuusEngine::Get().Initialize()`; create context sized to viewport;
load `Content/DevUI/m1_hud.rml`; `GEngine->GameViewport->AddViewportWidgetContent(SNew(SVaCuusHUDWidget, Ctx), 100)`;
second call removes it and tears down (shutdown order per spec §4: document → context).

- [x] **Step 8.4: Build. Commit.**

---

### Task 9: M1 HUD document + PIE visual check

**Files:** Create: `Content/DevUI/m1_hud.rml`, `Content/DevUI/m1_hud.rcss`,
`Content/DevUI/fonts/` (font TTF), `Content/DevUI/img/avatar.png`.

- [x] **Step 9.1: Author the M1 subset document** — port of the demo HUD *reduced to M1
  renderer scope* (solid fills, borders, text, one image, 2D transforms, scissor; NO
  gradients/box-shadow/filters — those are M5): player plate (name, level badge, solid
  HP/MP bars with % widths), 24-row scoreboard table, killfeed list, ability bar with
  4 slots + number keys legend, rotated compass strip (`transform: rotate(...)`).
  Base stylesheet MUST start with `div { display: block; }` etc. (RmlUi has no UA
  stylesheet — demo gotcha #1). Reuse markup structure from
  `docs/research/hud-demo/data/hud.rml` as the starting point, stripping styles outside
  the M1 feature set.
- [x] **Step 9.2: PIE check:** launch VcHost in editor, PIE, `vacuus.M1HUD` in console.
  Expected: HUD overlays the scene; text crisp; bars correctly sized; no alpha fringes
  (premultiplied path correct); toggling off cleans up with no crash; PIE stop with HUD
  visible also clean (subsystem-less M1 teardown hooked to viewport close — guard it).
- [x] **Step 9.3: Cross-check against GL3:** run the same document in the vendored GL3
  sample loader (`Backends` reference build from research is fine) side by side; fix
  transform orientation / scissor origin mismatches now (classic y-flip class of bugs).
- [x] **Step 9.4: Commit:** `git commit -am "feat: M1 HUD renders in PIE via RDG slate element"`

---

### Task 10: Measurement + §11 re-baseline

**Files:** Create: `Source/VaCuusRender/Private/VcStats.h`; Modify: recorder/replayer/widget
to instrument. Modify: `docs/superpowers/specs/2026-07-29-vacuus-architecture-design.md` (§11).

- [x] **Step 10.1: Counters** via `DECLARE_CYCLE_STAT` group `STATGROUP_VaCuus`:
  `VaCuus Update (GT)`, `VaCuus Record (GT)`, `VaCuus Replay (RT)`, `VaCuus Composite (RT)`.
  `stat vacuus` shows them.
- [x] **Step 10.2: Measure:** PIE at 1920×1080 (`r.SetRes 1920x1080`), HUD on, 60s soak;
  record the four stats (median + p99 from `stat unit`-style observation or a 60 s
  `-trace=cpu` capture opened in Unreal Insights). Also capture one Insights trace
  proving: no `FlushRenderingCommands` from VaCuus code and no render-thread wait on any
  VaCuus lock (search the trace for the VaCuus stat scopes' parents).
- [x] **Step 10.3: Re-baseline:** update spec §11 replay row with the measured number
  (replace "provisional" annotation with `measured M1: X.XX ms @1080p, N draws`), and
  paste the measurement block into `bd update VaCuus-akj.2 --notes="..."`.
- [x] **Step 10.4: Commit spec change:** `git commit -am "docs: re-baseline §11 replay budget from M1 measurement"`

---

### Task 11: Wrap-up

- [ ] **Step 11.1:** Push branch + merge to master in origin:

```bash
git push origin m1-render-spike
cd /w/Unreal/VaCuus && git merge --ff-only m1-render-spike 2>/dev/null || git merge m1-render-spike
```

- [ ] **Step 11.2:** `bd close VaCuus-akj.2 --reason="M1 accepted: <one-line numbers>" --suggest-next`
  (expect it to surface `VaCuus-akj.6` / M2).
- [ ] **Step 11.3:** File beads for every deviation/tech-debt noted during M1 (sync
  LoadTexture, game-thread Update residency, composite helper choice) as children of
  `VaCuus-akj.6` so M2 absorbs them explicitly.

---

## Acceptance (from spec §14 M1, restated)

1. M1 HUD document renders in PIE at visual parity with the GL3 reference for the M1
   feature subset.
2. Insights capture shows zero VaCuus-caused blocking waits on the render thread and no
   `FlushRenderingCommands` in steady state.
3. Replay ms measured at 1080p and spec §11 re-baselined from data.

## Plan self-review notes (done at write time)

- Spec coverage: M1 slice only (§4 replay model, §5 recorder/replay/composite path 1,
  §12 recorder unit tests, §14 M1 acceptance). Threading (§4 full), input (§8), binding
  (§6), JS (§7) intentionally out — they are M2–M4 plans.
- Deviations from spec, recorded as M2 debt (Task 11.3): `Context::Update/Render` on the
  game thread; synchronous `LoadTexture`.
- Type consistency: handles are `uint32` everywhere; `FVcCommandBuffer` is the only
  UI→RT contract; `FVcVertex` static-asserted against `Rml::Vertex`.
