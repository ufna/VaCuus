// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Engine/HitResult.h" // complete FHitResult (the CachedTraceHit member)
#include "Framework/Application/IInputProcessor.h"
#include "Layout/Geometry.h"

class APlayerController;
class FSlateApplication;
class UGameViewportClient;
class UVaCuusWorldComponent;
class UWorld;
struct FPointerEvent;
struct FVaCuusModifierState;

/**
 * The pure math half of world-panel input, kept static and free of any Slate or
 * world state so the M5 Task 7 unit tests can drive it against hand-computed
 * transforms on the automation thread.
 *
 * The coordinate contract everything below relies on: the quad proxy spans DrawSize
 * in WORLD UNITS with UV 0..1 (FVaCuusWorldSceneProxy, VaCuusWorldComponent.cpp
 * :80-105 -- the engine's exact plane, WidgetComponent.cpp:396-402), so a
 * component-local position converts to RT pixels with NO scaling step: world units
 * on the quad ARE view pixels.
 */
struct FVaCuusWorldHitMath
{
	/**
	 * UWidgetComponent::GetLocalHitLocation's plane math, ported verbatim
	 * (WidgetComponent.cpp:2036-2054): inverse-transform the world hit into
	 * component space, take (-Y, -Z) as the 2D position, offset by DrawSize * Pivot.
	 *
	 * The engine function's tail (:2049-2053, "Apply the parabola distortion") is an
	 * exact no-op for a plane -- it computes NormalizedLocation = Out / DrawSize and
	 * then Out.Y = DrawSize.Y * NormalizedLocation.Y, i.e. Y = Y -- so it is omitted
	 * rather than ported dead. (It exists there for the cylinder mode this component
	 * deliberately does not clone.)
	 */
	static FVector2D LocalHitToWidget(
		const FTransform& ComponentTransform, const FVector& WorldHit, FIntPoint DrawSize, FVector2D Pivot);

	/**
	 * The engine's front-face gate (WidgetComponent.cpp:186-189): interaction only
	 * when the trace came from the side the quad's +X normal faces, i.e.
	 * dot(Forward, Impact - TraceStart) < 0. Applied even when the panel renders
	 * two-sided -- UWidgetComponent gates the back face unconditionally too, and a
	 * mirrored UI taking mirrored clicks would be worse than one taking none.
	 */
	static bool IsFrontFacing(const FVector& ComponentForward, const FVector& ImpactPoint, const FVector& TraceStart);

	/**
	 * Ray vs the quad's own plane (X = 0 in component space -- every proxy vertex
	 * has X = 0, VaCuusWorldComponent.cpp:99-102), UNBOUNDED: the result can be
	 * outside [0, DrawSize), and that is the point. This is the latched-drag path's
	 * analog of Slate handing a captor out-of-bounds local coordinates
	 * (SVaCuusWidget.cpp:534-537's "a drag that started on a scrollbar must keep
	 * being ours even after the pointer wanders off it") -- a box trace cannot
	 * answer once the cursor leaves the quad, the plane always can.
	 *
	 * @return false for a ray parallel to the plane or intersecting it behind the
	 *         ray origin; the caller skips the move rather than inventing a position.
	 */
	static bool RayToWidget(const FTransform& ComponentTransform, const FVector& RayOrigin, const FVector& RayDirection,
		FIntPoint DrawSize, FVector2D Pivot, FVector2D& OutWidget);

	/** FVector2D widget coords -> snapshot pixels: floored, not rounded, because a pixel spans [n, n+1) and so does FIntRect::Contains (SVaCuusWidget.cpp:381-388's convention). */
	static FIntPoint WidgetToPixel(FVector2D Widget)
	{
		return FIntPoint(FMath::FloorToInt(Widget.X), FMath::FloorToInt(Widget.Y));
	}
};

