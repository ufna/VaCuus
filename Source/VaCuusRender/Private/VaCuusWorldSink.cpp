// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusWorldSink.h"

#include "VaCuusCommandBuffer.h"
#include "VaCuusDefines.h"
#include "VaCuusStats.h"

#include "RHICommandList.h"

void FVaCuusWorldSink::SetPendingBuffer_RenderThread(FRHICommandList& RHICmdList, TUniquePtr<FVaCuusCommandBuffer> InBuffer)
{
	check(IsInRenderingThread());
	if (!InBuffer)
	{
		return;
	}
	NumArrivals.fetch_add(1, std::memory_order_relaxed);

	// EnsureOutputRT BEFORE Replay, the Slate element's own order
	// (VaCuusSlateElement.cpp:108-134): Replay's mid-pass-recreate tripwire
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
