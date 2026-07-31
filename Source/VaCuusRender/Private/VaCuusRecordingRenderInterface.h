// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusCommandBuffer.h"

#include "Containers/MpscQueue.h"
#include "Tasks/Task.h"

#include <RmlUi/Core/RenderInterface.h>

#include <atomic>

class IImageWrapperModule;

/**
 * Whether a decode produced a payload, and if not, why. THE outcome field: the drain
 * branches on this alone (it used to infer failure from a zero Data.Size and read this
 * only to pick a message, which made one fact live in two places).
 *
 * The reason codes exist because "the decoder disagreed with itself" sends a reader
 * somewhere very different from "this asset will not decode", and both used to log the
 * same line.
 */
enum class EVaCuusTextureDecodeFailure : uint8
{
	/** Decoded fine; Data carries the payload. */
	None,

	/** The wrapper refused bytes the synchronous probe had already accepted. */
	Decode,

	/** The wrapper decoded a different size (or byte count) than the probe reported. */
	SizeMismatch
};

/** One finished async image decode on its way back to the recorder. */
struct FVaCuusTextureDecode
{
	FVaCuusTextureHandle Handle = 0;

	/** Premultiplied RGBA8, ready for NewTextures. Only valid when Failure is None. */
	FVaCuusTextureData Data;

	/** Whether Data holds a payload, and otherwise which log line the drain emits. */
	EVaCuusTextureDecodeFailure Failure = EVaCuusTextureDecodeFailure::None;

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
	 * (Containers/MpscQueue.h:29-44) so an abandoned sink leaks nothing.
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
 *
 * EVERY frame is recorded; not every frame is published. EndFrameAndPublish()
 * withholds a frame that draws what the render thread already has (the M2 Task 12
 * idle short-circuit) and returns null for it.
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

	// The M5 glass seven (spec §2(d)): the vocabulary `backdrop-filter` arrives in
	// (ElementEffects.cpp:247-282). Filters are resources (legal out of frame — document
	// teardown releases them); layer and clip-mask calls are draw state (in-frame only).
	virtual Rml::CompiledFilterHandle CompileFilter(const Rml::String& Name, const Rml::Dictionary& Parameters) override;
	virtual void ReleaseFilter(Rml::CompiledFilterHandle Filter) override;
	virtual Rml::LayerHandle PushLayer() override;
	virtual void CompositeLayers(Rml::LayerHandle Source, Rml::LayerHandle Destination, Rml::BlendMode BlendMode,
		Rml::Span<const Rml::CompiledFilterHandle> Filters) override;
	virtual void PopLayer() override;
	virtual void EnableClipMask(bool bEnable) override;
	virtual void RenderToClipMask(Rml::ClipMaskOperation Operation, Rml::CompiledGeometryHandle Geometry, Rml::Vector2f Translation) override;
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
	 *
	 * RETURNS NULL when the frame just recorded is the frame already on screen --
	 * the idle short-circuit (see the cpp for the full argument), unless
	 * `vacuus.IdleGate 0` has turned it off. A null return is a normal outcome, not a
	 * failure: the caller simply enqueues nothing, and the render thread keeps
	 * compositing the render target it already has.
	 *
	 * The RECORDING is never skipped, only the publish. That is load-bearing: the
	 * async image decodes are drained inside BeginFrame(), so a recorder that
	 * stopped running frames would leave a loaded image on its transparent
	 * placeholder forever.
	 */
	TUniquePtr<FVaCuusCommandBuffer> EndFrameAndPublish();

