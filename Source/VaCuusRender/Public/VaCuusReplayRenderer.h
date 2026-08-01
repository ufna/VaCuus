// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusCommandBuffer.h"

#include "RHIResources.h"

class FRHICommandList;

namespace VaCuusReplay
{
/**
 * Pixel-space -> clip-space ortho in UE row-vector convention (v' = v * M); defined with
 * the replayer, shared with the M5 glass draw so the two paths cannot disagree about
 * orientation. See the definition for the conventions.
 */
VACUUSRENDER_API FMatrix44f MakePixelToClipMatrix(FIntPoint ViewSize);
} // namespace VaCuusReplay

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
 *  - A buffer whose Generation was already consumed is skipped (idempotent).
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

	/**
	 * Consumes only the buffer's resource traffic: uploads NewGeometry/
	 * NewTextures, retires the Released* handles and advances the generation —
	 * no RT access, no draw pass. For draining a buffer backlog where only the
	 * newest buffer gets drawn (each buffer repaints the whole frame, so the
	 * older draws are worthless, but their resource DELTAS are not).
	 *
	 * Ordering safety: buffer N+1 was recorded after buffer N's releases were
	 * issued, so N+1 never references a handle that N released — consuming
	 * N's releases before N+1 draws is sound.
	 */
	void ConsumeResources(FRHICommandList& RHICmdList, const FVaCuusCommandBuffer& Buffer);

	/** Latest replayed UI frame (PF_B8G8R8A8, premultiplied alpha, SRV state); null before the first replay. */
	FTextureRHIRef GetOutputRT() const { return OutputRT; }

	/**
	 * (Re)creates OutputRT when missing or when ViewSize changed; no-op for a
	 * degenerate size. Public for the Slate element: the RT must exist at RDG
	 * graph-build time so it can be registered for the composite pass, while
	 * Replay() itself only runs later inside the pass lambda (Replay calls
	 * this internally, so the second call is a no-op).
	 */
	void EnsureOutputRT(FRHICommandList& RHICmdList, FIntPoint ViewSize);

	/**
	 * Render-thread teardown: drops all RHI resources and resets replay state.
	 *
	 * RESETTING LastConsumedGeneration IS LOAD-BEARING IN A WAY IT WAS NOT BEFORE THE IDLE
	 * GATE (M2 Task 12), and nothing exploits that today only because of how the pieces are
	 * currently paired. Until the gate existed, the recorder republished the whole frame
	 * every tick, so a buffer that was lost, or an RT that went away, self-healed on the
	 * NEXT frame. Now, on a static UI, the recorder deliberately never resends: the content
	 * hash matches, the frame is withheld, and this render target is the only copy of those
	 * pixels anywhere (FVaCuusRecordingRenderInterface::EndFrameAndPublish).
	 *
	 * So any FUTURE path that drops the RT without also destroying the recorder -- or that
	 * discards an unreplayed buffer -- produces a PERMANENTLY blank UI rather than a
	 * one-frame glitch, and nothing will log a thing. What keeps it safe here is that this
	 * runs with the recorder's own teardown, and the fresh recorder that replaces it publishes
	 * its first frame unconditionally, at generation 1, with no previous hash to compare
	 * against (the first-frame assertions in VaCuus.Render.IdleGate.UnchangedFrame, restated
	 * for a fresh recorder in .PerRecorder). Anyone adding a partial release must either bring
	 * the recorder with it or give the recorder a way to be told "republish next frame".
	 *
	 * Until then, vacuus.IdleGate 0 is the in-the-field workaround: it forces every recorded
	 * frame to publish, so a UI blanked by a lost RT comes back on the next frame.
	 */
	void ReleaseResources();

private:

	/** Idempotence/order guard shared by Replay and ConsumeResources; false = already consumed, skip. */
	bool ShouldConsume(const FVaCuusCommandBuffer& Buffer) const;

	/** Creates RHI resources for the buffer's NewGeometry/NewTextures. */
	void UploadNewResources(FRHICommandList& RHICmdList, const FVaCuusCommandBuffer& Buffer);

	/** Replays Buffer.Commands into OutputRT (assumes OutputRT matches Buffer.ViewSize). */
	void ReplayCommands(FRHICommandList& RHICmdList, const FVaCuusCommandBuffer& Buffer);

	/** Retires the buffer's Released* handles and marks its generation consumed. */
	void RetireBufferResources(const FVaCuusCommandBuffer& Buffer);

	struct FGeometry
	{
		FBufferRHIRef VB;
		FBufferRHIRef IB;
		int32 NumVertices = 0; // DrawIndexedPrimitive needs the referenced vertex span
		int32 NumIndices = 0;
	};

	TMap<FVaCuusGeometryHandle, FGeometry> Geometry;
	TMap<FVaCuusTextureHandle, FTextureRHIRef> Textures;

	/**
	 * Compiled shaders, beside Geometry/Textures with the same upload/retire lifecycle —
	 * but the "upload" creates no RHI object: a compiled gradient IS its parameters
	 * (FVaCuusShaderDesc), read at every DrawShader bind. The map entry is still a real
	 * resource with the deferred-release discipline, because a desc retired early would
	 * strand a later command's Shader handle exactly like a dropped texture.
	 */
	TMap<FVaCuusShaderHandle, FVaCuusShaderDesc> Shaders;

	FTextureRHIRef OutputRT;

	/** Newest buffer generation consumed so far (drawn via Replay or resource-only via ConsumeResources). */
	uint64 LastConsumedGeneration = 0;
};
