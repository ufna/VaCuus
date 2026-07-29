// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusReplayRenderer.h"

#include "VaCuusDefines.h"
#include "VaCuusUIShaders.h"

#include "GlobalRenderResources.h"
#include "GlobalShader.h"
#include "PipelineStateCache.h"
#include "RHICommandList.h"
#include "RHIResourceUtils.h"
#include "RHIStaticStates.h"
#include "ShaderParameterStruct.h"

namespace VaCuusReplay
{
/**
 * Pixel-space -> clip-space ortho in UE row-vector convention (v' = v * M):
 * x: 0..W -> -1..1, y: 0..H -> 1..-1 (y-down pixels, y-up clip). Incoming z is
 * flattened and clip z pinned to 0.5 — no depth buffer, and 0.5 sits safely
 * inside every RHI's 0..1 clip range. Orientation is verified visually in the
 * Task 9 PIE check.
 */
FMatrix44f MakePixelToClipMatrix(FIntPoint ViewSize)
{
	FMatrix44f M = FMatrix44f::Identity;
	M.M[0][0] = 2.0f / float(ViewSize.X);
	M.M[1][1] = -2.0f / float(ViewSize.Y);
	M.M[2][2] = 0.0f;
	M.M[3][0] = -1.0f;
	M.M[3][1] = 1.0f;
	M.M[3][2] = 0.5f;
	return M;
}
} // namespace VaCuusReplay

void FVaCuusReplayRenderer::Replay(FRHICommandList& RHICmdList, const FVaCuusCommandBuffer& Buffer)
{
	check(IsInRenderingThread());
	if (!ShouldConsume(Buffer))
	{
		return;
	}

	// Resources first: a buffer may create geometry/textures and draw with
	// them in the same frame.
	UploadNewResources(RHICmdList, Buffer);

	if (Buffer.ViewSize.X > 0 && Buffer.ViewSize.Y > 0)
	{
		// Tripwire for the drain contract: the Slate element ensured the RT at
		// graph-build time from this same buffer's ViewSize and registered it
		// for the composite; if EnsureOutputRT would recreate the RT HERE (mid
		// pass), the composite is sampling a stale texture this frame.
		ensureMsgf(!OutputRT.IsValid() || OutputRT->GetSizeXY() == Buffer.ViewSize,
			TEXT("Replay is recreating the RT mid-pass (%dx%d -> %dx%d) — a registered composite would sample a stale texture. ")
			TEXT("Only the newest queued buffer may be drawn; drain older ones via ConsumeResources"),
			OutputRT.IsValid() ? OutputRT->GetSizeXY().X : 0, OutputRT.IsValid() ? OutputRT->GetSizeXY().Y : 0,
			Buffer.ViewSize.X, Buffer.ViewSize.Y);

		EnsureOutputRT(RHICmdList, Buffer.ViewSize);
		ReplayCommands(RHICmdList, Buffer);
	}
	else
	{
		// Degenerate view (minimized window etc.): keep the previous RT
		// contents, still consume the buffer's resource traffic below.
		UE_LOG(LogVaCuus, Verbose, TEXT("Replay skipped draw pass: degenerate view size %dx%d"),
			Buffer.ViewSize.X, Buffer.ViewSize.Y);
	}

	RetireBufferResources(Buffer);
}

void FVaCuusReplayRenderer::ConsumeResources(FRHICommandList& RHICmdList, const FVaCuusCommandBuffer& Buffer)
{
	check(IsInRenderingThread());
	if (!ShouldConsume(Buffer))
	{
		return;
	}

	// Resource traffic only — no RT, no draws. See the header for why
	// consuming releases of a skipped buffer before the next one draws is
	// sound (the next buffer was recorded after these releases were issued).
	UploadNewResources(RHICmdList, Buffer);
	RetireBufferResources(Buffer);
}

bool FVaCuusReplayRenderer::ShouldConsume(const FVaCuusCommandBuffer& Buffer) const
{
	// Idempotence guard: consuming the same published buffer twice (e.g. two
	// paints of one UI frame) must not re-run resource creation or releases.
	if (Buffer.Generation == LastConsumedGeneration)
	{
		return false;
	}
	ensureMsgf(Buffer.Generation > LastConsumedGeneration,
		TEXT("Buffer generation went backwards (%llu after %llu) — buffers must arrive in publish order"),
		Buffer.Generation, LastConsumedGeneration);
	return true;
}

void FVaCuusReplayRenderer::RetireBufferResources(const FVaCuusCommandBuffer& Buffer)
{
	// Deferred release AFTER the buffer's commands ran (or were legitimately
	// skipped): that buffer was the last legal user of these handles. RHI refs
	// die with the map entries.
	for (const FVaCuusGeometryHandle Handle : Buffer.ReleasedGeometry)
	{
		Geometry.Remove(Handle);
	}
	for (const FVaCuusTextureHandle Handle : Buffer.ReleasedTextures)
	{
		Textures.Remove(Handle);
	}

	LastConsumedGeneration = Buffer.Generation;
}

