// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusModelSampler.h"

#include "VaCuusDefines.h"
#include "VaCuusModelChannel.h"

#include "UObject/AnsiStrProperty.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/StrProperty.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include "UObject/Utf8StrProperty.h"

// UE_FORCEINLINE_HINT is 5.8's per-target-configurable FORCEINLINE (HAL/Platform.h:766-776):
// it expands to FORCEINLINE unless the target defines UE_DEFINE_FORCEINLINE_HINT_TO_INLINE,
// in which case a plain `inline` lets the compiler (or PGO) decide. 5.6 has no such macro --
// grepping all of its Runtime/Core for the name returns nothing -- so define the same default
// there. #ifndef and not a version test: the guard is inert on any engine that has the macro,
// so a 5.7 either way needs no edit, and if a target opts into the `inline` form the engine's
// own definition still wins.
#ifndef UE_FORCEINLINE_HINT
#define UE_FORCEINLINE_HINT FORCEINLINE
#endif

namespace VaCuusModelSamplerPrivate
{
/**
 * The value pointer for one field in one instance of the model type.
 *
 * TWO STEPS, AND NEITHER IS OPTIONAL. ContainerPtr applies the flattening offset -- the base
 * of the struct that DIRECTLY contains this leaf -- and ContainerPtrToValuePtr then applies
 * Offset_Internal, which is relative to that container. Doing the second by hand is what
 * FProperty's own GetOffset_ReplaceWith_ContainerPtrToValuePtr (UnrealType.h:466) exists to
 * shame, and it is also the form that stays correct for a bitfield, whose value pointer is
 * the storage integer rather than the bit.
 */
static UE_FORCEINLINE_HINT const void* ValuePtr(const FVaCuusModelField& Field, const void* StructBase)
{
	return Field.Property->ContainerPtrToValuePtr<void>(Field.ContainerPtr(StructBase));
}

static UE_FORCEINLINE_HINT void* ValuePtr(const FVaCuusModelField& Field, void* StructBase)
{
	return Field.Property->ContainerPtrToValuePtr<void>(Field.ContainerPtr(StructBase));
}

/** The enum's underlying integer, in the two shapes an enum property comes in. */
static int64 ReadEnumValue(const FProperty* Property, const void* InValuePtr)
{
	// An FEnumProperty wraps an underlying integer property; a TEnumAsByte is an FByteProperty
	// that merely CARRIES a UEnum (UnrealType.h:2195, 2253). Only the first has
	// GetUnderlyingProperty(). Same split as the RmlUi adapter's GetEnumName, and it has to be:
	// the name the document shows is a pure function of this integer, so comparing the integer
	// is exactly comparing the string, without building one.
	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		return EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(InValuePtr);
	}

	return static_cast<int64>(CastFieldChecked<FNumericProperty>(Property)->GetUnsignedIntPropertyValue(InValuePtr));
}

/**
 * True when the two soft references would render as different strings.
 *
 * NOT operator==, which is `AssetPath == Other.AssetPath && SubPathString == Other.SubPathString`
 * (SoftObjectPath.cpp) -- an FName compare (case-insensitive by construction) and an
 * FUtf8String compare, whose operator== is Stricmp (UnrealString.h.inl:912-915). What the UI
 * ships is FSoftObjectPtr::ToString() (SoftObjectPtr.h:102-105), i.e. the DISPLAY form of
 * those names, so a path that differs only in case renders differently and compares equal.
 *
 * Spelled out rather than compared through ToString() because ToString() builds an FString:
 * two heap allocations per soft-reference field per frame, on a path budgeted at 0.02 ms.
 */
static bool SoftPathsDiffer(const FSoftObjectPath& A, const FSoftObjectPath& B)
{
	return !A.GetAssetPath().GetPackageName().IsEqual(B.GetAssetPath().GetPackageName(), ENameCase::CaseSensitive)
		|| !A.GetAssetPath().GetAssetName().IsEqual(B.GetAssetPath().GetAssetName(), ENameCase::CaseSensitive)
		|| !A.GetSubPathUtf8String().Equals(B.GetSubPathUtf8String(), ESearchCase::CaseSensitive);
}

