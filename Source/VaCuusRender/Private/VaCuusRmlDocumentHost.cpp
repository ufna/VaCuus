// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusRmlDocumentHost.h"

#include "VaCuusDefines.h"
#include "VaCuusInteractiveSnapshot.h"
#include "VaCuusRecordingRenderInterface.h"
#include "VaCuusSlateElement.h"
#include "VaCuusStats.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "CoreGlobals.h"
#include "HAL/PlatformTLS.h"
#include "HAL/PlatformTime.h"
#include "RenderingThread.h"

#include <RmlUi/Core.h>

namespace VaCuusRmlDocumentHost
{
/** Virtual source name for documents loaded from memory (used in RmlUi log messages). */
static const char* GMemorySourceName = "vacuus://memory.rml";

/**
 * How often the snapshot cost is logged (Verbose): the first frame, then every
 * ~10 s at 60 Hz. The DFS is cheap enough that measuring it every frame would
 * cost more than the walk, and one line per frame would be unreadable anyway.
 */
static constexpr uint64 GSnapshotLogInterval = 600;
}	 // namespace VaCuusRmlDocumentHost

FVaCuusRmlDocumentHost::FVaCuusRmlDocumentHost(const TSharedRef<FVaCuusSlateElement>& InElement)
	: Element(InElement)
{
	// Runs on the owner's thread (the UI thread may not even exist yet). Nothing
	// RmlUi-affine may happen here -- that is Initialize()'s job.
}

FVaCuusRmlDocumentHost::~FVaCuusRmlDocumentHost()
{
	// Normal path: the UI thread already ran Shutdown(), so the context is gone and
	// only the retained recorder is left (which is why Recorder is not part of the
	// test below). A live context means teardown was skipped, and it can only be
	// finished on the thread that built it.
	if (Context != nullptr)
	{
		check(FVaCuusUIThread::IsInUIThread());
		Shutdown();
	}
}

bool FVaCuusRmlDocumentHost::Initialize(uint32 InViewId, const TSharedRef<FVaCuusViewStatus>& InStatus)
{
	check(FVaCuusUIThread::IsInUIThread());

	ViewId = InViewId;
	Status = InStatus;

	// Per-view render interface: RmlUi gives each distinct interface its own
	// RenderManager, which is what keeps this view's geometry, textures and command
	// buffer separate from every other view's.
	Recorder = MakeUnique<FVaCuusRecordingRenderInterface>();

	// The real size arrives with the first command (every command carries one) and
	// is applied before the first Update(), so nothing is ever laid out at 1x1.
	ContextName = FString::Printf(TEXT("VaCuusView%u"), ViewId);
	Context = Rml::CreateContext(
		Rml::String(TCHAR_TO_UTF8(*ContextName)), Rml::Vector2i(1, 1), Recorder.Get());
	if (!Context)
	{
		UE_LOG(LogVaCuus, Error, TEXT("View %u failed to create its Rml context"), ViewId);
		Recorder.Reset();
		return false;
	}

	UE_LOG(LogVaCuus, Log, TEXT("View %u booted on the UI thread (id %u; game thread is %u)"),
		ViewId, FPlatformTLS::GetCurrentThreadId(), GGameThreadId);
	return true;
}

void FVaCuusRmlDocumentHost::Shutdown()
{
	check(FVaCuusUIThread::IsInUIThread());

	if (Document)
	{
		// Queues the document unload; RmlUi processes it during RemoveContext (the
		// context destructor owns `unloaded_documents`, so nothing is left behind).
		Document->Close();
		Document = nullptr;

		// NO CLEARING FRAME HERE, unlike CloseDocument(), and the difference is that this
		// path takes the render target away rather than needing something drawn into it:
		// the ENQUEUE_RENDER_COMMAND below releases it (ReleaseResources_RenderThread), and
		// FVaCuusSlateElement::Draw_RenderThread draws nothing without one. Recording a
		// frame here would be impossible anyway -- RemoveContext runs immediately after.
		//
		// Same retraction as CloseDocument(): nothing is interactive any more, and no
		// future frame will say so on our behalf.
		PublishEmptyInteractiveSnapshot();
	}

	if (Context)
	{
		// Destroys this view's element tree, which releases its geometry and
		// textures back through Recorder (still alive, and staying alive -- see the
		// header: RmlUi keeps a RenderManager keyed on it until Rml::Shutdown()).
		Rml::RemoveContext(Rml::String(TCHAR_TO_UTF8(*ContextName)));
		Context = nullptr;
	}

	if (Element.IsValid())
	{
		// Render-side teardown from THIS thread, so it is ordered after our own last
		// publish (same-thread enqueues keep their order); the element ref rides
		// along in the lambda and dies with it, after the release has run.
		ENQUEUE_RENDER_COMMAND(VaCuusReleaseView)(
			[LocalElement = MoveTemp(Element)](FRHICommandListImmediate&)
			{
				LocalElement->ReleaseResources_RenderThread();
			});
		Element.Reset();
	}
}

