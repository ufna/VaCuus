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

/**
 * The sample count `vacuus.ViewSampleCount` actually gets, given what the platform allows.
 *
 * A FREE FUNCTION SO IT IS TESTABLE WITHOUT A GPU, which matters because the failure it
 * prevents is a CRASH and not a wrong pixel: FVulkanTexture's sample switch ends in
 * `checkf(0, TEXT("Unsupported number of samples %d"))` for anything that is not a power of
 * two (VulkanTexture.cpp:481-506), so a typo'd `vacuus.ViewSampleCount 3` would take the
 * process down rather than degrade. Everything is rounded DOWN to a power of two in [1, 8]
 * and then clamped by PlatformMax (itself rounded down), so every path out of here is a
 * value the RHI can build.
 *
 * PlatformMax is GDynamicRHI->RHIGetPlatformTextureMaxSampleCount() at the call site --
 * 8 from the base class (DynamicRHI.h:920), lowered by the RHIs that know better.
 */
VACUUSRENDER_API int32 ResolveViewSampleCount(int32 Requested, int32 PlatformMax);

/** What one RenderToClipMask command has to do to the stencil buffer. See FVaCuusClipMaskState. */
struct FVaCuusClipMaskStep
{
	/** Clear the WHOLE stencil target to ClearValue before drawing the mask geometry. */
	bool bNeedsClear = false;
	uint32 ClearValue = 0;

	/** true = write WriteValue where the geometry covers; false = add 1 there instead. */
	bool bReplace = true;
	uint32 WriteValue = 1;

	/** The stencil ref every masked draw after this one tests EQUAL against. */
	uint32 TestValue = 1;
};

/**
 * The stencil VALUE PROTOCOL behind EnableClipMask/RenderToClipMask (bead VaCuus-4ik) -- the
 * half of the clip-mask pass that can be checked without a GPU in the room.
 *
 * RmlUi's contract, from the enum's own comments (RenderInterface.h:10-14): `Set` replaces the
 * mask with the geometry's area, `SetInverse` replaces it with the area OUTSIDE the geometry,
 * `Intersect` narrows the existing mask to the geometry. Lists are built by
 * ElementUtilities::ApplyActiveClipRegions -- Set for the first clipping ancestor, Intersect for
 * every one below it (ElementUtilities.cpp:165) -- and replayed by RenderManager::ApplyClipMask,
 * which calls EnableClipMask(true) and then one RenderToClipMask per element
 * (RenderManager.cpp:156-176). So a mask is always REBUILT from a Set/SetInverse; there is no
 * "pop one level" operation.
 *
 * THE REFERENCE BACKEND CLEARS THE WHOLE STENCIL ON EVERY `Set`
 * (RmlUi_Renderer_GL3.cpp:1135-1137, with its own `@performance Increment the reference value
 * instead of clearing each time` sitting right above it). We take the note's advice, because a
 * full-target clear is the one cost here that scales with RESOLUTION rather than with content:
 * at 1080x1920x4 samples a stencil clear touches 8.3M samples, and a document with a dozen
 * rounded scroll containers changes its mask list a dozen times per frame.
 *
 * SO VALUES ARE NEVER REUSED WITHIN A PASS. The render pass clears the stencil to 0 once at
 * BeginRenderPass; each `Set` then takes the next unused value, and the ONE INVARIANT that
 * makes this sound is
 *
 *     every value present anywhere in the stencil buffer is < NextFreeValue
 *
 * which holds by induction: `Set` writes NextFreeValue and then bumps it; `Intersect` can raise
 * a stale pixel to at most (max existing + 1), and it bumps NextFreeValue past that too. A
 * pixel left over from an earlier mask therefore can never equal the current TestValue, so the
 * stale mask cannot leak into the new one -- which is the exact failure a naive "always write 1"
 * scheme has, and it is why this is worth asserting rather than eyeballing.
 *
 * TWO OPERATIONS STILL FORCE A CLEAR, and both are named rather than hidden:
 *  - SetInverse, always. "Everything except the covered area passes" needs the passing value to
 *    be present where NOTHING was drawn, and no per-pixel write can put it there. (This is
 *    box-shadow's operation -- GeometryBoxShadow.cpp:206, :215, :221 -- so it is rare, and
 *    box-shadow needs two more things this bead does not build.)
 *  - Set, once the counter has climbed past HighWaterValue in a single pass. 8 bits is 255
 *    values and nothing bounds the number of mask changes in a frame, so the counter has to
 *    wrap somewhere; wrapping means a clear.
 *
 * A CLASS, NOT A FUNCTION, because the protocol is stateful by nature -- and a class with no RHI
 * in it, so VaCuus.Render.ClipMask.ValueProtocol can drive thousands of operations against a
 * SIMULATED stencil buffer and assert the invariant directly. That test runs under -nullrhi,
 * where the drawing tests cannot.
 */
