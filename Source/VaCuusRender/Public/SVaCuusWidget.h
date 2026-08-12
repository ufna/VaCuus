// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "InputCoreTypes.h"
#include "UObject/WeakObjectPtr.h"
#include "Widgets/SLeafWidget.h"

class FNavigationConfig;
class FVaCuusSlateElement;
class SVaCuusWidget;
class UVaCuusView;
struct FVaCuusInputEvent;
struct FVaCuusInteractiveSnapshot;
struct FVaCuusModifierState;

/**
 * One widget's Slate mouse-capture flag and the process-wide "who holds it" mirror behind
 * SVaCuusWidget::GetMouseCaptureHolder_Debug(), welded into one object.
 *
 * ENFORCEMENT BY SHAPE RATHER THAN BY COMMENT (bead VaCuus-akj.6.41): bHeld is private and
 * SVaCuusWidget is deliberately NOT a friend, so `bHasMouseCapture = true` -- what the three
 * capture sites used to be -- no longer compiles. Set(), one definition in SVaCuusWidget.cpp
 * writing both halves, is the only way in. That makes "a fourth capture write site cannot
 * forget the mirror" a compile error rather than a review note; there are three today (the
 * press that takes capture, the last button-up that hands it back, and OnMouseCaptureLost),
 * and an attribution that silently stopped tracking one of them would report "THE GAME" for a
 * press the UI took -- the exact wrong conclusion for an acceptance run to record.
 *
 * A FREE CLASS RATHER THAN A NESTED ONE only so that Set() can be defined out of line without
 * naming a private nested type from namespace scope.
 */
class VACUUSRENDER_API FVaCuusMouseCaptureState
{
public:
	bool IsHeld() const { return bHeld; }

	/** Sets this widget's flag and the process-wide holder in one statement. Game thread. */
	void Set(const TSharedRef<const SVaCuusWidget>& Widget, bool bInHeld);

private:
	bool bHeld = false;
};

/**
 * The fingers this widget has told RmlUi about and has not yet ended (bead VaCuus-ujm).
 *
 * WHY A LEDGER IS NEEDED AT ALL. RmlUi keeps per-touch state in a map keyed by identifier
 * and erases an entry only in ProcessTouchEnd or ProcessTouchCancel (Context.cpp:1017,
 * :1031). Slate has no "cancel" hook to mirror: a gesture can be taken away instead of
 * finished -- capture revoked, the window deactivated, the widget destroyed mid-drag -- and
 * when that happens the only notification is OnMouseCaptureLost, which names no finger. So
 * the widget has to remember which fingers are outstanding, or a stolen gesture leaves RmlUi
 * holding a touch that will never end AND a button 0 it will never see released.
 *
 * WHY IT IS NOT "MULTI-TOUCH SUPPORT", which is a separate and larger item (mobile-support
 * .md 3.1: the capture state is one bool and every finger dispatches RmlUi button 0). This
 * set decides only WHOM TO CANCEL. It grants no per-finger capture, no per-finger hover and
 * no second button; two fingers still share one gesture, exactly as before.
 *
 * ENFORCEMENT BY SHAPE: the set is private and every mutator answers whether the id was
 * actually new/known, so "send TouchEnd for a finger we never started" and "cancel a finger
 * twice" are decided here rather than by a condition at each call site. Drain() empties in
 * the same call that reports the contents, so a cancel loop cannot forget to clear.
 *
 * Game thread only, like every input handler that touches it.
 */
class FVaCuusTouchLedger
{
public:
	/** @return true if this id was NOT already outstanding -- i.e. this really is a new finger. */
	bool NoteStart(uint64 TouchId)
	{
		bool bAlreadyPresent = false;
		ActiveTouchIds.Add(TouchId, &bAlreadyPresent);
		return !bAlreadyPresent;
	}

	/** @return true if this id was outstanding (and is now gone): the finger was ours to end. */
	bool NoteEnd(uint64 TouchId) { return ActiveTouchIds.Remove(TouchId) > 0; }

