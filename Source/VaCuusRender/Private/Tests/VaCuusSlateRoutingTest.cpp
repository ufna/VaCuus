// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "SVaCuusWidget.h"
#include "VaCuus.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusInteractiveSnapshot.h"
#include "VaCuusSlateElement.h"
#include "VaCuusSubsystem.h"
#include "Engine/GameInstance.h"
#include "VaCuusUIThread.h"
#include "VaCuusView.h"
#include "VaCuusViewStatus.h"

#include "Framework/Application/NavigationConfig.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "InputCoreTypes.h"
#include "Input/Events.h"
#include "Layout/Geometry.h"
#include "UObject/StrongObjectPtr.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusSlateRoutingTest
{
/**
 * A document host that reports the RmlUi state the widget's events produced.
 *
 * Same shape as VaCuusInputRoutingTest's probe, plus the focused field's value as a
 * UTF-8 HEX STRING. Hex on purpose: the surrogate assertion below is about an exact
 * code point above the BMP, and comparing hex bytes cannot be confused by an
 * FString round-trip re-splitting the character back into a surrogate pair.
 *
 * THREAD HAND-OFF: the observations are plain members written on the UI thread and
 * read on the test thread. Sound because the test only reads them after
 * WaitForFrameCount() has seen the frame counter advance, and the UI thread stores
 * that counter with release ordering after RunFrame() returns.
 */
class FProbeHost final : public IVaCuusDocumentHost
{
public:
	virtual bool Initialize(uint32 InViewId, const TSharedRef<FVaCuusViewStatus>& InStatus) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		ViewId = InViewId;
		Status = InStatus;
		ContextName = FString::Printf(TEXT("vacuus_slate_routing_view_%u"), ViewId);

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
			Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(*RmlSource)), "vacuus://slate_routing.rml");
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
		if (RmlDocument)
		{
			bVisible ? RmlDocument->Show() : RmlDocument->Hide();
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
		Status->PublishSnapshot();

		Rml::Element* const Hover = Context->GetHoverElement();
		HoverId = Hover ? FString(UTF8_TO_TCHAR(Hover->GetId().c_str())) : FString();
		bHoverIsActive = Hover != nullptr && Hover->IsPseudoClassSet("active");

		Rml::Element* const Focus = Context->GetFocusElement();
		FocusId = Focus ? FString(UTF8_TO_TCHAR(Focus->GetId().c_str())) : FString();

		FieldValueUtf8Hex.Reset();
		if (RmlDocument)
		{
			if (Rml::Element* Field = RmlDocument->GetElementById("field"))
			{
				if (Rml::ElementFormControl* Control = rmlui_dynamic_cast<Rml::ElementFormControl*>(Field))
				{
					const Rml::String Value = Control->GetValue();
					for (const char Byte : Value)
					{
						FieldValueUtf8Hex += FString::Printf(TEXT("%02X"), uint8(Byte));
					}
				}
			}
		}

		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

	//~ Post-frame observations; see the class comment for why plain members are safe.
	FString HoverId;
	FString FocusId;
	FString FieldValueUtf8Hex;
	bool bHoverIsActive = false;

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
 * Two buttons to press and navigate between, a text field to type into, and a
 * pointer-only panel. 400x300 view.
 *
 *   #btn   (20,20)-(120,60)    tab-index:auto      -> interactive AND focusable
 *   #btn2  (20,80)-(120,120)   tab-index:auto      -> the spatial-nav target below #btn
 *   #field (200,150)-(350,180) <input>             -> focusable with no RCSS help
 *   #panel (20,200)-(140,240)  vacuus-interactive  -> interactive, NOT focusable
 *
 * #panel is controller decision D11's counter-example: an opt-in marker must make a
 * plain div take clicks without making it take the keyboard.
 *
 * The nav rules are what let the synthesized analog presses actually move focus. They
 * have to be per element (nav-* is not inherited and is read with GetLocalProperty),
 * and `body { nav: auto }` is the bootstrap for the load-time state where the document
 * itself holds focus.
 */
static const TCHAR* GDocument = TEXT(R"(<rml>
<head>
<style>
body { display: block; width: 100%; height: 100%; nav: auto; }
div, button { display: block; position: absolute; }
button, input { nav: auto; }
#btn   { left: 20px;  top: 20px;  width: 100px; height: 40px; tab-index: auto; }
#btn2  { left: 20px;  top: 80px;  width: 100px; height: 40px; tab-index: auto; }
#field { position: absolute; left: 200px; top: 150px; width: 150px; height: 30px; }
#panel { left: 20px;  top: 200px; width: 120px; height: 40px; }
</style>
</head>
<body>
	<button id="btn"/>
	<button id="btn2"/>
	<input id="field" type="text"/>
	<div id="panel" vacuus-interactive/>
</body>
</rml>)");

static const FVector2D GButtonPoint(70.0, 40.0);
static const FVector2D GFieldPoint(260.0, 165.0);

/** Interactive but not focusable: a click here must not take Slate's keyboard focus. */
static const FVector2D GPanelPoint(80.0, 220.0);

/** Somewhere the document has nothing: the pass-through case. */
static const FVector2D GEmptyPoint(350.0, 280.0);

/**
 * A pointer event as FSlateApplication would build it.
 *
 * PressedButtons is the caller's business precisely because the engine's own
 * semantics differ between down and up: FSlateApplication::OnMouseUp REMOVES the
 * released button before constructing the event (SlateApplication.cpp:6098-6106), so
 * an up event's set is the post-release state. Passing it explicitly is what lets the
 * test reproduce a two-button release faithfully instead of assuming.
 */
static FPointerEvent MakePointerEvent(const FVector2D& Position, const TSet<FKey>& PressedButtons,
	const FKey& EffectingButton, float WheelDelta = 0.0f)
{
	return FPointerEvent(
		FSlateApplicationBase::CursorPointerIndex,
		Position,
		Position,
		PressedButtons,
		EffectingButton,
		WheelDelta,
		FModifierKeysState());
}

/** A key event as FSlateApplication would build one for an unmodified press. */
static FKeyEvent MakeKeyEvent(const FKey& Key)
{
	return FKeyEvent(
		Key, FModifierKeysState(), /*UserIndex=*/0, /*bIsRepeat=*/false, /*CharacterCode=*/0, /*KeyCode=*/0);
}

/** An analog axis event; Value is UE's raw deflection (+Y up, +X right). */
static FAnalogInputEvent MakeAnalogEvent(const FKey& Key, float Value)
{
	return FAnalogInputEvent(Key, FModifierKeysState(), /*UserIndex=*/0, /*bIsRepeat=*/false,
		/*CharacterCode=*/0, /*KeyCode=*/0, Value);
}
}	 // namespace VaCuusSlateRoutingTest

