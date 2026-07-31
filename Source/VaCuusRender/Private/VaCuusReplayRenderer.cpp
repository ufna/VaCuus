// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusReplayRenderer.h"

#include "VaCuusDefines.h"
#include "VaCuusStats.h"
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
	VACUUS_PERF_SCOPE(Replay);

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

	// A backwards generation is a caller bug; refuse it rather than letting
	// RetireBufferResources drag the monotonic guard backwards (which would
	// defeat the idempotence check this ensure exists to protect).
	if (!ensureMsgf(Buffer.Generation > LastConsumedGeneration,
			TEXT("Buffer generation went backwards (%llu after %llu) — buffers must arrive in publish order"),
			Buffer.Generation, LastConsumedGeneration))
	{
		return false;
	}

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
	for (const FVaCuusShaderHandle Handle : Buffer.ReleasedShaders)
	{
		Shaders.Remove(Handle);
	}

	LastConsumedGeneration = Buffer.Generation;
}

void FVaCuusReplayRenderer::ReleaseResources()
{
	check(IsInRenderingThread());
	Geometry.Empty();
	Textures.Empty();
	Shaders.Empty();
	OutputRT.SafeRelease();

	// Reset the guard: after teardown this replayer can only be paired with a fresh
	// recorder, whose generations restart from 1.
	//
	// LOAD-BEARING SINCE THE M2 TASK 12 IDLE GATE, not hygiene. OutputRT above was the only
	// copy of an idle UI's pixels -- the recorder withholds the frame that would resend them
	// -- so a path that dropped the RT and left this guard where it was would refuse the very
	// first buffer of the replacement recorder as "already consumed" and blank the UI for
	// good. See the note on the declaration for what any future partial release owes.
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

	// Shaders: the "upload" is the map insert — see the member's declaration. Add on an
	// existing key is the swap, same argument as NewTextures' re-Add (cannot actually
	// happen for shaders — handles are never recycled and a desc is immutable once
	// compiled — but the map does not need to care).
	for (const TPair<FVaCuusShaderHandle, FVaCuusShaderDesc>& Pair : Buffer.NewShaders)
	{
		Shaders.Add(Pair.Key, Pair.Value);
	}
}

