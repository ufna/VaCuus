// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusModelLayout.h"

#include "VaCuusDefines.h"

#include "UObject/AnsiStrProperty.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/Field.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/StrProperty.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include "UObject/Utf8StrProperty.h"

// Out of line only because TUniquePtr<FVaCuusModelLayout> needs the complete type to destroy
// and the header declares the desc before the layout. All defaulted; the TUniquePtr member is
// what deletes copying, for the layout as well as for the desc.
FVaCuusModelArrayDesc::FVaCuusModelArrayDesc() = default;
FVaCuusModelArrayDesc::FVaCuusModelArrayDesc(FVaCuusModelArrayDesc&&) = default;
FVaCuusModelArrayDesc& FVaCuusModelArrayDesc::operator=(FVaCuusModelArrayDesc&&) = default;
FVaCuusModelArrayDesc::~FVaCuusModelArrayDesc() = default;

void FVaCuusModelArrayDesc::SyncCopy(void* DestValuePtr, const void* SrcValuePtr) const
{
	// NOT ArrayProperty->CopySingleValue, WHOSE WASTE IS STRUCTURAL. The engine's whole-array
	// copy is correct -- and it EMPTIES the destination first, destroying every non-POD
	// element and freeing its heap, before resizing and rebuilding (PropertyArray.cpp:
	// 1260-1328; EmptyAndAddValues at :1269 -> EmptyValues -> DestructItems, UnrealType.h:
	// 4459-4471, :4595-4611). A same-size republish through it can therefore never reuse an
	// element's buffer, and the container block itself reallocates on any Num change in
	// either direction (Empty(Slack) reallocates whenever Slack != ArrayMax --
	// ScriptArray.h:137-148).
	//
	// THIS FORM PAYS ONLY FOR THE DELTA. Resize adds or removes exactly the difference
	// (UnrealType.h:4387-4403), preserving surviving elements' VALUES -- their addresses may
	// still move on a realloc, which the no-stored-addresses invariant absorbs (spec 2(c)).
	// Each element is then ASSIGNED in place: for an FString inner that is FString::operator=
	// (TProperty::CopyValuesInternal, UnrealType.h:1626-1632), whose reallocation rule is
	// grow-only -- ReallocForCopy reallocates iff the quantized reserve of the source exceeds
	// the destination's capacity, else the buffer is reused outright (TArray::operator= at
	// Array.h:1011-1019 -> ReallocForCopy at :710-751, `NewMax > PrevMax`). Net: GROW-ONLY
	// reuse -- allocations where content outgrew capacity or Num grew -- at every pipeline
	// stage (spec 3.3). The SHRINK side is weaker, deliberately left to the allocator:
	// Resize's shrink is RemoveValues, which forwards to FScriptArray::Remove with shrinking
	// ALLOWED (UnrealType.h:4477-4483, ScriptArray.h:191-222), and ResizeShrink then
	// reallocates the container block whenever DefaultCalculateSlackShrink asks -- (slack >=
	// 16384 bytes OR Num < 2/3 of Max) AND (slack > 64 elements OR Num == 0)
	// (ScriptArray.h:255-263, ContainerAllocationPolicies.h:140-168) -- so a large trim can
	// move the survivors. Spec 3.4 measures the grow/shrink alternation rather than assuming
	// it away.
	FScriptArrayHelper DestHelper(ArrayProperty, DestValuePtr);
	FScriptArrayHelper SrcHelper(ArrayProperty, SrcValuePtr);

	const int32 Num = SrcHelper.Num();
	DestHelper.Resize(Num);
	if (Num == 0)
	{
		return;
	}

	// The same POD gate the engine's own copies apply (CopySingleValue, UnrealType.h:881-894;
	// the array copy's memcpy branch, PropertyArray.cpp:1323-1326): a POD inner has no
	// assignment semantics to respect, so the whole payload is one Memcpy. Num * ElementSize
	// is exact because the stride IS the element size -- tail padding is baked in for struct
	// inners (PropertyStruct.cpp:114) and GetRawPtr advances by exactly it (UnrealType.h:4332).
	if (Inner->HasAnyPropertyFlags(CPF_IsPlainOldData))
	{
		FMemory::Memcpy(DestHelper.GetRawPtr(0), SrcHelper.GetRawPtr(0), static_cast<size_t>(Num) * Inner->GetElementSize());
		return;
	}

	// Non-POD: per-element assignment into LIVE destination elements -- Resize left the
	// survivors constructed and constructed the growth (AddValues = AddUninitializedValues +
	// ConstructItems, UnrealType.h:4409-4414), so CopyCompleteValue assigns, never constructs
	// over garbage. Complete, not Single: an inner always has ArrayDim 1 -- containers of
	// C arrays cannot exist (UhtProperty.cs:2390-2393) -- so the two coincide, and Complete
	// is what the engine's own element loop calls (PropertyArray.cpp:1317-1321).
	for (int32 Index = 0; Index < Num; ++Index)
	{
		Inner->CopyCompleteValue(DestHelper.GetRawPtr(Index), SrcHelper.GetRawPtr(Index));
	}
}

const TCHAR* LexToString(EVaCuusFieldKind Kind)
{
	// No default case, on purpose: -Wswitch turns a new enumerator into a compile error
	// here instead of a "<unknown>" in a log line nobody reads.
	switch (Kind)
	{
		case EVaCuusFieldKind::Bool:
			return TEXT("Bool");
		case EVaCuusFieldKind::SignedInt:
			return TEXT("SignedInt");
		case EVaCuusFieldKind::UnsignedInt:
			return TEXT("UnsignedInt");
		case EVaCuusFieldKind::FloatingPoint:
			return TEXT("FloatingPoint");
		case EVaCuusFieldKind::String:
			return TEXT("String");
		case EVaCuusFieldKind::Utf8String:
			return TEXT("Utf8String");
		case EVaCuusFieldKind::AnsiString:
			return TEXT("AnsiString");
		case EVaCuusFieldKind::Name:
			return TEXT("Name");
		case EVaCuusFieldKind::Text:
			return TEXT("Text");
		case EVaCuusFieldKind::Enum:
			return TEXT("Enum");
		case EVaCuusFieldKind::ObjectPath:
			return TEXT("ObjectPath");
		case EVaCuusFieldKind::Array:
			return TEXT("Array");
	}

	checkNoEntry();
	return TEXT("<unknown>");
}

