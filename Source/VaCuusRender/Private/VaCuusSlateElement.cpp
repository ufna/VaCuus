// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusSlateElement.h"

#include "VaCuusCommandBuffer.h"
#include "VaCuusDefines.h"
#include "VaCuusStats.h"
#include "VaCuusUIShaders.h"

#include "GlobalRenderResources.h"
#include "PipelineStateCache.h"
#include "RHIResourceUtils.h"
#include "RHIStaticStates.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ScreenPass.h"

/**
 * Exp-GLASS-BACKBUFFER-SRV's runtime switch. The engine's own Slate blur binds the very
 * same OutputTexture as an SRV mid-frame (SlateRHIRenderingPolicy.cpp:1718-1738,
 * SlatePostProcessor.cpp:784-792), so direct sampling is the default; the fallback is the
 * engine's own copy-pass shape (SlateRHIRenderer.cpp:1140-1147) for an RHI whose
 * swapchain image refuses SRV use. Which path a session took is logged once, latched.
 */
static TAutoConsoleVariable<int32> CVarVaCuusGlassBackbufferSRV(
	TEXT("vacuus.GlassBackbufferSRV"),
	1,
	TEXT("1 (default) = the glass downsample samples the Slate output texture directly as an SRV (the engine's own blur ")
	TEXT("does the same). 0 = copy the glass region out first (SlateRHIRenderer.cpp:1140-1147 shape). The direct path ")
		TEXT("also falls back automatically when the output texture was created without ShaderResource."));

void FVaCuusSlateElement::SetPendingBuffer_RenderThread(FRHICommandList& RHICmdList, TUniquePtr<FVaCuusCommandBuffer> InBuffer)
{
	check(IsInRenderingThread());
	if (!InBuffer)
	{
		return;
	}

	// The glass distillation point (M5 spec §2(a)): every published buffer, in publish
	// order, BEFORE the queue can trim it — a backlog-consumed buffer surrenders its
	// resource deltas but its glass signature still replaced the list here, so "newest
	// published buffer defines the glass" holds however the queue is drained. Wholesale
	// replacement is the distiller's own first statement.
	GlassDistiller.Distill(*InBuffer);
	RefreshGlassDrawResources(RHICmdList);

	// Defensive bound: paints drain the queue and the volatile widget repaints
	// every frame, so reaching MaxPendingBuffers means some undiscovered
	// tick-without-paint path. Consume all but the newest queued buffer inline
	// — resource deltas only, no RT access, legal outside the draw pass — so
	// memory stays bounded either way.
	if (PendingBuffers.Num() >= MaxPendingBuffers)
	{
		for (int32 Index = 0; Index < PendingBuffers.Num() - 1; ++Index)
		{
			Replayer.ConsumeResources(RHICmdList, *PendingBuffers[Index]);
		}
		TUniquePtr<FVaCuusCommandBuffer> Newest = MoveTemp(PendingBuffers.Last());
		PendingBuffers.Reset();
		PendingBuffers.Add(MoveTemp(Newest));
	}

	PendingBuffers.Add(MoveTemp(InBuffer));
}

void FVaCuusSlateElement::SetDestRect_RenderThread(const FIntRect& InDestRect)
{
	check(IsInRenderingThread());
	DestRect = InDestRect;
}

void FVaCuusSlateElement::SetGlassAllowed_RenderThread(bool bInAllowed)
{
	check(IsInRenderingThread());
	if (bGlassAllowed != bInAllowed)
	{
		bGlassAllowed = bInAllowed;

		// The transition is worth a line each way: glass silently missing is otherwise
		// indistinguishable from a distiller bug, and HDR is exactly the config where
		// nothing else will look wrong (backdrop-glass.md §1: the elements texture has no
		// scene under bCompositeUIWithSceneHDR, so there is nothing valid to blur).
		UE_LOG(LogVaCuus, Log, TEXT("VaCuus glass %s (game-thread HDR mirror: r.HDR.EnableHDROutput is %s)"),
			bGlassAllowed ? TEXT("enabled") : TEXT("disabled — LDR-only by design"), bGlassAllowed ? TEXT("off") : TEXT("on"));
	}
}

void FVaCuusSlateElement::ReleaseResources_RenderThread()
{
	check(IsInRenderingThread());
	PendingBuffers.Empty();
	Replayer.ReleaseResources();

	// The glass state is torn down with the replayer for the same pairing reason its
	// ReleaseResources documents: a fresh recorder restarts handles at 1, and stale
	// cross-buffer maps would resolve the new handles to the old payloads.
	GlassDistiller.Reset();
	GlassDraws.Empty();
	GlassHalfRT[0].SafeRelease();
	GlassHalfRT[1].SafeRelease();
}

