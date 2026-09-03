// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusEngineCompat.h"

#include "MaterialDomain.h"
#include "MaterialShader.h"
#include "MaterialShaderType.h"
#include "RHICommandList.h"
// Complete FViewUniformShaderParameters (SceneView.h:1157): the inline SetParameters
// below instantiates GetUniformBufferParameter<> with it, and MaterialShader.h:18 only
// forward-declares the struct. Surfaced by the M6 BuildPlugin -StrictIncludes leg.
#include "SceneView.h"

class FMaterialRenderProxy;

/**
 * MATERIALS AS DECORATORS — THE PRODUCTION TIER (M5 Task 5b, the spike's GO follow-up,
 * spec §3.3): an MD_UI UMaterial drawn INSIDE the replay pass, without Slate, filling
 * recorded RmlUi geometry — how `decorator: shader(<stylekey>)` reaches pixels.
 *
 * The shader pair below IS the spike's proven mechanism, kept verbatim (same type
 * names, same .usf, so shader maps and DDC carry over): a plugin-declared
 * FMaterialShader pair, permutation-gated on MD_UI with NO editor-only flag — the point
 * the spike's monolithic -game check proved runtime-real (TextureGraph's otherwise
 * identical pair compiles only under EShaderPermutationFlags::HasEditorOnlyData,
 * FxMaterial_DrawMaterial.h:37-40; Slate's in-engine MD_UI types and this pair are the
 * runtime proofs).
 *
 * What RETIRED with the spike: the process-global console registry (vacuus.MatSpike.*),
 * the injected post-replay draws, and the process-global forced-republish gate term.
 * The production shape: keys resolve game-side through UVaCuusStyleSet →
 * FVaCuusStyleRegistry (immutable snapshot over the command queue → the recorder mints
 * Kind=Material descs), the render thread resolves StableId → FMaterialRenderProxy from
 * the registry's mirror, and DrawMaterial below runs as a DrawShader case inside
 * ReplayCommands — recorded geometry, recorded scissor/transform state, per-view
 * forced republish driven by the recorder's own live-shader table.
 */

/** The production VS: transforms FVaCuusVertex by the replay pass's draw matrix. */
class FVaCuusMaterialVS : public FMaterialShader
{
	DECLARE_SHADER_TYPE(FVaCuusMaterialVS, Material);

public:
	FVaCuusMaterialVS() = default;
	FVaCuusMaterialVS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FMaterialShader(Initializer)
	{
		ProjectionParameter.Bind(Initializer.ParameterMap, TEXT("VaCuusProjection"));
	}

	/**
	 * MD_UI only, and — THE LOAD-BEARING DIFFERENCE from TextureGraph — no
	 * HasEditorOnlyData requirement: this permutation must exist in cooked material
	 * shader maps or the tier is editor-only. Proven in the spike's monolithic -game
	 * check (spec §3.3's GO record: zero shader misses from cooked paks). The MD_UI test
	 * is Slate's own runtime-proven gate (SlateMaterialShader.cpp:29-32, :49-52).
	 */
	static bool ShouldCompilePermutation(const FMaterialShaderPermutationParameters& Parameters)
	{
		return Parameters.MaterialParameters.MaterialDomain == MD_UI;
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}

	void SetParameters(FRHIBatchedShaderParameters& BatchedParameters,
		const TUniformBufferRef<FViewUniformShaderParameters>& ViewUniformBuffer, const FMatrix44f& Projection)
	{
		// The synthetic view UB (the Slate recipe, SlateRHIRenderingPolicy.cpp:706-756):
		// bound if the compiled VS half references View at all; harmless no-op otherwise.
		SetUniformBufferParameter(BatchedParameters, GetUniformBufferParameter<FViewUniformShaderParameters>(), ViewUniformBuffer);
		SetShaderValue(BatchedParameters, ProjectionParameter, Projection);
	}

private:
	LAYOUT_FIELD(FShaderParameter, ProjectionParameter);
};

/** The production PS: evaluates the material and premultiplies per blend mode (see the .usf). */
class FVaCuusMaterialPS : public FMaterialShader
{
	DECLARE_SHADER_TYPE(FVaCuusMaterialPS, Material);

public:
	FVaCuusMaterialPS() = default;
	FVaCuusMaterialPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FMaterialShader(Initializer)
	{
		ElementSizeParameter.Bind(Initializer.ParameterMap, TEXT("VaCuusElementSize"));
	}

	static bool ShouldCompilePermutation(const FMaterialShaderPermutationParameters& Parameters)
	{
		return Parameters.MaterialParameters.MaterialDomain == MD_UI;
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}

