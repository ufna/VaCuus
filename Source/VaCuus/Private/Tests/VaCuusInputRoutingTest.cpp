// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusInputEvent.h"
#include "VaCuusInteractiveSnapshot.h"
#include "VaCuusRmlCasts.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "HAL/PlatformProcess.h"
#include "InputCoreTypes.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusInputRoutingTest
{
/**
 * A document host that reports back what RmlUi actually did with the input.
 *
 * WHY A PROBE HOST AND NOT PIE: the whole point of the test is that an event pushed
 * into the UI thread's queue changes real RmlUi state -- hover, active, focus,
 * scroll offset, an input element's value. That needs a live Rml::Context updated by
 * a real UI frame, and nothing else: no RHI, no viewport, no player controller. So
 * this is VaCuusSnapshotTest's probe host plus a set of observations taken right
 * after Context::Update().
 *
 * THREAD HAND-OFF: the observations below are plain members written on the UI thread
 * and read on the test thread, with no lock and no atomic. That is sound, not sloppy:
 * the test only ever reads them after WaitForFrameCount() has seen the frame counter
 * advance, and the UI thread stores that counter with release ordering AFTER RunFrame()
 * returns (FVaCuusUIThread::Run). The acquire load therefore happens-after every write
 * made during the frame.
 */
class FProbeHost final : public IVaCuusDocumentHost
{
public:
	virtual bool Initialize(uint32 InViewId, const TSharedRef<FVaCuusViewStatus>& InStatus) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		ViewId = InViewId;
		Status = InStatus;
		ContextName = FString::Printf(TEXT("vacuus_routing_view_%u"), ViewId);

		Context = Rml::CreateContext(Rml::String(TCHAR_TO_UTF8(*ContextName)), Rml::Vector2i(1, 1));
		return Context != nullptr;
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
		// Not exercised: this test only ever loads from memory.
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
			Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(*RmlSource)), "vacuus://routing.rml");
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

		Context->Update();

		FVaCuusInteractiveSnapshot& Snapshot = Status->GetSnapshotWriteBuffer();
		BuildVaCuusInteractiveSnapshot(*Context, ViewSize, ++SnapshotGeneration, Snapshot);

		// Mirrors the production host exactly (FVaCuusRmlDocumentHost): sample the
		// cursor latch right after our own Update() and adopt it only when the serial
		// moved, then stamp it into the snapshot the build just reset.
		uint64 CursorSerial = 0;
		const EMouseCursor::Type PushedCursor = GetVaCuusLatchedMouseCursor(CursorSerial);
		if (CursorSerial != LatchedCursorSerial)
		{
			LatchedCursorSerial = CursorSerial;
			LatchedCursor = PushedCursor;
		}
		Snapshot.Cursor = LatchedCursor;

		Status->PublishSnapshot();

		// Everything the test asserts on, read after the frame it belongs to.
		Rml::Element* const Hover = Context->GetHoverElement();
		HoverId = Hover ? FString(UTF8_TO_TCHAR(Hover->GetId().c_str())) : FString();
		bHoverIsActive = Hover != nullptr && Hover->IsPseudoClassSet("active");

		Rml::Element* const Focus = Context->GetFocusElement();
		FocusId = Focus ? FString(UTF8_TO_TCHAR(Focus->GetId().c_str())) : FString();

		ScrollTop = 0.0f;
		FieldValue.Reset();
		if (RmlDocument)
		{
			if (Rml::Element* Scroll = RmlDocument->GetElementById("scroll"))
			{
				ScrollTop = Scroll->GetScrollTop();
			}
			if (Rml::Element* Field = RmlDocument->GetElementById("field"))
			{
				// VaCuusRml's exported helper, not rmlui_dynamic_cast: the id compare only
				// resolves under every load order inside VaCuusRml.so (VaCuusRmlCasts.h,
				// bead VaCuus-akj.22).
				if (Rml::ElementFormControl* Control = VaCuusCastFormControl(*Field))
				{
					FieldValue = UTF8_TO_TCHAR(Control->GetValue().c_str());
				}
			}
		}

		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

	//~ Post-frame observations. See the class comment for why plain members are safe.
	FString HoverId;
	FString FocusId;
	FString FieldValue;
	bool bHoverIsActive = false;
	float ScrollTop = 0.0f;

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

	TSharedPtr<FVaCuusViewStatus> Status;
	FString ContextName;
	Rml::Context* Context = nullptr;
	Rml::ElementDocument* RmlDocument = nullptr;
	FIntPoint ViewSize = FIntPoint::ZeroValue;
	uint32 ViewId = 0;
	uint64 SnapshotGeneration = 0;
	EMouseCursor::Type LatchedCursor = EMouseCursor::Default;
	uint64 LatchedCursorSerial = 0;
};

