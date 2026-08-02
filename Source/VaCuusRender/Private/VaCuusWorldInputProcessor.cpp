// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusWorldInputProcessor.h"

#include "VaCuusDefines.h"
#include "VaCuusEngineCompat.h"
#include "VaCuusInputEvent.h"
#include "VaCuusStats.h"
#include "VaCuusView.h"
#include "VaCuusWorldComponent.h"
#include "VaCuusWorldSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Layout/WidgetPath.h"
#include "Widgets/SViewport.h"

// ---------------------------------------------------------------------------
// FVaCuusWorldHitMath
// ---------------------------------------------------------------------------

FVector2D FVaCuusWorldHitMath::LocalHitToWidget(
	const FTransform& ComponentTransform, const FVector& WorldHit, FIntPoint DrawSize, FVector2D Pivot)
{
	// UWidgetComponent::GetLocalHitLocation (WidgetComponent.cpp:2036-2054), plane
	// branch, line for line; the header explains the omitted no-op tail. The scale
	// division inside InverseTransformPosition is also why a world-scaled panel needs
	// no pixel correction: the quad's world size scales with the component, so the
	// local-space hit is back in pixel units regardless of scale.
	const FVector ComponentHitLocation = ComponentTransform.InverseTransformPosition(WorldHit);

	FVector2D OutWidget(-ComponentHitLocation.Y, -ComponentHitLocation.Z);
	OutWidget.X += DrawSize.X * Pivot.X;
	OutWidget.Y += DrawSize.Y * Pivot.Y;
	return OutWidget;
}

bool FVaCuusWorldHitMath::IsFrontFacing(const FVector& ComponentForward, const FVector& ImpactPoint, const FVector& TraceStart)
{
	// WidgetComponent.cpp:186-189: the trace must have arrived AGAINST the quad's +X
	// normal, i.e. from the face the UI is drawn on.
	return FVector::DotProduct(ComponentForward, ImpactPoint - TraceStart) < 0.0f;
}

bool FVaCuusWorldHitMath::RayToWidget(const FTransform& ComponentTransform, const FVector& RayOrigin,
	const FVector& RayDirection, FIntPoint DrawSize, FVector2D Pivot, FVector2D& OutWidget)
{
	// Into component space, where the quad's plane is exactly X = 0 (every proxy
	// vertex, VaCuusWorldComponent.cpp:99-102). InverseTransformVector carries the
	// component's (possibly non-uniform) scale into the direction, so the parameter t
	// below is consistent with the transformed origin.
	const FVector LocalOrigin = ComponentTransform.InverseTransformPosition(RayOrigin);
	const FVector LocalDirection = ComponentTransform.InverseTransformVector(RayDirection);

	if (FMath::IsNearlyZero(LocalDirection.X))
	{
		// Parallel to the plane: no intersection worth inventing.
		return false;
	}

	const double T = -LocalOrigin.X / LocalDirection.X;
	if (T < 0.0)
	{
		// The plane is behind the ray: the camera turned past the panel mid-drag.
		return false;
	}

	const FVector LocalHit = LocalOrigin + LocalDirection * T;
	OutWidget = FVector2D(-LocalHit.Y, -LocalHit.Z);
	OutWidget.X += DrawSize.X * Pivot.X;
	OutWidget.Y += DrawSize.Y * Pivot.Y;
	return true;
}

// ---------------------------------------------------------------------------
// Install refcount
// ---------------------------------------------------------------------------

