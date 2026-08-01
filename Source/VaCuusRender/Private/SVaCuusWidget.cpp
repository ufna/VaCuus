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
#include "Framework/Application/SlateUser.h"
#include "GenericPlatform/GenericWindow.h"
#include "HAL/IConsoleManager.h"
#include "Rendering/DrawElements.h"
#include "RenderingThread.h"
#include "UnrealClient.h"
#include "Widgets/SWindow.h"

// Debug helper for headless verification: request a UI-inclusive screenshot
// once the view has recorded N frames (0 = off). Set BEFORE toggling
// vacuus.M1HUD on, e.g. -ExecCmds="vacuus.M1HUD.AutoShot 10, vacuus.M1HUD".
static TAutoConsoleVariable<int32> CVarVaCuusM1HUDAutoShot(
	TEXT("vacuus.M1HUD.AutoShot"),
	0,
	TEXT("If > 0, request a screenshot (with UI) once the view has recorded this many UI frames."));

void SVaCuusWidget::Construct(const FArguments& InArgs,
	UVaCuusView* InView,
	const TSharedRef<FVaCuusSlateElement>& InElement)
{
	View = InView;
	Element = InElement;

	SetCanTick(true);

	// VOLATILE SO THIS WIDGET PAINTS EVERY FRAME, and since the M2 Task 12 idle
	// short-circuit that cadence carries the whole UI rather than one frame of catch-up.
	//
	// READ THIS BEFORE DELETING THE CALL. The comment that used to sit here justified it with
	// "the UI thread publishes a command buffer per frame that only a paint drains", which is
	// now FALSE — a static HUD records ~13,300 frames a minute and publishes ONE, because
	// FVaCuusRecordingRenderInterface::EndFrameAndPublish withholds every frame that draws
	// what the render thread already has. Anyone who notices that and concludes the reason is
	// gone has it backwards: the withheld publishes are exactly why the per-view render target
	// is now the ONLY copy of an idle UI's pixels, and why the recorder will never resend them.
	// FVaCuusSlateElement::Draw_RenderThread is what puts them on screen with no buffer in
	// flight at all — its replay sits inside `if (PendingBuffers.Num() > 0)`, the composite
	// after it does not (VaCuusSlateElement.cpp:56-96).
	//
	// WHAT VOLATILITY BUYS, checked against the engine rather than asserted.
	// ForceVolatile(true) makes SWidget::IsVolatile() true, and that does two things:
	//  - the widget carries NeedsVolatilePaint (SWidget.cpp:878-881), which lands it in the
	//    invalidation root's volatile update list (SlateInvalidationWidgetList.h:508-510,
	//    drained into the post-update heap at SlateInvalidationRoot.cpp:1335-1345) and
	//    repaints it every frame (WidgetProxy.cpp:63-66) even under Slate Global
	//    Invalidation, which is also what keeps SetDestRect_RenderThread current;
	//  - its draw elements stay OUT of Slate's element cache: FSlateWindowElementList's
	//    bAllowCache is `... && !WidgetDrawStack.Top().bIsVolatile` (DrawElements.h:269) and
	//    that flag is exactly IsVolatile() || IsVolatileIndirectly() (DrawElements.cpp:195).
	//
	// AND WHAT IT DOES NOT BUY, said out loud because the tempting counter-argument is "prove
	// it breaks". It would not obviously break: a CACHED custom-drawer batch still reaches the
	// render thread every frame. AddCustomElement stores the drawer ON the batch
	// (ElementBatcher.cpp:3035-3046), a cached batch lives in FSlateCachedElementData::
	// CachedBatches (DrawElements.h:204-206, "used to redraw when no invalidation occurs"),
	// and AddCachedElements re-adds every cached batch each frame (ElementBatcher.cpp:578-580)
	// while skipping only the RE-BATCHING of lists with new data (:528-553). So the honest
	// claim is narrower than "the UI would disappear": dropping this call swaps a tested path
	// for an untested one on the only code path here whose failure mode is now PERMANENT
	// blankness rather than a one-frame glitch — and it saves nothing, because this is a
	// full-screen leaf whose sole draw element is that custom drawer, so there is no cached
	// vertex work to keep. The element also still bounds its queue defensively for any
	// tick-without-paint path neither of these covers.
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

	// CONTROLLER DECISION D18, AND IT MUST BE BEFORE THE RESET. ITextInputMethodSystem holds
	// the IME context by TSharedRef, so a context registered on focus keeps the platform
	// pointing at this widget's window -- and this widget is usually pulled out of the tree
	// immediately after this call. Waiting for a destructor is not an option: it can run frames
	// later, and an IME left mid-composition would call EndComposition() on a dead owner.
	if (UVaCuusView* ViewPtr = View.Get())
	{
		ViewPtr->DetachIme();
	}

	View.Reset();

	// The view is going away, so there is no document left to navigate: stop suppressing
	// Slate's own navigation immediately rather than waiting for a focus change that may
	// never come (the widget is usually pulled out of the viewport right after this).
	RestoreNavigationConfig();
}

