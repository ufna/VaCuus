// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusCommandBuffer.h"
#include "VaCuusEngine.h"
#include "VaCuusRecordingRenderInterface.h"
#include "VaCuusRmlDocumentHost.h"
#include "VaCuusSlateElement.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "HAL/PlatformProcess.h"
#include "Misc/ScopeExit.h"

#include <RmlUi/Core.h>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * CLOSING A DOCUMENT MUST STILL CLEAR THE SCREEN.
 *
 * The rule the M2 render model creates: on an idle UI the render target is the ONLY copy
 * of the pixels -- the idle gate withholds every frame that draws what the render thread
 * already has, so the recorder never resends them, and
 * FVaCuusSlateElement::Draw_RenderThread composites that RT unconditionally, outside its
 * `PendingBuffers.Num() > 0` branch (VaCuusSlateElement.cpp:148-231). So ANY path that stops
 * recording must emit one clearing frame first, or the last thing it drew stays on screen
 * for good.
 *
 * UVaCuusView::Close() is BlueprintCallable, so this is a shipped API a designer can reach.
 *
 * Two tests, because the claim has two halves that are reachable at different levels:
 *  - ClearingFrame drives the PRODUCTION host on the real UI thread and pins the decision
 *    that was wrong -- a closed view records (and publishes) exactly one more frame, then
 *    stops. It is the restore-the-bug target: put `Document != nullptr` back into
 *    FVaCuusRmlDocumentHost::HasView() and both counters below stop dead at the close.
 *  - UnloadDrain pins what is IN that frame -- nothing to draw, plus the closed document's
 *    released geometry -- driving a real Rml::Context through the real recorder.
 *
 * WHAT NEITHER ESTABLISHES, said out loud: that the render target ends up blank. The clear
 * is ERenderTargetActions::Clear_Store on the replay pass (VaCuusReplayRenderer.cpp:263)
 * and there is no RHI under -nullrhi to execute it, let alone read back. What is proved is
 * everything up to that instruction: the frame is recorded, it publishes, and the buffer
 * that reaches the render thread has no draws in it -- which is exactly the input that
 * makes Clear_Store wipe the view.
 */
namespace VaCuusCloseDocumentTest
{
/** A single opaque div: geometry to lose, no text, so no font and no atlas texture. */
static const TCHAR* GDocument =
	TEXT("<rml><head><style>body{display:block;width:100%;height:100%;}")
	TEXT("div{display:block;position:absolute;left:10px;top:10px;width:100px;height:80px;background-color:#FF0000;}")
	TEXT("</style></head><body><div/></body></rml>");

static const FIntPoint GViewSize(320, 240);

/**
 * Runs exactly NumFrames UI frames, one trigger at a time. Triggering N times does NOT
 * give N frames: the wake event is an auto-reset binary latch, so triggers landing while
 * a frame is in flight coalesce.
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
}	 // namespace VaCuusCloseDocumentTest

/**
 * The production path: a real FVaCuusRmlDocumentHost on the real UI thread, closed the way
 * UVaCuusView::Close() closes it, observed through the same FVaCuusViewStatus counters the
 * game thread reads.
 *
 * The two counters are what make this testable at all. FramesRecorded says the view is
 * still being driven; FramesPublished says a buffer actually left for the render thread.
 * "One more of each, then neither again" is the whole contract: fewer means the ghost is
 * back, more means a closed view is burning a frame per tick forever.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusCloseClearingFrameTest, "VaCuus.Render.Close.ClearingFrame",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusCloseClearingFrameTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusCloseDocumentTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no worker thread to drive"));
		return true;
	}

	// The UI thread boots RmlUi itself and claims ownership of it, so nothing else may
	// hold the library when this starts.
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

	// Leaves the process as it was found whatever happens below: joins the thread, which
	// tears the view and RmlUi down on the UI thread.
	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	// A REAL Slate element, never painted. Nothing here draws, so the published buffers
	// simply queue up on it (SetPendingBuffer_RenderThread) and its Draw_RenderThread never
	// runs -- which is fine, and deliberate: what this test is about happens entirely on
	// the UI thread, and the element is here because the production host requires one.
	const TSharedRef<FVaCuusSlateElement> Element = MakeShared<FVaCuusSlateElement>();
	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();

	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MakeUnique<FVaCuusRmlDocumentHost>(Element), GViewSize, Status);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GDocument, /*LoadSerial=*/1);

	// Frame 1 drains the AddView and the load and records; frames 2-4 record the same
	// static document and are withheld, which is the state the close has to break out of.
	if (!TestTrue(TEXT("UI frames ran"), RunFrames(*UIThread, 4)))
	{
		return false;
	}

	if (!TestTrue(TEXT("The document loaded"),
			Status->LoadCompletedSerial.load(std::memory_order_acquire) == 1 &&
				Status->LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded)))
	{
		return false;
	}

	const uint64 RecordedBefore = Status->FramesRecorded.load(std::memory_order_acquire);
	const uint64 PublishedBefore = Status->FramesPublished.load(std::memory_order_acquire);
	TestTrue(TEXT("The view recorded frames"), RecordedBefore > 0);
	TestTrue(TEXT("...and published at least one"), PublishedBefore > 0);

	// THE PRECONDITION THAT MAKES THE REST MEAN ANYTHING. If the gate were not firing, the
	// view would be republishing its pixels every frame and a stale render target could
	// never be observed in the first place -- the close would look fine for the wrong
	// reason. This is the same per-view idle signal VaCuus.Threading.MultiView asserts.
	if (!TestTrue(TEXT("The idle gate is armed: fewer publishes than recorded frames"),
			PublishedBefore < RecordedBefore))
	{
		return false;
	}

	// The close, exactly as UVaCuusView::Close() issues it.
	UIThread->EnqueueCloseDocument(ViewId);

	// Three frames, not one: the first drains the close AND records the frame it owes, and
	// the other two are what would expose a view that kept recording forever.
	if (!TestTrue(TEXT("UI frames ran after the close"), RunFrames(*UIThread, 3)))
	{
		return false;
	}

	const uint64 RecordedAfter = Status->FramesRecorded.load(std::memory_order_acquire);
	const uint64 PublishedAfter = Status->FramesPublished.load(std::memory_order_acquire);

	// ONE more recorded frame. Zero is the bug this test exists for -- the closed document
	// left composited from the render target, un-clickable and undismissable. Two or more
	// would mean a document-less view is paying an Update() and a snapshot walk per tick
	// for the rest of the session.
	TestEqual(TEXT("A closed view records exactly one more frame"), RecordedAfter, RecordedBefore + 1);

	// ...and that frame PUBLISHES, which is the half that actually clears the screen: the
	// empty command list hashes differently from the document's, and the closed document's
	// released geometry is resource traffic besides, so the idle gate cannot withhold it.
	TestEqual(TEXT("...and publishes it, which is what clears the render target"),
		PublishedAfter, PublishedBefore + 1);

	return true;
}

