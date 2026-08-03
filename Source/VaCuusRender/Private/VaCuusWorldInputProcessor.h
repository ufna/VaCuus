// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Engine/HitResult.h" // complete FHitResult (the CachedTraceHit member)
#include "Framework/Application/IInputProcessor.h"
#include "InputCoreTypes.h"	  // complete FKey (the FVaCuusPointerPress member) and its GetTypeHash
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
 * ONE press this processor consumed, identified by what the EVENT itself carries:
 * which Slate user, which pointer, which button. FVaCuusPointerLatch below counts
 * these and counts NOTHING else -- see its comment for why the obvious alternative
 * (FPointerEvent::GetPressedButtons()) is unusable.
 *
 * The triple covers both device shapes with no branch:
 *   - a MOUSE puts every button on one pointer -- FSlateApplication always builds
 *     mouse events with PointerIndex = CursorPointerIndex (SlateApplication.cpp
 *     :6109-6118) -- so Button is what separates a left drag from a right one;
 *   - a TOUCH puts every finger on its OWN PointerIndex (OnTouchStarted passes the
 *     finger index straight through, SlateApplication.cpp:6781-6787) and hard-codes
 *     EffectingButton to LeftMouseButton for all of them (Events.h:939-940), so
 *     PointerIndex is what separates two fingers.
 * UserIndex keeps two local players' pointers apart (FInputEvent::GetUserIndex,
 * Events.h:332).
 */
struct FVaCuusPointerPress
{
	uint32 UserIndex = 0;
	uint32 PointerIndex = 0;
	FKey Button;

	/** The way to build one everywhere in the processor, so a press is exactly what its event said it was. */
	static FVaCuusPointerPress FromEvent(const FPointerEvent& Event);

	bool operator==(const FVaCuusPointerPress& Other) const
	{
		return UserIndex == Other.UserIndex && PointerIndex == Other.PointerIndex && Button == Other.Button;
	}

	friend uint32 GetTypeHash(const FVaCuusPointerPress& Press)
	{
		// FKey's GetTypeHash is an in-class friend (InputCoreTypes.h:112), so it is
		// reachable ONLY by argument-dependent lookup -- qualifying it `::` does not
		// compile.
		return HashCombine(
			HashCombine(::GetTypeHash(Press.UserIndex), ::GetTypeHash(Press.PointerIndex)), GetTypeHash(Press.Button));
	}
};

/**
 * THE CAPTURE LATCH'S STATE, as a type rather than a bool, so the release condition
 * cannot be written wrongly again (bead VaCuus-61d).
 *
 * WHAT IT HOLDS: the set of presses this processor consumed and has not yet seen
 * released, plus the panel they belong to. Held == that set is non-empty. A gesture
 * therefore ends exactly when the presses that STARTED it come back up -- not when
 * some other party's notion of "no buttons down" happens to agree.
 *
 * WHY NOT FPointerEvent::GetPressedButtons().IsEmpty(), which is what this was:
 * that predicate is honest for a MOUSE up, because FSlateApplication::OnMouseUp
 * removes the released button from PressedMouseButtons BEFORE it constructs the
 * value-copy event, and says so in its own comment (SlateApplication.cpp:6100-6107).
 * It is a CONSTANT for a touch up: OnTouchEnded builds its event with
 * bPressLeftMouseButton = true (SlateApplication.cpp:6832-6843) and the touch
 * constructor bakes PressedButtons = FTouchKeySet::StandardSet, which is
 * {LeftMouseButton}, into the copy (Events.h:938; Events.cpp:15). IsEmpty() is
 * therefore false for every touch release that will ever exist, so a latch waiting
 * on it never released and the first tap consumed every later pointer event in the
 * application, permanently.
 *
 * AND NOTHING WOULD HAVE CAUGHT IT. The processor holds no Slate capture by design
 * (it consumes before routing), so the engine's touch net has nothing of ours to
 * release: FSlateUser::NotifyPointerReleased force-releases CAPTURE on every touch
 * end (SlateUser.cpp:1284-1290), which is what quietly saves SVaCuusWidget's
 * identically-shaped predicate (SVaCuusWidget.cpp:638) by driving its
 * OnMouseCaptureLost (SlateUser.cpp:314). And IInputProcessor offers no touch hook
 * and no capture-lost hook to fall back on -- its complete surface is
 * IInputProcessor.h:20-53.
 *
 * ENFORCEMENT BY SHAPE, and its exact limit. The held set is private and every
 * mutator takes an FVaCuusPointerPress, so this type offers no predicate over a
 * button set to write: the release site now reads "remove the press this event
 * names", and there is no correct-looking wrong alternative to reach for. What it
 * does NOT do is forbid a future handler from calling MouseEvent.GetPressedButtons()
 * itself -- nothing in C++ can. It removes the path of least resistance, which is
 * the path the bug came down.
 */