class VACUUSRENDER_API FVaCuusClipMaskState
{
public:
	/**
	 * The `Set` value at which the next mask wraps and clears instead. 255 - 31: a wrap leaves at
	 * least 31 `Intersect` levels of headroom before SO_SaturatedIncrement would pin pixels at
	 * 255 and start over-including. RmlUi's list length is the number of CLIPPING ancestors of
	 * one element, so 31 is far past anything a document can produce -- see Step()'s ensure.
	 */
	static constexpr uint32 HighWaterValue = 224;

	/** Call once per render pass, matching the pass's own stencil clear to 0. */
	void BeginPass()
	{
		NextFreeValue = 1;
		TestValue = 0;
	}

	/** The stencil work one RenderToClipMask needs, and the ref its followers test against. */
	FVaCuusClipMaskStep Step(EVaCuusClipMaskOp Op);

	/** Ref for masked draws; 0 only before the first Step of a pass. */
	uint32 GetTestValue() const { return TestValue; }

	/** The invariant above, exposed so the test can assert it rather than restate it. */
	uint32 GetNextFreeValue() const { return NextFreeValue; }

private:
	uint32 NextFreeValue = 1;
	uint32 TestValue = 0;
};

/**
 * True when this buffer contains at least one RenderToClipMask, i.e. when the replay pass has to
 * attach a stencil target at all.
 *
 * WHY A SCAN RATHER THAN A FLAG ON THE BUFFER: the flag would be a new hashed field on a struct
 * whose hash image is guarded by a member-count static_assert (VaCuusCommandBuffer.h:679-690),
 * and it would be pure derivation -- the commands already say it. The scan runs once per
 * PUBLISHED frame (the idle gate makes that ~never on a static HUD) over a list the pass is
 * about to walk anyway, and it buys the whole stencil allocation for documents that never clip
 * under a transform, which today is every document.
 */