/**
 * Has the value at this pointer pair changed, judged by kind -- THE per-kind comparator,
 * shared by its two call sites (spec 3.3): the field dispatch reaches it through
 * ContainerPtrToValuePtr, the array element loop through FScriptArrayHelper::GetRawPtr(i).
 * One implementation is what keeps an element rule from drifting away from its field rule:
 * divergence now requires editing shared code.
 *
 * ONE EXHAUSTIVE SWITCH, NO `default`: -Wswitch turns a new EVaCuusFieldKind into a compile
 * error here, which is the only mechanism that would stop a new kind from silently defaulting
 * to "never changes" -- a bound variable that is correct once and then frozen, with no log
 * line, forever. Same shape as the adapter's Get() and LexToString().
 *
 * The rule for every branch: compare WHAT THE UI ACTUALLY SHIPS. Anything cheaper that is not
 * a function of the shipped value can be equal while the screen would differ.
 */
static bool HasValueChanged(EVaCuusFieldKind Kind, const FProperty* Property, const void* LiveValue, const void* ShadowValue)
{
	switch (Kind)
	{
		case EVaCuusFieldKind::Bool:
		{
			// THROUGH THE MASK-AWARE ACCESSOR, NEVER A BYTE COMPARE. Two adjacent `uint8 b : 1`
			// declarations share BOTH the address a comparison would use and the length it would
			// use: DetermineBitfieldOffsetAndMask sets a bit in a scratch instance and takes the
			// offset of the BYTE it landed in (PropertyBool.cpp:98-117), which becomes
			// Offset_Internal (:40), while ElementSize comes from the declared type (:41 ->
			// SetBoolSize at :66-96). So a memcmp of the value bytes -- or of a scratch buffer
			// copied out of them -- reports THIS field as changed whenever its NEIGHBOUR changes.
			// Every such false positive is a publish, a DirtyVariable and a view re-evaluation
			// for a value that did not move. GetPropertyValue is `!!(*ByteValue & FieldMask)`
			// (UnrealType.h:2682-2687), with FieldMask 255 for a native bool and the isolated bit
			// otherwise (PropertyBool.cpp:76-93).
			//
			// At the ELEMENT call site this is always the native-bool path: a bitfield cannot
			// exist inside a container -- UHT refuses bool static arrays outright and a TArray
			// inner is a plain bool (UhtProperty.cs:2395-2398) -- so the same accessor is a
			// plain byte read there, mask 255.
			const FBoolProperty* BoolProperty = CastFieldChecked<FBoolProperty>(Property);
			return BoolProperty->GetPropertyValue(LiveValue) != BoolProperty->GetPropertyValue(ShadowValue);
		}

		case EVaCuusFieldKind::SignedInt:
		{
			const FNumericProperty* Numeric = CastFieldChecked<FNumericProperty>(Property);
			return Numeric->GetSignedIntPropertyValue(LiveValue) != Numeric->GetSignedIntPropertyValue(ShadowValue);
		}

		case EVaCuusFieldKind::UnsignedInt:
		{
			const FNumericProperty* Numeric = CastFieldChecked<FNumericProperty>(Property);
			return Numeric->GetUnsignedIntPropertyValue(LiveValue) != Numeric->GetUnsignedIntPropertyValue(ShadowValue);
		}

		case EVaCuusFieldKind::FloatingPoint:
		{
			// BITWISE, NOT `!=`, AND THE REASON IS NaN. `NaN != NaN` is true for every pair
			// including a value compared with itself, so a UI-bound float that goes NaN -- a
			// percentage over a zero maximum is the everyday way -- would be reported changed on
			// EVERY frame from then on: one publish, one DirtyVariable and one view
			// re-evaluation per frame for a value that is not moving, which is spec 9's idle row
			// lost to a comparison rather than to a change. The bit pattern answers the question
			// the differ is actually asking, "did the stored value change".
			const FNumericProperty* Numeric = CastFieldChecked<FNumericProperty>(Property);
			const double Live = Numeric->GetFloatingPointPropertyValue(LiveValue);
			const double Old = Numeric->GetFloatingPointPropertyValue(ShadowValue);
			return FMemory::Memcmp(&Live, &Old, sizeof(double)) != 0;
		}

		case EVaCuusFieldKind::String:
		{
			// Equals(CaseSensitive), NEVER operator==. FString's operator== is
			// `Equals(Rhs, ESearchCase::IgnoreCase)` (UnrealString.h.inl:906-915, and the doc
			// comment there says "@note case insensitive"), while Equals() DEFAULTS to
			// CaseSensitive (:1271). The adapter ships the string byte for byte
			// (VaCuusDataVariable.cpp:100), so `"hp"` -> `"HP"` changes the screen and compares
			// equal -- a stale label forever, from the most natural line anyone would write here.
			// Spelled with the argument rather than relying on the default so that the intent
			// survives the next reader.
			const FStrProperty* StrProperty = CastFieldChecked<FStrProperty>(Property);
			return !StrProperty->GetPropertyValue(LiveValue).Equals(StrProperty->GetPropertyValue(ShadowValue), ESearchCase::CaseSensitive);
		}

		case EVaCuusFieldKind::Utf8String:
		{
			// Same trap, same fix: FUtf8String is the same template (UnrealString.h.inl is
			// included once per character type), so its operator== is Stricmp too.
			const FUtf8StrProperty* StrProperty = CastFieldChecked<FUtf8StrProperty>(Property);
			return !StrProperty->GetPropertyValue(LiveValue).Equals(StrProperty->GetPropertyValue(ShadowValue), ESearchCase::CaseSensitive);
		}

		case EVaCuusFieldKind::AnsiString:
		{
			const FAnsiStrProperty* StrProperty = CastFieldChecked<FAnsiStrProperty>(Property);
			return !StrProperty->GetPropertyValue(LiveValue).Equals(StrProperty->GetPropertyValue(ShadowValue), ESearchCase::CaseSensitive);
		}

		case EVaCuusFieldKind::Name:
		{
			// ENameCase::CaseSensitive, which compares the DISPLAY index; the default
			// (IgnoreCase) compares the comparison index, and so does operator==
			// (NameTypes.h:1543-1547, :1624-1626). The adapter ships FName::ToString(), i.e. the
			// display form, so the case-insensitive compare can call two names equal that render
			// differently.
			//
			// FREE WHERE IT CANNOT HELP: WITH_CASE_PRESERVING_NAME is WITH_EDITORONLY_DATA
			// (NameTypes.h:32-33), and without it FName has no DisplayIndex member at all
			// (:1264-1267) -- GetDisplayIndexFast returns the comparison index instead
			// (:1286-1293). So in a cooked build this compiles to exactly the comparison the
			// default would have made, and the case genuinely cannot change because the name
			// table did not keep it. An editor-and-PIE correctness fix with no runtime cost,
			// which is the right trade for a milestone whose bugs are all invisible.
			const FNameProperty* NameProperty = CastFieldChecked<FNameProperty>(Property);
			return !NameProperty->GetPropertyValue(LiveValue).IsEqual(NameProperty->GetPropertyValue(ShadowValue), ENameCase::CaseSensitive);
		}

		case EVaCuusFieldKind::Text:
		{
			// THE DISPLAY STRING, which is both what the adapter ships
			// (VaCuusDataVariable.cpp:130) and the only thing FTextProperty::Identical cannot be
			// trusted for: it picks EIdenticalLexicalCompareMethod::None when !GIsEditor
			// (TextProperty.cpp:63-67), and two texts with equal display strings but different
			// identities then compare NOT equal (:119-120) -- so a label rebuilt from the same
			// string each frame would publish every frame in a shipping build and never in the
			// editor.
			//
			// ToString() on the SHADOW side is free: the shadow's text was projected by
			// StoreField below and has no TextId, so Rebuild() short-circuits
			// (TextHistory.cpp:705-709, :940-943). On the LIVE side it may consult the
			// localization manager, which is legal here and nowhere else -- see the header.
			//
			// FIELD CALL SITE ONLY: a Text ELEMENT never exists, because the desc build
			// refuses FText anywhere in an element subtree (the projection below is per
			// field, and a whole-array copy would bypass it).
			const FTextProperty* TextProperty = CastFieldChecked<FTextProperty>(Property);
			return !TextProperty->GetPropertyValue(LiveValue).ToString().Equals(
				TextProperty->GetPropertyValue(ShadowValue).ToString(), ESearchCase::CaseSensitive);
		}

		case EVaCuusFieldKind::Enum:
			return ReadEnumValue(Property, LiveValue) != ReadEnumValue(Property, ShadowValue);

		case EVaCuusFieldKind::ObjectPath:
		{
			// SOFT ONLY -- the layout refuses FWeakObjectProperty, so there is no weak branch to
			// write and no place a weak pointer could be resolved off the game thread.
			const FSoftObjectProperty* SoftProperty = CastFieldChecked<FSoftObjectProperty>(Property);
			return SoftPathsDiffer(SoftProperty->GetPropertyValue(LiveValue).GetUniqueID(),
				SoftProperty->GetPropertyValue(ShadowValue).GetUniqueID());
		}

		case EVaCuusFieldKind::Array:
			// Unreachable by shape: HasFieldChanged dispatches an Array field to
			// HasArrayChanged before this switch runs, and an ELEMENT is never an array --
			// the desc build refuses nested containers.
			checkNoEntry();
			return false;
	}

	checkNoEntry();
	return false;
}

