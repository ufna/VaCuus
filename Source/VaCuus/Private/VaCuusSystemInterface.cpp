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
