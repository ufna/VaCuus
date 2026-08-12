// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "SVaCuusWidget.h"
#include "VaCuus.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusInputEvent.h"
#include "VaCuusInteractiveSnapshot.h"
#include "VaCuusSlateElement.h"
#include "VaCuusSubsystem.h"
#include "VaCuusTestDocumentHost.h"
#include "VaCuusUIThread.h"
#include "VaCuusView.h"
#include "VaCuusViewStatus.h"

#include "Engine/GameInstance.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Layout/Geometry.h"
#include "UObject/StrongObjectPtr.h"

#include <RmlUi/Core.h>

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusTouchInputTest
{
/**
 * A document host that reports what RmlUi's TOUCH pipeline did: how far the list scrolled,
 * and which pointer events actually reached the document.
 *
 * WHY THE EVENT COUNTS AND NOT JUST THE SCROLL OFFSET. Two of the four things this suite has
 * to prove are about events that must NOT happen -- a drag must not also fire `click`
 * (RmlUi cancels it, and we must not defeat that), and one finger must not press button 0
 * twice when Slate re-offers the same event to the mouse handler. Neither is visible in the
 * scroll offset, and "no phantom press" with no counter is precisely the invariant with no
 * observable that this codebase treats as a defect waiting to happen.
 *
 * THE LISTENER IS ON THE DOCUMENT, in the bubble phase: RmlUi's `<body>` IS the
 * ElementDocument, so one registration sees every pointer event in the tree, including the
 * ones whose target is the document itself (a press over empty space).
 *
 * THREAD HAND-OFF: the base's rule -- plain members written on the UI thread and read on the
 * test thread only after WaitForFrameCount() has seen the frame counter advance. The listener
 * callbacks run inside the context's ProcessTouch and ProcessMouse calls, which are on the UI thread
 * during the queue drain of the same frame, so they are covered by exactly that release/acquire
 * pair.
 */
class FProbeHost final
	: public FVaCuusTestDocumentHost
	, public Rml::EventListener
{
public:
	FProbeHost()
		: FVaCuusTestDocumentHost(TEXT("vacuus_touch_view"), "vacuus://touch.rml", Rml::FocusFlag::Auto)
	{
	}

	virtual void SetVisible(bool bVisible) override
	{
		check(FVaCuusUIThread::IsInUIThread());
		if (RmlDocument)
		{
			bVisible ? RmlDocument->Show() : RmlDocument->Hide();
		}
	}

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		Context->Update();

		FVaCuusInteractiveSnapshot& Snapshot = Status->GetSnapshotWriteBuffer();
		BuildVaCuusInteractiveSnapshot(*Context, ViewSize, ++SnapshotGeneration, Snapshot);
		Status->PublishSnapshot();

		ScrollTop = 0.0f;
		if (RmlDocument)
		{
			if (Rml::Element* List = RmlDocument->GetElementById("list"))
			{
				ScrollTop = List->GetScrollTop();
			}
		}

		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

	//~ Begin Rml::EventListener
	virtual void ProcessEvent(Rml::Event& Event) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		Rml::Element* const Target = Event.GetTargetElement();
		const FString TargetId = Target ? FString(UTF8_TO_TCHAR(Target->GetId().c_str())) : FString();

		switch (Event.GetId())
		{
			case Rml::EventId::Mousedown:
				++NumMouseDown;
				break;
			case Rml::EventId::Mouseup:
				++NumMouseUp;
				break;
			case Rml::EventId::Click:
				++NumClick;
				LastClickId = TargetId;
				break;
			case Rml::EventId::Dblclick:
				// THE PHANTOM. RmlUi synthesises this from two presses in quick succession at
				// nearly the same point (Context.cpp:653-661), which is exactly the shape a
				// finger delivered as BOTH a touch and a synthesized mouse press would have.
				++NumDblClick;
				break;
			default:
				break;
		}
	}
	//~ End Rml::EventListener

	//~ Post-frame observations; see the class comment.
	float ScrollTop = 0.0f;
	int32 NumMouseDown = 0;
	int32 NumMouseUp = 0;
	int32 NumClick = 0;
	int32 NumDblClick = 0;
	FString LastClickId;

protected:
	virtual void OnDocumentAdopted() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		// Bubble phase, so every descendant's pointer events arrive here too.
		RmlDocument->AddEventListener(Rml::EventId::Mousedown, this);
		RmlDocument->AddEventListener(Rml::EventId::Mouseup, this);
		RmlDocument->AddEventListener(Rml::EventId::Click, this);
		RmlDocument->AddEventListener(Rml::EventId::Dblclick, this);
	}

	virtual void OnDocumentClosing() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		RmlDocument->RemoveEventListener(Rml::EventId::Mousedown, this);
		RmlDocument->RemoveEventListener(Rml::EventId::Mouseup, this);
		RmlDocument->RemoveEventListener(Rml::EventId::Click, this);
		RmlDocument->RemoveEventListener(Rml::EventId::Dblclick, this);
	}

