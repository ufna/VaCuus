// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Widgets/SLeafWidget.h"

class FVaCuusM1Harness;
class FVaCuusSlateElement;

/**
 * Full-viewport overlay widget hosting the M1 HUD. Tick drives the harness
 * (records a UI frame at the widget's current pixel size); OnPaint pushes the
 * window-space composite rect to the Slate element and injects the element
 * into the draw list via FSlateDrawElement::MakeCustom.
 *
 * Render-only in M1: hit-test invisible, zero desired size (it is added as a
 * viewport overlay that fills the screen).
 */
class SVaCuusHUDWidget : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SVaCuusHUDWidget)
	{
	}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs,
		const TSharedRef<FVaCuusM1Harness>& InHarness,
		const TSharedRef<FVaCuusSlateElement>& InElement);

	//~ Begin SWidget
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;
	//~ End SWidget

private:
	TSharedPtr<FVaCuusM1Harness> Harness;
	TSharedPtr<FVaCuusSlateElement> Element;
};
