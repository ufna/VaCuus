// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"

class FArrayProperty;
class FProperty;
class FVaCuusModelLayout;
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

	/**
	 * FArrayProperty -> TArray<T>. A LEAF WITH A SUB-DESCRIPTION, not a flattened subtree:
	 * leaf count is fixed at layout-build time while element count is per-instance, so an
	 * array contributes ONE entry, one dirty bit -- the only granularity RmlUi can express
	 * anyway, since its view map keys on top-level names alone -- and one
	 * FVaCuusModelArrayDesc (FVaCuusModelField::ArrayDesc) describing the element type.
	 * Element addresses are computed per use and stored nowhere:
	 * FScriptArrayHelper::GetRawPtr(i) is GetData() + i*ElementSize evaluated at call time
	 * (UnrealType.h:4324-4333), which is what makes "valid only for the current Num()"
	 * harmless.
	 */
	Array,
};

VACUUS_API const TCHAR* LexToString(EVaCuusFieldKind Kind);

/**
 * What one Array field's ELEMENTS are, decided once at layout-build time -- the side table
 * an EVaCuusFieldKind::Array leaf points into through FVaCuusModelField::ArrayDesc.
 *
 * PER-TYPE AND STATELESS, like everything else in the layout: no element pointer, no cached
 * Num(), nothing per-instance. Every consumer constructs an FScriptArrayHelper over the
 * incoming value pointer at call time -- the helper caches only the FScriptArray* itself, so
 * it survives reallocation, while a SAVED GetRawPtr() result does not (UnrealType.h:4324-4333).
 * "No stage stores an element address" is the invariant the whole array design rests on
 * (spec 2(c)).
 *
 * Special members are out of line only because TUniquePtr<FVaCuusModelLayout> needs the
 * complete type to destroy and this struct is declared first.
 */
struct FVaCuusModelArrayDesc
{
	VACUUS_API FVaCuusModelArrayDesc();
	VACUUS_API FVaCuusModelArrayDesc(FVaCuusModelArrayDesc&&);
	VACUUS_API FVaCuusModelArrayDesc& operator=(FVaCuusModelArrayDesc&&);
	VACUUS_API ~FVaCuusModelArrayDesc();

	/** The array property itself; safe to hold raw for the reason FVaCuusModelField::Property is. */
	const FArrayProperty* ArrayProperty = nullptr;

	/**
	 * ArrayProperty->Inner. Its Offset_Internal is 0 -- an inner's owner is the
	 * FArrayProperty, an FField and not a UObject, so SetupOffset aligns from zero
	 * (Property.cpp:1269-1288) -- and the element stride is exactly GetElementSize():
	 * GetRawPtr computes GetData() + Index * ElementSize with the size captured from the
	 * inner (UnrealType.h:4285-4286, :4332), tail padding already baked in for struct
	 * elements (PropertyStruct.cpp:114). So a value pointer for element i IS a container
	 * pointer for nothing: it feeds the per-kind value accessors directly.
	 */
	const FProperty* Inner = nullptr;

	/**
	 * The element's scalar kind. MEANINGLESS FOR STRUCT ELEMENTS -- a struct element has a
	 * layout, not a kind; test IsStructElement() first. Never Text and never Array here:
	 * both are refused at desc build (see BuildLevel), so element compares and describes
	 * never meet either.
	 */
	EVaCuusFieldKind ElementKind = EVaCuusFieldKind::Bool;

	/**
	 * Struct elements only. A PLAIN FVaCuusModelLayout over the element UScriptStruct --
	 * same flattening, same classifier, same name rules, same TStrongObjectPtr pinning a
	 * model root gets. Deliberately NOT a special element mode: the definition registry
	 * keys on the raw UScriptStruct*, so a type used both as a model root and as a row type
	 * gets one layout policy or none (spec 3.1). What a shared layout cannot refuse -- Text
	 * anywhere in the element subtree, nested containers -- the desc build refuses on the
	 * ARRAY FIELD instead. Built through the constructor that shares the caller's cycle
	 * stack, which changes nothing for an acyclic element type; a type already on the stack
	 * never gets an element layout at all, because the array field is refused first
	 * (BuildLevel's cycle guard).
	 */
	TUniquePtr<FVaCuusModelLayout> ElementLayout;

