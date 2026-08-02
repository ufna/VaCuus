// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusFrameSink.h"
#include "VaCuusReplayRenderer.h"

#include <atomic>

struct IPooledRenderTarget;

/**
 * The world-space frame sink (M5 spec 2(g)): replay ON ARRIVAL, then one CopyTexture
 * into the owning component's UTextureRenderTarget2D -- so the copy rides the publish
 * rate, which the idle gate makes ~zero on a static HUD, and a material-decorator
 * document (whose forced republish is clamped to engine rate, M5 Task 5b) pays one
 * copy per engine frame. When the destination carries a mip chain (the component's
 * bGenerateMips, on by default), each copy is followed by one FGenerateMips pass over
 * the chain -- same cadence, same idle economics. Nothing here runs per engine frame
 * on its own: unlike
 * FVaCuusSlateElement there is no paint to defer to, so each published buffer is
 * replayed in the render command that delivered it. That also makes the Slate
 * element's newest-buffer drain unnecessary -- render commands from the one UI
 * thread execute in publish order, so there is never a backlog to trim.
 *
 * THE DESTINATION-SLOT DISCIPLINE (spec 2(g), v1 finding 12.4): Destination is ONE
 * FTextureRHIRef, written only by SetDestination_RenderThread, which only the GAME
 * thread enqueues -- after every UTextureRenderTarget2D (re)init, so the slot update
 * is FIFO with the resource recreation it follows and can never resurrect a texture
 * the RT no longer owns. Publishes arrive via render commands from the UI thread;
 * the two streams interleave arbitrarily, and the extent guard is what absorbs every
 * interleaving: a publish that lands before its matching slot update copies into the
 * OLD destination and is SKIPPED on mismatch, then either the next matching publish
 * or the slot update's own repaint (below) heals it -- the same accepted transient
 * the screen path documents for resize (VaCuusSlateElement.cpp:192-200).
 *
 * THE SLOT UPDATE COPIES IMMEDIATELY, and that is load-bearing, not polish: on an
 * idle view NO further publish is coming (the idle gate withholds them all), so a
 * fresh destination texture that waited for one would stay blank forever -- the M2
 * "the RT is the only copy of the pixels" trap, world edition
 * (VaCuusReplayRenderer.h, ReleaseResources' note). The replayer's persistent
 * OutputRT is that only copy, and the slot update repaints from it.
 *
 * THREAD AFFINITY: all state belongs to the render thread except the four counters,
 * which are atomics so tests and diagnostics can read them from the game thread.
 */
class FVaCuusWorldSink final : public IVaCuusFrameSink
{
public:
	//~ Begin IVaCuusFrameSink
	virtual void SetPendingBuffer_RenderThread(FRHICommandList& RHICmdList, TUniquePtr<FVaCuusCommandBuffer> InBuffer) override;
	virtual void ReleaseResources_RenderThread() override;
	//~ End IVaCuusFrameSink

	/**
	 * Points the copy at (the RHI texture of) the component's render target; null
	 * detaches. See the class comment for the enqueue discipline and why this copies
	 * immediately when it can.
	 */
	void SetDestination_RenderThread(FRHICommandList& RHICmdList, FTextureRHIRef InDestination);

	/** Buffers that arrived (== publishes delivered). Any thread. */
	uint64 GetNumArrivals() const { return NumArrivals.load(std::memory_order_relaxed); }

	/** Copies actually issued. Any thread. WS-COPY-COST's "~0 when idle" observable. */
	uint64 GetNumCopies() const { return NumCopies.load(std::memory_order_relaxed); }

	/** Copies skipped by the extent guard (resize in flight). Any thread. */
	uint64 GetNumExtentSkips() const { return NumExtentSkips.load(std::memory_order_relaxed); }

	/**
	 * Mip-chain generations run after copies (== copies into a >1-mip destination).
	 * Any thread. The observable for BOTH halves of the bGenerateMips contract: rides
	 * NumCopies one-to-one when the option is on, stays exactly 0 when it is off.
	 */
	uint64 GetNumMipGenerations() const { return NumMipGenerations.load(std::memory_order_relaxed); }

private:
	/** The guarded copy: OutputRT -> Destination, or a counted skip on extent mismatch. */
	void CopyToDestination(FRHICommandList& RHICmdList);

	/** FGenerateMips over the (already >1-mip) destination; see the .cpp for the transition story. */
	void GenerateDestinationMips(FRHICommandList& RHICmdList);

	FVaCuusReplayRenderer Replayer;

	/** The one destination slot. Render thread only; see the class comment. */
	FTextureRHIRef Destination;

	/**
	 * RDG can only speak to Destination through an IPooledRenderTarget wrapper; cached
	 * so the wrapper is built once per destination instead of once per publish
	 * (CacheRenderTarget no-ops while the texture matches, PooledRenderTarget.h:470-475
	 * -- the engine resource keeps the same cache member for the same reason,
	 * FTextureRenderTarget2DResource::MipGenerationCache). Render thread only; dropped
	 * with the destination so it can never outlive-and-pin a texture the RT released.
	 */
	TRefCountPtr<IPooledRenderTarget> DestinationPooled;

	std::atomic<uint64> NumArrivals{0};
	std::atomic<uint64> NumCopies{0};
	std::atomic<uint64> NumExtentSkips{0};
	std::atomic<uint64> NumMipGenerations{0};
};
