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

	// Every Tick publishes a command buffer that only a paint drains, so the
	// widget must repaint every frame even under Slate Global Invalidation —
	// volatility guarantees that 1:1 cadence (the element still bounds the
	// queue defensively for any path this doesn't cover).
	ForceVolatile(true);

	// Render-only overlay: never eat input meant for the game underneath.
	SetVisibility(EVisibility::HitTestInvisible);
}

void SVaCuusHUDWidget::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	Harness->DrawFrame(ComputeWindowRect(AllottedGeometry).Size());
}

int32 SVaCuusHUDWidget::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
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

FVector2D SVaCuusHUDWidget::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	// Viewport overlay stretched to fill the screen; the widget asks for nothing.
	return FVector2D::ZeroVector;
}

FIntRect SVaCuusHUDWidget::ComputeWindowRect(const FGeometry& Geometry)
{
	const FSlateRect BoundingRect = Geometry.GetRenderBoundingRect();
	return FIntRect(
		FMath::RoundToInt(BoundingRect.Left), FMath::RoundToInt(BoundingRect.Top),
		FMath::RoundToInt(BoundingRect.Right), FMath::RoundToInt(BoundingRect.Bottom));
}