void FVaCuusModelField::CopyValue(void* DestStructBase, const void* SourceStructBase) const
{
	void* DestValue = Property->ContainerPtrToValuePtr<void>(ContainerPtr(DestStructBase));
	const void* SourceValue = Property->ContainerPtrToValuePtr<void>(ContainerPtr(SourceStructBase));

	// THE FUNNEL: an Array field copies through its desc, so every pipeline stage that calls
	// CopyValue per dirty bit takes the array primitive with no new call site and no layout in
	// hand. ArrayDesc is non-null for every Array field by construction -- the layout
	// constructor fixes it up before any field is visible.
	if (Kind == EVaCuusFieldKind::Array)
	{
		ArrayDesc->SyncCopy(DestValue, SourceValue);
		return;
	}

	// See the header for why CopySingleValue and not a memcpy. Both pointers go through
	// ContainerPtrToValuePtr so that a bitfield's value pointer is its storage integer, which
	// is what FBoolProperty's accessors and its masked copy both expect.
	Property->CopySingleValue(DestValue, SourceValue);
}

namespace VaCuusModelLayoutPrivate
{
static FString DescribeScalarValue(EVaCuusFieldKind Kind, const FProperty* Property, const void* ValuePtr);
static FString DescribeArrayValue(const FVaCuusModelArrayDesc& Desc, const void* ValuePtr);
}	 // namespace VaCuusModelLayoutPrivate

FString FVaCuusModelField::DescribeValue(const void* StructBase) const
{
	// The same two-step addressing every other reader uses: ContainerPtr applies the flattening
	// offset, ContainerPtrToValuePtr applies Offset_Internal. Doing the second by hand is what
	// FProperty::GetOffset_ReplaceWith_ContainerPtrToValuePtr (UnrealType.h:466) exists to shame,
	// and it is also the only form that is correct for a bitfield -- whose value pointer is the
	// shared storage integer, not the bit.
	const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(ContainerPtr(StructBase));

	// An array prints through its desc; every scalar kind goes through the VALUE-POINTER form
	// below -- which is also what a scalar array's elements print through. One accessor set,
	// two call sites, so a field and an element cannot drift apart.
	if (Kind == EVaCuusFieldKind::Array)
	{
		return VaCuusModelLayoutPrivate::DescribeArrayValue(*ArrayDesc, ValuePtr);
	}

	return VaCuusModelLayoutPrivate::DescribeScalarValue(Kind, Property, ValuePtr);
}

FString VaCuusModelLayoutPrivate::DescribeScalarValue(EVaCuusFieldKind Kind, const FProperty* Property, const void* ValuePtr)
{
	// NO `default`: -Wswitch makes a new EVaCuusFieldKind a compile error here. Same shape as
	// FVaCuusScalarDefinition::Get(), the sampler's HasFieldChanged() and LexToString().
	switch (Kind)
	{
		case EVaCuusFieldKind::Bool:
			// The mask-aware accessor, never a byte read, for the reason the sampler spells out:
			// `uint8 b : 1` shares its storage byte with up to seven unrelated bitfields.
			// "1"/"0" rather than "true"/"false" because that is what RmlUi ships
			// (TypeConverter.inl:340-347).
			return CastFieldChecked<FBoolProperty>(Property)->GetPropertyValue(ValuePtr) ? TEXT("1") : TEXT("0");

		case EVaCuusFieldKind::SignedInt:
			return FString::Printf(TEXT("%lld"), CastFieldChecked<FNumericProperty>(Property)->GetSignedIntPropertyValue(ValuePtr));

		case EVaCuusFieldKind::UnsignedInt:
			return FString::Printf(TEXT("%llu"), CastFieldChecked<FNumericProperty>(Property)->GetUnsignedIntPropertyValue(ValuePtr));

		case EVaCuusFieldKind::FloatingPoint:
			// FULL PRECISION, WHICH IS WHERE THIS DELIBERATELY DIVERGES FROM THE SCREEN -- see
			// the header. %.17g round-trips an IEEE double exactly, so two shadows that differ
			// in the last bit differ here; RmlUi's own "%.3f" would print them the same.
			return FString::Printf(
				TEXT("%.17g"), CastFieldChecked<FNumericProperty>(Property)->GetFloatingPointPropertyValue(ValuePtr));

		case EVaCuusFieldKind::String:
			return CastFieldChecked<FStrProperty>(Property)->GetPropertyValue(ValuePtr);

		case EVaCuusFieldKind::Utf8String:
			return FString(CastFieldChecked<FUtf8StrProperty>(Property)->GetPropertyValue(ValuePtr));

		case EVaCuusFieldKind::AnsiString:
			return FString(CastFieldChecked<FAnsiStrProperty>(Property)->GetPropertyValue(ValuePtr));

		case EVaCuusFieldKind::Name:
			return CastFieldChecked<FNameProperty>(Property)->GetPropertyValue(ValuePtr).ToString();

		case EVaCuusFieldKind::Text:
			// The display string, which is what the adapter ships and what the sampler diffs.
			return CastFieldChecked<FTextProperty>(Property)->GetPropertyValue(ValuePtr).ToString();

		case EVaCuusFieldKind::Enum:
		{
			// Two shapes, one kind: an FEnumProperty wraps an underlying integer property while a
			// TEnumAsByte is an FByteProperty that merely CARRIES a UEnum (UnrealType.h:2195,
			// 2253). Only the first has GetUnderlyingProperty().
			const UEnum* Enum = nullptr;
			int64 Value = 0;
			if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
			{
				Enum = EnumProperty->GetEnum();
				Value = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
			}
			else
			{
				const FNumericProperty* Numeric = CastFieldChecked<FNumericProperty>(Property);
				Enum = Numeric->GetIntPropertyEnum();
				Value = static_cast<int64>(Numeric->GetUnsignedIntPropertyValue(ValuePtr));
			}

			// FindAuthoredNameStringByValue, matching the adapter: the Get form feeds an
			// INDEX_NONE straight into GetNameStringByIndex and answers with an empty string
			// (Enum.cpp:933-937), which is precisely the "reads as nothing, says nothing" outcome
			// a dump exists to expose. The Find form reports the miss (Enum.cpp:939-949), and the
			// number is printed because it is the only thing that identifies the bad value.
			FString Name;
			if (Enum == nullptr || !Enum->FindAuthoredNameStringByValue(Name, Value))
			{
				return FString::Printf(TEXT("<%lld is not a value of its enum; the document reads it as empty>"), Value);
			}
			return Name;
		}

		case EVaCuusFieldKind::ObjectPath:
			// ToString() is GetUniqueID().ToString() (SoftObjectPtr.h:96-105) -- no resolution, no
			// GUObjectArray read, so this is as safe on the UI thread as it is here.
			return CastFieldChecked<FSoftObjectProperty>(Property)->GetPropertyValue(ValuePtr).ToString();

		case EVaCuusFieldKind::Array:
			// Unreachable by shape: DescribeValue dispatches an Array field before this runs,
			// and an ELEMENT is never an array -- the desc build refuses nested containers.
			checkNoEntry();
			return FString();
	}

	checkNoEntry();
	return FString();
}

