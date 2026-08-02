// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusUIShaders.h"

#include "VaCuusCommandBuffer.h"

#include "RHICommandList.h"

IMPLEMENT_GLOBAL_SHADER(FVaCuusUIVS, "/Plugin/VaCuus/Private/VaCuusUI.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FVaCuusUIPS, "/Plugin/VaCuus/Private/VaCuusUI.usf", "MainPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FVaCuusCompositePS, "/Plugin/VaCuus/Private/VaCuusUI.usf", "MainCompositePS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FVaCuusBlurPS, "/Plugin/VaCuus/Private/VaCuusBlur.usf", "MainBlurPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FVaCuusGlassPS, "/Plugin/VaCuus/Private/VaCuusBlur.usf", "MainGlassPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FVaCuusGradientPS, "/Plugin/VaCuus/Private/VaCuusGradient.usf", "MainGradientPS", SF_Pixel);

bool VaCuusCompositeWantsLinearOutput(EPixelFormat OutputFormat)
{
	// IsFloatFormat (PixelFormat.h:382-399) covers every float RT the Slate viewport
	// path can produce: PF_FloatRGBA from r.DefaultBackBufferPixelFormat=3
	// (RendererSettings.cpp:37-43) or scRGB HDR (SlateRHIRenderer.cpp:763-766), and
	// PF_FloatR11G11B10/PF_A32B32G32R32F should a host composite us into its own float
	// target. PF_A2B10G10R10 — the desktop SDR default's 10-bit sibling AND the HDR10
	// swapchain format — stays display-encoded on both branches and correctly reads
	// false here (the HDR10 case is the out-of-scope ST2084 story, see the header).
	return IsFloatFormat(OutputFormat);
}

namespace VaCuusBuiltinShaders
{
/**
 * Key -> VaCuusGradient.usf mode. Modes 0-2 are reserved for the three gradient kinds
 * (the .usf's own constants); builtin entries start at 3. Adding a builtin = one row
 * here + one mode branch in the .usf.
 */
// TMap<FString,...> keys hash and compare CASE-INSENSITIVELY, so shader(GLASS-PANEL)
// resolves too. Accepted: RCSS decorator values are author-controlled strings, and the
// permissive match cannot alias two distinct builtins (the registry owns its namespace).
// Documented so nobody reads exact-match semantics into it -- this project has been
// bitten by assumed case rules before (see the fname-cooked memory).
static const TMap<FString, int32>& GetRegistry()
{
	static const TMap<FString, int32> Registry = {
		{TEXT("glass-panel"), 3},
	};
	return Registry;
}

int32 FindMode(const FString& Key)
{
	const int32* Mode = GetRegistry().Find(Key);
	return Mode ? *Mode : INDEX_NONE;
}

const FString& KnownKeysForLog()
{
	static const FString Known = []
	{
		TArray<FString> Keys;
		GetRegistry().GetKeys(Keys);
		Keys.Sort();
		return FString::Join(Keys, TEXT(", "));
	}();
	return Known;
}
} // namespace VaCuusBuiltinShaders

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
