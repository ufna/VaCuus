// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"

class FProperty;
class UScriptStruct;

/**
 * What a bound field IS, decided once at layout-build time.
 *
 * ONE ENUMERATOR PER READ ACCESSOR, not per C++ width: every kind below names exactly
 * one way to get the value out of a container pointer, which is what the sampler and
 * the RmlUi adapter each need to switch on. Widths that share an accessor share a kind
 * (int8..int64 all read through FNumericProperty::GetSignedIntPropertyValue,
 * UnrealType.h:1886-1902), because splitting them would multiply both switches without
 * changing either one's behaviour.
 */
enum class EVaCuusFieldKind : uint8
{
	/**
	 * FBoolProperty, native `bool` AND `uint8 b : 1` bitfield alike. One kind, because
	 * FBoolProperty::GetPropertyValue is already mask-aware and handles both
	 * (UnrealType.h:2682-2691: `!!(*ByteValue & FieldMask)`, where FieldMask is 255 for
	 * a native bool and the isolated bit for a bitfield -- PropertyBool.cpp:80-91). What
	 * must NOT be shared is the diff: a bitfield's storage byte carries up to seven
	 * unrelated bits, so the comparison has to run on the extracted bool. Spec 5.
	 */
	Bool,

	/** FInt8/16/64Property, FIntProperty. GetSignedIntPropertyValue -> int64. */
	SignedInt,

	/** FUInt16/32/64Property and a plain FByteProperty. GetUnsignedIntPropertyValue -> uint64. */
	UnsignedInt,

	/** FFloatProperty, FDoubleProperty. GetFloatingPointPropertyValue -> double. */
	FloatingPoint,

	/** FStrProperty -> FString. */
	String,

	/**
	 * FUtf8StrProperty -> FUtf8String. A SEPARATE KIND, not a String, because
	 * CastField<FStrProperty> silently misses it: the cast is a flag test against
	 * CASTCLASS_FUtf8StrProperty (ObjectMacros.h:407) which FStrProperty's
	 * CASTCLASS_FStrProperty (:361) does not include, so a UPROPERTY(FUtf8String) that
	 * is only checked for FStrProperty just never appears in the model.
	 */
	Utf8String,

	/** FAnsiStrProperty -> FAnsiString. Separate for the same reason (CASTCLASS_FAnsiStrProperty). */
	AnsiString,

	/** FNameProperty -> FName. Diffed as an index, shipped as a string. */
	Name,

	/**
	 * FTextProperty -> FText, shadowed and diffed as its DISPLAY STRING.
	 * FTextProperty::Identical picks EIdenticalLexicalCompareMethod::None when
	 * !GIsEditor (TextProperty.cpp:63-67) and then reports two equal-looking texts as
	 * different, i.e. exactly in the builds that ship. Spec 5.
	 */
	Text,

	/**
	 * FEnumProperty, or an FByteProperty carrying a UEnum. ONE variable holding the
	 * name string (spec 3.4): exposing name and value would need two legal wire names
	 * per property, and the numeric value is reachable by binding the underlying
	 * integer property when a document wants it.
	 */
	Enum,

	/**
	 * FSoftObjectProperty, which FSoftClassProperty derives from. Read as its path
	 * string, never resolved.
	 *
	 * SOFT ONLY -- FWeakObjectProperty is refused, correcting spec 3.4. An FSoftObjectPtr
	 * already IS a path (ToString() is GetUniqueID().ToString(), SoftObjectPtr.h:96-105),
	 * so no GC-visible state is touched and no ownership is implied, which is the whole
	 * reason it is in where a hard FObjectProperty is out. An FWeakObjectPtr is an
	 * index/serial pair with no path in it, and turning one into a path means resolving
	 * the object -- which WeakObjectPtr.h:295-296 says cannot be done from another
	 * thread. See the classifier for the full argument.
	 */
	ObjectPath,
};

VACUUS_API const TCHAR* LexToString(EVaCuusFieldKind Kind);

/** One bound leaf. Nested structs contribute their leaves and no entry of their own. */
struct FVaCuusModelField
{
	/**
	 * The resolved leaf property. NEVER an FStructProperty -- nesting is flattened at
	 * build time (spec 3.2), which is what keeps FStructProperty::Identical (a fresh
	 * TFieldIterator per call, Class.cpp:3683-3692) off the per-frame path.
	 *
	 * Safe to hold raw: an already-linked type's FProperty chain is not rewritten at
	 * runtime, and the layout holds a strong reference to the owning UScriptStruct,
	 * which is what actually owns these.
	 */
	const FProperty* Property = nullptr;

	/**
	 * The dotted path a document author writes: "Health", or "Crosshair.X" for a leaf
	 * of a flattened nested struct. From FField::GetAuthoredName(), never GetName():
	 * a Blueprint member is named `<Base>_<UniqueId>_<32hexGUID>`
	 * (StructureEditorUtils.cpp:256-264) and `{{Health}}` must not need the GUID.
	 */
	FString WireName;

