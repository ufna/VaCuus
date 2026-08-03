// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusWorldSink.h"

#include "VaCuusCommandBuffer.h"
#include "VaCuusDefines.h"
#include "VaCuusStats.h"

#include "GenerateMips.h"
#include "PooledRenderTarget.h"
#include "RHICommandList.h"
#include "RenderGraphBuilder.h"

void FVaCuusWorldSink::SetPendingBuffer_RenderThread(FRHICommandList& RHICmdList, TUniquePtr<FVaCuusCommandBuffer> InBuffer)
{
	check(IsInRenderingThread());
	if (!InBuffer)
	{
		return;
	}
	NumArrivals.fetch_add(1, std::memory_order_relaxed);

	// EnsureOutputRT BEFORE Replay, the Slate element's own order
	// (VaCuusSlateElement.cpp:113-141): Replay's mid-pass-recreate tripwire
	// (VaCuusReplayRenderer.cpp:56-64) ensure()s the RT already matches the buffer's
	// ViewSize -- the tripwire protects a composite that registered the texture at
	// graph-build time, which this path does not have, but tripping an ensure per
	// resize would still bury the log and the ordering costs nothing.
	Replayer.EnsureOutputRT(RHICmdList, InBuffer->ViewSize);
	Replayer.Replay(RHICmdList, *InBuffer);

	CopyToDestination(RHICmdList);
}

void FVaCuusWorldSink::SetDestination_RenderThread(FRHICommandList& RHICmdList, FTextureRHIRef InDestination)
{
	check(IsInRenderingThread());
	Destination = MoveTemp(InDestination);

	// The pooled wrapper holds its own TRefCountPtr<FRHITexture>; released with the
	// slot it wrapped, so a retired destination is not kept alive by our cache.
	// GenerateDestinationMips rebuilds it on the next >1-mip copy.
	DestinationPooled.SafeRelease();

	// The immediate repaint the header calls load-bearing: a fresh destination on an
	// idle view would otherwise wait for a publish the idle gate never sends. No-ops
	// harmlessly before the first replay (no OutputRT yet) or while extents disagree
	// mid-resize (the guard skips; the matching publish is already on its way, since
	// a resize always relayouts and ViewSize is hashed -- VaCuusCommandBuffer.h:561).
	if (Destination.IsValid())
	{
		CopyToDestination(RHICmdList);
	}
}

void FVaCuusWorldSink::ReleaseResources_RenderThread()
{
	check(IsInRenderingThread());
	Replayer.ReleaseResources();
	Destination.SafeRelease();
	DestinationPooled.SafeRelease();
}