void FVaCuusSlateElement::Draw_RenderThread(FRDGBuilder& GraphBuilder, const FVaCuusDrawPassInputs& Inputs)
{
	// Engine-version seam (VaCuusEngineCompat.h hotspot 1): every FDrawPassInputs
	// field this pass consumes is read ONCE here through the VaCuusCompat accessors,
	// so a 5.6/5.7 field rename is an edit to the compat header, never to this body.
	FRDGTexture* OutputTexture = VaCuusCompat::GetOutputTexture(Inputs);
	const FVector2f ElementsOffset = VaCuusCompat::GetElementsOffset(Inputs);
	const bool bHDRDisplayOutput = VaCuusCompat::IsHDRDisplayOutput(Inputs);

	if (PendingBuffers.Num() > 0)
	{
		// 1. Setup-time (graph build): make sure the persistent RT exists at
		// the newest buffer's size NOW — RegisterExternalTexture below needs
		// the RHI texture, and the replay pass lambda runs too late for that.
		Replayer.EnsureOutputRT(GraphBuilder.RHICmdList, PendingBuffers.Last()->ViewSize);

		// 2. Raw-RHI replay wrapped in an RDG pass. The parameterless AddPass
		// overload implies SkipRenderPass, so the replayer's own
		// BeginRenderPass/Transitions are legal inside the lambda (engine
		// precedent: LandscapeUtils' RDGRecordedRenderCommand). The RT is
		// deliberately NOT declared to this pass; the replayer's class
		// invariant — SRVMask outside Replay() — means RDG's view of the
		// external texture stays truthful for the composite pass below.
		GraphBuilder.AddPass(RDG_EVENT_NAME("VaCuusReplay"), ERDGPassFlags::NeverCull,
			[this, Buffers = MoveTemp(PendingBuffers)](FRHICommandListImmediate& RHICmdList)
			{
				// Only the NEWEST buffer is drawn — each buffer repaints the
				// whole frame, so older draws are worthless. Older buffers
				// surrender just their resource deltas: fully replaying them
				// could recreate the RT mid-pass on a ViewSize change and
				// orphan the texture the composite registered at graph-build
				// time (one-frame stale/blank composite after a resize).
				for (int32 Index = 0; Index < Buffers.Num() - 1; ++Index)
				{
					Replayer.ConsumeResources(RHICmdList, *Buffers[Index]);
				}
				Replayer.Replay(RHICmdList, *Buffers.Last());
			});

		// MoveTemp above leaves the member in a valid-but-unspecified state;
		// make it definitively empty.
		PendingBuffers.Reset();
	}

	FRHITexture* OutputRT = Replayer.GetOutputRT();
	if (!OutputRT || !OutputTexture || DestRect.Area() <= 0)
	{
		// Nothing replayed yet (or torn down): draw nothing this frame.
		return;
	}

	// 3. The M5 glass passes, BEFORE the UI composite so the panel's own translucent
	// background blends over the blurred scene (spec §2(a)). ENGINE-FRAME WORK, not
	// publish work: the list persists across idle frames like the RT itself, and the
	// scene under it moves every frame — baking this at replay time is the frozen
	// backdrop Exp-GLASS-IDLE-FREEZE exists to rule out. Skipped whole under HDR
	// output: the game-thread mirror (SetGlassAllowed_RenderThread) is the shipped
	// discriminator, with bOutputIsHDRDisplay honored as the belt for the non-composite
	// HDR case where it still reads true.
	if (bGlassAllowed && !bHDRDisplayOutput && GlassDistiller.GetEntries().Num() > 0 &&
		GlassDistiller.GetViewSize().X > 0 && GlassDistiller.GetViewSize().Y > 0)
	{
		AddGlassPasses(GraphBuilder, Inputs);
	}

	// Composite scope: graph-build cost of the composite section (registration,
	// parameters, AddDrawScreenPass). The pass's own execution shows up under
	// the RDG event VaCuusComposite.
	VACUUS_PERF_SCOPE(Composite);

	// 4. Composite. Registration is consistent by construction: RDG assumes
	// external textures sit in SRVMask (kDefaultAccess), which is exactly the
	// replayer's out-of-Replay invariant.
	FRDGTextureRef UITexture = RegisterExternalTexture(GraphBuilder, OutputRT, TEXT("VaCuusUIRT"));

	// DestRect is window-space; the elements texture may host the window at an
	// offset (same convention as the Slate post-process blur pass).
	const FIntPoint Offset(FMath::RoundToInt(ElementsOffset.X), FMath::RoundToInt(ElementsOffset.Y));
	const FIntRect OutputRect(DestRect.Min + Offset, DestRect.Max + Offset);

	// The PF_FloatRGBA permutation (M6, spec §3.2): a float elements texture holds
	// LINEAR pixels — the engine pins DisplayGamma to 1.0 for a linear-SDR backbuffer
	// (UnrealEngine.cpp:2501-2504) and for HDR display targets
	// (SlateRHIRenderingPolicy.cpp:1508-1509) — so the sRGB-encoded UI RT must be
	// decoded at composite time. Format-keyed, per frame, because the elements texture
	// a widget lands in can differ per window and per session (GetViewportPixelFormat,
	// SlateRHIRenderer.cpp:760-781). Logged once, latched, like the backbuffer-SRV
	// line: the matrix/PIE check greps this to know which permutation composited.
	const bool bLinearOutput = VaCuusCompositeWantsLinearOutput(OutputTexture->Desc.Format);
	if (!bLoggedCompositeGamma)
	{
		bLoggedCompositeGamma = true;
		UE_LOG(LogVaCuus, Log, TEXT("VaCuus composite: elements texture is %s -> %s permutation"),
			GPixelFormats[OutputTexture->Desc.Format].Name,
			bLinearOutput ? TEXT("LinearOutput (sRGB->linear decode, gamma 1.0 target)") : TEXT("pass-through (display-gamma target)"));
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FScreenPassVS> VertexShader(ShaderMap);
	FVaCuusCompositePS::FPermutationDomain CompositePermutation;
	CompositePermutation.Set<FVaCuusCompositePS::FLinearOutput>(bLinearOutput);
	TShaderMapRef<FVaCuusCompositePS> PixelShader(ShaderMap, CompositePermutation);

	FVaCuusCompositePS::FParameters* Parameters = GraphBuilder.AllocParameters<FVaCuusCompositePS::FParameters>();
	Parameters->CompositeTexture = UITexture;
	Parameters->CompositeSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Parameters->RenderTargets[0] = FRenderTargetBinding(OutputTexture, ERenderTargetLoadAction::ELoad);

	// Premultiplied-over: the RT holds premultiplied content (Task 7 contract),
	// and the elements texture is the post-tonemap Slate target — no gamma
	// conversion in M1 (the LinearOutput permutation above is the one M6 exception,
	// and it converts INTO the target's own encoding, never out of it).
	FRHIBlendState* BlendState =
		TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI();

	// RESIZE BEHAVIOUR (deliberate): the RT is whatever size the newest replayed
	// buffer was recorded at, and it is stretched (bilinear) into DestRect. Since
	// M2 Task 3 the resize round-trip is asynchronous — the widget queues the new
	// size, the UI thread relayouts, and the matching buffer lands a frame or two
	// later — so during a resize the previous size is briefly scaled into the new
	// rect. That is the accepted trade: the UI stays visible and self-heals on the
	// first matching buffer. The alternatives (compositing at the RT's native size
	// with letterboxing, or blanking until sizes agree) both look worse for a few
	// frames and cost more than they buy.
	AddDrawScreenPass(GraphBuilder, RDG_EVENT_NAME("VaCuusComposite"), FScreenPassViewInfo(),
		/*OutputViewport=*/FScreenPassTextureViewport(OutputTexture, OutputRect),
		/*InputViewport=*/FScreenPassTextureViewport(UITexture),
		VertexShader, PixelShader, BlendState, Parameters);
}

namespace VaCuusGlass
{
/**
 * The engine's paired-weight gaussian fill (AddSlatePostProcessOldGaussianBlur,
 * SlatePostProcessor.cpp:658-696), reproduced because that function is module-private.
 * Two mirrored taps share one bilinear fetch: each vec4 slot packs (weight, offset,
 * weight, offset), slot 0's xy being the center tap. Unnormalized like the engine's — at
 * a 3-sigma kernel the tail loss is under half a percent.
 */
static int32 FillBlurWeights(FVaCuusBlurPS::FParameters* Parameters, float Sigma)
{
	const float Strength = FMath::Max(0.5f, Sigma);

	// The engine's own kernel rule (SBackgroundBlur::ComputeEffectiveKernelSize,
	// SBackgroundBlur.cpp:172-188): 3x the strength, made odd. Clamped to what the
	// uniform array carries — ~41.7 sigma in half-res texels = ~80px of view-space sigma,
	// far past anything a HUD panel asks for.
	int32 KernelSize = FMath::RoundToInt(Strength * 3.0f);
	KernelSize = FMath::Clamp(KernelSize | 1, 3, 2 * FVaCuusBlurPS::MaxBlurSamples - 1);

	const auto GetWeight = [](float Dist, float InStrength)
	{
		const float Strength2 = InStrength * InStrength;
		return (1.0f / FMath::Sqrt(2.0f * PI * Strength2)) * FMath::Exp(-(Dist * Dist) / (2.0f * Strength2));
	};

	const auto GetWeightAndOffset = [&GetWeight](float Dist, float InStrength)
	{
		const float Weight1 = GetWeight(Dist, InStrength);
		const float Weight2 = GetWeight(Dist + 1.0f, InStrength);
		const float TotalWeight = Weight1 + Weight2;
		const float Offset = TotalWeight > 0.0f ? (Weight1 * Dist + Weight2 * (Dist + 1.0f)) / TotalWeight : 0.0f;
		return FVector2f(TotalWeight, Offset);
	};

	const int32 SampleCount = FMath::DivideAndRoundUp(KernelSize, 2);

	Parameters->WeightAndOffsets[0] = FVector4f(FVector2f(GetWeight(0.0f, Strength), 0.0f), GetWeightAndOffset(1.0f, Strength));
	for (int32 Dist = 3, SampleIndex = 1; Dist < KernelSize && SampleIndex < FVaCuusBlurPS::MaxBlurSamples; Dist += 4, ++SampleIndex)
	{
		Parameters->WeightAndOffsets[SampleIndex] =
			FVector4f(GetWeightAndOffset(float(Dist), Strength), GetWeightAndOffset(float(Dist + 2), Strength));
	}

	Parameters->SampleCount = SampleCount;
	return SampleCount;
}

/** One separable blur direction over HalfRect: Source -> Dest, sigma in DEST texels. */
static void AddBlurPass(FRDGBuilder& GraphBuilder, FGlobalShaderMap* ShaderMap, FRDGTextureRef Source, FRDGTextureRef Dest,
	const FIntRect& HalfRect, float Sigma, const FVector2f& Direction)
{
	TShaderMapRef<FScreenPassVS> VertexShader(ShaderMap);
	TShaderMapRef<FVaCuusBlurPS> PixelShader(ShaderMap);

	const FScreenPassTextureViewport InputViewport(Source, HalfRect);
	const FScreenPassTextureViewportParameters InputParameters = GetScreenPassTextureViewportParameters(InputViewport);

	FVaCuusBlurPS::FParameters* Parameters = GraphBuilder.AllocParameters<FVaCuusBlurPS::FParameters>();
	Parameters->BlurTexture = Source;
	Parameters->BlurSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Parameters->BufferSizeAndDirection = FVector4f(InputParameters.ExtentInverse, Direction);
	Parameters->UVBounds = FVector4f(InputParameters.UVViewportBilinearMin, InputParameters.UVViewportBilinearMax);
	Parameters->RenderTargets[0] = FRenderTargetBinding(Dest, ERenderTargetLoadAction::ELoad);
	FillBlurWeights(Parameters, Sigma);

	AddDrawScreenPass(GraphBuilder, RDG_EVENT_NAME("VaCuusGlassBlur"), FScreenPassViewInfo(),
		/*OutputViewport=*/FScreenPassTextureViewport(Dest, HalfRect), InputViewport, VertexShader, PixelShader, Parameters);
}
} // namespace VaCuusGlass

void FVaCuusSlateElement::RefreshGlassDrawResources(FRHICommandList& RHICmdList)
{
	if (GlassDrawsGeneration == GlassDistiller.GetListGeneration())
	{
		return;
	}
	GlassDrawsGeneration = GlassDistiller.GetListGeneration();
	GlassDraws.Reset();

	for (const FVaCuusGlassEntry& Entry : GlassDistiller.GetEntries())
	{
		FGlassDraw& Draw = GlassDraws.AddDefaulted_GetRef();

		// The square case generates a DrawRegion quad through the SAME vertex layout and
		// draw path as a mask, so there is exactly one glass draw code path. White with
		// full alpha — the same coverage the mask geometry carries
		// (ElementBackgroundBorder.cpp:72). All four channel bytes equal, so the
		// RGBA-vs-FColor byte-order note on FVaCuusVertex cannot bite here.
		TArray<FVaCuusVertex, TInlineAllocator<4>> QuadVertices;
		TArray<int32, TInlineAllocator<6>> QuadIndices;
		if (!Entry.MaskGeometry.IsValid())
		{
			const FVector2f Min(float(Entry.DrawRegion.Min.X), float(Entry.DrawRegion.Min.Y));
			const FVector2f Max(float(Entry.DrawRegion.Max.X), float(Entry.DrawRegion.Max.Y));
			const FColor White(255, 255, 255, 255);
			QuadVertices.Append({{Min, White, FVector2f::ZeroVector}, {FVector2f(Max.X, Min.Y), White, FVector2f::ZeroVector},
				{Max, White, FVector2f::ZeroVector}, {FVector2f(Min.X, Max.Y), White, FVector2f::ZeroVector}});
			QuadIndices.Append({0, 1, 2, 0, 2, 3});
		}

		const TConstArrayView<FVaCuusVertex> VertexView = Entry.MaskGeometry.IsValid()
			? MakeConstArrayView(Entry.MaskGeometry->Vertices.GetData(), Entry.MaskGeometry->Vertices.Num())
			: MakeConstArrayView(QuadVertices.GetData(), QuadVertices.Num());
		const TConstArrayView<int32> IndexView = Entry.MaskGeometry.IsValid()
			? MakeConstArrayView(Entry.MaskGeometry->Indices.GetData(), Entry.MaskGeometry->Indices.Num())
			: MakeConstArrayView(QuadIndices.GetData(), QuadIndices.Num());

		if (VertexView.Num() == 0 || IndexView.Num() < 3)
		{
			continue; // Keep the (empty) slot so GlassDraws stays parallel to the entries.
		}

		Draw.VB = UE::RHIResourceUtils::CreateVertexBufferFromArray<FVaCuusVertex>(
			RHICmdList, TEXT("VaCuusGlassVB"), EBufferUsageFlags::Static, VertexView);
		Draw.IB = UE::RHIResourceUtils::CreateIndexBufferFromArray<int32>(
			RHICmdList, TEXT("VaCuusGlassIB"), EBufferUsageFlags::Static, IndexView);
		Draw.NumVertices = VertexView.Num();
		Draw.NumIndices = IndexView.Num();
	}
}

void FVaCuusSlateElement::AddGlassPasses(FRDGBuilder& GraphBuilder, const FVaCuusDrawPassInputs& Inputs)
{
	// Graph-build cost of the whole glass section; the passes' execution shows under the
	// RDG events VaCuusGlass*. The per-window SAMPLE COUNT of this scope against
	// `published=` in the same PerfLog window is the idle-freeze observable: an idle
	// glass HUD keeps producing these at engine rate with publishes at zero.
	VACUUS_PERF_SCOPE(Glass);

	// Engine-version seam: field reads once, through VaCuusEngineCompat.h hotspot 1.
	FRDGTexture* OutputTexture = VaCuusCompat::GetOutputTexture(Inputs);

	const TArray<FVaCuusGlassEntry>& Entries = GlassDistiller.GetEntries();
	const FIntPoint OutputExtent = OutputTexture->Desc.Extent;
	const FVaCuusGlassMapping Mapping = VaCuusMakeGlassMapping(DestRect, VaCuusCompat::GetElementsOffset(Inputs),
		GlassDistiller.GetViewSize(), VaCuusCompat::GetSceneViewRect(Inputs), OutputExtent);

	// THE COORDINATE MAPPING (spec §2(a)), per engine frame from the LIVE transform:
	// regions, mask vertices (via the draw matrix below) and sigma all go through it,
	// clamped to SceneViewRect — view space is never pre-baked into window space.
	struct FMappedEntry
	{
		int32 EntryIndex = 0;
		FIntRect SampleRect;
		FIntRect DrawRect;
		FIntPoint HalfSize;
		FVector2f SigmaOut;
	};
	TArray<FMappedEntry, TInlineAllocator<4>> MappedEntries;
	FIntPoint NeededExtent = FIntPoint::ZeroValue;
	FIntRect SampleBounds;
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		if (!GlassDraws.IsValidIndex(Index) || GlassDraws[Index].NumIndices < 3)
		{
			continue;
		}

		FMappedEntry Mapped;
		Mapped.EntryIndex = Index;
		Mapped.SampleRect = Mapping.MapRect(Entries[Index].SampleRegion);
		Mapped.DrawRect = Mapping.MapRect(Entries[Index].DrawRegion);
		Mapped.SigmaOut = Mapping.MapSigma(Entries[Index].Sigma);
		if (Mapped.SampleRect.Area() <= 0 || Mapped.DrawRect.Area() <= 0)
		{
			continue; // Fully clipped (off the scene view): nothing to sample or draw.
		}

		Mapped.HalfSize = FIntPoint(
			FMath::Max(1, FMath::DivideAndRoundUp(Mapped.SampleRect.Width(), 2)),
			FMath::Max(1, FMath::DivideAndRoundUp(Mapped.SampleRect.Height(), 2)));
		NeededExtent = FIntPoint(FMath::Max(NeededExtent.X, Mapped.HalfSize.X), FMath::Max(NeededExtent.Y, Mapped.HalfSize.Y));
		SampleBounds = MappedEntries.Num() == 0 ? Mapped.SampleRect : FIntRect(
			FIntPoint(FMath::Min(SampleBounds.Min.X, Mapped.SampleRect.Min.X), FMath::Min(SampleBounds.Min.Y, Mapped.SampleRect.Min.Y)),
			FIntPoint(FMath::Max(SampleBounds.Max.X, Mapped.SampleRect.Max.X), FMath::Max(SampleBounds.Max.Y, Mapped.SampleRect.Max.Y)));
		MappedEntries.Add(Mapped);
	}
	if (MappedEntries.Num() == 0)
	{
		return;
	}
	++NumGlassFrames;

	// The pooled ping-pong pair: PERSISTENT and sized to the mapped glass bounds, not the
	// screen (spec §2(c)) — per element, so N glass-bearing views cost N pairs, which is
	// the budget table's stated multiplier. Format follows the output so a 10-bit LDR
	// backbuffer is not squeezed through 8 bits on the way round.
	const EPixelFormat HalfFormat = OutputTexture->Desc.Format;
	for (FTextureRHIRef& RT : GlassHalfRT)
	{
		if (!RT.IsValid() || RT->GetSizeXY() != NeededExtent || RT->GetFormat() != HalfFormat)
		{
			const FRHITextureCreateDesc Desc =
				FRHITextureCreateDesc::Create2D(TEXT("VaCuusGlassHalfRT"), NeededExtent, HalfFormat)
					.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource)
					.SetClearValue(FClearValueBinding::Transparent)
					// Invariant: outside the glass passes both RTs sit in SRVMask —
					// consistent with RDG's external-texture default, like the UI RT.
					.SetInitialState(ERHIAccess::SRVMask);
			RT = GraphBuilder.RHICmdList.CreateTexture(Desc);
		}
	}
	FRDGTextureRef HalfA = RegisterExternalTexture(GraphBuilder, GlassHalfRT[0], TEXT("VaCuusGlassHalfA"));
	FRDGTextureRef HalfB = RegisterExternalTexture(GraphBuilder, GlassHalfRT[1], TEXT("VaCuusGlassHalfB"));

	// The scene source (Exp-GLASS-BACKBUFFER-SRV): direct SRV when the output texture
	// carries ShaderResource and the cvar has not forced the fallback; otherwise one
	// bounded AddCopyTexturePass into a transient (the engine fallback shape,
	// SlateRHIRenderer.cpp:1140-1147). The DESC FLAG is the runtime check — an RHI whose
	// swapchain image cannot be sampled does not create it ShaderResource.
	const bool bOutputSampleable = EnumHasAnyFlags(OutputTexture->Desc.Flags, TexCreate_ShaderResource);
	const bool bDirectSRV = bOutputSampleable && CVarVaCuusGlassBackbufferSRV.GetValueOnRenderThread() != 0;
	FRDGTextureRef SceneSource = OutputTexture;
	FIntPoint SourceShift = FIntPoint::ZeroValue;
	if (!bDirectSRV)
	{
		FRDGTextureRef SceneCopy = GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2D(SampleBounds.Size(), HalfFormat, FClearValueBinding::Black,
				TexCreate_RenderTargetable | TexCreate_ShaderResource),
			TEXT("VaCuusGlassSceneCopy"));
		AddCopyTexturePass(GraphBuilder, OutputTexture, SceneCopy, SampleBounds.Min, FIntPoint::ZeroValue, SampleBounds.Size());
		SceneSource = SceneCopy;
		SourceShift = SampleBounds.Min;
	}
	if (!bLoggedBackbufferPath)
	{
		bLoggedBackbufferPath = true;
		UE_LOG(LogVaCuus, Log,
			TEXT("Exp-GLASS-BACKBUFFER-SRV: glass samples the Slate output %s (texture ShaderResource=%s, vacuus.GlassBackbufferSRV=%d)"),
			bDirectSRV ? TEXT("DIRECTLY as an SRV") : TEXT("through a bounded copy pass"),
			bOutputSampleable ? TEXT("yes") : TEXT("no"), CVarVaCuusGlassBackbufferSRV.GetValueOnRenderThread());
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FScreenPassVS> ScreenVertexShader(ShaderMap);
	// The default (LinearOutput=false) permutation, DELIBERATELY: the downsample reads
	// the elements texture and writes a half-res copy of it — identical encoding in and
	// out, whatever that encoding is. Glass is gamma-neutral by construction
	// (backdrop-glass.md §6); a decode here would double-decode on a float target.
	TShaderMapRef<FVaCuusCompositePS> DownsamplePS(ShaderMap, FVaCuusCompositePS::FPermutationDomain());
	TShaderMapRef<FVaCuusUIVS> GlassVertexShader(ShaderMap);
	TShaderMapRef<FVaCuusGlassPS> GlassPixelShader(ShaderMap);
	const FMatrix44f PixelToClip = VaCuusReplay::MakePixelToClipMatrix(OutputExtent);

	for (const FMappedEntry& Mapped : MappedEntries)
	{
		const FVaCuusGlassEntry& Entry = Entries[Mapped.EntryIndex];
		const FIntRect HalfRect(0, 0, Mapped.HalfSize.X, Mapped.HalfSize.Y);

		// (1) One bilinear pass sampling the scene region into half-res — simultaneously
		// the copy and the downsample (backdrop-glass.md §2). The pass-through composite
		// PS is exactly the sampler this needs; the default opaque blend overwrites.
		//
		// EVERY ENGINE FRAME, deliberately — gating passes (1)-(2) on "a publish arrived"
		// is the replay-baked shape and it FREEZES: prototyped for Exp-GLASS-IDLE-FREEZE
		// (2026-08-01) — with the refresh publish-gated, the backdrop under an idle panel
		// measured RMSE exactly 0 between two beats 8s apart while the scene behind the
		// blur-free control panel changed 24.8%; this shipped per-frame refresh measured
		// 11.6% in the same protocol. Both outcomes in the Task 3 report.
		{
			FVaCuusCompositePS::FParameters* Parameters = GraphBuilder.AllocParameters<FVaCuusCompositePS::FParameters>();
			Parameters->CompositeTexture = SceneSource;
			Parameters->CompositeSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			Parameters->RenderTargets[0] = FRenderTargetBinding(HalfA, ERenderTargetLoadAction::ELoad);

			const FIntRect ShiftedSample(Mapped.SampleRect.Min - SourceShift, Mapped.SampleRect.Max - SourceShift);
			AddDrawScreenPass(GraphBuilder, RDG_EVENT_NAME("VaCuusGlassDownsample"), FScreenPassViewInfo(),
				/*OutputViewport=*/FScreenPassTextureViewport(HalfA, HalfRect),
				/*InputViewport=*/FScreenPassTextureViewport(SceneSource, ShiftedSample), ScreenVertexShader, DownsamplePS, Parameters);
		}

		// (2) Separable gaussian ping-pong at half-res, sigma mapped per axis and then
		// scaled into half-res texels by each axis's actual downsample ratio.
		const FVector2f HalfRatio(
			float(Mapped.HalfSize.X) / float(Mapped.SampleRect.Width()), float(Mapped.HalfSize.Y) / float(Mapped.SampleRect.Height()));
		VaCuusGlass::AddBlurPass(GraphBuilder, ShaderMap, HalfA, HalfB, HalfRect, Mapped.SigmaOut.X * HalfRatio.X, FVector2f(1.0f, 0.0f));
		VaCuusGlass::AddBlurPass(GraphBuilder, ShaderMap, HalfB, HalfA, HalfRect, Mapped.SigmaOut.Y * HalfRatio.Y, FVector2f(0.0f, 1.0f));

		// (3) The masked glass draw: the entry's geometry (mask copy or generated quad)
		// through the mapping matrix, sampling the blurred half-res at the output pixel,
		// scissored to the mapped write region. SrcAlpha/InvSrcAlpha on color so the
		// mask's coverage lerps blurred-over-sharp; dest alpha untouched (CW_RGB) — the
		// output's alpha channel is never meaningful (2 bits on the desktop default).
		{
			// Row-vector composition: mask translation (view px) -> mapping scale+offset
			// (output px) -> clip. Collapsed into one affine before the ortho.
			FMatrix44f Affine = FMatrix44f::Identity;
			Affine.M[0][0] = Mapping.Scale.X;
			Affine.M[1][1] = Mapping.Scale.Y;
			Affine.M[3][0] = Entry.MaskTranslation.X * Mapping.Scale.X + Mapping.Offset.X;
			Affine.M[3][1] = Entry.MaskTranslation.Y * Mapping.Scale.Y + Mapping.Offset.Y;

			FVaCuusUIShaderParameters VSParameters;
			VSParameters.Projection = Affine * PixelToClip;
			VSParameters.UITexture = GWhiteTexture->TextureRHI;
			VSParameters.UISampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			VSParameters.bUseTexture = 0;

			FVaCuusGlassPS::FParameters* PSParameters = GraphBuilder.AllocParameters<FVaCuusGlassPS::FParameters>();
			PSParameters->GlassTexture = HalfA;
			PSParameters->GlassSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			PSParameters->RenderTargets[0] = FRenderTargetBinding(OutputTexture, ERenderTargetLoadAction::ELoad);

			// SV_Position (output px, already at pixel centers) -> half-res UV: the
			// downsample maps SampleRect.Min to half texel 0, at HalfRatio texels per
			// output pixel. Bounds keep bilinear taps inside this entry's region of the
			// pooled RT, whose extent may exceed it.
			const FVector2f RTExtentInv(1.0f / float(NeededExtent.X), 1.0f / float(NeededExtent.Y));
			PSParameters->GlassUVTransform = FVector4f(HalfRatio.X * RTExtentInv.X, HalfRatio.Y * RTExtentInv.Y,
				-float(Mapped.SampleRect.Min.X) * HalfRatio.X * RTExtentInv.X,
				-float(Mapped.SampleRect.Min.Y) * HalfRatio.Y * RTExtentInv.Y);
			PSParameters->GlassUVBounds = FVector4f(0.5f * RTExtentInv.X, 0.5f * RTExtentInv.Y,
				(float(Mapped.HalfSize.X) - 0.5f) * RTExtentInv.X, (float(Mapped.HalfSize.Y) - 0.5f) * RTExtentInv.Y);

			const FGlassDraw Draw = GlassDraws[Mapped.EntryIndex]; // ref-counted copies for the lambda
			const FIntRect DrawRect = Mapped.DrawRect;

			GraphBuilder.AddPass(RDG_EVENT_NAME("VaCuusGlassDraw"), PSParameters, ERDGPassFlags::Raster,
				[PSParameters, VSParameters, GlassVertexShader, GlassPixelShader, Draw, DrawRect, OutputExtent](
					FRDGAsyncTask, FRHICommandList& RHICmdList)
				{
					RHICmdList.SetViewport(0.0f, 0.0f, 0.0f, float(OutputExtent.X), float(OutputExtent.Y), 1.0f);
					RHICmdList.SetScissorRect(
						true, uint32(DrawRect.Min.X), uint32(DrawRect.Min.Y), uint32(DrawRect.Max.X), uint32(DrawRect.Max.Y));

					FGraphicsPipelineStateInitializer GraphicsPSOInit;
					RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
					GraphicsPSOInit.BlendState =
						TStaticBlendState<CW_RGB, BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha, BO_Add, BF_Zero, BF_One>::GetRHI();
					GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
					GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
					GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GVaCuusVertexDeclaration.VertexDeclarationRHI;
					GraphicsPSOInit.BoundShaderState.VertexShaderRHI = GlassVertexShader.GetVertexShader();
					GraphicsPSOInit.BoundShaderState.PixelShaderRHI = GlassPixelShader.GetPixelShader();
					GraphicsPSOInit.PrimitiveType = PT_TriangleList;
					SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

					{
						FRHIBatchedShaderParameters& BatchedParameters = RHICmdList.GetScratchShaderParameters();
						SetShaderParameters(BatchedParameters, GlassVertexShader, VSParameters);
						RHICmdList.SetBatchedShaderParameters(GlassVertexShader.GetVertexShader(), BatchedParameters);
					}
					SetShaderParameters(RHICmdList, GlassPixelShader, GlassPixelShader.GetPixelShader(), *PSParameters);

					RHICmdList.SetStreamSource(0, Draw.VB, 0);
					RHICmdList.DrawIndexedPrimitive(Draw.IB,
						/*BaseVertexIndex=*/0, /*FirstInstance=*/0, uint32(Draw.NumVertices),
						/*StartIndex=*/0, uint32(Draw.NumIndices / 3), /*NumInstances=*/1);

					RHICmdList.SetScissorRect(false, 0, 0, 0, 0);
				});
		}
	}
}
