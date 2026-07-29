// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "UObject/WeakObjectPtr.h"
#include "Widgets/SLeafWidget.h"

class FVaCuusSlateElement;
class UVaCuusView;

/**
 * Full-viewport overlay widget hosting the M1 HUD. Tick keeps the view's layout
 * size in sync with the widget's pixel rect (a queued Resize command -- never a
 * direct RmlUi call); OnPaint pushes the window-space composite rect to the Slate
 * element and injects the element into the draw list via
 * FSlateDrawElement::MakeCustom.
 *
 * The UI thread produces its frames asynchronously and publishes them straight to
 * the render thread, and the once-per-frame pulse belongs to UVaCuusSubsystem, so
 * Tick does no UI work of its own beyond the resize check.
 *
 * Render-only in M2 Task 4: hit-test invisible, zero desired size (it is added as
 * a viewport overlay that fills the screen). Input arrives in Task 6.
 */
class SVaCuusHUDWidget : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SVaCuusHUDWidget)
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
	//~ End SWidget

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

	/** Services the vacuus.M1HUD.AutoShot debug screenshot on the game thread. */
	void TickAutoShot();

	/** Nulled by DetachView() so a late Tick is a no-op. */
	TWeakObjectPtr<UVaCuusView> View;

	TSharedPtr<FVaCuusSlateElement> Element;

	bool bAutoShotDone = false;
};