/**
 * The array diff: ONE BIT, FIRST DIFFERENCE WINS. The bit is per-array, so finding one
 * difference is finding them all -- and RmlUi could not consume anything finer anyway; its
 * view map keys on top-level names alone, so DirtyVariable(root) is the entire expressible
 * vocabulary (spec 2(b)). A Num() mismatch is a difference by itself.
 *
 * Helpers are constructed per call over the incoming VALUE pointers -- the ctor takes the
 * value pointer, not the container (UnrealType.h:4285-4288) -- and every element address is
 * GetRawPtr(i), call-time arithmetic stored nowhere (UnrealType.h:4324-4333, spec 2(c)).
 */
static bool HasArrayChanged(const FVaCuusModelArrayDesc& Desc, const void* LiveValue, const void* ShadowValue)
{
	FScriptArrayHelper LiveHelper(Desc.ArrayProperty, LiveValue);
	FScriptArrayHelper ShadowHelper(Desc.ArrayProperty, ShadowValue);

	const int32 Num = LiveHelper.Num();
	if (Num != ShadowHelper.Num())
	{
		return true;
	}

	if (Desc.IsStructElement())
	{
		// Struct rows walk the element layout's flat leaves with the ELEMENT BASE standing in
		// for the struct base: GetRawPtr(i) addresses an instance of exactly the type the
		// element layout was built over, so ValuePtr composes the same two offsets it
		// composes for a model root. Rows before leaves, so the scan can stop at the first
		// changed row without touching the tail.
		const TConstArrayView<FVaCuusModelField> ElementFields = Desc.ElementLayout->GetFields();
		for (int32 Index = 0; Index < Num; ++Index)
		{
			const void* LiveElement = LiveHelper.GetRawPtr(Index);
			const void* ShadowElement = ShadowHelper.GetRawPtr(Index);
			for (const FVaCuusModelField& ElementField : ElementFields)
			{
				if (HasValueChanged(ElementField.Kind, ElementField.Property, ValuePtr(ElementField, LiveElement),
						ValuePtr(ElementField, ShadowElement)))
				{
					return true;
				}
			}
		}

		return false;
	}

	// Scalar elements: the Inner IS the value property and GetRawPtr(i) IS its value
	// pointer -- Offset_Internal is 0 and the stride is GetElementSize() (see the desc).
	for (int32 Index = 0; Index < Num; ++Index)
	{
		if (HasValueChanged(Desc.ElementKind, Desc.Inner, LiveHelper.GetRawPtr(Index), ShadowHelper.GetRawPtr(Index)))
		{
			return true;
		}
	}

	return false;
}

