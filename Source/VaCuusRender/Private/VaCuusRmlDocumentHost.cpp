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
		// Queues the document unload; RmlUi processes it during RemoveContext.
		Document->Close();
		Document = nullptr;

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
	// <Project>/Content/DevUI.
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
	Document->Show();

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
		// Queues the unload; RmlUi processes it in the context's next update.
		Document->Close();
		Document = nullptr;

		// HasView() is false from here, so no frame will be recorded and no snapshot
		// rebuilt -- the game thread would keep answering Handled from the closed
		// document's geometry forever. This is the retraction.
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

	// Hide() rather than "stop recording": the view keeps publishing frames, they
	// are simply empty, which is what actually clears the composite. Skipping the
	// frame would leave the last published content in this view's render target.
	if (bVisible)
	{
		Document->Show();
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
	return Context != nullptr && Document != nullptr && ViewSize.X > 0 && ViewSize.Y > 0;
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

	Recorder->BeginFrame(ViewSize);

	{
		VACUUS_PERF_SCOPE(Update);
		Context->Update();
	}

	// Between Update() and Render(), and deliberately so: Update() is what leaves
	// every element's absolute offset and box clean, which is the whole reason the
	// walk is a field read per element rather than a layout pass.
	PublishInteractiveSnapshot();

	TUniquePtr<FVaCuusCommandBuffer> Buffer;
	{
		VACUUS_PERF_SCOPE(Record);
		Context->Render();
		Buffer = Recorder->EndFrameAndPublish();
	}

	// Straight from the UI thread to the render thread: FRenderThreadCommandPipe
	// has no game-thread requirement, and the element is a thread-safe shared ptr
	// captured by value, so it outlives the enqueue no matter what the game thread
	// is doing with its own reference.
	ENQUEUE_RENDER_COMMAND(VaCuusPublishUIFrame)(
		[LocalElement = Element, Buf = MoveTemp(Buffer)](FRHICommandListImmediate& RHICmdList) mutable
		{
			LocalElement->SetPendingBuffer_RenderThread(RHICmdList, MoveTemp(Buf));
		});

	if (Status.IsValid())
	{
		// Per-view frame count: what a headless screenshot actually needs to wait on.
		Status->FramesPublished.fetch_add(1, std::memory_order_release);
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
			TEXT("View %u snapshot %llu: %d interactive rect(s) from %d element(s) in %d document(s), %.4f ms (%dx%d, keyboard focus %s)"),
			ViewId, SnapshotGeneration, NumRects, Stats.NumElementsVisited,
			Stats.NumDocuments, ElapsedMs, ViewSize.X, ViewSize.Y,
			bWantsKeyboardFocus ? TEXT("yes") : TEXT("no"));
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
