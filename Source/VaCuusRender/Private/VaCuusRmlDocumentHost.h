// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusDocumentHost.h"
#include "VaCuusUnsupportedTally.h" // FVaCuusUnsupportedTally, re-exported by GetUnsupportedTally()

#include "GenericPlatform/ICursor.h" // EMouseCursor (the latched-cursor members below)

class FVaCuusRecordingRenderInterface;
class IVaCuusFrameSink;

namespace Rml
{
class Context;
class ElementDocument;
}

/**
 * ONE VIEW'S allocated:logical texture relationship, sampled from its sink (VaCuus-cyn).
 *
 * THE TWO UNITS THE BUDGET LIVES BETWEEN. The recorder counts what it decoded -- width x
 * height x 4, on the UI thread, with no RHI in reach -- while vacuus.TextureBudgetMB is a
 * number a buyer read off vacuus.TextureStats, which since VaCuus-aam prints what the platform
 * reserves. On icon-sized art those differ by ~46% (VaCuus.Render.Texture.PlatformSize), so
 * comparing one against the other made the knob admit ~58 MiB when it was set to 40.
 *
 * TWO NAMED CONVERSIONS RATHER THAN A RATIO, because a bare scale factor is applied in the
 * wrong direction eventually and both directions are plausible-looking numbers. Neither can be
 * called with the arguments swapped.
 *
 * IT IS A WHOLE-VIEW RATIO, NOT A PER-TEXTURE SIZE, and that bound is real: converting the
 * BUDGET (which this does) is exact whenever the recorder's set and the replayer's set agree,
 * whatever the art; converting an individual texture's contribution would only be right for
 * uniform art. So the threshold moves into the right unit while CollectEvictableSources keeps
 * choosing and measuring in the recorder's own bytes, where its per-texture figures are exact.
 * Shape (b) on VaCuus-cyn -- per-handle allocated sizes carried back beside Bytes -- is what
 * would make the depth of a sweep exact for mixed documents too.
 *
 * UNKNOWN IS A STATE, not a 1:1 default in disguise: a NullRHI run has no allocator to ask and
 * a view that has published nothing yet has no textures to ask about. Both leave the figures
 * at zero, both convert as identity, and the caller says which unit it ended up in.
 */
struct FVaCuusTextureUnitScale
{
	/** This view's published census; 0/0 until it publishes, allocated 0 when no RHI could say. */
	uint64 LogicalBytes = 0;
	uint64 AllocatedBytes = 0;

	bool IsKnown() const { return LogicalBytes > 0 && AllocatedBytes > 0; }

	/** A budget in platform-reserved bytes, in the recorder's logical bytes. Identity when unknown. */
	uint64 AllocatedToLogical(uint64 Bytes) const { return Scale(Bytes, LogicalBytes, AllocatedBytes); }

	/** A recorder total in the platform-reserved bytes the cvar is written in. Identity when unknown. */
	uint64 LogicalToAllocated(uint64 Bytes) const { return Scale(Bytes, AllocatedBytes, LogicalBytes); }

private:
	/**
	 * DOUBLE, NOT A 64-BIT MulDiv, and the reason is overflow rather than taste: Bytes x
	 * Allocated is two texture-memory figures multiplied together, and a wrap there reads as a
	 * budget of a few bytes -- i.e. "evict everything" -- which is the one failure this must
	 * not have. Byte counts sit far inside double's exactly-representable integer range (2^53
	 * is 8 PiB), and the result feeds a MiB-granularity comparison, so nothing is lost.
	 */
	static uint64 Scale(uint64 Bytes, uint64 Numerator, uint64 Denominator)
	{
		return (Numerator > 0 && Denominator > 0) ? uint64(double(Bytes) * double(Numerator) / double(Denominator))
												  : Bytes;
	}
};