void FVaCuusReplayRenderer::ReplayCommands(FRHICommandList& RHICmdList, const FVaCuusCommandBuffer& Buffer)
{
	const FIntPoint RTSize = Buffer.ViewSize;
	const FMatrix44f Projection = VaCuusReplay::MakePixelToClipMatrix(RTSize);
	int32 NumDrawCalls = 0;

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FVaCuusUIVS> VertexShader(ShaderMap);
	TShaderMapRef<FVaCuusUIPS> PixelShader(ShaderMap);
	TShaderMapRef<FVaCuusGradientPS> GradientShader(ShaderMap);

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
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;

		// THE MID-PASS PSO SWITCH (M5 spec §2(e)) — DrawShader is the first command that
		// changes pipelines inside this pass, and only the PIXEL SHADER differs: blend,
		// rasterizer, depth-stencil, vertex declaration and VS are shared, so a switch is
		// one PSO-cache hit, not a new state vector. Bound LAZILY on demand: a geometry-only
		// buffer (every pre-M5 document) binds the UI pipeline once and never switches, and
		// N consecutive DrawShaders cost one switch, not N. Scissor and viewport survive the
		// switch — they are command-list state, not PSO state (the glass draw's own pattern:
		// set once, draw through PSO binds, VaCuusSlateElement.cpp:522-536).
		enum class EBoundPS : uint8
		{
			None,
			UI,
			Gradient
		};
		EBoundPS BoundPS = EBoundPS::None;
		const auto BindPipeline = [&RHICmdList, &GraphicsPSOInit, &BoundPS, &PixelShader, &GradientShader](EBoundPS Wanted)
		{
			if (BoundPS == Wanted)
			{
				return;
			}
			BoundPS = Wanted;
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI =
				(Wanted == EBoundPS::Gradient) ? GradientShader.GetPixelShader() : PixelShader.GetPixelShader();
			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
		};

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

					BindPipeline(EBoundPS::UI);

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
					++NumDrawCalls;
					break;
				}

				case EVaCuusCommandType::DrawShader:
				{
					// M5 decorators stage 1 (spec §2(e)): the recorded RenderShader — fill
					// Command.Geometry with the compiled gradient/builtin in Command.Shader.
					// Command.Texture is recorded but unused: no RmlUi shader decorator
					// passes one (geometry.Render(offset, {}, shader),
					// DecoratorShader.cpp:61-65 and the gradient equivalents), and the
					// gradient PS samples nothing.
					const FVaCuusShaderDesc* Desc = Shaders.Find(Command.Shader);
					if (!ensureMsgf(Desc, TEXT("DrawShader references unknown shader handle %llu"), Command.Shader))
					{
						break;
					}
					const FGeometry* Geo = Geometry.Find(Command.Geometry);
					if (!ensureMsgf(Geo, TEXT("DrawShader references unknown geometry handle %llu"), Command.Geometry))
					{
						break;
					}
					if (Geo->NumIndices < 3)
					{
						break;
					}

					// The dictionary -> uniform conversion is the reference backend's
					// (RmlUi_Renderer_GL3.cpp:1646-1674): P/V per kind, stops as resolved.
					// The three Max/IsNearlyZero guards are ours — the reference divides by
					// whatever arrives, and a degenerate paint box would put NaN in the RT.
					// Memzero, not member-by-member: a shader parameter struct's default
					// constructor is EMPTY (INTERNAL_SHADER_PARAMETER_STRUCT_BEGIN's `{}`
					// suffix, ShaderParameterMacros.h:1330-1334, :1412), and the stop arrays
					// beyond NumStops would otherwise upload stack garbage as uniforms.
					FVaCuusGradientPS::FParameters PSParameters;
					FMemory::Memzero(PSParameters);
					PSParameters.bRepeating = Desc->bRepeating;
					PSParameters.BuiltinDimensions = FVector2f(FMath::Max(Desc->Dimensions.X, 1.0f), FMath::Max(Desc->Dimensions.Y, 1.0f));

					switch (Desc->Kind)
					{
						case EVaCuusShaderKind::LinearGradient:
							PSParameters.GradientMode = 0;
							PSParameters.GradientP = Desc->P0;
							PSParameters.GradientV = Desc->P1 - Desc->P0;
							if (PSParameters.GradientV.IsNearlyZero())
							{
								// Zero-length gradient line: dot(V,V) divides in the PS.
								// Any direction paints the whole box with an edge stop.
								PSParameters.GradientV = FVector2f(1.0f, 0.0f);
							}
							break;

						case EVaCuusShaderKind::RadialGradient:
							PSParameters.GradientMode = 1;
							PSParameters.GradientP = Desc->Center;
							// The reference's 2d curvature, 1/radius per axis.
							PSParameters.GradientV = FVector2f(
								1.0f / FMath::Max(Desc->Radius.X, 0.01f), 1.0f / FMath::Max(Desc->Radius.Y, 0.01f));
							break;

						case EVaCuusShaderKind::ConicGradient:
							PSParameters.GradientMode = 2;
							PSParameters.GradientP = Desc->Center;
							PSParameters.GradientV = FVector2f(FMath::Cos(Desc->Angle), FMath::Sin(Desc->Angle));
							break;

						case EVaCuusShaderKind::Builtin:
						{
							// Known-valid by the recorder's registry check; INDEX_NONE here
							// would mean the registry changed between record and replay,
							// which a static map cannot do. Clamped to the glass-panel mode
							// under the ensure so even that impossibility draws something
							// deterministic rather than reading mode garbage.
							const int32 Mode = VaCuusBuiltinShaders::FindMode(Desc->BuiltinKey);
							ensureMsgf(Mode != INDEX_NONE, TEXT("DrawShader carries unregistered builtin '%s'"), *Desc->BuiltinKey);
							PSParameters.GradientMode = uint32(FMath::Max(Mode, 3));
							break;
						}
					}

					const int32 NumStops = FMath::Min(Desc->Stops.Num(), VaCuusMaxGradientStops);
					PSParameters.NumStops = NumStops;
					for (int32 StopIndex = 0; StopIndex < NumStops; ++StopIndex)
					{
						PSParameters.StopColors[StopIndex] = Desc->Stops[StopIndex].Color;
						// Four positions per register — the PS reads [i>>2][i&3].
						PSParameters.StopPositions[StopIndex / 4][StopIndex % 4] = Desc->Stops[StopIndex].Position;
					}

					BindPipeline(EBoundPS::Gradient);

					// Same VS, same matrix math as DrawGeometry above.
					FMatrix44f Translate = FMatrix44f::Identity;
					Translate.M[3][0] = Command.Translation.X;
					Translate.M[3][1] = Command.Translation.Y;

					FVaCuusUIShaderParameters VSParameters;
					VSParameters.Projection = Translate * CurrentTransform * Projection;
					VSParameters.UITexture = GWhiteTexture->TextureRHI;
					VSParameters.UISampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
					VSParameters.bUseTexture = 0;

					{
						FRHIBatchedShaderParameters& BatchedParameters = RHICmdList.GetScratchShaderParameters();
						SetShaderParameters(BatchedParameters, VertexShader, VSParameters);
						RHICmdList.SetBatchedShaderParameters(VertexShader.GetVertexShader(), BatchedParameters);
					}
					{
						FRHIBatchedShaderParameters& BatchedParameters = RHICmdList.GetScratchShaderParameters();
						SetShaderParameters(BatchedParameters, GradientShader, PSParameters);
						RHICmdList.SetBatchedShaderParameters(GradientShader.GetPixelShader(), BatchedParameters);
					}

					RHICmdList.SetStreamSource(0, Geo->VB, 0);
					RHICmdList.DrawIndexedPrimitive(Geo->IB,
						/*BaseVertexIndex=*/0, /*FirstInstance=*/0, uint32(Geo->NumVertices),
						/*StartIndex=*/0, uint32(Geo->NumIndices / 3), /*NumInstances=*/1);
					++NumDrawCalls;
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

				case EVaCuusCommandType::PushLayer:
				case EVaCuusCommandType::PopLayer:
				case EVaCuusCommandType::CompositeLayers:
				{
					// PASS-THROUGH IN v1, DELIBERATELY (M5 spec §2(d)) — glass is not a
					// replay effect. Replay runs only on publish and the idle gate makes
					// publishes ~never on a static HUD, so a backdrop baked here would
					// freeze over the moving scene; the M5 Task 3 pipeline instead distills
					// these commands from the BUFFER into a glass list the Slate element
					// composites per engine frame (backdrop-glass.md §5, design A).
					//
					// Skipping cannot misplace any draw: a backdrop-only sequence issues no
					// geometry between its PushLayer and PopLayer — only the two composites
					// (ElementEffects.cpp:256-281). For element `filter:`/`mask-image:`
					// content (the Exit-stage stack, ElementEffects.cpp:283-315), draws
					// recorded "into" the pushed layer land directly in the base RT and the
					// filtered composite is skipped, i.e. the element renders unfiltered —
					// the pre-M5 behavior for those properties, kept for v1.
					break;
				}

				case EVaCuusCommandType::EnableClipMask:
				case EVaCuusCommandType::RenderToClipMask:
				{
					// SKIPPED IN v1 outside glass extraction (M5 spec §2(d)): applying the
					// mask needs a stencil pass this RT does not carry yet. This preserves
					// pre-M5 behavior for ordinary rounded-corner clipping exactly —
					// before M5 these two virtuals sat at RmlUi's silent no-op defaults
					// (RenderInterface.cpp:20-22) and no command was even recorded, so
					// border-radius overflow has always been clipped by scissor alone here.
					// Glass corners are Task 3's business: the distiller reads the mask
					// geometry from the buffer (Command.Geometry resolves in NewGeometry)
					// and masks the composite-time glass draw with it.
					break;
				}
			}
		}
	}
	RHICmdList.EndRenderPass();

	RHICmdList.Transition(FRHITransitionInfo(OutputRT, ERHIAccess::RTV, ERHIAccess::SRVMask));

	// SET, not INC, and no longer "one replay per frame so these read as per-frame". Since the
	// M2 Task 12 idle gate there are frames with NO replay at all, and what saves these two
	// numbers from latching stale values is that both are declared
	// DECLARE_DWORD_COUNTER_STAT_EXTERN (VaCuusStats.h:18-19), i.e. EStatFlags::ClearEveryFrame
	// (Stats/Stats.h:180-182). So `stat vacuus` honestly reads 0 draw calls / 0 commands on a
	// withheld frame rather than the last replay's figures.
	//
	// WHAT THAT LOOKS LIKE ON SCREEN, so nobody files it as a bug: on an idle UI the displayed
	// numbers flicker between 0 and the real per-frame count, because the frames in between
	// genuinely did not replay anything -- the composite is still running from the render
	// target. Zero here means "nothing changed", not "nothing is drawn". The steady per-second
	// view of the same thing is vacuus.M1HUD.PerfLog's published/skipped line.
	SET_DWORD_STAT(STAT_VaCuusDrawCalls, NumDrawCalls);
	SET_DWORD_STAT(STAT_VaCuusCommands, Buffer.Commands.Num());
	FVaCuusPerfLog::AddDraws(NumDrawCalls);
}
