// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FRHICommandListImmediate;
struct FVaCuusCommandBuffer;

/**
 * What a document host publishes recorded frames INTO -- the render-thread half of a
 * view, seen from the UI thread (M5 spec 2(g), 3.4).
 *
 * Exactly the two calls FVaCuusRmlDocumentHost makes on its render side, no more: the
 * publish (VaCuusRmlDocumentHost.cpp, RecordAndPublishFrame's ENQUEUE_RENDER_COMMAND)
 * and the teardown release (Shutdown's). Everything else on FVaCuusSlateElement --
 * SetDestRect, the glass HDR mirror -- is called by the WIDGET that owns the concrete
 * element, not by the host, so it stays off the interface: a sink with no Slate
 * composite (the world sink) has no DestRect to be told about.
 *
 * Implementations:
 *  - FVaCuusSlateElement: queue + distill on publish, replay at Slate paint, composite
 *    into the window (the screen path, unchanged by this interface's extraction);
 *  - FVaCuusWorldSink: replay on arrival, one CopyTexture into a component's
 *    UTextureRenderTarget2D (the world path).
 *
 * THREAD CONTRACT: both methods run on the render thread only -- the host reaches them
 * exclusively through ENQUEUE_RENDER_COMMAND from the UI thread, which is also what
 * orders the release after the view's last publish (same-thread enqueues keep their
 * order). The host holds the sink as a thread-safe shared ptr captured by value into
 * those commands, so the sink outlives every enqueue.
 */
class IVaCuusFrameSink
{
public:
	virtual ~IVaCuusFrameSink() = default;

	/**
	 * Takes ownership of one published command buffer. Buffers arrive in publish order
	 * (monotonic Generation) and each carries the resource DELTA since the last, so an
	 * implementation may defer or coalesce draws but must never DROP a buffer's
	 * resource traffic (FVaCuusReplayRenderer::ConsumeResources is the drop-safe drain).
	 *
	 * THE IMMEDIATE LIST IS THE CONTRACT, not merely the list that happens to be at hand
	 * (bead VaCuus-9b3). Starting the async upload for a large payload means handing a
	 * parallel list to FRHICommandListImmediate::QueueAsyncCommandListSubmit
	 * (RHICommandList.h:4448-4453), and this parameter having been FRHICommandList& is the
	 * whole reason the world sink kept uploading inline for a milestone: the publish site
	 * always HAD the immediate list (VaCuusRmlDocumentHost.cpp's ENQUEUE_RENDER_COMMAND,
	 * whose lambda parameter is FRHICommandListImmediate&), the interface was the only
	 * thing that threw the type away. Widening it costs implementors nothing — every
	 * FRHICommandList& they pass on still binds — and buys the compiler the ability to
	 * reject a sink reached from anywhere else.
	 */
	virtual void SetPendingBuffer_RenderThread(FRHICommandListImmediate& RHICmdList, TUniquePtr<FVaCuusCommandBuffer> InBuffer) = 0;

	/** Drops all RHI resources; the sink draws/copies nothing afterwards. */
	virtual void ReleaseResources_RenderThread() = 0;

	/**
	 * This view's published texture census, both figures, read from ANY thread (VaCuus-cyn).
	 * The only method here that is not a render-thread call, and the only one the UI thread
	 * may touch: it reads two atomics the replayer stores at publish time.
	 *
	 * WHY IT IS ON THIS INTERFACE, which is otherwise exactly the host's two render-side
	 * calls. The eviction sweep is per-view and lives on the host; the allocated:logical
	 * relationship it needs to make vacuus.TextureBudgetMB mean allocated bytes is known only
	 * where the textures are created, and the sink is the ONLY edge the host already has to
	 * that side. The alternative was the process-wide census, which blends other views in.
	 *
	 * PURE, WITH TWO TEST FAKES THAT MUST WRITE `0, 0` BY HAND, deliberately: a default that
	 * answered "no allocated figure" would let a sink silently put the budget back in logical
	 * bytes -- the exact bug this closes -- and no test would see it. OutAllocated == 0 means
	 * "cannot say" (NullRHI, or a sink with no replayer), never "nothing resident".
	 */
	virtual void GetPublishedTextureCensus(uint64& OutLogicalBytes, uint64& OutAllocatedBytes) const = 0;
};