void FVaCuusRmlDocumentHost::SetViewSize(FIntPoint InViewSize)
{
	check(FVaCuusUIThread::IsInUIThread());

	if (InViewSize == ViewSize || InViewSize.X <= 0 || InViewSize.Y <= 0)
	{
		return;
	}

	ViewSize = InViewSize;
	if (Context)
	{
		Context->SetDimensions(Rml::Vector2i(ViewSize.X, ViewSize.Y));
	}

	// The resize proof: this line only ever prints on the UI thread, in response to
	// a queued Resize command, and means the context has been re-laid out.
	UE_LOG(LogVaCuus, Log, TEXT("View %u size now %dx%d (UI thread %u)"),
		ViewId, ViewSize.X, ViewSize.Y, FPlatformTLS::GetCurrentThreadId());
}

void FVaCuusRmlDocumentHost::LoadDocumentFromFile(const FString& VfsPath, uint64 LoadSerial)
{
	check(FVaCuusUIThread::IsInUIThread());
	if (!Context)
	{
		ReportLoadResult(LoadSerial, /*bSuccess=*/false);
		return;
	}

	// Goes through Rml::GetFileInterface() (FVaCuusFileInterface): relative paths --
	// including the document's own <link>/<img> references -- resolve against
	// the ordered DevUI roots (plugin's Content/DevUI first -- D19).
	AdoptDocument(Context->LoadDocument(Rml::String(TCHAR_TO_UTF8(*VfsPath))),
		FString::Printf(TEXT("VFS ('%s')"), *VfsPath), LoadSerial);
}

void FVaCuusRmlDocumentHost::LoadDocumentFromMemory(const FString& RmlSource, uint64 LoadSerial)
{
	check(FVaCuusUIThread::IsInUIThread());
	if (!Context)
	{
		ReportLoadResult(LoadSerial, /*bSuccess=*/false);
		return;
	}

	AdoptDocument(
		Context->LoadDocumentFromMemory(
			Rml::String(TCHAR_TO_UTF8(*RmlSource)), VaCuusRmlDocumentHost::GMemorySourceName),
		TEXT("inline"), LoadSerial);
}

void FVaCuusRmlDocumentHost::AdoptDocument(Rml::ElementDocument* NewDocument, const FString& Description, uint64 LoadSerial)
{
	check(FVaCuusUIThread::IsInUIThread());

	if (!NewDocument)
	{
		// The previous document (if any) stays up: a failed load must not blank a
		// working view. The game thread hears about it through the status and can
		// decide on a fallback (vacuus.M1HUD does).
		UE_LOG(LogVaCuus, Error, TEXT("View %u failed to load the %s document"), ViewId, *Description);
		ReportLoadResult(LoadSerial, /*bSuccess=*/false);
		return;
	}

	CloseDocument();
	Document = NewDocument;

	// FocusFlag::Document, spelled out rather than left to the default, because
	// keyboard and pad navigation are DEAD without it. Tab, the arrow keys and
	// Return/Space all live in ElementDocument::ProcessDefaultAction, which only runs
	// when the keydown was dispatched to something inside a document -- and
	// Context::ProcessKeyDown falls back to the context ROOT when nothing holds focus
	// (Context.cpp:533-537). The root is not an ElementDocument and has no default
	// action, so every arrow key would be silently swallowed.
	//
	// NOT FocusFlag::Auto (the library default), and that is a policy choice: Auto
	// additionally focuses the first element carrying `autofocus`
	// (ElementDocument.cpp:371-388), which would make a freshly loaded HUD take the
	// keyboard away from the game the moment it appears -- exactly what controller
	// decision D9 exists to prevent. Focus lands on the document itself, which
	// bWantsKeyboardFocus deliberately does not count, so navigation works while the
	// game keeps its keys until the player actually moves the focus. Honouring
	// `autofocus` for a modal dialog is a per-view opt-in for a later milestone.
	Document->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document);

	UE_LOG(LogVaCuus, Log, TEXT("View %u loaded the %s document (%dx%d)"),
		ViewId, *Description, ViewSize.X, ViewSize.Y);
	ReportLoadResult(LoadSerial, /*bSuccess=*/true);
}