class FVaCuusPointerLatch
{
public:
	/**
	 * Record a consumed press. The FIRST one takes the latch and its panel; further
	 * presses while held (a second mouse button, a second finger) join the same
	 * gesture and each must come up before it ends. A TSet, so a repeated press --
	 * the same button re-reported without an intervening up -- is idempotent rather
	 * than a leak, which a counter would not be.
	 */
	void NotePress(UVaCuusWorldComponent* InPanel, const FVaCuusPointerPress& Press)
	{
		if (HeldPresses.IsEmpty())
		{
			Panel = InPanel;
		}
		HeldPresses.Add(Press);
	}

	/**
	 * Record a release. Removing a press that is not ours is a deliberate no-op: a
	 * second finger's release, or the release of a button whose press the game got,
	 * must not end a drag this processor owns.
	 */
	void NoteRelease(const FVaCuusPointerPress& Press)
	{
		HeldPresses.Remove(Press);
		if (HeldPresses.IsEmpty())
		{
			Panel = nullptr;
		}
	}

	/** The panel died mid-drag, or there is nothing left to release to: forget the whole gesture. */
	void Drop()
	{
		HeldPresses.Reset();
		Panel = nullptr;
	}

	/** True while a consumed press keeps the event stream ours (the widget's bHasMouseCapture analog). */
	bool IsHeld() const { return !HeldPresses.IsEmpty(); }

	/** The latched panel, or null -- weak, because a panel can die mid-drag (level streaming). */
	UVaCuusWorldComponent* GetPanel() const { return Panel.Get(); }

	/** The observable a multi-button / multi-finger gesture is asserted through. */
	int32 NumHeldPresses() const { return HeldPresses.Num(); }

private:
	TWeakObjectPtr<UVaCuusWorldComponent> Panel;
	TSet<FVaCuusPointerPress> HeldPresses;
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
 * leaves the quad) until the last press THIS PROCESSOR RECORDED comes back up.
 * That bookkeeping is FVaCuusPointerLatch above, and its comment carries the whole
 * argument for why the release condition is a set of presses we saw rather than the
 * event's own button set. The behaviour it produces is still the widget's
 * (SVaCuusWidget.cpp:638): a second button pressed mid-drag keeps the drag, and only
 * the last release ends it -- reached now by counting our own presses instead of by
 * trusting a field a touch event cannot populate.
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

	/**
	 * The latch, read-only. It IS the invariant "a gesture ends when its presses
	 * end", and an invariant with no observable cannot be tested and will rot -- this
	 * one rotted for exactly that reason (bead VaCuus-61d).
	 */
	const FVaCuusPointerLatch& GetLatch() const { return Latch; }

#if WITH_DEV_AUTOMATION_TESTS
	/**
	 * TEST SEAM: put the processor into the state a consumed press leaves it in.
	 *
	 * A press cannot reach the latch under automation, because QueryEngagement needs
	 * GEngine->GameViewport and an editor automation session has none, so every
	 * synthesized press is deferred before it can latch (see QueryEngagement's
	 * ViewportWidget gate). Everything AFTER the press needs no viewport: each
	 * handler consults the latch before QueryEngagement, and ResolveLatchedPixel
	 * simply declines when there is no viewport to project through, leaving the
	 * consume/release decision -- the part under test -- to run for real.
	 *
	 * PASS A LIVE PANEL. With a null one the handlers take their panel-died branch,
	 * and a test that never latches onto anything cannot tell a released latch from a
	 * dropped one: the "was the latch really released" assertions would pass under
	 * the very bug they exist to catch.
	 */
	void SeedLatchForTest(const FPointerEvent& PressEvent, UVaCuusWorldComponent* Panel)
	{
		Latch.NotePress(Panel, FVaCuusPointerPress::FromEvent(PressEvent));
	}
#endif

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

	/** The gesture in progress; see FVaCuusPointerLatch. */
	FVaCuusPointerLatch Latch;

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
