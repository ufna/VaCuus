// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "PixelFormat.h" // EPixelFormat + IsFloatFormat for VaCuusCompositeWantsLinearOutput
#include "RenderResource.h"
#include "ShaderParameterStruct.h"

#include "VaCuusCommandBuffer.h" // VaCuusMaxGradientStops pins FVaCuusGradientPS's arrays

/**
 * Parameters for the VaCuus UI shader pair, shared between VS and PS: each
 * stage binds only what survives in its parameter map (VS: Projection;
 * PS: texture path), unused members are simply skipped.
 *
 * UITexture/UISampler must always be bound when drawing with the PS — the
 * shader samples them dynamically, so they stay in the parameter map even for
 * untextured draws (bUseTexture = 0). Use GWhiteTexture as the dummy.
 */
BEGIN_SHADER_PARAMETER_STRUCT(FVaCuusUIShaderParameters, )
	SHADER_PARAMETER(FMatrix44f, Projection)
	SHADER_PARAMETER_TEXTURE(Texture2D, UITexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, UISampler)
	SHADER_PARAMETER(uint32, bUseTexture)

	/**
	 * ENGINE-TEXTURE READ MODES (spec 2026-08-22 §4). Both are 0 for every draw of a
	 * VaCuus-owned texture, which is what makes the ordinary path free.
	 *
	 * TextureEncoding: 0 = Raw (the sampler already yields display-encoded values, which is
	 * true of our own PF_R8G8B8A8-without-sRGB uploads), 1 = EncodeFromLinear (the sampler
	 * decoded an _SRGB format, or the format is float and the content is linear -- either
	 * way the pipeline's sRGB-encoded RT contract needs the curve re-applied).
	 *
	 * TextureAlphaMode: 0 = Premultiplied (the RmlUi contract), 1 = Opaque (force A to 1 --
	 * a SceneCapture render target usually carries A = 0, which the premultiplied blend
	 * would render fully invisible), 2 = Straight (an imported texture asset's alpha).
	 */
	SHADER_PARAMETER(uint32, TextureEncoding)
	SHADER_PARAMETER(uint32, TextureAlphaMode)
END_SHADER_PARAMETER_STRUCT()

class FVaCuusUIVS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVaCuusUIVS);
	SHADER_USE_PARAMETER_STRUCT(FVaCuusUIVS, FGlobalShader);
	using FParameters = FVaCuusUIShaderParameters;
};

class FVaCuusUIPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVaCuusUIPS);
	SHADER_USE_PARAMETER_STRUCT(FVaCuusUIPS, FGlobalShader);
	using FParameters = FVaCuusUIShaderParameters;
};

/**
 * Composites the replayer's premultiplied UI RT into the Slate elements
 * texture. Paired with the engine's FScreenPassVS via AddDrawScreenPass and a
 * premultiplied One/InvSrcAlpha blend state — none of the AddDrawTexturePass
 * variants take a blend state (they copy/overwrite), hence this trivial PS.
 *
 * THE PF_FloatRGBA PERMUTATION (M6 Task 5, spec §3.2 — M5's explicit assignment,
 * arch spec :194-195 "the PF_FloatRGBA editor/PIE composition permutation (gamma
 * 1.0)"). The M1 contract writes the UI RT's sRGB-encoded premultiplied pixels RAW
 * into the Slate elements texture — correct exactly because an LDR elements texture
 * holds display-encoded pixels. A FLOAT elements texture does not: with
 * r.DefaultBackBufferPixelFormat=3 the backbuffer is PF_FloatRGBA
 * (RendererSettings.cpp:37-43 via GetViewportPixelFormat, SlateRHIRenderer.cpp:779-781),
 * the engine pins DisplayGamma to 1.0 (UnrealEngine.cpp:2501-2504 via
 * IsDefaultBackBufferLinearSDR, RenderCore.cpp:464-473), and Slate's own element
 * batches skip their LinearToSrgb encode (SlateRHIRenderingPolicy.cpp:1508-1509 —
 * DisplayGamma 1.0 makes GammaCorrect a no-op, SlateElementPixelShader.usf:87-99).
 * Every neighbouring pixel in that target is LINEAR; raw sRGB bytes composited into
 * it read ~2.2-gamma too bright. LinearOutput=1 decodes at composite time:
 * un-premultiply, sRGBToLinear (the exact inverse of the LinearToSrgb Slate would
 * have applied, GammaCorrectionCommon.ush:45-60), re-premultiply.
 */
