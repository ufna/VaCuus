// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "SVaCuusHUDWidget.h"

#include "VaCuusM1Harness.h"
#include "VaCuusSlateElement.h"

#include "RenderingThread.h"
#include "Rendering/DrawElements.h"

void SVaCuusHUDWidget::Construct(const FArguments& InArgs,
	const TSharedRef<FVaCuusM1Harness>& InHarness,
	const TSharedRef<FVaCuusSlateElement>& InElement)
{
	Harness = InHarness;
	Element = InElement;

	SetCanTick(true);

	// Render-only overlay: never eat input meant for the game underneath.
	SetVisibility(EVisibility::HitTestInvisible);
}

void SVaCuusHUDWidget::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	// Record this frame at the widget's on-screen pixel size (layout units x
	// DPI scale); OnPaint composites at exactly this rect, so RmlUi lays out
	// 1:1 with the pixels it ends up on.
	const FVector2D PixelSize = AllottedGeometry.GetLocalSize() * AllottedGeometry.Scale;
	Harness->DrawFrame(FIntPoint(FMath::RoundToInt(PixelSize.X), FMath::RoundToInt(PixelSize.Y)));
}

int32 SVaCuusHUDWidget::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId,
	const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	// Window-space pixel rect of the widget. The element applies the
	// elements-texture offset render-side (FDrawPassInputs::ElementsOffset),
	// mirroring the Slate blur pass convention.
	const FSlateRect BoundingRect = AllottedGeometry.GetRenderBoundingRect();
	const FIntRect DestRect(
		FMath::RoundToInt(BoundingRect.Left), FMath::RoundToInt(BoundingRect.Top),
		FMath::RoundToInt(BoundingRect.Right), FMath::RoundToInt(BoundingRect.Bottom));

	ENQUEUE_RENDER_COMMAND(VaCuusSetDestRect)(
		[LocalElement = Element, DestRect](FRHICommandListImmediate&)
		{
			LocalElement->SetDestRect_RenderThread(DestRect);
		});

	FSlateDrawElement::MakeCustom(OutDrawElements, LayerId, Element);
	return LayerId;
}

FVector2D SVaCuusHUDWidget::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	// Viewport overlay stretched to fill the screen; the widget asks for nothing.
	return FVector2D::ZeroVector;
}
