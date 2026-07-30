// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "UObject/WeakObjectPtr.h"
#include "Widgets/SLeafWidget.h"

class FVaCuusSlateElement;
class UVaCuusView;
struct FVaCuusInputEvent;
struct FVaCuusInteractiveSnapshot;
struct FVaCuusModifierState;

/**
 * The Slate face of one VaCuus view: composites the UI thread's frames and routes
 * input into it.
 *
 * RENDER SIDE: Tick keeps the view's layout size in sync with the widget's pixel
 * rect (a queued Resize command -- never a direct RmlUi call); OnPaint pushes the
 * window-space composite rect to the Slate element and injects the element into the
 * draw list via FSlateDrawElement::MakeCustom. The UI thread publishes frames
 * asynchronously straight to the render thread, and the once-per-frame pulse belongs
 * to UVaCuusSubsystem, so Tick does no UI work beyond the resize check.
 *
 * INPUT SIDE -- the whole contract in three sentences: every handler converts to
 * view-space pixels, queues the event for the UI thread, and answers Slate
 * SYNCHRONOUSLY from the view's published interactive-region snapshot. The queue and
 * the answer are independent, and the event is queued whatever the FReply, because
 * the authoritative hit test (Context::GetElementAtPoint) lives on the UI thread and
 * may not be called from here at all. Unhandled is what lets a click, a wheel or a
 * key reach the game: it bubbles up to SViewport, which is an ancestor of the
 * viewport overlay (GameViewportClient.cpp:1326).
 *
 * WHY NOT NARROW THE HIT AREA INSTEAD: SWidget's only hook for that is
 * GetHitTestBoundingRect(), one rectangle. Multi-region pass-through has to be "stay
 * hit-test visible, test the point per event, return Unhandled" -- which is also why
 * OnCursorQuery must answer Unhandled over uncovered points, or the UI would own the
 * cursor shape across regions it deliberately does not own.
 *
 * The widget is EVisibility::Visible (it was HitTestInvisible while it was
 * render-only in Task 4) and SupportsKeyboardFocus() is true; without either, none
 * of the handlers below would ever be called.
 */
class SVaCuusWidget : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SVaCuusWidget)
	{
	}
	SLATE_END_ARGS()

	/**
	 * InView is a handle, not an owner: the subsystem owns the view and the console
	 * command calls DetachView() before destroying it. Held weakly, so a
	 * garbage-collected or invalidated view simply stops being driven.
	 */
	void Construct(const FArguments& InArgs,
		UVaCuusView* InView,
		const TSharedRef<FVaCuusSlateElement>& InElement);

	/** Teardown step 1: stop queueing commands so the view can be retired. */
	void DetachView();

	//~ Begin SWidget
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;

	//~ Input. Signatures are SWidget's (SLeafWidget declares no input virtuals of its
	//~ own); all of them are non-deprecated in 5.8.
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonDoubleClick(const FGeometry& InMyGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual void OnMouseEnter(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual void OnMouseLeave(const FPointerEvent& MouseEvent) override;
	virtual void OnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent) override;
	virtual FCursorReply OnCursorQuery(const FGeometry& MyGeometry, const FPointerEvent& CursorEvent) const override;
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent) override;
	virtual FReply OnFocusReceived(const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent) override;
	virtual void OnFocusLost(const FFocusEvent& InFocusEvent) override;
	//~ End SWidget

	/**
	 * Debug hook for headless verification (vacuus.M1HUD.Mouse): the geometry Slate
	 * last arranged this widget with, so a console command can synthesize a pointer
	 * event in screen space without guessing where the widget is.
	 */
	const FGeometry& GetCachedGeometry_Debug() const { return CachedInputGeometry; }

	/**
	 * Whether this widget believes it holds Slate's mouse capture.
	 *
	 * Exposed for VaCuus.Input.SlateRouting, which asserts the multi-button release
	 * rule against the bookkeeping and not only against the returned FReply -- the two
	 * can disagree, and a stuck flag makes every later mouse move answer Handled.
	 */
	bool IsTrackingMouseCapture_Debug() const { return bHasMouseCapture; }