	/** Everything still outstanding, removed in the same call. The cancel path's only input. */
	TArray<uint64> Drain()
	{
		TArray<uint64> Outstanding = ActiveTouchIds.Array();
		ActiveTouchIds.Reset();
		return Outstanding;
	}

	int32 Num() const { return ActiveTouchIds.Num(); }

private:
	TSet<uint64> ActiveTouchIds;
};

/**
 * The Slate face of one VaCuus view: composites the UI thread's frames and routes
 * input into it.
 *
 * PUBLIC, AND THE ONLY CONCRETE TYPE OF THE COMPOSITION API THAT IS (bead VaCuus-akj.25).
 * Everything else a caller needs to stand a view up in Slate is reachable through the
 * opaque factories in VaCuusSlateView.h; this class is the exception because SUBCLASSING is
 * the extension point. A game that stacks two views and wants one hit-testable widget to
 * route between them -- which is what the shipped lobby demo does, and what forced this
 * header out of Private -- has to override the input virtuals and call Super, and no
 * factory can offer that. Chaining to Super is the contract: each handler here queues the
 * event and answers from the snapshot, and skipping it silently drops both halves.
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
 *
 * KEYBOARD ARBITRATION (controller decision D12) has two halves, both of them here
 * because both are Slate-side policy:
 *
 *  - While this widget holds Slate focus it installs FNullNavigationConfig. Slate's
 *    navigation config consumes arrows and analog sticks in
 *    ProcessKeyDownEvent/ProcessAnalogInputEvent BEFORE OnKeyDown is ever reached, so
 *    without this RmlUi's own nav-* graph would simply never see a direction key. The
 *    previous config is saved and restored exactly, on focus loss and on teardown.
 *
 *  - A PASS-THROUGH KEY SET the UI never consumes even while focused (Escape, F1-F12,
 *    the console key). This exists because the per-key answer is unobtainable: RmlUi
 *    reports consumption on the UI thread, a frame later in queue terms, and Slate
 *    needs Handled/Unhandled now -- so the honest contract is a set of keys declared
 *    up front rather than a guess per event.
 *
 * GAMEPAD (controller decision D13) is synthesized, because RmlUi has no pad support
 * whatsoever: DPad and the left stick become arrow keys, FaceButton_Bottom becomes
 * Return (RmlUi's handler clicks the focused element), FaceButton_Right becomes a
 * Back event. The stick needs a dead zone and repeat throttling on this side --
 * RmlUi does no key repeat -- which is what Tick drives.
 */
