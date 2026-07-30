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
 * Has this field changed since the shadow was written?
 *
 * ONE EXHAUSTIVE SWITCH, NO `default`: -Wswitch turns a new EVaCuusFieldKind into a compile
 * error here, which is the only mechanism that would stop a new kind from silently defaulting
 * to "never changes" -- a bound variable that is correct once and then frozen, with no log
 * line, forever. Same shape as the adapter's Get() and LexToString().
 *
 * The rule for every branch: compare WHAT THE UI ACTUALLY SHIPS. Anything cheaper that is not
 * a function of the shipped value can be equal while the screen would differ.
 */
static bool HasFieldChanged(const FVaCuusModelField& Field, const void* LiveBase, const void* ShadowBase)
{
	const void* LiveValue = ValuePtr(Field, LiveBase);
	const void* ShadowValue = ValuePtr(Field, ShadowBase);

	switch (Field.Kind)
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
			const FBoolProperty* BoolProperty = CastFieldChecked<FBoolProperty>(Field.Property);
			return BoolProperty->GetPropertyValue(LiveValue) != BoolProperty->GetPropertyValue(ShadowValue);
		}

		case EVaCuusFieldKind::SignedInt:
		{
			const FNumericProperty* Numeric = CastFieldChecked<FNumericProperty>(Field.Property);
			return Numeric->GetSignedIntPropertyValue(LiveValue) != Numeric->GetSignedIntPropertyValue(ShadowValue);
		}

		case EVaCuusFieldKind::UnsignedInt:
		{
			const FNumericProperty* Numeric = CastFieldChecked<FNumericProperty>(Field.Property);
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
			const FNumericProperty* Numeric = CastFieldChecked<FNumericProperty>(Field.Property);
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
			const FStrProperty* StrProperty = CastFieldChecked<FStrProperty>(Field.Property);
			return !StrProperty->GetPropertyValue(LiveValue).Equals(StrProperty->GetPropertyValue(ShadowValue), ESearchCase::CaseSensitive);
		}

		case EVaCuusFieldKind::Utf8String:
		{
			// Same trap, same fix: FUtf8String is the same template (UnrealString.h.inl is
			// included once per character type), so its operator== is Stricmp too.
			const FUtf8StrProperty* StrProperty = CastFieldChecked<FUtf8StrProperty>(Field.Property);
			return !StrProperty->GetPropertyValue(LiveValue).Equals(StrProperty->GetPropertyValue(ShadowValue), ESearchCase::CaseSensitive);
		}

		case EVaCuusFieldKind::AnsiString:
		{
			const FAnsiStrProperty* StrProperty = CastFieldChecked<FAnsiStrProperty>(Field.Property);
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
			const FNameProperty* NameProperty = CastFieldChecked<FNameProperty>(Field.Property);
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
			const FTextProperty* TextProperty = CastFieldChecked<FTextProperty>(Field.Property);
			return !TextProperty->GetPropertyValue(LiveValue).ToString().Equals(
				TextProperty->GetPropertyValue(ShadowValue).ToString(), ESearchCase::CaseSensitive);
		}

		case EVaCuusFieldKind::Enum:
			return ReadEnumValue(Field.Property, LiveValue) != ReadEnumValue(Field.Property, ShadowValue);

		case EVaCuusFieldKind::ObjectPath:
		{
			// SOFT ONLY -- the layout refuses FWeakObjectProperty, so there is no weak branch to
			// write and no place a weak pointer could be resolved off the game thread.
			const FSoftObjectProperty* SoftProperty = CastFieldChecked<FSoftObjectProperty>(Field.Property);
			return SoftPathsDiffer(SoftProperty->GetPropertyValue(LiveValue).GetUniqueID(),
				SoftProperty->GetPropertyValue(ShadowValue).GetUniqueID());
		}
	}

	checkNoEntry();
	return false;
}

/**
 * Writes the live value into the shadow, in the form the rest of the pipeline carries.
 *
 * A straight copy for ten of the eleven kinds. FText is the exception, and the header carries
 * the whole argument: what is stored is the display string resolved HERE, on the game thread,
 * frozen into a culture-invariant FText, so that nothing downstream ever asks the localization
 * manager anything.
 */
static void StoreField(const FVaCuusModelField& Field, void* ShadowBase, const void* LiveBase)
{
	if (Field.Kind == EVaCuusFieldKind::Text)
	{
		const FTextProperty* TextProperty = CastFieldChecked<FTextProperty>(Field.Property);

		// FString(...) rather than passing the const FString& straight through: the
		// AsCultureInvariant overload set is (const TCHAR*, FStringView, FString&&, FText)
		// (Text.cpp:1234-1272) and an explicit temporary picks the move overload with no
		// ambiguity to argue about.
		TextProperty->SetPropertyValue(
			ValuePtr(Field, ShadowBase), FText::AsCultureInvariant(FString(TextProperty->GetPropertyValue(ValuePtr(Field, LiveBase)).ToString())));
		return;
	}

	Field.CopyValue(ShadowBase, LiveBase);
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
	// (VaCuusSubsystem.cpp:68) -- an assert cannot express that, so it is stated in the header
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
