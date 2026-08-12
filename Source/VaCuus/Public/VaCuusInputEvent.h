// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "InputCoreTypes.h"

/** What a queued input event asks the UI thread to do to one view's context. */
enum class EVaCuusInputEventKind : uint8
{
	/**
	 * Unset. Default on purpose, exactly like EVaCuusCommandKind::None: an event
	 * that reaches the drain still carrying it is a producer that forgot to set
	 * Kind, and the drain says so instead of performing whichever kind is first.
	 */
	None,

	/** Position; also what re-arms a context after MouseLeave. */
	MouseMove,

	/** Position + Key (a mouse button). */
	MouseDown,
	MouseUp,

	/** Position + WheelDelta, in UE's sign convention (see the field). */
	MouseWheel,

	/** No payload. Required, or RmlUi's `:hover` styling sticks forever. */
	MouseLeave,

	//~ TOUCH (bead VaCuus-ujm). Position + TouchId; NO Key, and that absence is the
	//~ enforcement -- see the factories.
	//~
	//~ WHY THESE EXIST AT ALL, given that a touch already reached RmlUi as a mouse press:
	//~ ProcessMouseButtonDown/Up carry no finger identity and no drag history, so RmlUi
	//~ could never do the one thing a phone needs -- turn a finger's travel into a scroll.
	//~ Its touch verbs do: ProcessTouchStart records the press position and the closest
	//~ scrollable ancestor (Context.cpp:892-916), ProcessTouchMove scrolls that container by
	//~ the frame's delta, accumulates a decayed inertia position and cancels the pending
	//~ click once the finger has travelled TOUCH_CLICK_MAX_DISTANCE (:928-981), and
	//~ ProcessTouchEnd converts the accumulated velocity into a fling (:996-1014).
	//~
	//~ NONE OF THAT IS RE-IMPLEMENTED HOST-SIDE, deliberately: drag-vs-tap, the distance
	//~ threshold and the inertia model are RmlUi's, and the embedder's whole job is to feed
	//~ honest starts, moves and ends. What the host DOES owe is that each finger's stream is
	//~ well formed -- one start, then moves, then exactly one end or cancel -- because
	//~ RmlUi's duplicate-touch guard is an RMLUI_ASSERTMSG (Context.cpp:895) that this build
	//~ compiles out: UBT sets NDEBUG in every configuration the plugin ships, so RMLUI_DEBUG is
	//~ never defined and Debug.h:47-48 expands the macro to nothing (VaCuusRml.Build.cs:99-115
	//~ makes the same point from the build side). A malformed stream degrades silently rather
	//~ than complaining.
	//~
	//~ EACH VERB ALSO DOES THE MOUSE HALF ITSELF -- ProcessMouseMove plus
	//~ ProcessMouseButtonDown(0) on start (Context.cpp:916-919), the matching
	//~ ProcessMouseButtonUp(0) on end (:1019-1022) and, with no move before it, on cancel
	//~ (:1033) -- which is what keeps hover, `:active`, focus and `click` working for a tap. It is also why a touch that
	//~ was ALSO delivered as a synthesized mouse event would press twice; see
	//~ SVaCuusWidget's touch handlers for how that is prevented.

	/** Finger down. Starts RmlUi's per-touch state and presses button 0. */
	TouchStart,

	/** Finger moved. Scrolls the container the start latched onto, or does nothing if there was none. */
	TouchMove,

	/** Finger lifted. Releases button 0 and may start an inertial fling. */
	TouchEnd,

	/**
	 * The gesture was taken away rather than finished -- Slate revoked capture, the widget
	 * died mid-drag, the OS stole the touch. Distinct from TouchEnd because it must NOT
	 * fling: ProcessTouchCancel drops the touch state and releases button 0 with no
	 * inertia and no final move (Context.cpp:1025-1034).
	 */
	TouchCancel,

	/** Key + Modifiers. Distinct from TextInput: this is the navigation/shortcut path. */
	KeyDown,
	KeyUp,

	/** CodePoint. The typing path, and the only one that produces characters. */
	TextInput,

	/**
	 * No payload. "Back out of the UI": blurs whatever holds RmlUi focus.
	 *
	 * NOT A KEY, because RmlUi has nothing to route it to -- there is no KI_ identifier
	 * for cancel and no default action that consumes one (ElementDocument handles Tab,
	 * the arrows and Return/Space, and nothing else). Controller decision D13 makes it
	 * an event of its own so the pad's B button has somewhere to go without inventing a
	 * key identifier a document could not interpret.
	 *
	 * SCOPE FOR M2 -- blur only, and the log line says so. Blurring has one real
	 * consequence the caller should know about: with nothing focusable focused, the
	 * view stops claiming the keyboard (FVaCuusInteractiveSnapshot::bWantsKeyboardFocus
	 * goes false) and keys reach the game again. "Close the menu", "pop the screen" and
	 * every other Back semantic is game-side policy and belongs to whatever binds it.
	 */
	NavigateBack,

