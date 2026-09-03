// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusMaterialDraw.h"

#include "VaCuusDefines.h"
#include "VaCuusReplayRenderer.h" // VaCuusReplay::MakePixelToClipMatrix — the pass projection the view UB mirrors
#include "VaCuusStyleSet.h"
#include "VaCuusUIShaders.h"

#include "GameTime.h"
#include "GlobalRenderResources.h"
#include "MaterialShared.h"
#include "Materials/MaterialRenderProxy.h"
#include "PipelineStateCache.h"
#include "RHIStaticStates.h"
#include "SceneView.h"
#include "ShaderParameterUtils.h"
#include "ShowFlags.h"

#include <atomic>

// Same type names, same entry points, same .usf as the spike (commit 8d14cd5): the
// registered shader types carry over, so nothing recompiles for the rename.
IMPLEMENT_MATERIAL_SHADER_TYPE(, FVaCuusMaterialVS, TEXT("/Plugin/VaCuus/Private/VaCuusMaterial.usf"), TEXT("MainVS"), SF_Vertex);
IMPLEMENT_MATERIAL_SHADER_TYPE(, FVaCuusMaterialPS, TEXT("/Plugin/VaCuus/Private/VaCuusMaterial.usf"), TEXT("MainPS"), SF_Pixel);

/**
 * RENDERER_API but declared in a private engine header (SceneRendering.h:3135), so the
 * declaration is repeated here rather than included. It swaps the real noise textures
 * into the fabricated view UB — the same patch Slate applies to ITS fabricated UB
 * (SlateRHIRenderingPolicy.cpp:751-753); without it a material using the Noise node
 * samples a white dummy.
 */
RENDERER_API void UpdateNoiseTextureParameters(FViewUniformShaderParameters& ViewUniformShaderParameters);

/**
 * THE TIER'S MASTER SWITCH, default 1 since Task 5b (the spike ran it dark at 0). Off
 * means the recorder stops resolving style keys — `shader(<stylekey>)` then refuses
 * like any unknown key. Flipping it OFF at runtime does not undraw already-compiled
 * decorators (their descs are live resources); it stops new compiles and is honestly a
 * kill-switch, not a toggle.
 */
static TAutoConsoleVariable<int32> CVarVaCuusMaterialDecorators(
	TEXT("vacuus.MaterialDecorators"),
	1,
	TEXT("M5 material-decorator tier (spec §3.3, GO). 1 (default) = `decorator: shader(<key>)` resolves registered ")
	TEXT("UVaCuusStyleSet keys to MD_UI materials drawn in the replay pass. 0 = style keys refuse at CompileShader ")
	TEXT("(builtins and gradients unaffected)."));

/**
 * THE FREEZE REMEDY'S KILL-SWITCH, kept from the spike (same name, same default): the
 * composite cannot re-evaluate a material and the RT is written only in the
 * publish-gated replay branch (spec §2(f)), so a time-animated MD_UI material freezes
 * between publishes. 1 = a view whose live shader table holds a Material kind publishes
 * every recorded frame, clamped to engine rate (the per-recorder term in
 * EndFrameAndPublish). 0 = observe the freeze the remedy exists for.
 */
static TAutoConsoleVariable<int32> CVarVaCuusMaterialForcedRepublish(
	TEXT("vacuus.MaterialForcedRepublish"),
	1,
	TEXT("1 (default) = while a view holds a live material decorator, every recorded UI frame publishes (clamped to ")
	TEXT("one per engine frame), so the replay pass re-evaluates materials with fresh time/parameters. ")
	TEXT("0 = observe the publish-gate freeze the remedy exists for."));

