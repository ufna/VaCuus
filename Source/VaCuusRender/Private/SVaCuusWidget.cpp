// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "SVaCuusWidget.h"

#include "VaCuusDefines.h"
#include "VaCuusInputEvent.h"
#include "VaCuusInteractiveSnapshot.h"
#include "VaCuusSlateElement.h"
#include "VaCuusStats.h"
#include "VaCuusView.h"

#include "Framework/Application/NavigationConfig.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "Rendering/DrawElements.h"
#include "RenderingThread.h"
#include "UnrealClient.h"

// Debug helper for headless verification: request a UI-inclusive screenshot
// once the view has published N frames (0 = off). Set BEFORE toggling
// vacuus.M1HUD on, e.g. -ExecCmds="vacuus.M1HUD.AutoShot 10, vacuus.M1HUD".
static TAutoConsoleVariable<int32> CVarVaCuusM1HUDAutoShot(
	TEXT("vacuus.M1HUD.AutoShot"),
	0,
	TEXT("If > 0, request a screenshot (with UI) once the view has published this many frames."));

void SVaCuusWidget::Construct(const FArguments& InArgs,
	UVaCuusView* InView,
	const TSharedRef<FVaCuusSlateElement>& InElement)
{
	View = InView;
	Element = InElement;

	SetCanTick(true);

	// The UI thread publishes a command buffer per frame that only a paint drains,
	// so the widget must repaint every frame even under Slate Global Invalidation —
	// volatility guarantees that cadence (the element still bounds the queue
	// defensively for any path this doesn't cover).
	ForceVolatile(true);

	// Hit-test visible, which it was NOT while this widget was render-only: without
	// this no pointer event ever reaches the handlers below. Pass-through is not
	// expressed here (Slate has no multi-region hit hook) but per event, by answering
	// Unhandled for points the snapshot does not cover.
	//
	// Note SLeafWidget declares SetVisibility as `override final` (SLeafWidget.h:35),
	// so this can be called but never overridden -- another reason the pass-through
	// decision has to live in the handlers.
	SetVisibility(EVisibility::Visible);

	// The default pass-through set (controller decision D12). Every key here reaches
	// the game whether or not a document has focus, because a UI that can swallow them
	// is a UI the player cannot escape from:
	//
	//   Escape  -- the universal "get me out". Task 6 answered Handled for it whenever
	//              a document held focus, which meant a focused text field could trap
	//              the player in the menu with no way back.
	//   F1-F12  -- debug and engine bindings (stat, screenshots, profiling).
	//   Tilde   -- the console. UE resolves the actual console key from
	//              Engine.Console's ConsoleKey/ConsoleKeys (Input.ini) and Tilde is the
	//              default; a project that rebinds it should extend this set.
	//
	// Note this set is keyed on FKey and therefore only affects OnKeyDown/OnKeyUp.
	// OnKeyChar has no FKey to test, but none of these keys produces a character the
	// text path would keep anyway -- they are all C0 controls or non-printing, and
	// OnKeyChar already drops those. Typing `~` into a field still works, because that
	// arrives as a CHARACTER while the key event is what passes through.
	static const FKey DefaultPassThroughKeys[] = {EKeys::Escape, EKeys::Tilde, EKeys::F1, EKeys::F2, EKeys::F3,
		EKeys::F4, EKeys::F5, EKeys::F6, EKeys::F7, EKeys::F8, EKeys::F9, EKeys::F10, EKeys::F11, EKeys::F12};
	PassThroughKeys.Reserve(UE_ARRAY_COUNT(DefaultPassThroughKeys));
	for (const FKey& Key : DefaultPassThroughKeys)
	{
		PassThroughKeys.Add(Key);
	}
}

SVaCuusWidget::~SVaCuusWidget()
{
	// Last line of defence for the navigation config: a widget destroyed while it still
	// holds focus never gets an OnFocusLost, and a leaked FNullNavigationConfig would
	// disable arrow-key navigation for every other widget in the application -- with
	// nothing left pointing at us to explain why.
	RestoreNavigationConfig();
}

void SVaCuusWidget::AddPassThroughKey(const FKey& Key)
{
	check(IsInGameThread());
	PassThroughKeys.Add(Key);
}

