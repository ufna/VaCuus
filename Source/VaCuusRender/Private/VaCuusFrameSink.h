// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FRHICommandList;
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
	 */
	virtual void SetPendingBuffer_RenderThread(FRHICommandList& RHICmdList, TUniquePtr<FVaCuusCommandBuffer> InBuffer) = 0;

	/** Drops all RHI resources; the sink draws/copies nothing afterwards. */
	virtual void ReleaseResources_RenderThread() = 0;
};