namespace VaCuusMaterialDraw
{
static std::atomic<int64> GDrawCount{0};		 // render thread writes, anyone reads
static std::atomic<int64> GShaderMissCount{0};	 // render thread writes, anyone reads
static std::atomic<int64> GUnresolvedCount{0};	 // render thread writes, anyone reads
static TSet<uint64> GLoggedUnresolvedIds;		 // render thread only, latch
static bool bLoggedShaderMissThisPass = false;	 // render thread only, reset per pass by the UB build

bool IsEnabled()
{
	return CVarVaCuusMaterialDecorators.GetValueOnAnyThread() != 0;
}

bool IsForcedRepublishEnabled()
{
	return CVarVaCuusMaterialForcedRepublish.GetValueOnAnyThread() != 0;
}

int64 GetDrawCount()
{
	return GDrawCount.load(std::memory_order_relaxed);
}

int64 GetShaderMissCount()
{
	return GShaderMissCount.load(std::memory_order_relaxed);
}

int64 GetUnresolvedCount()
{
	return GUnresolvedCount.load(std::memory_order_relaxed);
}

/**
 * The synthetic view UB — Slate's own recipe for drawing UI materials outside any scene
 * (SlateRHIRenderingPolicy.cpp:706-756), reproduced: ortho FViewMatrices whose
 * "projection" is the very pixel->clip matrix the pass draws with, the RT-sized view
 * rect, SetupCommonViewUniformBufferParameters over game show flags, then the two
 * patches Slate applies on top (noise textures; BufferToSceneTextureScale positive).
 * ONE UB per replay pass with material draws — built lazily by the first such draw.
 */
static TUniformBufferRef<FViewUniformShaderParameters> CreateViewUniformBuffer(FIntPoint RTSize, const FMatrix44f& Projection)
{
	static const FEngineShowFlags DefaultShowFlags(ESFIM_Game);
	const FIntRect ViewRect(0, 0, RTSize.X, RTSize.Y);

	FViewMatrices::FMinimalInitializer Initializer;
	Initializer.ProjectionMatrix = FMatrix(Projection);
	Initializer.ConstrainedViewRect = ViewRect;
	const FViewMatrices ViewMatrices(Initializer);

	const FSetupViewUniformParametersInputs SetupInputs = {
		.EngineShowFlags = &DefaultShowFlags,
		.UnscaledViewRect = ViewRect,
		// Real app time so Time-driven materials ANIMATE across publishes — the forced
		// republish is what makes the publishes happen; this is what makes them differ.
		.Time = FGameTime::GetTimeSinceAppStart(),
	};

	FViewUniformShaderParameters Parameters;
	SetupCommonViewUniformBufferParameters(Parameters, RTSize, 1, ViewRect, ViewMatrices, ViewMatrices, SetupInputs);

	// Slate's own two post-patches (SlateRHIRenderingPolicy.cpp:744-753). VT feedback is
	// NOT wired (VT-sampling materials are refused at registration — the registry's
	// named refusal); the empty UAV keeps the UB's resource table valid where the RHI
	// validates it.
	Parameters.BufferToSceneTextureScale = FVector2f(1.0f, 1.0f);
	UpdateNoiseTextureParameters(Parameters);
	if (GEmptyStructuredBufferWithUAV && GEmptyStructuredBufferWithUAV->UnorderedAccessViewRHI)
	{
		Parameters.VTFeedbackBuffer = GEmptyStructuredBufferWithUAV->UnorderedAccessViewRHI;
	}

	return TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(Parameters, UniformBuffer_SingleFrame);
}

/**
 * THE MATERIAL-SHADER WALK — Slate's, verbatim in shape (SlateRHIRenderingPolicy.cpp
 * :1157-1170): refresh the proxy's uniform expression cache, ask the material for OUR
 * shader types, and fall back down the proxy chain (an MD_UI material still compiling
 * lands on the default UI material rather than drawing nothing). Returns the resolved
 * material, or null when the whole chain is pair-less.
 */
static const FMaterial* WalkForShaders(FRHICommandListBase& RHICmdList, const FMaterialRenderProxy*& InOutProxy,
	TShaderRef<FVaCuusMaterialVS>& OutVS, TShaderRef<FVaCuusMaterialPS>& OutPS)
{
	FMaterialShaderTypes ShaderTypes;
	ShaderTypes.AddShaderType<FVaCuusMaterialVS>();
	ShaderTypes.AddShaderType<FVaCuusMaterialPS>();

	const FMaterialRenderProxy* Proxy = InOutProxy;
	while (Proxy)
	{
		const FMaterial* Candidate = Proxy->UpdateUniformExpressionCacheIfNeeded(RHICmdList, GMaxRHIFeatureLevel);
		FMaterialShaders Shaders;
		if (Candidate && Candidate->TryGetShaders(ShaderTypes, nullptr, Shaders))
		{
			Shaders.TryGetShader(SF_Vertex, OutVS);
			Shaders.TryGetShader(SF_Pixel, OutPS);
			if (OutVS.IsValid() && OutPS.IsValid())
			{
				InOutProxy = Proxy;
				return Candidate;
			}
		}
		Proxy = Proxy->GetFallback(GMaxRHIFeatureLevel);
	}

	return nullptr;
}

bool DrawMaterial_RenderThread(FRHICommandList& RHICmdList, FPassState& PassState, FIntPoint RTSize,
	const FMatrix44f& DrawMatrix, uint64 StableId, const FString& Key, const FVector2f& ElementSize,
	FRHIBuffer* VertexBuffer, FRHIBuffer* IndexBuffer, int32 NumVertices, int32 NumIndices,
	FRHIDepthStencilState* DepthStencilState, uint32 StencilRef)
{
	check(IsInRenderingThread());

	const FMaterialRenderProxy* Proxy = FVaCuusStyleRegistry::ResolveProxy_RenderThread(StableId);
	if (!Proxy)
	{
		// The unregistered-under-a-live-draw refusal the registry documents: the mirror
		// dropped the id before the game-side root could drop the material, so this can
		// only ever be an absent entry, never a dangling proxy. Latched per id — a
		// forced-republishing view would otherwise repeat it every frame.
		GUnresolvedCount.fetch_add(1, std::memory_order_relaxed);
		bool bAlreadyLogged = false;
		GLoggedUnresolvedIds.Add(StableId, &bAlreadyLogged);
		if (!bAlreadyLogged)
		{
			UE_LOG(LogVaCuus, Warning,
				TEXT("DrawShader: style key '%s' (id %llu) is no longer registered — the decorator draws nothing. ")
				TEXT("Live documents keep their compiled keys until they restyle or reload"),
				*Key, StableId);
		}
		return false;
	}

	if (!PassState.ViewUB.IsValid())
	{
		// First material draw of this pass: build the pass's one view UB, and reset the
		// per-pass shader-miss latch with it (the two have the same lifetime).
		PassState.ViewUB = CreateViewUniformBuffer(RTSize, VaCuusReplay::MakePixelToClipMatrix(RTSize));
		bLoggedShaderMissThisPass = false;
	}

	TShaderRef<FVaCuusMaterialVS> VertexShader;
	TShaderRef<FVaCuusMaterialPS> PixelShader;
	const FMaterial* Material = WalkForShaders(RHICmdList, Proxy, VertexShader, PixelShader);
	if (!Material)
	{
		// The async-compile transient (the spike's frame-2 observation): skipped draw +
		// one Verbose line per pass until TryGetShaders yields — which the fallback walk
		// does naturally once the shader map lands. The registration-time pre-warm
		// exists to make this window end before the first draw.
		GShaderMissCount.fetch_add(1, std::memory_order_relaxed);
		if (!bLoggedShaderMissThisPass)
		{
			bLoggedShaderMissThisPass = true;
			UE_LOG(LogVaCuus, Verbose,
				TEXT("DrawShader: no FVaCuusMaterialVS/PS pair down the proxy chain for '%s' yet (shader map still ")
				TEXT("compiling?) — draw skipped this pass"),
				*Key);
		}
		return false;
	}

	// A material draw is a full pipeline, not a PS swap — the VS differs from the
	// recorded-draw pipeline too. Everything else matches it: SAME vertex declaration,
	// SAME premultiplied One/InvSrcAlpha blend (the .usf maps material blend modes onto
	// it, so no blend-state switch either), so PSO count grows by one per distinct
	// material shader map, cached by the engine's PSO cache like any other. N
	// consecutive draws with one material bind once (the PassState memo); the caller
	// invalidates ITS pipeline memo after us (EBoundPS::Material in ReplayCommands).
	if (PassState.BoundMaterialPS != PixelShader.GetPixelShader() || PassState.BoundDepthStencil != DepthStencilState)
	{
		PassState.BoundMaterialPS = PixelShader.GetPixelShader();
		PassState.BoundDepthStencil = DepthStencilState;
		PassState.BoundStencilRef = int64(StencilRef);

		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
		GraphicsPSOInit.BlendState =
			TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI();
		GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
		// The CALLER's, since bead VaCuus-4ik: inside a clip-masked subtree this is the stencil
		// EQUAL test, and hardcoding "no stencil" here is what would let a material decorator
		// paint outside the scroll container every other draw in the frame respects.
		GraphicsPSOInit.DepthStencilState = DepthStencilState;
		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GVaCuusVertexDeclaration.VertexDeclarationRHI;
		GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
		GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;
		SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, StencilRef);
	}
	else if (PassState.BoundStencilRef != int64(StencilRef))
	{
		// Same pipeline, deeper (or shallower) mask level. Command-list state, no PSO switch.
		PassState.BoundStencilRef = int64(StencilRef);
		RHICmdList.SetStencilRef(StencilRef);
	}