bool SVaCuusWidget::RemovePassThroughKey(const FKey& Key)
{
	check(IsInGameThread());
	return PassThroughKeys.Remove(Key) > 0;
}

void SVaCuusWidget::DetachView()
{
	check(IsInGameThread());
	View.Reset();

	// The view is going away, so there is no document left to navigate: stop suppressing
	// Slate's own navigation immediately rather than waiting for a focus change that may
	// never come (the widget is usually pulled out of the viewport right after this).
	RestoreNavigationConfig();
}

void SVaCuusWidget::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	// Cached for the handlers Slate gives no geometry to (OnMouseLeave, OnFocusLost)
	// and for the debug mouse command. Kept even when the view is gone, so a late
	// query answers with the last real layout instead of an identity transform.
	CachedInputGeometry = AllottedGeometry;

	UVaCuusView* ViewPtr = View.Get();
	if (ViewPtr == nullptr)
	{
		return;
	}

	// Resize is a command, not a direct call: Context::SetDimensions belongs to the
	// UI thread. The view itself drops unchanged sizes, so the steady state costs
	// nothing and a burst of resizes coalesces into one relayout.
	ViewPtr->Resize(ComputeWindowRect(AllottedGeometry).Size());

	// No trigger here: UVaCuusSubsystem::Tick is the once-per-frame pulse, which is
	// a better slot than a widget's Tick (and the only one that works for views
	// without a widget).

	// The analog stick's repeat clock (D13). Here rather than in OnAnalogValueChanged
	// because a held stick stops producing events.
	TickAnalogNavigation(InCurrentTime);

	TickAutoShot();
	FVaCuusPerfLog::TickLog();
}

void SVaCuusWidget::TickAutoShot()
{
	const int32 AutoShotFrame = CVarVaCuusM1HUDAutoShot.GetValueOnGameThread();
	if (bAutoShotDone || AutoShotFrame <= 0)
	{
		return;
	}

	// Counted in published UI frames for THIS view, not game frames: that is what
	// guarantees the document has actually been recorded and handed to the render
	// thread by the time we shoot.
	const UVaCuusView* ViewPtr = View.Get();
	const uint64 PublishedFrames = ViewPtr ? ViewPtr->GetFramesPublished() : 0;
	if (PublishedFrames < uint64(AutoShotFrame))
	{
		return;
	}

	bAutoShotDone = true;
	UE_LOG(LogVaCuus, Log, TEXT("M1 HUD auto-screenshot after %llu published UI frames"), PublishedFrames);
	FScreenshotRequest::RequestScreenshot(/*bInShowUI=*/true);
}

int32 SVaCuusWidget::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId,
	const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	// Window-space pixel rect of the widget (shared with Tick's frame size).
	// The element applies the elements-texture offset render-side
	// (FDrawPassInputs::ElementsOffset), mirroring the Slate blur pass.
	const FIntRect DestRect = ComputeWindowRect(AllottedGeometry);

	ENQUEUE_RENDER_COMMAND(VaCuusSetDestRect)(
		[LocalElement = Element, DestRect](FRHICommandListImmediate&)
		{
			LocalElement->SetDestRect_RenderThread(DestRect);
		});

	FSlateDrawElement::MakeCustom(OutDrawElements, LayerId, Element);
	return LayerId;
}

FVector2D SVaCuusWidget::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	// Viewport overlay stretched to fill the screen; the widget asks for nothing.
	return FVector2D::ZeroVector;
}

FIntRect SVaCuusWidget::ComputeWindowRect(const FGeometry& Geometry)
{
	const FSlateRect BoundingRect = Geometry.GetRenderBoundingRect();
	return FIntRect(
		FMath::RoundToInt(BoundingRect.Left), FMath::RoundToInt(BoundingRect.Top),
		FMath::RoundToInt(BoundingRect.Right), FMath::RoundToInt(BoundingRect.Bottom));
}

FIntPoint SVaCuusWidget::ToViewPixels(const FGeometry& Geometry, const UE::Slate::FDeprecateVector2DResult& ScreenPosition)
{
	// Bound by value, not by `const FVector2D&`: AbsoluteToLocal returns
	// FDeprecateVector2DResult and a reference binding would point at a temporary.
	const FVector2f Local = FVector2f(Geometry.AbsoluteToLocal(ScreenPosition));
	return FIntPoint(
		FMath::FloorToInt(Local.X * Geometry.Scale), FMath::FloorToInt(Local.Y * Geometry.Scale));
}

