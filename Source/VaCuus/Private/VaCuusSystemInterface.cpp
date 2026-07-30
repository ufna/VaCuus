// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusSystemInterface.h"

#include "VaCuusDefines.h"
#include "VaCuusInputMap.h"
#include "VaCuusInteractiveSnapshot.h"
#include "VaCuusUIThread.h"

#include "HAL/PlatformTime.h"

namespace
{
/**
 * The cursor RmlUi last asked for, and a counter that moves only when it changed.
 *
 * WHY A GLOBAL LATCH FOR PER-VIEW STATE: RmlUi has exactly one SystemInterface for
 * the whole process (it is library state, not context state), but `cursor` is
 * resolved per context from its own hover element -- and each Rml::Context keeps
 * its own `cursor_name` and only calls out when *its* value changes
 * (Context.cpp:1324-1327). Since the UI thread updates views strictly one at a
 * time, a change observed immediately after view V's Update() is by construction
 * view V's. Each host therefore compares the serial right after its own Update()
 * and adopts the name only if it moved, which keeps N views from inheriting each
 * other's cursor.
 *
 * Plain (non-atomic) statics: written and read only from the one thread allowed to
 * call into RmlUi at all.
 */
EMouseCursor::Type GLatchedCursor = EMouseCursor::Default;
uint64 GLatchedCursorSerial = 0;

/**
 * The caret RmlUi last asked the keyboard to follow, and its own serial.
 *
 * SAME SHAPE AND SAME JUSTIFICATION AS THE CURSOR LATCH ABOVE, and the mismatch is if
 * anything sharper: `ActivateKeyboard` carries no context and no element (SystemInterface.h:59),
 * so this one global is the whole channel for every context in the process. The host
 * attributes it (serial moved AND my own context has a text control focused); see
 * GetVaCuusLatchedCaret's comment for why nothing better exists at 0ae381e.
 *
 * Plain (non-atomic) statics: written and read only from the one thread allowed to call
 * into RmlUi at all.
 */
FVaCuusCaretLatch GLatchedCaret;
uint64 GLatchedCaretSerial = 0;
}	 // namespace

EMouseCursor::Type GetVaCuusLatchedMouseCursor(uint64& OutSerial)
{
	check(FVaCuusUIThread::IsInUIThread());

	OutSerial = GLatchedCursorSerial;
	return GLatchedCursor;
}

FVaCuusSystemInterface::FVaCuusSystemInterface()
	: StartTime(FPlatformTime::Seconds())
{
}

double FVaCuusSystemInterface::GetElapsedTime()
{
	return FPlatformTime::Seconds() - StartTime;
}

/**
 * RmlUi'S OWN DIAGNOSTICS ARE NOT COMPILED OUT. THIS FUNCTION IS WHY, AND IT IS THE ONE PLACE
 * THAT CLAIM IS DECIDED -- every comment in the data-binding code that talks about what RmlUi
 * does or does not report cites this one.
 *
 * Rml::Log::Message is an ordinary function: it formats into a stack buffer and calls
 * GetSystemInterface()->LogMessage (Log.cpp:11-31). Nothing about it is conditional. So every
 * Log::Message(LT_ERROR/LT_WARNING, ...) anywhere in the library arrives HERE and leaves a
 * `LogVaCuus: Error: [Rml] ...` (or `Warning:`) line in the ordinary engine log.
 *
 * WHAT IS COMPILED OUT IS THE ASSERT FAMILY, AND ONLY THAT. RMLUI_ASSERT, RMLUI_ASSERTMSG,
 * RMLUI_ERROR and RMLUI_ERRORMSG expand to nothing unless RMLUI_DEBUG is defined
 * (Debug.h:45-50), and Platform.h:20-21 defines RMLUI_DEBUG only when NDEBUG is absent -- which
 * no configuration this plugin builds satisfies. RMLUI_LOG_TYPE_ERROR, the macro RmlUi's data
 * binding reports type mistakes through, is RMLUI_ERRORMSG (DataTypes.h:74) and appears only in
 * DataModelHandle.h, DataTypeRegister.h and DataStructHandle.h -- the TEMPLATED registration
 * path FVaCuusStructDefinition deliberately bypasses, so none of them was ever on our path.
 *
 * THE NARROWER CLAIM THAT SURVIVES, and the one the model code is written against: an RmlUi API
 * whose only failure signal is an assert is silent here, so VaCuus must check that precondition
 * itself. An API that LOGS is not silent, and must not be described as if it were.
 */