/**
 * The RmlUi-backed document host for ONE view: owns that view's recording render
 * interface, its Rml context and its current document (RmlUi headers stay in the
 * cpp).
 *
 * ONE CONTEXT, ONE RECORDER, ONE SINK: Rml::CreateContext() takes a per-context
 * render interface and RmlUi gives each distinct interface its own RenderManager,
 * so N hosts on the shared UI thread record N independent command buffers and
 * publish each to its own frame sink (the Slate element for a screen view, the
 * world sink for a quad -- IVaCuusFrameSink). That is what makes several views
 * (e.g. one per PIE client) work on a single UI thread.
 *
 * THREAD AFFINITY: everything except the constructor runs on the VaCuus UI
 * thread, asserted per method. The UI thread calls Initialize() when it drains the
 * AddView command and Shutdown() on RemoveView or in Exit(), so every RmlUi call
 * in the process happens on that one thread. The host does NOT boot or shut down
 * RmlUi itself -- the UI thread does that once for the process.
 *
 * RecordAndPublishFrame() publishes two things on two different cadences, neither with a
 * game-thread hop:
 *  - the interactive-region snapshot, ONCE PER RECORDED FRAME, to the game thread through
 *    the shared FVaCuusViewStatus (see FVaCuusInteractiveSnapshot);
 *  - the recorded command buffer, ONCE PER CHANGE, to the frame sink via
 *    ENQUEUE_RENDER_COMMAND. Since the M2 Task 12 idle short-circuit a frame that draws
 *    what the render thread already has is recorded and then withheld, so on a static
 *    document this is a handful of publishes and then nothing.
 *
 * The difference between the two cadences is deliberate, not an oversight; the call site
 * says why.
 */
class FVaCuusRmlDocumentHost final : public IVaCuusDocumentHost
{
public:
	/** Built on the owner's thread and handed to UVaCuusSubsystem::CreateView(). */
	explicit FVaCuusRmlDocumentHost(const TSharedRef<IVaCuusFrameSink>& InSink);

	/** Safety net only; normal teardown runs Shutdown() from the UI thread. */
	virtual ~FVaCuusRmlDocumentHost() override;

	FVaCuusRmlDocumentHost(const FVaCuusRmlDocumentHost&) = delete;
	FVaCuusRmlDocumentHost& operator=(const FVaCuusRmlDocumentHost&) = delete;

	//~ Begin IVaCuusDocumentHost
	virtual bool Initialize(uint32 InViewId, const TSharedRef<FVaCuusViewStatus>& InStatus) override;
	virtual void Shutdown() override;
	virtual void SetViewSize(FIntPoint InViewSize) override;
	virtual void LoadDocumentFromFile(const FString& VfsPath, uint64 LoadSerial) override;
	virtual void LoadDocumentFromMemory(const FString& RmlSource, uint64 LoadSerial) override;
	virtual void CloseDocument() override;
	virtual void SetVisible(bool bVisible) override;
	virtual bool HasView() const override;
	virtual void DrainAsyncArrivals() override;
	virtual bool ReleaseTextures(const FString& Source) override;

	/** VaCuus-dqs.3: evict undrawn file textures when this view is over vacuus.TextureBudgetMB.
	 *  Called at the TOP of the frame; see the definition for why it cannot be anywhere else. */
	void SweepTextureBudget();

	virtual Rml::Context* GetContext() const override;
	virtual void RecordAndPublishFrame() override;
	//~ End IVaCuusDocumentHost

	/**
	 * Finished image decodes this view's recorder has taken delivery of, whatever became of
	 * them afterwards (installed, dropped as already-released, or logged as a failed decode).
	 *
	 * THE ONLY OBSERVABLE FOR akj.6.27's WINDOW, and that is why it is here rather than only
	 * inside the recorder: while a view is unsized it records nothing and publishes nothing, so
	 * FVaCuusViewStatus's frame counters -- the game thread's usual readout -- both stay at
	 * zero whether or not the arrival was taken. Any thread; see the recorder's accessor.
	 */
	uint64 GetNumDecodeArrivals() const;

	/**
	 * This view's refusals of the two unsupported layer-capture virtuals, and the log lines they
	 * emitted. THE ONLY OBSERVABLE FOR THE LATCH (beads VaCuus-u0q, VaCuus-iuv): "a 200-element
	 * document logs once" is otherwise invisible from every angle — the automation framework does
	 * not capture Warnings, and the refused frame looks identical on screen either way. Any
	 * thread; see the recorder's accessor.
	 */
	FVaCuusUnsupportedTally GetUnsupportedTally() const;

private:
	/** Shared tail of both load paths: adopts and shows the new document, or logs the failure. */
	void AdoptDocument(Rml::ElementDocument* NewDocument, const FString& Description, uint64 LoadSerial);