/**
 * Has this field changed since the shadow was written? Dispatch only: an Array field goes
 * to the element-walking diff, every scalar kind to the shared value-pointer comparator --
 * whose exhaustive switch is what stops a new kind from silently defaulting to "never
 * changes".
 */
static bool HasFieldChanged(const FVaCuusModelField& Field, const void* LiveBase, const void* ShadowBase)
{
	const void* LiveValue = ValuePtr(Field, LiveBase);
	const void* ShadowValue = ValuePtr(Field, ShadowBase);

	if (Field.Kind == EVaCuusFieldKind::Array)
	{
		return HasArrayChanged(*Field.ArrayDesc, LiveValue, ShadowValue);
	}

	return HasValueChanged(Field.Kind, Field.Property, LiveValue, ShadowValue);
}

/**
 * Writes the live value into the shadow, in the form the rest of the pipeline carries.
 *
 * A straight copy for most kinds. FText is the exception, and the header carries the whole
 * argument: what is stored is the display string resolved HERE, on the game thread, frozen
 * into a culture-invariant FText, so that nothing downstream ever asks the localization
 * manager anything.
 *
 * AN EXHAUSTIVE SWITCH NOW, NOT AN IF-CHAIN, and the change is the point (spec 3.3): the
 * old `if (Text) ... else CopyValue` let a NEW kind fall through to the generic copy with
 * no one choosing it -- exactly how an Array field would have silently taken the engine's
 * destroy-and-rebuild whole-container copy. -Wswitch now forces every kind to pick its
 * store form here, the same protection the other four kind switches already have.
 */
