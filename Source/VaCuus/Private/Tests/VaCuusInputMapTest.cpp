// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusInputEvent.h"
#include "VaCuusInputMap.h"

#include "InputCoreTypes.h"

#include <RmlUi/Core/Input.h>

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusInputMapTest
{
struct FKeyExpectation
{
	FKey Key;
	Rml::Input::KeyIdentifier Expected;
	const TCHAR* Description;
};

struct FCursorExpectation
{
	const char* Name;
	EMouseCursor::Type Expected;
	const TCHAR* Description;
};
}	 // namespace VaCuusInputMapTest

/**
 * The translation layer between UE input identities and RmlUi's own. Pure
 * functions, no RmlUi context and no threads -- which is exactly why it is worth
 * testing directly: every one of these values is a silent-wrong-behaviour bug
 * (a key that types the wrong character, a modifier that breaks shift-Tab, a
 * cursor that never changes shape) rather than a crash.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusInputMapTest, "VaCuus.Input.KeyMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusInputMapTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusInputMapTest;
	using namespace Rml::Input;

	// One representative from every band of the table: a letter, a digit, a numpad
	// key, an editing key, an arrow, a function key, and the three keys whose
	// identifiers RmlUi's own default actions read (Tab, Return, Space).
	const FKeyExpectation Keys[] = {
		{EKeys::A, KI_A, TEXT("A")},
		{EKeys::Z, KI_Z, TEXT("Z")},
		{EKeys::Zero, KI_0, TEXT("Zero")},
		{EKeys::Nine, KI_9, TEXT("Nine")},
		{EKeys::NumPadFive, KI_NUMPAD5, TEXT("NumPadFive")},
		{EKeys::Escape, KI_ESCAPE, TEXT("Escape")},
		{EKeys::Left, KI_LEFT, TEXT("Left")},
		{EKeys::Down, KI_DOWN, TEXT("Down")},
		{EKeys::F5, KI_F5, TEXT("F5")},
		{EKeys::SpaceBar, KI_SPACE, TEXT("SpaceBar")},
		{EKeys::Enter, KI_RETURN, TEXT("Enter")},
		{EKeys::BackSpace, KI_BACK, TEXT("BackSpace")},
		{EKeys::Tab, KI_TAB, TEXT("Tab")},
		{EKeys::LeftShift, KI_LSHIFT, TEXT("LeftShift")},
		{EKeys::RightControl, KI_RCONTROL, TEXT("RightControl")},
		{EKeys::Delete, KI_DELETE, TEXT("Delete")},
		{EKeys::Semicolon, KI_OEM_1, TEXT("Semicolon")},
	};

	for (const FKeyExpectation& Expectation : Keys)
	{
		TestEqual(FString::Printf(TEXT("%s maps to its RmlUi key identifier"), Expectation.Description),
			int32(VaCuusInput::ToRmlKey(Expectation.Key)), int32(Expectation.Expected));
	}

	// Everything RmlUi has no identifier for must land on KI_UNKNOWN rather than on
	// whatever happens to be next in the enum. Printable oddities from localized
	// layouts still reach the document through ProcessTextInput; axes and the
	// catch-all pseudo-key have no business being sent at all.
	const FKeyExpectation Unmapped[] = {
		{EKeys::Ampersand, KI_UNKNOWN, TEXT("Ampersand (AZERTY, no KI_ equivalent)")},
		{EKeys::Section, KI_UNKNOWN, TEXT("Section")},
		{EKeys::MouseX, KI_UNKNOWN, TEXT("MouseX (an axis)")},
		{EKeys::AnyKey, KI_UNKNOWN, TEXT("AnyKey (a pseudo-key)")},
		{FKey(), KI_UNKNOWN, TEXT("An invalid FKey")},
	};

	for (const FKeyExpectation& Expectation : Unmapped)
	{
		TestEqual(FString::Printf(TEXT("%s maps to KI_UNKNOWN"), Expectation.Description),
			int32(VaCuusInput::ToRmlKey(Expectation.Key)), int32(KI_UNKNOWN));
	}

	// Modifier composition. KM_SHIFT is the load-bearing one: ElementDocument reads
	// the "shift_key" event parameter to decide reverse tabbing (ElementDocument.cpp:581),
	// so dropping the bit silently breaks Shift-Tab while forward Tab keeps working.
	{
		FVaCuusModifierState None;
		TestEqual(TEXT("No modifiers compose to 0"), VaCuusInput::ToRmlModifiers(None), 0);

		FVaCuusModifierState CtrlShift;
		CtrlShift.bControlDown = true;
		CtrlShift.bShiftDown = true;
		TestEqual(TEXT("Ctrl+Shift composes to KM_CTRL|KM_SHIFT"),
			VaCuusInput::ToRmlModifiers(CtrlShift), int32(KM_CTRL | KM_SHIFT));

		FVaCuusModifierState All;
		All.bControlDown = true;
		All.bShiftDown = true;
		All.bAltDown = true;
		All.bCommandDown = true;
		All.bCapsLock = true;
		TestEqual(TEXT("Every modifier VaCuus can observe composes"),
			VaCuusInput::ToRmlModifiers(All), int32(KM_CTRL | KM_SHIFT | KM_ALT | KM_META | KM_CAPSLOCK));

		// FInputEvent exposes no NumLock/ScrollLock state, so those two bits must stay
		// clear rather than be guessed at.
		TestEqual(TEXT("NumLock and ScrollLock are never asserted"),
			VaCuusInput::ToRmlModifiers(All) & int32(KM_NUMLOCK | KM_SCROLLLOCK), 0);
	}

	// Mouse buttons: RmlUi only gives meaning to 0/1/2 (and only 0 does focus,
	// active, drag and double-click processing).
	{
		TestEqual(TEXT("Left mouse button is RmlUi button 0"),
			VaCuusInput::ToRmlMouseButton(EKeys::LeftMouseButton), 0);
		TestEqual(TEXT("Right mouse button is RmlUi button 1"),
			VaCuusInput::ToRmlMouseButton(EKeys::RightMouseButton), 1);
		TestEqual(TEXT("Middle mouse button is RmlUi button 2"),
			VaCuusInput::ToRmlMouseButton(EKeys::MiddleMouseButton), 2);
		TestEqual(TEXT("A thumb button has no RmlUi equivalent"),
			VaCuusInput::ToRmlMouseButton(EKeys::ThumbMouseButton), int32(INDEX_NONE));
		TestEqual(TEXT("A non-mouse key has no RmlUi button"),
			VaCuusInput::ToRmlMouseButton(EKeys::A), int32(INDEX_NONE));
	}

	// Cursor names. RmlUi's `cursor` property is a free-form string and the library
	// pushes it verbatim through SystemInterface::SetMouseCursor; the names below are
	// the set every RmlUi backend recognises (RmlUi_Platform_Win32.cpp:55-70).
	{
		const FCursorExpectation Cursors[] = {
			{"", EMouseCursor::Default, TEXT("no cursor specified")},
			{"arrow", EMouseCursor::Default, TEXT("arrow")},
			{"pointer", EMouseCursor::Hand, TEXT("pointer")},
			{"text", EMouseCursor::TextEditBeam, TEXT("text")},
			{"move", EMouseCursor::CardinalCross, TEXT("move")},
			{"resize", EMouseCursor::ResizeSouthEast, TEXT("resize")},
			{"cross", EMouseCursor::Crosshairs, TEXT("cross")},
			{"unavailable", EMouseCursor::SlashedCircle, TEXT("unavailable")},
			{"rmlui-scroll-both", EMouseCursor::CardinalCross, TEXT("an autoscroll cursor")},
			{"nonsense", EMouseCursor::Default, TEXT("an unrecognised name")},
		};

		for (const FCursorExpectation& Expectation : Cursors)
		{
			TestEqual(FString::Printf(TEXT("Cursor '%s' maps to the expected Slate cursor"), Expectation.Description),
				int32(VaCuusInput::RmlCursorNameToSlate(Expectation.Name)), int32(Expectation.Expected));
		}
	}

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
