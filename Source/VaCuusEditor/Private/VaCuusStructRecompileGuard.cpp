// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusStructRecompileGuard.h"

#include "VaCuusSubsystem.h"

void FVaCuusStructRecompileGuard::PreChange(
	const UUserDefinedStruct* Changed, FStructureEditorUtils::EStructureEditorChangeInfo ChangedType)
{
	// THE ONE EXEMPT REASON, taken from the compiler's own exemption rather than invented:
	// CleanAndSanitizeStruct skips the property teardown ONLY for DefaultValueChanged
	// (UserDefinedStructureCompilerUtils.cpp:221-232 -- the `!= DefaultValueChanged` guard
	// around DestroyChildPropertiesAndResetPropertyLinks). No properties die, so nothing a
	// model resolved dangles, and killing live models over a tweaked default would punish
	// the most common edit there is. Every OTHER reason -- Unknown included, since the guard
	// in the compiler treats it as destructive too -- tears the chain down and gets the
	// refusal.
	if (ChangedType == FStructureEditorUtils::DefaultValueChanged)
	{
		return;
	}

	// Everything else is the runtime module's: the walk over every game instance's views,
	// the per-model condemnation and Error, and the fenced UI-side teardown -- see
	// UVaCuusSubsystem::NotifyStructPreRecompile. PreChange broadcasts on the game thread
	// (the struct editor and the compiler run there), which that entry point asserts.
	UVaCuusSubsystem::NotifyStructPreRecompile(Changed);
}

void FVaCuusStructRecompileGuard::PostChange(
	const UUserDefinedStruct* Changed, FStructureEditorUtils::EStructureEditorChangeInfo ChangedType)
{
	// Nothing. The refusal design has no "rebuild on PostChange" half ON PURPOSE: RmlUi
	// retains a raw void* into the UI shadow and revalidates it never, so the only sound
	// recovery is the one the refusal Error already names -- re-bind (BindModel replaces a
	// dead entry) and reload the document. A silent automatic rebuild here would have to
	// re-run exactly that pair anyway, from a callback that cannot know whether the game
	// still wants the model.
}
