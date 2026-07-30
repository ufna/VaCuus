// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "SVaCuusWidget.h"

#include "VaCuusDefines.h"
#include "VaCuusInputEvent.h"
#include "VaCuusInteractiveSnapshot.h"
#include "VaCuusSlateElement.h"
#include "VaCuusStats.h"
#include "VaCuusView.h"

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
}

void SVaCuusWidget::DetachView()
{
	check(IsInGameThread());
	View.Reset();
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

	// Keyboard focus only when a REAL focusable element already holds RmlUi focus
	// (controller decision D9). Anything looser -- "the document is up" -- would take
	// the keyboard away from the game on the first click on any button.
	//
	// The known cost: the snapshot is one frame old, so the click that first focuses
	// a text field does not know it did, and typing needs the click after it. Buttons
	// are unaffected (RmlUi drives those entirely UI-side); see
	// FVaCuusInteractiveSnapshot::bWantsKeyboardFocus.
	if (Snapshot.bWantsKeyboardFocus)
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

	// GetPressedButtons() still contains the button being released, so "no buttons
	// left" is a set of exactly one.
	if (bHasMouseCapture && MouseEvent.GetPressedButtons().Num() <= 1)
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
	SendInput(FVaCuusInputEvent::KeyEvent(/*bDown=*/true, InKeyEvent.GetKey(), ToModifierState(InKeyEvent)));

	// Answered from the snapshot exactly like pointer events are: keys are consumed
	// only while a real focusable element holds RmlUi focus. Otherwise they bubble on
	// -- so a document that merely happens to hold Slate focus cannot swallow the
	// game's Escape or its movement keys.
	//
	// Note this is a per-view verdict, not a per-key one: RmlUi's own "was it
	// consumed" answer arrives on the UI thread, frames later in queue terms, and
	// cannot be waited for here.
	return GetSnapshot().bWantsKeyboardFocus ? FReply::Handled() : FReply::Unhandled();
}

FReply SVaCuusWidget::OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	SendInput(FVaCuusInputEvent::KeyEvent(/*bDown=*/false, InKeyEvent.GetKey(), ToModifierState(InKeyEvent)));
	return GetSnapshot().bWantsKeyboardFocus ? FReply::Handled() : FReply::Unhandled();
}

FReply SVaCuusWidget::OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent)
{
	const uint32 Unit = uint32(InCharacterEvent.GetCharacter());

	uint32 CodePoint = Unit;
	if (Unit >= 0xD800 && Unit <= 0xDBFF)
	{
		// High surrogate: half a code point. Hold it and wait -- forwarding it alone
		// would insert a replacement character or garbage.
		PendingHighSurrogate = Unit;
		return GetSnapshot().bWantsKeyboardFocus ? FReply::Handled() : FReply::Unhandled();
	}

	if (Unit >= 0xDC00 && Unit <= 0xDFFF)
	{
		if (PendingHighSurrogate == 0)
		{
			// A lone low surrogate: nothing sane to build from it.
			UE_LOG(LogVaCuus, Verbose, TEXT("Unpaired UTF-16 low surrogate U+%04X dropped"), Unit);
			return GetSnapshot().bWantsKeyboardFocus ? FReply::Handled() : FReply::Unhandled();
		}

		CodePoint = 0x10000 + ((PendingHighSurrogate - 0xD800) << 10) + (Unit - 0xDC00);
		PendingHighSurrogate = 0;
	}
	else
	{
		// Any non-surrogate ends a pending pair; a stale high half must not join it.
		PendingHighSurrogate = 0;
	}

	// C0 controls and DEL are dropped. Slate delivers Backspace, Return, Tab and
	// Escape through OnKeyChar as well as OnKeyDown, and RmlUi's text-input path is
	// the SDL_TEXTINPUT equivalent -- it expects characters, not control codes. The
	// one control character a document does want, the newline from Return, is
	// synthesised on the key path instead (the SDL backend does the same).
	if (CodePoint < 0x20 || CodePoint == 0x7F)
	{
		return GetSnapshot().bWantsKeyboardFocus ? FReply::Handled() : FReply::Unhandled();
	}

	SendInput(FVaCuusInputEvent::TextInput(CodePoint, ToModifierState(InCharacterEvent)));
	return GetSnapshot().bWantsKeyboardFocus ? FReply::Handled() : FReply::Unhandled();
}

FReply SVaCuusWidget::OnFocusReceived(const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent)
{
	// Nothing is pushed into RmlUi: its focus is its own state, already set by the
	// click that brought us here. This exists so the transition is observable (and,
	// in Task 9, so the IME context can be activated).
	UE_LOG(LogVaCuus, Verbose, TEXT("VaCuus widget received Slate focus (cause %d)"), int32(InFocusEvent.GetCause()));
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

	UE_LOG(LogVaCuus, Verbose, TEXT("VaCuus widget lost Slate focus (cause %d)"), int32(InFocusEvent.GetCause()));
}