namespace VaCuusWorldInput
{
static int32 GInstallRefCount = 0;
static TSharedPtr<FVaCuusWorldInputProcessor> GProcessor;

/** FInputEvent's five observable modifiers; the same conversion the Slate widget makes (SVaCuusWidget.cpp:371-380). */
static FVaCuusModifierState ToModifiers(const FInputEvent& Event)
{
	FVaCuusModifierState State;
	State.bControlDown = Event.IsControlDown();
	State.bShiftDown = Event.IsShiftDown();
	State.bAltDown = Event.IsAltDown();
	State.bCommandDown = Event.IsCommandDown();
	State.bCapsLock = Event.AreCapsLocked();
	return State;
}

/** The FWidget3DHitTester player resolution (WidgetComponent.cpp:171-173): local player 0's controller. */
static APlayerController* ResolvePlayerController(const UGameViewportClient* ViewportClient)
{
	UWorld* World = ViewportClient ? ViewportClient->GetWorld() : nullptr;
	ULocalPlayer* Player = (World && GEngine) ? GEngine->GetLocalPlayerFromControllerId(World, 0) : nullptr;
	return Player ? Player->PlayerController : nullptr;
}
}	 // namespace VaCuusWorldInput

void FVaCuusWorldInputProcessor::AddInstallRef()
{
	check(IsInGameThread());

	if (++VaCuusWorldInput::GInstallRefCount > 1)
	{
		return;
	}

	if (!FSlateApplication::IsInitialized())
	{
		// Dedicated server or commandlet: the refcount still pairs (so a later
		// release stays balanced) but there is no input pipeline to join.
		return;
	}

	VaCuusWorldInput::GProcessor = MakeShared<FVaCuusWorldInputProcessor>();

	// PreGame: after the Engine/Editor buckets so PIE editor shortcuts stay ahead of
	// us, before the Game bucket (SlateApplication.h:189-208 -- ascending order is
	// earlier evaluation; the CommonUI shape, CommonUIActionRouterBase.cpp:353-358).
	// Routed through the engine-version seam because the registration-key overload is
	// the newest of 5.8's four (VaCuusEngineCompat.h hotspot 2, M6 spec §2(f)).
	VaCuusCompat::RegisterInputPreProcessor_PreGame(FSlateApplication::Get(), VaCuusWorldInput::GProcessor);

	UE_LOG(LogVaCuus, Log, TEXT("World input processor installed (PreGame; first panel registered)"));
}

void FVaCuusWorldInputProcessor::ReleaseInstallRef()
{
	check(IsInGameThread());

	if (!ensureMsgf(VaCuusWorldInput::GInstallRefCount > 0, TEXT("World input processor ref over-released")))
	{
		return;
	}
	if (--VaCuusWorldInput::GInstallRefCount > 0)
	{
		return;
	}

	if (VaCuusWorldInput::GProcessor.IsValid())
	{
		// The IsInitialized guard: the last panel can unregister during application
		// teardown, after Slate is gone (the precedent's exact guard,
		// CommonUIActionRouterBase.cpp:367-380).
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().UnregisterInputPreProcessor(VaCuusWorldInput::GProcessor);
		}
		VaCuusWorldInput::GProcessor.Reset();
		UE_LOG(LogVaCuus, Log, TEXT("World input processor uninstalled (last panel unregistered)"));
	}
}

TSharedPtr<FVaCuusWorldInputProcessor> FVaCuusWorldInputProcessor::Get()
{
	return VaCuusWorldInput::GProcessor;
}

// ---------------------------------------------------------------------------
// The occlusion rule and the trace
// ---------------------------------------------------------------------------