private:
	/**
	 * Single source of truth for the widget's window-space pixel rect: Tick asks
	 * the view to lay out at this rect's SIZE and OnPaint composites into this
	 * exact rect, so RmlUi lays out 1:1 with the pixels it composites
	 * to. Corners are rounded individually — deriving the size separately
	 * (e.g. rounding LocalSize * Scale) can disagree with the rounded corner
	 * rect by a pixel when the widget origin is non-integer (PIE-in-viewport)
	 * and stretch the UI.
	 */
	static FIntRect ComputeWindowRect(const FGeometry& Geometry);

	/**
	 * Screen space -> view-space pixels, the space the snapshot's rects and RmlUi's
	 * context coordinates both use.
	 *
	 * AbsoluteToLocal gives Slate LOCAL units; multiplying by Geometry.Scale is what
	 * turns those into pixels -- the same two steps FWidget3DHitTester
	 * (WidgetComponent.cpp:175) and FCEFWebBrowserWindow::GetCefMouseEvent take.
	 * Floored, not rounded, because a pixel spans [n, n+1) and so does
	 * FIntRect::Contains.
	 *
	 * Sub-pixel caveat: ComputeWindowRect rounds the widget's bounding rect to whole
	 * pixels while this converts relative to the unrounded origin, so a non-integer
	 * widget origin (PIE-in-viewport) can make the two disagree by a pixel at the
	 * edges. Below what any pointer event can resolve.
	 */
	static FIntPoint ToViewPixels(const FGeometry& Geometry, const UE::Slate::FDeprecateVector2DResult& ScreenPosition);

	/** Modifier state as VaCuus records it; RmlUi's bit mask is built UI-thread-side. */
	static FVaCuusModifierState ToModifierState(const FInputEvent& Event);

	/**
	 * The view's newest published snapshot, or a permanently empty one when there is
	 * no view. Never fails and never blocks -- that is the whole point of it -- so every
	 * handler below can be a plain synchronous test.
	 */
	const FVaCuusInteractiveSnapshot& GetSnapshot() const;

	/** Queues one event for this view, if there still is one. */
	void SendInput(const FVaCuusInputEvent& Event);

	/** Services the vacuus.M1HUD.AutoShot debug screenshot on the game thread. */
	void TickAutoShot();

	/** Nulled by DetachView() so a late Tick is a no-op. */
	TWeakObjectPtr<UVaCuusView> View;

	TSharedPtr<FVaCuusSlateElement> Element;

	/**
	 * The geometry Slate last handed us, cached because two handlers need one and are
	 * not given one: OnMouseLeave takes no FGeometry (SWidget.h:430) and neither does
	 * OnFocusLost. Also what the debug mouse console command aims at.
	 */
	FGeometry CachedInputGeometry;

	/**
	 * True while we hold Slate's mouse capture. Capture is taken on a press over an
	 * interactive region so a drag that wanders off it keeps being delivered here, and
	 * released when the last button comes up. It is also why a drag's moves keep
	 * answering Handled after the pointer has left every rect.
	 */
	bool bHasMouseCapture = false;

	/**
	 * Pending UTF-16 high surrogate from a previous OnKeyChar, or 0.
	 *
	 * TCHAR is char16_t here (PLATFORM_TCHAR_IS_CHAR16 is 1 for Unix), so a code point
	 * above the BMP arrives as two calls. RmlUi wants one UTF-32 value and its `char`
	 * overload would silently drop the whole thing anyway (Context.cpp:553-557), so
	 * the halves are joined here rather than forwarded.
	 */
	uint32 PendingHighSurrogate = 0;

	bool bAutoShotDone = false;
};