/**
 * What is actually in the frame a close owes, driven against a real Rml::Context and the
 * real recorder on this thread (no UI thread needed -- RmlUi's own calls are affine to
 * whichever thread owns the library, and FVaCuusEngine lets an automation test be that
 * thread).
 *
 * Two properties, and the second is the one nothing else covers: RmlUi's Close() only
 * QUEUES the unload (ElementDocument.cpp:421-425 -> Context::UnloadDocument, which moves
 * the document into `unloaded_documents`). The elements, their compiled geometry and their
 * textures are freed by ReleaseUnloadedDocuments, which runs at the END of the next
 * Context::Update() (Context.cpp:216-217) -- so without a post-close frame the closed
 * document stays fully resident on both sides until Rml::RemoveContext.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusCloseUnloadDrainTest, "VaCuus.Render.Close.UnloadDrain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusCloseUnloadDrainTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusCloseDocumentTest;

	// This test boots RmlUi on the test thread and owns it for its duration, which only
	// works while nobody else does (FVaCuusEngine's owner-thread contract).
	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	FVaCuusEngine& Engine = FVaCuusEngine::Get();
	if (!TestTrue(TEXT("Initialized"), Engine.Initialize()))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Engine.Shutdown();
	};

	// Per-context render interface, exactly as FVaCuusRmlDocumentHost::Initialize does it:
	// RmlUi gives each distinct interface its own RenderManager, so this recorder sees this
	// context's traffic and nothing else.
	FVaCuusRecordingRenderInterface Recorder;
	const Rml::String ContextName("vacuus_close_unload_test");
	Rml::Context* Context =
		Rml::CreateContext(ContextName, Rml::Vector2i(GViewSize.X, GViewSize.Y), &Recorder);
	if (!TestNotNull(TEXT("Context"), Context))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Rml::RemoveContext(ContextName);
	};

	Rml::ElementDocument* Document =
		Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(GDocument)), "vacuus://close_test.rml");
	if (!TestNotNull(TEXT("Document"), Document))
	{
		return false;
	}
	Document->Show();

	const auto RecordFrame = [&Recorder, Context]()
	{
		Recorder.BeginFrame(GViewSize);
		Context->Update();
		Context->Render();
		return Recorder.EndFrameAndPublish();
	};

	const TUniquePtr<FVaCuusCommandBuffer> First = RecordFrame();
	if (!TestNotNull(TEXT("The document's first frame is published"), First.Get()))
	{
		return false;
	}
	TestTrue(TEXT("...with something to draw"), First->Commands.Num() > 0);
	if (!TestTrue(TEXT("...and the geometry it draws with"), First->NewGeometry.Num() > 0))
	{
		return false;
	}

	// Arms the gate, and pins the other half of the premise: nothing at all changed between
	// frames 1 and 2, resource traffic included (a withheld frame is by definition a frame
	// with none -- FVaCuusCommandBuffer::HasResourceTraffic), so every handle created above
	// is still live at the close below.
	TestNull(TEXT("A second, identical frame is withheld"), RecordFrame().Get());

	Document->Close();

	const TUniquePtr<FVaCuusCommandBuffer> Clearing = RecordFrame();
	if (!TestNotNull(TEXT("The frame after a close is published"), Clearing.Get()))
	{
		return false;
	}

	// THE CLEARING FRAME: no draws. This is precisely the input that makes the replayer's
	// ERenderTargetActions::Clear_Store pass (VaCuusReplayRenderer.cpp:263) wipe the view
	// instead of redrawing it.
	TestEqual(TEXT("The post-close frame draws nothing"), Clearing->Commands.Num(), 0);

	// ...and RmlUi really let the document go. Geometry only reaches ReleasedGeometry when
	// the element that owned it is destroyed, and that happens inside
	// ReleaseUnloadedDocuments at the end of Update() -- so this array IS the observable for
	// "the close drained `unloaded_documents`". Without the post-close frame it never fills.
	for (const TPair<FVaCuusGeometryHandle, FVaCuusGeometryData>& Pair : First->NewGeometry)
	{
		TestTrue(TEXT("Every geometry the document drew with is released by the post-close frame"),
			Clearing->ReleasedGeometry.Contains(Pair.Key));
	}

	// Steady state again: an empty context records an empty frame, which now hashes equal to
	// the clearing frame's and carries no traffic, so the gate withholds it. That is what
	// makes the one clearing frame *one* frame rather than a new permanent cost.
	TestNull(TEXT("The frame after the clearing frame is withheld"), RecordFrame().Get());

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