	/**
	 * ElementSize is the paint box in PIXELS, and it is what makes a Slate-authored UI
	 * material work here. The engine's GetUserInterfaceUV function reads the material's
	 * texture-coordinate slots by index -- slot 3 is its "Pixel Size" output, "the screen
	 * space size of the element being drawn" -- and a material that snaps to the pixel
	 * grid divides by it, so that slot cannot hold a UV. See the slot table in
	 * VaCuusMaterial.usf.
	 */
	void SetParameters(FRHIBatchedShaderParameters& BatchedParameters,
		const TUniformBufferRef<FViewUniformShaderParameters>& ViewUniformBuffer,
		const FMaterialRenderProxy* MaterialRenderProxy, const FMaterial& Material,
		const FVector2f& ElementSize)
	{
		SetUniformBufferParameter(BatchedParameters, GetUniformBufferParameter<FViewUniformShaderParameters>(), ViewUniformBuffer);
		SetShaderValue(BatchedParameters, ElementSizeParameter, ElementSize);
		// The FSceneView-free, batched-parameters overload with null Scene — an
		// explicitly handled input (ShaderBaseClasses.cpp:264: feature level falls back
		// to GMaxRHIFeatureLevel, parameter collections to the process defaults).
		// Routed through the engine-version seam: the overload's 5.6/5.7 presence is
		// unconfirmed (VaCuusEngineCompat.h hotspot 3, M6 spec §2(f)).
		VaCuusCompat::SetMaterialShaderParameters_NullScene(*this, BatchedParameters, MaterialRenderProxy, Material);
	}

private:
	LAYOUT_FIELD(FShaderParameter, ElementSizeParameter);
};

namespace VaCuusMaterialDraw
{
/**
 * Per-replay-pass state the material path keeps on the caller's stack: the synthetic
 * view uniform buffer, built LAZILY by the first material draw of the pass (a pass with
 * no material draws pays nothing) and shared by the rest — ONE UB per pass, the cost
 * the spike priced. Also the bound-PS memo that keeps N consecutive draws with the same
 * material at one PSO bind.
 */
struct FPassState
{
	TUniformBufferRef<FViewUniformShaderParameters> ViewUB;

	/** The material PS bound by the previous material draw of this pass, if any. */
	FRHIPixelShader* BoundMaterialPS = nullptr;

	/**
	 * The depth-stencil state and reference the previous material draw of this pass bound
	 * (bead VaCuus-4ik). -1 = "no material pipeline is bound", which is what the caller writes
	 * when it binds one of its own.
	 *
	 * PART OF THE MEMO KEY, NOT DECORATION: a material bound while no clip mask was active must
	 * REBIND when one becomes active, or the material fill would be the one element in the
	 * document that escapes its scroll container. Two fields because the state and the reference
	 * change independently -- the state only when masking turns on or off, the reference on every
	 * mask level -- and only the state costs a pipeline switch.
	 */
	FRHIDepthStencilState* BoundDepthStencil = nullptr;
	int64 BoundStencilRef = -1;
};

/** The tier's master switch: vacuus.MaterialDecorators (default 1 since Task 5b). Any thread. */
bool IsEnabled();

/** The freeze-remedy kill-switch: vacuus.MaterialForcedRepublish (default 1). Any thread. */
bool IsForcedRepublishEnabled();

/**
 * RENDER THREAD, inside the replay render pass: draw the recorded geometry filled by
 * the material behind StableId. Resolves the proxy from the style registry's mirror,
 * walks the proxy chain for the FVaCuusMaterial* pair, binds the full pipeline (a
 * material draw is a new PSO — the VS differs too, not just the PS), and issues the
 * indexed draw. Recorded scissor state applies — this is a recorded command's draw, not
 * an injection.
 *
 * Returns true when a draw was issued. False = skipped, in one of two named ways:
 *  - the id no longer resolves (key unregistered under a live draw): latched Warning;
 *  - the whole proxy chain is pair-less (shader map still async-compiling — the spike's
 *    frame-2 transient): one Verbose line per pass until the walk yields, which it does
 *    by itself once the map lands (the fallback walk is the honest mitigation; the
 *    registration-time pre-warm makes the window ~unobservable).
 *
 * DepthStencilState/StencilRef come from the caller's clip-mask state (bead VaCuus-4ik): the
 * material tier binds a FULL pipeline of its own, so it is the one draw path that would not
 * inherit the replayer's stencil test and would render unclipped inside a masked subtree.
 * Passing them in rather than reading them here keeps the whole protocol in ReplayCommands.
 */
bool DrawMaterial_RenderThread(FRHICommandList& RHICmdList, FPassState& PassState, FIntPoint RTSize,
	const FMatrix44f& DrawMatrix, uint64 StableId, const FString& Key, const FVector2f& ElementSize,
	FRHIBuffer* VertexBuffer, FRHIBuffer* IndexBuffer, int32 NumVertices, int32 NumIndices,
	FRHIDepthStencilState* DepthStencilState, uint32 StencilRef);

/**
 * RENDER THREAD, at registration (the style registry's pre-warm hook, Task 5b.2): run
 * the uniform-expression-cache refresh and the TryGetShaders walk once, so the work —
 * and any pair-less transient — happens at registration rate, before a document can
 * draw. Logs the outcome at Verbose; the measurement the report wants is whether a
 * first draw still misses after this ran.
 */
void PreWarmProxy_RenderThread(FRHICommandListImmediate& RHICmdList, const FMaterialRenderProxy* Proxy);

/** Total material draws issued this process — the test/demo observable. */
int64 GetDrawCount();

/** Draws skipped because the walk found no shader pair — the transient's observable. */
int64 GetShaderMissCount();

/** Draws skipped because the id did not resolve in the mirror — the unregistration observable. */
int64 GetUnresolvedCount();
} // namespace VaCuusMaterialDraw