void FVaCuusWorldSink::CopyToDestination(FRHICommandList& RHICmdList)
{
	FRHITexture* Source = Replayer.GetOutputRT();
	if (!Source || !Destination.IsValid())
	{
		return;
	}

	// The extent guard (spec 2(g)): skip on mismatch, self-healing -- see the header.
	if (Source->GetSizeXY() != Destination->GetSizeXY())
	{
		NumExtentSkips.fetch_add(1, std::memory_order_relaxed);
		UE_LOG(LogVaCuus, Verbose, TEXT("World sink copy skipped: RT %dx%d vs destination %dx%d (resize in flight)"),
			Source->GetSizeXY().X, Source->GetSizeXY().Y, Destination->GetSizeXY().X, Destination->GetSizeXY().Y);
		return;
	}

	// Formats agree by construction -- both sides are PF_B8G8R8A8 without TexCreate_SRGB
	// (VaCuusReplayRenderer.cpp:172-178; the component's InitCustomFormat with
	// bForceLinearGamma=true, whose IsSRGB() is !bForceLinearGamma for override formats,
	// TextureRenderTarget2D.cpp:72-87) -- so this can only fire if someone changes one
	// side's creation without the other.
	if (!ensureMsgf(Source->GetFormat() == Destination->GetFormat(),
			TEXT("World sink copy format mismatch: %d vs %d"), int32(Source->GetFormat()), int32(Destination->GetFormat())))
	{
		return;
	}

	{
		// Braced so the scope CLOSES before the mips branch: WorldCopy and WorldMips
		// are disjoint samples a reader may sum, never nested -- an open copy scope
		// around the generation would double-count it into both lines.
		VACUUS_PERF_SCOPE(WorldCopy);

		// OutputRT sits in SRVMask outside Replay by class invariant
		// (VaCuusReplayRenderer.cpp:176-177, :617), so its transitions are explicit
		// round trips. The destination's before-state is Unknown on purpose: a freshly
		// (re)initialized render-target resource arrives in whatever state its InitRHI
		// left, and only after our first copy is SRVMask its steady state.
		RHICmdList.Transition(FRHITransitionInfo(Source, ERHIAccess::SRVMask, ERHIAccess::CopySrc));
		RHICmdList.Transition(FRHITransitionInfo(Destination, ERHIAccess::Unknown, ERHIAccess::CopyDest));
		RHICmdList.CopyTexture(Source, Destination, FRHICopyTextureInfo());
		RHICmdList.Transition(FRHITransitionInfo(Destination, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
		RHICmdList.Transition(FRHITransitionInfo(Source, ERHIAccess::CopySrc, ERHIAccess::SRVMask));

		NumCopies.fetch_add(1, std::memory_order_relaxed);
	}

	// The copy above writes mip 0 only (FRHICopyTextureInfo::NumMips defaults to 1,
	// RHICommandList.h:207); a >1-mip destination means the component asked for a
	// chain (bGenerateMips -> bAutoGenerateMips -> NumMips = FloorLog2(max side) + 1,
	// TextureRenderTarget2D.cpp:96-103), and stale far mips under a fresh mip 0 would
	// show the OLD frame to any minified sample. One branch is the whole cost of the
	// off path: a 1-mip destination never reaches GenerateDestinationMips.
	if (Destination->GetDesc().NumMips > 1)
	{
		GenerateDestinationMips(RHICmdList);
	}
}

void FVaCuusWorldSink::GenerateDestinationMips(FRHICommandList& RHICmdList)
{
	VACUUS_PERF_SCOPE(WorldMips);

	// FGenerateMips is RDG-only (GenerateMips.h:38-43), and an FRDGBuilder wants the
	// immediate list -- which every caller already is: both entry points are bodies of
	// ENQUEUE_RENDER_COMMAND lambdas (VaCuusRmlDocumentHost.cpp:493-497, the
	// component's slot update), whose parameter IS FRHICommandListImmediate. Get()
	// check()s that (RHICommandList.h:4411-4415), so a future non-immediate caller
	// fails loudly here instead of corrupting command order.
	FRDGBuilder GraphBuilder(FRHICommandListImmediate::Get(RHICmdList));

	CacheRenderTarget(Destination, TEXT("VaCuusWorldPanelMips"), DestinationPooled);
	FRDGTextureRef DestinationRDG = GraphBuilder.RegisterExternalTexture(DestinationPooled);

	// THE TRANSITION STORY. The copy block left Destination in SRVMask (all
	// subresources -- our transitions are whole-texture). RDG assumes nothing about a
	// registered external texture's current state (prologue Access defaults to
	// Unknown, RenderGraphResources.h:110), transitions per PASS and per SUBRESOURCE
	// while the chain builds -- each level reads mip N-1 as SRV and writes mip N as
	// RTV or UAV (GenerateMips.cpp:154-196 raster, :234-261 compute; AutoDetect picks
	// compute iff the format has TypedUAVStore AND the texture carries the UAV flag,
	// :370-376, which is exactly when the RT resource added that flag,
	// TextureRenderTarget2D.cpp:498-504 -- the two sites consult the same
	// WillFormatSupportCompute, so they cannot disagree) -- and its epilogue returns
	// the whole texture to SRVMask (EpilogueAccess default, RenderGraphResources.h:361
	// and :454, applied at RenderGraphBuilder.cpp:4163). SRVMask in, SRVMask out: the
	// steady state the material sampler and the next copy's Unknown->CopyDest both
	// expect.
	//
	// THE GAMMA NOTE (WS-GAMMA): the chain is downsampled in display-encoded
	// premultiplied space -- the only space these pixels exist in (the replay RT is
	// raw-encoded bytes, VaCuusReplayRenderer.cpp:170-178, and this destination is
	// deliberately not sRGB-tagged, so the generator's own decode permutation stays
	// off, GenerateMips.cpp:209). Averaging encoded values darkens gradients slightly
	// on far mips -- accepted; premultiplied is the CORRECT form for edge filtering
	// (a straight-alpha average would bleed the transparent texels' dead RGB into
	// glyph edges as fringes).
	FGenerateMips::Execute(GraphBuilder, GMaxRHIFeatureLevel, DestinationRDG, FGenerateMipsParams{});

	GraphBuilder.Execute();

	NumMipGenerations.fetch_add(1, std::memory_order_relaxed);
}