bool FVaCuusSystemInterface::LogMessage(Rml::Log::Type Type, const Rml::String& Message)
{
	const FString Text = UTF8_TO_TCHAR(Message.c_str());

	switch (Type)
	{
	case Rml::Log::LT_ERROR:
	case Rml::Log::LT_ASSERT:
		UE_LOG(LogVaCuus, Error, TEXT("[Rml] %s"), *Text);
		break;
	case Rml::Log::LT_WARNING:
		UE_LOG(LogVaCuus, Warning, TEXT("[Rml] %s"), *Text);
		break;
	case Rml::Log::LT_ALWAYS:
		UE_LOG(LogVaCuus, Display, TEXT("[Rml] %s"), *Text);
		break;
	case Rml::Log::LT_INFO:
		UE_LOG(LogVaCuus, Log, TEXT("[Rml] %s"), *Text);
		break;
	case Rml::Log::LT_DEBUG:
	default:
		UE_LOG(LogVaCuus, Verbose, TEXT("[Rml] %s"), *Text);
		break;
	}

	// Continue execution (returning false asks RmlUi to break into the debugger).
	return true;
}

void FVaCuusSystemInterface::SetMouseCursor(const Rml::String& CursorName)
{
	const EMouseCursor::Type Cursor = VaCuusInput::RmlCursorNameToSlate(CursorName);

	// The serial advances on every call, not on every distinct cursor: RmlUi already
	// filters out unchanged names, and "the name changed but maps to the same Slate
	// cursor" still has to be recorded as an observation, or a later view's change
	// would be attributed to whichever view happened to push last.
	GLatchedCursor = Cursor;
	++GLatchedCursorSerial;

	UE_LOG(LogVaCuus, Verbose, TEXT("[Rml] cursor '%s' -> EMouseCursor %d (serial %llu)"),
		UTF8_TO_TCHAR(CursorName.c_str()), int32(Cursor), GLatchedCursorSerial);
}

FVaCuusCaretLatch GetVaCuusLatchedCaret(uint64& OutSerial)
{
	check(FVaCuusUIThread::IsInUIThread());

	OutSerial = GLatchedCaretSerial;
	return GLatchedCaret;
}

void FVaCuusSystemInterface::ActivateKeyboard(Rml::Vector2f CaretPosition, float LineHeight)
{
	// No UI-thread assert here on purpose: this is called from inside RmlUi, so being on
	// any other thread is impossible by construction, and an assert on a path RmlUi takes
	// a dozen times per keystroke is cost for nothing.
	GLatchedCaret.Position = FVector2f(CaretPosition.x, CaretPosition.y);
	GLatchedCaret.LineHeight = LineHeight;
	GLatchedCaret.bActive = true;

	// The serial advances on EVERY call, not on every distinct position -- same rule as the
	// cursor: a repeat at the same place is still an observation, and swallowing it would
	// let a later view's caret be attributed to whichever view pushed last.
	++GLatchedCaretSerial;
}

void FVaCuusSystemInterface::DeactivateKeyboard()
{
	// Position is deliberately kept: bActive false is what the consumer tests, and a
	// stale position that nothing reads is cheaper than a branch that clears it.
	GLatchedCaret.bActive = false;
	++GLatchedCaretSerial;
}
