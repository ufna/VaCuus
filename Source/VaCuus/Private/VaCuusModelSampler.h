// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusModelLayout.h"
#include "VaCuusModelShadow.h"

class FVaCuusModelChannel;
class UScriptStruct;

/**
 * The game-thread half of a bound model: read the live struct, decide field by field what
 * changed since last frame, mark those fields on the channel, and keep a shadow of what was
 * last seen.
 *
 *
 * WHY IT DIFFS AT ALL. There is no "has this property changed" anywhere in UE
 * (research ue-reflection 4.2): FieldNotify is opt-in and push-based, RepNotify is
 * replication-only, and PostEditChangeProperty is inside #if WITH_EDITOR. Every changed-value
 * system in the engine keeps a shadow buffer and compares against it, and FRepLayout's flat
 * pre-resolved command list is the shape this copies (RepLayout.cpp:668-753).
 *
 *
 * WHY THE COMPARISON IS PER KIND AND NOT FProperty::Identical.
 *
 * Two of the eleven kinds get a WRONG ANSWER out of the generic path, and the rest get a
 * needlessly slow one:
 *
 *  - FTextProperty::Identical picks EIdenticalLexicalCompareMethod::None when !GIsEditor
 *    (TextProperty.cpp:63-67), i.e. exactly in the builds that ship, and then two texts with
 *    identical display strings but different identities are "not equal" -- so a HUD label
 *    rebuilt each frame from the same string would publish, dirty a variable and re-run its
 *    views EVERY FRAME. That is spec 9's idle row lost to a comparison, not to a change.
 *  - FStructProperty::Identical answers for a whole nested struct at once, so `Origin` would
 *    carry one dirty bit and none of the per-leaf rules below would run. Nesting is flattened
 *    at layout-build time precisely so this is never reached. (Its COST is a weaker argument
 *    than it looks: Identical forwards to UScriptStruct::CompareScriptStruct
 *    (PropertyStruct.cpp:139-142), which short-circuits through ICppStructOps::Identical for
 *    any STRUCT_IdenticalNative type -- FVector, FVector2D, FQuat -- and never reaches the
 *    TFieldIterator at all. FVaCuusModelLayout's header has the full correction.)
 *
 * And two more get a wrong answer out of the OBVIOUS hand-written comparison, which is what
 * the per-kind switch is really defending against -- see the .cpp: FString::operator== is
 * case-INSENSITIVE, and FName::operator== compares the case-insensitive comparison index.
 * Both ship the display form to RmlUi, so both can change visibly while comparing equal.
 *
 *
 * THE SHADOW IS A PROJECTION, NOT A PHOTOCOPY, AND EXACTLY ONE KIND MAKES IT SO.
 *
 * An FText field is stored as FText::AsCultureInvariant(Live.ToString()) rather than as a
 * copy of the live FText. That decision is where FText::ToString() -- the one read on this
 * path that touches process-global localization state instead of plain value data -- is
 * pinned to the game thread:
 *
 *   FText::ToString() calls Rebuild() (Text.cpp:1342-1346), which is
 *   FTextHistory::UpdateDisplayStringIfOutOfDate (TextHistory.cpp:705-721). For a text with a
 *   TextId that asks FTextLocalizationManager for the current revisions -- whose own comment
 *   is "GlobalRevision and LocalRevision can be updated by concurrent threads!" -- takes the
 *   history's mutex and, when the culture has moved, REPLACES the shared LocalizedString the
 *   display string is a reference into. A copy of the live FText shares that history with
 *   gameplay, so the UI thread reading it during a culture change would be reading an FString
 *   another thread is swapping out from under it.
 *
 *   A text projected here has an EMPTY TextId, so FTextHistory_Base::CanUpdateDisplayString
 *   returns false (TextHistory.cpp:940-943), Rebuild does nothing at all, and
 *   GetDisplayString() is a plain read of an FString the FText owns -- `LocalizedString ?
 *   *LocalizedString : SourceString`, and LocalizedString stays null because only
 *   UpdateDisplayString ever sets it (TextHistory.cpp:794-797, :945-957).
 *   The UI thread's read becomes pure value data, which is the property every other kind
 *   already has.
 *
 * The obvious objection -- "a frozen display string will not follow a culture change" -- does
 * not hold: a culture change moves the LIVE text's display string, the next sample compares
 * display strings, sees the difference and republishes. The freeze costs one FString copy and
 * one small allocation per CHANGED text, never per frame.
 *
 *
 * THREADING. Game thread only, asserted, and the reason is not convention: instance data has
 * no engine synchronisation whatsoever (research ue-reflection 6.3), so this reads gameplay
 * memory that only the game thread may be reading. The spec also requires it be driven from
 * UVaCuusSubsystem::Tick, because that is what lands it inside the existing GameTick perf
 * scope (VaCuusSubsystem.cpp:68); from an actor tick or a Blueprint node it would be outside
 * every scope and the budget would become an inference on top of an inference.
 */
class FVaCuusModelSampler
{
public:
	/** InLayout is BORROWED and must outlive this sampler; the model object owns both. */
	explicit FVaCuusModelSampler(const FVaCuusModelLayout& InLayout);

	FVaCuusModelSampler(const FVaCuusModelSampler&) = delete;
	FVaCuusModelSampler& operator=(const FVaCuusModelSampler&) = delete;

	/** False when the layout had no struct, or the shadow could not be allocated. */
	bool IsValid() const { return Shadow.IsValid(); }

	/**
	 * Diffs LiveData against the shadow, updates the shadow for every field that moved, and
	 * marks those fields on Channel.
	 *
	 * @param LiveType the type of LiveData. A PARAMETER, not an assumption, for spec 7's
	 *        reason applied one level down: this function is where the layout's offsets meet
	 *        raw memory, so "wrong type" and "wrong size" are undiagnosable exactly here, and
	 *        the first FString field turns an undiagnosed mismatch into a crash.
	 * @return the number of fields marked, i.e. how many actually changed.
	 */
	int32 Sample(const UScriptStruct* LiveType, const void* LiveData, FVaCuusModelChannel& Channel);

	/**
	 * What the sampler last saw, and what FVaCuusModelChannel::Publish must read from.
	 *
	 * NOT INTERCHANGEABLE WITH THE LIVE STRUCT even though every value in it is equal to the
	 * live one: the FText fields here are the projected, culture-invariant form (see above),
	 * and publishing from the live struct would put a localized FText -- one whose display
	 * string the UI thread would then have to resolve -- into the channel.
	 */
	const FVaCuusModelShadow& GetShadow() const { return Shadow; }

	/** Samples taken, and fields marked across all of them. Observables for the dump command and the tests. */
	uint64 GetNumSamples() const { return NumSamples; }
	uint64 GetNumFieldsMarked() const { return NumFieldsMarked; }

private:
	/** Borrowed; see the constructor. */
	const FVaCuusModelLayout& Layout;

	/** The previous-value shadow: a real instance of the model type, game-thread-owned. */
	FVaCuusModelShadow Shadow;

	uint64 NumSamples = 0;
	uint64 NumFieldsMarked = 0;
};