private:
	uint64 SnapshotGeneration = 0;
};

/** One UI frame at a time; the wake event coalesces, so N triggers are not N frames. */
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
 * 400x300. A button to tap, a second button that only the double-delivery block touches, a
 * scrollable list with content far taller than its box, and deliberately empty space.
 *
 * `vacuus-interactive` ON THE LIST IS LOAD-BEARING AND IS NOT A TEST CONVENIENCE. The snapshot
 * calls an element interactive when its `tab-index` is auto, its tag is one of the known
 * interactive ones, or it carries that attribute (VaCuusInteractiveSnapshot.cpp:460-462); a
 * bare `overflow-y: auto` div satisfies none of them. So an unmarked list is PASS-THROUGH by
 * design -- the UI scrolls it and the game also hears the drag -- and marking it is how an
 * author says "this panel owns the finger". Both cases are asserted below, which is why the
 * document carries a marked list and an empty region rather than only one of them.
 *
 * #tall is `position: static` so it participates in the list's layout and gives it something
 * to scroll; a positioned child would leave GetScrollHeight() equal to the client height and
 * Element::GetClosestScrollableContainer would then walk straight past the list
 * (Element.cpp:2005-2011).
 */
static const TCHAR* GDocument = TEXT(R"(<rml>
<head>
<style>
body { display: block; width: 100%; height: 100%; }
div, button { display: block; position: absolute; }
#btn   { left: 20px;  top: 20px;  width: 100px; height: 40px; tab-index: auto; }
#btn2  { left: 20px;  top: 100px; width: 100px; height: 40px; tab-index: auto; }
#list  { left: 200px; top: 20px;  width: 120px; height: 60px; overflow-y: auto; }
#tall  { position: static; display: block; width: 60px; height: 400px; }
</style>
</head>
<body>
	<button id="btn"/>
	<button id="btn2"/>
	<div id="list" vacuus-interactive><div id="tall"/></div>
</body>
</rml>)");

static const FVector2D GButtonPoint(70.0, 40.0);
static const FVector2D GButton2Point(70.0, 120.0);

//~ The drag, entirely inside #list (x 200..320, y 20..80). It runs UPWARDS, because that is
//~ the direction a reader has to get right: ProcessTouchMove scrolls by MINUS the finger's
//~ delta (Context.cpp:937, :945), so a finger travelling up (y decreasing) moves the content down
//~ and GetScrollTop() GROWS. A sign error here scrolls the wrong way, which compiles fine.
static const FVector2D GListDragStart(240.0, 70.0);
static const FVector2D GListDragMid(240.0, 50.0);
static const FVector2D GListDragEnd(240.0, 30.0);

/** Empty space: no element and no snapshot coverage, so the game must keep hearing it. */
static const FVector2D GEmptyPoint(360.0, 270.0);

/**
 * A TOUCH event, through the touch constructor -- see VaCuusWorldInputTest.cpp:92-124 for the
 * full argument about which of the two live overloads this is and why every field matters.
 * The short version: this sets bIsTouchEvent, puts the finger on its own PointerIndex, names
 * LeftMouseButton as the effecting button and bakes a CONSTANT {LeftMouseButton} into
 * PressedButtons -- for the release as much as the press, because that is what
 * FSlateApplication::OnTouchEnded does (SlateApplication.cpp:6837-6843).
 */