void FVaCuusRmlDocumentHost::ReportLoadResult(uint64 LoadSerial, bool bSuccess)
{
	if (!Status.IsValid() || LoadSerial == 0)
	{
		return;
	}

	// Result first, serial second (release): a game-thread reader that sees the new
	// serial is guaranteed to see the matching result.
	Status->LoadResult.store(
		static_cast<uint8>(bSuccess ? EVaCuusLoadResult::Succeeded : EVaCuusLoadResult::Failed),
		std::memory_order_relaxed);
	Status->LoadCompletedSerial.store(LoadSerial, std::memory_order_release);
}

void FVaCuusRmlDocumentHost::CloseDocument()
{
	check(FVaCuusUIThread::IsInUIThread());

	if (Document)
	{
		// QUEUES the unload -- it does not perform it. Close() only calls
		// Context::UnloadDocument (ElementDocument.cpp:421-425), which moves the document
		// into the context's `unloaded_documents` list; the elements, their compiled
		// geometry and their textures are not freed until the context's next Update()
		// reaches ReleaseUnloadedDocuments (Context.cpp:216-217). So a close with no
		// further Update() leaks the whole document tree on both sides until
		// Rml::RemoveContext.
		Document->Close();
		Document = nullptr;

		// THE ONE FRAME THIS VIEW STILL OWES, and the reason HasView() below has a second
		// clause. It is not an optimisation; without it the closed document stays on screen
		// forever.
		//
		// On an idle UI the render target is the ONLY copy of the pixels -- the idle gate
		// withholds every frame that draws what the render thread already has, so the
		// recorder will never resend them -- and FVaCuusSlateElement::Draw_RenderThread
		// composites that RT unconditionally, outside its `PendingBuffers.Num() > 0` branch
		// (VaCuusSlateElement.cpp:56-96). Any path that stops recording must therefore emit
		// one CLEARING frame first: an empty command list hashes differently from the last
		// published one, so it publishes, and the replayer opens its pass with
		// ERenderTargetActions::Clear_Store (VaCuusReplayRenderer.cpp:233) -- that clear with
		// no draws behind it is what wipes the view.
		//
		// Before this flag existed, HasView() went false the moment Document did, the UI
		// thread's record loop skipped this view for good (VaCuusUIThread.cpp:812-818), and
		// the player was left with a pixel-perfect ghost of a dead document: clicks fall
		// through (the empty snapshot below), the cursor reverts, focus is released, and
		// nothing dismisses it short of loading another document, hiding the widget or
		// destroying the view. SetVisible(false) had this right all along -- see the comment
		// there, which describes this exact failure and rules it out for the hide path.
		//
		// The frame it buys is also the Update() that drains `unloaded_documents`, so it
		// discharges both debts at once.
		bOwesClearingFrame = true;

		// The snapshot retraction, separately and immediately: hit coverage must stop
		// NOW, not one frame later, or the game thread keeps answering Handled from the
		// closed document's geometry for a frame. It is also the only retraction that
		// survives the one path where the clearing frame cannot run -- an in-band Shutdown
		// closes every document and then leaves the loop (VaCuusUIThread.cpp:836-859) --
		// and there the render side is retracted by Shutdown()'s
		// ReleaseResources_RenderThread instead, which drops the RT outright.
		PublishEmptyInteractiveSnapshot();
	}
}