FVaCuusModifierState SVaCuusWidget::ToModifierState(const FInputEvent& Event)
{
	FVaCuusModifierState State;
	State.bControlDown = Event.IsControlDown();
	State.bShiftDown = Event.IsShiftDown();
	State.bAltDown = Event.IsAltDown();
	State.bCommandDown = Event.IsCommandDown();
	State.bCapsLock = Event.AreCapsLocked();
	return State;
}

const FVaCuusInteractiveSnapshot& SVaCuusWidget::GetSnapshot() const
{
	if (const UVaCuusView* ViewPtr = View.Get())
	{
		return ViewPtr->GetSnapshot();
	}

	// Detached or garbage-collected view: "nothing here is interactive", which makes
	// every handler below fall through to the game. A shared const default rather
	// than a member, because there is nothing view-specific about it.
	static const FVaCuusInteractiveSnapshot Empty;
	return Empty;
}

void SVaCuusWidget::SendInput(const FVaCuusInputEvent& Event)
{
	if (UVaCuusView* ViewPtr = View.Get())
	{
		ViewPtr->SendInput(Event);
	}
}

FReply SVaCuusWidget::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	const FIntPoint Position = ToViewPixels(MyGeometry, MouseEvent.GetScreenSpacePosition());
	SendInput(FVaCuusInputEvent::MouseMove(Position, ToModifierState(MouseEvent)));

	// While we hold capture the answer is Handled regardless of coverage: a drag that
	// started on a scrollbar must keep being ours even after the pointer wanders off
	// it, which is exactly the case the snapshot cannot express.
	if (bHasMouseCapture || GetSnapshot().Contains(Position))
	{
		return FReply::Handled();
	}

	return FReply::Unhandled();
}

FReply SVaCuusWidget::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	const FIntPoint Position = ToViewPixels(MyGeometry, MouseEvent.GetScreenSpacePosition());
	SendInput(FVaCuusInputEvent::MouseButton(
		/*bDown=*/true, Position, MouseEvent.GetEffectingButton(), ToModifierState(MouseEvent)));

	const FVaCuusInteractiveSnapshot& Snapshot = GetSnapshot();
	if (!Snapshot.Contains(Position))
	{
		// Pass-through: bubbles to SViewport and reaches the game's own input.
		return FReply::Unhandled();
	}

	FReply Reply = FReply::Handled();

	// Capture on the FIRST button only, and release when the last one comes up --
	// the idiom FWebBrowserViewport uses (WebBrowserViewport.cpp:52-78). Capture is
	// what keeps a drag alive outside the rect it started in.
	if (!bHasMouseCapture)
	{
		bHasMouseCapture = true;
		Reply.CaptureMouse(SharedThis(this));
	}

	// Keyboard focus on the FIRST click, which is what controller decision D11 buys.
	//
	// The question asked here is about the rect, not about the view: "would a click on
	// this take RmlUi focus". That is a property of the published GEOMETRY -- a frame
	// old, but a focusable element's rect is already in the snapshot before anyone
	// clicks it -- so the answer is right on the click that focuses the field.
	//
	// Task 6 asked the view-wide bWantsKeyboardFocus instead, which describes whether
	// something focusable ALREADY had focus: true only from the frame AFTER the click,
	// so a fresh <input> needed two clicks before it would accept a keystroke.
	//
	// Note what does NOT happen here: a click on a non-focusable interactive rect does
	// not clear Slate focus. If a text field had it, RmlUi's own press handling decides
	// whether the field keeps RmlUi focus, and holding Slate focus while it does is
	// exactly right.
	if (Snapshot.IsFocusableAt(Position))
	{
		Reply.SetUserFocus(SharedThis(this), EFocusCause::Mouse);
	}

	return Reply;
}

