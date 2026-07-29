// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusReplayRenderer.h"

#include "Rendering/RenderingCommon.h"

struct FVaCuusCommandBuffer;

/**
 * ICustomSlateElement that puts recorded UI frames on screen: replays the
 * pending command buffers into the replayer's persistent RT, then composites
 * that RT into the Slate elements texture at DestRect with premultiplied-over
 * blending.
 *
 * THREAD AFFINITY: all mutable state belongs to the render thread. The game
 * thread talks to the element exclusively through ENQUEUE_RENDER_COMMAND'd
 * setters (Noesis pattern); Draw_RenderThread runs on the render thread at
 * RDG graph-build time during Slate window rendering.
 *
 * Lifetime: handed to FSlateDrawElement::MakeCustom as a thread-safe shared
 * ptr; Slate keeps the ref alive across the frame's render commands. Teardown
 * enqueues ReleaseResources_RenderThread, after which Draw is a no-op.
 */
class FVaCuusSlateElement : public ICustomSlateElement
{
public:
	/**
	 * Takes ownership of the next recorded frame. Buffers queue up (instead of
	 * replacing) because each one carries the resource DELTA since the last:
	 * dropping a buffer would lose geometry/texture creations and releases.
	 * The drain draws only the newest buffer; older ones are consumed for
	 * their resources alone (FVaCuusReplayRenderer::ConsumeResources). In
	 * steady state Tick and paint alternate 1:1 (the widget is volatile) and
	 * the queue holds one entry; past MaxPendingBuffers the backlog is
	 * consumed inline so memory stays bounded even if some undiscovered
	 * tick-without-paint path defeats the volatility guarantee.
	 */
	void SetPendingBuffer_RenderThread(FRHICommandList& RHICmdList, TUniquePtr<FVaCuusCommandBuffer> InBuffer);

	/** Window-space pixel rect the UI RT is composited into (pre-ElementsOffset). */
	void SetDestRect_RenderThread(const FIntRect& InDestRect);

	/** Drops the replayer's RHI resources and any queued buffers; Draw becomes a no-op. */
	void ReleaseResources_RenderThread();

	//~ Begin ICustomSlateElement
	virtual void Draw_RenderThread(FRDGBuilder& GraphBuilder, const FDrawPassInputs& Inputs) override;
	//~ End ICustomSlateElement

private:
	/** Queue length that triggers the inline backlog consume in SetPendingBuffer_RenderThread. */
	static constexpr int32 MaxPendingBuffers = 4;

	FVaCuusReplayRenderer Replayer;

	/** Recorded frames awaiting replay, oldest first (see SetPendingBuffer_RenderThread). */
	TArray<TUniquePtr<FVaCuusCommandBuffer>> PendingBuffers;

	/** Composite destination in window-space pixels; empty until the first paint. */
	FIntRect DestRect = FIntRect(0, 0, 0, 0);
};