FVaCuusWorldInputProcessor::FEngagement FVaCuusWorldInputProcessor::QueryEngagement(
	FSlateApplication& SlateApp, const FPointerEvent& Event)
{
	FEngagement Out;

	UGameViewportClient* ViewportClient = GEngine ? GEngine->GameViewport : nullptr;
	const TSharedPtr<SViewport> ViewportWidget = ViewportClient ? ViewportClient->GetGameViewportWidget() : nullptr;

	// (b), cached per (frame, position) -- the engine hit tester's key
	// (WidgetComponent.cpp:259-279). One hit-grid lookup when it misses; Slate's own
	// router repeats the identical query right after us either way.
	const FVector2D ScreenPos = Event.GetScreenSpacePosition();
	bool bTerminatesAtViewport = false;
	if (ViewportWidget.IsValid())
	{
		if (GFrameNumber != CachedPathFrame || CachedPathScreenPos != ScreenPos)
		{
			CachedPathFrame = GFrameNumber;
			CachedPathScreenPos = ScreenPos;

			const FWidgetPath Path = SlateApp.LocateWindowUnderMouse(
				ScreenPos, SlateApp.GetInteractiveTopLevelWindows(), /*bIgnoreEnabledStatus=*/false, Event.GetUserIndex());

			// DEEPEST-equals, not contains -- the header's argument: UMG-in-viewport
			// widgets are INSIDE the SViewport subtree, so containment would engage
			// under an overlaying button.
			bCachedPathTerminatesAtViewport = Path.IsValid() && &Path.GetLastWidget().Get() == ViewportWidget.Get();
			CachedViewportGeometry = bCachedPathTerminatesAtViewport ? Path.Widgets.Last().Geometry : FGeometry();
		}
		bTerminatesAtViewport = bCachedPathTerminatesAtViewport;
	}

	// (a) is deliberately OUTSIDE the cache: capture changes between events inside
	// one frame (the down we decline is what lets a widget take it).
	if (!ShouldEngage(SlateApp.HasAnyMouseCaptor(), bTerminatesAtViewport))
	{
		return Out;
	}

	Out.bEngaged = true;
	Out.ViewportGeometry = CachedViewportGeometry;
	Out.ViewportClient = ViewportClient;
	return Out;
}

FVaCuusWorldInputProcessor::FPanelHit FVaCuusWorldInputProcessor::ResolvePanelHit(
	const FEngagement& Engagement, const FPointerEvent& Event, bool bForceRetrace)
{
	FPanelHit Out;

	APlayerController* PlayerController = VaCuusWorldInput::ResolvePlayerController(Engagement.ViewportClient);
	if (PlayerController == nullptr)
	{
		return Out;
	}

	// Desktop -> viewport render pixels: AbsoluteToLocal then * Scale, against the
	// geometry the located path carried -- the FWidget3DHitTester conversion
	// (WidgetComponent.cpp:175).
	const FVector2D ScreenPos = Event.GetScreenSpacePosition();
	const FVector2D ViewportPos =
		FVector2D(Engagement.ViewportGeometry.AbsoluteToLocal(ScreenPos)) * Engagement.ViewportGeometry.Scale;

	// The trace, (frame, position)-cached like the engine's
	// (GetHitResultAtScreenPositionAndCache, WidgetComponent.cpp:259-279); button
	// events pass bForceRetrace and refuse the cache -- WS-STALE-RAY, priced in the
	// header. ECC_Visibility is the FWidget3DHitTester channel (:270): profile "UI"
	// blocks exactly it (BaseEngine.ini:3120), and anything else that blocks
	// Visibility occludes a panel by being hit first, which is the wanted semantics.
	if (bForceRetrace || GFrameNumber != CachedTraceFrame || CachedTraceScreenPos != ScreenPos)
	{
		CachedTraceFrame = GFrameNumber;
		CachedTraceScreenPos = ScreenPos;
		bCachedTraceHasHit =
			PlayerController->GetHitResultAtScreenPosition(ViewportPos, ECC_Visibility, /*bTraceComplex=*/true, CachedTraceHit);
	}

	if (!bCachedTraceHasHit)
	{
		return Out;
	}

	UPrimitiveComponent* HitComponent = CachedTraceHit.Component.Get();
	UWorld* World = Engagement.ViewportClient ? Engagement.ViewportClient->GetWorld() : nullptr;
	UVaCuusWorldSubsystem* Roster = World ? World->GetSubsystem<UVaCuusWorldSubsystem>() : nullptr;
	if (HitComponent == nullptr || Roster == nullptr)
	{
		return Out;
	}

	// Roster membership decides "is this a VaCuus panel", THEN the cast narrows the
	// type -- the roster is typed UPrimitiveComponent by module layering and the
	// render side casts (VaCuusWorldSubsystem.h:20-24).
	const bool bInRoster = Roster->GetWorldComponents().ContainsByPredicate(
		[HitComponent](const TWeakObjectPtr<UPrimitiveComponent>& Entry) { return Entry.Get() == HitComponent; });
	UVaCuusWorldComponent* Panel = bInRoster ? Cast<UVaCuusWorldComponent>(HitComponent) : nullptr;
	if (Panel == nullptr || Panel->GetView() == nullptr)
	{
		return Out;
	}

	if (!FVaCuusWorldHitMath::IsFrontFacing(Panel->GetForwardVector(), CachedTraceHit.ImpactPoint, CachedTraceHit.TraceStart))
	{
		return Out;
	}

	const FVector2D Widget = FVaCuusWorldHitMath::LocalHitToWidget(
		Panel->GetComponentTransform(), CachedTraceHit.ImpactPoint, Panel->GetCurrentDrawSize(), Panel->Pivot);

	Out.Component = Panel;
	Out.Pixel = FVaCuusWorldHitMath::WidgetToPixel(Widget);
	return Out;
}