/**
 * Runs exactly NumFrames UI frames, one trigger at a time.
 *
 * Triggering N times and waiting for N frames does NOT work: the wake event is an
 * auto-reset binary latch, so triggers arriving while a frame is in flight coalesce.
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

/**
 * 400x300. Two focusable buttons (Tab and Shift-Tab move between them), a scrollable
 * box with content four times its height (the wheel's sign is only observable if the
 * content can actually move), and a text input (the typing path).
 *
 * #btn carries `cursor: pointer`, which is what makes the published snapshot's Cursor
 * observable end to end: RmlUi pushes the name through
 * SystemInterface::SetMouseCursor during Update, and the host latches it.
 */
static const TCHAR* GDocument = TEXT(R"(<rml>
<head>
<style>
body { display: block; width: 100%; height: 100%; }
div, button { display: block; position: absolute; }
#btn    { left: 20px;  top: 20px;  width: 100px; height: 40px; tab-index: auto; cursor: pointer; }
#btn2   { left: 20px;  top: 80px;  width: 100px; height: 40px; tab-index: auto; }
#scroll { left: 200px; top: 20px;  width: 120px; height: 60px; overflow-y: auto; }
#tall   { position: static; display: block; width: 60px; height: 400px; }
#field  { position: absolute; left: 200px; top: 150px; width: 150px; height: 30px; }
</style>
</head>
<body>
	<button id="btn"/>
	<button id="btn2"/>
	<div id="scroll"><div id="tall"/></div>
	<input id="field" type="text"/>
</body>
</rml>)");

/** Points used below, so a rect and the point inside it stay in sync. */
static const FIntPoint GButtonPoint(70, 40);
static const FIntPoint GScrollPoint(240, 40);
static const FIntPoint GFieldPoint(260, 165);
}	 // namespace VaCuusInputRoutingTest