void FVaCuusRmlDocumentHost::SetVisible(bool bVisible)
{
	check(FVaCuusUIThread::IsInUIThread());

	if (!Document)
	{
		return;
	}

	// HIDE THE DOCUMENT rather than stop recording the view. Hide still works, but not by
	// the mechanism this comment used to describe, and the correction matters because the old
	// wording sends a reader looking for a bug that is not there.
	//
	// WHAT HAPPENS: Hide() sets `visibility: hidden` on the document
	// (ThirdParty/RmlUi/Source/Core/ElementDocument.cpp:406-419), so the next recorded frame's
	// Render() emits nothing at all. An empty command list hashes differently from the last
	// published frame's, so that ONE frame publishes; the replayer opens its render pass with
	// ERenderTargetActions::Clear_Store (VaCuusReplayRenderer.cpp:233), and that clear with no
	// draws behind it is what wipes this view's render target. Every hidden frame after it
	// records the same empty list, hashes equal, and is WITHHELD by the idle gate.
	//
	// So: one empty publish, not one per frame. "The view keeps publishing frames, they are
	// simply empty" has been false since M2 Task 12, and the warning that used to follow it --
	// "skipping the frame would leave the last published content in this view's render target"
	// -- describes exactly what the gate now does from the second hidden frame onward. That is
	// safe for the reason the old comment had no way to state: the RT was already cleared by
	// the frame before, so withholding a frame that would clear it again changes nothing.
	//
	// The alternative that warning was really aimed at is still ruled out. Skipping the frame
	// ENTIRELY -- never recording, never publishing -- would leave the last VISIBLE content in
	// the RT, because nothing would ever emit the empty frame that clears it. A hidden view
	// therefore still pays a full Update() and a full record every frame; only the publish
	// stops. It also stops claiming hit coverage without any extra work here, because the
	// snapshot walk skips invisible elements (VaCuusInteractiveSnapshot.cpp:157-160).
	if (bVisible)
	{
		// FocusFlag::Keep, not Document: Hide() ran UnfocusDocument() but left this
		// document's own focus chain intact (ElementDocument.cpp:415-418), so Keep
		// re-focuses the leaf the player was on (ElementDocument.cpp:392-395, the
		// focus_previous branch calling GetFocusLeafNode) and a hide/show round trip does
		// not throw away where they were. It degrades to focusing the document when there
		// was no leaf, which is the Document case.
		Document->Show(Rml::ModalFlag::None, Rml::FocusFlag::Keep);
	}
	else
	{
		Document->Hide();
	}

	UE_LOG(LogVaCuus, Verbose, TEXT("View %u is now %s"), ViewId, bVisible ? TEXT("visible") : TEXT("hidden"));
}

bool FVaCuusRmlDocumentHost::HasView() const
{
	check(FVaCuusUIThread::IsInUIThread());

	// bOwesClearingFrame is what keeps a just-closed view recordable for exactly one more
	// frame. See CloseDocument() for why that frame is mandatory; the short version is that
	// the render target is the only copy of an idle UI's pixels, so a view that stops being
	// recorded stops being erasable.
	//
	// Context and ViewSize are still hard requirements: with no context there is nothing to
	// Update() or Render(), and at a degenerate size the replayer skips its draw pass
	// entirely (VaCuusReplayRenderer.cpp:53-74) so the clearing frame would clear nothing.
	// Neither case can leave a ghost anyway -- a view that never had a size never published
	// pixels, and a view whose context is gone has been through Shutdown(), which released
	// the render target itself.
	return Context != nullptr && (Document != nullptr || bOwesClearingFrame) &&
		ViewSize.X > 0 && ViewSize.Y > 0;
}

Rml::Context* FVaCuusRmlDocumentHost::GetContext() const
{
	// Handed to the UI thread's input drain, which owns every FKey/modifier/button
	// translation (see IVaCuusDocumentHost::GetContext for why the seam is here).
	// Note it stays valid while there is no DOCUMENT: input into an empty context is
	// harmless, and refusing it would mean losing the mouse-leave that clears hover.
	check(FVaCuusUIThread::IsInUIThread());
	return Context;
}