FString VaCuusModelLayoutPrivate::DescribeArrayValue(const FVaCuusModelArrayDesc& Desc, const void* ValuePtr)
{
	// The helper takes the VALUE pointer, not the container (UnrealType.h:4285-4288), and it has
	// no const access path at all: constness is laundered at construction, with the engine's own
	// "we are casting away the const here" comment on the private ctor (UnrealType.h:4549-4557).
	// Read-only by discipline, like every other reader of two shadows.
	FScriptArrayHelper Helper(Desc.ArrayProperty, ValuePtr);
	const int32 Num = Helper.Num();

	// First 8 and an elision marker (spec 6): the dump exists to show the model's scalar fields
	// next to its arrays, and 200 printed rows would bury them.
	constexpr int32 MaxShown = 8;
	const int32 Shown = FMath::Min(Num, MaxShown);

	FString Result = FString::Printf(TEXT("%d elements ["), Num);
	for (int32 Index = 0; Index < Shown; ++Index)
	{
		if (Index > 0)
		{
			Result += TEXT(", ");
		}

		// Computed per element per use, stored nowhere -- GetRawPtr is call-time arithmetic
		// (UnrealType.h:4324-4333) and the invariant is spec 2(c)'s.
		const void* ElementPtr = Helper.GetRawPtr(Index);

		if (Desc.IsStructElement())
		{
			// The element base stands in for the struct base: an element layout's fields carry
			// offsets relative to the element type, and GetRawPtr(i) addresses an instance of
			// exactly that type.
			Result += TEXT("{");
			const TConstArrayView<FVaCuusModelField> ElementFields = Desc.ElementLayout->GetFields();
			for (int32 FieldIndex = 0; FieldIndex < ElementFields.Num(); ++FieldIndex)
			{
				if (FieldIndex > 0)
				{
					Result += TEXT(" ");
				}
				Result += ElementFields[FieldIndex].WireName + TEXT("=") + ElementFields[FieldIndex].DescribeValue(ElementPtr);
			}
			Result += TEXT("}");
		}
		else
		{
			Result += DescribeScalarValue(Desc.ElementKind, Desc.Inner, ElementPtr);
		}
	}

	if (Num > Shown)
	{
		Result += FString::Printf(TEXT(", ... %d more"), Num - Shown);
	}

	Result += TEXT("]");
	return Result;
}

