// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "RenderResource.h"
#include "ShaderParameterStruct.h"

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