void FVaCuusRmlDocumentHost::RecordAndPublishFrame()
{
	check(FVaCuusUIThread::IsInUIThread());
	check(HasView());

	if (!bLoggedFirstFrame)
	{
		// Standing evidence for the M2 threading contract: if RmlUi ever crept back
		// onto the game thread, this line (and the checks above) would say so.
		bLoggedFirstFrame = true;
		UE_LOG(LogVaCuus, Log,
			TEXT("View %u recorded its first UI frame on thread %u (game thread is %u; IsInGameThread=%s)"),
			ViewId, FPlatformTLS::GetCurrentThreadId(), GGameThreadId,
			IsInGameThread() ? TEXT("true") : TEXT("false"));
	}

	// ONE SHOT, CLEARED HERE RATHER THAN ON THE CLOSE PATH, because ANY frame discharges the
	// debt and only this function knows one ran. Two cases, both correct by the same rule:
	//
	//  - nothing was loaded in between, so this frame records an empty context: the publish
	//    clears the render target, which is the debt;
	//  - a load landed first (AdoptDocument() calls CloseDocument() before adopting), so this
	//    frame records the NEW document and overwrites the render target with it. The old
	//    pixels are gone either way, and clearing first would only add a one-frame blank
	//    between two documents.
	bOwesClearingFrame = false;

	Recorder->BeginFrame(ViewSize);

	{
		VACUUS_PERF_SCOPE(Update);
		Context->Update();
	}

	// Between Update() and Render(), and deliberately so: Update() is what leaves
	// every element's absolute offset and box clean, which is the whole reason the
	// walk is a field read per element rather than a layout pass.
	//
	// UNCONDITIONAL, AND BEFORE THE GATE: this publishes on EVERY recorded frame, including
	// the ones whose command buffer is withheld. That is not an oversight and it must not be
	// "optimised" to match the publish, because the DOM can change hit geometry with NO pixel
	// change at all -- `pointer-events` toggled, `disabled` removed from a button whose
	// styling does not move, an invisible overlay activated. Every one of those produces a
	// byte-identical command list, so the gate withholds the frame; gating the snapshot on
	// the same decision would freeze hit-testing on exactly the changes that only affect
	// hit-testing, and clicks would land in the wrong place with nothing to show why.
	//
	// It costs a DFS over the visible tree per frame either way, which is what the Verbose
	// timing line in PublishInteractiveSnapshot() is there to keep honest.
	//
	// PRE-EXISTING SKEW, unchanged by Task 12 and noted so nobody blames it: on a frame that
	// DOES publish, the snapshot reaches the game thread through a triple buffer while the
	// command buffer travels via ENQUEUE_RENDER_COMMAND, so the game thread can hit-test the
	// new geometry a frame before the matching pixels land. The gate neither creates nor
	// widens that; it is the cost of the two channels being independent.
	PublishInteractiveSnapshot();

	// The hash the idle gate compares is computed inside EndFrameAndPublish(), so its
	// cost is deliberately inside the Record scope: it is part of what recording a
	// frame costs now, and hiding it outside the scope it pays for would make the
	// perf log lie about the trade.
	TUniquePtr<FVaCuusCommandBuffer> Buffer;
	{
		VACUUS_PERF_SCOPE(Record);
		Context->Render();
		Buffer = Recorder->EndFrameAndPublish();
	}

	// Null == the idle gate withheld this frame because it draws what the render
	// thread already has (FVaCuusRecordingRenderInterface::EndFrameAndPublish). The
	// right response is to enqueue NOTHING: the element re-composites its persistent
	// render target every frame regardless of whether a buffer arrived
	// (VaCuusSlateElement.cpp:91-140), which is the whole idle model.
	const bool bPublished = Buffer.IsValid();
	if (bPublished)
	{
		// Straight from the UI thread to the render thread: FRenderThreadCommandPipe
		// has no game-thread requirement, and the element is a thread-safe shared ptr
		// captured by value, so it outlives the enqueue no matter what the game thread
		// is doing with its own reference.
		ENQUEUE_RENDER_COMMAND(VaCuusPublishUIFrame)(
			[LocalElement = Element, Buf = MoveTemp(Buffer)](FRHICommandListImmediate& RHICmdList) mutable
			{
				LocalElement->SetPendingBuffer_RenderThread(RHICmdList, MoveTemp(Buf));
			});
	}

	// Both outcomes are counted, so the perf log can show how much of the window was
	// idle next to the Replay samples that stopped happening.
	FVaCuusPerfLog::AddUIFrame(bPublished);

	// THE SECOND-CHEAPEST DIAGNOSTIC for a gate whose failure mode is a silently frozen UI,
	// after the vacuus.IdleGate kill switch itself. Per TRANSITION, never per frame: a static
	// HUD records ~13,000 frames per minute and publishes one, so a per-frame line would bury
	// the log and a per-frame Verbose check would still cost a string format. Two lines per
	// idle period is enough to answer "is this view wedged or just idle" from
	// -LogCmds="LogVaCuus Verbose" instead of from a rebuild.
	//
	// The view id is in the line because the process-wide perf log cannot tell two views
	// apart, and the counters are there because "went idle and never came back" versus "went
	// idle at frame 12 of 13,000" are different bugs.
	const bool bNowIdle = !bPublished;
	if (bNowIdle != bIdle)
	{
		bIdle = bNowIdle;
		UE_LOG(LogVaCuus, Verbose, TEXT("View %u UI frames went %s (published=%llu withheld=%llu)"),
			ViewId, bIdle ? TEXT("IDLE, publishes withheld from here") : TEXT("ACTIVE, publishing again"),
			Recorder->GetNumFramesPublished(), Recorder->GetNumFramesSkipped());
	}

	if (Status.IsValid())
	{
		// Two counters, and the game thread needs both: RECORDED is what a headless wait
		// thresholds on (see FVaCuusViewStatus), PUBLISHED is the only per-view readout of
		// whether the gate is firing. Their difference is this view's idle signal.
		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
		if (bPublished)
		{
			Status->FramesPublished.fetch_add(1, std::memory_order_release);
		}
	}
}

