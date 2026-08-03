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
 *
 * MODULE UNLOAD IS THE OTHER TEARDOWN (bead VaCuus-akj.6.26), and the sink answers only for
 * the recorder's. Two things were filed as able to dangle under a running decode; they are
 * not the same kind of thing at all, and only one of them needed a change:
 *
 *  (a) THE TASK'S CODE, which lives in libUnrealEditor-VaCuusRender.so. NOT REACHABLE in any
 *      supported configuration -- read rather than assumed.
 *      FModuleManager::UnloadModulesAtShutdown unloads every module with bIsShutdown = true
 *      (ModuleManager.cpp:1491-1492), and that flag is precisely what suppresses the free:
 *      "If we're shutting down then don't bother actually unloading the DLL. We'll simply
 *      abandon it in memory instead. This makes it much less likely that code will be
 *      unloaded that could still be called by another module" (ModuleManager.cpp:1369-1381)
 *      -- the OS reclaims it at process exit. The path that DOES call InternalFreeLibrary is
 *      an UnloadModule with bIsShutdown = false (the default, ModuleManager.h:306); for a
 *      plugin module that is FPluginManager::UnmountExplicitlyLoadedPlugin
 *      (PluginManager.cpp:3650), which refuses any plugin whose descriptor does not set
 *      bExplicitlyLoaded (:3660). VaCuus.uplugin does not. So there is no wait to write and
 *      nothing to fix; what would make this live again is marking the plugin explicitly
 *      loaded, and that is the line to re-read this paragraph from.
 *
 *  (b) THE CACHED IImageWrapperModule*, which was real and is now closed BY SHAPE. Unlike
 *      the library, the module OBJECT is destroyed by every unload, shutdown or not --
 *      "Release reference to module interface. This will actually destroy the module object"
 *      (ModuleManager.cpp:1344-1348) -- so a captured raw module pointer dangles even while
 *      its code stays mapped, and CreateImageWrapper is a virtual call through it. The task
 *      no longer holds one: LoadTexture creates the wrapper on the frame-owning thread and
 *      the worker receives the INSTANCE (see FVaCuusDecodeWork in the cpp, whose field-count
 *      guard is what keeps it that way). An instance is a plain MakeShared owned by its
 *      holder (ImageWrapperModule.cpp:79-158) and FImageWrapperModule::ShutdownModule is
 *      empty (:529), so module teardown has nothing to take away from it.
 *
 * STILL NO WAIT AT TEARDOWN, deliberately, and that is why the answer to (b) had to be a
 * shape: waiting on the task from the recorder's destructor would retract-and-execute the
 * decode on the UI thread (FTaskBase::WaitImpl -> TryRetractAndExecute,
 * TaskPrivate.cpp:246-251), which is the hitch the async path exists to remove.
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

	// The M5 shader trio (spec §2(e)): the vocabulary `decorator: linear/radial/conic-
	// gradient` and `decorator: shader(<builtin>)` arrive in (DecoratorGradient.cpp:252-259,
	// :422-428, :619-625; DecoratorShader.cpp:35). Shaders are resources (legal out of frame
	// — document teardown releases them via RenderManager::ReleaseResource,
	// RenderManager.cpp:364-368); RenderShader is draw state (in-frame only).
	virtual Rml::CompiledShaderHandle CompileShader(const Rml::String& Name, const Rml::Dictionary& Parameters) override;
	virtual void RenderShader(Rml::CompiledShaderHandle Shader, Rml::CompiledGeometryHandle Geometry, Rml::Vector2f Translation,
		Rml::TextureHandle Texture) override;
	virtual void ReleaseShader(Rml::CompiledShaderHandle Shader) override;
	//~ End Rml::RenderInterface

	/**
	 * Opens a frame: subsequent draw-state calls are recorded into the pending
	 * buffer. Also a drain point for finished async image decodes.
	 */
	void BeginFrame(FIntPoint ViewSize);

	/**
	 * Moves finished decodes into the pending buffer's NewTextures and forgets their
	 * handles. Idempotent and cheap when nothing is outstanding (one empty MPSC dequeue).
	 *
	 * PUBLIC BECAUSE A FRAME IS NOT THE ONLY OCCASION TO DRAIN (bead akj.6.27):
	 * BeginFrame() still calls it, but a recorder whose view is not currently RECORDABLE --
	 * unsized, or between a close and the next load -- never reaches BeginFrame at all, and
	 * its payloads would sit in the queue for the whole window. FVaCuusRmlDocumentHost::
	 * DrainAsyncArrivals() is the second caller, driven once per UI frame for every live host.
	 *
	 * FRAME-OWNING THREAD ONLY (the VaCuus UI thread in production, the test thread in a unit
	 * test): it writes the pending buffer and InFlightTextures, neither of which is
	 * synchronised. It is legal outside a Begin/End pair -- that is what the second caller
	 * does -- for the same reason the resource calls are (see the class comment); the
	 * arrivals simply ride along with the next published frame.
	 */
	void DrainCompletedDecodes();

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
	 * The RECORDING is never skipped, only the publish. That is load-bearing: an arrival
	 * drained into this frame is resource traffic, which is one of the gate's wake
	 * conditions, so the frame carrying a late payload always publishes. A recorder that
	 * stopped running FRAMES is a different matter and no longer strands anything -- the
	 * drain has a second, frame-independent caller (see DrainCompletedDecodes) -- but its
	 * payloads then wait in the pending buffer for the next frame that does run.
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
	 * Finished decodes this recorder has taken delivery of, whatever became of them
	 * afterwards -- installed, dropped because ReleaseTexture() got there first, or logged as
	 * a failed decode. It counts DELIVERIES, not successes, because the property it exists to
	 * make testable is "the queue was drained", not "the image decoded".
	 *
	 * The observable for bead akj.6.27, in the same spirit as GetNumFramesSkipped() being the
	 * observable for the idle gate -- and here the invariant is invisible from EVERY other
	 * angle, not merely from outside: during the window the bead is about, the view records no
	 * frame and publishes no buffer, so both frame counters above and both FVaCuusViewStatus
	 * counters stay at zero whether the payload was taken or left to rot in the queue.
	 *
	 * ATOMIC, UNLIKE THE TWO COUNTERS ABOVE, and for a reason that is theirs in reverse: they
	 * have a cross-thread mirror in FVaCuusViewStatus and so never need to be read off the
	 * frame-owning thread, while this one is read exactly when that mirror says nothing. One
	 * relaxed-family add per decoded IMAGE (not per frame) is not a cost worth arguing about.
	 */
	uint64 GetNumDecodeArrivals() const { return NumDecodeArrivals.load(std::memory_order_acquire); }

	/**
	 * Resolves the ImageWrapper module ONCE from the game thread and caches it for
	 * LoadTexture. Call from module startup. NOT for the decode workers, which is a
	 * lifetime rule rather than a convention -- see FVaCuusTextureDecodeSink (b).
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

	/** Cached ImageWrapper module, or nullptr if it could not be resolved; see CacheImageWrapperModule. */
	static IImageWrapperModule* GetImageWrapperModule();

	uint64 NextGeometryHandle = 1;
	uint64 NextTextureHandle = 1;
	uint64 NextFilterHandle = 1;
	uint64 NextShaderHandle = 1;

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
	 * Shader names/builtin keys already refused with a log line — the filter latch's
	 * discipline for the same reason (a hover restyle re-runs CompileShader per element).
	 * One set for both unknown builtin keys and unknown CompileShader names: either way
	 * the entry is "the string RmlUi sent that this recorder returned 0 for", and RmlUi's
	 * own per-element warning ("Could not generate decorator element data",
	 * ElementEffects.cpp:150-151) carries the element; this latch carries the why and the
	 * known keys.
	 */
	TSet<FString> RefusedShaderKeys;

	/**
	 * Latched once per recorder: a gradient whose stop list exceeded
	 * VaCuusMaxGradientStops was truncated to the reference-backend cap. The truncation
	 * itself is RmlUi's own backends' behavior (RmlUi_Renderer_GL3.cpp:1635) — the latch
	 * exists because they do it silently and we do not.
	 */
	bool bLoggedStopOverflow = false;

	/**
	 * THE RECORDER'S LIVE MATERIAL TABLE (M5 Task 5b) — handles of compiled
	 * Kind=Material shaders not yet released. Non-empty is this view's forced-republish
	 * flag: a live material decorator is GPU-evaluated state neither the content hash
	 * nor the traffic predicate can see (spec §2(f) — the composite only samples the
	 * RT), so while this set is non-empty the idle gate republishes, clamped to engine
	 * rate (see EndFrameAndPublish). PER RECORDER, therefore per view — one view's
	 * material HUD cannot force another view's publishes, which is what retires the
	 * spike's process-global gate term.
	 */
	TSet<FVaCuusShaderHandle> LiveMaterialShaders;

	/**
	 * GFrameCounter value at the last publish that a live material was re-evaluated by —
	 * the forced-republish CLAMP's memory. The UI thread can outrun the engine (a
	 * multi-view frame, a triggered catch-up), and publishing twice inside one engine
	 * frame buys a second replay no composite ever samples; the spike's own record
	 * says to clamp rather than adopt composite-time draws (spec §3.3's remedy pricing).
	 */
	uint64 LastMaterialRepublishFrame = 0;

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

	/** Finished decodes taken off the sink queue; see GetNumDecodeArrivals(). */
	std::atomic<uint64> NumDecodeArrivals{0};

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
