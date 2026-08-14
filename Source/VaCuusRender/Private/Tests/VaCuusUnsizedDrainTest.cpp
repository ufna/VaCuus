// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusCommandBuffer.h"
#include "VaCuusEngine.h"
#include "VaCuusFrameSink.h"
#include "VaCuusProbeImages.h"
#include "VaCuusRmlDocumentHost.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A DECODE LAUNCHED WHILE THE VIEW IS UNSIZED MUST STILL BE TAKEN DELIVERY OF (bead
 * VaCuus-akj.6.27).
 *
 * The mismatch this pins: the decode is LAUNCHED from a size-independent path -- the load
 * runs Show(), Show() forces a layout (ElementDocument.cpp:367), layout asks the img element
 * for its intrinsic size, and that reaches FVaCuusRecordingRenderInterface::LoadTexture
 * through FileTextureDatabase::EnsureLoaded -- while DELIVERY used to happen only inside the
 * recorder's BeginFrame(), i.e. only for a view that passes HasView(), which additionally
 * demands a non-degenerate size.
 *
 * That is not a hypothetical ordering: the shipped UMG route hits it on every auto-loading
 * widget. UVaCuusWidget::RebuildWidget creates its view at FIntPoint::ZeroValue
 * (VaCuusUMGWidget.cpp:75-76) because the only correct size is the arranged pixel rect UMG
 * has not measured yet, SynchronizeProperties loads the document immediately afterwards
 * (:121-122), and the size only arrives on the first SVaCuusWidget::Tick
 * (SVaCuusWidget.cpp:253-256). A payload finishing in that window -- one measured at 144 MB
 * for a single 6000x6000 PNG (bead akj.6.25) -- stayed resident in the recorder's queue for
 * as long as the view stayed unsized, which for a widget that is never arranged is the rest
 * of the session.
 *
 * WHY THE COUNTER HAD TO BE ADDED BEFORE THIS COULD BE TESTED: during that window the view
 * records nothing and publishes nothing, so every frame counter -- the recorder's and the
 * view status's alike -- reads zero whether the payload was taken or left to rot. Delivery
 * had no observable at all; GetNumDecodeArrivals() is it.
 *
 * RESTORE-THE-BUG: delete the DrainAsyncArrivals() call from FVaCuusUIThread::RunFrame's
 * record loop. FramesRecorded stays 0 (it already is), and the arrival count stays 0 too --
 * the second assertion below is what fails.
 */
namespace VaCuusUnsizedDrainTest
{
/** 4x2, opaque: distinguishable from the 1x1 placeholder by size alone. */
static const FIntPoint GProbeSize(4, 2);

static const FIntPoint GLateViewSize(320, 240);

/**
 * ONE published buffer, kept whole so the test can look inside it. The production sinks
 * (FVaCuusSlateElement, FVaCuusWorldSink) consume a buffer into RHI state the moment it
 * arrives, which under -nullrhi would leave nothing to assert against.
 */
class FCaptureSink final : public IVaCuusFrameSink
{
public:
	virtual void SetPendingBuffer_RenderThread(FRHICommandListImmediate&, TUniquePtr<FVaCuusCommandBuffer> InBuffer) override
	{
		check(IsInRenderingThread());
		Buffers.Add(MoveTemp(InBuffer));
	}

	virtual void ReleaseResources_RenderThread() override
	{
		check(IsInRenderingThread());
		Buffers.Empty();
	}

	/** No replayer, so no census: 0 allocated is the interface's "cannot say" (VaCuus-cyn). */
	virtual void GetPublishedTextureCensus(uint64& OutLogicalBytes, uint64& OutAllocatedBytes) const override
	{
		OutLogicalBytes = 0;
		OutAllocatedBytes = 0;
	}