class VACUUSRENDER_API SVaCuusWidget : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SVaCuusWidget)
	{
	}
	SLATE_END_ARGS()

	//~ Analog navigation tuning (controller decision D13). Public and constexpr so the
	//~ VaCuus.Input.SlateRouting assertions are written against the real values and
	//~ cannot drift from them.

	/**
	 * Deflection at which the left stick starts navigating, on the dominant axis.
	 * 0.5 is deliberately high for a dead zone: this is not analog movement, it is a
	 * discrete direction, and a low threshold makes a resting stick creep the focus.
	 */
	static constexpr float AnalogNavDeadZone = 0.5f;

	/** Delay between the first nav step and the second, i.e. the "did you mean it" grace. */
	static constexpr double AnalogNavInitialRepeatSeconds = 0.4;

	/** Interval between later steps while the stick stays deflected. */
	static constexpr double AnalogNavRepeatIntervalSeconds = 0.12;

	/**
	 * InView is a handle, not an owner: the subsystem owns the view and the console
	 * command calls DetachView() before destroying it. Held weakly, so a
	 * garbage-collected or invalidated view simply stops being driven.
	 */
	void Construct(const FArguments& InArgs,
		UVaCuusView* InView,
		const TSharedRef<FVaCuusSlateElement>& InElement);

	/** Restores Slate's navigation config if this widget is destroyed while still focused. */
	virtual ~SVaCuusWidget() override;

	/** Teardown step 1: stop queueing commands so the view can be retired. */
	void DetachView();

	/**
	 * Hands back Slate's pointer capture, but ONLY for the users whose captor is this
	 * widget. No-op when nothing captured us.
	 *
	 * WHY THE OWNER CALLS IT AND THE WIDGET DOES NOT DO IT ITSELF: Slate stores its captor
	 * as a WEAK WIDGET PATH whose IsValid() only tests Num() > 0, so it never notices that
	 * the captured leaf was destroyed -- dropping a VaCuus widget mid-drag leaves a captor
	 * pointing at nothing and the next mouse-up trips
	 * `ensureMsgf(MouseCaptorPath.Widgets.Num() > 0, ...)` (SlateApplication.cpp:5558). The
	 * destructor is the wrong place to fix that (it can run from inside Slate's own tick or
	 * paint, when the widget is already half-gone), so every owner that can drop this widget
	 * -- vacuus.M1HUD's teardown and UVaCuusWidget::ReleaseSlateResources -- calls this
	 * first. What the widget owns is only the KNOWLEDGE of how to ask.
	 *
	 * PER USER, NOT FSlateApplication::ReleaseAllPointerCapture() (bead VaCuus-akj.6.16):
	 * that call releases capture for EVERY Slate user, so in split-screen -- one FSlateUser
	 * per local player, each with its own captor -- tearing this widget down would abort an
	 * unrelated player's drag on a completely different widget. FSlateUser answers the
	 * precise question ("is THIS widget YOUR captor") and only those users are released.
	 */
	void ReleaseOwnPointerCapture(const TCHAR* Reason);

	//~ Pass-through keys (controller decision D12): keys the UI never consumes and
	//~ never forwards, even while a document holds focus. The default set is Escape,
	//~ F1-F12 and the console key; a game (or the debug console command) extends it.

	/** Adds a key the UI must never take. Idempotent. */
	void AddPassThroughKey(const FKey& Key);

	/** Removes a key from the set; returns false if it was not in it. */
	bool RemovePassThroughKey(const FKey& Key);

	const TSet<FKey>& GetPassThroughKeys() const { return PassThroughKeys; }

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

	//~ TOUCH (bead VaCuus-ujm). These exist so a finger reaches RmlUi's own touch
	//~ pipeline -- drag-to-scroll, inertia and click-cancel-on-scroll -- instead of being
	//~ laundered into a mouse press that can do none of it. The .cpp carries the
	//~ double-delivery argument, which is the whole reason overriding these is not free.
	//~
	//~ OnTouchForceChanged and OnTouchGesture are deliberately NOT overridden: a force
	//~ change carries no new position (SlateApplication.cpp:6866-6874 reuses Location for
	//~ both), so forwarding one would be a zero-delta move, and RmlUi has no pressure
	//~ concept to spend it on. OnTouchFirstMove IS overridden, because Slate routes the
	//~ first move of a drag there instead of OnTouchMoved and suppresses the mouse
	//~ fallback for it (SlateApplication.cpp:5856-5863, :5951-5958) -- so without it the
	//~ first delta of every drag is simply lost.
	virtual FReply OnTouchStarted(const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent) override;
	virtual FReply OnTouchMoved(const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent) override;
	virtual FReply OnTouchFirstMove(const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent) override;
	virtual FReply OnTouchEnded(const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent) override;

	virtual void OnMouseEnter(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual void OnMouseLeave(const FPointerEvent& MouseEvent) override;
	virtual void OnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent) override;
	virtual FCursorReply OnCursorQuery(const FGeometry& MyGeometry, const FPointerEvent& CursorEvent) const override;
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent) override;
	virtual FReply OnAnalogValueChanged(const FGeometry& MyGeometry, const FAnalogInputEvent& InAnalogInputEvent) override;
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
	bool IsTrackingMouseCapture_Debug() const { return MouseCapture.IsHeld(); }

	/**
	 * How many fingers this widget has started with RmlUi and not yet ended.
	 *
	 * Exposed for VaCuus.Input.TouchRouting: "every start is matched" is the invariant that
	 * keeps RmlUi's touch_states map from accumulating fingers that never lift, and an
	 * invariant with no observable cannot be tested and will rot. It is also what separates
	 * "we cancelled the gesture" from "the engine happened to send an end anyway".
	 */
	int32 GetNumActiveTouches_Debug() const { return TouchLedger.Num(); }

	/**
	 * WHICH VaCuus widget holds Slate's mouse capture right now, process-wide, or null.
	 *
	 * THE ONE WIDGET-AGNOSTIC ANSWER TO "WHOSE PRESS WAS IT" (bead VaCuus-akj.6.41).
	 * FSlateApplication::ProcessMouseButtonDownEvent returns true when ANY widget on the
	 * bubble path handled the press, and SViewport -- the game's own widget -- is our
	 * ancestor: UGameViewportClient::AddAssociation puts SGameLayerManager (whose overlay
	 * hosts us) inside the SViewport as its content (GameViewportClient.cpp:1312-1326). So a
	 * pass-through press and a UI press BOTH report "handled" and the console commands cannot
	 * tell them apart from that return alone. Capture can: this widget takes Slate's mouse
	 * capture on the first press it answers Handled and takes NONE at all when the snapshot
	 * does not cover the point, so the holder is a direct readout of the FReply produced.
	 *
	 * STATIC RATHER THAN "ASK THE WIDGET" because the commands that need the answer are
	 * shared: ClickWhereThePointerIs is called by vacuus.M1HUD.TypeShot AND by
	 * vacuus.LobbyDemo.Click (which reaches it by forward declaration inside the module), and
	 * the lobby demo has no VaCuusM1HUD::GState to reach a widget through at all.
	 * vacuus.M2Demo.Drag asks the same question and now reads the same answer.
	 *
	 * SAMPLE IT BETWEEN A PRESS AND ITS RELEASE: the release hands capture back, so a sample
	 * afterwards always reads "nobody".
	 */
	static TSharedPtr<const SVaCuusWidget> GetMouseCaptureHolder_Debug();

	/**
	 * Whether this widget currently has Slate's navigation config replaced with
	 * FNullNavigationConfig (controller decision D12).
	 *
	 * Exposed for VaCuus.Input.SlateRouting: the swap has to happen on focus and be
	 * undone on focus loss, and a leaked override would silently kill arrow-key
	 * navigation for every other widget in the application.
	 */
	bool IsNavigationConfigOverridden_Debug() const { return InstalledNavigationConfig.IsValid(); }

	/**
	 * How many navigation key presses the analog throttle has synthesized.
	 *
	 * Exposed for VaCuus.Input.SlateRouting, which drives Tick's clock by hand: the
	 * dead zone, the initial delay and the repeat interval are all timing behaviour that
	 * a real pad cannot reproduce deterministically, and a count is what makes "exactly
	 * one press on the first frame" an assertion rather than an observation.
	 */
	int32 GetNumAnalogNavKeys_Debug() const { return NumAnalogNavKeys; }

	/**
	 * Whether this widget believes it is holding Slate focus that it asked for itself.
	 *
	 * Exposed for VaCuus.Input.AnalogNavGate, which asserts that the release in
	 * TickKeyboardFocusRelease is edge-triggered and actually fires: the Slate half of it
	 * (handing focus to the game viewport) needs a real window and cannot run headless,
	 * but the bookkeeping that decides whether to release can.
	 */
	bool HasSelfRequestedUserFocus_Debug() const { return bSelfRequestedUserFocus; }