	bool IsStructElement() const { return ElementLayout.IsValid(); }

	/**
	 * Copies a whole array value: Dest and Src are VALUE pointers (the TArray's own
	 * address) into two instances of the containing type, in any pairing the pipeline
	 * needs. The one array copy primitive -- every stage reaches it through
	 * FVaCuusModelField::CopyValue, which is what keeps the pipeline's call sites
	 * kind-agnostic and the cost story below true at all of them.
	 *
	 * Resize to the source Num -- touching only the delta, surviving elements keep their
	 * values -- then per-element assignment, or one Memcpy for POD inners. Deliberately
	 * NOT the engine's own whole-array copy, which destroys every destination element
	 * before rebuilding and so can never reuse an element's buffer; the .cpp carries the
	 * full argument with the engine citations. Net: GROW-ONLY reuse -- allocations where
	 * content outgrew capacity or Num grew -- while a SHRINK may still reallocate the
	 * container block: the shrink path is RemoveValues -> FScriptArray::Remove with
	 * shrinking allowed (UnrealType.h:4477-4483, ScriptArray.h:191-222), and the allocator
	 * reallocates whenever its shrink policy says the slack is too big; the .cpp cites the
	 * exact thresholds (spec 3.3, scoped by 3.4).
	 */
	VACUUS_API void SyncCopy(void* DestValuePtr, const void* SrcValuePtr) const;
};

/** One bound leaf. Nested structs contribute their leaves and no entry of their own. */
struct FVaCuusModelField
{
	/**
	 * The resolved leaf property. NEVER an FStructProperty -- nesting is flattened at
	 * build time (spec 3.2). That is what gives every leaf its own dirty bit and its own
	 * per-kind comparison rule; see FVaCuusModelLayout below for what the flattening does
	 * and does not buy against FStructProperty::Identical.
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
	 * Index into the owning layout's array-desc table for an Array field; INDEX_NONE for
	 * every other kind. The BUILD-TIME identity: descs are appended while the table can
	 * still reallocate, so during the build only the index is stable.
	 */
	int32 ArrayDescIndex = INDEX_NONE;

