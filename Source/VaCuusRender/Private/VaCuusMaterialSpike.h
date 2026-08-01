// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "MaterialDomain.h"
#include "MaterialShader.h"
#include "MaterialShaderType.h"
#include "RHICommandList.h"

class UMaterialInterface;
class FMaterialRenderProxy;

/**
 * THE M5 MATERIAL SPIKE (spec §2(f), §3.3 stage 2; material-decorators.md §6): can an
 * arbitrary MD_UI UMaterial be drawn INSIDE the replay pass, without Slate, with correct
 * premultiplied blending — and what does the publish-gate freeze remedy cost?
 *
 * SPIKE CODE, DELIBERATELY NOT THE PRODUCTION SHAPE. The production tier (UVaCuusStyleSet
 * asset, publish-by-replacement snapshots with a monotonic version counter, the recorder
 * resolving `decorator: shader(<key>)` through the snapshot) is a follow-up task gated on
 * this spike's GO. What is here instead:
 *  - a process-global registry driven by console commands (vacuus.MatSpike.*),
 *  - draws INJECTED after the replayed commands (a synthetic DrawShader — the recorder
 *    is skipped on purpose; Task 4 proved the plumbing and the buffer format needs
 *    nothing new either way),
 *  - everything behind `vacuus.MaterialDecorators` (default 0).
 *
 * The two shader classes are the one part that IS the production mechanism: a
 * plugin-declared FMaterialShader pair against a plugin .usf, permutation-gated on MD_UI
 * with NO editor-only flag — the exact point the monolithic -game check is load-bearing
 * for (TextureGraph's otherwise-identical pair compiles only under
 * EShaderPermutationFlags::HasEditorOnlyData, FxMaterial_DrawMaterial.h:37-40, so it
 * proves nothing about cooked runtime; Slate's in-engine MD_UI types are the only
 * runtime proof until this spike's).
 */

/** The spike VS: transforms FVaCuusVertex by the replay pass's pixel->clip matrix. */
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
	 * shader maps or the tier is editor-only and the gate is NO-GO. The MD_UI test is
	 * Slate's own runtime-proven gate (SlateMaterialShader.cpp:29-32, :49-52).
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

/** The spike PS: evaluates the material and premultiplies per blend mode (see the .usf). */
class FVaCuusMaterialPS : public FMaterialShader
{
	DECLARE_SHADER_TYPE(FVaCuusMaterialPS, Material);

public:
	FVaCuusMaterialPS() = default;
	FVaCuusMaterialPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FMaterialShader(Initializer)
	{
	}

	static bool ShouldCompilePermutation(const FMaterialShaderPermutationParameters& Parameters)
	{
		return Parameters.MaterialParameters.MaterialDomain == MD_UI;
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}

	void SetParameters(FRHIBatchedShaderParameters& BatchedParameters,
		const TUniformBufferRef<FViewUniformShaderParameters>& ViewUniformBuffer,
		const FMaterialRenderProxy* MaterialRenderProxy, const FMaterial& Material)
	{
		SetUniformBufferParameter(BatchedParameters, GetUniformBufferParameter<FViewUniformShaderParameters>(), ViewUniformBuffer);
		// The FSceneView-free, batched-parameters overload (MaterialShader.h:88-92);
		// null Scene is an explicitly handled input (ShaderBaseClasses.cpp:264: feature
		// level falls back to GMaxRHIFeatureLevel, parameter collections to the process
		// defaults).
		FMaterialShader::SetParameters(BatchedParameters, MaterialRenderProxy, Material, static_cast<const FSceneInterface*>(nullptr));
	}
};

namespace VaCuusMaterialSpike
{
/**
 * GAME THREAD: root InMaterial, cache its render proxy, and mirror {proxy, rect} to the
 * render thread. Rect is in view pixels (the RT's space). Returns false (logged) when
 * the material is not MD_UI — the registry-validation refusal the research note names.
 */
bool Register(UMaterialInterface* InMaterial, const FIntRect& InRect);

/** GAME THREAD: drop every registered material (game-side roots and render-side mirror). */
void ClearAll();

/**
 * UI THREAD (the publish gate): true while material draws are injected AND the
 * forced-republish remedy is on — the gate then publishes every recorded frame so the
 * replay pass re-evaluates the material with fresh time/parameters. THE FREEZE REMEDY
 * (spec §2(f)): the composite cannot re-evaluate a material, and the RT is written only
 * in the publish-gated replay branch, so a time-animated MD_UI material freezes between
 * publishes; this is the cheaper of the two named remedies, measured by the spike.
 */
bool WantsForcedRepublish();

/**
 * RENDER THREAD, inside the replay render pass after the recorded commands: the
 * injected material draws. Returns the number of draws issued (added to the replay's
 * draw-call stat). Quietly zero when the cvar is off or nothing is registered.
 */
int32 DrawInjected_RenderThread(FRHICommandList& RHICmdList, FIntPoint RTSize, const FMatrix44f& Projection);

/** GAME THREAD, module shutdown: drop the render-side mirror's RHI refs before RHI teardown. */
void ReleaseRenderResources();

/** Total injected material draws this process — the test observable. */
int64 GetDrawCount();

/** Draws whose proxy walk found no usable FVaCuusMaterialVS/PS pair — the failure observable. */
int64 GetShaderMissCount();
} // namespace VaCuusMaterialSpike
