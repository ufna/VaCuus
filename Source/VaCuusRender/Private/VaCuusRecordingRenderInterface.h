// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusCommandBuffer.h"

#include "Containers/MpscQueue.h"
#include "Tasks/Task.h"

#include <RmlUi/Core/RenderInterface.h>

#include <atomic>

class IImageWrapperModule;

/** One finished async image decode on its way back to the recorder. */
struct FVaCuusTextureDecode
{
	FVaCuusTextureHandle Handle = 0;

	/** Premultiplied RGBA8, ready for NewTextures. A zero Size means the decode FAILED. */
	FVaCuusTextureData Data;

	/** Source path, kept only so a failed decode can name the file it choked on. */
	FString Source;
};

/**
 * Rendezvous between the decode workers and the recorder's UI thread, held by
 * TSharedRef so it outlives the recorder.
 *
 * THIS IS THE STRUCTURAL ANSWER to "recorder destroyed with decodes in flight":
 * a task never touches the recorder, only its own reference to this sink, so
 * there is nothing to dangle. The recorder's destructor sets bAbandoned and
 * drops its reference; the last in-flight task frees the sink. Nobody blocks the
 * UI thread waiting for a decode, which is the entire point of the async path.
 */
struct FVaCuusTextureDecodeSink
{
	/**
	 * MPSC by construction: every decode task is its own producer thread, and only
	 * the frame-owning thread dequeues. NOT TSpscQueue (used for the game -> UI
	 * thread queues in VaCuusUIQueues.h) — that one is valid for exactly one
	 * producer. TQueue/TCircularQueue/TAtomic all carry 5.8 deprecation notices;
	 * TMpscQueue does not, and its destructor destroys the elements still queued
	 * (Containers/MpscQueue.h:29-43) so an abandoned sink leaks nothing.
	 */
	TMpscQueue<FVaCuusTextureDecode> Completed;

	/** Set by the recorder's destructor: a task that sees it drops its work. */
	std::atomic<bool> bAbandoned{false};
};

/**
 * Rml::RenderInterface implementation that records the frame into an
 * FVaCuusCommandBuffer instead of touching any RHI. Thread-agnostic
 * single-writer contract: any one thread may drive the recorder
 * (BeginFrame() / Context::Render() / EndFrameAndPublish()), but calls must
 * never race. While a frame is open, the recorder is pinned to the thread
 * that called BeginFrame() — enforced with an ensure. Published buffers may
 * be consumed on any thread.
 *
 * Resource calls (Compile/Generate/Load/Release*) are legal outside a
 * Begin/End pair: RmlUi releases geometry and textures during document
 * teardown and Rml::Shutdown(). Such traffic accumulates in the pending
 * buffer and rides along with the next published frame. Draw-state calls
 * (RenderGeometry, scissor, transform) outside a frame are a caller bug:
 * ensureMsgf + drop.
 *
 * Image decodes are the one piece of work that leaves the frame-owning thread:
 * see LoadTexture and DrainCompletedDecodes.
 */
class VACUUSRENDER_API FVaCuusRecordingRenderInterface : public Rml::RenderInterface
{
public:
	virtual ~FVaCuusRecordingRenderInterface();

	//~ Begin Rml::RenderInterface
	virtual Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> Vertices, Rml::Span<const int> Indices) override;
	virtual void RenderGeometry(Rml::CompiledGeometryHandle Handle, Rml::Vector2f Translation, Rml::TextureHandle Texture) override;
	virtual void ReleaseGeometry(Rml::CompiledGeometryHandle Handle) override;
	virtual Rml::TextureHandle LoadTexture(Rml::Vector2i& OutDimensions, const Rml::String& Source) override;
	virtual Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> SourceData, Rml::Vector2i Dimensions) override;
	virtual void ReleaseTexture(Rml::TextureHandle Handle) override;
	virtual void EnableScissorRegion(bool bEnable) override;
	virtual void SetScissorRegion(Rml::Rectanglei Region) override;
	virtual void SetTransform(const Rml::Matrix4f* Transform) override;
	//~ End Rml::RenderInterface

	/**
	 * Opens a frame: subsequent draw-state calls are recorded into the pending
	 * buffer. Also the drain point for finished async image decodes.
	 */
	void BeginFrame(FIntPoint ViewSize);

	/**
	 * Closes the frame and hands out the pending buffer with a strictly
	 * increasing Generation. The next frame starts from a fresh buffer,
	 * though out-of-frame resource traffic arriving before the next
	 * BeginFrame() may pre-populate it (see class comment).
	 */
	TUniquePtr<FVaCuusCommandBuffer> EndFrameAndPublish();

	/**
	 * Resolves the ImageWrapper module ONCE from the game thread and caches it for
	 * LoadTexture and the decode workers. Call from module startup.
	 *
	 * WHY THIS EXISTS: FModuleManager::GetOrLoadModule() refuses to load anything
	 * off the game thread (ModuleManager.cpp:940-944 returns nullptr with
	 * NotLoadedByGameThread), and LoadModuleChecked then checkf()s on the null
	 * (ModuleManager.cpp:973). LoadTexture runs on the UI thread, so resolving the
	 * module there was a latent hard crash of exactly the kind we already hit with
	 * LoadModuleChecked inside FVaCuusUIThread::Exit(). Closes VaCuus-akj.6.12.
	 */
	static IImageWrapperModule* CacheImageWrapperModule();

	/**
	 * Blocks until every decode launched so far has finished; their payloads are
	 * then queued for the next BeginFrame() drain. Returns false on timeout.
	 *
	 * TESTS ONLY. The UI thread must never call this: waiting on a decode is the
	 * hitch the async path exists to remove.
	 */
	bool WaitForTextureDecodes(FTimespan Timeout);

private:
	/** Lazily creates the pending buffer; see class comment for out-of-frame semantics. */
	FVaCuusCommandBuffer& GetPending();

	/** Ensures the caller is the frame-owning thread while a frame is open. */
	void CheckOwnerThread() const;

	/**
	 * Moves finished decodes into the pending buffer's NewTextures and forgets
	 * their handles. Frame-owning thread only.
	 */
	void DrainCompletedDecodes();

	/** Cached ImageWrapper module, or nullptr if it could not be resolved; see CacheImageWrapperModule. */
	static IImageWrapperModule* GetImageWrapperModule();

	uint64 NextGeometryHandle = 1;
	uint64 NextTextureHandle = 1;
	uint64 Generation = 0;
	bool bInFrame = false;

	/** Thread that called BeginFrame(); only meaningful while bInFrame. */
	uint32 OwnerThreadId = 0;

	TUniquePtr<FVaCuusCommandBuffer> Pending;

	/** Shared with every launched decode; see FVaCuusTextureDecodeSink. */
	TSharedRef<FVaCuusTextureDecodeSink> DecodeSink = MakeShared<FVaCuusTextureDecodeSink>();

	/**
	 * Handles whose decode is still outstanding. RELEASE-BEFORE-ARRIVAL GUARD:
	 * ReleaseTexture() erases the handle here, so the drain drops a payload that
	 * arrives for an already-retired handle instead of re-creating an RHI texture
	 * whose only release has already been consumed by the replayer.
	 */
	TSet<FVaCuusTextureHandle> InFlightTextures;

	/** Launched decode tasks, pruned as they complete; only WaitForTextureDecodes reads them. */
	TArray<UE::Tasks::FTask> DecodeTasks;
};