static void StoreField(const FVaCuusModelField& Field, void* ShadowBase, const void* LiveBase)
{
	switch (Field.Kind)
	{
		case EVaCuusFieldKind::Text:
		{
			const FTextProperty* TextProperty = CastFieldChecked<FTextProperty>(Field.Property);

			// FString(...) rather than passing the const FString& straight through: the
			// AsCultureInvariant overload set is (const TCHAR*, FStringView, FString&&, FText)
			// (Text.cpp:1234-1272) and an explicit temporary picks the move overload with no
			// ambiguity to argue about.
			TextProperty->SetPropertyValue(ValuePtr(Field, ShadowBase),
				FText::AsCultureInvariant(FString(TextProperty->GetPropertyValue(ValuePtr(Field, LiveBase)).ToString())));
			return;
		}

		case EVaCuusFieldKind::Array:
			// CHOSEN, not fallen into: the funnel resolves to FVaCuusModelArrayDesc::SyncCopy
			// -- Resize plus per-element assignment, never the engine's destroy-and-rebuild
			// copy (spec 3.3; SyncCopy's own comment carries the citations).
			Field.CopyValue(ShadowBase, LiveBase);
			return;

		case EVaCuusFieldKind::Bool:
		case EVaCuusFieldKind::SignedInt:
		case EVaCuusFieldKind::UnsignedInt:
		case EVaCuusFieldKind::FloatingPoint:
		case EVaCuusFieldKind::String:
		case EVaCuusFieldKind::Utf8String:
		case EVaCuusFieldKind::AnsiString:
		case EVaCuusFieldKind::Name:
		case EVaCuusFieldKind::Enum:
		case EVaCuusFieldKind::ObjectPath:
			Field.CopyValue(ShadowBase, LiveBase);
			return;
	}

	checkNoEntry();
}
}	 // namespace VaCuusModelSamplerPrivate

FVaCuusModelSampler::FVaCuusModelSampler(const FVaCuusModelLayout& InLayout)
	: Layout(InLayout)
	, Shadow(InLayout.GetStruct())
{
}

int32 FVaCuusModelSampler::Sample(const UScriptStruct* LiveType, const void* LiveData, FVaCuusModelChannel& Channel)
{
	// GAME THREAD, AND NOT AS A CONVENTION: LiveData is gameplay memory, which has no engine
	// synchronisation of any kind, and the shadow plus the channel's pending set are plain
	// non-atomic state this shares with Publish(). The spec additionally requires the call to
	// come from UVaCuusSubsystem::Tick so it lands inside the GameTick perf scope
	// (VaCuusSubsystem.cpp:117) -- an assert cannot express that, so it is stated in the header
	// and enforced by there being no other caller.
	check(IsInGameThread());

	if (!IsValid())
	{
		return 0;
	}

	if (LiveData == nullptr)
	{
		UE_LOG(LogVaCuus, Warning, TEXT("VaCuus model '%s': sampled from a null pointer; nothing was read"),
			*Layout.GetStruct()->GetName());
		return 0;
	}

	// THE TYPE CHECK EARNS ITS BRANCH. Every offset below came out of Layout; applied to an
	// instance of a different type they address whatever happens to sit at those bytes, and the
	// first FString field then reads a pointer that is not one. There is no later opportunity
	// to notice.
	if (LiveType != Layout.GetStruct())
	{
		UE_LOG(LogVaCuus, Error, TEXT("VaCuus model: sampled a '%s' through a layout built for '%s'; nothing was read"),
			LiveType != nullptr ? *LiveType->GetName() : TEXT("none"), *Layout.GetStruct()->GetName());
		return 0;
	}

	using namespace VaCuusModelSamplerPrivate;

	const TConstArrayView<FVaCuusModelField> Fields = Layout.GetFields();
	void* ShadowBase = Shadow.GetData();

	int32 NumMarked = 0;
	for (int32 Index = 0; Index < Fields.Num(); ++Index)
	{
		const FVaCuusModelField& Field = Fields[Index];
		if (!HasFieldChanged(Field, LiveData, ShadowBase))
		{
			continue;
		}

		// STORE FIRST, THEN MARK. The order is not load-bearing here -- both are game-thread
		// writes and the publish happens after this whole loop -- but keeping the shadow write
		// adjacent to its mark is what makes "marked but not stored" impossible to introduce by
		// editing one branch.
		StoreField(Field, ShadowBase, LiveData);
		Channel.MarkFieldDirty(Index);
		++NumMarked;
	}

	++NumSamples;
	NumFieldsMarked += NumMarked;
	return NumMarked;
}