	//~ IME MUTATIONS (controller decision D15). The platform text-input system drives
	//~ these from the game thread; they are the ONLY half of the IME contract that
	//~ travels game -> UI. Everything the OS READS is answered from the shadow state in
	//~ the published snapshot (FVaCuusTextFieldState) and never queued at all.
	//~
	//~ WHY THEY RIDE THE INPUT QUEUE RATHER THAN ONE OF THEIR OWN: order against typing
	//~ is the whole point. During a composition the OS interleaves SetTextInRange with
	//~ the key and character events that provoked it; two queues would let a
	//~ composition commit overtake the keystroke after it and corrupt the field. One
	//~ ordered channel is the only arrangement in which "the OS asked for this after
	//~ that" survives the hand-off.
	//~
	//~ EVERY ONE carries FieldGeneration and is DROPPED on mismatch -- see
	//~ FVaCuusTextFieldState::Generation for why a silently half-applied mutation is the
	//~ failure mode being designed out.

	/** RangeBegin/RangeEnd + Text: replace a span. ITextInputMethodContext::SetTextInRange. */
	ImeSetTextInRange,

	/** RangeBegin/RangeEnd + bCaretAtRangeBeginning. ITextInputMethodContext::SetSelectionRange. */
	ImeSetSelectionRange,

	/**
	 * RangeBegin/RangeEnd: the span RmlUi should draw as composing (it already renders the
	 * underline itself, WidgetTextInput::SetCompositionRange -> FormatText). An empty range
	 * clears it.
	 */
	ImeSetCompositionRange,

	/**
	 * RangeBegin/RangeEnd + Text: commit the composition. Distinct from ImeSetTextInRange
	 * because RmlUi's `CommitComposition` respects the element's maxlength while its
	 * `SetText` explicitly does not (TextInputContext.h:48) -- and the commit is exactly
	 * the moment maxlength has to hold.
	 */
	ImeCommitComposition,

	/**
	 * Text + FieldGeneration: the whole field becomes this string, caret after it.
	 *
	 * THE MOBILE HALF OF TEXT ENTRY, and it is a different shape from every Ime* kind above
	 * because the interface behind it is a different shape. `ITextInputMethodContext` is a
	 * SPAN editor -- it names ranges and expects us to splice. `IVirtualKeyboardEntry` is a
	 * WHOLE-VALUE push: the platform hands over the complete contents of its own edit buffer
	 * and nothing else (`SetTextFromVirtualKeyboard(const FText&, ETextEntryType)`,
	 * IVirtualKeyboardEntry.h:64). Android literally reads the field's value once when the
	 * dialog opens and hands back the finished string
	 * (AndroidPlatformTextField.cpp:102 out, AndroidJNI.cpp:1234-1245 back).
	 *
	 * SO IT CARRIES NO RANGE AT ALL, deliberately, and that is what keeps it out of the
	 * index-space trap the Ime* kinds live in. RangeBegin/RangeEnd above are RmlUi CHARACTER
	 * offsets converted by the producer against a shadow value that is one UI frame old --
	 * correct there, because the OS derived those indices from that same shadow. A whole-value
	 * push has no such anchor: the OS's string is authoritative and the live element's value
	 * is whatever it is now, so the only range that can be right is "all of it", and the only
	 * side that can measure "all of it" is the UI thread. It does, with RmlUi's own
	 * LengthUTF8; nothing about an index space crosses the queue.
	 *
	 * STILL STAMPED. A whole-value push into the WRONG field is the worst mutation in this
	 * enum -- it would not corrupt an offset, it would overwrite a different field's contents
	 * entirely -- so the generation guard matters more here, not less.
	 */
	VirtualKeyboardValue
};

/**
 * Modifier keys as the engine lets us observe them, kept RmlUi-free so the Slate
 * side can fill it in without seeing a single RmlUi header.
 *
 * NumLock and ScrollLock are absent because FInputEvent has no accessor for them
 * (Events.h exposes Shift/Control/Alt/Command/CapsLock only), and RmlUi's
 * KM_NUMLOCK/KM_SCROLLLOCK are better left clear than guessed at.
 */
struct FVaCuusModifierState
{
	bool bControlDown = false;
	bool bShiftDown = false;
	bool bAltDown = false;
	bool bCommandDown = false;
	bool bCapsLock = false;
};