FReply SVaCuusWidget::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	const FIntPoint Position = ToViewPixels(MyGeometry, MouseEvent.GetScreenSpacePosition());
	SendInput(FVaCuusInputEvent::MouseButton(
		/*bDown=*/false, Position, MouseEvent.GetEffectingButton(), ToModifierState(MouseEvent)));

	const bool bWasCapturing = bHasMouseCapture;

	// GetPressedButtons() is the POST-release set: FSlateApplication::OnMouseUp removes
	// the released button before it constructs the FPointerEvent, and says so in its
	// own comment ("Update PressedMouseButtons before constructing the event so the
	// value-copy snapshot reflects the post-release state", SlateApplication.cpp:6098-6106).
	// So IsEmpty() -- not Num() <= 1 -- is "the last button just came up". With Left and
	// Right both down, releasing Left leaves {Right}, and a `<= 1` test would drop
	// capture with a button still held, breaking any drag that uses a second button.
	// This is also exactly what the precedent does
	// (FWebBrowserViewport::OnMouseButtonUp, WebBrowserViewport.cpp:52-78).
	if (bHasMouseCapture && MouseEvent.GetPressedButtons().IsEmpty())
	{
		bHasMouseCapture = false;

		// Handled AND releasing: an FReply that releases capture without being handled
		// would leave Slate looking for another handler for an event we consumed.
		return FReply::Handled().ReleaseMouseCapture();
	}

	if (bWasCapturing || GetSnapshot().Contains(Position))
	{
		return FReply::Handled();
	}

	// The press was pass-through, so the release must be too -- swallowing it would
	// leave the game holding a button down forever.
	return FReply::Unhandled();
}

FReply SVaCuusWidget::OnMouseButtonDoubleClick(const FGeometry& InMyGeometry, const FPointerEvent& InMouseEvent)
{
	// NOT optional, and not about double-click support. Slate delivers the SECOND
	// press of a double-click as this event instead of OnMouseButtonDown whenever the
	// widget does not hold capture (SlateApplication.cpp:6046-6050 only rewrites it
	// into a button-down for the current captor), and we release capture on every
	// button-up. Without this override RmlUi would simply never see that press: a
	// double-click would arrive as down/up/up.
	//
	// Forwarded as a plain press. RmlUi synthesises its own `dblclick` from
	// consecutive presses (Context.cpp:621+), so it needs the press, not a special
	// event.
	return OnMouseButtonDown(InMyGeometry, InMouseEvent);
}

FReply SVaCuusWidget::OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	const FIntPoint Position = ToViewPixels(MyGeometry, MouseEvent.GetScreenSpacePosition());

	// UE's sign and unit are carried through unchanged; the flip to RmlUi's
	// convention and its 80-dp unit happen at dispatch (see DispatchInputEvent).
	SendInput(FVaCuusInputEvent::MouseWheel(Position, MouseEvent.GetWheelDelta(), ToModifierState(MouseEvent)));

	// Coverage, not "is anything scrollable": the snapshot cannot say whether the
	// element under the pointer scrolls, so a wheel over any interactive region is
	// taken by the UI. The visible consequence is that a wheel over a non-scrolling
	// panel does not zoom the game camera -- which is the behaviour a player expects
	// from a UI panel anyway.
	if (GetSnapshot().Contains(Position))
	{
		return FReply::Handled();
	}

	return FReply::Unhandled();
}

void SVaCuusWidget::OnMouseEnter(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	// Returns void, so there is nothing to answer -- but the move still has to be
	// queued: it is what re-arms `mouse_active` after a ProcessMouseLeave
	// (Context.cpp:583-585), and without it hover stays dead until the pointer
	// happens to move again inside the widget.
	SendInput(FVaCuusInputEvent::MouseMove(
		ToViewPixels(MyGeometry, MouseEvent.GetScreenSpacePosition()), ToModifierState(MouseEvent)));
}

void SVaCuusWidget::OnMouseLeave(const FPointerEvent& MouseEvent)
{
	// Mandatory. Without ProcessMouseLeave the hover chain is never cleared and
	// `:hover` styling sticks forever (Context.cpp:839-846). No geometry is needed --
	// and none is given (SWidget.h:430).
	SendInput(FVaCuusInputEvent::MouseLeave());
}