namespace VaCuusModelLayoutPrivate
{
/** The character class RmlUi accepts inside a name, in both positions. */
static bool IsWireNameBodyChar(const TCHAR Char)
{
	return (Char >= TEXT('a') && Char <= TEXT('z')) || (Char >= TEXT('A') && Char <= TEXT('Z'))
		|| (Char >= TEXT('0') && Char <= TEXT('9')) || Char == TEXT('_');
}

/**
 * Classifies one leaf property.
 *
 * ORDER IS THE CONTRACT, not a style choice. CastField is a cast-FLAG test
 * (Field.h:765-776, `GetCastFlags() & T::StaticClassCastFlagsPrivate()`), and those flags
 * are inherited, so a base-class test matches every derived kind: FClassProperty is an
 * FObjectProperty (UnrealType.h:3465), FSoftClassProperty is an FSoftObjectProperty
 * (:3550), and every integer and float property satisfies CastField<FNumericProperty>.
 * Most-derived first, always.
 *
 * @return true with OutKind set; otherwise false with OutReason set to why M3a will not
 *         carry this property. OutReason is never null on a false return.
 */
static bool ClassifyProperty(const FProperty* Property, EVaCuusFieldKind& OutKind, const TCHAR*& OutReason)
{
	// Bools first: FBoolProperty is not an FNumericProperty, but putting it anywhere
	// after the integer tests invites someone to "simplify" the integer list into one
	// FNumericProperty test and silently swallow it.
	if (CastField<FBoolProperty>(Property) != nullptr)
	{
		OutKind = EVaCuusFieldKind::Bool;
		return true;
	}

	if (CastField<FEnumProperty>(Property) != nullptr)
	{
		OutKind = EVaCuusFieldKind::Enum;
		return true;
	}

	// An FByteProperty is EITHER a uint8 or a TEnumAsByte, and only its Enum member says
	// which (UnrealType.h:2195, 2253). CastField<FNumericProperty> matches it in both
	// cases, so this test must come before the numeric ones.
	if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
	{
		OutKind = ByteProperty->GetIntPropertyEnum() != nullptr ? EVaCuusFieldKind::Enum : EVaCuusFieldKind::UnsignedInt;
		return true;
	}

	if (CastField<FFloatProperty>(Property) != nullptr || CastField<FDoubleProperty>(Property) != nullptr)
	{
		OutKind = EVaCuusFieldKind::FloatingPoint;
		return true;
	}

	// Spelled out per width rather than tested as FNumericProperty, because FNumericProperty
	// exposes IsInteger() and IsFloatingPoint() but nothing that answers "is it signed" --
	// and signedness decides which of the two 64-bit getters reads it without wrapping.
	if (CastField<FInt8Property>(Property) != nullptr || CastField<FInt16Property>(Property) != nullptr
		|| CastField<FIntProperty>(Property) != nullptr || CastField<FInt64Property>(Property) != nullptr)
	{
		OutKind = EVaCuusFieldKind::SignedInt;
		return true;
	}

	if (CastField<FUInt16Property>(Property) != nullptr || CastField<FUInt32Property>(Property) != nullptr
		|| CastField<FUInt64Property>(Property) != nullptr)
	{
		OutKind = EVaCuusFieldKind::UnsignedInt;
		return true;
	}

	if (CastField<FStrProperty>(Property) != nullptr)
	{
		OutKind = EVaCuusFieldKind::String;
		return true;
	}

	// Siblings of FStrProperty, not subclasses: three separate cast flags
	// (ObjectMacros.h:361, 407, 408) generated from the same StrProperty.h.inl. Testing
	// only FStrProperty drops them from the model with no diagnostic at all.
	if (CastField<FUtf8StrProperty>(Property) != nullptr)
	{
		OutKind = EVaCuusFieldKind::Utf8String;
		return true;
	}
	if (CastField<FAnsiStrProperty>(Property) != nullptr)
	{
		OutKind = EVaCuusFieldKind::AnsiString;
		return true;
	}

	if (CastField<FNameProperty>(Property) != nullptr)
	{
		OutKind = EVaCuusFieldKind::Name;
		return true;
	}
	if (CastField<FTextProperty>(Property) != nullptr)
	{
		OutKind = EVaCuusFieldKind::Text;
		return true;
	}

	// A value type with no GC ownership AND no resolution: an FSoftObjectPtr already IS a
	// path, so ToString() is GetUniqueID().ToString() (SoftObjectPtr.h:96-105) -- two FNames
	// and a subpath string, no GUObjectArray read and nothing the collector can move. That is
	// what makes it readable from the shadow buffer on the UI thread, and it is why
	// LoadObjectPropertyValue is never called: that one synchronously LOADS
	// (UnrealType.h:3435), which on a per-frame path is a hitch.
	//
	// FSoftClassProperty derives from FSoftObjectProperty (UnrealType.h:3550), so it is in.
	if (CastField<FSoftObjectProperty>(Property) != nullptr)
	{
		OutKind = EVaCuusFieldKind::ObjectPath;
		return true;
	}

	// FWeakObjectProperty IS REFUSED, WHICH CORRECTS SPEC 3.4 AND THIS FILE'S FIRST VERSION.
	//
	// Both said "projected to a path string at sample time". There is nowhere to project it
	// TO: the shadow is a real UScriptStruct instance (spec 3.1), so this field is an
	// FWeakObjectPtr in the shadow just as it is in the live struct -- an index/serial pair
	// with no path inside it. Producing a path therefore means resolving the object, and
	// WeakObjectPtr.h:295-296 says outright that a weak pointer cannot be tested from another
	// thread, "as it will incorrectly return false during the mark phase of the GC". Worse
	// than a wrong answer: GetPathName() then walks the object's Outer chain while the purge
	// phase may be destroying it, which is the same use-after-free the shadow buffer exists
	// to prevent.
	//
	// Refused here rather than handled in the adapter because a field that binds and then
	// reads as empty forever is exactly this milestone's signature failure.
	if (CastField<FWeakObjectProperty>(Property) != nullptr)
	{
		OutReason = TEXT("a weak object reference cannot be turned into a path without resolving it, and a weak pointer cannot be "
						 "resolved off the game thread (WeakObjectPtr.h:295-296) -- use a TSoftObjectPtr, which already carries "
						 "its path");
		return false;
	}

	// ---- Refusals, each with the reason rather than a generic "unsupported". ----

	// NO FArrayProperty BRANCH, AND NONE IS REACHABLE. BuildLevel intercepts arrays before
	// classification (like nested structs), because building the element description needs
	// layout state a pure classifier cannot hold. An array cannot arrive as an ELEMENT type
	// either: UHT refuses containers of containers outright (UhtArrayProperty.cs:257-260),
	// so classifying an array's Inner through this function never meets one. A hand-built
	// FArrayProperty that somehow did would fall to the generic tail -- refused, with a log
	// line.
	if (CastField<FMapProperty>(Property) != nullptr)
	{
		OutReason = TEXT("TMap has no RmlUi map view, and FMapProperty::Identical is O(n^2) via IsPermutation");
		return false;
	}
	if (CastField<FSetProperty>(Property) != nullptr)
	{
		OutReason = TEXT("TSet has no RmlUi view, and FSetProperty::Identical is O(n^2) via IsPermutation");
		return false;
	}

	// After the soft/weak tests above, so this catches only the HARD ones (and
	// FClassProperty, which derives from it).
	//
	// "BOUND OR READ", NOT "NEVER IN THE SHADOW". Since arrays, the weaker wording would be
	// false: an UNEXPOSED hard reference inside a bound array row is copied into every shadow
	// with its row -- SyncCopy's whole-row CopyCompleteValue applies no exposure filter (see
	// the desc-build scan) -- as inert bytes with no leaf. What this refusal enforces is the
	// invariant that actually protects the UI thread: no UObject* is ever bound, so none is
	// ever read or dereferenced from a shadow the collector cannot see.
	if (CastField<FObjectProperty>(Property) != nullptr)
	{
		OutReason = TEXT("a hard UObject reference cannot be bound: the shadow is a UScriptStruct instance the UI thread "
						 "owns, nothing calls AddStructReferencedObjects on it, so it is invisible to GC and a read through "
						 "the pointer would dangle with no diagnostic");
		return false;
	}
	if (CastField<FObjectPropertyBase>(Property) != nullptr)
	{
		// FLazyObjectProperty, FInterfaceProperty's cousins: not in M3a's type table.
		OutReason = TEXT("this object-reference kind is not in M3a's type table; use a soft or weak reference");
		return false;
	}

	if (CastField<FDelegateProperty>(Property) != nullptr || CastField<FMulticastDelegateProperty>(Property) != nullptr)
	{
		OutReason = TEXT("a delegate is not a value; events travel the other way (M4)");
		return false;
	}

	OutReason = TEXT("the property kind is not supported by M3a");
	return false;
}
}	 // namespace VaCuusModelLayoutPrivate

