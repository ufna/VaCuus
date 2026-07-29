// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusUIShaders.h"

#include "VaCuusCommandBuffer.h"

#include "RHICommandList.h"

IMPLEMENT_GLOBAL_SHADER(FVaCuusUIVS, "/Plugin/VaCuus/Private/VaCuusUI.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FVaCuusUIPS, "/Plugin/VaCuus/Private/VaCuusUI.usf", "MainPS", SF_Pixel);

TGlobalResource<FVaCuusVertexDeclaration> GVaCuusVertexDeclaration;

void FVaCuusVertexDeclaration::InitRHI(FRHICommandListBase& RHICmdList)
{
	static_assert(sizeof(FVaCuusVertex) == 20, "FVaCuusVertex layout changed — update the vertex declaration");
	static_assert(STRUCT_OFFSET(FVaCuusVertex, Position) == 0, "Position expected at offset 0");
	static_assert(STRUCT_OFFSET(FVaCuusVertex, Color) == 8, "Color expected at offset 8");
	static_assert(STRUCT_OFFSET(FVaCuusVertex, UV) == 12, "UV expected at offset 12");

	FVertexDeclarationElementList Elements;
	const uint16 Stride = sizeof(FVaCuusVertex);
	Elements.Add(FVertexElement(0, STRUCT_OFFSET(FVaCuusVertex, Position), VET_Float2, 0, Stride));
	// VET_UByte4N: raw normalized bytes in memory order = RmlUi RGBA. See header.
	Elements.Add(FVertexElement(0, STRUCT_OFFSET(FVaCuusVertex, Color), VET_UByte4N, 1, Stride));
	Elements.Add(FVertexElement(0, STRUCT_OFFSET(FVaCuusVertex, UV), VET_Float2, 2, Stride));
	VertexDeclarationRHI = RHICreateVertexDeclaration(Elements);
}

void FVaCuusVertexDeclaration::ReleaseRHI()
{
	VertexDeclarationRHI.SafeRelease();
}