	/**
	 * Index into FVaCuusModelLayout::GetTopLevelNames() -- the first segment of
	 * WireName, and the ONLY granularity RmlUi's DirtyVariable accepts. A leaf of a
	 * nested struct dirties the struct's name, not its own.
	 */
	int32 TopLevelNameIndex = INDEX_NONE;

	/**
	 * Byte offset from the model struct's base to the struct instance that DIRECTLY
	 * contains Property; 0 for a top-level field. Flattening makes this necessary:
	 * Property->Offset_Internal is relative to its own owner, so a nested leaf cannot
	 * be addressed from the root without it.
	 *
	 * ONE offset, not two. This is the same number in the live struct and in a shadow
	 * buffer precisely because the shadow is a real UScriptStruct instance (spec 3.1);
	 * v1's separate "struct offset" and "shadow offset" were the incoherent form of
	 * this (spec 13.2).
	 */
	int32 ContainerOffset = 0;

	EVaCuusFieldKind Kind = EVaCuusFieldKind::Bool;

	/**
	 * The container to hand to Property's *_InContainer accessors, or to
	 * ContainerPtrToValuePtr. Exists so that no caller ever does the offset arithmetic
	 * itself -- FProperty offers five identically-behaved offset accessors, one of
	 * which is literally named GetOffset_ReplaceWith_ContainerPtrToValuePtr
	 * (UnrealType.h:446-469), and the blessed path is the only one that stays correct
	 * for bitfields and fixed arrays.
	 */
	const void* ContainerPtr(const void* StructBase) const
	{
		return static_cast<const uint8*>(StructBase) + ContainerOffset;
	}
	void* ContainerPtr(void* StructBase) const
	{
		return static_cast<uint8*>(StructBase) + ContainerOffset;
	}

	/**
	 * Copies THIS FIELD ONLY between two instances of the model type -- live struct, game
	 * shadow, channel slot and UI shadow are all instances of it, so this is the one copy
	 * every stage of the pipeline uses.
	 *
	 * CopySingleValue AND NOT A memcpy OF GetElementSize() BYTES, and the difference is a
	 * bitfield. For `uint8 b : 1` the value pointer addresses the storage integer that up to
	 * seven unrelated bitfields share; CopySingleValue routes a non-POD property through
	 * CopyValuesInternal (UnrealType.h:881-894), and FBoolProperty's override is a
	 * read-modify-write under FieldMask (PropertyBool.cpp:442-451), so the siblings in the
	 * destination survive. A native bool takes the POD path instead -- SetBoolSize sets
	 * CPF_IsPlainOldData only for one (PropertyBool.cpp:67-73) -- which is why the same call
	 * is correct for both, and why FBoolProperty::CopyValuesInternal can assert !IsNativeBool()
	 * (:444) without that assert ever firing here.
	 *
	 * Single, not Complete: FVaCuusModelLayout refuses ArrayDim > 1, so there is exactly one
	 * value per entry.
	 *
	 * Out of line, unlike ContainerPtr above, only because calling into FProperty needs the
	 * complete type and this is a Public header that today gets away with a forward
	 * declaration.
	 */
	VACUUS_API void CopyValue(void* DestStructBase, const void* SourceStructBase) const;
};

/**
 * The flat, pre-resolved description of one model type: built ONCE per UScriptStruct,
 * one entry per bound leaf, nested structs flattened, no TFieldIterator afterwards.
 *
 * WHY FLAT AND PRE-RESOLVED. This is deliberately the shape UE's own per-frame differ
 * uses. FRepLayout keeps a flat command list with a typed comparator per entry and
 * falls back to the virtual Identical only for kinds it did not special-case
 * (RepLayout.cpp:668-753). The reason is cost: FStructProperty::Identical spins up a
 * fresh TFieldIterator per call (Class.cpp:3683-3692) and FMapProperty::Identical is
 * O(n^2) via IsPermutation (PropertyMap.cpp:85-148). A binder that reached for those
 * once per frame would become the frame's dominant cost.
 *
 * THREADING. Building is safe on any thread for a type that is already loaded and
 * linked: nothing in a cooked runtime rewrites a linked type's ChildProperties /
 * PropertyLink chain or its FProperty offsets and flags, and TStrongObjectPtr's
 * ref-count is an InterlockedIncrement on the object's FUObjectItem
 * (UObjectArray.h:347-359 via GarbageCollection.cpp:6652-6662). Do NOT build one over a
 * type the async loading thread is still linking.
 *
 * WHAT IS DELIBERATELY ABSENT. There is no hard FObjectProperty kind, and the reason is
 * GC rather than data races: the UI-side shadow buffer this layout addresses is a
 * UScriptStruct instance the UI thread owns, so nothing ever calls
 * AddStructReferencedObjects on it and it is invisible to the collector. A hard
 * UObject* inside it would dangle with no diagnostic (spec 3.1).
 *
 * INDICES ARE NOT IDENTITIES. Entry order is declaration order and stable within a
 * build only -- reordering UPROPERTYs reorders it, and a Blueprint struct is rebuilt
 * from editor data on every recompile. Never persist an index.
 */