	/** Publishes the outcome of a load back to the game thread (see FVaCuusViewStatus). */
	void ReportLoadResult(uint64 LoadSerial, bool bSuccess);

	/**
	 * Walks the context and publishes this frame's interactive regions to the game
	 * thread. Called after Context::Update() and before Context::Render().
	 */
	void PublishInteractiveSnapshot();

	/**
	 * Publishes an empty snapshot, so a view with no document stops claiming
	 * coverage. Without it the game thread would keep answering Handled from the
	 * geometry of a document that is already gone -- the snapshot is only refreshed
	 * by a recorded frame, and a document-less view records none.
	 */
	void PublishEmptyInteractiveSnapshot();

	/** Frame sink the published buffers are enqueued to (thread-safe SP). Dropped by Shutdown(). */
	TSharedPtr<IVaCuusFrameSink> Sink;

	/**
	 * This view's Rml::RenderInterface, handed to Rml::CreateContext().
	 *
	 * DELIBERATELY OUTLIVES Shutdown(): RmlUi keys a RenderManager on this pointer
	 * and only destroys it -- releasing this view's font textures through it -- in
	 * Rml::Shutdown(). The UI thread therefore keeps a retired host alive until the
	 * library is down (FVaCuusUIThread::RetiredHosts).
	 */
	TUniquePtr<FVaCuusRecordingRenderInterface> Recorder;

	/** Shared with UVaCuusView; how load results and frame counts reach the game thread. */
	TSharedPtr<FVaCuusViewStatus> Status;

	/** Owned by RmlUi; freed via Rml::RemoveContext in Shutdown(). */
	Rml::Context* Context = nullptr;

	/** Owned by the context; closed in CloseDocument()/Shutdown(). */
	Rml::ElementDocument* Document = nullptr;

	/** Layout/record size in pixels; pushed into the context on change. */
	FIntPoint ViewSize = FIntPoint::ZeroValue;

	/** Context name, derived from the view id: RmlUi keys its context registry by name. */
	FString ContextName;

	/** Routing/diagnostics only; the UI thread's map is the real registry. */
	uint32 ViewId = 0;

	/** One-shot thread-attribution log: the evidence that RmlUi left the game thread. */
	bool bLoggedFirstFrame = false;

	/**
	 * Set by CloseDocument(), consumed by the next RecordAndPublishFrame(): the one frame a
	 * closed view still owes, which HasView() stays true for.
	 *
	 * WHY A VIEW WITH NO DOCUMENT MUST STILL RECORD ONCE: since the M2 Task 12 idle gate the
	 * render target is the only copy of an idle UI's pixels, and the Slate element composites
	 * it whether or not a buffer arrived. A view that simply stopped recording would leave
	 * its last frame on screen for good. The clearing frame's Update() is also what drains
	 * RmlUi's `unloaded_documents`, i.e. what actually frees the closed document. See
	 * CloseDocument() for the whole argument.
	 */
	bool bOwesClearingFrame = false;

	/**
	 * Whether the last recorded frame's publish was withheld by the idle gate. Kept only to
	 * turn a per-frame condition into a per-TRANSITION log line; see RecordAndPublishFrame.
	 * Starts false because a view's first frame always publishes.
	 */
	bool bIdle = false;

	/**
	 * Strictly increasing snapshot id, stamped into every publish (including the
	 * empty ones). It is the only way the game thread can tell a fresh snapshot from
	 * the previous buffer handed back again, so it must never repeat or reset.
	 */
	uint64 SnapshotGeneration = 0;

	/**
	 * This view's cursor, and the global latch serial it was taken from.
	 *
	 * Held per host because the latch is process-wide (there is one RmlUi
	 * SystemInterface) while `cursor` is per context: sampling right after our own
	 * Update() and adopting only on a serial change is what attributes a push to the
	 * view that caused it. See GetVaCuusLatchedMouseCursor().
	 */
	EMouseCursor::Type LatchedCursor = EMouseCursor::Default;
	uint64 LatchedCursorSerial = 0;

	/** Published snapshots, for the throttled perf log below. */
	uint64 NumSnapshotsPublished = 0;
};
