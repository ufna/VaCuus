// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "GenericPlatform/ICursor.h"
#include "InputCoreTypes.h"

#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Types.h>

struct FVaCuusModifierState;

/**
 * UE input identities -> RmlUi's own. Private to VaCuus on purpose: the module
 * boundary is what keeps every RmlUi-facing convention in one place, so the Slate
 * side never has to know that RmlUi numbers its mouse buttons from zero, inverts
 * the wheel, or names its cursors after CSS.
 *
 * Pure functions, no state beyond one lazily-built table, callable from any
 * thread. In practice the UI thread calls them while draining input; the
 * VaCuus.Input.KeyMap test calls them from the test thread.
 */
namespace VaCuusInput
{
/**
 * The key identifier RmlUi knows this FKey as, or KI_UNKNOWN.
 *
 * KI_UNKNOWN IS A NORMAL ANSWER, not a failure: UE has keys RmlUi has no
 * identifier for at all (localized layouts contribute Ampersand, Caret, Section,
 * accented letters; there are also axes and pseudo-keys like AnyKey). Those still
 * reach a document through the TextInput path, which is where a *character*
 * belongs -- a key identifier is for navigation and shortcuts.
 *
 * Mapped from FKey rather than from FKeyEvent::GetKeyCode(): the key code is the
 * platform virtual key and differs between Linux, Windows and Mac, while FKey is
 * stable everywhere.
 */
Rml::Input::KeyIdentifier ToRmlKey(const FKey& Key);

/** The `int key_modifier_state` mask every Context::Process* call takes. */
int32 ToRmlModifiers(const FVaCuusModifierState& Modifiers);

/** RmlUi's button index (Left 0, Right 1, Middle 2), or INDEX_NONE if it has none. */
int32 ToRmlMouseButton(const FKey& Button);

/**
 * An RmlUi `cursor` property value -> a Slate cursor.
 *
 * The property is a free-form string that RmlUi passes through verbatim, so the
 * recognised set is a convention rather than an enum: these are the names every
 * RmlUi backend handles (RmlUi_Platform_Win32.cpp:55-70), plus the `rmlui-scroll*`
 * family the autoscroll controller generates. Anything unrecognised -- including
 * the empty string RmlUi sends when nothing under the mouse asks for a cursor --
 * is the default arrow.
 */
EMouseCursor::Type RmlCursorNameToSlate(const Rml::String& CursorName);
}	 // namespace VaCuusInput
