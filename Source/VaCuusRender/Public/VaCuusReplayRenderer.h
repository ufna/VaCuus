// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusCommandBuffer.h"

#include "RHIResources.h"

class FRHICommandList;

/**
 * Render-thread consumer of FVaCuusCommandBuffer: owns the RHI mirror of the
 * recorder's resources (vertex/index buffers, textures) and a persistent
 * render target the recorded commands are replayed into each frame.
 *
 * THREAD AFFINITY: render thread only. Every method — including the
 * destructor — must run on the render thread; the recorded buffers cross
 * threads, this class does not.
 *
 * Replay contract (mirrors FVaCuusCommandBuffer):
 *  - NewGeometry/NewTextures are uploaded BEFORE the commands run, so a
 *    resource created and drawn in the same buffer works.
 *  - Released* lists are processed AFTER the commands run (deferred release:
 *    the buffer carrying a release may still draw with that handle).
 *  - A buffer whose Generation was already replayed is skipped (idempotent).
 */
class VACUUSRENDER_API FVaCuusReplayRenderer
{
public:
	/**
	 * Uploads the buffer's resource delta, replays its commands into the
	 * persistent output RT (recreated on ViewSize change), then retires the
	 * buffer's released handles.
	 */
	void Replay(FRHICommandList& RHICmdList, const FVaCuusCommandBuffer& Buffer);

	/** Latest replayed UI frame (PF_B8G8R8A8, premultiplied alpha, SRV state); null before the first replay. */
	FTextureRHIRef GetOutputRT() const { return OutputRT; }

	/** Render-thread teardown: drops all RHI resources and resets replay state. */
	void ReleaseResources();

private:
	/** (Re)creates OutputRT when missing or when ViewSize changed. */
	void EnsureOutputRT(FRHICommandList& RHICmdList, FIntPoint ViewSize);

	/** Creates RHI resources for the buffer's NewGeometry/NewTextures. */
	void UploadNewResources(FRHICommandList& RHICmdList, const FVaCuusCommandBuffer& Buffer);

	/** Replays Buffer.Commands into OutputRT (assumes OutputRT matches Buffer.ViewSize). */
	void ReplayCommands(FRHICommandList& RHICmdList, const FVaCuusCommandBuffer& Buffer);

	struct FGeometry
	{
		FBufferRHIRef VB;
		FBufferRHIRef IB;
		int32 NumVertices = 0; // DrawIndexedPrimitive needs the referenced vertex span
		int32 NumIndices = 0;
	};

	TMap<FVaCuusGeometryHandle, FGeometry> Geometry;
	TMap<FVaCuusTextureHandle, FTextureRHIRef> Textures;
	FTextureRHIRef OutputRT;
	uint64 LastReplayedGeneration = 0;
};
