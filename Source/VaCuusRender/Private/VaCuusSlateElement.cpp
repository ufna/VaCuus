// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusSlateElement.h"

#include "VaCuusCommandBuffer.h"
#include "VaCuusUIShaders.h"

#include "RHIStaticStates.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ScreenPass.h"

void FVaCuusSlateElement::SetPendingBuffer_RenderThread(FRHICommandList& RHICmdList, TUniquePtr<FVaCuusCommandBuffer> InBuffer)
{
	check(IsInRenderingThread());
	if (!InBuffer)
	{
		return;
	}

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

void FVaCuusSlateElement::ReleaseResources_RenderThread()
{
	check(IsInRenderingThread());
	PendingBuffers.Empty();
	Replayer.ReleaseResources();
}

void FVaCuusSlateElement::Draw_RenderThread(FRDGBuilder& GraphBuilder, const FDrawPassInputs& Inputs)
{
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
	if (!OutputRT || !Inputs.OutputTexture || DestRect.Area() <= 0)
	{
		// Nothing replayed yet (or torn down): draw nothing this frame.
		return;
	}

	// 3. Composite. Registration is consistent by construction: RDG assumes
	// external textures sit in SRVMask (kDefaultAccess), which is exactly the
	// replayer's out-of-Replay invariant.
	FRDGTextureRef UITexture = RegisterExternalTexture(GraphBuilder, OutputRT, TEXT("VaCuusUIRT"));

	// DestRect is window-space; the elements texture may host the window at an
	// offset (same convention as the Slate post-process blur pass).
	const FIntPoint Offset(FMath::RoundToInt(Inputs.ElementsOffset.X), FMath::RoundToInt(Inputs.ElementsOffset.Y));
	const FIntRect OutputRect(DestRect.Min + Offset, DestRect.Max + Offset);

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FScreenPassVS> VertexShader(ShaderMap);
	TShaderMapRef<FVaCuusCompositePS> PixelShader(ShaderMap);

	FVaCuusCompositePS::FParameters* Parameters = GraphBuilder.AllocParameters<FVaCuusCompositePS::FParameters>();
	Parameters->CompositeTexture = UITexture;
	Parameters->CompositeSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Parameters->RenderTargets[0] = FRenderTargetBinding(Inputs.OutputTexture, ERenderTargetLoadAction::ELoad);

	// Premultiplied-over: the RT holds premultiplied content (Task 7 contract),
	// and Inputs.OutputTexture is the post-tonemap Slate target — no gamma
	// conversion in M1.
	FRHIBlendState* BlendState =
		TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI();

	AddDrawScreenPass(GraphBuilder, RDG_EVENT_NAME("VaCuusComposite"), FScreenPassViewInfo(),
		/*OutputViewport=*/FScreenPassTextureViewport(Inputs.OutputTexture, OutputRect),
		/*InputViewport=*/FScreenPassTextureViewport(UITexture),
		VertexShader, PixelShader, BlendState, Parameters);
}