class FVaCuusCompositePS : public FGlobalShader
{
public:
	class FLinearOutput : SHADER_PERMUTATION_BOOL("VACUUS_LINEAR_OUTPUT");
	using FPermutationDomain = TShaderPermutationDomain<FLinearOutput>;

	DECLARE_GLOBAL_SHADER(FVaCuusCompositePS);
	SHADER_USE_PARAMETER_STRUCT(FVaCuusCompositePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CompositeTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, CompositeSampler)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};

/**
 * Whether compositing into OutputFormat needs FVaCuusCompositePS's LinearOutput
 * permutation — i.e. whether the target holds LINEAR (gamma 1.0) pixels.
 *
 * The predicate is the pixel format, not a mirrored engine setting, because the
 * format is the one fact the render thread holds that is true in EVERY route to a
 * float target: linear-SDR (r.DefaultBackBufferPixelFormat=3 — the editor/PIE case
 * the arch spec names) and scRGB HDR (GRHIHDRDisplayOutputFormat = PF_FloatRGBA,
 * SlateRHIRenderer.cpp:763-766) are both gamma-1.0 targets by engine contract
 * (UnrealEngine.cpp:2501-2504; SlateRHIRenderingPolicy.cpp:1508-1509 pins 1.0
 * whenever the elements texture is an HDR display target). IsFloatFormat is
 * PixelFormat.h:382-399. The one target this deliberately does NOT correct is
 * HDR10 (PF_A2B10G10R10 under PQ): not a float format, needs an ST2084 encode this
 * plugin does not ship — the LDR-only scope M5 confirmed (backdrop-glass.md §6),
 * where glass is disabled by the same discriminator family.
 *
 * Free function, not a lambda at the call site, so the decision has a unit test
 * (VaCuus.Render.Composite.LinearOutputSelection) that runs under NullRHI.
 */
bool VaCuusCompositeWantsLinearOutput(EPixelFormat OutputFormat);

/**
 * One direction of the M5 glass blur (spec §2(c)): separable gaussian at half-res,
 * paired with FScreenPassVS via AddDrawScreenPass exactly like the composite above.
 * The parameter scheme is the engine's own Slate blur verbatim
 * (FSlatePostProcessBlurPS, SlateRHIRenderer/Private/SlatePostProcessor.cpp:636-653):
 * paired weight/offset packing so bilinear filtering halves the tap count, direction and
 * texel size in one vector, bilinear-safe UV bounds. MAX_BLUR_SAMPLES matches the .usf
 * array — a 125-tap kernel ceiling: ~41.7 sigma in HALF-RES texels, i.e. ~80px of
 * view-space sigma at 1:1 scale (the earlier ~40px figure counted the texels once).
 */
class FVaCuusBlurPS : public FGlobalShader
{
public:
	static constexpr int32 MaxBlurSamples = 63;

	DECLARE_GLOBAL_SHADER(FVaCuusBlurPS);
	SHADER_USE_PARAMETER_STRUCT(FVaCuusBlurPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, BlurTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, BlurSampler)
		SHADER_PARAMETER_ARRAY(FVector4f, WeightAndOffsets, [MaxBlurSamples])
		SHADER_PARAMETER(int32, SampleCount)
		SHADER_PARAMETER(FVector4f, BufferSizeAndDirection)
		SHADER_PARAMETER(FVector4f, UVBounds)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};

/**
 * The glass draw (spec §2(a)): renders the clip-mask geometry (or a generated quad)
 * through FVaCuusUIVS's Projection into the Slate elements texture, sampling the blurred
 * half-res RT at the output pixel's position. Bound with SrcAlpha/InvSrcAlpha so the
 * mask's vertex alpha lerps blurred-over-sharp at the edge.
 */
class FVaCuusGlassPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVaCuusGlassPS);
	SHADER_USE_PARAMETER_STRUCT(FVaCuusGlassPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GlassTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, GlassSampler)
		SHADER_PARAMETER(FVector4f, GlassUVTransform)
		SHADER_PARAMETER(FVector4f, GlassUVBounds)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};