const TCHAR* VaCuusWireName::ValidateTopLevel(const FString& Name)
{
	using namespace VaCuusModelLayoutPrivate;

	if (Name.IsEmpty())
	{
		return TEXT("the name is empty");
	}

	// RmlUi lowercases the whole name before testing the first character
	// (DataModel.cpp:58-62), so 'A'-'Z' passes; what does not is a digit or an underscore.
	const TCHAR First = Name[0];
	if (!((First >= TEXT('a') && First <= TEXT('z')) || (First >= TEXT('A') && First <= TEXT('Z'))))
	{
		return TEXT("an RmlUi variable name must start with a letter");
	}

	for (const TCHAR Char : Name)
	{
		if (!IsWireNameBodyChar(Char))
		{
			// UE additionally permits '-', '+', '<', '>' and '?' in an FName; RmlUi does not.
			return TEXT("an RmlUi variable name may contain only A-Z, a-z, 0-9 and underscore");
		}
	}

	static const TCHAR* const ReservedNames[] = {
		TEXT("it"), TEXT("it_index"), TEXT("ev"), TEXT("true"), TEXT("false"), TEXT("size"), TEXT("literal")};
	for (const TCHAR* const Reserved : ReservedNames)
	{
		if (Name.Equals(Reserved, ESearchCase::IgnoreCase))
		{
			return TEXT("the name is reserved by RmlUi (it, it_index, ev, true, false, size, literal -- compared "
						"case-insensitively)");
		}
	}

	return nullptr;
}

const TCHAR* VaCuusWireName::ValidateNested(const FString& Name)
{
	using namespace VaCuusModelLayoutPrivate;

	if (Name.IsEmpty())
	{
		return TEXT("the name is empty");
	}

	for (const TCHAR Char : Name)
	{
		if (!IsWireNameBodyChar(Char))
		{
			return TEXT("a segment of a dotted address may contain only A-Z, a-z, 0-9 and underscore");
		}
	}

	return nullptr;
}

FVaCuusModelLayout::FVaCuusModelLayout(const UScriptStruct* InStruct)
{
	// The stack lives on THIS frame and threads by reference through every element layout
	// constructed below it -- per build TREE, not per layout, which is the only scope a
	// container cycle is visible at (see the private constructor in the header).
	TArray<const UScriptStruct*> BuildStack;
	Build(InStruct, BuildStack);
}

FVaCuusModelLayout::FVaCuusModelLayout(const UScriptStruct* InStruct, TArray<const UScriptStruct*>& BuildStack)
{
	Build(InStruct, BuildStack);
}

void FVaCuusModelLayout::Build(const UScriptStruct* InStruct, TArray<const UScriptStruct*>& BuildStack)
{
	if (InStruct == nullptr)
	{
		UE_LOG(LogVaCuus, Error, TEXT("VaCuus model layout: no struct type was given; nothing is bound"));
		return;
	}

	// Root build or element build? Decided by the stack rather than a flag: only the public
	// constructor starts with an empty one.
	const bool bRootBuild = BuildStack.IsEmpty();

	// STRONG, not raw. A native UScriptStruct is created with RF_MarkAsNative, which
	// becomes EInternalObjectFlags::Native, which is one of the GC keep flags
	// (ObjectMacros.h:707-709) -- so for those a raw pointer would be safe forever. A
	// UUserDefinedStruct is an ordinary collectable object with none of that
	// (UserDefinedStruct.cpp / spec 6), and every FProperty* below is owned by it. Taking
	// the reference unconditionally is what makes the Blueprint case indistinguishable
	// from the native one at every use site.
	Struct.Reset(InStruct);

	// ON THE STACK FOR THE DURATION of this layout's build: the array interception refuses
	// any element type it finds in here, which is what terminates a container-cyclic type
	// graph that neither UHT nor MaxNestingDepth can stop (the guard carries the argument).
	BuildStack.Push(InStruct);
	BuildLevel(InStruct, FString(), /*BaseOffset=*/0, /*TopLevelNameIndex=*/INDEX_NONE, /*Depth=*/0, BuildStack);
	BuildStack.Pop();

	// ARRAY-DESC FIX-UP, AFTER THE BUILD AND NEVER DURING IT. BuildLevel appends to ArrayDescs
	// while it appends to Fields, so mid-build the table can still reallocate and only the
	// INDEX is stable. From here on the layout is immutable -- nothing appends after the
	// constructor returns -- so the pointer written now cannot dangle; moving the layout moves
	// the table's allocation ownership, not its elements' addresses. The pointer exists so
	// CopyValue reaches SyncCopy without callers carrying the layout (the funnel; see the
	// header).
	for (FVaCuusModelField& Field : Fields)
	{
		if (Field.ArrayDescIndex != INDEX_NONE)
		{
			Field.ArrayDesc = &ArrayDescs[Field.ArrayDescIndex];
		}
	}

	// ROOT BUILDS ONLY. For an element layout this line is wrong twice over: it names the
	// row type as if it were a model root, and "the document will resolve nothing against
	// this model" is false -- the document resolves against the ARRAY, whose desc build
	// refuses the field with a Warning naming the array property (the one diagnostic that
	// case gets).
	if (Fields.IsEmpty() && bRootBuild)
	{
		UE_LOG(LogVaCuus, Warning,
			TEXT("VaCuus model '%s': no property could be bound; the document will resolve nothing against this model"),
			*InStruct->GetName());
	}
}

const FVaCuusModelField* FVaCuusModelLayout::FindField(FStringView InWireName) const
{
	return Fields.FindByPredicate([InWireName](const FVaCuusModelField& Field) { return Field.WireName == InWireName; });
}