void SVaCuusWidget::OnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent)
{
	// Slate can take capture away without ever sending the matching button-up: the
	// window loses focus, another widget captures, the viewport changes input mode.
	// Without this the flag would stay set forever and every later mouse move would
	// answer Handled, silently eating the game's camera input.
	if (bHasMouseCapture)
	{
		bHasMouseCapture = false;

		// RmlUi is told the pointer is gone rather than left mid-drag: it has no
		// "capture lost" concept, and a stuck :active/drag state is exactly what
		// ProcessMouseLeave clears.
		SendInput(FVaCuusInputEvent::MouseLeave());
		UE_LOG(LogVaCuus, Verbose, TEXT("VaCuus widget lost mouse capture; hover and drag state cleared"));
	}
}

FCursorReply SVaCuusWidget::OnCursorQuery(const FGeometry& MyGeometry, const FPointerEvent& CursorEvent) const
{
	// const, so this may only read the snapshot -- which is precisely why the cursor
	// shape is published in it rather than queried from RmlUi.
	const FIntPoint Position = ToViewPixels(MyGeometry, CursorEvent.GetScreenSpacePosition());
	const FVaCuusInteractiveSnapshot& Snapshot = GetSnapshot();

	if (!Snapshot.Contains(Position))
	{
		// Unhandled, not Cursor(Default): over a pass-through region the game (or
		// whatever else is under us) owns the cursor shape, and answering at all would
		// override it.
		return FCursorReply::Unhandled();
	}

	return FCursorReply::Cursor(Snapshot.Cursor);
}

FReply SVaCuusWidget::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();

	// The pass-through set (controller decision D12): not consumed AND not queued, so
	// the document never even hears about it. Checked first, before anything else can
	// have an opinion.
	if (PassThroughKeys.Contains(Key))
	{
		return FReply::Unhandled();
	}

	// Answered from the snapshot exactly like pointer events are: keys are consumed
	// only while a real focusable element holds RmlUi focus. Otherwise they bubble on
	// -- so a document that merely happens to hold Slate focus cannot swallow the
	// game's movement keys.
	//
	// Note this is a per-view verdict, not a per-key one, and it cannot be anything
	// else: RmlUi's own "was it consumed" answer is produced on the UI thread, frames
	// later in queue terms, and Slate needs an answer now. That asymmetry is precisely
	// why the pass-through set above exists as a declared contract.
	const bool bConsumeKeys = GetSnapshot().bWantsKeyboardFocus;
	const FReply Reply = bConsumeKeys ? FReply::Handled() : FReply::Unhandled();

	// The pad's Back button (D13). Not a key: RmlUi has no identifier for "cancel" and
	// no default action that would consume one, so it becomes an event of its own that
	// the UI thread answers by blurring the focused element.
	if (Key == EKeys::Gamepad_FaceButton_Right)
	{
		SendInput(FVaCuusInputEvent::NavigateBack());
		return Reply;
	}

	// Everything else, gamepad included, is a plain key: the DPad and FaceButton_Bottom
	// are in the FKey -> KeyIdentifier map (VaCuusInputMap), so no special case belongs
	// here. That is the point of putting the pad in the map rather than in the widget.
	SendInput(FVaCuusInputEvent::KeyEvent(/*bDown=*/true, Key, ToModifierState(InKeyEvent)));
	return Reply;
}

FReply SVaCuusWidget::OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();

	if (PassThroughKeys.Contains(Key))
	{
		return FReply::Unhandled();
	}

	const bool bConsumeKeys = GetSnapshot().bWantsKeyboardFocus;
	const FReply Reply = bConsumeKeys ? FReply::Handled() : FReply::Unhandled();

	// Back was fully handled on the press; there is no "un-blur" and no key identifier
	// to send, so the release is answered but not queued.
	if (Key == EKeys::Gamepad_FaceButton_Right)
	{
		return Reply;
	}

	SendInput(FVaCuusInputEvent::KeyEvent(/*bDown=*/false, Key, ToModifierState(InKeyEvent)));
	return Reply;
}