void FVaCuusRmlDocumentHost::PublishInteractiveSnapshot()
{
	check(FVaCuusUIThread::IsInUIThread());

	if (!Status.IsValid() || Context == nullptr)
	{
		return;
	}

	// Built straight into the triple buffer's write slot: the three buffers keep
	// their rect-array capacity between publishes, so a steady-state UI frame does
	// not allocate.
	FVaCuusInteractiveSnapshot& Snapshot = Status->GetSnapshotWriteBuffer();

	const double StartSeconds = FPlatformTime::Seconds();
	const FVaCuusSnapshotBuildStats Stats =
		BuildVaCuusInteractiveSnapshot(*Context, ViewSize, ++SnapshotGeneration, Snapshot);
	const double ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;

	// The cursor RmlUi asked for during the Update() that just ran. Sampled HERE, one
	// call after our own Update() and before any other view's, because the latch is
	// process-wide while `cursor` is per context -- a serial that moved in this window
	// belongs to this view. Set after the build, which resets the whole snapshot.
	uint64 CursorSerial = 0;
	const EMouseCursor::Type PushedCursor = GetVaCuusLatchedMouseCursor(CursorSerial);
	if (CursorSerial != LatchedCursorSerial)
	{
		LatchedCursorSerial = CursorSerial;
		LatchedCursor = PushedCursor;
	}
	Snapshot.Cursor = LatchedCursor;

	// Copied out BEFORE the publish: the swap hands this buffer to the game thread,
	// and reading it afterwards means reading a buffer somebody else now owns.
	const int32 NumRects = Snapshot.InteractiveRects.Num();
	const bool bWantsKeyboardFocus = Snapshot.bWantsKeyboardFocus;

	Status->PublishSnapshot();

	// First frame, then every ~10 s at 60 Hz. Verbose because on a healthy view this
	// says nothing new; it is here so the DFS cost and the rect count are measurable
	// on a real document without attaching a profiler
	// (-LogCmds="LogVaCuus Verbose").
	if (++NumSnapshotsPublished % VaCuusRmlDocumentHost::GSnapshotLogInterval == 1)
	{
		UE_LOG(LogVaCuus, Verbose,
			TEXT("View %u snapshot %llu: %d interactive rect(s) from %d element(s) in %d document(s), %.4f ms (%dx%d, keyboard focus %s, cursor %d)"),
			ViewId, SnapshotGeneration, NumRects, Stats.NumElementsVisited,
			Stats.NumDocuments, ElapsedMs, ViewSize.X, ViewSize.Y,
			bWantsKeyboardFocus ? TEXT("yes") : TEXT("no"), int32(LatchedCursor));
	}
}

void FVaCuusRmlDocumentHost::PublishEmptyInteractiveSnapshot()
{
	check(FVaCuusUIThread::IsInUIThread());

	if (!Status.IsValid())
	{
		return;
	}

	// The generation still advances: an empty snapshot is a real publish that the
	// game thread must notice, not an absence of one.
	FVaCuusInteractiveSnapshot& Snapshot = Status->GetSnapshotWriteBuffer();
	Snapshot.Reset();
	Snapshot.Generation = ++SnapshotGeneration;
	Snapshot.ViewSize = ViewSize;
	Status->PublishSnapshot();
}