/**
 * Input routing end to end: an event pushed onto the UI thread's queue reaches
 * Rml::Context and changes its state.
 *
 * This is the half of Task 6 that a screenshot cannot prove. It covers every
 * asymmetry the RmlUi input API has: hover and `:active` from mouse move/press,
 * focus from a press (and therefore controller decision D9's bWantsKeyboardFocus),
 * the modifier mask via Shift-Tab, the wheel's inverted sign, the UTF-32 text path,
 * and the mouse-leave that must clear hover.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusInputRoutingTest, "VaCuus.Input.Routing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusInputRoutingTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusInputRoutingTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no worker thread to drive"));
		return true;
	}

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

	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	const FIntPoint ViewSize(400, 300);
	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();

	TUniquePtr<FProbeHost> OwnedHost = MakeUnique<FProbeHost>();
	FProbeHost* Host = OwnedHost.Get();

	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), ViewSize, Status);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GDocument, /*LoadSerial=*/1);

	if (!TestTrue(TEXT("UI frames ran"), RunFrames(*UIThread, 2)))
	{
		return false;
	}

	if (!TestTrue(TEXT("Document loaded"),
			Status->LoadCompletedSerial.load(std::memory_order_acquire) == 1 &&
				Status->LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded)))
	{
		return false;
	}

	// Baseline. No pointer has been anywhere near the context yet.
	{
		const FVaCuusInteractiveSnapshot Snapshot = Status->AcquireSnapshot();
		TestTrue(TEXT("The button is reported interactive"), Snapshot.Contains(GButtonPoint));
		TestEqual(TEXT("Nothing is hovered before any input"), Host->HoverId, FString());

		// Controller decision D9, negative half: Show() focused the document element
		// itself (FocusFlag::Auto with no `autofocus`), and that must not count.
		TestFalse(TEXT("A document-only focus does not want the keyboard"), Snapshot.bWantsKeyboardFocus);
		TestEqual(TEXT("Cursor starts as the default arrow"), int32(Snapshot.Cursor), int32(EMouseCursor::Default));
	}

	const FVaCuusModifierState NoModifiers;

	// 1. Mouse move -> hover. The point is inside #btn's rect, and #btn is the
	// top-most element there, so RmlUi's own hit test must land on it.
	UIThread->EnqueueInput(ViewId, FVaCuusInputEvent::MouseMove(GButtonPoint, NoModifiers));
	if (!TestTrue(TEXT("UI frame ran after the move"), RunFrames(*UIThread, 1)))
	{
		return false;
	}
	TestEqual(TEXT("A mouse move over the button hovers it"), Host->HoverId, FString(TEXT("btn")));
	TestFalse(TEXT("Hovering alone does not activate"), Host->bHoverIsActive);

	// The cursor is published, not queried: RmlUi pushed "pointer" through
	// SystemInterface::SetMouseCursor while updating, the host latched it, and the
	// snapshot now carries it for SVaCuusWidget::OnCursorQuery to answer from.
	{
		const FVaCuusInteractiveSnapshot Snapshot = Status->AcquireSnapshot();
		TestEqual(TEXT("The hovered button's `cursor: pointer` reaches the snapshot"),
			int32(Snapshot.Cursor), int32(EMouseCursor::Hand));
	}

	// 2. Press -> `:active` and focus. Both are what a document's RCSS styles on, and
	// focus is what makes the keyboard reach the UI at all.
	UIThread->EnqueueInput(ViewId,
		FVaCuusInputEvent::MouseButton(/*bDown=*/true, GButtonPoint, EKeys::LeftMouseButton, NoModifiers));
	if (!TestTrue(TEXT("UI frame ran after the press"), RunFrames(*UIThread, 1)))
	{
		return false;
	}
	TestTrue(TEXT("A press sets :active on the hovered button"), Host->bHoverIsActive);
	TestEqual(TEXT("A press focuses the button"), Host->FocusId, FString(TEXT("btn")));

	{
		// Controller decision D9, positive half: a real focusable element holds focus,
		// so a click may now take Slate's keyboard focus.
		const FVaCuusInteractiveSnapshot Snapshot = Status->AcquireSnapshot();
		TestTrue(TEXT("A focused element makes the view want the keyboard"), Snapshot.bWantsKeyboardFocus);
	}

	// 3. Release -> `:active` clears (focus stays, as it should).
	UIThread->EnqueueInput(ViewId,
		FVaCuusInputEvent::MouseButton(/*bDown=*/false, GButtonPoint, EKeys::LeftMouseButton, NoModifiers));
	if (!TestTrue(TEXT("UI frame ran after the release"), RunFrames(*UIThread, 1)))
	{
		return false;
	}
	TestFalse(TEXT("A release clears :active"), Host->bHoverIsActive);
	TestEqual(TEXT("The button keeps focus after the release"), Host->FocusId, FString(TEXT("btn")));

	// 4. Tab and Shift-Tab. This is the modifier mask under test as much as the key
	// map: ElementDocument reads the "shift_key" event parameter to choose the
	// direction (ElementDocument.cpp:581), so a dropped KM_SHIFT would leave the
	// second assertion sitting on "btn2".
	UIThread->EnqueueInput(ViewId, FVaCuusInputEvent::KeyEvent(/*bDown=*/true, EKeys::Tab, NoModifiers));
	if (!TestTrue(TEXT("UI frame ran after Tab"), RunFrames(*UIThread, 1)))
	{
		return false;
	}
	TestEqual(TEXT("Tab moves focus forward"), Host->FocusId, FString(TEXT("btn2")));

	FVaCuusModifierState ShiftDown;
	ShiftDown.bShiftDown = true;
	UIThread->EnqueueInput(ViewId, FVaCuusInputEvent::KeyEvent(/*bDown=*/true, EKeys::Tab, ShiftDown));
	if (!TestTrue(TEXT("UI frame ran after Shift-Tab"), RunFrames(*UIThread, 1)))
	{
		return false;
	}
	TestEqual(TEXT("Shift-Tab moves focus back"), Host->FocusId, FString(TEXT("btn")));

	// 5. The wheel, and specifically its SIGN. UE's delta is positive for scroll-up
	// while RmlUi's positive Y is DOWN, so a negative UE delta must scroll the content
	// down (scroll offset grows). Getting the flip wrong scrolls the wrong way, which
	// no amount of "it compiled" catches.
	//
	// SEVERAL FRAMES PER NOTCH on purpose: RmlUi 6 smooth-scrolls a wheel notch over
	// time through its ScrollController (velocity model, ScrollController.cpp:39-63)
	// driven by SystemInterface::GetElapsedTime, so one frame only moves part of the
	// way. The first frame is guaranteed to move something
	// (SMOOTHSCROLL_FIRST_FRAME_DELTA_TIME_MIN, ScrollController.cpp:16), but running
	// the animation out makes the assertion about the sign rather than about timing.
	UIThread->EnqueueInput(ViewId, FVaCuusInputEvent::MouseMove(GScrollPoint, NoModifiers));
	UIThread->EnqueueInput(ViewId, FVaCuusInputEvent::MouseWheel(GScrollPoint, /*UE wheel-down=*/-1.0f, NoModifiers));
	if (!TestTrue(TEXT("UI frames ran after the wheel"), RunFrames(*UIThread, 12)))
	{
		return false;
	}
	const float ScrolledDown = Host->ScrollTop;
	AddInfo(FString::Printf(TEXT("Scroll offset after one wheel-down notch: %.1f px (one RmlUi unit is 80 dp)"),
		ScrolledDown));
	TestTrue(TEXT("A UE wheel-down notch scrolls the content down"), ScrolledDown > 0.0f);

	UIThread->EnqueueInput(ViewId, FVaCuusInputEvent::MouseWheel(GScrollPoint, /*UE wheel-up=*/1.0f, NoModifiers));
	if (!TestTrue(TEXT("UI frames ran after the second wheel"), RunFrames(*UIThread, 12)))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("Scroll offset after the opposite notch: %.1f px"), Host->ScrollTop));
	TestTrue(TEXT("A UE wheel-up notch scrolls back"), Host->ScrollTop < ScrolledDown);

	// 6. Typing. Focus the field with a press (RmlUi's form controls set tab-index
	// auto themselves, ElementFormControl.cpp:8), then send two code points the way
	// OnKeyChar would.
	UIThread->EnqueueInput(ViewId,
		FVaCuusInputEvent::MouseButton(/*bDown=*/true, GFieldPoint, EKeys::LeftMouseButton, NoModifiers));
	UIThread->EnqueueInput(ViewId,
		FVaCuusInputEvent::MouseButton(/*bDown=*/false, GFieldPoint, EKeys::LeftMouseButton, NoModifiers));
	if (!TestTrue(TEXT("UI frame ran after clicking the field"), RunFrames(*UIThread, 1)))
	{
		return false;
	}
	TestEqual(TEXT("Clicking the text field focuses it"), Host->FocusId, FString(TEXT("field")));

	UIThread->EnqueueInput(ViewId, FVaCuusInputEvent::TextInput(uint32('V'), NoModifiers));
	UIThread->EnqueueInput(ViewId, FVaCuusInputEvent::TextInput(uint32('a'), NoModifiers));
	if (!TestTrue(TEXT("UI frame ran after typing"), RunFrames(*UIThread, 1)))
	{
		return false;
	}
	TestEqual(TEXT("Text input reaches the focused field"), Host->FieldValue, FString(TEXT("Va")));

	// 7. Mouse leave. Without ProcessMouseLeave the hover chain is never cleared and
	// `:hover` styling sticks for the rest of the session.
	UIThread->EnqueueInput(ViewId, FVaCuusInputEvent::MouseMove(GButtonPoint, NoModifiers));
	if (!TestTrue(TEXT("UI frame ran after moving back onto the button"), RunFrames(*UIThread, 1)))
	{
		return false;
	}
	TestEqual(TEXT("The pointer is back on the button"), Host->HoverId, FString(TEXT("btn")));

	UIThread->EnqueueInput(ViewId, FVaCuusInputEvent::MouseLeave());
	if (!TestTrue(TEXT("UI frame ran after the leave"), RunFrames(*UIThread, 1)))
	{
		return false;
	}
	TestEqual(TEXT("Mouse leave clears hover"), Host->HoverId, FString());

	// 8. Input for a view that no longer exists is dropped, not a crash: the widget
	// outlives its view by a frame or two during teardown, every time.
	UIThread->EnqueueRemoveView(ViewId);
	UIThread->EnqueueInput(ViewId, FVaCuusInputEvent::MouseMove(GButtonPoint, NoModifiers));
	TestTrue(TEXT("A UI frame survives input for a removed view"), RunFrames(*UIThread, 2));

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