void SVaCuusWidget::ReleaseOwnPointerCapture(const TCHAR* Reason)
{
	check(IsInGameThread());

	if (!FSlateApplication::IsInitialized())
	{
		return;
	}

	// Cheap gate first: HasMouseCapture() asks the whole application, so the per-user walk
	// below only runs on the rare teardown that really is mid-drag.
	if (!HasMouseCapture())
	{
		return;
	}

	const TSharedRef<const SWidget> Self = SharedThis(this);

	int32 NumReleased = 0;
	FSlateApplication::Get().ForEachUser(
		[&Self, &NumReleased](FSlateUser& User)
		{
			// DoesWidgetHaveAnyCapture compares the LAST widget of each of this user's
			// captor paths against us (SlateUser.cpp), which is exactly "this widget is
			// your captor" and nothing wider.
			if (User.DoesWidgetHaveAnyCapture(Self))
			{
				User.ReleaseAllCapture();
				++NumReleased;
			}
		});

	UE_LOG(LogVaCuus, Log, TEXT("VaCuus widget released pointer capture for %d user(s) (%s)"), NumReleased, Reason);
}

void SVaCuusWidget::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	{
		// The other half of the spec's game-thread budget (Task 14): the per-frame work a
		// HOSTED view costs, next to UVaCuusSubsystem::Tick's snapshot poll. The scope stops
		// before TickLog() below, which is the logger's own 5-second print -- inside it, the
		// print would be the max sample of every window that contained one.
		VACUUS_PERF_SCOPE(SlateTick);

		// Cached for the handlers Slate gives no geometry to (OnMouseLeave, OnFocusLost)
		// and for the debug mouse command. Kept even when the view is gone, so a late
		// query answers with the last real layout instead of an identity transform.
		CachedInputGeometry = AllottedGeometry;

		UVaCuusView* ViewPtr = View.Get();
		if (ViewPtr == nullptr)
		{
			// Unchanged: a detached widget stops driving the log too, so its frames do not
			// inflate the window's fps for a view that no longer exists.
			return;
		}

		// Resize is a command, not a direct call: Context::SetDimensions belongs to the
		// UI thread. The view itself drops unchanged sizes, so the steady state costs
		// nothing and a burst of resizes coalesces into one relayout.
		ViewPtr->Resize(ComputeWindowRect(AllottedGeometry).Size());

		// No trigger here: UVaCuusSubsystem::Tick is the once-per-frame pulse, which is
		// a better slot than a widget's Tick (and the only one that works for views
		// without a widget).

		// Focus first: the analog clock below is gated on the UI wanting the keyboard, and
		// this is what makes that condition stop being true.
		TickKeyboardFocusRelease();

		// The analog stick's repeat clock (D13). Here rather than in OnAnalogValueChanged
		// because a held stick stops producing events.
		TickAnalogNavigation(InCurrentTime);

		// The IME's coordinate basis (Task 9). Once per frame rather than on demand, because the
		// rect it publishes is what the OS candidate window is anchored to and a viewport can be
		// resized or dragged without any input reaching this widget at all.
		PushImeSurface();

		TickAutoShot();
	}

	// OUTSIDE the SlateTick scope: this is the logger's own 5-second window print, and a
	// sample that contained it would report the print as the frame's cost.
	FVaCuusPerfLog::TickLog();
}