	/**
	 * The desc itself for an Array field; null for every other kind. Fixed up ONCE by the
	 * layout constructor, after BuildLevel has stopped appending -- the pointer targets an
	 * element of the layout's desc table, and the layout is immutable from the
	 * constructor's return on, so the table can never reallocate under it (moving the
	 * layout moves the table's allocation ownership, not its elements). Exists so that
	 * CopyValue below reaches the array copy without any caller carrying the layout: the
	 * pipeline stages copy through the field alone.
	 */
	const FVaCuusModelArrayDesc* ArrayDesc = nullptr;

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
	 * AN ARRAY FIELD DOES NOT TAKE THIS PATH: it funnels to ArrayDesc->SyncCopy, through the
	 * pointer the layout fixed up at build time, so the pipeline's call sites stay
	 * kind-agnostic while the array primitive stays swappable in one place. (CopySingleValue
	 * on an FArrayProperty would be CORRECT -- it is the virtual whole-container deep copy --
	 * but it empties the destination first, destroying every non-POD element,
	 * PropertyArray.cpp:1260-1328; what to do about that is SyncCopy's concern, not this
	 * function's.)
	 *
	 * Out of line, unlike ContainerPtr above, only because calling into FProperty needs the
	 * complete type and this is a Public header that today gets away with a forward
	 * declaration.
	 */
	VACUUS_API void CopyValue(void* DestStructBase, const void* SourceStructBase) const;

	/**
	 * This field's value in StructBase, as text, for `vacuus.DumpModel` and nothing else.
	 *
	 * WHAT IT IS FOR: spec 8 says the dump prints both shadows, and a shadow is a
	 * UScriptStruct instance whose bytes mean nothing without the per-kind accessor. This is
	 * that accessor, once, for every kind -- exhaustive switch with no `default`, so a new
	 * EVaCuusFieldKind is a compile error here rather than a field the dump silently omits.
	 *
	 * NOT AN RmlUi CALL, AND THAT IS THE REASON IT EXISTS AT ALL RATHER THAN REUSING
	 * FVaCuusScalarDefinition::Get(). The game-side shadow is game-thread state, so the dump's
	 * game half runs on the game thread -- and building an Rml::Variant there would weaken the
	 * one invariant this whole design rests on (every RmlUi call on the UI thread). One UE-only
	 * function serves both halves.
	 *
	 * IT AGREES WITH WHAT THE DOCUMENT SHIPS FOR EVERY KIND EXCEPT FLOATS, deliberately.
	 * RmlUi renders a bound double through TypeConverter<double, String>, which is
	 * `FormatString(dest, "%.3f", src)` followed by TrimTrailingDotZeros
	 * (ThirdParty/RmlUi/Include/RmlUi/Core/TypeConverter.inl:282-295), i.e. three decimals.
	 * A dump that rounded the same way would hide the difference between two values the
	 * document renders identically -- which is exactly the disagreement somebody dumping two
	 * shadows is looking for. Bools follow RmlUi ("1"/"0", :340-347) because there the shipped
	 * form loses nothing.
	 *
	 * ARRAYS PRINT Num() AND THE FIRST 8 ELEMENTS with an elision marker (spec 6): a
	 * 200-row killfeed dumped in full would bury the scalar fields the dump exists to show.
	 * Struct elements print through the element layout's own fields, scalar elements
	 * through the same per-kind accessors in value-pointer form.
	 */
	VACUUS_API FString DescribeValue(const void* StructBase) const;
};

/**
 * The flat, pre-resolved description of one model type: built ONCE per UScriptStruct,
 * one entry per bound leaf, nested structs flattened, no TFieldIterator afterwards.
 *
 * WHY FLAT AND PRE-RESOLVED. This is deliberately the shape UE's own per-frame differ
 * uses. FRepLayout keeps a flat command list with a typed comparator per entry and
 * falls back to the virtual Identical only for kinds it did not special-case
 * (RepLayout.cpp:668-753).
 *
 * THE PRIMARY REASON IS RESOLUTION, NOT COST, AND THIS IS A CORRECTION TO WHAT THIS
 * COMMENT USED TO CLAIM. A nested struct compared as one unit has ONE answer, so it
 * would get one dirty bit for the whole of `Origin` -- and the per-kind rules that make
 * this milestone correct (case-sensitive strings and names, display-string FText, the
 * mask-aware bitfield read) live per LEAF and would never run at all. Flattening is what
 * makes both possible.
 *
 * COST IS THE SECOND REASON, AND IT IS NARROWER THAN IT LOOKS. FStructProperty::Identical
 * is three lines forwarding to UScriptStruct::CompareScriptStruct (PropertyStruct.cpp:
 * 139-142), and CompareScriptStruct SHORT-CIRCUITS through ICppStructOps::Identical when
 * the type has STRUCT_IdenticalNative (Class.cpp:3672-3681) -- which every struct a HUD
 * actually nests has: FVector, FVector2D and FQuat all declare WithIdenticalViaEquality
 * or WithIdentical (Property.cpp:53-73, :146-161, :338-353), and Class.cpp:3263-3267 sets
 * the flag from that. So the fresh TFieldIterator at Class.cpp:3683-3692 is reached only
 * by structs with no native comparison, e.g. a designer's USTRUCT. Real, but not the
 * dominant cost this comment used to assert for the common case.
 *
 * The container kinds are a different matter and the cost argument does hold there:
 * FMapProperty::Identical (PropertyMap.cpp:233) is O(n^2) via IsPermutation
 * (PropertyMap.cpp:85-148). Containers are M3b.
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
	 * How many struct properties deep the flattening will go. Not a cycle guard, and with
	 * arrays in the type graph one is needed ELSEWHERE: by-value nesting still cannot cycle
	 * -- a USTRUCT containing itself by value would need infinite size -- but a TArray
	 * member is heap indirection, so a TYPE GRAPH can loop through one, and this limit
	 * would never fire on the way down because every element layout restarts Depth at 0.
	 * UHT happens to refuse every native writing of the shape -- the direct one explicitly
	 * (UhtArrayProperty.cs:216-222), the indirect ones at the forward reference they need
	 * (zero code-generation hash, UhtProperty.cs:3066-3071) -- but nothing validates the
	 * FProperty graph itself, which a runtime-built UUserDefinedStruct assembles freely.
	 * That cycle is refused by the build stack at BuildLevel's array interception. What
	 * this constant bounds is the wire names -- every level adds a dotted segment that a
	 * document author has to type.
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
	/**
	 * The element-layout form: the same build, threaded through the CALLER'S cycle stack.
	 * A container cycle is visible only ACROSS layouts -- each element layout is a fresh
	 * FVaCuusModelLayout whose Depth restarts at 0, so no per-layout state can see the
	 * recursion; the stack of in-progress build roots is the one thing that spans them.
	 * Private because the stack only means something mid-build: the sole caller is
	 * BuildLevel's array interception, which checks the stack BEFORE constructing.
	 */
	FVaCuusModelLayout(const UScriptStruct* InStruct, TArray<const UScriptStruct*>& BuildStack);