void FVaCuusModelLayout::BuildLevel(const UScriptStruct* InStruct, const FString& Prefix, int32 BaseOffset,
	int32 TopLevelNameIndex, int32 Depth, TArray<const UScriptStruct*>& BuildStack)
{
	using namespace VaCuusModelLayoutPrivate;

	const FString ModelName = Struct->GetName();
	const bool bTopLevel = Prefix.IsEmpty();

	// PropertyLink, NOT TFieldIterator. UStruct::Link builds this list by walking
	// TFieldIterator<FProperty> in its DEFAULT flags and appending in iteration order
	// (Class.cpp:1042, 1096-1099), so the two yield exactly the same sequence -- this one
	// without a cast-flag test and a struct-chain hop per node. Head-to-tail is C++
	// declaration order, most-derived struct first: UHT emits PropPointers in declaration
	// order, ConstructFProperties walks that array backwards (UObjectGlobals.cpp:6499-6507)
	// and AddCppProperty prepends (Class.cpp:723-727), so the two reversals cancel.
	for (const FProperty* Property = InStruct->PropertyLink; Property != nullptr; Property = Property->PropertyLinkNext)
	{
		// DEPRECATED PROPERTIES ARE SKIPPED, and this is a decision, not an inherited
		// default. EFieldIterationFlags::Default is IncludeSuper|IncludeDeprecated
		// (UnrealType.h:7143) and PropertyLink is built from it, so they arrive here; and
		// the exposure test below would not remove them, because CPF_Deprecated is
		// orthogonal to CPF_BlueprintVisible and a property usually keeps both. The reason
		// to skip: ObjectMacros.h:463 defines CPF_Deprecated as "read it from an archive,
		// but don't save it" -- nothing maintains the value, so binding one puts a stale
		// number on screen. Logged at Log rather than Warning because the outcome is
		// correct, not defective.
		if (Property->HasAnyPropertyFlags(CPF_Deprecated))
		{
			UE_LOG(LogVaCuus, Log, TEXT("VaCuus model '%s': skipping deprecated property '%s%s'"), *ModelName, *Prefix,
				*Property->GetAuthoredName());
			continue;
		}

		// EDITOR-ONLY PROPERTIES ARE SKIPPED SO THE EDITOR AND THE SHIPPED GAME AGREE.
		// A CPF_EditorOnly property is declared inside #if WITH_EDITORONLY_DATA, so it does
		// not exist at all in a Game target: binding it here would build a layout in PIE
		// that the packaged build cannot reproduce, and the document would work in the
		// editor and silently show nothing in the game. That is precisely the failure class
		// this milestone is built against.
		if (Property->IsEditorOnlyProperty())
		{
			UE_LOG(LogVaCuus, Log,
				TEXT("VaCuus model '%s': skipping editor-only property '%s%s' (it does not exist in a packaged build)"),
				*ModelName, *Prefix, *Property->GetAuthoredName());
			continue;
		}

		// EXPOSURE IS FLAGS, NEVER METADATA. The whole HasMetaData/GetMetaData API lives
		// inside #if WITH_METADATA == WITH_EDITORONLY_DATA (Field.h:916-1073,
		// CoreMiscDefines.h:29-34), and UHT does not even emit the strings for a Game
		// target (METADATA_PARAMS expands to nothing, UObjectGlobals.h:4075-4087). Flags
		// are compiled in unconditionally.
		//
		// CPF_Edit is in the set as a deliberate CONVENIENCE, not a semantic claim: it
		// means "the author ticked EditAnywhere", which is not the same as "the shipped
		// game considers this visible". It earns its place concretely -- FUtf8String and
		// FAnsiString CANNOT be BlueprintVisible at all (UhtUtf8StrProperty.cs:46-47 and
		// UhtAnsiStrProperty.cs leave IsMemberSupportedByBlueprint out of PropertyCaps), so
		// without CPF_Edit two of M3a's supported kinds would be unreachable.
		//
		// Verbose, not Warning: a struct with unexposed members is the normal case, and
		// this line would fire for every one of them on every model.
		if (!Property->HasAnyPropertyFlags(CPF_BlueprintVisible | CPF_Edit))
		{
			UE_LOG(LogVaCuus, Verbose, TEXT("VaCuus model '%s': '%s%s' is exposed to neither Blueprint nor the details panel"),
				*ModelName, *Prefix, *Property->GetAuthoredName());
			continue;
		}

		// A fixed-size C array (UPROPERTY() float Foo[4]) is ONE FProperty with ArrayDim 4.
		// Binding element 0 and dropping the rest would be a silent partial bind, which is
		// worse than not binding it.
		//
		// IT STAYS REFUSED NOW THAT TArray BINDS -- the revisit M3a promised, decided against
		// (spec 3.2). A fixed array cannot be Blueprint-exposed at all (UhtScriptStruct.cs:
		// 1147-1149, UhtClass.cs:2203-2205), so its whole audience is C++ code for which a
		// TArray is strictly more idiomatic; and support would fork the copy contract, because
		// CopySingleValue copies exactly one element from an element address while the whole
		// property needs CopyCompleteValue's ArrayDim loop (UnrealType.h:881-894, :915-928) --
		// a second copy shape for one rare shape of data.
		if (Property->ArrayDim > 1)
		{
			UE_LOG(LogVaCuus, Warning,
				TEXT("VaCuus model '%s': property '%s%s' (%s) cannot be bound -- a fixed-size array (ArrayDim %d) is one "
					 "property with many values and cannot be Blueprint-exposed; use a TArray, which binds"),
				*ModelName, *Prefix, *Property->GetAuthoredName(), *Property->GetCPPType(), Property->ArrayDim);
			continue;
		}

		// GetAuthoredName(), never GetName(). For a native struct they are the same string;
		// for a UUserDefinedStruct member GetName() is `<Base>_<UniqueId>_<32hexGUID>`
		// (StructureEditorUtils.cpp:256-264) and GetAuthoredName chops it back to what the
		// designer typed (UserDefinedStruct.cpp:281-314), in editor AND cooked builds
		// (Field.h:898-903 states that contract). Returns an FString by value, so this is a
		// build-time call and never a per-frame one.
		const FString AuthoredName = Property->GetAuthoredName();

		// Two rules, by position -- see VaCuusWireName in the header for why nested
		// segments are held to the weaker one.
		if (const TCHAR* NameError =
				bTopLevel ? VaCuusWireName::ValidateTopLevel(AuthoredName) : VaCuusWireName::ValidateNested(AuthoredName))
		{
			// ERROR, AND OMITTED. Renaming it to something legal would put a name in the
			// document that appears nowhere in the C++. Leaving it to RmlUi would produce a
			// Log::LT_WARNING (DataModel.cpp:119-124) which does reach LogVaCuus -- see
			// FVaCuusSystemInterface::LogMessage -- but names only the wire name RmlUi was
			// handed, not the property it came from, and then an absent variable whose every
			// reference warns again about something that looks like a document bug. This line
			// names the model, the property and the rule.
			UE_LOG(LogVaCuus, Error, TEXT("VaCuus model '%s': property '%s' cannot be bound under the name '%s%s' -- %s"),
				*ModelName, *Property->GetName(), *Prefix, *AuthoredName, NameError);
			continue;
		}

		const FString WireName = Prefix + AuthoredName;

		if (bTopLevel ? TopLevelNames.Contains(WireName) : (FindField(WireName) != nullptr))
		{
			// Reachable without anyone writing a duplicate: GetAuthoredName() chops a
			// Blueprint member down to its base, and two never-renamed members both chop to
			// "MemberVar" in a cooked build (UserDefinedStruct.cpp:300-312).
			UE_LOG(LogVaCuus, Error,
				TEXT("VaCuus model '%s': property '%s' cannot be bound under the name '%s' -- that name is already taken by "
					 "another property"),
				*ModelName, *Property->GetName(), *WireName);
			continue;
		}

		// ARRAYS ARE INTERCEPTED HERE, NOT CLASSIFIED, for the same reason nested structs
		// are: the element description needs layout state -- a desc table entry, possibly a
		// whole element layout -- that the pure classifier cannot build. The array itself is
		// a LEAF: one entry, one dirty bit, addressed by ContainerOffset like any other leaf,
		// with an FVaCuusModelArrayDesc on the side because element count is per-instance
		// while leaf count is fixed at build time (spec 3.1).
		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			FVaCuusModelArrayDesc Desc;
			Desc.ArrayProperty = ArrayProperty;
			Desc.Inner = ArrayProperty->Inner;

			if (const FStructProperty* InnerStruct = CastField<FStructProperty>(Desc.Inner))
			{
				if (InnerStruct->Struct == nullptr)
				{
					UE_LOG(LogVaCuus, Warning, TEXT("VaCuus model '%s': array property '%s' has no element type; skipped"),
						*ModelName, *WireName);
					continue;
				}

				// THE CYCLE GUARD, and it must run BEFORE the element layout is constructed:
				// building one for a type that is still being built above us recurses until the
				// process stack overflows. Nothing else stops the shape at THIS level. A TArray
				// member is heap indirection, so the infinite-size argument that terminates
				// by-value nesting dies at the pointer; and MaxNestingDepth never fires on the
				// way down, because each element layout is a fresh build whose Depth restarts
				// at 0. UHT refuses every NATIVE writing of the loop -- the direct shape by its
				// explicit check, `structProperty.ScriptStruct == outerStruct`
				// (UhtArrayProperty.cs:216-222), the mutual pair FA{TArray<FB>}/FB{TArray<FA>}
				// and the by-value hop FRow{FSub}/FSub{TArray<FRow>} at the forward reference
				// they cannot avoid (zero code-generation hash, UhtProperty.cs:3066-3071) --
				// but UHT guards source text, not the FProperty graph: a runtime-built
				// UUserDefinedStruct closes the loop with no validator anywhere on the path,
				// and this build is the first thing that would walk it.
				if (BuildStack.Contains(InnerStruct->Struct))
				{
					// The loop by name, from the element type's first appearance back to
					// itself, so the log reader sees the whole cycle and not just its last edge.
					FString Cycle;
					for (int32 StackIndex = BuildStack.IndexOfByKey(InnerStruct->Struct); StackIndex < BuildStack.Num();
						 ++StackIndex)
					{
						Cycle += BuildStack[StackIndex]->GetName() + TEXT(" -> ");
					}
					Cycle += InnerStruct->Struct->GetName();

					UE_LOG(LogVaCuus, Warning,
						TEXT("VaCuus model '%s': array property '%s' (%s) cannot be bound -- element type '%s' participates in "
							 "a container cycle (%s), which a flat layout cannot terminate; break the cycle or bind a "
							 "different type"),
						*ModelName, *WireName, *Property->GetCPPType(), *InnerStruct->Struct->GetName(), *Cycle);
					continue;
				}

				// A PLAIN LAYOUT, DELIBERATELY (spec 3.1): the element type gets the same
				// flattening, classifier, name rules and pinning a model root gets, because the
				// definition registry keys on the raw UScriptStruct* and a type used both as a
				// root and as a row type cannot carry two policies. The consequences are owned:
				// element TOP-LEVEL member names obey the full root rule -- a row member named
				// `Size` is refused with the root Error, and the fix is a rename -- while what
				// a shared layout cannot refuse, the scan below refuses on the ARRAY FIELD.
				// Plain `new`, not MakeUnique, only because the stack-sharing constructor is
				// private and MakeUnique is not a friend.
				Desc.ElementLayout = TUniquePtr<FVaCuusModelLayout>(new FVaCuusModelLayout(InnerStruct->Struct, BuildStack));

				// A ROW TYPE WITH NOTHING TO BIND refuses the array too. With zero element
				// leaves the only observable left is Num(), so the binding would render row
				// counts and never row content -- a document that looks bound and shows
				// nothing, this milestone's signature failure. The element build suppressed
				// its root-flavored "no property could be bound" line (see Build), so this
				// Warning, naming the ARRAY property, is the one diagnostic.
				if (Desc.ElementLayout->GetFields().IsEmpty())
				{
					UE_LOG(LogVaCuus, Warning,
						TEXT("VaCuus model '%s': array property '%s' (%s) cannot be bound -- row type '%s' has no bindable "
							 "member, so only the element count could ever reach a document; expose a member of the row type"),
						*ModelName, *WireName, *Property->GetCPPType(), *InnerStruct->Struct->GetName());
					continue;
				}

				// THE DESC-BUILD SCAN, over the element layout's flat leaf list: every BINDABLE
				// leaf within MaxNestingDepth of the element type, which is exactly the set the
				// binding will ever read. Two kinds refuse the whole array field:
				//
				//  - Text, ANYWHERE in the subtree: M3a's Text contract -- shadow and compare
				//    the display string -- is a per-field projection at StoreField
				//    (VaCuusModelSampler.cpp) that a whole-container copy bypasses, and an
				//    unprojected FText in the UI shadow would resolve localization on the UI
				//    thread, the exact race the sampler pins to the game thread.
				//  - Array, i.e. a nested container: dirtiness is one bit per TOP-LEVEL array,
				//    so an inner array's cost would multiply invisibly under a single bit.
				//
				// WHAT THE SCAN CANNOT SEE RIDES ALONG INERT, and that is safe by shape, not by
				// luck. A member that is unexposed, deprecated, editor-only, illegally named or
				// deeper than MaxNestingDepth has no leaf, so it neither surfaces here nor
				// refuses the array -- yet SyncCopy's whole-row Inner->CopyCompleteValue copies
				// it anyway: FStructProperty::CopyValuesInternal is UScriptStruct::CopyScriptStruct
				// (PropertyStruct.cpp:341-344), whose property loop applies no exposure filter
				// (Class.cpp:3697-3731). Payload with no leaf is never read and never diffed,
				// so: a hidden FText copies as an atomic refcount bump and resolves no
				// localization (FText's copy is defaulted over TRefCountPtr<ITextData>,
				// Text.h:416, :941, thread-safe count via TextHistory.h:143 +
				// RefCounting.h:190-197); a hidden hard UObject* copies as bytes no stage
				// dereferences (see the classifier's refusal for the invariant as enforced).
				//
				// One Warning, first offender: finding one is enough to refuse, and one line
				// naming the array, the member and the reason is what a designer can act on.
				const FVaCuusModelField* Offender = nullptr;
				const TCHAR* OffenceReason = nullptr;
				for (const FVaCuusModelField& Leaf : Desc.ElementLayout->GetFields())
				{
					if (Leaf.Kind == EVaCuusFieldKind::Text)
					{
						Offender = &Leaf;
						OffenceReason = TEXT("an FText, whose display-string projection is per field and would be bypassed by a "
											 "whole-array copy; project it to an FString on the game side");
						break;
					}
					if (Leaf.Kind == EVaCuusFieldKind::Array)
					{
						Offender = &Leaf;
						OffenceReason = TEXT("itself a container, and dirtiness is one bit per top-level array, so an inner "
											 "array's cost would be invisible under it");
						break;
					}
				}
				if (Offender != nullptr)
				{
					UE_LOG(LogVaCuus, Warning,
						TEXT("VaCuus model '%s': array property '%s' (%s) cannot be bound -- element member '%s' is %s"),
						*ModelName, *WireName, *Property->GetCPPType(), *Offender->WireName, OffenceReason);
					continue;
				}
			}
			else
			{
				// Scalar elements share the field classifier -- the per-kind rules are the same
				// rules, value-pointer form -- so a refused element kind carries the same reason
				// a refused field of that kind would.
				EVaCuusFieldKind ElementKind = EVaCuusFieldKind::Bool;
				const TCHAR* ElementReason = nullptr;
				if (!ClassifyProperty(Desc.Inner, ElementKind, ElementReason))
				{
					UE_LOG(LogVaCuus, Warning,
						TEXT("VaCuus model '%s': array property '%s' (%s) cannot be bound -- its element type cannot be: %s"),
						*ModelName, *WireName, *Property->GetCPPType(), ElementReason);
					continue;
				}

				// Text ELEMENTS are refused even though Text FIELDS bind: the classifier's
				// answer is right for a field, where StoreField projects the display string per
				// leaf; an element has no leaf of its own to project through (spec 3.2).
				if (ElementKind == EVaCuusFieldKind::Text)
				{
					UE_LOG(LogVaCuus, Warning,
						TEXT("VaCuus model '%s': array property '%s' (%s) cannot be bound -- FText elements would bypass the "
							 "per-field display-string projection and resolve localization on the UI thread; project to FString "
							 "on the game side"),
						*ModelName, *WireName, *Property->GetCPPType());
					continue;
				}

				Desc.ElementKind = ElementKind;
			}

			FVaCuusModelField& Field = Fields.AddDefaulted_GetRef();
			Field.Property = Property;
			Field.WireName = WireName;
			Field.TopLevelNameIndex = bTopLevel ? TopLevelNames.Add(WireName) : TopLevelNameIndex;
			Field.ContainerOffset = BaseOffset;
			Field.Kind = EVaCuusFieldKind::Array;

			// The INDEX now, the pointer later: the table can still reallocate while this
			// level and its siblings keep appending, so the constructor fixes ArrayDesc up
			// only after BuildLevel has returned for good.
			Field.ArrayDescIndex = ArrayDescs.Add(MoveTemp(Desc));
			continue;
		}

		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == nullptr)
			{
				UE_LOG(LogVaCuus, Warning, TEXT("VaCuus model '%s': struct property '%s' has no type; skipped"), *ModelName,
					*WireName);
				continue;
			}

			if (Depth >= MaxNestingDepth)
			{
				UE_LOG(LogVaCuus, Warning,
					TEXT("VaCuus model '%s': nested struct property '%s' is deeper than the %d-level binding limit; skipped"),
					*ModelName, *WireName, MaxNestingDepth);
				continue;
			}

			// The nested struct itself gets NO entry -- only its leaves, which is what
			// "flattened at build time" means. It does get a top-level name when it is one,
			// because that name is what its leaves dirty.
			const int32 NestedNameIndex = bTopLevel ? TopLevelNames.Add(WireName) : TopLevelNameIndex;
			const int32 FieldsBefore = Fields.Num();

			// Offsets compose, and GetOffset_ForInternal is the honest accessor for this:
			// the scolding name at UnrealType.h:466 belongs to the case where you want a
			// VALUE, and ContainerPtrToValuePtr is that answer. Building a container-offset
			// chain is the one case that has no ContainerPtrToValuePtr form, because the
			// chain has to exist before there is a pointer to apply it to. Every read
			// downstream still goes through the blessed accessor, via
			// FVaCuusModelField::ContainerPtr.
			BuildLevel(StructProperty->Struct, WireName + TEXT("."), BaseOffset + StructProperty->GetOffset_ForInternal(),
				NestedNameIndex, Depth + 1, BuildStack);

			if (Fields.Num() == FieldsBefore)
			{
				UE_LOG(LogVaCuus, Warning,
					TEXT("VaCuus model '%s': nested struct property '%s' (%s) contributed no bindable field; it will not "
						 "appear in the model"),
					*ModelName, *WireName, *StructProperty->Struct->GetName());

				// Roll the name back rather than leave an orphan. A top-level name with no
				// field behind it would be dirtied by nothing and resolve to nothing, and
				// TopLevelNames is what the bind step iterates. Safe as a RemoveAt: only a
				// top-level property appends, and this is the append that just happened.
				if (bTopLevel)
				{
					check(NestedNameIndex == TopLevelNames.Num() - 1);
					TopLevelNames.RemoveAt(NestedNameIndex);
				}
			}

			continue;
		}

		EVaCuusFieldKind Kind = EVaCuusFieldKind::Bool;
		const TCHAR* Reason = nullptr;
		if (!ClassifyProperty(Property, Kind, Reason))
		{
			UE_LOG(LogVaCuus, Warning, TEXT("VaCuus model '%s': property '%s' (%s) cannot be bound -- %s"), *ModelName,
				*WireName, *Property->GetCPPType(), Reason);
			continue;
		}

		FVaCuusModelField& Field = Fields.AddDefaulted_GetRef();
		Field.Property = Property;
		Field.WireName = WireName;
		Field.TopLevelNameIndex = bTopLevel ? TopLevelNames.Add(WireName) : TopLevelNameIndex;
		Field.ContainerOffset = BaseOffset;
		Field.Kind = Kind;
	}
}