FReply SVaCuusWidget::OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent)
{
	// Fetched once: the verdict cannot change inside one handler (the snapshot is a
	// per-frame value), and this handler has several early returns -- with more coming
	// when Task 9 adds the IME path.
	const bool bConsumeKeys = GetSnapshot().bWantsKeyboardFocus;
	const FReply Reply = bConsumeKeys ? FReply::Handled() : FReply::Unhandled();

	const uint32 Unit = uint32(InCharacterEvent.GetCharacter());

	uint32 CodePoint = Unit;
	if (Unit >= 0xD800 && Unit <= 0xDBFF)
	{
		// High surrogate: half a code point. Hold it and wait -- forwarding it alone
		// would insert a replacement character or garbage.
		if (PendingHighSurrogate != 0)
		{
			// Two high halves in a row: the first will never be completed now.
			UE_LOG(LogVaCuus, Verbose,
				TEXT("Stale UTF-16 high surrogate U+%04X dropped; replaced by U+%04X"), PendingHighSurrogate, Unit);
		}

		PendingHighSurrogate = Unit;
		return Reply;
	}

	if (Unit >= 0xDC00 && Unit <= 0xDFFF)
	{
		if (PendingHighSurrogate == 0)
		{
			// A lone low surrogate: nothing sane to build from it.
			UE_LOG(LogVaCuus, Verbose, TEXT("Unpaired UTF-16 low surrogate U+%04X dropped"), Unit);
			return Reply;
		}

		CodePoint = 0x10000 + ((PendingHighSurrogate - 0xD800) << 10) + (Unit - 0xDC00);
		PendingHighSurrogate = 0;
	}
	else if (PendingHighSurrogate != 0)
	{
		// Any non-surrogate ends a pending pair; a stale high half must not join it.
		UE_LOG(LogVaCuus, Verbose,
			TEXT("Stale UTF-16 high surrogate U+%04X dropped; U+%04X is not a low surrogate"),
			PendingHighSurrogate, Unit);
		PendingHighSurrogate = 0;
	}

	// C0 controls and DEL are dropped. Slate delivers Backspace, Return, Tab and
	// Escape through OnKeyChar as well as OnKeyDown, and RmlUi's text-input path is
	// the SDL_TEXTINPUT equivalent -- it expects characters, not control codes. The
	// one control character a document does want, the newline from Return, is
	// synthesised on the key path instead (the SDL backend does the same).
	if (CodePoint < 0x20 || CodePoint == 0x7F)
	{
		return Reply;
	}

	SendInput(FVaCuusInputEvent::TextInput(CodePoint, ToModifierState(InCharacterEvent)));
	return Reply;
}

FReply SVaCuusWidget::OnAnalogValueChanged(const FGeometry& MyGeometry, const FAnalogInputEvent& InAnalogInputEvent)
{
	const FKey Key = InAnalogInputEvent.GetKey();
	const float Value = InAnalogInputEvent.GetAnalogValue();

	// LEFT STICK ONLY. The right stick and the trigger axes are left entirely to the
	// game: a camera stick that also moved the UI focus would be unusable, and there is
	// no second navigation axis for it to drive.
	if (Key == EKeys::Gamepad_LeftX)
	{
		AnalogAxisX = Value;
	}
	else if (Key == EKeys::Gamepad_LeftY)
	{
		AnalogAxisY = Value;
	}
	else
	{
		return FReply::Unhandled();
	}

	// Latched, not acted on: the press is emitted from Tick, where the repeat clock
	// lives (see TickAnalogNavigation).
	//
	// Handled while the UI owns the keyboard, so the same deflection does not also drive
	// the game's camera. Below the dead zone the deflection is not ours at all, and a
	// stick resting at 0.02 must not be reported as consumed.
	const bool bPastDeadZone = ResolveAnalogNavDirection() != EAnalogNavDirection::None;
	return (bPastDeadZone && GetSnapshot().bWantsKeyboardFocus) ? FReply::Handled() : FReply::Unhandled();
}

SVaCuusWidget::EAnalogNavDirection SVaCuusWidget::ResolveAnalogNavDirection() const
{
	const float AbsX = FMath::Abs(AnalogAxisX);
	const float AbsY = FMath::Abs(AnalogAxisY);

	// DOMINANT AXIS, not both: a diagonal push would otherwise emit two directions per
	// step and send the focus somewhere the player did not aim. Vertical wins a tie only
	// because something has to; at exactly 45 degrees there is no right answer.
	if (AbsY >= AnalogNavDeadZone && AbsY >= AbsX)
	{
		// UE's gamepad Y is positive UP (the reason MoveForward binds LeftY at +1).
		return AnalogAxisY > 0.0f ? EAnalogNavDirection::Up : EAnalogNavDirection::Down;
	}
	if (AbsX >= AnalogNavDeadZone)
	{
		return AnalogAxisX > 0.0f ? EAnalogNavDirection::Right : EAnalogNavDirection::Left;
	}

	return EAnalogNavDirection::None;
}

