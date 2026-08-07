// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusEngineCompat.h" // FVaCuusDrawPassInputs + the engine-version seam (M6 spec §2(f))
#include "VaCuusFrameSink.h"
#include "VaCuusGlassDistiller.h"
#include "VaCuusReplayRenderer.h"

#include "Rendering/RenderingCommon.h"

struct FVaCuusCommandBuffer;

/**
 * ICustomSlateElement that puts recorded UI frames on screen: replays the
 * pending command buffers into the replayer's persistent RT, runs the M5 glass
 * passes against the live scene, then composites that RT into the Slate
 * elements texture at DestRect with premultiplied-over blending.
 *
 * Also the screen-path IVaCuusFrameSink (M5 Task 6): the document host publishes
 * through that interface, and this class's two sink methods predate it -- the
 * extraction changed their declarations, not a line of their bodies, which is what
 * keeps the glass and material suites' behavior identical by construction.
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
class FVaCuusSlateElement : public ICustomSlateElement, public IVaCuusFrameSink
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
	 *
	 * Also the glass distillation point (M5): every arriving buffer — in
	 * publish order, before any queue trimming can touch it — rebuilds the
	 * glass list wholesale, so parsing rides the publish rate (~zero when
	 * idle) and never the engine frame rate. See FVaCuusGlassDistiller.
	 */
	virtual void SetPendingBuffer_RenderThread(FRHICommandListImmediate& RHICmdList, TUniquePtr<FVaCuusCommandBuffer> InBuffer) override;

	/** Window-space pixel rect the UI RT is composited into (pre-ElementsOffset). */
	void SetDestRect_RenderThread(const FIntRect& InDestRect);

	/**
	 * The game-thread HDR mirror (Exp-GLASS-HDR-DETECT's shipped answer): under
	 * bCompositeUIWithSceneHDR the elements texture carries no scene and no
	 * FDrawPassInputs field says so — bOutputIsHDRDisplay is reset false for the SDR
	 * elements pass (SlateRHIRenderer.cpp:1068) — so the widget reads
	 * r.HDR.EnableHDROutput game-side each paint and pushes the verdict here. False
	 * disables every glass pass; the UI itself keeps compositing.
	 */
	void SetGlassAllowed_RenderThread(bool bInAllowed);

	/** Drops the replayer's RHI resources and any queued buffers; Draw becomes a no-op. */
	virtual void ReleaseResources_RenderThread() override;

	//~ Begin ICustomSlateElement
	// FVaCuusDrawPassInputs = the engine's FDrawPassInputs behind the version seam
	// (VaCuusEngineCompat.h hotspot 1); field reads go through VaCuusCompat accessors.
	virtual void Draw_RenderThread(FRDGBuilder& GraphBuilder, const FVaCuusDrawPassInputs& Inputs) override;
	//~ End ICustomSlateElement

	/**
	 * Engine frames on which the glass passes actually drew (render thread). The
	 * idle-freeze observable: on an idle glass HUD this keeps counting while
	 * FramesPublished stands still — glass is engine-frame work by design (spec §6).
	 */
	uint64 GetNumGlassFrames_RenderThread() const { return NumGlassFrames; }

private:
	/** Queue length that triggers the inline backlog consume in SetPendingBuffer_RenderThread. */
	static constexpr int32 MaxPendingBuffers = 4;

	/** Adds the downsample -> blur -> masked-draw chain for the current glass list. */
	void AddGlassPasses(FRDGBuilder& GraphBuilder, const FVaCuusDrawPassInputs& Inputs);

	/** (Re)creates VB/IB pairs for the current glass entries (mask geometry or generated quads). */
	void RefreshGlassDrawResources(FRHICommandList& RHICmdList);

	FVaCuusReplayRenderer Replayer;

	/** Recorded frames awaiting replay, oldest first (see SetPendingBuffer_RenderThread). */
	TArray<TUniquePtr<FVaCuusCommandBuffer>> PendingBuffers;

	/** Composite destination in window-space pixels; empty until the first paint. */
	FIntRect DestRect = FIntRect(0, 0, 0, 0);

	/**
	 * The glass list + its cross-buffer maps, living here beside DestRect because both
	 * persist across idle frames exactly like the RT itself (spec §2(a)): replay stops
	 * when publishes stop, the scene does not, and these are what Draw_RenderThread keeps
	 * drawing from.
	 */
	FVaCuusGlassDistiller GlassDistiller;

	/** One glass entry's uploaded geometry: the mask copy, or a generated DrawRegion quad. */
	struct FGlassDraw
	{
		FBufferRHIRef VB;
		FBufferRHIRef IB;
		int32 NumVertices = 0;
		int32 NumIndices = 0;
	};

	/** Parallel to GlassDistiller.GetEntries(); rebuilt when the list generation moves. */
	TArray<FGlassDraw> GlassDraws;
	uint64 GlassDrawsGeneration = 0;

	/**
	 * The pooled half-res pair the blur ping-pongs through: persistent (recreated only
	 * when the required extent or the output format changes), registered external each
	 * frame, sized to the mapped glass bounds rather than the screen (spec §2(c)).
	 * Outside the glass passes both stay in SRVMask, the same invariant as the UI RT.
	 */
	FTextureRHIRef GlassHalfRT[2];

	/** See SetGlassAllowed_RenderThread. */
	bool bGlassAllowed = true;

	/** See GetNumGlassFrames_RenderThread. */
	uint64 NumGlassFrames = 0;

	/** Latched log for which backbuffer-access path this session took (Exp-GLASS-BACKBUFFER-SRV). */
	bool bLoggedBackbufferPath = false;

	/** Latched log for which composite gamma permutation this session selected (M6, spec §3.2). */
	bool bLoggedCompositeGamma = false;
};