void FVaCuusReplayRenderer::ReleaseResources()
{
	check(IsInRenderingThread());
	Geometry.Empty();
	Textures.Empty();
	OutputRT.SafeRelease();
	// Reset the guard: after teardown this replayer can only be paired with a
	// fresh recorder, whose generations restart from 1.
	LastConsumedGeneration = 0;
}

void FVaCuusReplayRenderer::EnsureOutputRT(FRHICommandList& RHICmdList, FIntPoint ViewSize)
{
	// Degenerate view (minimized window etc.): keep whatever RT exists.
	if (ViewSize.X <= 0 || ViewSize.Y <= 0)
	{
		return;
	}

	if (OutputRT.IsValid() && OutputRT->GetSizeXY() == ViewSize)
	{
		return;
	}

	// PF_B8G8R8A8 to match Slate's default backbuffer expectations for the
	// Task 8 composite; shaders write float4, so byte order is irrelevant here.
	const FRHITextureCreateDesc Desc =
		FRHITextureCreateDesc::Create2D(TEXT("VaCuusOutputRT"), ViewSize, PF_B8G8R8A8)
			.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource)
			.SetClearValue(FClearValueBinding::Transparent)
			// Invariant: outside Replay() the RT is always in SRV state.
			.SetInitialState(ERHIAccess::SRVMask);
	OutputRT = RHICmdList.CreateTexture(Desc);
}

void FVaCuusReplayRenderer::UploadNewResources(FRHICommandList& RHICmdList, const FVaCuusCommandBuffer& Buffer)
{
	for (const TPair<FVaCuusGeometryHandle, FVaCuusGeometryData>& Pair : Buffer.NewGeometry)
	{
		const FVaCuusGeometryData& Data = Pair.Value;
		FGeometry& Geo = Geometry.Add(Pair.Key);
		if (Data.Vertices.Num() == 0 || Data.Indices.Num() == 0)
		{
			// Keep the (empty) entry so draws resolve the handle and no-op.
			continue;
		}

		Geo.VB = UE::RHIResourceUtils::CreateVertexBufferFromArray<FVaCuusVertex>(
			RHICmdList, TEXT("VaCuusVB"), EBufferUsageFlags::Static, MakeConstArrayView(Data.Vertices));
		// int32 elements -> stride 4 -> 32-bit index buffer, matching the
		// memcpy'd Rml index stream.
		Geo.IB = UE::RHIResourceUtils::CreateIndexBufferFromArray<int32>(
			RHICmdList, TEXT("VaCuusIB"), EBufferUsageFlags::Static, MakeConstArrayView(Data.Indices));
		Geo.NumVertices = Data.Vertices.Num();
		Geo.NumIndices = Data.Indices.Num();
	}

	for (const TPair<FVaCuusTextureHandle, FVaCuusTextureData>& Pair : Buffer.NewTextures)
	{
		const FVaCuusTextureData& Data = Pair.Value;
		if (!ensureMsgf(Data.Size.X > 0 && Data.Size.Y > 0 && Data.RGBA.Num() == Data.Size.X * Data.Size.Y * 4,
				TEXT("Texture %llu payload mismatch: %dx%d, %d bytes"), Pair.Key, Data.Size.X, Data.Size.Y, Data.RGBA.Num()))
		{
			continue;
		}

		// PF_R8G8B8A8: the payload is RmlUi RGBA memory order (premultiplied
		// alpha) for both generated and loaded textures — see FVaCuusTextureData.
		const FRHITextureCreateDesc Desc =
			FRHITextureCreateDesc::Create2D(TEXT("VaCuusUITexture"), Data.Size, PF_R8G8B8A8)
				.SetFlags(ETextureCreateFlags::ShaderResource)
				.SetInitialState(ERHIAccess::SRVMask);
		FTextureRHIRef Texture = RHICmdList.CreateTexture(Desc);

		const FUpdateTextureRegion2D Region(0, 0, 0, 0, uint32(Data.Size.X), uint32(Data.Size.Y));
		RHICmdList.UpdateTexture2D(Texture, 0, Region, uint32(Data.Size.X) * 4u, Data.RGBA.GetData());

		Textures.Add(Pair.Key, MoveTemp(Texture));
	}
}