FKey SVaCuusWidget::AnalogNavDirectionToKey(EAnalogNavDirection Direction)
{
	// The DIGITAL left-stick keys, which VaCuusInputMap maps onto KI_UP/DOWN/LEFT/RIGHT.
	// Going through an FKey rather than reaching for a KeyIdentifier here keeps every
	// RmlUi-facing decision in the one file that owns them.
	switch (Direction)
	{
		case EAnalogNavDirection::Up:
			return EKeys::Gamepad_LeftStick_Up;
		case EAnalogNavDirection::Down:
			return EKeys::Gamepad_LeftStick_Down;
		case EAnalogNavDirection::Left:
			return EKeys::Gamepad_LeftStick_Left;
		case EAnalogNavDirection::Right:
			return EKeys::Gamepad_LeftStick_Right;
		default:
			return FKey();
	}
}

void SVaCuusWidget::SendAnalogNavKey(EAnalogNavDirection Direction)
{
	const FKey Key = AnalogNavDirectionToKey(Direction);
	if (!Key.IsValid())
	{
		return;
	}

	// A down AND an up per step. RmlUi keeps no key state of its own, so the up changes
	// nothing there -- but a document (or M4's script) listening for keyup would
	// otherwise see presses that never end, and a stream of unmatched downs is the kind
	// of asymmetry that is very hard to explain later.
	const FVaCuusModifierState NoModifiers;
	SendInput(FVaCuusInputEvent::KeyEvent(/*bDown=*/true, Key, NoModifiers));
	SendInput(FVaCuusInputEvent::KeyEvent(/*bDown=*/false, Key, NoModifiers));

	++NumAnalogNavKeys;
}

void SVaCuusWidget::TickAnalogNavigation(double InCurrentTime)
{
	const EAnalogNavDirection Direction = ResolveAnalogNavDirection();

	if (Direction == EAnalogNavDirection::None)
	{
		// Back inside the dead zone: forget the hold, so the next deflection fires
		// immediately instead of waiting out a stale repeat deadline.
		HeldAnalogDirection = EAnalogNavDirection::None;
		return;
	}

	if (Direction != HeldAnalogDirection)
	{
		// A new direction fires at once -- including a direction CHANGE while the stick
		// stays deflected, which is what makes flicking from right to down feel
		// immediate. The delay below is only for the second step of the same direction.
		HeldAnalogDirection = Direction;
		SendAnalogNavKey(Direction);
		NextAnalogNavTime = InCurrentTime + AnalogNavInitialRepeatSeconds;
		return;
	}

	if (InCurrentTime >= NextAnalogNavTime)
	{
		SendAnalogNavKey(Direction);

		// Relative to NOW, not to the previous deadline: a frame hitch must not produce
		// a burst of catch-up steps.
		NextAnalogNavTime = InCurrentTime + AnalogNavRepeatIntervalSeconds;
	}
}

void SVaCuusWidget::OverrideNavigationConfig()
{
	check(IsInGameThread());

	if (InstalledNavigationConfig.IsValid())
	{
		// Already ours. OnFocusReceived can fire more than once for the same focus
		// (different users, a re-focus within the same widget), and saving the config a
		// second time would save our own null config as the thing to restore.
		return;
	}

	if (!FSlateApplication::IsInitialized())
	{
		return;
	}

	FSlateApplication& Slate = FSlateApplication::Get();

	// SAVED, NEVER ASSUMED. The engine default is one possibility among several: the
	// editor installs its own, CommonUI installs FCommonAnalogCursor's, a game may
	// install FTwinStickNavigationConfig. Restoring "the default" would silently break
	// whichever of those was there.
	SavedNavigationConfig = Slate.GetNavigationConfig();

	const TSharedRef<FNullNavigationConfig> NullConfig = MakeShared<FNullNavigationConfig>();
	InstalledNavigationConfig = NullConfig;
	Slate.SetNavigationConfig(NullConfig);

	// WHY THIS IS NOT OPTIONAL: FSlateApplication resolves a navigation direction from
	// the key event and, if it finds one, turns the event into a navigation attempt
	// BEFORE OnKeyDown is offered the key. RmlUi's nav-* graph would therefore never see
	// an arrow, and a pad would move Slate's focus between engine widgets instead of the
	// document's. FNullNavigationConfig turns all three sources off (tab, key, analog).
	UE_LOG(LogVaCuus, Verbose,
		TEXT("VaCuus widget installed FNullNavigationConfig while focused (previous config '%s'); ")
		TEXT("RmlUi's nav-* graph now owns arrows, Tab and the stick"),
		*SavedNavigationConfig->ToString());
}