/**
 * One unit of game-thread -> UI-thread input.
 *
 * DELIBERATELY RmlUi-FREE, and therefore public: the producer is
 * SVaCuusWidget over in VaCuusRender, and every translation into RmlUi's own
 * vocabulary (FKey -> Rml::Input::KeyIdentifier, modifier bits, button indices,
 * the wheel's sign and unit) happens on the UI thread in VaCuusInputMap. Two
 * reasons that split is worth the extra type: VaCuusRender needs no RmlUi
 * include for input at all, and every RmlUi-facing decision -- all of which are
 * asymmetric and easy to get wrong -- lives in one testable place instead of
 * being spread across Slate handlers.
 *
 * ROUTING mirrors FVaCuusUICommand: the event carries the ViewId it belongs to
 * and the UI thread looks that up in its host map. See FVaCuusUIQueues for why
 * one shared queue rather than one per view.
 *
 * CHEAP, AND ALLOCATION-FREE FOR EVERY KIND EXCEPT THE IME ONES. An FKey is an FName plus
 * flags and the rest are scalars, so the pointer/mouse/key path never allocates beyond the
 * queue's own nodes. `Text` below is the single exception: it exists only for the two IME
 * kinds that carry a string, which arrive at composition rate (tens per session, not per
 * frame) and are moved rather than copied into the queue. That is why the type is
 * move-friendly rather than trivially copyable -- TSpscQueue's in-place variadic Enqueue and
 * TOptional-returning Dequeue both handle it, exactly as they already do for
 * FVaCuusUICommand's FString payload.
 */
struct FVaCuusInputEvent
{
	EVaCuusInputEventKind Kind = EVaCuusInputEventKind::None;

	/** View this event applies to; stamped by FVaCuusUIThread::EnqueueInput. */
	uint32 ViewId = 0;

	/** View-space pixels, top-left origin -- the same space the snapshot's rects use. */
	FIntPoint Position = FIntPoint::ZeroValue;

	FVaCuusModifierState Modifiers;

	/** KeyDown/KeyUp: the keyboard key. MouseDown/MouseUp: the mouse button. */
	FKey Key;

	/**
	 * MouseWheel only, in UE's convention: POSITIVE IS SCROLL-UP
	 * (FPointerEvent::GetWheelDelta), and one notch is 1.0. RmlUi's convention is
	 * the opposite sign and its own unit, so the flip and the scaling happen at
	 * dispatch time -- deliberately not here, where a reader would have to guess
	 * which convention a stored value is already in.
	 */
	float WheelDelta = 0.0f;

	/**
	 * TextInput only: ONE COMPLETE UTF-32 CODE POINT, never a UTF-16 unit.
	 * Slate delivers characters as TCHAR, which is char16_t on this platform
	 * (PLATFORM_TCHAR_IS_CHAR16 is 1 for Unix), so anything above the BMP arrives as
	 * two consecutive OnKeyChar calls. The widget recombines the surrogate pair
	 * before it gets here, because RmlUi's `char` overload silently drops every byte
	 * above 127 (Context.cpp:553-557) and a raw cast of half a surrogate pair would
	 * insert garbage rather than fail.
	 */
	uint32 CodePoint = 0;

	/**
	 * Touch* only: WHICH FINGER, in the identifier space RmlUi keys its per-touch state on
	 * (Rml::Touch::identifier, Types.h:92-96 -- a `uintptr_t`, hence 64 bits here).
	 *
	 * COMPOSED, NEVER ASSIGNED: the factories below build it from the Slate pointer's
	 * (UserIndex, PointerIndex) pair and nothing else can, so a producer cannot invent an
	 * identifier that two fingers share. Both halves are load-bearing -- a touch puts every
	 * finger on its own PointerIndex (FSlateApplication::OnTouchStarted passes the finger
	 * index straight through, SlateApplication.cpp:6781-6787) while UserIndex is what keeps
	 * two local players' finger 0 apart (FInputEvent::GetUserIndex, Events.h:332).
	 *
	 * A MOUSE NEVER PRODUCES ONE. Mouse events all carry CursorPointerIndex
	 * (SlateApplication.cpp:6109-6118) and none of the mouse kinds reads this field, so
	 * there is no id space to collide in.
	 */
	uint64 TouchId = 0;

	//~ IME mutation payload. Unused (and untouched) by every other kind.