void FVaCuusReplayRenderer::ReplayCommands(FRHICommandList& RHICmdList, const FVaCuusCommandBuffer& Buffer)
{
	const FIntPoint RTSize = Buffer.ViewSize;
	const FMatrix44f Projection = VaCuusReplay::MakePixelToClipMatrix(RTSize);

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FVaCuusUIVS> VertexShader(ShaderMap);
	TShaderMapRef<FVaCuusUIPS> PixelShader(ShaderMap);

	RHICmdList.Transition(FRHITransitionInfo(OutputRT, ERHIAccess::SRVMask, ERHIAccess::RTV));

	FRHIRenderPassInfo RPInfo(OutputRT, ERenderTargetActions::Clear_Store);
	RHICmdList.BeginRenderPass(RPInfo, TEXT("VaCuusReplay"));
	{
		RHICmdList.SetViewport(0.0f, 0.0f, 0.0f, float(RTSize.X), float(RTSize.Y), 1.0f);

		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
		// Premultiplied-alpha blend for both color and alpha, so the RT
		// accumulates a premultiplied image ready for the Task 8 composite.
		GraphicsPSOInit.BlendState =
			TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI();
		GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GVaCuusVertexDeclaration.VertexDeclarationRHI;
		GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
		GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;
		SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

		// SetTransform state, already in UE row-vector convention (the recorder
		// memcpy's Rml's column-major matrix, which lands as the transpose).
		FMatrix44f CurrentTransform = FMatrix44f::Identity;

		for (const FVaCuusCommand& Command : Buffer.Commands)
		{
			switch (Command.Type)
			{
				case EVaCuusCommandType::DrawGeometry:
				{
					const FGeometry* Geo = Geometry.Find(Command.Geometry);
					if (!ensureMsgf(Geo, TEXT("Draw references unknown geometry handle %llu"), Command.Geometry))
					{
						break;
					}
					if (Geo->NumIndices < 3)
					{
						break; // Empty/degenerate geometry: nothing to draw.
					}

					// Row-vector composition, left to right = application order:
					// translate (pixels) -> Rml transform -> pixel-to-clip ortho.
					// Matches RmlUi backends, which compute
					// projection * user_transform * (position + translation)
					// in their column-vector convention.
					FMatrix44f Translate = FMatrix44f::Identity;
					Translate.M[3][0] = Command.Translation.X;
					Translate.M[3][1] = Command.Translation.Y;

					FVaCuusUIShaderParameters Parameters;
					Parameters.Projection = Translate * CurrentTransform * Projection;

					// PS contract: UITexture/UISampler must be live descriptors
					// even for untextured draws (Vulkan). GWhiteTexture is the
					// dummy; bUseTexture=0 skips the sample's contribution.
					FRHITexture* TextureRHI = GWhiteTexture->TextureRHI;
					uint32 bUseTexture = 0;
					if (Command.Texture != 0)
					{
						if (const FTextureRHIRef* Found = Textures.Find(Command.Texture))
						{
							TextureRHI = *Found;
							bUseTexture = 1;
						}
						else
						{
							ensureMsgf(false, TEXT("Draw references unknown texture handle %llu"), Command.Texture);
						}
					}
					Parameters.UITexture = TextureRHI;
					Parameters.UISampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
					Parameters.bUseTexture = bUseTexture;

					// One shared parameter struct, bound per stage: each stage
					// only picks up what survives in its parameter map (VS:
					// Projection; PS: texture path). 5.8 non-RDG path verified
					// in Task 6: scratch parameters + batched set per shader.
					{
						FRHIBatchedShaderParameters& BatchedParameters = RHICmdList.GetScratchShaderParameters();
						SetShaderParameters(BatchedParameters, VertexShader, Parameters);
						RHICmdList.SetBatchedShaderParameters(VertexShader.GetVertexShader(), BatchedParameters);
					}
					{
						FRHIBatchedShaderParameters& BatchedParameters = RHICmdList.GetScratchShaderParameters();
						SetShaderParameters(BatchedParameters, PixelShader, Parameters);
						RHICmdList.SetBatchedShaderParameters(PixelShader.GetPixelShader(), BatchedParameters);
					}

					RHICmdList.SetStreamSource(0, Geo->VB, 0);
					RHICmdList.DrawIndexedPrimitive(Geo->IB,
						/*BaseVertexIndex=*/0, /*FirstInstance=*/0, uint32(Geo->NumVertices),
						/*StartIndex=*/0, uint32(Geo->NumIndices / 3), /*NumInstances=*/1);
					break;
				}

				case EVaCuusCommandType::SetScissor:
				{
					// Rml scissor rects are y-down window pixels, same space as
					// the RT (verified against the recorder's Rectanglei
					// conversion; visual check in Task 9). Clamp: RHIs reject
					// rects outside the RT.
					const int32 MinX = FMath::Clamp(Command.Scissor.Min.X, 0, RTSize.X);
					const int32 MinY = FMath::Clamp(Command.Scissor.Min.Y, 0, RTSize.Y);
					const int32 MaxX = FMath::Clamp(Command.Scissor.Max.X, MinX, RTSize.X);
					const int32 MaxY = FMath::Clamp(Command.Scissor.Max.Y, MinY, RTSize.Y);
					RHICmdList.SetScissorRect(true, uint32(MinX), uint32(MinY), uint32(MaxX), uint32(MaxY));
					break;
				}

				case EVaCuusCommandType::DisableScissor:
				{
					RHICmdList.SetScissorRect(false, 0, 0, 0, 0);
					break;
				}

				case EVaCuusCommandType::SetTransform:
				{
					CurrentTransform = Command.Transform;
					break;
				}
			}
		}
	}
	RHICmdList.EndRenderPass();

	RHICmdList.Transition(FRHITransitionInfo(OutputRT, ERHIAccess::RTV, ERHIAccess::SRVMask));
}