	/** The one build body behind both constructors; keeps InStruct on the stack for its duration. */
	void Build(const UScriptStruct* InStruct, TArray<const UScriptStruct*>& BuildStack);

	/** Walks one struct level, appending leaves and recursing into nested structs. */
	void BuildLevel(const UScriptStruct* InStruct, const FString& Prefix, int32 BaseOffset, int32 TopLevelNameIndex, int32 Depth,
		TArray<const UScriptStruct*>& BuildStack);

	TStrongObjectPtr<const UScriptStruct> Struct;
	TArray<FVaCuusModelField> Fields;
	TArray<FString> TopLevelNames;

	/**
	 * One entry per Array field, indexed by FVaCuusModelField::ArrayDescIndex. Appended
	 * only inside BuildLevel; the constructor fixes each Array field's ArrayDesc pointer
	 * into this table AFTER the build, and nothing may append past that point -- the
	 * pointers dangle otherwise. (TUniquePtr inside the desc also makes the layout
	 * move-only, which is what stops a copied layout carrying pointers into another
	 * layout's table.)
	 */
	TArray<FVaCuusModelArrayDesc> ArrayDescs;
};

/**
 * RmlUi's variable-name rule, restated in UE terms because the layout must refuse a
 * name BEFORE RmlUi sees it (spec 3.3). RmlUi's own refusal is a Log::LT_WARNING and a
 * `false` from BindVariable (DataModel.cpp:119-124), after which the variable is simply
 * absent and every reference to it fails address resolution.
 *
 * THAT WARNING IS NOT INVISIBLE -- Log::Message reaches LogVaCuus through
 * FVaCuusSystemInterface::LogMessage, which is where the whole compiled-out question is
 * settled. What it cannot do is name the UE side: it prints the WIRE name RmlUi was
 * handed, so a designer reading `[Rml] Could not bind data variable '2Hearts'` has no
 * path back to the UPROPERTY, and the second symptom -- one
 * `Could not get value from data variable` per reference per evaluation
 * (DataModel.cpp:320-322) -- reads like an unrelated document bug. Refusing here instead
 * costs one line that names the model, the property AND the rule, and the field then does
 * not exist anywhere downstream.
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