	/**
	 * Half-open range the mutation applies to, in **RmlUi UTF-8 CHARACTER OFFSETS** --
	 * already converted out of the engine's UTF-16 index space by the producer.
	 *
	 * CONVERTED ON THE GAME THREAD, DELIBERATELY, even though RmlUi lives on the UI thread:
	 * the conversion needs the field's text, and the game thread has an exact copy of it
	 * (FVaCuusTextFieldState::Value) in the same publish the OS's indices were derived from.
	 * Converting on the UI thread would use the LIVE value instead -- a different string
	 * whenever the field changed in between -- and silently shift the range.
	 */
	int32 RangeBegin = 0;
	int32 RangeEnd = 0;

	/**
	 * The value of FVaCuusTextFieldState::Generation the producer believed it was mutating.
	 * A mismatch on the UI thread means the platform's active field changed underneath the
	 * queue, and the event is dropped rather than applied to whatever is focused now.
	 */
	uint64 FieldGeneration = 0;

	/** ImeSetTextInRange / ImeCommitComposition: the replacement text (UTF-16 in, UTF-8 out). */
	FString Text;

	/**
	 * ImeSetSelectionRange: which end of the range the caret sits at
	 * (ITextInputMethodContext::ECaretPosition). RmlUi has no such concept -- its selection
	 * is a range plus a separate cursor -- so this becomes SetCursorPosition(begin) or
	 * SetCursorPosition(end) at dispatch.
	 */
	bool bCaretAtRangeBeginning = false;

	static FVaCuusInputEvent MouseMove(FIntPoint InPosition, const FVaCuusModifierState& InModifiers)
	{
		FVaCuusInputEvent Event;
		Event.Kind = EVaCuusInputEventKind::MouseMove;
		Event.Position = InPosition;
		Event.Modifiers = InModifiers;
		return Event;
	}

	static FVaCuusInputEvent MouseButton(
		bool bDown, FIntPoint InPosition, const FKey& InButton, const FVaCuusModifierState& InModifiers)
	{
		FVaCuusInputEvent Event;
		Event.Kind = bDown ? EVaCuusInputEventKind::MouseDown : EVaCuusInputEventKind::MouseUp;
		Event.Position = InPosition;
		Event.Key = InButton;
		Event.Modifiers = InModifiers;
		return Event;
	}

	static FVaCuusInputEvent MouseWheel(FIntPoint InPosition, float InWheelDelta, const FVaCuusModifierState& InModifiers)
	{
		FVaCuusInputEvent Event;
		Event.Kind = EVaCuusInputEventKind::MouseWheel;
		Event.Position = InPosition;
		Event.WheelDelta = InWheelDelta;
		Event.Modifiers = InModifiers;
		return Event;
	}

	static FVaCuusInputEvent MouseLeave()
	{
		FVaCuusInputEvent Event;
		Event.Kind = EVaCuusInputEventKind::MouseLeave;
		return Event;
	}

	//~ TOUCH FACTORIES. Four verbs, one shape, and TWO deliberate absences:
	//~
	//~ NO FKey PARAMETER. RmlUi's touch verbs hard-code button 0 internally
	//~ (Context.cpp:919, :1022, :1033) -- there is no other button a finger can
	//~ press -- so a signature that accepted one would accept something it then dropped on
	//~ the floor. Slate would happily supply it: every touch event names LeftMouseButton as
	//~ its effecting button, for every finger (Events.h:939-940).
	//~
	//~ NO ASSIGNABLE TouchId. MakeTouchId below is the only composer in the codebase, and
	//~ the id it returns is what these take -- so "two fingers accidentally sharing an id"
	//~ has one place it could be got wrong instead of one per call site. The id rather than
	//~ the (user, pointer) pair, because the CANCEL path has only an id: it replays fingers
	//~ recorded earlier, from a ledger, with no event in hand to re-derive them from.

	/** UserIndex/PointerIndex are FPointerEvent::GetUserIndex()/GetPointerIndex(); see TouchId. */
	static uint64 MakeTouchId(uint32 UserIndex, uint32 PointerIndex)
	{
		return (uint64(UserIndex) << 32) | uint64(PointerIndex);
	}

	static FVaCuusInputEvent Touch(
		EVaCuusInputEventKind InKind, uint64 InTouchId, FIntPoint InPosition, const FVaCuusModifierState& InModifiers)
	{
		checkf(InKind == EVaCuusInputEventKind::TouchStart || InKind == EVaCuusInputEventKind::TouchMove ||
				InKind == EVaCuusInputEventKind::TouchEnd,
			TEXT("FVaCuusInputEvent::Touch built with a kind that is not a positioned touch verb"));

		FVaCuusInputEvent Event;
		Event.Kind = InKind;
		Event.TouchId = InTouchId;
		Event.Position = InPosition;
		Event.Modifiers = InModifiers;
		return Event;
	}