/**
 * The M5 decorator PS (spec §2(e)): fills recorded RmlUi geometry with a gradient or a
 * builtin effect — the replayer's DrawShader case, its first mid-pass PSO switch. One
 * shader with a Mode branch rather than permutations: every mode shares the parameter
 * block and the draw path, the branch is uniform (zero divergence within a draw), and
 * one PSO per mode would cost three more pipelines for no measured win at UI draw
 * counts. Paired with FVaCuusUIVS — the geometry is ordinary recorded Rml geometry
 * (position/color/UV), only the fill computation differs from FVaCuusUIPS.
 *
 * Gradient math and packing follow RmlUi's own reference backend so the semantics
 * cannot drift from what the dictionaries mean (RmlUi_Renderer_GL3.cpp:96-153 — the
 * shader — and :1625-1688 — the dictionary-to-uniform conversion):
 *   linear: P = p0, V = p1 - p0;  radial: P = center, V = 1/radius (2d curvature);
 *   conic:  P = center, V = (cos angle, sin angle).
 * Stop positions ride four-per-float4 (StopPositions[i>>2][i&3]) so the fixed-size
 * uniform block stays 4 vectors instead of 16 padded scalars.
 */
class FVaCuusGradientPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVaCuusGradientPS);
	SHADER_USE_PARAMETER_STRUCT(FVaCuusGradientPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, GradientMode)
		SHADER_PARAMETER(uint32, bRepeating)
		SHADER_PARAMETER(FVector2f, GradientP)
		SHADER_PARAMETER(FVector2f, GradientV)
		SHADER_PARAMETER(FVector2f, BuiltinDimensions)
		SHADER_PARAMETER(int32, NumStops)
		SHADER_PARAMETER_ARRAY(FVector4f, StopColors, [16])
		SHADER_PARAMETER_ARRAY(FVector4f, StopPositions, [4])
	END_SHADER_PARAMETER_STRUCT()
};

// The parameter arrays above are sized by the shared stop cap; a drifted copy here would
// truncate silently at the bind. (VaCuusMaxGradientStops lives in VaCuusCommandBuffer.h.)
static_assert(VaCuusMaxGradientStops == 16,
	"FVaCuusGradientPS's StopColors[16]/StopPositions[4] arrays and VaCuusGradient.usf's MAX_NUM_STOPS "
	"are sized for VaCuusMaxGradientStops == 16 — change all three together");

/**
 * The builtin registry behind `decorator: shader(<key>)` — a static name -> PS-mode map
 * (M5 spec §3.3: the gated UVaCuusStyleSet asset tier is Task 5's business; stage 1
 * ships exactly this). The recorder consults it to refuse unknown keys at CompileShader
 * (handle 0 — suppresses that one decorator on that one element, Decorator.h:42-44 +
 * ElementEffects.cpp:196-200); the replayer consults it to pick the PS mode. One map so
 * the two cannot disagree about what exists.
 */
namespace VaCuusBuiltinShaders
{
/** PS mode for a registered key, or INDEX_NONE. Modes 0-2 are the gradients; builtins start at 3. */
int32 FindMode(const FString& Key);

/** Comma-separated known keys, for the recorder's refusal log. */
const FString& KnownKeysForLog();
} // namespace VaCuusBuiltinShaders

/**
 * Vertex declaration matching FVaCuusVertex (bit-identical to Rml::Vertex,
 * 20 bytes): Position float2 @0, Color 4 bytes @8, UV float2 @12.
 *
 * Color is VET_UByte4N, NOT VET_Color: the vertex bytes are RmlUi RGBA memory
 * order (premultiplied), and VET_UByte4N feeds the raw normalized bytes to
 * ATTRIBUTE1 in memory order (x = byte0 = R ... w = byte3 = A). VET_Color
 * would apply the FColor BGRA swizzle and swap R/B. See VaCuusCommandBuffer.h.
 */
class FVaCuusVertexDeclaration : public FRenderResource
{
public:
	FVertexDeclarationRHIRef VertexDeclarationRHI;

	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
	virtual void ReleaseRHI() override;
};

extern TGlobalResource<FVaCuusVertexDeclaration> GVaCuusVertexDeclaration;
