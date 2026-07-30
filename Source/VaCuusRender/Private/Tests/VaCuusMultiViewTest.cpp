// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusCommandBuffer.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusRecordingRenderInterface.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "HAL/PlatformProcess.h"

#include <RmlUi/Core.h>

#include <atomic>

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusMultiViewTest
{
/**
 * What the test thread gets to look at while the UI thread drives the views.
 * Shared (thread-safe SP) so it stays valid no matter when the host dies.
 */
struct FProbe
{
	std::atomic<bool> bBooted{false};
	std::atomic<int32> Frames{0};
	std::atomic<int32> LastDraws{0};
	std::atomic<bool> bShutdown{false};
};

/**
 * A document host with no render backend: it creates a real Rml::Context with its
 * own recording render interface (exactly like the production host) but keeps the
 * published buffer instead of handing it to a Slate element. That keeps the test
 * headless -- no viewport, no PIE, no RHI -- while still proving the structural
 * claim: N contexts on ONE UI thread, each recording its own commands, and
 * removing one leaves the others running.
 */
class FProbeHost final : public IVaCuusDocumentHost
{
public:
	explicit FProbeHost(const TSharedRef<FProbe>& InProbe)
		: Probe(InProbe)
	{
	}

	virtual bool Initialize(uint32 InViewId, const TSharedRef<FVaCuusViewStatus>& InStatus) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		ViewId = InViewId;
		Status = InStatus;
		Recorder = MakeUnique<FVaCuusRecordingRenderInterface>();
		ContextName = FString::Printf(TEXT("vacuus_probe_view_%u"), ViewId);

		Context = Rml::CreateContext(Rml::String(TCHAR_TO_UTF8(*ContextName)), Rml::Vector2i(1, 1), Recorder.Get());
		if (!Context)
		{
			Recorder.Reset();
			return false;
		}

		Probe->bBooted.store(true, std::memory_order_release);
		return true;
	}

	virtual void Shutdown() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		if (RmlDocument)
		{
			RmlDocument->Close();
			RmlDocument = nullptr;
		}
		if (Context)
		{
			Rml::RemoveContext(Rml::String(TCHAR_TO_UTF8(*ContextName)));
			Context = nullptr;
		}

		// Recorder deliberately retained -- same reason as the production host:
		// Rml::Shutdown() releases this view's font textures through it.
		Probe->bShutdown.store(true, std::memory_order_release);
	}

	virtual void SetViewSize(FIntPoint InViewSize) override
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
	}

	virtual void LoadDocumentFromFile(const FString& VfsPath, uint64 LoadSerial) override
	{
		// Not exercised: the probe only ever loads from memory.
		Report(LoadSerial, /*bSuccess=*/false);
	}

	virtual void LoadDocumentFromMemory(const FString& RmlSource, uint64 LoadSerial) override
	{
		check(FVaCuusUIThread::IsInUIThread());
		if (!Context)
		{
			Report(LoadSerial, /*bSuccess=*/false);
			return;
		}

		Rml::ElementDocument* NewDocument =
			Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(*RmlSource)), "vacuus://probe.rml");
		if (!NewDocument)
		{
			Report(LoadSerial, /*bSuccess=*/false);
			return;
		}

		CloseDocument();
		RmlDocument = NewDocument;
		RmlDocument->Show();
		Report(LoadSerial, /*bSuccess=*/true);
	}

	virtual void CloseDocument() override
	{
		check(FVaCuusUIThread::IsInUIThread());
		if (RmlDocument)
		{
			RmlDocument->Close();
			RmlDocument = nullptr;
		}
	}

	virtual void SetVisible(bool bVisible) override
	{
		check(FVaCuusUIThread::IsInUIThread());
		if (!RmlDocument)
		{
			return;
		}

		if (bVisible)
		{
			RmlDocument->Show();
		}
		else
		{
			RmlDocument->Hide();
		}
	}

	virtual bool HasView() const override
	{
		check(FVaCuusUIThread::IsInUIThread());
		return Context != nullptr && RmlDocument != nullptr && ViewSize.X > 0 && ViewSize.Y > 0;
	}

	virtual Rml::Context* GetContext() const override
	{
		check(FVaCuusUIThread::IsInUIThread());
		return Context;
	}

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		Recorder->BeginFrame(ViewSize);
		Context->Update();
		Context->Render();
		const TUniquePtr<FVaCuusCommandBuffer> Buffer = Recorder->EndFrameAndPublish();

		Probe->LastDraws.store(Buffer.IsValid() ? Buffer->Commands.Num() : 0, std::memory_order_relaxed);
		Probe->Frames.fetch_add(1, std::memory_order_release);
		Status->FramesPublished.fetch_add(1, std::memory_order_release);
	}

