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

	/** Key + Modifiers. Distinct from TextInput: this is the navigation/shortcut path. */
	KeyDown,
	KeyUp,

	/** CodePoint. The typing path, and the only one that produces characters. */
	TextInput
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
 * Trivially copyable and cheap (an FKey is an FName plus flags); nothing here
 * owns memory, so the queue never allocates beyond its own nodes.
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

	static FVaCuusInputEvent KeyEvent(bool bDown, const FKey& InKey, const FVaCuusModifierState& InModifiers)
	{
		FVaCuusInputEvent Event;
		Event.Kind = bDown ? EVaCuusInputEventKind::KeyDown : EVaCuusInputEventKind::KeyUp;
		Event.Key = InKey;
		Event.Modifiers = InModifiers;
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
};
