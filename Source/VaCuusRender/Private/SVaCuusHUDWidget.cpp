// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "SVaCuusHUDWidget.h"

#include "VaCuusDefines.h"
#include "VaCuusSlateElement.h"
#include "VaCuusStats.h"
#include "VaCuusUIThread.h"

#include "HAL/IConsoleManager.h"
#include "RenderingThread.h"
#include "Rendering/DrawElements.h"
#include "UnrealClient.h"

// Debug helper for headless verification: request a UI-inclusive screenshot
// once the UI thread has published N frames (0 = off). Set BEFORE toggling
// vacuus.M1HUD on, e.g. -ExecCmds="vacuus.M1HUD.AutoShot 10, vacuus.M1HUD".
static TAutoConsoleVariable<int32> CVarVaCuusM1HUDAutoShot(
	TEXT("vacuus.M1HUD.AutoShot"),
	0,
	TEXT("If > 0, request a screenshot (with UI) once the UI thread has completed this many frames."));

void SVaCuusHUDWidget::Construct(const FArguments& InArgs,
	FVaCuusUIThread* InUIThread,
	const TSharedRef<FVaCuusSlateElement>& InElement)
{
	UIThread = InUIThread;
	Element = InElement;

	SetCanTick(true);

	// The UI thread publishes a command buffer per frame that only a paint drains,
	// so the widget must repaint every frame even under Slate Global Invalidation —
	// volatility guarantees that cadence (the element still bounds the queue
	// defensively for any path this doesn't cover).
	ForceVolatile(true);

	// Render-only overlay: never eat input meant for the game underneath.
	SetVisibility(EVisibility::HitTestInvisible);
}

void SVaCuusHUDWidget::DetachUIThread()
{
	check(IsInGameThread());
	UIThread = nullptr;
}

void SVaCuusHUDWidget::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	if (UIThread == nullptr)
	{
		return;
	}

	// Resize is a command, not a direct call: Context::SetDimensions belongs to the
	// UI thread. Only sent on change, so the steady state costs nothing.
	const FIntPoint ViewSize = ComputeWindowRect(AllottedGeometry).Size();
	if (ViewSize != LastViewSize && ViewSize.X > 0 && ViewSize.Y > 0)
	{
		LastViewSize = ViewSize;
		UIThread->EnqueueResize(ViewSize);
	}

	// One coalescing pulse per game frame; never blocks. Task 4 moves this to
	// UVaCuusSubsystem::Tick, which is a better slot than a widget's Tick.
	UIThread->Trigger();

	TickAutoShot();
	FVaCuusPerfLog::TickLog();
}

void SVaCuusHUDWidget::TickAutoShot()
{
	const int32 AutoShotFrame = CVarVaCuusM1HUDAutoShot.GetValueOnGameThread();
	if (bAutoShotDone || AutoShotFrame <= 0)
	{
		return;
	}

	// Counted in UI-thread frames, not game frames: that is what guarantees the
	// document has actually been recorded and published by the time we shoot.
	const uint64 UIFrames = UIThread->GetFrameCount();
	if (UIFrames < uint64(AutoShotFrame))
	{
		return;
	}

	bAutoShotDone = true;
	UE_LOG(LogVaCuus, Log, TEXT("M1 HUD auto-screenshot after %llu UI frames"), UIFrames);
	FScreenshotRequest::RequestScreenshot(/*bInShowUI=*/true);
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