	/**
	 * Buffers this recorder has actually handed out, i.e. the newest Generation.
	 * Frames the idle gate withheld are NOT counted -- that is what keeps
	 * Generation meaning "publishes", which the replayer's idempotence guard
	 * relies on (FVaCuusReplayRenderer::ShouldConsume).
	 *
	 * THREAD AFFINITY, and it is narrower than it looks: both counters below are plain
	 * uint64 written only by EndFrameAndPublish(), so they may be read only by the thread
	 * that drives this recorder -- the VaCuus UI thread in production, the test thread in
	 * a unit test. Deliberately NOT atomic, because nothing needs them to be: the
	 * GAME-THREAD readout of the same two facts is FVaCuusViewStatus::FramesPublished
	 * against FramesRecorded, which is atomic and per view, and that is where a
	 * cross-thread reader belongs. CheckOwnerThread() is not called here on purpose --
	 * it only bites while a frame is open, and these are read between frames, so it would
	 * be an assertion that never fires pretending to be a guarantee.
	 */
	uint64 GetNumFramesPublished() const { return Generation; }

	/**
	 * Frames recorded whose publish the idle gate withheld.
	 *
	 * The observable that makes the gate TESTABLE: "an unchanged frame is not published" is
	 * otherwise invisible from outside -- the screen looks identical either way -- and an
	 * invariant with no observable cannot be tested. Same thread affinity as above; the
	 * runtime diagnostics read the view-status counters and the vacuus.M1HUD.PerfLog
	 * window instead.
	 */
	uint64 GetNumFramesSkipped() const { return NumFramesSkipped; }

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

#if WITH_DEV_AUTOMATION_TESTS
	/**
	 * Blocks until every decode launched so far has finished; their payloads are
	 * then queued for the next BeginFrame() drain. Returns false on timeout.
	 *
	 * TESTS ONLY, and compiled out of shipping builds along with the DecodeTasks
	 * array it waits on: production never needs an FTask handle (the scheduler holds
	 * its own reference), so keeping the array, the per-LoadTexture Add and the
	 * per-frame prune in a shipping build would be pure test scaffolding. The UI
	 * thread must never call this anyway — waiting on a decode is the hitch the
	 * async path exists to remove.
	 */
	bool WaitForTextureDecodes(FTimespan Timeout);
#endif

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
	uint64 NextFilterHandle = 1;

	/**
	 * PER FRAME, unlike every counter above: reset to 1 by BeginFrame(). Layer handles
	 * are frame-scoped in RmlUi (the render stack is asserted empty at every frame start,
	 * RmlUi Source/Core/RenderManager.cpp:51, and PopLayer is the only end of a layer's
	 * life — no release call exists, RenderInterface.h:106-107), so restarting keeps two
	 * identical glass frames byte-identical and therefore hash-equal. A cross-frame
	 * counter here would defeat the idle gate for every glass document; see
	 * FVaCuusLayerHandle. 0 stays reserved for the base layer (RenderInterface.h:96).
	 */
	uint64 NextLayerHandle = 1;

	uint64 Generation = 0;
	bool bInFrame = false;

	/**
	 * Filter types already refused with a log line, so the refusal is loud ONCE PER TYPE
	 * per recorder rather than once per element per recompile (a hover restyle re-runs
	 * CompileFilter for every filter on the element). RmlUi's own per-element warning
	 * ("Could not compile filter on element", ElementEffects.cpp:164-165) still fires
	 * each time and carries the element address; this latch carries the why.
	 */
	TSet<FString> RefusedFilterTypes;

	/**
	 * VaCuusHashFrameContent() of the last buffer this recorder PUBLISHED, and the
	 * only state the idle gate keeps.
	 *
	 * Per recorder, therefore per view: there is one recorder per Rml::Context
	 * (FVaCuusRmlDocumentHost::Initialize), so one view going idle cannot suppress
	 * another view's publish, and a view that is torn down and recreated gets a new
	 * recorder and hence no hash to inherit. Only meaningful once Generation > 0,
	 * which is also how the first frame of a view is forced to publish.
	 */
	uint64 LastPublishedContentHash = 0;

	/** Frames the idle gate withheld; see GetNumFramesSkipped(). */
	uint64 NumFramesSkipped = 0;

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

#if WITH_DEV_AUTOMATION_TESTS
	/** Launched decode tasks, pruned as they complete; only WaitForTextureDecodes reads them. */
	TArray<UE::Tasks::FTask> DecodeTasks;
#endif
};
