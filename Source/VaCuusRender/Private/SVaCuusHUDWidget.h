// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Widgets/SLeafWidget.h"

class FVaCuusSlateElement;
class FVaCuusUIThread;

/**
 * Full-viewport overlay widget hosting the M1 HUD. Tick drives the UI thread
 * (pushes the widget's current pixel size as a resize command when it changed,
 * then triggers one UI frame); OnPaint pushes the window-space composite rect to
 * the Slate element and injects the element into the draw list via
 * FSlateDrawElement::MakeCustom.
 *
 * The UI thread produces its frames asynchronously and publishes them straight to
 * the render thread, so Tick does no UI work of its own -- it is only the
 * once-per-game-frame pulse (Task 4 moves that pulse to UVaCuusSubsystem).
 *
 * Render-only in M2 Task 3: hit-test invisible, zero desired size (it is added as
 * a viewport overlay that fills the screen).
 */
class SVaCuusHUDWidget : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SVaCuusHUDWidget)
	{
	}
	SLATE_END_ARGS()

	/**
	 * InUIThread is borrowed, not owned: the console command's module state owns it
	 * and calls DetachUIThread() before destroying it.
	 */
	void Construct(const FArguments& InArgs,
		FVaCuusUIThread* InUIThread,
		const TSharedRef<FVaCuusSlateElement>& InElement);

	/** Teardown step 1: stop pulsing/queueing so the UI thread can be joined. */
	void DetachUIThread();

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
	 * the UI thread to lay out at this rect's SIZE and OnPaint composites into
	 * this exact rect, so RmlUi lays out 1:1 with the pixels it composites
	 * to. Corners are rounded individually — deriving the size separately
	 * (e.g. rounding LocalSize * Scale) can disagree with the rounded corner
	 * rect by a pixel when the widget origin is non-integer (PIE-in-viewport)
	 * and stretch the UI.
	 */
	static FIntRect ComputeWindowRect(const FGeometry& Geometry);

	/** Services the vacuus.M1HUD.AutoShot debug screenshot on the game thread. */
	void TickAutoShot();

	/** Borrowed; nulled by DetachUIThread() so a late Tick is a no-op. */
	FVaCuusUIThread* UIThread = nullptr;

	TSharedPtr<FVaCuusSlateElement> Element;

	/** Last size pushed to the UI thread; resize commands are only sent on change. */
	FIntPoint LastViewSize = FIntPoint::ZeroValue;

	bool bAutoShotDone = false;
};
