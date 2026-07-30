// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusInputMap.h"

#include "VaCuusInputEvent.h"

namespace VaCuusInput
{
namespace
{
using Rml::Input::KeyIdentifier;

/**
 * Runs of contiguous identifiers, mapped by index.
 *
 * RmlUi's KeyIdentifier values are Win32 virtual-key-derived and the runs really
 * are contiguous (KI_0..KI_9 = 2..11, KI_A..KI_Z = 12..37, KI_NUMPAD0..9 = 51..60,
 * KI_F1..F24 = 107..130, Input.h:10-231), so indexing a run is both shorter and
 * less error-prone than sixty hand-written pairs -- one typo in the FKey array is
 * visible, one typo in a KI_ constant is not.
 */
template <int32 NumKeys>
void AddRun(TMap<FKey, KeyIdentifier>& Map, const FKey (&Keys)[NumKeys], KeyIdentifier First)
{
	for (int32 Index = 0; Index < NumKeys; ++Index)
	{
		Map.Add(Keys[Index], KeyIdentifier(int32(First) + Index));
	}
}

TMap<FKey, KeyIdentifier> BuildKeyMap()
{
	using namespace Rml::Input;

	TMap<FKey, KeyIdentifier> Map;
	Map.Reserve(128);

	static const FKey Digits[] = {EKeys::Zero, EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five,
		EKeys::Six, EKeys::Seven, EKeys::Eight, EKeys::Nine};
	AddRun(Map, Digits, KI_0);

	static const FKey Letters[] = {EKeys::A, EKeys::B, EKeys::C, EKeys::D, EKeys::E, EKeys::F, EKeys::G, EKeys::H,
		EKeys::I, EKeys::J, EKeys::K, EKeys::L, EKeys::M, EKeys::N, EKeys::O, EKeys::P, EKeys::Q, EKeys::R, EKeys::S,
		EKeys::T, EKeys::U, EKeys::V, EKeys::W, EKeys::X, EKeys::Y, EKeys::Z};
	AddRun(Map, Letters, KI_A);

	static const FKey NumPad[] = {EKeys::NumPadZero, EKeys::NumPadOne, EKeys::NumPadTwo, EKeys::NumPadThree,
		EKeys::NumPadFour, EKeys::NumPadFive, EKeys::NumPadSix, EKeys::NumPadSeven, EKeys::NumPadEight,
		EKeys::NumPadNine};
	AddRun(Map, NumPad, KI_NUMPAD0);

	// UE stops at F12; KI_F13..KI_F24 exist but are unreachable from an FKey.
	static const FKey Function[] = {EKeys::F1, EKeys::F2, EKeys::F3, EKeys::F4, EKeys::F5, EKeys::F6, EKeys::F7,
		EKeys::F8, EKeys::F9, EKeys::F10, EKeys::F11, EKeys::F12};
	AddRun(Map, Function, KI_F1);

	// Editing and navigation.
	Map.Add(EKeys::BackSpace, KI_BACK);
	Map.Add(EKeys::Tab, KI_TAB);
	Map.Add(EKeys::Enter, KI_RETURN);
	Map.Add(EKeys::Pause, KI_PAUSE);
	Map.Add(EKeys::CapsLock, KI_CAPITAL);
	Map.Add(EKeys::Escape, KI_ESCAPE);
	Map.Add(EKeys::SpaceBar, KI_SPACE);
	Map.Add(EKeys::PageUp, KI_PRIOR);
	Map.Add(EKeys::PageDown, KI_NEXT);
	Map.Add(EKeys::End, KI_END);
	Map.Add(EKeys::Home, KI_HOME);
	Map.Add(EKeys::Left, KI_LEFT);
	Map.Add(EKeys::Up, KI_UP);
	Map.Add(EKeys::Right, KI_RIGHT);
	Map.Add(EKeys::Down, KI_DOWN);
	Map.Add(EKeys::Insert, KI_INSERT);
	Map.Add(EKeys::Delete, KI_DELETE);

	// The Mac/Linux "forward delete" alias for the same physical key.
	Map.Add(EKeys::Platform_Delete, KI_DELETE);

	// Numpad operators and locks.
	Map.Add(EKeys::Multiply, KI_MULTIPLY);
	Map.Add(EKeys::Add, KI_ADD);
	Map.Add(EKeys::Subtract, KI_SUBTRACT);
	Map.Add(EKeys::Decimal, KI_DECIMAL);
	Map.Add(EKeys::Divide, KI_DIVIDE);
	Map.Add(EKeys::NumLock, KI_NUMLOCK);
	Map.Add(EKeys::ScrollLock, KI_SCROLL);

	// Modifier keys as keys in their own right: RmlUi dispatches keydown for them
	// too, and a document may well style on them.
	Map.Add(EKeys::LeftShift, KI_LSHIFT);
	Map.Add(EKeys::RightShift, KI_RSHIFT);
	Map.Add(EKeys::LeftControl, KI_LCONTROL);
	Map.Add(EKeys::RightControl, KI_RCONTROL);
	Map.Add(EKeys::LeftAlt, KI_LMENU);
	Map.Add(EKeys::RightAlt, KI_RMENU);
	Map.Add(EKeys::LeftCommand, KI_LMETA);
	Map.Add(EKeys::RightCommand, KI_RMETA);

	// US-layout punctuation. RmlUi names these after the Win32 OEM virtual keys, so
	// the identifier describes a physical key position rather than the character it
	// produces -- the character itself arrives through the TextInput path.
	Map.Add(EKeys::Semicolon, KI_OEM_1);
	Map.Add(EKeys::Equals, KI_OEM_PLUS);
	Map.Add(EKeys::Comma, KI_OEM_COMMA);
	Map.Add(EKeys::Hyphen, KI_OEM_MINUS);
	Map.Add(EKeys::Underscore, KI_OEM_MINUS);
	Map.Add(EKeys::Period, KI_OEM_PERIOD);
	Map.Add(EKeys::Slash, KI_OEM_2);
	Map.Add(EKeys::Tilde, KI_OEM_3);
	Map.Add(EKeys::LeftBracket, KI_OEM_4);
	Map.Add(EKeys::Backslash, KI_OEM_5);
	Map.Add(EKeys::RightBracket, KI_OEM_6);
	Map.Add(EKeys::Apostrophe, KI_OEM_7);

	return Map;
}

/**
 * Built on first use and never rebuilt. A function-local static is what makes
 * that thread-safe without a lock: C++11 guarantees the initialization runs
 * exactly once even if two threads arrive together (which, given the UI thread
 * and the automation tests, is not purely theoretical).
 */
const TMap<FKey, KeyIdentifier>& GetKeyMap()
{
	static const TMap<FKey, KeyIdentifier> Map = BuildKeyMap();
	return Map;
}
}	 // namespace

Rml::Input::KeyIdentifier ToRmlKey(const FKey& Key)
{
	const KeyIdentifier* Found = GetKeyMap().Find(Key);
	return Found ? *Found : Rml::Input::KI_UNKNOWN;
}

int32 ToRmlModifiers(const FVaCuusModifierState& Modifiers)
{
	int32 Mask = 0;

	if (Modifiers.bControlDown)
	{
		Mask |= Rml::Input::KM_CTRL;
	}
	if (Modifiers.bShiftDown)
	{
		// Load-bearing: ElementDocument reads the "shift_key" event parameter to pick
		// the direction of Tab navigation (ElementDocument.cpp:581). Drop this bit and
		// Shift-Tab silently stops going backwards while forward Tab keeps working.
		Mask |= Rml::Input::KM_SHIFT;
	}
	if (Modifiers.bAltDown)
	{
		Mask |= Rml::Input::KM_ALT;
	}
	if (Modifiers.bCommandDown)
	{
		Mask |= Rml::Input::KM_META;
	}
	if (Modifiers.bCapsLock)
	{
		Mask |= Rml::Input::KM_CAPSLOCK;
	}

	// KM_NUMLOCK and KM_SCROLLLOCK are deliberately never set: FInputEvent exposes
	// no state for them, and a wrong bit is worse than a missing one.
	return Mask;
}

int32 ToRmlMouseButton(const FKey& Button)
{
	if (Button == EKeys::LeftMouseButton)
	{
		// The only index RmlUi does focus, active, drag and double-click processing
		// for (Context.cpp:621-723).
		return 0;
	}
	if (Button == EKeys::RightMouseButton)
	{
		return 1;
	}
	if (Button == EKeys::MiddleMouseButton)
	{
		return 2;
	}

	// Thumb buttons and anything else: RmlUi has no meaning for indices above 2
	// (index 2 is what starts autoscroll), so the event is dropped rather than
	// dispatched under a number the document cannot interpret.
	return INDEX_NONE;
}

EMouseCursor::Type RmlCursorNameToSlate(const Rml::String& CursorName)
{
	// Empty is the common case, not an error: RmlUi sends it whenever nothing under
	// the mouse specifies `cursor`, and it means "the default arrow".
	if (CursorName.empty() || CursorName == "arrow")
	{
		return EMouseCursor::Default;
	}
	if (CursorName == "pointer")
	{
		return EMouseCursor::Hand;
	}
	if (CursorName == "text")
	{
		return EMouseCursor::TextEditBeam;
	}
	if (CursorName == "move")
	{
		return EMouseCursor::CardinalCross;
	}
	if (CursorName == "resize")
	{
		// RmlUi's single `resize` is the bottom-right corner grip (Win32 maps it to
		// IDC_SIZENWSE); it has no per-edge variants to map onto ResizeLeftRight/UpDown.
		return EMouseCursor::ResizeSouthEast;
	}
	if (CursorName == "cross")
	{
		return EMouseCursor::Crosshairs;
	}
	if (CursorName == "unavailable")
	{
		return EMouseCursor::SlashedCircle;
	}

	// The autoscroll controller synthesises names like "rmlui-scroll-both" and
	// "rmlui-scroll-idle" while a middle-drag is active; every RmlUi backend shows
	// its move cursor for the whole family.
	if (CursorName.rfind("rmlui-scroll", 0) == 0)
	{
		return EMouseCursor::CardinalCross;
	}

	return EMouseCursor::Default;
}
}	 // namespace VaCuusInput