void SVaCuusWidget::RestoreNavigationConfig()
{
	if (!InstalledNavigationConfig.IsValid())
	{
		return;
	}

	// Taken out of the members first, so every exit below leaves the override flag
	// cleared -- a half-restored state that still reports "overridden" would never be
	// retried.
	const TSharedPtr<FNavigationConfig> Installed = MoveTemp(InstalledNavigationConfig);
	const TSharedPtr<FNavigationConfig> Saved = MoveTemp(SavedNavigationConfig);
	InstalledNavigationConfig.Reset();
	SavedNavigationConfig.Reset();

	if (!FSlateApplication::IsInitialized() || !Saved.IsValid())
	{
		// Slate is already down (engine shutdown): there is nothing left to restore into.
		return;
	}

	FSlateApplication& Slate = FSlateApplication::Get();
	if (&Slate.GetNavigationConfig().Get() != Installed.Get())
	{
		// Somebody installed a config after us. Putting ours back would stomp theirs, and
		// theirs is newer -- so leave it alone and say so, because the alternative is a
		// silent fight over a global.
		UE_LOG(LogVaCuus, Warning,
			TEXT("VaCuus widget will not restore the navigation config: another config was installed after ours ")
			TEXT("(now '%s'); leaving it in place"),
			*Slate.GetNavigationConfig()->ToString());
		return;
	}

	Slate.SetNavigationConfig(Saved.ToSharedRef());
	UE_LOG(LogVaCuus, Verbose, TEXT("VaCuus widget restored the previous navigation config ('%s')"),
		*Saved->ToString());
}

FReply SVaCuusWidget::OnFocusReceived(const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent)
{
	// Nothing is pushed into RmlUi: its focus is its own state, already set by the
	// click that brought us here. This exists so the transition is observable (and,
	// in Task 9, so the IME context can be activated).
	UE_LOG(LogVaCuus, Verbose, TEXT("VaCuus widget received Slate focus (cause %d)"), int32(InFocusEvent.GetCause()));

	// Controller decision D12: while we own the keyboard, Slate must stop eating
	// directions before OnKeyDown sees them.
	OverrideNavigationConfig();
	return FReply::Handled();
}

void SVaCuusWidget::OnFocusLost(const FFocusEvent& InFocusEvent)
{
	// Deliberately does NOT blur RmlUi. Slate focus and RmlUi focus are different
	// things: clearing RmlUi's would lose the caret and the selection every time the
	// player alt-tabs or the game takes focus back for a frame, and RmlUi's focus is
	// what makes returning to the field resume typing where it stopped.
	//
	// The surrogate pair state does get dropped: its second half will never arrive
	// now, and keeping it would splice it onto whatever is typed next.
	PendingHighSurrogate = 0;

	// The stick state does too, and for the same reason: a deflection held while focus
	// moves away would otherwise still be "held" when focus returns, and the first Tick
	// after that would repeat a direction the player is no longer pushing.
	AnalogAxisX = 0.0f;
	AnalogAxisY = 0.0f;
	HeldAnalogDirection = EAnalogNavDirection::None;

	// Hand navigation back. Not conditional on anything: whoever has focus now owns
	// arrow keys, and it is not us.
	RestoreNavigationConfig();

	UE_LOG(LogVaCuus, Verbose, TEXT("VaCuus widget lost Slate focus (cause %d)"), int32(InFocusEvent.GetCause()));
}