	/**
	 * Written on the render thread, read on the game thread AFTER FlushRenderingCommands():
	 * the flush is the happens-before edge, exactly as the world-sink tests use it.
	 */
	TArray<TUniquePtr<FVaCuusCommandBuffer>> Buffers;
};

/**
 * Runs exactly NumFrames UI frames, one trigger at a time (the wake event is an auto-reset
 * binary latch, so triggers landing while a frame is in flight coalesce). The same helper
 * VaCuusCloseDocumentTest drives its rig with.
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
}	 // namespace VaCuusUnsizedDrainTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusUnsizedDecodeDrainTest, "VaCuus.Render.Texture.UnsizedDrain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusUnsizedDecodeDrainTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusUnsizedDrainTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no worker thread to drive"));
		return true;
	}

	// The UI thread boots RmlUi itself and claims ownership of it, so nothing else may hold
	// the library when this starts.
	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	const FString TestDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VaCuusTest"));
	const FString PngPath = TestDir / TEXT("unsized_probe.png");
	TArray<uint8> ProbePixels;
	if (!TestTrue(TEXT("Probe PNG saved"), SaveVaCuusProbePng(PngPath, GProbeSize, /*Alpha=*/255, ProbePixels)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*PngPath);
		IFileManager::Get().DeleteDirectory(*TestDir);
	};

	// THE DOUBLE SLASH IS LOAD-BEARING, and it is RmlUi's rule rather than a trick of ours:
	// every <img src> goes through SystemInterface::JoinPath, whose first branch is "if the
	// path is absolute, strip the leading / and return it" (SystemInterface.cpp:54-59), so a
	// bare /abs/path would arrive at our file interface as the RELATIVE path abs/path and be
	// looked up under the DevUI roots. One extra slash survives that strip as the absolute
	// path FVaCuusFileInterface::Open passes through unresolved -- which is what lets this
	// test keep its asset in Saved/ instead of writing into a content root.
	const FString Document = FString::Printf(
		TEXT("<rml><head><style>body{display:block;width:100%%;height:100%%;}")
		TEXT("img{display:block;position:absolute;left:0px;top:0px;}")
		TEXT("</style></head><body><img src=\"/%s\"/></body></rml>"),
		*PngPath);

	FVaCuusModule& Module = FVaCuusModule::Get();
	FVaCuusUIThread* UIThread = Module.GetOrStartUIThread();
	if (!TestNotNull(TEXT("UI thread"), UIThread))
	{
		return false;
	}

	// Leaves the process as it was found whatever happens below: joins the thread, which tears
	// the view and RmlUi down on the UI thread.
	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	const TSharedRef<FCaptureSink> Sink = MakeShared<FCaptureSink>();
	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();

	TUniquePtr<FVaCuusRmlDocumentHost> OwnedHost = MakeUnique<FVaCuusRmlDocumentHost>(Sink);

	// Kept across the hand-over so the arrival counter can be read at all. Safe for exactly
	// this window: the UI thread owns the host from AddView until RemoveView or Exit(), and
	// neither happens before the StopUIThread above, which runs after every read below. The
	// counter itself is atomic and every read is ordered behind a WaitForFrameCount.
	FVaCuusRmlDocumentHost* Host = OwnedHost.Get();

	// ZERO SIZE, THE UMG SHAPE: the view is registered and the document loaded before any
	// size arrives, so HasView() is false for both commands and everything they trigger.
	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), FIntPoint::ZeroValue, Status);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, Document, /*LoadSerial=*/1);

	// The decode runs on a background worker, so no particular frame is the one it lands in.
	// Frames are what drain it, hence the loop rather than a sleep; the deadline is generous
	// because a loaded machine can defer a BackgroundHigh task.
	uint64 Arrivals = 0;
	const double Deadline = FPlatformTime::Seconds() + 5.0;
	while (FPlatformTime::Seconds() < Deadline)
	{
		if (!TestTrue(TEXT("UI frames ran"), RunFrames(*UIThread, 1)))
		{
			return false;
		}

		Arrivals = Host->GetNumDecodeArrivals();
		if (Arrivals > 0)
		{
			break;
		}

		FPlatformProcess::Sleep(0.005f);
	}

	if (!TestTrue(TEXT("The document loaded"),
			Status->LoadCompletedSerial.load(std::memory_order_acquire) == 1 &&
				Status->LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded)))
	{
		return false;
	}

	// THE PRECONDITION: this view really is unrecordable, so nothing that follows can be
	// explained by a frame having been recorded after all. If this ever fails, the test has
	// stopped covering the bead rather than the bead having been fixed.
	const uint64 RecordedWhileUnsized = Status->FramesRecorded.load(std::memory_order_acquire);
	if (!TestEqual(TEXT("An unsized view records no frame at all"), RecordedWhileUnsized, uint64(0)))
	{
		return false;
	}

	// ...and the payload was taken anyway. This is the bead.
	TestEqual(TEXT("...and its finished decode is drained anyway"), Arrivals, uint64(1));

	// The size finally arrives, exactly as SVaCuusWidget::Tick delivers it.
	//
	// Sampled BEFORE the enqueue because Enqueue() triggers the wake event itself
	// (VaCuusUIThread.cpp:800-801): the resize's own trigger can start a frame before
	// RunFrames below ever samples anything, so the count of frames this leg actually ran
	// is not a constant. It bounds the recording assertion at the end of the test.
	const uint64 FramesBeforeResize = UIThread->GetFrameCount();

	UIThread->EnqueueResize(ViewId, GLateViewSize);
	if (!TestTrue(TEXT("UI frames ran after the resize"), RunFrames(*UIThread, 1)))
	{
		return false;
	}

	// The publish crosses to the render thread by ENQUEUE_RENDER_COMMAND; this is the edge
	// that makes the sink's array readable here.
	FlushRenderingCommands();

	if (!TestEqual(TEXT("The first sized frame publishes exactly one buffer"), Sink->Buffers.Num(), 1))
	{
		return false;
	}

	const FVaCuusCommandBuffer& Published = *Sink->Buffers[0];
	TestTrue(TEXT("...at the size that finally arrived"), Published.ViewSize == GLateViewSize);
	TestTrue(TEXT("...with something to draw"), Published.Commands.Num() > 0);

	// THE PAYLOAD, NOT THE PLACEHOLDER. The arrival above overwrote the 1x1 transparent
	// texel in the pending buffer before this frame published it, so the render thread is
	// handed the real image the first time it is handed anything at all -- which is the
	// user-visible half of the bead (the placeholder never reaches the screen).
	if (!TestEqual(TEXT("One texture rides out with the frame"), Published.NewTextures.Num(), 1))
	{
		return false;
	}

	for (const TPair<FVaCuusTextureHandle, FVaCuusTextureData>& Pair : Published.NewTextures)
	{
		TestTrue(TEXT("...carrying the decoded size, not the 1x1 placeholder"), Pair.Value.Size == GProbeSize);
		TestEqual(TEXT("...and a full RGBA8 payload"), Pair.Value.RGBA.Num(), GProbeSize.X * GProbeSize.Y * 4);
	}

	// The sized view records, and it records only the frames it actually ran -- the resize
	// did not retroactively record the frames the view sat out.
	//
	// A RANGE RATHER THAN AN EXACT COUNT, and the range is the finding. This line asserted
	// == 1 until a Win64 pass failed it in both of its runs, deterministically, with every
	// substantive assertion above passing (docs/passport/2026-08-vacuus-win64-results.md §5).
	// The count is not the test's to fix at 1: RunFrames waits with WaitForFrameCount(Before
	// + 1), a FLOOR, and the resize's own Enqueue() has already triggered the wake event, so
	// whether the two triggers coalesce into one frame or land as two is a race between the
	// waiter's sample and the worker's wake. Both outcomes are correct; a 16-core Windows box
	// reproducibly takes the second one, Linux and macOS happen to take the first.
	//
	// THE READ ORDER IS LOAD-BEARING, which is why the two loads are not folded into the
	// assertion arguments: FVaCuusRmlDocumentHost bumps FramesRecorded inside the frame
	// (VaCuusRmlDocumentHost.cpp:576) and FVaCuusUIThread::Run bumps FrameCount only after
	// RunFrame returns (VaCuusUIThread.cpp:979-980), so a frame in flight is recorded but not
	// yet counted. Reading Recorded first and the frame count second makes that skew at most
	// ONE frame -- the worker is a single thread, so frame k's count bump precedes frame k+1's
	// record -- and the ceiling carries exactly that one frame of slack, no more.
	//
	// What the ceiling still catches is what the == 1 was written for: a resize that replayed
	// the frames the view sat out would push Recorded past the frames the thread ran here,
	// however many that was. The user-visible half stays exact and is asserted above -- one
	// buffer PUBLISHED, because the idle gate withholds the duplicate frame's publish, which
	// is why that assertion held on Win64 while this one did not.
	const uint64 RecordedWhenSized = Status->FramesRecorded.load(std::memory_order_acquire);
	const uint64 FramesRunSinceResize = UIThread->GetFrameCount() - FramesBeforeResize;

	TestTrue(FString::Printf(TEXT("The sized view recorded at least one frame (recorded %llu)"), RecordedWhenSized),
		RecordedWhenSized >= 1);
	TestTrue(FString::Printf(TEXT("...and no more than the %llu frame(s) the UI thread ran since the resize, "
								  "+1 for a frame in flight (recorded %llu)"),
				 FramesRunSinceResize, RecordedWhenSized),
		RecordedWhenSized <= FramesRunSinceResize + 1);

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