/**
 * THE OCCLUSION RULE'S RAYCAST FORWARDER (M5 spec 2(h), 3.4): one process-wide
 * IInputProcessor that turns pointer events over the game viewport into world
 * traces, resolves hits against UVaCuusWorldSubsystem's panel roster, forwards
 * them to the panel's view through the exact constructors the Slate widget uses,
 * and consumes ONLY what the interactive snapshot claims.
 *
 * WHY A PREPROCESSOR MUST DEFER, spelled out because v1 of the spec got it wrong
 * (spec 12.2): IInputProcessor runs before ALL Slate routing
 * (SlateApplication.cpp:5324, :6041, :6148, :6232, :6401 -- every Process* consults
 * InputPreProcessors first), so an unconditional consume would starve every
 * UMG/Slate widget stacked over the viewport. The engine's own world-widget input
 * is an ICustomHitTestPath *inside* viewport hit-testing for exactly this reason.
 * Hence THE OCCLUSION RULE, checked before anything else on every event:
 *
 *   (a) no Slate widget holds mouse capture (HasAnyMouseCaptor,
 *       SlateApplication.cpp:4690-4700 -- any user, any pointer index). A captor
 *       owns the event stream outright: a camera-drag that started on the viewport
 *       or a scrollbar-drag on an overlay must never be poached mid-gesture.
 *       Checked fresh per event -- capture changes between events within a frame.
 *   (b) the widget path under the cursor terminates AT the game viewport:
 *       LocateWindowUnderMouse (the SAME query Slate's own router runs immediately
 *       after us, SlateApplication.cpp:5601, :6052, :6155, :6239 -- so the marginal
 *       cost is one extra hit-grid lookup per event), deepest widget compared
 *       against UGameViewportClient::GetGameViewportWidget(). DEEPEST-EQUALS, not
 *       path-contains: widgets added over the game (UMG AddToViewport) live INSIDE
 *       the SViewport subtree via SGameLayerManager, so a path to an overlaying
 *       button still CONTAINS the viewport -- only the deepest-widget identity says
 *       "nothing interactive overlays this point". Over a bare scene the deepest
 *       hit-testable widget IS the SViewport: SGameLayerManager is
 *       SelfHitTestInvisible (SGameLayerManager.h:97). Cached per
 *       (frame, position), the engine hit tester's own pattern
 *       (WidgetComponent.cpp:259-279).
 *
 * Only then: viewport-local coords via the located path's viewport geometry
 * (AbsoluteToLocal * Scale, WidgetComponent.cpp:175) -> the player controller's
 * ECC_Visibility trace (GetHitResultAtScreenPosition, the FWidget3DHitTester
 * precedent :270; profile "UI" blocks exactly ECC_Visibility, BaseEngine.ini:3120,
 * so panels are hit and a Visibility-blocking wall in front of one occludes it by
 * being hit first) -> roster membership -> front-face gate -> FVaCuusWorldHitMath
 * -> UVaCuusView::SendInput (which only enqueues; every RmlUi call stays on the UI
 * thread).
 *
 * CONSUME = View->GetSnapshot().Contains(Pixel) -- the same one-frame-stale,
 * per-frame-stable answer the screen path gives Slate (VaCuusView.h:539-569);
 * pass-through is the absence of coverage, never an occluder
 * (VaCuusInteractiveSnapshot.h:259-265). Returning false hands the untouched event
 * to Slate, which routes it to the viewport and the game hears it.
 *
 * WS-STALE-RAY (research note m5-api-notes/worldspace-cli.md 6): button events
 * RE-TRACE instead of reusing the frame's cached move hit -- between the frame's
 * move and its click the panel (or the camera) can have moved, and a press resolved
 * against stale geometry lands on the wrong pixel. The re-traced position reaches
 * RmlUi correctly because the UI thread's dispatcher replays a ProcessMouseMove at
 * the event's own position before every ProcessMouseButtonDown/Up
 * (VaCuusUIThread.cpp:153-172), so the fresh pixel updates hover before the press
 * fires. THE DECISION AND ITS PRICE: re-trace kept. The extra trace measured
 * avg 2.17 us / max 31.59 us per button event (vacuus.M5World.InputSmoke's
 * measurement block, 500 samples, 1920x1080 Vulkan headless -game, the M5World
 * demo scene) -- two orders of magnitude under the 0.10 ms-class per-event input
 * budget, so button accuracy on moving panels costs effectively nothing and the
 * (frame, position) cache stays a move-only optimization. The occlusion query
 * itself measured avg 0.41 us / max 4.16 us per event on the same scene (the
 * spec 9 risk row's number).
 *
 * THE CAPTURE LATCH: there is no Slate capture to take (we consume before routing,
 * so Slate never sees the press), so the widget's capture semantics are replayed
 * locally -- a consumed press latches the panel, and every move/up keeps being
 * consumed and forwarded (moves via the unbounded plane projection once the ray
 * leaves the quad) until the LAST button releases: GetPressedButtons().IsEmpty()
 * on the up, the post-release set -- established by the CALLERS, which remove the
 * button from PressedMouseButtons BEFORE constructing the value-copy event
 * (OnMouseUp, SlateApplication.cpp:6098-6118; the drag-drop synthesized up,
 * :7565-7581); the Remove inside ProcessMouseButtonUpEvent runs after the event
 * copy exists and cannot affect what a preprocessor sees -- exactly the widget's
 * rule and for the widget's reason (SVaCuusWidget.cpp:629-637 -- `<= 1` would
 * drop a two-button drag).
 *
 * MOUSE LEAVE IS MANDATORY: when the ray leaves a hovered panel (trace miss, a
 * different panel, or the occlusion rule disengaging), MouseLeave is sent or
 * RmlUi's `:hover` sticks forever (SVaCuusWidget.cpp:705-711; Context.cpp:839-846).
 * Event-driven only: Slate's synthesized moves skip preprocessors
 * (SlateApplication.cpp:6399 gates on !bIsSynthetic), so a panel occluded UNDER a
 * motionless cursor un-hovers on the next real pointer event, not the same frame.
 * The stickiness runs the other way too, honestly: a CONSUMED engaged move
 * returns from ProcessMouseMoveEvent before RoutePointerMoveEvent ever runs, so
 * Slate's own enter/leave diff is skipped for that event and a Slate/UMG widget
 * the cursor left FOR the panel keeps ITS :hover until the next real move the
 * processor does not consume -- Slate cannot synthesize one past us, by the same
 * :6399 gate.
 *
 * Pointer-only, IME-less by decision D17; keys stay with the screen path -- a
 * preprocessor has no char hook anyway (slate-input.md:619, IInputProcessor.h has
 * no OnKeyChar).
 *
 * LIFETIME: refcount-installed by UVaCuusWorldComponent registration -- first
 * panel in any world installs, last one out uninstalls -- the idiom of
 * UWidgetComponent::RegisterHitTesterWithViewport/Unregister, which creates the
 * shared hit tester on first registration and tears it down when
 * GetNumRegisteredComponents()==0 (WidgetComponent.cpp:1097-1138). Registered
 * PreGame: after the Engine/Editor buckets (PIE editor shortcuts keep working
 * above us), before the Game bucket (SlateApplication.h:189-208 -- ascending
 * order is earlier evaluation; the M2 note's guidance "PreGame or Game -- never
 * Engine/Editor", slate-input.md:249). Uninstall keeps the
 * FSlateApplication::IsInitialized() guard because the last component can
 * unregister during application teardown (the CommonUI precedent,
 * CommonUIActionRouterBase.cpp:353-382).
 *
 * Everything here runs on the game thread (Slate input always is; every handler
 * asserts it) and never blocks: the trace is synchronous scene-query work the
 * engine's own hit tester does per event, and SendInput enqueues.
 */