private:
	/** Which way the left stick is currently pushed, once the dead zone has been applied. */
	enum class EAnalogNavDirection : uint8
	{
		None,
		Up,
		Down,
		Left,
		Right
	};
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
	 * Hands the view's IME bridge this widget's absolute rect, its native window, the
	 * platform's text-input system and whether we hold Slate focus (Task 9).
	 *
	 * FROM HERE AND NOWHERE ELSE, because this module is the only one that may touch Slate:
	 * `FSlateApplication::GetTextInputMethodSystem()` and `FindWidgetWindow()` both live in
	 * the Slate module, while VaCuus deliberately depends only on ApplicationCore -- which is
	 * where the whole ITextInputMethod* interface lives anyway. So the widget MEASURES and the
	 * view's handler DECIDES.
	 *
	 * Called from Tick (cheap and idempotent -- the handler ignores an unchanged surface) and
	 * from both focus handlers, where the focus bit is what actually changed.
	 *
	 * bFocusOverride is how the two focus handlers say what they KNOW rather than what Slate
	 * happens to report at that instant: OnFocusLost runs inside the focus transition, and
	 * relying on HasAnyUserFocus() there would make the answer depend on exactly where in
	 * FSlateApplication's bookkeeping the callback sits. Unset means "ask Slate", which is what
	 * Tick wants.
	 */
	void PushImeSurface(TOptional<bool> bFocusOverride = TOptional<bool>());

	/**
	 * The view's newest published snapshot, or a permanently empty one when there is
	 * no view. Never fails and never blocks -- that is the whole point of it -- so every
	 * handler below can be a plain synchronous test.
	 */
	const FVaCuusInteractiveSnapshot& GetSnapshot() const;

	/** Queues one event for this view, if there still is one. */
	void SendInput(const FVaCuusInputEvent& Event);

	/**
	 * Queues a MOUSE-shaped translation of a pointer event -- unless the pointer is a finger,
	 * in which case the touch handlers already queued it and this drops it.
	 *
	 * THE SINGLE ANSWER TO SLATE'S TOUCH->MOUSE FALLBACK (bead VaCuus-ujm). Every
	 * mouse-shaped handler that enqueues goes through here rather than repeating the test,
	 * because the failure mode of the repeated form is a silent phantom double-click. The
	 * .cpp carries the engine lines and the argument.
	 */
	void SendMouseInput(const FPointerEvent& PointerEvent, const FVaCuusInputEvent& Event);

	/** The RmlUi touch identity of a Slate pointer; FVaCuusInputEvent::MakeTouchId is the composer. */
	static uint64 ToTouchId(const FPointerEvent& TouchEvent);

	/**
	 * The reply half of a press -- capture, D11 focus and D14a IME -- shared by the mouse's
	 * OnMouseButtonDown and the finger's OnTouchStarted. See the .cpp for why it is shared
	 * rather than copied.
	 */
	FReply AnswerPointerDown(FIntPoint Position);

	/**
	 * Services the AutoShot debug screenshot for WHATEVER view this widget hosts, on the
	 * game thread.
	 *
	 * THE `vacuus.M1HUD.*` COMMAND AND CVAR NAMES STAY AS THEY ARE, deliberately: the prefix
	 * is historical (the toggle became a variable over ten documents in M2 Task 14 -- see
	 * GDocumentVfsPath in VaCuusRender.cpp) but every recipe in docs/passport spells the
	 * names out, and the -VaCuusPerfLog launch flag looks `vacuus.M1HUD.PerfLog` up BY STRING
	 * (VaCuusRender.cpp, FindConsoleVariable). Renaming them would silently break both. What
	 * gets fixed is the WORDING of what they print, which no recipe depends on.
	 */
	void TickAutoShot();

	/** The only writer of MouseCapture; FVaCuusMouseCaptureState above says why it is the only one. */
	void SetMouseCapture(bool bInCaptured);

	/**
	 * SLATE'S ANSWER TO "DO YOU ROUTE POINTER EVENTS TO ME", with our mirror reconciled to it.
	 * Every routing decision asks THIS, never MouseCapture.IsHeld() directly (bead VaCuus-86x).
	 *
	 * The flag alone is wrong in both directions, and each has a named mechanism:
	 *
	 *  - SLATE HOLDS AND WE DO NOT KNOW (macOS). FSlateUser::RestoreCaptorPathsByIndex assigns
	 *    the whole captor map and notifies nobody -- it is a one-line setter, and the engine's
	 *    own comment above it says "Unclear why from existing code, but Mac seems to need to
	 *    cache & restore all mouse captor paths when activating the top level window"
	 *    (SlateUser.h:191-193). Its only caller is the PLATFORM_MAC block in
	 *    FSlateApplication::ProcessApplicationActivationEvent, which snapshots the captors,
	 *    calls ActivateApplication -- releasing every capture, which DOES fire our
	 *    OnMouseCaptureLost and clears the flag -- and then puts the map back behind us. Slate
	 *    then keeps routing a held drag here while the widget believes it holds nothing, so
	 *    the first move that leaves the interactive rect answers Unhandled and the drag dies
	 *    mid-gesture.
	 *
	 *  - WE HOLD AND SLATE DOES NOT (every platform). The press sets the flag BEFORE the reply
	 *    is processed, and FSlateUser::SetPointerCaptor can refuse -- it returns false when the
	 *    widget is not reachable from the event path (SlateUser.cpp:256-282). ProcessReply only
	 *    broadcasts a WITH_SLATE_DEBUGGING trace for that, so no OnMouseCaptureLost ever
	 *    arrives and the widget answers Handled for every move of a capture it never got,
	 *    eating the game's camera input until the next button-up.
	 *
	 * Both are the same defect -- trusting our own bookkeeping about a fact Slate owns -- so
	 * both get the same answer rather than two guards. HasMouseCapture() reads the same
	 * PointerCaptorPathsByIndex the Mac restore writes (SWidget::HasMouseCapture ->
	 * FSlateApplication::DoesWidgetHaveMouseCapture -> FSlateUser::DoesWidgetHaveAnyCapture),
	 * which is what makes it the authority and not just a second opinion.
	 *
	 * WHAT THIS IS NOT FOR: the press and button-up bookkeeping. "Have we already requested
	 * capture for this gesture" and "is the last button up" are questions about OUR presses,
	 * and they keep reading the mirror -- see the comments at those two sites. Only the
	 * ROUTING answer (does Slate deliver to us) comes from here.
	 *
	 * NOT const: adopting an unknown capture writes the mirror, which is what the process-wide
	 * "who holds capture" attribution reads. The adopt is ONE-WAY on purpose; the
	 * implementation says why the other direction would be wrong.
	 */
	bool HoldsPointerCapture();


	/** The left stick's latched deflection as a direction, or None inside the dead zone. */
	EAnalogNavDirection ResolveAnalogNavDirection() const;

	/**
	 * Turns a held stick deflection into discrete navigation presses (D13).
	 *
	 * DRIVEN FROM TICK, NOT FROM THE ANALOG EVENT, and that is the whole design: UE
	 * delivers an analog event when the value CHANGES, so a stick held at a constant
	 * deflection stops producing events entirely -- repeat has to come from a clock. It
	 * also means the test can drive that clock.
	 */
	void TickAnalogNavigation(double InCurrentTime);

	/** Queues one synthesized nav press (down and up) and counts it. */
	void SendAnalogNavKey(EAnalogNavDirection Direction);

	/**
	 * Hands back the Slate focus this widget took itself, once the view stops wanting the
	 * keyboard.
	 *
	 * Slate never un-focuses a widget on its own, so without this one click on a text
	 * field leaves this widget owning the focus path for the rest of the session -- which
	 * keeps FNullNavigationConfig installed on the whole application and keeps every key
	 * and stick event routing here to be declined one at a time. Edge-triggered on
	 * bWantsKeyboardFocus going true -> false, and only for focus this widget requested.
	 */
	void TickKeyboardFocusRelease();

	/** The FKey a synthesized stick direction is queued as; the UI thread maps it to KI_*. */
	static FKey AnalogNavDirectionToKey(EAnalogNavDirection Direction);

	/**
	 * Would this key move RmlUi's focus INTO the document, given what the snapshot says?
	 *
	 * Task 14 acceptance decision A1: the press that ENTERS the UI is consumed rather than
	 * being handed to the game as well. The whole argument -- including why the left-stick
	 * keys and the activation keys are deliberately not in the set -- is on the definition.
	 *
	 * Static and snapshot-driven so it is pure: no member state can make the press and the
	 * release disagree.
	 */
	static bool DoesKeyEnterUIFocus(const FKey& Key, const FVaCuusInteractiveSnapshot& Snapshot);

	/**
	 * Installs FNullNavigationConfig, saving the exact config that was there (D12).
	 * Idempotent, and a no-op without Slate.
	 *
	 * UserIndex is only used to check, and log, whether the config that will ACTUALLY be
	 * consulted for that user is the one we installed -- see the known limitation in the
	 * implementation: SetNavigationConfig replaces the global config, while resolution
	 * goes through GetRelevantNavConfig(UserIndex).
	 */
	void OverrideNavigationConfig(int32 UserIndex);

	/**
	 * Puts the saved config back -- but only if ours is still the installed one, so a
	 * config somebody else swapped in meanwhile is not stomped.
	 */
	void RestoreNavigationConfig();

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
	 *
	 * Only SetMouseCapture() may write it, and only through FVaCuusMouseCaptureState::Set();
	 * that type's comment says why the bool is not a plain member any more.
	 */
	FVaCuusMouseCaptureState MouseCapture;

	/**
	 * The fingers RmlUi still believes are down for this view; see FVaCuusTouchLedger.
	 *
	 * SEPARATE FROM MouseCapture ON PURPOSE. Capture answers "does Slate route this stream
	 * to us" and is one flag for the whole gesture; this answers "which touch identifiers
	 * has RmlUi been told about", which is a set and has to be, because the cancel path
	 * needs to name every one of them.
	 */
	FVaCuusTouchLedger TouchLedger;

	/**
	 * Pending UTF-16 high surrogate from a previous OnKeyChar, or 0.
	 *
	 * TCHAR is char16_t here (PLATFORM_TCHAR_IS_CHAR16 is 1 for Unix), so a code point
	 * above the BMP arrives as two calls. RmlUi wants one UTF-32 value and its `char`
	 * overload would silently drop the whole thing anyway (Context.cpp:553-557), so
	 * the halves are joined here rather than forwarded.
	 */
	uint32 PendingHighSurrogate = 0;

	/**
	 * Keys the UI never consumes and never forwards, even while a document holds focus
	 * (controller decision D12). Populated in Construct(); see the class comment for
	 * why a declared set is the honest contract here.
	 */
	TSet<FKey> PassThroughKeys;

	/**
	 * Slate's navigation config as it was before we replaced it, and the
	 * FNullNavigationConfig we put in its place.
	 *
	 * BOTH, not just a bool: the previous config must be restored EXACTLY (an editor
	 * session, CommonUI or a game may all have installed their own -- never assume the
	 * engine default), and keeping our own instance is what lets RestoreNavigationConfig
	 * check that it is still the installed one before stomping whatever is there now.
	 * InstalledNavigationConfig being valid is also the "override is active" flag.
	 */
	TSharedPtr<FNavigationConfig> SavedNavigationConfig;
	TSharedPtr<FNavigationConfig> InstalledNavigationConfig;

	//~ Left-stick navigation state (D13). Latched from OnAnalogValueChanged, consumed by
	//~ Tick -- see TickAnalogNavigation for why the clock and not the event drives it.

	/** Newest raw axis values, in UE's convention: +Y is UP, +X is RIGHT. */
	float AnalogAxisX = 0.0f;
	float AnalogAxisY = 0.0f;

	/** Direction currently held past the dead zone; a change is what emits immediately. */
	EAnalogNavDirection HeldAnalogDirection = EAnalogNavDirection::None;

	/** Slate time at which the held direction may fire again. */
	double NextAnalogNavTime = 0.0;

	/** Synthesized nav presses so far; diagnostics and VaCuus.Input.SlateRouting. */
	int32 NumAnalogNavKeys = 0;

	/**
	 * True while this widget holds Slate focus that IT asked for, on a click over a
	 * focusable rect.
	 *
	 * The distinction is what keeps TickKeyboardFocusRelease from fighting the game: a
	 * game that focuses this widget deliberately (it just opened a pad-driven menu) does
	 * so while nothing inside the document is focused yet, which is exactly the state
	 * that would otherwise look like "the UI is done with the keyboard".
	 */
	bool bSelfRequestedUserFocus = false;

	/** Previous frame's bWantsKeyboardFocus; makes the release above edge-triggered. */
	bool bLastWantedKeyboardFocus = false;

	bool bAutoShotDone = false;
};