bool FVaCuusWorldInputProcessor::ResolveLatchedPixel(
	const FPointerEvent& Event, UVaCuusWorldComponent* Panel, FIntPoint& OutPixel)
{
	UGameViewportClient* ViewportClient = GEngine ? GEngine->GameViewport : nullptr;
	const TSharedPtr<SViewport> ViewportWidget = ViewportClient ? ViewportClient->GetGameViewportWidget() : nullptr;
	APlayerController* PlayerController = VaCuusWorldInput::ResolvePlayerController(ViewportClient);
	if (!ViewportWidget.IsValid() || PlayerController == nullptr)
	{
		return false;
	}

	// While latched the cursor may be over ANY widget (that is what the latch is
	// for), so there is no located path to take geometry from; the viewport's
	// persistent cached geometry from its last paint serves instead.
	const FGeometry Geometry = ViewportWidget->GetCachedGeometry();
	const FVector2D ViewportPos = FVector2D(Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition())) * Geometry.Scale;

	// A fresh trace first: exact while the ray still crosses the panel's box, and it
	// keeps the latched release on WS-STALE-RAY's fresh-geometry footing.
	FHitResult Hit;
	if (PlayerController->GetHitResultAtScreenPosition(ViewportPos, ECC_Visibility, /*bTraceComplex=*/true, Hit) &&
		Hit.Component.Get() == Panel &&
		FVaCuusWorldHitMath::IsFrontFacing(Panel->GetForwardVector(), Hit.ImpactPoint, Hit.TraceStart))
	{
		OutPixel = FVaCuusWorldHitMath::WidgetToPixel(FVaCuusWorldHitMath::LocalHitToWidget(
			Panel->GetComponentTransform(), Hit.ImpactPoint, Panel->GetCurrentDrawSize(), Panel->Pivot));
		return true;
	}

	// Off the quad (or something now occludes it): project the ray onto the panel's
	// own unbounded plane -- the capture analog the header argues for.
	FVector RayOrigin, RayDirection;
	if (!PlayerController->DeprojectScreenPositionToWorld(ViewportPos.X, ViewportPos.Y, RayOrigin, RayDirection))
	{
		return false;
	}

	FVector2D Widget;
	if (!FVaCuusWorldHitMath::RayToWidget(
			Panel->GetComponentTransform(), RayOrigin, RayDirection, Panel->GetCurrentDrawSize(), Panel->Pivot, Widget))
	{
		return false;
	}

	OutPixel = FVaCuusWorldHitMath::WidgetToPixel(Widget);
	return true;
}