class FVaCuusWorldInputProcessor : public IInputProcessor, public TSharedFromThis<FVaCuusWorldInputProcessor>
{
public:
	/**
	 * One refcount tick per registered world panel. First ref creates + registers
	 * the processor (PreGame); the count still ticks with no Slate application
	 * (dedicated server, unattended null-RHI) so pairing stays honest even where
	 * installation is impossible.
	 */
	static void AddInstallRef();

	/** The matching release; the last one unregisters (IsInitialized-guarded) and drops the instance. */
	static void ReleaseInstallRef();

	/** The installed instance, or null. Tests and the demo smoke read counters through this. */
	static TSharedPtr<FVaCuusWorldInputProcessor> Get();

	/**
	 * The occlusion rule as a pure decision, factored out so the Task 7 unit test
	 * can table it without a Slate mock (spec 2(h)): consume-eligible iff no Slate
	 * widget holds mouse capture AND the widget path under the cursor terminates at
	 * the game viewport. The captor test is FIRST and independent: a captor defers
	 * even when the cursor floats over bare viewport, because the captor owns the
	 * whole stream (see the class comment's (a)).
	 */
	static bool ShouldEngage(bool bAnyMouseCaptor, bool bPathTerminatesAtGameViewport)
	{
		return !bAnyMouseCaptor && bPathTerminatesAtGameViewport;
	}