VACUUSRENDER_API bool BufferUsesClipMask(const FVaCuusCommandBuffer& Buffer);
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

	/**
	 * Latest replayed UI frame (PF_B8G8R8A8, premultiplied alpha, SRV state); null before the
	 * first replay.
	 *
	 * SINGLE-SAMPLED AT ViewSize WHATEVER `vacuus.ViewSampleCount` SAYS, and that is the whole
	 * reason the MSAA knob (bead VaCuus-3tg) needed no consumer to change. When the knob is on,
	 * the draws land in a multisampled companion and the render pass's own store action resolves
	 * it into THIS texture at EndRenderPass -- so the Slate composite
	 * (VaCuusSlateElement.cpp:200), the world sink's CopyTexture + mip chain
	 * (VaCuusWorldSink.cpp:66-100, which SKIPS on any extent mismatch) and every readback still
	 * see the same format, the same extent and the same SRVMask state they always did.
	 */
	FTextureRHIRef GetOutputRT() const { return OutputRT; }

	/**
	 * Samples the replay pass is currently rasterizing with: 1 (no MSAA texture) or whatever
	 * `vacuus.ViewSampleCount` resolved to.
	 *
	 * THE OBSERVABLE FOR THE KNOB, and it has to be this rather than GetOutputRT()->GetNumSamples()
	 * -- which is 1 by design on every path, so it can never distinguish "MSAA off" from "MSAA on
	 * and resolving". VaCuus.Render.MSAA.OutputRTUnchanged asserts against it.
	 */
	int32 GetReplaySampleCount() const { return MSAART.IsValid() ? int32(MSAART->GetDesc().NumSamples) : 1; }

	/**
	 * Bytes the clip-mask stencil target is currently holding, 0 while no replayed buffer has
	 * ever used a clip mask.
	 *
	 * THE OBSERVABLE FOR THE LAZY ALLOCATION, and the reason it is a byte count rather than a
	 * bool: the cost this feature adds is memory, the buyer-facing number is per view at the
	 * view's resolution and sample count, and a test that asserts "0 for a document that does
	 * not clip" is asserting the thing that actually matters. PF_DepthStencil's bytes-per-block
	 * is the platform's (D24S8 on most, D32S8X24 on some), so this reads GPixelFormats rather
	 * than assuming 4.
	 */
	uint64 GetClipMaskStencilBytes() const;

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

	/**
	 * Brings MSAART in line with `vacuus.ViewSampleCount` and the given size, releasing it when
	 * the knob is off. Called from EnsureOutputRT so the two targets can never disagree about
	 * extent -- a resolve whose source and destination differ is an RHI error, and the size is
	 * only known there.
	 */
	void EnsureSampleTarget(FRHICommandList& RHICmdList, FIntPoint ViewSize);

	/**
	 * Brings StencilRT in line with the colour target's extent and SAMPLE COUNT, creating it on
	 * first use. Called from ReplayCommands, before BeginRenderPass, and only for a buffer that
	 * actually carries a clip mask.
	 *
	 * SAMPLE COUNT IS THE HALF THAT MUST NOT BE GOT WRONG: a depth-stencil attachment has to
	 * agree with its colour attachment on samples, so this reads GetReplaySampleCount() -- the
	 * count the pass is REALLY rasterizing with -- and not the cvar, which can have been changed
	 * since MSAART was built and is capped by the platform anyway.
	 */
	void EnsureStencilTarget(FRHICommandList& RHICmdList, FIntPoint ViewSize);

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

	/**
	 * The multisampled companion the replay pass rasterizes into while `vacuus.ViewSampleCount`
	 * is above 1, resolved into OutputRT by the pass's own store action. Null = knob off, and
	 * then ReplayCommands takes exactly the path it took before bead VaCuus-3tg.
	 *
	 * IT ADDS TO OutputRT RATHER THAN REPLACING IT, which is the knob's entire cost: 1080p
	 * PF_B8G8R8A8 is 7.91 MiB, so 2x is +15.8 and 4x is +31.6 MiB per view, permanently, on top
	 * of the 7.91 that has to stay because nothing downstream can sample a multisampled texture.
	 * The cost table is in docs/buyer/perf-guide.md.
	 *
	 * NO ShaderResource FLAG, deliberately: nothing samples it and asking for a sampled
	 * multisampled image is how you fail texture creation on the RHIs that restrict them.
	 * RenderTargetable is what a resolve source needs and all it needs.
	 *
	 * IT NEVER LEAVES ERHIAccess::RTV. A colour attachment that is only ever an attachment has
	 * no second state to be in -- the Vulkan resolve happens INSIDE the render pass through
	 * pResolveAttachments (VulkanRenderpass.h:147-153), not as a separate transfer -- so the
	 * only resource whose state moves is the resolve DESTINATION, and ReplayCommands round-trips
	 * that one through ResolveDst exactly like it round-trips it through RTV without the knob.
	 */
	FTextureRHIRef MSAART;

	/**
	 * Depth-stencil companion for the clip-mask pass (bead VaCuus-4ik). Null until a replayed
	 * buffer first carries a RenderToClipMask; after that it lives as long as the view does.
	 *
	 * ALLOCATED LAZILY AND THEN KEPT, which is a deliberate asymmetry with MSAART above.
	 * MSAART is released the moment its cvar is turned off, because the cvar IS the feature and a
	 * knob that does not give the memory back is a lie. This target has no knob: it is demanded
	 * by the DOCUMENT, and a document that clipped once will clip again on the next frame that
	 * moves. Freeing it between frames would trade a fixed allocation for a per-frame one.
	 *
	 * WHAT IT COSTS, per view: PF_DepthStencil is 4 bytes per sample on the common platforms, so
	 * 1080p is 7.91 MiB at 1x, 15.8 at 2x, 31.6 at 4x, 63.3 at 8x -- the same table as MSAART's,
	 * for the same reason (same extent, same sample count, same bytes per sample). Nothing pays
	 * it unless a mask is actually recorded. GetClipMaskStencilBytes() is the observable.
	 *
	 * NEITHER PLANE IS EVER STORED OR SAMPLED. The pass clears both at BeginRenderPass and
	 * discards both at EndRenderPass (ClearDepthStencil_DontStoreDepthStencil), and no
	 * ShaderResource flag is asked for -- so the glass distiller, the world sink's CopyTexture
	 * and the Slate composite still see exactly one texture, single-sampled OutputRT, unchanged.
	 * The depth plane exists only because no RHI here offers a stencil-only format.
	 */
	FTextureRHIRef StencilRT;

	/** Latched so the sample count a session actually got is logged once rather than per frame. */
	bool bLoggedSampleCount = false;

	/** Latched so the stencil allocation is announced once per view, not once per frame. */
	bool bLoggedStencilTarget = false;

	/** Newest buffer generation consumed so far (drawn via Replay or resource-only via ConsumeResources). */
	uint64 LastConsumedGeneration = 0;

	/** See GetNumAsyncTextureUploads(). */
	std::atomic<uint64> NumAsyncTextureUploads{0};
	std::atomic<uint64> NumSyncTextureUploads{0};
};
