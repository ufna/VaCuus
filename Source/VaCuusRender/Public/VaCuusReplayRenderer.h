// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusCommandBuffer.h"

#include "RHIResources.h"

#include <atomic>

class FRHICommandList;
class FRHICommandListImmediate;

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

	/**
	 * Starts the ASYNC half of the upload for every texture payload in this buffer at or above
	 * `vacuus.AsyncTextureUploadBytes`, and takes those payloads out of the buffer. Handles it
	 * takes are then skipped by the UploadNewResources() that follows inside the replay pass.
	 *
	 * WHY THIS IS A SEPARATE ENTRY POINT AND NOT PART OF Replay(), which is the whole shape of
	 * bead akj.6.25 and is forced by the engine rather than chosen: the async list is handed to
	 * the immediate list with FRHICommandListImmediate::QueueAsyncCommandListSubmit, and that
	 * call DETACHES AND DISPATCHES whatever the immediate list has recorded so far --
	 * "Commands may already be queued on the immediate command list. These need to be executed
	 * first before any parallel commands can be inserted, otherwise commands will run
	 * out-of-order" (FRHICommandListExecutor::Submit, RHICommandList.cpp:1482-1510). Inside an
	 * RDG pass lambda that would dispatch the graph's own half-recorded stream. So the call has
	 * to sit at RDG GRAPH-BUILD time, next to EnsureOutputRT, which is exactly where the engine
	 * makes its own (ShadowSetup.cpp:6902, SkeletalMeshUpdater.cpp:440, both on
	 * GraphBuilder.RHICmdList).
	 *
	 * ORDERING IS THE POINT AND IT IS THE ENGINE'S, not a fence of ours: the async list takes
	 * its place in the immediate stream HERE, and every command the replay pass records
	 * afterwards executes after it. The RHI thread waits on the recording task before replaying
	 * the list ("The provided command lists are not dispatched until FinishRecording() is called
	 * on them, and their dispatch prerequisites have been completed", RHICommandList.h:4448-4453),
	 * so a draw can never sample an unfilled image. What moves off the render thread is the
	 * staging memcpy -- on Vulkan the whole measured cost, since RHIUpdateTexture2D acquires a
	 * staging buffer and memcpy's the payload into it ON THE CALLING THREAD and only then
	 * enqueues the GPU copy (VulkanTexture.cpp:1664-1696).
	 *
	 * RENDER THREAD ONLY, and the buffer is taken by non-const reference because the payload is
	 * MOVED into the task: the command buffer dies with the replay pass and the memcpy has not
	 * necessarily happened by then.
	 */
	void BeginAsyncTextureUploads(FRHICommandListImmediate& RHICmdList, FVaCuusCommandBuffer& Buffer);

	/** Latest replayed UI frame (PF_B8G8R8A8, premultiplied alpha, SRV state); null before the first replay. */
	FTextureRHIRef GetOutputRT() const { return OutputRT; }

	/**
	 * Texture payloads this replayer sent down the async path, and the ones it uploaded inline.
	 *
	 * THE OBSERVABLE FOR THE THRESHOLD, which otherwise has none: both paths leave the same
	 * pixels in the same map, so "the big one went async and the small one did not" is invisible
	 * from every other angle -- the RT looks identical either way and the wall clock on a shared
	 * build box is not an assertion. VaCuus.Render.Upload.AsyncPayload reads both.
	 *
	 * Atomic for the same reason FVaCuusRecordingRenderInterface::NumDecodeArrivals is: they are
	 * written on the render thread and read by a test on the game thread after
	 * FlushRenderingCommands(), and a flush orders the work but not a plain uint64 read.
	 */
	uint64 GetNumAsyncTextureUploads() const { return NumAsyncTextureUploads.load(std::memory_order_acquire); }
	uint64 GetNumSyncTextureUploads() const { return NumSyncTextureUploads.load(std::memory_order_acquire); }

#if WITH_DEV_AUTOMATION_TESTS
	/** The RHI texture a handle resolves to, or null. Tests only: the map is render-thread-private state. */
	FRHITexture* GetTextureForTest(FVaCuusTextureHandle Handle) const
	{
		const FTextureRHIRef* Found = Textures.Find(Handle);
		return Found ? Found->GetReference() : nullptr;
	}
#endif

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

	/**
	 * Textures BeginAsyncTextureUploads() created and whose payload it took, held until the
	 * UploadNewResources() of the buffer they came from installs them into Textures above.
	 *
	 * THIS MAP IS WHAT KEEPS THE HANDOVER IN BUFFER ORDER, and skipping it was a real bug rather
	 * than a tidiness question. The async start runs at RDG graph-build time, for EVERY queued
	 * buffer at once; the replay pass then consumes those buffers one at a time, oldest first.
	 * Installing straight into Textures at graph-build time therefore let an OLDER buffer's
	 * upload run afterwards and win -- and the one handle that legitimately appears in two
	 * buffers is exactly the interesting one: a late image decode records the 1x1 placeholder in
	 * the buffer that ran LoadTexture and the real payload in a later one
	 * (FVaCuusCommandBuffer::NewTextures). Placeholder in buffer N-1, payload in buffer N, both
	 * queued, and the transparent 1x1 overwrote the image that had just been uploaded.
	 *
	 * Deferring to UploadNewResources costs nothing: that is the first thing Replay() does, so
	 * the handle still resolves for every draw the same buffer records.
	 *
	 * A MAP RATHER THAN A SET OF HANDLES because the entry must also carry the ref -- and either
	 * way it cannot be an empty-payload test, which is the trap in the cheaper version: the
	 * payload is MOVED out, so what NewTextures still holds is a zero-byte entry that
	 * UploadNewResources' payload-mismatch ensure would report as a corrupt buffer.
	 */
	TMap<FVaCuusTextureHandle, FTextureRHIRef> PendingAsyncTextures;

	FTextureRHIRef OutputRT;

	/** Newest buffer generation consumed so far (drawn via Replay or resource-only via ConsumeResources). */
	uint64 LastConsumedGeneration = 0;

	/** See GetNumAsyncTextureUploads(). */
	std::atomic<uint64> NumAsyncTextureUploads{0};
	std::atomic<uint64> NumSyncTextureUploads{0};
};