static FPointerEvent MakeTouchEvent(
	const FVector2D& Position, const FVector2D& LastPosition, int32 FingerIndex = 0, int32 SlateUserIndex = 0)
{
	return FPointerEvent(static_cast<uint32>(SlateUserIndex), static_cast<uint32>(FingerIndex), Position, LastPosition,
		/*InForce=*/1.0f, /*bPressLeftMouseButton=*/true, /*bInIsForceChanged=*/false, /*bInIsFirstMove=*/false,
		FModifierKeysState());
}
}	 // namespace VaCuusTouchInputTest

/**
 * TOUCH END TO END (bead VaCuus-ujm): a finger drag over a list scrolls it, a tap still
 * clicks, a drag that started on nothing still reaches the game, and one finger presses
 * button 0 exactly once.
 *
 * WHY THIS TEST EXISTS. Before it, the plugin's only scroll path was ProcessMouseWheel
 * (VaCuusUIThread.cpp's MouseWheel case) and RmlUi's touch pipeline was never called at all,
 * so a finger drag over a scrollable list did nothing on a phone -- the row
 * docs/research/mobile-support.md 1 records as "Scrolling". The suite could not have caught
 * that: until bead VaCuus-61d every synthesized FPointerEvent under Tests/ was a mouse.
 *
 * WHAT IS AND IS NOT SYNTHETIC, the same split VaCuus.Input.SlateRouting states: the widget,
 * view, queue, UI thread, RmlUi context and document are all real, and the FPointerEvents
 * carry the values FSlateApplication itself would supply. What is NOT exercised is Slate's
 * hit-test grid and its bubble routing -- a headless run has no window to hit-test against --
 * so the handlers are invoked directly, in the order and combination the engine invokes them
 * in. Block 4 depends on that: it replays Slate's touch->mouse fallback by hand, because the
 * fallback is a branch inside FSlateApplication (SlateApplication.cpp:5455, :5644) that no
 * headless venue can reach.
 *
 * RESTORE-THE-BUG, both run and recorded, and deliberately aimed at the TWO independent
 * mechanisms this change introduces -- one test that only ever caught one of them would be
 * evidence for half the work.
 *
 * 1. The move forward removed (`Context.ProcessTouchMove(Touches, Modifiers);` commented out
 *    in DispatchInputEvent, VaCuusUIThread.cpp). Fails, and fails ONLY:
 *      Scroll offset after a 20 px finger drag: 0.0 px
 *      Expected '2: a finger drag scrolls the list' to be true.
 *      Scroll offset after 40 px total: 0.0 px
 *      Expected '2: ...and keeps scrolling as the finger keeps travelling' to be true.
 *      Expected '2: the drag cancelled the pending click, as RmlUi does for a scroll' to be 0,
 *        but it was 1.
 *    The third row is the interesting one: with no move reaching RmlUi the finger never
 *    travels past TOUCH_CLICK_MAX_DISTANCE, so the drag that should have been a scroll fires a
 *    click instead -- the user-visible symptom, caught by a row phrased about clicks.
 *
 * 2. The double-delivery guard removed (SendMouseInput's `if (PointerEvent.IsTouchEvent())
 *    return;`). A completely different set fails, and again only these:
 *      Expected '4: one finger presses button 0 exactly once' to be 3, but it was 4.
 *      Expected '4: ...and releases it exactly once' to be 3, but it was 4.
 *      Expected '4: THE PHANTOM -- no double-click is synthesised from one tap' to be 0, but
 *        it was 1.
 *    The phantom double-click is not a prediction: it is what the run printed.
 *
 * With both mechanisms in, every row passes.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusTouchInputTest, "VaCuus.Input.TouchRouting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusTouchInputTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusTouchInputTest;

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

	// A view handle wired by hand; VaCuusSlateRoutingTest.cpp carries the argument for why a
	// bare UGameInstance outer is enough and why the pointers are strong.
	TStrongObjectPtr<UGameInstance> GameInstance(NewObject<UGameInstance>());
	TStrongObjectPtr<UVaCuusSubsystem> Subsystem(NewObject<UVaCuusSubsystem>(GameInstance.Get()));
	TStrongObjectPtr<UVaCuusView> View(NewObject<UVaCuusView>(Subsystem.Get()));
	View->InitializeView(Subsystem.Get(), ViewId, Status, ViewSize);

	const TSharedRef<FVaCuusSlateElement> Element = MakeShared<FVaCuusSlateElement>();
	TSharedRef<SVaCuusWidget> Widget = SNew(SVaCuusWidget, View.Get(), Element);

	// Origin (0,0), scale 1: screen-space positions ARE view pixels, so the coordinates above
	// can be read straight against the document's own RCSS.
	const FGeometry Geometry = FGeometry::MakeRoot(FVector2f(ViewSize.X, ViewSize.Y), FSlateLayoutTransform());

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

	// The widget answers Slate from the view's CACHED snapshot, and only PollStatus() refreshes
	// it -- the same once-per-frame call UVaCuusSubsystem::Tick makes (VaCuusView.cpp:863).
	// One call is enough here: this document's geometry never changes, so its coverage does
	// not either, and every FReply below is asked of the same published picture.
	View->PollStatus();

	// -- 1. The fixture is really a finger. If this fails, everything below is testing a
	//       mouse with unusual field values. --
	{
		const FPointerEvent Probe = MakeTouchEvent(GListDragStart, GListDragStart);
		TestTrue(TEXT("1: the fixture builds a touch event"), Probe.IsTouchEvent());
		TestTrue(TEXT("1: ...on a finger, not the cursor's pointer index"),
			Probe.GetPointerIndex() != FSlateApplicationBase::CursorPointerIndex);
		TestFalse(TEXT("1: ...whose pressed-button set is a constant, not a release signal"),
			Probe.GetPressedButtons().IsEmpty());

		const FVaCuusInteractiveSnapshot Snapshot = Status->AcquireSnapshot();
		TestTrue(TEXT("1: the list is published as interactive"),
			Snapshot.Contains(FIntPoint(int32(GListDragStart.X), int32(GListDragStart.Y))));
		TestFalse(TEXT("1: the empty region is not"),
			Snapshot.Contains(FIntPoint(int32(GEmptyPoint.X), int32(GEmptyPoint.Y))));
	}

	// -- 2. THE BEAD: a finger drag scrolls the list. --
	{
		const FReply Down = Widget->OnTouchStarted(Geometry, MakeTouchEvent(GListDragStart, GListDragStart));
		TestTrue(TEXT("2: a touch on the list is claimed by the UI"), Down.IsEventHandled());
		TestTrue(TEXT("2: ...and takes the gesture, so a drag off the list stays ours"),
			Widget->IsTrackingMouseCapture_Debug());
		TestEqual(TEXT("2: ...and is one outstanding finger"), Widget->GetNumActiveTouches_Debug(), 1);

		if (!TestTrue(TEXT("2: UI frame ran after the touch down"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		TestEqual(TEXT("2: the press alone scrolls nothing"), Host->ScrollTop, 0.0f);

		// UP the screen, so the content moves DOWN and ScrollTop grows -- see GListDragStart.
		TestTrue(TEXT("2: a move inside the list is ours"),
			Widget->OnTouchMoved(Geometry, MakeTouchEvent(GListDragMid, GListDragStart)).IsEventHandled());
		if (!TestTrue(TEXT("2: UI frame ran after the first move"), RunFrames(*UIThread, 1)))
		{
			return false;
		}

		const float AfterFirstMove = Host->ScrollTop;
		AddInfo(FString::Printf(TEXT("Scroll offset after a 20 px finger drag: %.1f px"), AfterFirstMove));
		TestTrue(TEXT("2: a finger drag scrolls the list"), AfterFirstMove > 0.0f);

		Widget->OnTouchMoved(Geometry, MakeTouchEvent(GListDragEnd, GListDragMid));
		if (!TestTrue(TEXT("2: UI frame ran after the second move"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		AddInfo(FString::Printf(TEXT("Scroll offset after 40 px total: %.1f px"), Host->ScrollTop));
		TestTrue(TEXT("2: ...and keeps scrolling as the finger keeps travelling"), Host->ScrollTop > AfterFirstMove);

		// The release ends the gesture -- by the LEDGER, not by the event's button set, which
		// is the same constant it was on the press (see MakeTouchEvent).
		const FReply Up = Widget->OnTouchEnded(Geometry, MakeTouchEvent(GListDragEnd, GListDragEnd));
		TestTrue(TEXT("2: the release is ours"), Up.IsEventHandled());
		TestFalse(TEXT("2: THE 61d LESSON ON THIS SIDE -- the release ends the gesture"),
			Widget->IsTrackingMouseCapture_Debug());
		TestEqual(TEXT("2: ...and no finger is left outstanding"), Widget->GetNumActiveTouches_Debug(), 0);

		if (!TestTrue(TEXT("2: UI frame ran after the release"), RunFrames(*UIThread, 1)))
		{
			return false;
		}

		// RmlUi cancels the pending click once the finger has travelled past
		// TOUCH_CLICK_MAX_DISTANCE (Context.cpp:978-981; the constant is 3 dp, :27 and :33). We must not defeat that by
		// re-implementing tap detection, and we must not defeat it by failing to forward the
		// moves that make the distance add up -- which is what makes this row the one that
		// notices a missing ProcessTouchMove even though it is phrased about clicks.
		TestEqual(TEXT("2: the drag cancelled the pending click, as RmlUi does for a scroll"), Host->NumClick, 0);
	}

	// -- 3. A plain tap still clicks. The whole gesture is one down and one up, no move. --
	{
		const int32 ClicksBefore = Host->NumClick;

		TestTrue(TEXT("3: a tap on the button is claimed by the UI"),
			Widget->OnTouchStarted(Geometry, MakeTouchEvent(GButtonPoint, GButtonPoint)).IsEventHandled());
		if (!TestTrue(TEXT("3: UI frame ran after the tap down"), RunFrames(*UIThread, 1)))
		{
			return false;
		}

		Widget->OnTouchEnded(Geometry, MakeTouchEvent(GButtonPoint, GButtonPoint));
		if (!TestTrue(TEXT("3: UI frame ran after the tap up"), RunFrames(*UIThread, 1)))
		{
			return false;
		}

		TestEqual(TEXT("3: a tap fires exactly one click"), Host->NumClick, ClicksBefore + 1);
		TestEqual(TEXT("3: ...on the button under the finger"), Host->LastClickId, FString(TEXT("btn")));
		TestEqual(TEXT("3: ...and leaves nothing outstanding"), Widget->GetNumActiveTouches_Debug(), 0);
	}

	// -- 4. DOUBLE DELIVERY: one finger, one press. --
	//
	// Slate offers a touch to OnTouchStarted and then, if that answered Unhandled, offers the
	// SAME FPointerEvent to OnMouseButtonDown (SlateApplication.cpp:5455 bubble, :5369 captor;
	// the release pair is :5644 and :5576). bTouchFallbackToMouse defaults true (:905), so the
	// fallback has to be assumed live. If the mouse handler queued as well, RmlUi would press
	// button 0 twice for one finger -- and two presses in quick succession at the same point
	// is exactly what it synthesises `dblclick` from (Context.cpp:653-661).
	//
	// Both handlers are called here on purpose, which is the fallback's worst case rather than
	// its actual one: a press over the button answers Handled, so the real Slate would not
	// have called the mouse handler at all. Asserting against the worst case is what makes the
	// row about SendMouseInput's guard rather than about which branch Slate happened to take.
	{
		const int32 DownBefore = Host->NumMouseDown;
		const int32 UpBefore = Host->NumMouseUp;
		const int32 DblBefore = Host->NumDblClick;
		const FPointerEvent Finger = MakeTouchEvent(GButton2Point, GButton2Point);

		Widget->OnTouchStarted(Geometry, Finger);
		Widget->OnMouseButtonDown(Geometry, Finger);
		if (!TestTrue(TEXT("4: UI frame ran after the press pair"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		TestEqual(TEXT("4: one finger presses button 0 exactly once"), Host->NumMouseDown, DownBefore + 1);

		Widget->OnTouchEnded(Geometry, Finger);
		Widget->OnMouseButtonUp(Geometry, Finger);
		if (!TestTrue(TEXT("4: UI frame ran after the release pair"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		TestEqual(TEXT("4: ...and releases it exactly once"), Host->NumMouseUp, UpBefore + 1);
		TestEqual(TEXT("4: THE PHANTOM -- no double-click is synthesised from one tap"),
			Host->NumDblClick, DblBefore);
		TestEqual(TEXT("4: ...and the ledger is balanced"), Widget->GetNumActiveTouches_Debug(), 0);
	}

	// -- 5. Pass-through: a drag that starts on nothing keeps reaching the game. --
	{
		TestFalse(TEXT("5: a touch on empty space is NOT claimed"),
			Widget->OnTouchStarted(Geometry, MakeTouchEvent(GEmptyPoint, GEmptyPoint)).IsEventHandled());
		TestFalse(TEXT("5: ...so no gesture is taken"), Widget->IsTrackingMouseCapture_Debug());

		// Ledgered even so: RmlUi was told about the finger (the press is queued before the
		// verdict, exactly as the mouse press is), so it is a finger that must be ended.
		TestEqual(TEXT("5: ...but RmlUi still knows about the finger"), Widget->GetNumActiveTouches_Debug(), 1);

		const FVector2D EmptyDragTo = GEmptyPoint - FVector2D(0.0, 20.0);
		TestFalse(TEXT("5: its moves keep reaching the game"),
			Widget->OnTouchMoved(Geometry, MakeTouchEvent(EmptyDragTo, GEmptyPoint)).IsEventHandled());
		TestFalse(TEXT("5: and so does its release"),
			Widget->OnTouchEnded(Geometry, MakeTouchEvent(EmptyDragTo, EmptyDragTo)).IsEventHandled());
		TestEqual(TEXT("5: ...which still balances the ledger"), Widget->GetNumActiveTouches_Debug(), 0);

		if (!TestTrue(TEXT("5: UI frame ran after the pass-through gesture"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
	}

	// -- 6. CANCEL: a gesture taken away is cancelled, not left hanging. --
	//
	// SWidget has no touch-cancel virtual and FSlateApplication has no ProcessTouchCancelled:
	// OnMouseCaptureLost is the only notification a stolen gesture produces. Left unhandled,
	// RmlUi keeps the finger in its touch_states map forever -- only ProcessTouchEnd and
	// ProcessTouchCancel erase it (Context.cpp:1017, :1031) -- and keeps button 0 down.
	{
		const int32 UpBefore = Host->NumMouseUp;

		TestTrue(TEXT("6: a touch on the list takes the gesture"),
			Widget->OnTouchStarted(Geometry, MakeTouchEvent(GListDragStart, GListDragStart)).IsEventHandled());
		TestEqual(TEXT("6: ...leaving one finger outstanding"), Widget->GetNumActiveTouches_Debug(), 1);

		// The event Slate builds when it revokes capture (FCaptureLostEvent, Events.h:118): a
		// user index and the pointer index it was revoked for, and nothing else -- which is
		// exactly why the handler cannot ask "which finger" and has to keep its own ledger.
		Widget->OnMouseCaptureLost(FCaptureLostEvent(/*InUserIndex=*/0, /*InPointerIndex=*/0));

		TestEqual(TEXT("6: capture loss cancels the outstanding finger"), Widget->GetNumActiveTouches_Debug(), 0);
		TestFalse(TEXT("6: ...and drops the gesture"), Widget->IsTrackingMouseCapture_Debug());

		if (!TestTrue(TEXT("6: UI frame ran after the cancel"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		TestEqual(TEXT("6: RmlUi released button 0 for the cancelled finger"), Host->NumMouseUp, UpBefore + 1);

		// The end Slate may still deliver afterwards is refused rather than double-counted --
		// which is what makes "one start, one end" true by construction.
		TestFalse(TEXT("6: a late end for a cancelled finger is refused"),
			Widget->OnTouchEnded(Geometry, MakeTouchEvent(GListDragStart, GListDragStart)).IsEventHandled());
		TestEqual(TEXT("6: ...and does not resurrect it"), Widget->GetNumActiveTouches_Debug(), 0);
	}

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