/**
 * The SLATE half of input routing: SVaCuusWidget's own handlers, their FReply
 * bookkeeping, and the chain from a widget handler all the way into Rml::Context.
 *
 * WHY THIS EXISTS SEPARATELY FROM VaCuus.Input.Routing: that test pushes events onto
 * the UI thread's queue directly, so it never executes a single line of
 * SVaCuusWidget. That is exactly how the capture bug this test now guards got in --
 * capture was released when the second-to-last button came up, because the code
 * assumed GetPressedButtons() still contained the button being released.
 *
 * WHAT IS AND IS NOT SYNTHETIC: the FPointerEvents, FCharacterEvents and FGeometry
 * are real engine types carrying the values FSlateApplication itself would supply
 * (see MakePointerEvent), and the widget, view, queue, UI thread, RmlUi context and
 * document are all real. What is NOT exercised is FSlateApplication's hit-test grid
 * and bubble routing -- the handlers are invoked directly rather than through a live
 * window, because a headless automation run has no window to hit-test against. That
 * routing is covered instead by the in-game verification (vacuus.M1HUD.Mouse drives
 * FSlateApplication::ProcessMouseMoveEvent for real).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusSlateRoutingTest, "VaCuus.Input.SlateRouting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusSlateRoutingTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusSlateRoutingTest;

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

	// A view handle wired by hand rather than through UVaCuusSubsystem::CreateView():
	// the subsystem reaches the UI thread only through the module
	// (UVaCuusSubsystem::GetUIThread asks FVaCuusModule), so the handle works without a
	// PIE world -- but the subsystem still needs a UGameInstance outer, because
	// UGameInstanceSubsystem declares ClassWithin=UGameInstance and NewObject ensures on
	// a mismatched outer. A bare game instance is enough; it is never initialized.
	// Strong pointers because nothing else roots any of these.
	TStrongObjectPtr<UGameInstance> GameInstance(NewObject<UGameInstance>());
	TStrongObjectPtr<UVaCuusSubsystem> Subsystem(NewObject<UVaCuusSubsystem>(GameInstance.Get()));
	TStrongObjectPtr<UVaCuusView> View(NewObject<UVaCuusView>(Subsystem.Get()));
	View->InitializeView(Subsystem.Get(), ViewId, Status, ViewSize);

	const TSharedRef<FVaCuusSlateElement> Element = MakeShared<FVaCuusSlateElement>();
	TSharedRef<SVaCuusWidget> Widget = SNew(SVaCuusWidget, View.Get(), Element);

	// Absolute origin (0,0) and scale 1, so screen-space positions ARE view pixels and
	// the coordinates below can be read against the document's own RCSS.
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

	// The widget answers from the view's CACHED snapshot, which only PollStatus()
	// refreshes -- the same once-per-frame call UVaCuusSubsystem::Tick makes.
	View->PollStatus();
	if (!TestTrue(TEXT("The cached snapshot covers the button"), View->GetSnapshot().Contains(FIntPoint(70, 40))))
	{
		return false;
	}

	const TSet<FKey> NoButtons;
	const TSet<FKey> LeftOnly = {EKeys::LeftMouseButton};
	const TSet<FKey> RightOnly = {EKeys::RightMouseButton};
	const TSet<FKey> LeftAndRight = {EKeys::LeftMouseButton, EKeys::RightMouseButton};

	// 0. CONTROLLER DECISION D11: ONE click on a focusable rect takes Slate focus.
	//
	// The premise is the state this runs in: nothing focusable holds RmlUi focus yet
	// (Show(FocusFlag::Document) focused the document element), so the view-wide
	// bWantsKeyboardFocus is FALSE -- and that is exactly the state in which Task 6's
	// version declined to take focus, which is why a fresh <input> needed two clicks
	// before it would accept a keystroke. Per-rect focusability answers on the first.
	{
		const FVaCuusInteractiveSnapshot& Snapshot = View->GetSnapshot();
		TestFalse(TEXT("Nothing focusable holds focus before the first click"), Snapshot.bWantsKeyboardFocus);
		TestTrue(TEXT("The text field's rect is published as focusable"),
			Snapshot.IsFocusableAt(FIntPoint(260, 165)));

		const FReply Reply =
			Widget->OnMouseButtonDown(Geometry, MakePointerEvent(GFieldPoint, LeftOnly, EKeys::LeftMouseButton));
		TestTrue(TEXT("The FIRST click on a focusable rect requests Slate focus"), Reply.ShouldSetUserFocus());
		TestEqual(TEXT("The focus request names this widget"),
			Reply.GetUserFocusRecepient().Get(), static_cast<SWidget*>(&Widget.Get()));
		Widget->OnMouseButtonUp(Geometry, MakePointerEvent(GFieldPoint, NoButtons, EKeys::LeftMouseButton));

		// The counter-example, and the reason focusability is not just "is it
		// interactive": a `vacuus-interactive` div takes the click but must not take the
		// keyboard away from the game.
		TestTrue(TEXT("The marked panel is interactive"), Snapshot.Contains(FIntPoint(80, 220)));
		TestFalse(TEXT("The marked panel is not focusable"), Snapshot.IsFocusableAt(FIntPoint(80, 220)));

		const FReply PanelReply =
			Widget->OnMouseButtonDown(Geometry, MakePointerEvent(GPanelPoint, LeftOnly, EKeys::LeftMouseButton));
		TestTrue(TEXT("A click on a non-focusable interactive rect is still Handled"), PanelReply.IsEventHandled());
		TestFalse(TEXT("...but does not request Slate focus"), PanelReply.ShouldSetUserFocus());
		Widget->OnMouseButtonUp(Geometry, MakePointerEvent(GPanelPoint, NoButtons, EKeys::LeftMouseButton));

		TestFalse(TEXT("Capture is back after both clicks"), Widget->IsTrackingMouseCapture_Debug());
	}

	// 1. Pass-through: a press where the snapshot reports nothing must be Unhandled,
	// or it never reaches the game, and it must not take capture either.
	{
		const FReply Reply = Widget->OnMouseButtonDown(
			Geometry, MakePointerEvent(GEmptyPoint, LeftOnly, EKeys::LeftMouseButton));
		TestFalse(TEXT("A press outside every interactive rect is Unhandled"), Reply.IsEventHandled());
		TestFalse(TEXT("A pass-through press takes no capture"), Reply.GetMouseCaptor().IsValid());
		TestFalse(TEXT("A pass-through press does not start tracking capture"),
			Widget->IsTrackingMouseCapture_Debug());

		// And the matching release must fall through too: swallowing it would leave the
		// game holding a button down forever.
		const FReply UpReply = Widget->OnMouseButtonUp(
			Geometry, MakePointerEvent(GEmptyPoint, NoButtons, EKeys::LeftMouseButton));
		TestFalse(TEXT("The matching pass-through release is Unhandled"), UpReply.IsEventHandled());
	}

	// 2. THE REGRESSION TEST for the capture bug: Left down, Right down, Left up
	// (still holding Right), Right up.
	{
		const FReply LeftDown = Widget->OnMouseButtonDown(
			Geometry, MakePointerEvent(GButtonPoint, LeftOnly, EKeys::LeftMouseButton));
		TestTrue(TEXT("A press on the button is Handled"), LeftDown.IsEventHandled());
		TestEqual(TEXT("The press captures the mouse to this widget"),
			LeftDown.GetMouseCaptor().Get(), static_cast<SWidget*>(&Widget.Get()));
		TestTrue(TEXT("The widget is tracking capture"), Widget->IsTrackingMouseCapture_Debug());

		// Second button while the first is held: already captured, so no new request.
		const FReply RightDown = Widget->OnMouseButtonDown(
			Geometry, MakePointerEvent(GButtonPoint, LeftAndRight, EKeys::RightMouseButton));
		TestTrue(TEXT("A second press is Handled"), RightDown.IsEventHandled());
		TestFalse(TEXT("A second press does not re-request capture"), RightDown.GetMouseCaptor().IsValid());
		TestTrue(TEXT("The widget still tracks capture"), Widget->IsTrackingMouseCapture_Debug());

		// Releasing Left leaves {Right} in the POST-release set, which is what
		// FSlateApplication::OnMouseUp hands us (SlateApplication.cpp:6098-6106). Capture
		// must survive: this is the exact case a `Num() <= 1` test gets wrong.
		const FReply LeftUp = Widget->OnMouseButtonUp(
			Geometry, MakePointerEvent(GButtonPoint, RightOnly, EKeys::LeftMouseButton));
		TestTrue(TEXT("Releasing one of two buttons is Handled"), LeftUp.IsEventHandled());
		TestFalse(TEXT("Releasing one of two buttons does NOT release capture"), LeftUp.ShouldReleaseMouse());
		TestTrue(TEXT("The widget still tracks capture with a button still down"),
			Widget->IsTrackingMouseCapture_Debug());

		// The last button up: now capture goes back.
		const FReply RightUp = Widget->OnMouseButtonUp(
			Geometry, MakePointerEvent(GButtonPoint, NoButtons, EKeys::RightMouseButton));
		TestTrue(TEXT("Releasing the last button is Handled"), RightUp.IsEventHandled());
		TestTrue(TEXT("Releasing the last button releases capture"), RightUp.ShouldReleaseMouse());
		TestFalse(TEXT("The widget stops tracking capture"), Widget->IsTrackingMouseCapture_Debug());
	}

	// 3. Those presses were real events on a real queue, so RmlUi must have seen them.
	if (!TestTrue(TEXT("UI frame ran after the presses"), RunFrames(*UIThread, 1)))
	{
		return false;
	}
	TestEqual(TEXT("The widget's presses reached RmlUi and focused the button"),
		Host->FocusId, FString(TEXT("btn")));
	TestEqual(TEXT("The widget's move reached RmlUi and hovered the button"),
		Host->HoverId, FString(TEXT("btn")));

	// 4. Capture lost without a button-up (window deactivation, another captor) must
	// clear the flag, or every later move answers Handled and eats the game's camera.
	{
		Widget->OnMouseButtonDown(Geometry, MakePointerEvent(GButtonPoint, LeftOnly, EKeys::LeftMouseButton));
		TestTrue(TEXT("Tracking capture again"), Widget->IsTrackingMouseCapture_Debug());

		Widget->OnMouseCaptureLost(FCaptureLostEvent(0, FSlateApplicationBase::CursorPointerIndex));
		TestFalse(TEXT("Losing capture clears the tracking flag"), Widget->IsTrackingMouseCapture_Debug());

		// And a move over a pass-through point is Unhandled again rather than stuck.
		const FReply Move = Widget->OnMouseMove(Geometry, MakePointerEvent(GEmptyPoint, NoButtons, FKey()));
		TestFalse(TEXT("After losing capture a pass-through move is Unhandled"), Move.IsEventHandled());
	}

	// 4b. CONTROLLER DECISION D12, first half: FNullNavigationConfig while focused.
	//
	// WHY THIS MATTERS MORE THAN IT LOOKS: FSlateApplication asks the navigation config
	// for a direction and, if it gets one, turns the key event into a navigation attempt
	// BEFORE OnKeyDown is offered the key. Arrow keys and the analog stick would
	// therefore never reach RmlUi's nav-* graph at all. The other half of the assertion
	// is the restore: a leaked null config would silently disable arrow navigation for
	// every other widget in the process, with nothing pointing back at us.
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication& Slate = FSlateApplication::Get();
		const TSharedRef<FNavigationConfig> ConfigBefore = Slate.GetNavigationConfig();

		TestFalse(TEXT("No navigation override before focus"), Widget->IsNavigationConfigOverridden_Debug());

		Widget->OnFocusReceived(Geometry, FFocusEvent(EFocusCause::Mouse, /*UserIndex=*/0));
		TestTrue(TEXT("Taking Slate focus overrides the navigation config"),
			Widget->IsNavigationConfigOverridden_Debug());
		TestTrue(TEXT("The installed config is not the one that was there"),
			&Slate.GetNavigationConfig().Get() != &ConfigBefore.Get());
		TestFalse(TEXT("The installed config disables key navigation"), Slate.GetNavigationConfig()->bKeyNavigation);
		TestFalse(TEXT("The installed config disables analog navigation"),
			Slate.GetNavigationConfig()->bAnalogNavigation);
		TestFalse(TEXT("The installed config disables tab navigation"), Slate.GetNavigationConfig()->bTabNavigation);

		// A second focus event must not overwrite the saved config with our own null one.
		// Slate really does re-deliver focus (a different user, a re-focus of the same
		// widget), and that mistake would make the restore below a no-op forever.
		Widget->OnFocusReceived(Geometry, FFocusEvent(EFocusCause::SetDirectly, /*UserIndex=*/0));

		Widget->OnFocusLost(FFocusEvent(EFocusCause::Mouse, /*UserIndex=*/0));
		TestFalse(TEXT("Losing focus clears the override"), Widget->IsNavigationConfigOverridden_Debug());
		TestTrue(TEXT("The EXACT config that was there before is restored, not a fresh default"),
			&Slate.GetNavigationConfig().Get() == &ConfigBefore.Get());

		AddInfo(FString::Printf(TEXT("Navigation config restored to '%s'"), *ConfigBefore->ToString()));
	}
	else
	{
		AddInfo(TEXT("Skipped the navigation-config assertions: Slate is not initialized in this session"));
	}

	// 4c. ANALOG NAVIGATION (controller decision D13). Dead zone 0.5, one press on the
	// first frame, initial repeat delay 0.4 s, then 0.12 s -- and TIME IS DRIVEN BY HAND,
	// because that is the only way any of it is assertable: a real stick cannot be held
	// for a deterministic 0.41 seconds.
	//
	// Prerequisite: #btn holds RmlUi focus from block 2's presses, so the view claims the
	// keyboard and the widget will consume the stick.
	if (!TestTrue(TEXT("UI frame ran before the analog block"), RunFrames(*UIThread, 1)))
	{
		return false;
	}
	View->PollStatus();
	if (!TestTrue(TEXT("The view wants the keyboard (a real element holds RmlUi focus)"),
			View->GetSnapshot().bWantsKeyboardFocus))
	{
		return false;
	}

	{
		const int32 NavKeysBefore = Widget->GetNumAnalogNavKeys_Debug();

		// A stick resting short of the dead zone: not consumed, and no navigation however
		// long it is held. 0.3 is a realistic idle drift on a worn pad.
		const FReply Drifting =
			Widget->OnAnalogValueChanged(Geometry, MakeAnalogEvent(EKeys::Gamepad_LeftY, -0.3f));
		TestFalse(TEXT("A stick inside the dead zone is not consumed"), Drifting.IsEventHandled());

		double Time = 1000.0;
		Widget->Tick(Geometry, Time, 0.016f);
		Widget->Tick(Geometry, Time + 1.0, 0.016f);
		TestEqual(TEXT("A stick inside the dead zone never navigates"),
			Widget->GetNumAnalogNavKeys_Debug(), NavKeysBefore);

		// Past the dead zone. UE's gamepad Y is positive UP, so -0.9 is DOWN.
		const FReply Pushed = Widget->OnAnalogValueChanged(Geometry, MakeAnalogEvent(EKeys::Gamepad_LeftY, -0.9f));
		TestTrue(TEXT("A stick past the dead zone is consumed while the UI owns the keyboard"),
			Pushed.IsEventHandled());

		// EXACTLY ONE press on the first frame -- not one per frame, which is what no
		// throttle at all looks like, and not zero, which is what a throttle that waits
		// out its own initial delay before the first step feels like.
		Time = 2000.0;
		Widget->Tick(Geometry, Time, 0.016f);
		TestEqual(TEXT("A held stick navigates exactly once on the first frame"),
			Widget->GetNumAnalogNavKeys_Debug(), NavKeysBefore + 1);

		// Cashed in immediately, while exactly one press is in flight: that press is a
		// real navigation key on the real path, and RmlUi's focus moved from #btn down to
		// #btn2 exactly as the document's `nav: auto` says it should. Doing this now (and
		// not after the repeats below) is what keeps the assertion about ONE press.
		if (!TestTrue(TEXT("UI frame ran after the first analog press"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		TestEqual(TEXT("One synthesized stick press navigated RmlUi's focus downwards"),
			Host->FocusId, FString(TEXT("btn2")));

		Widget->Tick(Geometry, Time + 0.016, 0.016f);
		Widget->Tick(Geometry, Time + 0.2, 0.016f);
		Widget->Tick(Geometry, Time + SVaCuusWidget::AnalogNavInitialRepeatSeconds - 0.01, 0.016f);
		TestEqual(TEXT("It does not repeat before the initial delay is up"),
			Widget->GetNumAnalogNavKeys_Debug(), NavKeysBefore + 1);

		const double SecondStep = Time + SVaCuusWidget::AnalogNavInitialRepeatSeconds + 0.001;
		Widget->Tick(Geometry, SecondStep, 0.016f);
		TestEqual(TEXT("It repeats once the initial delay is up"),
			Widget->GetNumAnalogNavKeys_Debug(), NavKeysBefore + 2);

		// And then at the shorter interval, not the initial delay again.
		Widget->Tick(Geometry, SecondStep + SVaCuusWidget::AnalogNavRepeatIntervalSeconds * 0.5, 0.016f);
		TestEqual(TEXT("It does not repeat inside the repeat interval"),
			Widget->GetNumAnalogNavKeys_Debug(), NavKeysBefore + 2);
		Widget->Tick(Geometry, SecondStep + SVaCuusWidget::AnalogNavRepeatIntervalSeconds + 0.001, 0.016f);
		TestEqual(TEXT("It repeats again after the repeat interval"),
			Widget->GetNumAnalogNavKeys_Debug(), NavKeysBefore + 3);

		// A DIRECTION CHANGE fires immediately rather than waiting out the delay --
		// otherwise flicking from down to up feels stuck.
		const double Flick = SecondStep + 1.0;
		Widget->OnAnalogValueChanged(Geometry, MakeAnalogEvent(EKeys::Gamepad_LeftY, 0.9f));
		Widget->Tick(Geometry, Flick, 0.016f);
		TestEqual(TEXT("Changing direction navigates immediately"),
			Widget->GetNumAnalogNavKeys_Debug(), NavKeysBefore + 4);

		// Release: back inside the dead zone and it stops, however long we wait. Done
		// before the right-stick check so that one is about the right stick alone.
		Widget->OnAnalogValueChanged(Geometry, MakeAnalogEvent(EKeys::Gamepad_LeftY, 0.0f));
		const int32 NavKeysAtRelease = Widget->GetNumAnalogNavKeys_Debug();
		Widget->Tick(Geometry, Flick + 10.0, 0.016f);
		Widget->Tick(Geometry, Flick + 20.0, 0.016f);
		TestEqual(TEXT("A released stick stops navigating"),
			Widget->GetNumAnalogNavKeys_Debug(), NavKeysAtRelease);

		// The right stick belongs to the game's camera and is never ours -- a camera stick
		// that also crept the UI focus would be unusable.
		const FReply RightStick =
			Widget->OnAnalogValueChanged(Geometry, MakeAnalogEvent(EKeys::Gamepad_RightY, -0.9f));
		TestFalse(TEXT("The right stick is never consumed"), RightStick.IsEventHandled());
		Widget->Tick(Geometry, Flick + 30.0, 0.016f);
		TestEqual(TEXT("The right stick never navigates"),
			Widget->GetNumAnalogNavKeys_Debug(), NavKeysAtRelease);
	}

	// 4d. CONTROLLER DECISION D12, second half: the pass-through key set. Escape must
	// reach the game even while the UI owns the keyboard -- Task 6 answered Handled for
	// every key in that state, which meant a focused text field could trap the player in
	// a menu with no way out.
	{
		TestTrue(TEXT("Escape is in the default pass-through set"),
			Widget->GetPassThroughKeys().Contains(EKeys::Escape));
		TestTrue(TEXT("So is the console key"), Widget->GetPassThroughKeys().Contains(EKeys::Tilde));
		TestTrue(TEXT("So are the function keys"), Widget->GetPassThroughKeys().Contains(EKeys::F5));

		// NOT ENQUEUED is the half a snapshot cannot show, and the queue counter is the
		// only observable for it: an event dropped on the game thread leaves no other
		// trace anywhere.
		const uint64 QueuedBefore = View->GetNumInputEventsQueued();
		const FReply EscapeDown = Widget->OnKeyDown(Geometry, MakeKeyEvent(EKeys::Escape));
		const FReply EscapeUp = Widget->OnKeyUp(Geometry, MakeKeyEvent(EKeys::Escape));
		TestFalse(TEXT("Escape is Unhandled even while the UI owns the keyboard"), EscapeDown.IsEventHandled());
		TestFalse(TEXT("The matching release is Unhandled too"), EscapeUp.IsEventHandled());
		TestEqual(TEXT("Escape is not even queued for the UI thread"),
			View->GetNumInputEventsQueued(), QueuedBefore);

		// The control: a key that is not in the set IS consumed and IS queued, so the
		// assertion above is about the set and not about the whole key path being dead.
		const FReply NavDown = Widget->OnKeyDown(Geometry, MakeKeyEvent(EKeys::Down));
		TestTrue(TEXT("A non-pass-through key is Handled while the UI owns the keyboard"),
			NavDown.IsEventHandled());
		TestEqual(TEXT("...and is queued"), View->GetNumInputEventsQueued(), QueuedBefore + 1);
		Widget->OnKeyUp(Geometry, MakeKeyEvent(EKeys::Down));

		// The set is a member the console command extends, not a constant.
		Widget->AddPassThroughKey(EKeys::Down);
		const uint64 QueuedAfterAdd = View->GetNumInputEventsQueued();
		TestFalse(TEXT("An added key stops being consumed"),
			Widget->OnKeyDown(Geometry, MakeKeyEvent(EKeys::Down)).IsEventHandled());
		TestEqual(TEXT("...and stops being queued"), View->GetNumInputEventsQueued(), QueuedAfterAdd);
		TestTrue(TEXT("...and can be taken back out"), Widget->RemovePassThroughKey(EKeys::Down));
		TestFalse(TEXT("Removing a key that was never in the set says so"),
			Widget->RemovePassThroughKey(EKeys::Down));
	}

	// 5. Typing, and specifically the UTF-16 surrogate pair. Focus the field with a
	// real press pair first.
	Widget->OnMouseButtonDown(Geometry, MakePointerEvent(GFieldPoint, LeftOnly, EKeys::LeftMouseButton));
	Widget->OnMouseButtonUp(Geometry, MakePointerEvent(GFieldPoint, NoButtons, EKeys::LeftMouseButton));
	if (!TestTrue(TEXT("UI frame ran after clicking the field"), RunFrames(*UIThread, 1)))
	{
		return false;
	}
	if (!TestEqual(TEXT("Clicking the field focuses it"), Host->FocusId, FString(TEXT("field"))))
	{
		return false;
	}

	{
		// U+1F600 GRINNING FACE arrives as two OnKeyChar calls, because TCHAR is
		// char16_t here (PLATFORM_TCHAR_IS_CHAR16 for Unix). Only the second one may
		// produce a character, and it must be the joined code point -- not two
		// half-characters, and not a dropped one.
		const FCharacterEvent High(TCHAR(0xD83D), FModifierKeysState(), /*UserIndex=*/0, /*bIsRepeat=*/false);
		const FCharacterEvent Low(TCHAR(0xDE00), FModifierKeysState(), /*UserIndex=*/0, /*bIsRepeat=*/false);

		Widget->OnKeyChar(Geometry, High);
		if (!TestTrue(TEXT("UI frame ran after the high surrogate"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		TestEqual(TEXT("A high surrogate alone produces no text"), Host->FieldValueUtf8Hex, FString());

		Widget->OnKeyChar(Geometry, Low);
		if (!TestTrue(TEXT("UI frame ran after the low surrogate"), RunFrames(*UIThread, 1)))
		{
			return false;
		}

		// U+1F600 in UTF-8 is F0 9F 98 80. Any other value means the pair was joined
		// wrongly, sent twice, or truncated to a single UTF-16 unit.
		AddInfo(FString::Printf(TEXT("Field value after the surrogate pair: %s"), *Host->FieldValueUtf8Hex));
		TestEqual(TEXT("The surrogate pair became one U+1F600 (UTF-8 F09F9880)"),
			Host->FieldValueUtf8Hex, FString(TEXT("F09F9880")));
	}

	{
		// A plain BMP non-ASCII character needs no joining and must not be mangled:
		// U+00E9 LATIN SMALL LETTER E WITH ACUTE is C3 A9 in UTF-8. This is also the
		// case ProcessTextInput(char) would have silently dropped (>127).
		const FCharacterEvent Accented(TCHAR(0x00E9), FModifierKeysState(), /*UserIndex=*/0, /*bIsRepeat=*/false);
		Widget->OnKeyChar(Geometry, Accented);
		if (!TestTrue(TEXT("UI frame ran after the accented character"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		TestEqual(TEXT("A BMP non-ASCII character appends as UTF-8"),
			Host->FieldValueUtf8Hex, FString(TEXT("F09F9880C3A9")));
	}

	{
		// Control characters are dropped: Slate delivers Backspace and friends through
		// OnKeyChar too, and a document's text field must not receive them as text.
		const FCharacterEvent Backspace(TCHAR('\b'), FModifierKeysState(), /*UserIndex=*/0, /*bIsRepeat=*/false);
		Widget->OnKeyChar(Geometry, Backspace);
		if (!TestTrue(TEXT("UI frame ran after the control character"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		TestEqual(TEXT("A C0 control character produces no text"),
			Host->FieldValueUtf8Hex, FString(TEXT("F09F9880C3A9")));
	}

	// 6. Detaching the view makes every handler fall through, which is what a widget
	// outliving its view by a frame during teardown must do.
	Widget->DetachView();
	{
		const FReply Reply = Widget->OnMouseButtonDown(
			Geometry, MakePointerEvent(GButtonPoint, LeftOnly, EKeys::LeftMouseButton));
		TestFalse(TEXT("A detached widget answers Unhandled"), Reply.IsEventHandled());
	}

	UIThread->EnqueueRemoveView(ViewId);
	TestTrue(TEXT("UI frames survive the removal"), RunFrames(*UIThread, 2));

	return true;
}

/**
 * The analog throttle must not navigate a UI that does not own the keyboard -- and must
 * hand back the focus it took when the UI stops owning it.
 *
 * THE BUG THIS GUARDS (found in review): the first version of TickAnalogNavigation
 * synthesized nav keys whenever the stick was past the dead zone, while every FReply path
 * correctly gated on bWantsKeyboardFocus. Slate never un-focuses a widget on its own, so
 * after ANY single click on the HUD this widget kept receiving analog events forever; a
 * player then simply holding the left stick to WALK would, 0.4 s later, have a synthesized
 * direction key delivered to a document whose own element holds focus -- which RmlUi
 * treats as "enter the grid" (FindNextNavigationElement short-cuts `current_element ==
 * this` to tab order, ElementDocument.cpp:795-796). From that frame on the view wanted the
 * keyboard, the stick was answered Handled, and movement input vanished.
 *
 * WHY VaCuus.Input.SlateRouting CANNOT CATCH IT: that test establishes
 * bWantsKeyboardFocus == true as a PREREQUISITE for its analog assertions, which is the
 * one state in which the missing gate is invisible. This test is the same code driven from
 * the opposite state, which is also the state a walking player is in.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusAnalogNavGateTest, "VaCuus.Input.AnalogNavGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusAnalogNavGateTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusSlateRoutingTest;

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

	TStrongObjectPtr<UGameInstance> GameInstance(NewObject<UGameInstance>());
	TStrongObjectPtr<UVaCuusSubsystem> Subsystem(NewObject<UVaCuusSubsystem>(GameInstance.Get()));
	TStrongObjectPtr<UVaCuusView> View(NewObject<UVaCuusView>(Subsystem.Get()));
	View->InitializeView(Subsystem.Get(), ViewId, Status, ViewSize);

	const TSharedRef<FVaCuusSlateElement> Element = MakeShared<FVaCuusSlateElement>();
	TSharedRef<SVaCuusWidget> Widget = SNew(SVaCuusWidget, View.Get(), Element);
	const FGeometry Geometry = FGeometry::MakeRoot(FVector2f(ViewSize.X, ViewSize.Y), FSlateLayoutTransform());

	if (!TestTrue(TEXT("UI frames ran"), RunFrames(*UIThread, 2)))
	{
		return false;
	}
	View->PollStatus();

	// THE TRIGGERING STATE, and the whole point of this test: a document is up, so its own
	// element holds focus (Show(FocusFlag::Document)), but nothing focusable inside it does
	// -- which is precisely what a player walking around with the HUD on top looks like.
	if (!TestFalse(TEXT("The view does not want the keyboard (only the document holds focus)"),
			View->GetSnapshot().bWantsKeyboardFocus))
	{
		return false;
	}

	const TSet<FKey> NoButtons;
	const TSet<FKey> LeftOnly = {EKeys::LeftMouseButton};

	{
		const int32 NavKeysBefore = Widget->GetNumAnalogNavKeys_Debug();
		const uint64 QueuedBefore = View->GetNumInputEventsQueued();

		// The player pushes the movement stick a long way and holds it.
		const FReply Walking = Widget->OnAnalogValueChanged(Geometry, MakeAnalogEvent(EKeys::Gamepad_LeftY, 0.95f));
		TestFalse(TEXT("A stick the UI does not own is not consumed"), Walking.IsEventHandled());

		// Held well past the initial delay and many repeat intervals -- the old code would
		// have fired at 0.4 s and then every 0.12 s.
		double Time = 5000.0;
		for (int32 Step = 0; Step < 40; ++Step)
		{
			Widget->Tick(Geometry, Time, 0.016f);
			Time += 0.05;
		}

		TestEqual(TEXT("A held stick never navigates a UI that does not own the keyboard"),
			Widget->GetNumAnalogNavKeys_Debug(), NavKeysBefore);
		TestEqual(TEXT("...and queues nothing for the UI thread, so movement is not stolen"),
			View->GetNumInputEventsQueued(), QueuedBefore);

		// And RmlUi never "entered the grid": the document still holds focus, so the view
		// still does not want the keyboard and the player keeps walking.
		if (!TestTrue(TEXT("UI frame ran after two seconds of held stick"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		View->PollStatus();
		TestFalse(TEXT("Holding the stick did not focus anything in the document"),
			View->GetSnapshot().bWantsKeyboardFocus);
		TestEqual(TEXT("Nothing inside the document took focus"), Host->FocusId, FString());

		// THE POSITIVE CONTROL: the gate is a gate, not a disable. One click on a focusable
		// rect and the same still-held stick navigates immediately -- a direction that was
		// gated out is not remembered as "already held", which is why the reset in the gate
		// matters.
		Widget->OnMouseButtonDown(Geometry, MakePointerEvent(GButtonPoint, LeftOnly, EKeys::LeftMouseButton));
		Widget->OnMouseButtonUp(Geometry, MakePointerEvent(GButtonPoint, NoButtons, EKeys::LeftMouseButton));
		TestTrue(TEXT("The click took focus this widget asked for itself"),
			Widget->HasSelfRequestedUserFocus_Debug());

		if (!TestTrue(TEXT("UI frame ran after the click"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		View->PollStatus();
		if (!TestTrue(TEXT("Clicking the button makes the view want the keyboard"),
				View->GetSnapshot().bWantsKeyboardFocus))
		{
			return false;
		}

		Widget->Tick(Geometry, Time, 0.016f);
		TestEqual(TEXT("Once the UI owns the keyboard the held stick navigates at once"),
			Widget->GetNumAnalogNavKeys_Debug(), NavKeysBefore + 1);

		// FIX 3: when the view stops wanting the keyboard, the focus this widget took is
		// handed back. Back is what a player presses to get there.
		UIThread->EnqueueInput(ViewId, FVaCuusInputEvent::NavigateBack());
		if (!TestTrue(TEXT("UI frame ran after Back"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		View->PollStatus();
		if (!TestFalse(TEXT("Back stops the view wanting the keyboard"),
				View->GetSnapshot().bWantsKeyboardFocus))
		{
			return false;
		}

		// Edge-triggered, so it is this Tick that releases. Headless has no window and
		// therefore no Slate focus path to hand back -- what is observable here is the
		// bookkeeping deciding to release and clearing itself, which is the half that
		// decides whether the release happens at all.
		Widget->Tick(Geometry, Time + 0.05, 0.016f);
		TestFalse(TEXT("The widget released the Slate focus it had taken"),
			Widget->HasSelfRequestedUserFocus_Debug());

		// And the stick is gated again, so a player who backed out of the UI can walk.
		const int32 NavKeysAfterRelease = Widget->GetNumAnalogNavKeys_Debug();
		for (int32 Step = 0; Step < 20; ++Step)
		{
			Widget->Tick(Geometry, Time + 1.0 + Step * 0.05, 0.016f);
		}
		TestEqual(TEXT("After backing out, the held stick is gated again"),
			Widget->GetNumAnalogNavKeys_Debug(), NavKeysAfterRelease);
	}

	Widget->DetachView();
	UIThread->EnqueueRemoveView(ViewId);
	TestTrue(TEXT("UI frames survive the removal"), RunFrames(*UIThread, 2));

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