void FVaCuusWorldInputProcessor::SendLeaveIfHovering(const UVaCuusWorldComponent* ExceptFor)
{
	UVaCuusWorldComponent* Panel = HoveredPanel.Get();
	if (Panel == nullptr)
	{
		HoveredPanel = nullptr;
		return;
	}
	if (Panel == ExceptFor)
	{
		return;
	}

	// Mandatory, or `:hover` sticks forever -- the widget's OnMouseLeave contract
	// (SVaCuusWidget.cpp:591-597; Context.cpp:839-846). The next MouseMove re-arms
	// the context on its own.
	if (UVaCuusView* View = Panel->GetView())
	{
		View->SendInput(FVaCuusInputEvent::MouseLeave());
		++NumLeavesSent;
	}
	HoveredPanel = nullptr;
}

// ---------------------------------------------------------------------------
// IInputProcessor handlers
// ---------------------------------------------------------------------------

bool FVaCuusWorldInputProcessor::HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
	check(IsInGameThread());

	// The same per-EVENT budget scope as the Slate widget's handlers (SVaCuusWidget
	// .cpp:404-415's argument): the path query, the trace and the enqueue are all
	// game-thread input cost the M2 budget line is stated against.
	VACUUS_PERF_SCOPE(Input);

	if (bLatched)
	{
		UVaCuusWorldComponent* Panel = LatchedPanel.Get();
		if (Panel != nullptr && Panel->GetView() != nullptr)
		{
			FIntPoint Pixel;
			if (ResolveLatchedPixel(MouseEvent, Panel, Pixel))
			{
				Panel->GetView()->SendInput(FVaCuusInputEvent::MouseMove(Pixel, VaCuusWorldInput::ToModifiers(MouseEvent)));
			}
			// Consumed regardless of coverage while latched -- the widget's capture
			// rule (SVaCuusWidget.cpp:421-428): a drag that started on a scrollbar
			// keeps being ours after the pointer wanders off it.
			++NumConsumed;
			return true;
		}
		// The panel died mid-drag (level streaming, Destroy): nothing to release to.
		ClearLatch();
	}

	const FEngagement Engagement = QueryEngagement(SlateApp, MouseEvent);
	if (!Engagement.bEngaged)
	{
		SendLeaveIfHovering();
		++NumDeferredToSlate;
		return false;
	}

	const FPanelHit Hit = ResolvePanelHit(Engagement, MouseEvent, /*bForceRetrace=*/false);
	if (Hit.Component == nullptr)
	{
		SendLeaveIfHovering();
		++NumPassedToGame;
		return false;
	}

	// Crossing directly from one panel to another leaves the old one first.
	SendLeaveIfHovering(Hit.Component);
	HoveredPanel = Hit.Component;

	UVaCuusView* View = Hit.Component->GetView();
	View->SendInput(FVaCuusInputEvent::MouseMove(Hit.Pixel, VaCuusWorldInput::ToModifiers(MouseEvent)));

	if (View->GetSnapshot().Contains(Hit.Pixel))
	{
		++NumConsumed;
		return true;
	}
	++NumPassedToGame;
	return false;
}