	/**
	 * A cancel needs no position and no modifiers, and this signature is why that cannot be
	 * got wrong: ProcessTouchCancel takes no modifier state (Context.h:230) and does not move
	 * the pointer -- it drops the touch state and releases button 0, nothing else
	 * (Context.cpp:1031-1033). Passing a position here would imply a final move that never
	 * happens.
	 */
	static FVaCuusInputEvent TouchCancel(uint64 InTouchId)
	{
		FVaCuusInputEvent Event;
		Event.Kind = EVaCuusInputEventKind::TouchCancel;
		Event.TouchId = InTouchId;
		return Event;
	}

	static FVaCuusInputEvent KeyEvent(bool bDown, const FKey& InKey, const FVaCuusModifierState& InModifiers)
	{
		FVaCuusInputEvent Event;
		Event.Kind = bDown ? EVaCuusInputEventKind::KeyDown : EVaCuusInputEventKind::KeyUp;
		Event.Key = InKey;
		Event.Modifiers = InModifiers;
		return Event;
	}

	static FVaCuusInputEvent NavigateBack()
	{
		FVaCuusInputEvent Event;
		Event.Kind = EVaCuusInputEventKind::NavigateBack;
		return Event;
	}

	static FVaCuusInputEvent TextInput(uint32 InCodePoint, const FVaCuusModifierState& InModifiers)
	{
		FVaCuusInputEvent Event;
		Event.Kind = EVaCuusInputEventKind::TextInput;
		Event.CodePoint = InCodePoint;
		Event.Modifiers = InModifiers;
		return Event;
	}

	//~ IME factories. Ranges are RmlUi CHARACTER offsets (see RangeBegin) and every one of
	//~ them takes the generation, so forgetting the stamp is not expressible.

	static FVaCuusInputEvent ImeSetTextInRange(
		uint64 InFieldGeneration, int32 InRangeBegin, int32 InRangeEnd, FString InText)
	{
		FVaCuusInputEvent Event;
		Event.Kind = EVaCuusInputEventKind::ImeSetTextInRange;
		Event.FieldGeneration = InFieldGeneration;
		Event.RangeBegin = InRangeBegin;
		Event.RangeEnd = InRangeEnd;
		Event.Text = MoveTemp(InText);
		return Event;
	}

	static FVaCuusInputEvent ImeSetSelectionRange(
		uint64 InFieldGeneration, int32 InRangeBegin, int32 InRangeEnd, bool bInCaretAtRangeBeginning)
	{
		FVaCuusInputEvent Event;
		Event.Kind = EVaCuusInputEventKind::ImeSetSelectionRange;
		Event.FieldGeneration = InFieldGeneration;
		Event.RangeBegin = InRangeBegin;
		Event.RangeEnd = InRangeEnd;
		Event.bCaretAtRangeBeginning = bInCaretAtRangeBeginning;
		return Event;
	}

	static FVaCuusInputEvent ImeSetCompositionRange(uint64 InFieldGeneration, int32 InRangeBegin, int32 InRangeEnd)
	{
		FVaCuusInputEvent Event;
		Event.Kind = EVaCuusInputEventKind::ImeSetCompositionRange;
		Event.FieldGeneration = InFieldGeneration;
		Event.RangeBegin = InRangeBegin;
		Event.RangeEnd = InRangeEnd;
		return Event;
	}

	static FVaCuusInputEvent ImeCommitComposition(
		uint64 InFieldGeneration, int32 InRangeBegin, int32 InRangeEnd, FString InText)
	{
		FVaCuusInputEvent Event;
		Event.Kind = EVaCuusInputEventKind::ImeCommitComposition;
		Event.FieldGeneration = InFieldGeneration;
		Event.RangeBegin = InRangeBegin;
		Event.RangeEnd = InRangeEnd;
		Event.Text = MoveTemp(InText);
		return Event;
	}

	/**
	 * The whole-value push from a mobile virtual keyboard. NO RANGE ARGUMENT -- see the kind.
	 *
	 * The absent range is the enforcement: a caller cannot accidentally hand this an offset in
	 * the wrong index space, because there is nowhere to put one.
	 */
	static FVaCuusInputEvent VirtualKeyboardValue(uint64 InFieldGeneration, FString InText)
	{
		FVaCuusInputEvent Event;
		Event.Kind = EVaCuusInputEventKind::VirtualKeyboardValue;
		Event.FieldGeneration = InFieldGeneration;
		Event.Text = MoveTemp(InText);
		return Event;
	}
};