class VACUUS_API FVaCuusModelLayout
{
public:
	/**
	 * How many struct properties deep the flattening will go. Not a cycle guard: a
	 * USTRUCT cannot contain itself by value at any depth, because its own size would
	 * have to be infinite, so the recursion terminates by construction. This bounds
	 * the wire names instead -- every level adds a dotted segment that a document
	 * author has to type.
	 */
	static constexpr int32 MaxNestingDepth = 4;

	FVaCuusModelLayout() = default;

	/** Builds the layout. Diagnostics for everything skipped go to LogVaCuus. */
	explicit FVaCuusModelLayout(const UScriptStruct* InStruct);

	/** True once a struct was resolved; a struct with no bindable field is still valid, and empty. */
	bool IsValid() const { return Struct.IsValid(); }

	const UScriptStruct* GetStruct() const { return Struct.Get(); }

	/** One entry per bound leaf, in declaration order. */
	TConstArrayView<FVaCuusModelField> GetFields() const { return Fields; }

	/** The names DirtyVariable takes, in first-appearance order. */
	TConstArrayView<FString> GetTopLevelNames() const { return TopLevelNames; }

	/** Exact match on the dotted path. Linear; a build-time and diagnostic helper, not a per-frame one. */
	const FVaCuusModelField* FindField(FStringView InWireName) const;

private:
	/** Walks one struct level, appending leaves and recursing into nested structs. */
	void BuildLevel(const UScriptStruct* InStruct, const FString& Prefix, int32 BaseOffset, int32 TopLevelNameIndex, int32 Depth);

	TStrongObjectPtr<const UScriptStruct> Struct;
	TArray<FVaCuusModelField> Fields;
	TArray<FString> TopLevelNames;
};

/**
 * RmlUi's variable-name rule, restated in UE terms because the layout must refuse a
 * name BEFORE RmlUi sees it (spec 3.3). RmlUi's own refusal is a Log::LT_WARNING and a
 * `false` from BindVariable (DataModel.cpp:119-124), after which the variable is simply
 * absent and every reference to it fails address resolution -- and the library's
 * logging is compiled out in every configuration this plugin builds (spec 8), so that
 * refusal is invisible.
 *
 * TWO RULES, NOT ONE, AND THIS IS A CORRECTION TO THE SPEC. Spec 3.3 says to validate
 * "each wire name" against LegalVariableName. RmlUi applies that function in exactly
 * two places, BindVariable and BindEventCallback (DataModel.cpp:119, 160), i.e. only to
 * the name a variable is REGISTERED under -- the first segment of a dotted address.
 * `{{Panel.Size}}` is parsed by ParseAddress, which splits on '.' and applies no name
 * rule at all (DataModel.cpp:9-46), and resolved by DataVariable::Child
 * (DataModel.cpp:285-290). So `Size` is unbindable as a top-level name and perfectly
 * bindable as a nested member, and refusing the second would refuse a field that works.
 */
namespace VaCuusWireName
{
/**
 * The full rule, for a name that will be passed to BindVariable: first character a
 * letter, the rest [A-Za-z0-9_], and not one of {it, it_index, ev, true, false, size,
 * literal} compared case-insensitively (DataModel.cpp:51-74 -- note it lowercases the
 * whole name first, so both the first-character test and the reserved-word test are
 * case-insensitive).
 *
 * @return nullptr when the name is legal, otherwise the reason, phrased for a log line.
 */
VACUUS_API const TCHAR* ValidateTopLevel(const FString& Name);

/**
 * The weaker rule that a non-first segment of a dotted address must satisfy: non-empty
 * and [A-Za-z0-9_] throughout. That set is not ours -- it is what the data-expression
 * lexer accepts after the first character (DataExpression.cpp:315-332, which also
 * admits '.' as the segment separator), so a segment containing anything else
 * terminates the lexed address early and the rest is parsed as an operator. The
 * first-character and reserved-word rules do NOT apply here: RmlUi's keyword check
 * compares the whole dotted string (`name == "true"`, DataExpression.cpp:668-671) and
 * its alias and `literal` lookups both key on address.front() only
 * (DataModel.cpp:234-247, 295).
 *
 * @return nullptr when the name is legal, otherwise the reason.
 */
VACUUS_API const TCHAR* ValidateNested(const FString& Name);
}	 // namespace VaCuusWireName