bool FVaCuusWorldInputProcessor::HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
	check(IsInGameThread());
	VACUUS_PERF_SCOPE(Input);

	if (bLatched)
	{
		UVaCuusWorldComponent* Panel = LatchedPanel.Get();
		if (Panel != nullptr && Panel->GetView() != nullptr)
		{
			// A second button pressed mid-drag is part of the gesture: forwarded and
			// consumed; the latch already owns the stream until the LAST release.
			FIntPoint Pixel;
			if (ResolveLatchedPixel(MouseEvent, Panel, Pixel))
			{
				Panel->GetView()->SendInput(FVaCuusInputEvent::MouseButton(
					/*bDown=*/true, Pixel, MouseEvent.GetEffectingButton(), VaCuusWorldInput::ToModifiers(MouseEvent)));
			}
			++NumConsumed;
			return true;
		}
		ClearLatch();
	}

	const FEngagement Engagement = QueryEngagement(SlateApp, MouseEvent);
	if (!Engagement.bEngaged)
	{
		++NumDeferredToSlate;
		return false;
	}

	// WS-STALE-RAY: a press re-traces rather than reusing the frame's cached move
	// hit; the fresh pixel reaches RmlUi's hover before the press because the UI
	// dispatcher replays a move at the event's own position first
	// (VaCuusUIThread.cpp:153-172).
	const FPanelHit Hit = ResolvePanelHit(Engagement, MouseEvent, /*bForceRetrace=*/true);
	if (Hit.Component == nullptr)
	{
		++NumPassedToGame;
		return false;
	}

	SendLeaveIfHovering(Hit.Component);
	HoveredPanel = Hit.Component;

	// Sent BEFORE the verdict -- the widget's own order (SVaCuusWidget.cpp:432-444):
	// RmlUi sees the press either way (it may close a dropdown); coverage only
	// decides whether the game ALSO hears it.
	UVaCuusView* View = Hit.Component->GetView();
	View->SendInput(FVaCuusInputEvent::MouseButton(
		/*bDown=*/true, Hit.Pixel, MouseEvent.GetEffectingButton(), VaCuusWorldInput::ToModifiers(MouseEvent)));

	if (View->GetSnapshot().Contains(Hit.Pixel))
	{
		// The latch engages on the FIRST consumed button and releases on the last-up
		// rule in the up handler -- the widget's capture idiom without Slate capture
		// (there is nothing routed to capture: we consumed pre-routing).
		bLatched = true;
		LatchedPanel = Hit.Component;
		++NumConsumed;
		return true;
	}
	++NumPassedToGame;
	return false;
}

bool FVaCuusWorldInputProcessor::HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
	check(IsInGameThread());
	VACUUS_PERF_SCOPE(Input);

	if (bLatched)
	{
		UVaCuusWorldComponent* Panel = LatchedPanel.Get();
		if (Panel != nullptr && Panel->GetView() != nullptr)
		{
			FIntPoint Pixel;
			if (ResolveLatchedPixel(MouseEvent, Panel, Pixel))
			{
				Panel->GetView()->SendInput(FVaCuusInputEvent::MouseButton(
					/*bDown=*/false, Pixel, MouseEvent.GetEffectingButton(), VaCuusWorldInput::ToModifiers(MouseEvent)));
			}
		}

		// GetPressedButtons() is the POST-release set: the CALLERS remove the released
		// button from PressedMouseButtons BEFORE constructing the value-copy event --
		// OnMouseUp (SlateApplication.cpp:6098-6118) and the drag-drop synthesized up
		// (:7565-7581); the Remove inside ProcessMouseButtonUpEvent runs after the
		// event copy exists (a no-op for the OnMouseUp path, kept for touch) and
		// cannot affect what this handler sees. So IsEmpty() -- not Num() <= 1 -- is
		// "the last button just came up". The widget's exact rule and reason
		// (SVaCuusWidget.cpp:514-523): `<= 1` would drop the latch with a second
		// button still held mid-drag.
		if (MouseEvent.GetPressedButtons().IsEmpty())
		{
			ClearLatch();
		}

		// The press was ours, so its release must be too.
		++NumConsumed;
		return true;
	}

	const FEngagement Engagement = QueryEngagement(SlateApp, MouseEvent);
	if (!Engagement.bEngaged)
	{
		++NumDeferredToSlate;
		return false;
	}

	const FPanelHit Hit = ResolvePanelHit(Engagement, MouseEvent, /*bForceRetrace=*/true);
	if (Hit.Component == nullptr)
	{
		++NumPassedToGame;
		return false;
	}

	SendLeaveIfHovering(Hit.Component);
	HoveredPanel = Hit.Component;

	// An unlatched up means the press was not ours (it passed through, or predates
	// the processor). Forwarded, and consumed only on coverage -- the widget's
	// no-capture rule (SVaCuusWidget.cpp:533-541): swallowing a release whose press
	// the game heard would leave the game holding a button down forever.
	UVaCuusView* View = Hit.Component->GetView();
	View->SendInput(FVaCuusInputEvent::MouseButton(
		/*bDown=*/false, Hit.Pixel, MouseEvent.GetEffectingButton(), VaCuusWorldInput::ToModifiers(MouseEvent)));

	if (View->GetSnapshot().Contains(Hit.Pixel))
	{
		++NumConsumed;
		return true;
	}
	++NumPassedToGame;
	return false;
}