private:
	void Report(uint64 LoadSerial, bool bSuccess)
	{
		if (Status.IsValid() && LoadSerial != 0)
		{
			Status->LoadResult.store(
				static_cast<uint8>(bSuccess ? EVaCuusLoadResult::Succeeded : EVaCuusLoadResult::Failed),
				std::memory_order_relaxed);
			Status->LoadCompletedSerial.store(LoadSerial, std::memory_order_release);
		}
	}

	TSharedRef<FProbe> Probe;
	TSharedPtr<FVaCuusViewStatus> Status;
	TUniquePtr<FVaCuusRecordingRenderInterface> Recorder;
	FString ContextName;
	Rml::Context* Context = nullptr;
	Rml::ElementDocument* RmlDocument = nullptr;
	FIntPoint ViewSize = FIntPoint::ZeroValue;
	uint32 ViewId = 0;
};

/**
 * Runs exactly NumFrames UI frames, one trigger at a time.
 *
 * Triggering N times and waiting for N frames does NOT work: the wake event is an
 * auto-reset binary latch, so triggers arriving while a frame is in flight
 * coalesce (that is the whole point of it). Waiting for each frame before asking
 * for the next is the only way to *count* frames from outside.
 */
static bool RunFrames(FVaCuusUIThread& UIThread, int32 NumFrames)
{
	for (int32 Index = 0; Index < NumFrames; ++Index)
	{
		const uint64 Before = UIThread.GetFrameCount();
		UIThread.Trigger();
		if (!UIThread.WaitForFrameCount(Before + 1, 5.0))
		{
			return false;
		}
	}

	return true;
}

/** Two visually distinct single-div documents; no font needed, so no font dependency. */
static FString MakeDocument(const TCHAR* Colour, int32 Size)
{
	return FString::Printf(
		TEXT("<rml><head><style>body{display:block;width:100%%;height:100%%;}")
		TEXT("div{display:block;position:absolute;left:10px;top:10px;width:%dpx;height:%dpx;background-color:%s;}")
		TEXT("</style></head><body><div/></body></rml>"),
		Size, Size, Colour);
}
}	 // namespace VaCuusMultiViewTest