void SVaCuusWidget::TickAutoShot()
{
	const int32 AutoShotFrame = CVarVaCuusM1HUDAutoShot.GetValueOnGameThread();
	if (bAutoShotDone || AutoShotFrame <= 0)
	{
		return;
	}

	// Counted in recorded UI frames for THIS view, not game frames: that is what
	// guarantees the document has actually been laid out and drawn by the time we
	// shoot. Recorded rather than published on purpose -- the idle short-circuit
	// (M2 Task 12) withholds the publish of a frame that draws what the render thread
	// already has, so a static HUD would stop the publish count dead at two or three
	// and a threshold above that would never be reached.
	const UVaCuusView* ViewPtr = View.Get();
	const uint64 RecordedFrames = ViewPtr ? ViewPtr->GetFramesRecorded() : 0;
	if (RecordedFrames < uint64(AutoShotFrame))
	{
		return;
	}

	bAutoShotDone = true;
	UE_LOG(LogVaCuus, Log, TEXT("M1 HUD auto-screenshot after %llu recorded UI frames"), RecordedFrames);
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

	// THE GAME-THREAD HDR MIRROR (M5 Exp-GLASS-HDR-DETECT's shipped answer). Under HDR
	// composite the elements texture carries no scene at all and the render thread has no
	// in-band way to know: bOutputIsHDRDisplay is reset false for the SDR elements pass
	// (SlateRHIRenderer.cpp:1069 sets bElementsTextureIsHDRDisplay = false after the HDR
	// batch). So the ONE fact glass needs — "is HDR output requested" — is read here,
	// game-side, and pushed with the rect it travels with anyway. Conservative on
	// purpose: the cvar being on disables glass even on an SDR display where composite
	// mode never engages, because a blur of a maybe-sceneless texture is worse than no
	// blur (spec §2(b): LDR-only).
	static const IConsoleVariable* HDROutputCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.HDR.EnableHDROutput"));
	const bool bGlassAllowed = HDROutputCVar == nullptr || HDROutputCVar->GetInt() == 0;

	ENQUEUE_RENDER_COMMAND(VaCuusSetDestRect)(
		[LocalElement = Element, DestRect, bGlassAllowed](FRHICommandListImmediate&)
		{
			LocalElement->SetDestRect_RenderThread(DestRect);
			LocalElement->SetGlassAllowed_RenderThread(bGlassAllowed);
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

void SVaCuusWidget::PushImeSurface(TOptional<bool> bFocusOverride)
{
	check(IsInGameThread());

	UVaCuusView* ViewPtr = View.Get();
	if (ViewPtr == nullptr || !FSlateApplication::IsInitialized())
	{
		return;
	}

	FSlateApplication& Slate = FSlateApplication::Get();

	FVaCuusImeSurface Surface;

	// NULL ON THIS PLATFORM, AND THAT IS THE POINT (controller decision D16): only
	// FWindowsApplication and FMacApplication override
	// GenericApplication::GetTextInputMethodSystem(); FLinuxApplication does not, so this is
	// null here and every IME path downstream no-ops while typing keeps working through
	// OnKeyChar. The handler logs the difference once so it is visible rather than mysterious.
	Surface.TextInputMethodSystem = Slate.GetTextInputMethodSystem();

	if (const TSharedPtr<SWindow> Window = Slate.FindWidgetWindow(SharedThis(this)))
	{
		// ITextInputMethodContext::GetWindow wants the PLATFORM window, not the Slate one:
		// TSF attaches its document manager to an HWND.
		Surface.NativeWindow = Window->GetNativeWindow();
	}

	// THE SAME RECT THE RENDER PATH COMPOSITES INTO, deliberately: ComputeWindowRect is built
	// from GetRenderBoundingRect too, so the caret's mapping and the pixels it is drawn over
	// cannot disagree. GetRenderBoundingRect is already in Slate ABSOLUTE space, which is
	// exactly what GetTextBounds/GetScreenBounds are specified in -- no extra transform, and
	// no FVector2f/FVector2D narrowing to get wrong.
	const FSlateRect Bounds = CachedInputGeometry.GetRenderBoundingRect();
	Surface.AbsolutePosition = FVector2D(Bounds.Left, Bounds.Top);
	Surface.AbsoluteSize = FVector2D(Bounds.Right - Bounds.Left, Bounds.Bottom - Bounds.Top);
	Surface.ViewPixelSize = ComputeWindowRect(CachedInputGeometry).Size();

	// Slate focus, not RmlUi focus: keys travel Slate's focus path, so a platform IME context
	// activated while another widget owns focus is pointed at a field nothing can type into.
	Surface.bHostHasFocus = bFocusOverride.IsSet() ? bFocusOverride.GetValue() : HasAnyUserFocus().IsSet();

	ViewPtr->UpdateIme(Surface);
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
	// THE "input" HALF OF THE SPEC'S GAME-THREAD BUDGET (Task 14), and every handler below
	// carries the same scope. ONE SAMPLE PER EVENT, covering the whole handler rather than
	// only the enqueue: the transform, the snapshot scan that produces the FReply and the
	// queue push are all game-thread cost the budget is stated against, and the scan is the
	// part that grows with a document's rect count.
	//
	// Note what a sample here is NOT: a per-frame figure. A frame with four events pays four
	// of these, so the budget arithmetic is GameTick + SlateTick + (events x Input) -- which
	// is why the three are separate scopes.
	VACUUS_PERF_SCOPE(Input);

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
	VACUUS_PERF_SCOPE(Input);

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

		// Remembered so Tick can give the focus BACK when the UI stops wanting it (see
		// TickKeyboardFocusRelease). Only focus this widget asked for is ever released;
		// focus a game handed us deliberately -- because it opened a menu -- is that
		// game's to take away again.
		bSelfRequestedUserFocus = true;
	}

	// CONTROLLER DECISION D14a: the platform IME context is activated on THIS click, from the
	// rect flags, and not from the view-level bTextInputFocused that will confirm it a frame
	// later. Same asymmetry the focus branch above exploits -- the geometry already knows the
	// rect takes text, while "a field HAS the caret" cannot be true until the UI thread has
	// processed the press we just queued. Waiting costs the player's first composition,
	// silently; the D11 bug in a different coat.
	//
	// The surface is pushed first so the handler has a window and a rect to register with: a
	// press can be the first thing that ever happens to a freshly built widget.
	if (Snapshot.IsTextInputAt(Position))
	{
		PushImeSurface();

		if (UVaCuusView* ViewPtr = View.Get())
		{
			ViewPtr->NotifyImeTextInputClicked();
		}
	}

	return Reply;
}

FReply SVaCuusWidget::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	VACUUS_PERF_SCOPE(Input);

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
	VACUUS_PERF_SCOPE(Input);

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
	VACUUS_PERF_SCOPE(Input);

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

/**
 * Which navigation direction this key means to RmlUi, or None if it is not one.
 *
 * THE ARROWS AND THE DPAD ONLY, and that pairing is VaCuusInputMap's, not a guess: the four
 * DPad names are mapped onto KI_UP/DOWN/LEFT/RIGHT there, which is what makes them reach
 * ElementDocument's arrow branch at all. The LEFT STICK's digital names are deliberately not
 * here -- see the exclusion note in DoesKeyEnterUIFocus.
 */
static EVaCuusNavDirection KeyToNavDirection(const FKey& Key)
{
	if (Key == EKeys::Up || Key == EKeys::Gamepad_DPad_Up)
	{
		return EVaCuusNavDirection::Up;
	}
	if (Key == EKeys::Down || Key == EKeys::Gamepad_DPad_Down)
	{
		return EVaCuusNavDirection::Down;
	}
	if (Key == EKeys::Left || Key == EKeys::Gamepad_DPad_Left)
	{
		return EVaCuusNavDirection::Left;
	}
	if (Key == EKeys::Right || Key == EKeys::Gamepad_DPad_Right)
	{
		return EVaCuusNavDirection::Right;
	}

	return EVaCuusNavDirection::None;
}

bool SVaCuusWidget::DoesKeyEnterUIFocus(const FKey& Key, const FVaCuusInteractiveSnapshot& Snapshot)
{
	// TASK 14 ACCEPTANCE DECISION A1, and the whole of it is here.
	//
	// THE BEHAVIOUR BEING CHOSEN: the press that moves RmlUi's focus INTO a document is
	// consumed, so the game does not also see it. Before this, the entering press was
	// answered from bWantsKeyboardFocus alone -- which is false until the frame AFTER the
	// press has landed -- so it was Unhandled, bubbled to SViewport, and a player who
	// opened a pad-driven menu and pressed a direction got both the menu highlight moving
	// and their character stepping. Every press after the first was already consumed, so
	// the old behaviour was not even self-consistent.
	//
	// WHY CONSUMING IS SAFE, which is the half that is easy to get wrong: this handler only
	// runs when THIS WIDGET HOLDS SLATE KEYBOARD FOCUS -- FSlateApplication routes key
	// events along the focus path and nothing else. Focus gets here two ways: a click on a
	// focusable rect (OnMouseButtonDown's SetUserFocus), or the game putting it here
	// deliberately because it just opened a menu. In both cases somebody has already
	// decided the UI is the keyboard's target, so there is no third party whose direction
	// key we could be stealing. The cost the alternative feared -- "a swallowed first input
	// in the other direction" -- is a swallowed key in a state the game itself asked for.
	//
	// WHY IT IS GATED ON THE PUBLISHED FACTS RATHER THAN ON "a document is up": if the UI
	// cannot act on the key, consuming it costs the player an input and buys nothing.
	// bTabEntersFocus and DirectionsEnteringFocus are exactly RmlUi's own preconditions, read
	// off the frame the UI thread published; see their comments.
	if (Key == EKeys::Tab)
	{
		return Snapshot.bTabEntersFocus;
	}

	// PER DIRECTION, not per "is this an arrow at all": RmlUi answers per direction, because
	// `nav: vertical` on a document moves focus for Up/Down and provably nothing for
	// Left/Right (ElementDocument.cpp:787-794). Asking the coarse question is what used to
	// eat a vertical menu's Left and Right and give the player nothing back.
	if (const EVaCuusNavDirection Direction = KeyToNavDirection(Key); Direction != EVaCuusNavDirection::None)
	{
		return Snapshot.DoesDirectionEnterFocus(Direction);
	}

	// THE LEFT-STICK KEYS ARE DELIBERATELY ABSENT, and this is the one exclusion that
	// carries a bug with it if it is undone. Gamepad_LeftStick_Up/Down/Left/Right are not a
	// second DPad: they are what a HELD MOVEMENT STICK produces on the platforms that
	// digitize the axes -- FLinuxApplication raises OnControllerButtonPressed for
	// FGamepadKeyNames::LeftStickUp when the axis crosses its own dead zone
	// (Linux/LinuxApplication.cpp:626-632, released at :634-638), and XInput maps
	// Buttons[16..19] onto the same four names (XInputDevice/XInputInterface.cpp:100-103).
	// Controller decision D13 already answered "may a walking player's stick enter the UI"
	// with NO -- that is what the gate in TickAnalogNavigation is -- and letting these keys
	// enter here would answer YES through a different door, 0.0 s instead of 0.4 s after
	// the player started walking. Entering the UI is the DPad's job, the arrows', Tab's, or
	// the game's by focusing this widget.
	//
	// ACTIVATION KEYS ARE ABSENT FOR A DIFFERENT AND STRONGER REASON: they provably do
	// nothing in this state. Return/NumpadEnter/Space resolve GetFocusLeafNode(), which
	// returns the document itself when no child holds focus (Element.cpp:879-885), and then
	// click it only if THAT element has `tab-index: auto` (ElementDocument.cpp:641-650) --
	// which a document does not, since tab-index defaults to none and is not inherited
	// (StyleSheetSpecification.cpp:375). So an Enter with nothing focused clicks nothing,
	// and consuming it would eat the player's jump or fire to achieve exactly that. Once
	// something focusable IS focused, bWantsKeyboardFocus consumes them, which is the
	// existing rule and the right one.
	return false;
}

FReply SVaCuusWidget::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	VACUUS_PERF_SCOPE(Input);

	const FKey Key = InKeyEvent.GetKey();

	// The pass-through set (controller decision D12): not consumed AND not queued, so
	// the document never even hears about it. Checked first, before anything else can
	// have an opinion.
	if (PassThroughKeys.Contains(Key))
	{
		return FReply::Unhandled();
	}

	// Answered from the snapshot exactly like pointer events are: keys are consumed
	// only while a real focusable element holds RmlUi focus, OR while this key is the one
	// that would give it focus (Task 14 decision A1, see DoesKeyEnterUIFocus). Otherwise
	// they bubble on -- so a document that merely happens to hold Slate focus cannot
	// swallow the game's movement keys.
	//
	// Note this is a per-view verdict, not a per-key one, and it cannot be anything
	// else: RmlUi's own "was it consumed" answer is produced on the UI thread, frames
	// later in queue terms, and Slate needs an answer now. That asymmetry is precisely
	// why the pass-through set above exists as a declared contract. The entry rule is the
	// one case where the game thread can PREDICT the UI's answer instead of guessing it,
	// because both of RmlUi's preconditions are published facts.
	const FVaCuusInteractiveSnapshot& Snapshot = GetSnapshot();
	const bool bConsumeKeys = Snapshot.bWantsKeyboardFocus || DoesKeyEnterUIFocus(Key, Snapshot);
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
	VACUUS_PERF_SCOPE(Input);

	const FKey Key = InKeyEvent.GetKey();

	if (PassThroughKeys.Contains(Key))
	{
		return FReply::Unhandled();
	}

	// THE SAME VERDICT AS THE PRESS, and it has to be: the snapshot the game thread holds
	// is stable for the whole frame and the entering press has not been drained yet, so
	// both halves of one key see the same answer. If the release were answered from
	// bWantsKeyboardFocus alone it would fall through while its press did not, and the game
	// would see a key release it never saw pressed -- which is how a game ends up believing
	// a direction is held forever.
	const FVaCuusInteractiveSnapshot& Snapshot = GetSnapshot();
	const bool bConsumeKeys = Snapshot.bWantsKeyboardFocus || DoesKeyEnterUIFocus(Key, Snapshot);
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
	VACUUS_PERF_SCOPE(Input);

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
	VACUUS_PERF_SCOPE(Input);

	const FKey Key = InAnalogInputEvent.GetKey();
	const float Value = InAnalogInputEvent.GetAnalogValue();

	// SCOPE NOTE: InAnalogInputEvent.GetUserIndex() is deliberately ignored, so every
	// connected controller writes the same two axes and the last one to move wins. That
	// is the right trade for a single-player dev HUD and wrong for split-screen or any
	// couch-multiplayer UI, where each local player needs their own view, their own
	// snapshot and their own stick state. The fix when it is needed is per-user analog
	// state keyed on GetUserIndex() (and a per-user view), not a filter here -- filtering
	// to user 0 would silently ignore player two rather than serve them.

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
	// GATED ON THE UI ACTUALLY OWNING THE KEYBOARD, and this gate is not symmetry for
	// its own sake -- without it the repeat clock eats the player's movement stick.
	//
	// The chain: Slate never takes focus away from a widget on its own, so after any
	// single click on the HUD this widget keeps receiving analog events forever. A player
	// then simply holding the left stick to walk would be past the dead zone, the
	// throttle would synthesize a direction key, and RmlUi treats the first direction key
	// delivered while the DOCUMENT holds focus as "enter the grid"
	// (FindNextNavigationElement short-cuts `current_element == this` to tab order,
	// ElementDocument.cpp:798-799). From that moment bWantsKeyboardFocus is true, every
	// later stick event is answered Handled, and the player's movement input is silently
	// eaten -- 0.4 s after they started walking.
	//
	// So the stick may only navigate a UI that already owns the keyboard. Entering the UI
	// is the DPad's job (OnKeyDown enqueues regardless), or the game's, by focusing this
	// widget deliberately. The other half of the fix is in Tick: focus this widget took
	// itself is released again as soon as the view stops wanting the keyboard.
	if (!GetSnapshot().bWantsKeyboardFocus)
	{
		// Reset, not just skip: otherwise a stick held across the transition would count
		// as "already held" and its first press after the UI regains focus would be
		// swallowed as a repeat that is not due yet.
		HeldAnalogDirection = EAnalogNavDirection::None;
		return;
	}

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

void SVaCuusWidget::TickKeyboardFocusRelease()
{
	// Edge-triggered on the view's own answer, not level-triggered: this must fire once,
	// on the frame the UI stops wanting the keyboard, and then stay quiet -- otherwise it
	// would fight anything that focuses this widget while no element is focused, which is
	// exactly how a pad-driven menu is opened.
	const bool bWantsKeyboard = GetSnapshot().bWantsKeyboardFocus;
	const bool bLost = bLastWantedKeyboardFocus && !bWantsKeyboard;
	bLastWantedKeyboardFocus = bWantsKeyboard;

	if (!bLost || !bSelfRequestedUserFocus || !FSlateApplication::IsInitialized())
	{
		return;
	}

	// WHY RELEASE AT ALL: Slate never takes focus away from a widget on its own, so
	// without this a single click on the HUD leaves this widget holding the focus path
	// forever -- which keeps FNullNavigationConfig installed (so the rest of the
	// application loses arrow-key navigation) and keeps every key and analog event coming
	// here to be answered Unhandled one at a time. Blurring the last focused element (the
	// pad's Back, or a script) is the player saying they are done with the UI.
	FSlateApplication& Slate = FSlateApplication::Get();

	// Only the users whose focus is actually on THIS widget. ForEachUser rather than
	// HasAnyUserFocus() + ClearAllUserFocus(), because the latter would also drop focus
	// for users who are looking at something else entirely.
	TArray<int32, TInlineAllocator<4>> UsersToRelease;
	Slate.ForEachUser(
		[this, &UsersToRelease](FSlateUser& User)
		{
			if (User.GetFocusedWidget().Get() == this)
			{
				UsersToRelease.Add(User.GetUserIndex());
			}
		});

	if (UsersToRelease.IsEmpty())
	{
		// Focus already moved on by itself; nothing to hand back, and nothing to remember.
		bSelfRequestedUserFocus = false;
		return;
	}

	// HANDED TO THE GAME VIEWPORT, not cleared. ClearUserFocus() leaves the user with an
	// empty focus path, and FSlateApplication routes keys ALONG the focus path
	// (ProcessKeyDownEvent -> SlateUser->GetFocusPath()), so the game viewport would stop
	// receiving keys altogether -- the same class of bug as the one this fix is for.
	// SetUserFocusToGameViewport is the engine's own idiom for this
	// (UGameViewportClient.cpp:3354); it no-ops when there is no game viewport, which is
	// why the clear below is the fallback rather than the default.
	const bool bHasGameViewport = Slate.GetGameViewport().IsValid();
	for (const int32 UserIndex : UsersToRelease)
	{
		if (bHasGameViewport)
		{
			Slate.SetUserFocusToGameViewport(UserIndex, EFocusCause::SetDirectly);
		}
		else
		{
			Slate.ClearUserFocus(UserIndex, EFocusCause::SetDirectly);
		}
	}

	// OnFocusLost has run by now and cleared the flag; this is belt and braces for the
	// case where the focus change did not produce one (no game viewport, no other widget).
	bSelfRequestedUserFocus = false;

	UE_LOG(LogVaCuus, Verbose,
		TEXT("VaCuus widget released the Slate focus it took (%d user(s)) because the view stopped wanting the keyboard; ")
		TEXT("focus went to %s"),
		UsersToRelease.Num(), bHasGameViewport ? TEXT("the game viewport") : TEXT("nothing (no game viewport)"));
}

void SVaCuusWidget::OverrideNavigationConfig(int32 UserIndex)
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

	// KNOWN LIMITATION -- SetNavigationConfig() only replaces the GLOBAL config, while
	// GetNavigationDirectionFromKey/FromAnalog resolve through
	// FSlateApplication::GetRelevantNavConfig(UserIndex), which prefers a config
	// registered for that specific user and, in editor builds, substitutes
	// EditorNavigationConfig for widget paths outside AllGameViewports. So an application
	// that registers per-user navigation configs (CommonUI does) or a document hosted in
	// an editor utility widget can still have its arrows eaten before OnKeyDown, and our
	// save/restore would not notice.
	//
	// Detected rather than assumed away: the divergence is logged here so a "the pad does
	// nothing in this one context" report has a first line to check, instead of looking
	// like a VaCuus bug. Widening the fix (per-user overrides, or an IInputProcessor that
	// intercepts before navigation runs at all) is deferred until something actually needs
	// it -- both cost more global state than this milestone can justify.
	//
	// PROBED BEHAVIOURALLY, not by comparing config pointers: GetRelevantNavConfig itself
	// is protected, and the question that matters is not "is our object installed" but
	// "will Slate still turn an arrow into navigation for this user". Asking
	// GetNavigationDirectionFromKey answers exactly that -- it resolves through
	// GetRelevantNavConfig(InKeyEvent.GetUserIndex()), the same call the real key path
	// makes, and the analog path resolves identically. Anything other than Invalid means
	// some other config is winning.
	const FKeyEvent ProbeEvent(EKeys::Right, FModifierKeysState(), uint32(UserIndex), /*bIsRepeat=*/false,
		/*CharacterCode=*/0, /*KeyCode=*/0);
	if (const EUINavigation ProbeDirection = Slate.GetNavigationDirectionFromKey(ProbeEvent);
		ProbeDirection != EUINavigation::Invalid)
	{
		UE_LOG(LogVaCuus, Verbose,
			TEXT("VaCuus widget: the effective navigation config for user %d still resolves the Right arrow to ")
			TEXT("navigation (%d) despite our override -- a per-user or editor config takes precedence over the ")
			TEXT("global one, so Slate may consume arrows and the stick before OnKeyDown"),
			UserIndex, int32(ProbeDirection));
	}
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
	// click that brought us here. This exists so the transition is observable, and so the
	// IME learns that keys now reach us.
	UE_LOG(LogVaCuus, Verbose, TEXT("VaCuus widget received Slate focus (cause %d)"), int32(InFocusEvent.GetCause()));

	// Task 9: bHostHasFocus is what gates activation, so it has to be republished on the edge
	// rather than waited for until the next Tick -- a pad-driven menu can focus this widget and
	// have a field focused inside the document in the same frame.
	PushImeSurface(/*bFocusOverride=*/true);

	// Controller decision D12: while we own the keyboard, Slate must stop eating
	// directions before OnKeyDown sees them. The user index is passed through only for
	// the per-user divergence check documented in OverrideNavigationConfig.
	OverrideNavigationConfig(int32(InFocusEvent.GetUser()));
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

	// Whatever focus we were holding is gone, so there is nothing left for
	// TickKeyboardFocusRelease to hand back.
	bSelfRequestedUserFocus = false;

	// Hand navigation back. Not conditional on anything: whoever has focus now owns
	// arrow keys, and it is not us.
	RestoreNavigationConfig();

	// Task 9: the platform IME context is DEACTIVATED here (through the republished
	// bHostHasFocus == false), and deliberately not unregistered -- the field keeps its caret
	// and selection in RmlUi, exactly as the comment above says, so returning to the widget
	// resumes composing where the player stopped. Unregistration belongs to teardown (D18).
	PushImeSurface(/*bFocusOverride=*/false);

	UE_LOG(LogVaCuus, Verbose, TEXT("VaCuus widget lost Slate focus (cause %d)"), int32(InFocusEvent.GetCause()));
}