bool FVaCuusWorldInputProcessor::HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
	// Slate delivers the SECOND press of a double-click as this event whenever no
	// widget holds capture (SlateApplication.cpp:6046-6050 rewrites it into a down
	// only for a captor) -- and this processor never holds Slate capture. Forwarded
	// as a plain press: RmlUi synthesises its own `dblclick` from consecutive
	// presses, so it needs the press, not a special event -- the widget's argument
	// verbatim (SVaCuusWidget.cpp:543-556).
	return HandleMouseButtonDownEvent(SlateApp, MouseEvent);
}

bool FVaCuusWorldInputProcessor::HandleMouseWheelOrGestureEvent(
	FSlateApplication& SlateApp, const FPointerEvent& InWheelEvent, const FPointerEvent* InGestureEvent)
{
	check(IsInGameThread());
	VACUUS_PERF_SCOPE(Input);

	// Gesture halves with no wheel motion (trackpad pans arrive here with the
	// gesture pointer set, slate-input.md:227) are not v1's problem: the screen
	// widget never sees them either (SWidget has no gesture hook it overrides).
	if (FMath::IsNearlyZero(InWheelEvent.GetWheelDelta()))
	{
		return bLatched;
	}

	if (bLatched)
	{
		UVaCuusWorldComponent* Panel = LatchedPanel.Get();
		if (Panel != nullptr && Panel->GetView() != nullptr)
		{
			FIntPoint Pixel;
			if (ResolveLatchedPixel(InWheelEvent, Panel, Pixel))
			{
				Panel->GetView()->SendInput(FVaCuusInputEvent::MouseWheel(
					Pixel, InWheelEvent.GetWheelDelta(), VaCuusWorldInput::ToModifiers(InWheelEvent)));
			}
			++NumConsumed;
			return true;
		}
		ClearLatch();
	}

	const FEngagement Engagement = QueryEngagement(SlateApp, InWheelEvent);
	if (!Engagement.bEngaged)
	{
		++NumDeferredToSlate;
		return false;
	}

	const FPanelHit Hit = ResolvePanelHit(Engagement, InWheelEvent, /*bForceRetrace=*/false);
	if (Hit.Component == nullptr)
	{
		++NumPassedToGame;
		return false;
	}

	SendLeaveIfHovering(Hit.Component);
	HoveredPanel = Hit.Component;

	// UE's sign and unit carried through unchanged; the flip to RmlUi's convention
	// happens at dispatch (VaCuusUIThread.cpp:175-192). Consume on coverage, not on
	// "is anything scrollable" -- the widget's wheel rule and its stated visible
	// consequence (SVaCuusWidget.cpp:560-576).
	UVaCuusView* View = Hit.Component->GetView();
	View->SendInput(FVaCuusInputEvent::MouseWheel(
		Hit.Pixel, InWheelEvent.GetWheelDelta(), VaCuusWorldInput::ToModifiers(InWheelEvent)));

	if (View->GetSnapshot().Contains(Hit.Pixel))
	{
		++NumConsumed;
		return true;
	}
	++NumPassedToGame;
	return false;
}