/**
 * The structural proof for multi-view (and therefore for multi-PIE): one
 * process-wide UI thread, two Rml contexts, both producing draw commands, and
 * removing one view leaving the other rendering.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiViewTest, "VaCuus.Threading.MultiView",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusMultiViewTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiViewTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no worker thread to drive"));
		return true;
	}

	// The UI thread boots RmlUi itself and claims ownership of it, so nothing else
	// may hold the library when this starts.
	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	FVaCuusModule& Module = FVaCuusModule::Get();
	FVaCuusUIThread* UIThread = Module.GetOrStartUIThread();
	if (!TestNotNull(TEXT("UI thread"), UIThread))
	{
		return false;
	}

	// Leaves the process exactly as it was found, whatever happens below: joins the
	// thread, which tears every view and RmlUi down on the UI thread.
	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	TestTrue(TEXT("RmlUi booted with the UI thread"), FVaCuusEngine::Get().IsInitialized());

	const TSharedRef<FProbe> ProbeA = MakeShared<FProbe>();
	const TSharedRef<FProbe> ProbeB = MakeShared<FProbe>();
	const TSharedRef<FVaCuusViewStatus> StatusA = MakeShared<FVaCuusViewStatus>();
	const TSharedRef<FVaCuusViewStatus> StatusB = MakeShared<FVaCuusViewStatus>();

	const uint32 ViewA = UIThread->AllocateViewId();
	const uint32 ViewB = UIThread->AllocateViewId();
	TestNotEqual(TEXT("View ids are unique"), ViewA, ViewB);

	UIThread->EnqueueAddView(ViewA, MakeUnique<FProbeHost>(ProbeA), FIntPoint(320, 240), StatusA);
	UIThread->EnqueueAddView(ViewB, MakeUnique<FProbeHost>(ProbeB), FIntPoint(640, 480), StatusB);

	// Same thread, same frame loop, two contexts: the id on each command is what
	// routes it.
	UIThread->EnqueueLoadDocumentFromMemory(ViewA, MakeDocument(TEXT("#FF0000"), 100), /*LoadSerial=*/1);
	UIThread->EnqueueLoadDocumentFromMemory(ViewB, MakeDocument(TEXT("#0000FF"), 200), /*LoadSerial=*/1);

	// Two frames: the first drains the AddView/load commands and records, the second
	// proves the steady state.
	if (!TestTrue(TEXT("UI frames ran"), RunFrames(*UIThread, 2)))
	{
		return false;
	}

	TestEqual(TEXT("Both views are registered"), UIThread->GetNumViews(), 2);
	TestTrue(TEXT("View A booted its context"), ProbeA->bBooted.load(std::memory_order_acquire));
	TestTrue(TEXT("View B booted its context"), ProbeB->bBooted.load(std::memory_order_acquire));
	TestTrue(TEXT("View A loaded its document"),
		StatusA->LoadCompletedSerial.load(std::memory_order_acquire) == 1 &&
			StatusA->LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded));
	TestTrue(TEXT("View B loaded its document"),
		StatusB->LoadCompletedSerial.load(std::memory_order_acquire) == 1 &&
			StatusB->LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded));

	const int32 FramesA = ProbeA->Frames.load(std::memory_order_acquire);
	const int32 FramesB = ProbeB->Frames.load(std::memory_order_acquire);
	TestTrue(TEXT("View A published frames"), FramesA > 0);
	TestTrue(TEXT("View B published frames"), FramesB > 0);
	TestTrue(TEXT("View A recorded draw commands"), ProbeA->LastDraws.load(std::memory_order_relaxed) > 0);
	TestTrue(TEXT("View B recorded draw commands"), ProbeB->LastDraws.load(std::memory_order_relaxed) > 0);

	// Destroy one view; the other must keep rendering on the same thread.
	UIThread->EnqueueRemoveView(ViewA);

	const int32 FramesAAtRemoval = ProbeA->Frames.load(std::memory_order_acquire);
	if (!TestTrue(TEXT("UI frames ran after the removal"), RunFrames(*UIThread, 3)))
	{
		return false;
	}

	TestEqual(TEXT("Only one view is left"), UIThread->GetNumViews(), 1);
	TestTrue(TEXT("Removed view was shut down"), ProbeA->bShutdown.load(std::memory_order_acquire));

	// A frame that was already in flight when the removal was queued may still have
	// counted, so allow one; what must not happen is A rendering on and on.
	TestTrue(TEXT("Removed view stopped rendering"),
		ProbeA->Frames.load(std::memory_order_acquire) <= FramesAAtRemoval + 1);
	TestTrue(TEXT("Surviving view still renders"),
		ProbeB->Frames.load(std::memory_order_acquire) > FramesB + 1);
	TestTrue(TEXT("Surviving view still records draw commands"),
		ProbeB->LastDraws.load(std::memory_order_relaxed) > 0);

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