	{
		FRHIBatchedShaderParameters& BatchedParameters = RHICmdList.GetScratchShaderParameters();
		VertexShader->SetParameters(BatchedParameters, PassState.ViewUB, DrawMatrix);
		RHICmdList.SetBatchedShaderParameters(VertexShader.GetVertexShader(), BatchedParameters);
	}
	{
		FRHIBatchedShaderParameters& BatchedParameters = RHICmdList.GetScratchShaderParameters();
		PixelShader->SetParameters(BatchedParameters, PassState.ViewUB, Proxy, *Material, ElementSize);
		RHICmdList.SetBatchedShaderParameters(PixelShader.GetPixelShader(), BatchedParameters);
	}

	RHICmdList.SetStreamSource(0, VertexBuffer, 0);
	RHICmdList.DrawIndexedPrimitive(IndexBuffer,
		/*BaseVertexIndex=*/0, /*FirstInstance=*/0, uint32(NumVertices),
		/*StartIndex=*/0, uint32(NumIndices / 3), /*NumInstances=*/1);
	GDrawCount.fetch_add(1, std::memory_order_relaxed);
	return true;
}

void PreWarmProxy_RenderThread(FRHICommandListImmediate& RHICmdList, const FMaterialRenderProxy* Proxy)
{
	check(IsInRenderingThread());

	const FMaterialRenderProxy* Resolved = Proxy;
	TShaderRef<FVaCuusMaterialVS> VertexShader;
	TShaderRef<FVaCuusMaterialPS> PixelShader;
	const FMaterial* Material = WalkForShaders(RHICmdList, Resolved, VertexShader, PixelShader);

	// The measurement hook for the report's "did the frame-2 transient survive?": a
	// pair found HERE means the first draw cannot miss; "not yet" here followed by no
	// draw-time Verbose miss means the map landed in between; a draw-time miss after a
	// "not yet" is the transient surviving pre-warm.
	UE_LOG(LogVaCuus, Verbose, TEXT("Style pre-warm: material shader pair %s at registration (proxy %p)"),
		Material ? TEXT("READY") : TEXT("not compiled yet — first draws may skip until the map lands"),
		static_cast<const void*>(Proxy));
}
} // namespace VaCuusMaterialDraw