	//~ Begin IInputProcessor
	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}
	virtual bool HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	virtual bool HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	virtual bool HandleMouseWheelOrGestureEvent(
		FSlateApplication& SlateApp, const FPointerEvent& InWheelEvent, const FPointerEvent* InGestureEvent) override;
	virtual const TCHAR* GetDebugName() const override { return TEXT("VaCuusWorldInput"); }
	//~ End IInputProcessor

	//~ The observables (M5 spec 7's occlusion/pass-through tests read these; an
	//~ invariant with no observable cannot be tested and will rot). Cumulative,
	//~ game thread.

	/** Events this processor consumed (returned true): snapshot-covered panel hits and everything latched. */
	uint64 GetNumConsumed() const { return NumConsumed; }

	/** Events the occlusion rule deferred untouched: a captor existed or the path did not terminate at the game viewport. */
	uint64 GetNumDeferredToSlate() const { return NumDeferredToSlate; }

	/** Events that engaged (traced) but returned false: no roster panel under the ray, back face, or a snapshot miss. */
	uint64 GetNumPassedToGame() const { return NumPassedToGame; }

	/** MouseLeave events sent because the ray left a hovered panel. */
	uint64 GetNumLeavesSent() const { return NumLeavesSent; }

private:
	/** The occlusion rule's runtime evaluation plus everything a trace needs once engaged. */
	struct FEngagement
	{
		bool bEngaged = false;

		/** The game viewport's arranged geometry from the located path -- the desktop->viewport-pixel transform. */
		FGeometry ViewportGeometry;

		UGameViewportClient* ViewportClient = nullptr;
	};

	/** The occlusion rule against live Slate state; (b)'s path query cached per (frame, position). */
	FEngagement QueryEngagement(FSlateApplication& SlateApp, const FPointerEvent& Event);

	/** A resolved roster hit: the panel and the RT pixel, or Component == null. */
	struct FPanelHit
	{
		UVaCuusWorldComponent* Component = nullptr;
		FIntPoint Pixel = FIntPoint::ZeroValue;
	};

	/**
	 * Deproject + trace + roster + front face + hit math. bForceRetrace is
	 * WS-STALE-RAY: button events refuse the frame's cached move hit and trace
	 * fresh; moves reuse the (frame, position) cache.
	 */
	FPanelHit ResolvePanelHit(const FEngagement& Engagement, const FPointerEvent& Event, bool bForceRetrace);

	/**
	 * The latched-drag position: a fresh trace when the ray still hits the latched
	 * panel's box (exact, and honors WS-STALE-RAY on the latched up), otherwise the
	 * unbounded plane projection (see FVaCuusWorldHitMath::RayToWidget).
	 */
	bool ResolveLatchedPixel(const FPointerEvent& Event, UVaCuusWorldComponent* Panel, FIntPoint& OutPixel);

	/** MouseLeave to the hovered panel (unless it is ExceptFor) and forget it. */
	void SendLeaveIfHovering(const UVaCuusWorldComponent* ExceptFor = nullptr);

	void ClearLatch()
	{
		bLatched = false;
		LatchedPanel = nullptr;
	}

	/** True while a consumed press keeps the event stream ours (the widget's bHasMouseCapture analog). */
	bool bLatched = false;

	/** The panel the latch belongs to. Weak: a panel can die mid-drag (level streaming), which simply ends the latch. */
	TWeakObjectPtr<UVaCuusWorldComponent> LatchedPanel;

	/** The panel the pointer last resolved onto; who MouseLeave goes to. */
	TWeakObjectPtr<UVaCuusWorldComponent> HoveredPanel;

	//~ The (frame, position) caches -- the engine hit tester's pattern
	//~ (WidgetComponent.cpp:259-279), keyed the same way: GFrameNumber plus the
	//~ event's screen-space position.

	uint32 CachedPathFrame = MAX_uint32;
	FVector2D CachedPathScreenPos = FVector2D(-FLT_MAX, -FLT_MAX);
	bool bCachedPathTerminatesAtViewport = false;
	FGeometry CachedViewportGeometry;

	uint32 CachedTraceFrame = MAX_uint32;
	FVector2D CachedTraceScreenPos = FVector2D(-FLT_MAX, -FLT_MAX);
	FHitResult CachedTraceHit;
	bool bCachedTraceHasHit = false;

	//~ See the accessors.
	uint64 NumConsumed = 0;
	uint64 NumDeferredToSlate = 0;
	uint64 NumPassedToGame = 0;
	uint64 NumLeavesSent = 0;
};
