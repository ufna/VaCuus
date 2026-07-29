// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusRmlDocumentHost.h"

#include "VaCuusDefines.h"
#include "VaCuusEngine.h"
#include "VaCuusRecordingRenderInterface.h"
#include "VaCuusSlateElement.h"
#include "VaCuusStats.h"
#include "VaCuusUIThread.h"

#include "CoreGlobals.h"
#include "HAL/PlatformTLS.h"
#include "RenderingThread.h"

#include <RmlUi/Core.h>

namespace VaCuusRmlDocumentHost
{
/** Context name; one context per host, and one host in M2. */
static const char* GContextName = "VaCuusUI";

/** Virtual source name for documents loaded from memory (used in RmlUi log messages). */
static const char* GMemorySourceName = "vacuus://memory.rml";
}	 // namespace VaCuusRmlDocumentHost

FVaCuusRmlDocumentHost::FVaCuusRmlDocumentHost(const TSharedRef<FVaCuusSlateElement>& InElement)
	: Element(InElement)
{
	// Runs on the owner's thread (the UI thread does not exist yet). Nothing
	// RmlUi-affine may happen here -- that is Initialize()'s job.
}

FVaCuusRmlDocumentHost::~FVaCuusRmlDocumentHost()
{
	// Normal path: FVaCuusUIThread::Exit() already ran Shutdown() and dropped us, so
	// there is nothing left. Anything still live means teardown was skipped, and it
	// can only be finished on the thread that built it.
	if (Context != nullptr || Recorder.IsValid())
	{
		check(FVaCuusUIThread::IsInUIThread());
		Shutdown();
	}
}

bool FVaCuusRmlDocumentHost::Initialize()
{
	check(FVaCuusUIThread::IsInUIThread());

	FVaCuusEngine& Engine = FVaCuusEngine::Get();
	if (Engine.IsInitialized())
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("The document host requires an uninitialized RmlUi: the recorder must be installed before Initialize()"));
		return false;
	}

	Recorder = MakeUnique<FVaCuusRecordingRenderInterface>();
	Engine.SetRenderInterface(Recorder.Get());
	if (!Engine.Initialize())
	{
		Engine.SetRenderInterface(nullptr);
		Recorder.Reset();
		return false;
	}

	// The real size arrives with the first command (every command carries one) and
	// is applied before the first Update(), so nothing is ever laid out at 1x1.
	Context = Rml::CreateContext(VaCuusRmlDocumentHost::GContextName, Rml::Vector2i(1, 1));
	if (!Context)
	{
		UE_LOG(LogVaCuus, Error, TEXT("The document host failed to create the Rml context"));
		Shutdown();
		return false;
	}

	UE_LOG(LogVaCuus, Log, TEXT("Document host booted on the UI thread (id %u; game thread is %u)"),
		FPlatformTLS::GetCurrentThreadId(), GGameThreadId);
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
	}

	if (Context)
	{
		Rml::RemoveContext(VaCuusRmlDocumentHost::GContextName);
		Context = nullptr;
	}

	if (Recorder)
	{
		FVaCuusEngine& Engine = FVaCuusEngine::Get();
		if (Engine.IsInitialized())
		{
			// Rml::Shutdown() releases remaining geometry/textures through the
			// recorder, so the recorder must still be alive here. That trailing
			// release traffic lands in a pending buffer that is never published
			// -- dropped with the recorder below, together with the replayer's
			// mirror resources (the documented teardown pattern).
			Engine.Shutdown();
		}

		// Don't leave FVaCuusEngine holding a raw pointer to the dead recorder.
		Engine.SetRenderInterface(nullptr);
		Recorder.Reset();
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

	UE_LOG(LogVaCuus, Log, TEXT("UI view size now %dx%d"), ViewSize.X, ViewSize.Y);
}

void FVaCuusRmlDocumentHost::LoadDocumentFromFile(const FString& VfsPath)
{
	check(FVaCuusUIThread::IsInUIThread());
	if (!Context)
	{
		return;
	}

	// Goes through Rml::GetFileInterface() (FVaCuusFileInterface): relative paths --
	// including the document's own <link>/<img> references -- resolve against
	// <Project>/Content/DevUI.
	AdoptDocument(Context->LoadDocument(Rml::String(TCHAR_TO_UTF8(*VfsPath))),
		FString::Printf(TEXT("VFS ('%s')"), *VfsPath));
}

void FVaCuusRmlDocumentHost::LoadDocumentFromMemory(const FString& RmlSource)
{
	check(FVaCuusUIThread::IsInUIThread());
	if (!Context)
	{
		return;
	}

	AdoptDocument(
		Context->LoadDocumentFromMemory(
			Rml::String(TCHAR_TO_UTF8(*RmlSource)), VaCuusRmlDocumentHost::GMemorySourceName),
		TEXT("inline"));
}

void FVaCuusRmlDocumentHost::AdoptDocument(Rml::ElementDocument* NewDocument, const FString& Description)
{
	check(FVaCuusUIThread::IsInUIThread());

	if (!NewDocument)
	{
		// The previous document (if any) stays up: a failed load must not blank a
		// working view.
		UE_LOG(LogVaCuus, Error, TEXT("Failed to load the %s document"), *Description);
		return;
	}

	CloseDocument();
	Document = NewDocument;
	Document->Show();

	UE_LOG(LogVaCuus, Log, TEXT("Loaded the %s document (%dx%d)"), *Description, ViewSize.X, ViewSize.Y);
}

void FVaCuusRmlDocumentHost::CloseDocument()
{
	check(FVaCuusUIThread::IsInUIThread());

	if (Document)
	{
		// Queues the unload; RmlUi processes it in the context's next update.
		Document->Close();
		Document = nullptr;
	}
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
			TEXT("First UI frame recorded on thread %u (game thread is %u; IsInGameThread=%s)"),
			FPlatformTLS::GetCurrentThreadId(), GGameThreadId,
			IsInGameThread() ? TEXT("true") : TEXT("false"));
	}

	Recorder->BeginFrame(ViewSize);

	{
		VACUUS_PERF_SCOPE(Update);
		Context->Update();
	}

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
}
