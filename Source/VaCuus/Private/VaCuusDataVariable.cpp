// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusDataVariable.h"

#include "VaCuusDefines.h"
#include "VaCuusModelShadow.h"
#include "VaCuusUIThread.h"
#include "VaCuusWriteRouter.h"

#include "UObject/AnsiStrProperty.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/StrProperty.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include "UObject/Utf8StrProperty.h"

#include <RmlUi/Core/DataModelHandle.h>

#include <atomic>

namespace
{
/**
 * Every model type's definitions, and the count of writes refused.
 *
 * UI-THREAD-ONLY, PLAIN (NON-ATOMIC) STATICS, and asserted at every entry point that
 * reaches them -- the same shape and the same justification as the cursor and caret
 * latches in VaCuusSystemInterface.cpp: written and read only from the one thread allowed
 * to call into RmlUi at all.
 */
TMap<const UScriptStruct*, TUniquePtr<FVaCuusModelDefinitions>> GDefinitionsByStruct;
int32 GNumRefusedSets = 0;

/**
 * Stale definition sets evicted and rebuilt after a struct recompile (VaCuus-akj.16).
 * ATOMIC, unlike its neighbours, on the write-router counters' pattern (GNumRoutedWrites):
 * written on the UI thread inside GetOrCreate's evict branch, read by tests from the game
 * thread -- and it exists BECAUSE of the invariant-needs-an-observable rule: without it,
 * "the rebuild happened instead of the corpse being handed out" is only a Log line the
 * automation framework cannot count (its expected-message filter sees Warning/Error only).
 */
std::atomic<uint64> GNumStaleEvictions{0};

/**
 * The evaluation counters (spec 3.5), same UI-thread-only plain-static rule as above:
 * incremented only inside definition virtuals, which run only under Context::Update() on
 * the UI thread, and read only through accessors that assert that thread.
 */
int32 GNumScalarGets = 0;
int32 GNumArraySizes = 0;
int32 GNumArrayChilds = 0;

/** UTF-8, because that is what RmlUi's String holds (Config.h:108 aliases it to std::string). */
Rml::String ToRmlString(const FString& Value)
{
	return Rml::String(TCHAR_TO_UTF8(*Value));
}
}	 // namespace

// ---------------------------------------------------------------------------------------
// FVaCuusScalarDefinition
// ---------------------------------------------------------------------------------------

FVaCuusScalarDefinition::FVaCuusScalarDefinition(const FProperty* InProperty, EVaCuusFieldKind InKind, FString InDiagnosticPath)
	: Rml::VariableDefinition(Rml::DataVariableType::Scalar)
	, Property(InProperty)
	, Kind(InKind)
	, DiagnosticPath(MoveTemp(InDiagnosticPath))
{
	check(InProperty != nullptr);
}

bool FVaCuusScalarDefinition::Get(void* InValuePtr, Rml::Variant& OutVariant)
{
	// Counted before the guard: the idle gate asserts "no evaluation REACHED a definition",
	// and a null-pointer arrival is still an arrival.
	++GNumScalarGets;

	// GUARDED EVEN THOUGH THE ONLY CALLER GUARDS FIRST. BasePointerDefinition::Get null-checks
	// its own pointer before dereferencing (DataVariable.cpp:138-142), so this is unreachable
	// today -- but RmlUi's own ScalarDefinition<T>::Get has no such check
	// (DataVariable.h:74-78), and that missing check is a real crash in the library
	// (research 8.3). Ours costs one predictable branch on a path that already does a virtual
	// call.
	if (InValuePtr == nullptr)
	{
		return false;
	}

	// NO `default` CASE, ON PURPOSE: -Wswitch turns a new EVaCuusFieldKind into a compile
	// error right here rather than a silently unbound variable. That is the whole reason this
	// is one class with a switch instead of eleven classes with a factory.
	switch (Kind)
	{
		case EVaCuusFieldKind::Bool:
			// Through the MASK-AWARE accessor, never a byte read: for `uint8 b : 1` the value
			// pointer addresses the storage integer that up to seven unrelated bitfields share,
			// and FBoolProperty::GetPropertyValue is `!!(*ByteValue & FieldMask)`
			// (UnrealType.h:2682-2687) with FieldMask 0xFF for a native bool and the isolated
			// bit otherwise (:2634).
			OutVariant = CastFieldChecked<FBoolProperty>(Property)->GetPropertyValue(InValuePtr);
			return true;

		case EVaCuusFieldKind::SignedInt:
			// int64_t, not UE's int64: they are distinct types on Linux (long vs long long) and
			// Variant's Set overload set contains int, int64_t, unsigned int and uint64_t
			// (Variant.h:103-106), so an unconverted `long long` is an ambiguous conversion, not
			// a narrowing one.
			OutVariant = static_cast<int64_t>(CastFieldChecked<FNumericProperty>(Property)->GetSignedIntPropertyValue(InValuePtr));
			return true;

		case EVaCuusFieldKind::UnsignedInt:
			OutVariant = static_cast<uint64_t>(CastFieldChecked<FNumericProperty>(Property)->GetUnsignedIntPropertyValue(InValuePtr));
			return true;

		case EVaCuusFieldKind::FloatingPoint:
			// One accessor for float and double alike, and DOUBLE is also what the expression
			// interpreter works in -- every arithmetic and relational operator does
			// `L.Get<double>() OP R.Get<double>()` (DataExpression.cpp:1007-1016), so storing a
			// float would only add a conversion.
			OutVariant = CastFieldChecked<FNumericProperty>(Property)->GetFloatingPointPropertyValue(InValuePtr);
			return true;

		case EVaCuusFieldKind::String:
			OutVariant = ToRmlString(CastFieldChecked<FStrProperty>(Property)->GetPropertyValue(InValuePtr));
			return true;

		case EVaCuusFieldKind::Utf8String:
		{
			// ALREADY UTF-8: copied byte for byte rather than round-tripped through FString,
			// which would decode and re-encode every character for nothing.
			const FUtf8String& Value = CastFieldChecked<FUtf8StrProperty>(Property)->GetPropertyValue(InValuePtr);
			OutVariant = Rml::String(reinterpret_cast<const char*>(*Value), static_cast<size_t>(Value.Len()));
			return true;
		}

		case EVaCuusFieldKind::AnsiString:
		{
			// ROUND-TRIPPED THROUGH FString, unlike the UTF-8 case above, and that is
			// correctness rather than caution: FAnsiString holds 8-bit characters, so every
			// byte above 0x7F is a valid FAnsiString character and an INVALID UTF-8 sequence.
			// Copying the bytes straight into an Rml::String would hand RmlUi's UTF-8 decoder
			// a malformed string.
			const FAnsiString& Value = CastFieldChecked<FAnsiStrProperty>(Property)->GetPropertyValue(InValuePtr);
			OutVariant = ToRmlString(FString(*Value));
			return true;
		}

		case EVaCuusFieldKind::Name:
			OutVariant = ToRmlString(CastFieldChecked<FNameProperty>(Property)->GetPropertyValue(InValuePtr).ToString());
			return true;

		case EVaCuusFieldKind::Text:
			// THE DISPLAY STRING (spec 5), which is also the only thing a document can render.
			OutVariant = ToRmlString(CastFieldChecked<FTextProperty>(Property)->GetPropertyValue(InValuePtr).ToString());
			return true;

		case EVaCuusFieldKind::Enum:
			return GetEnumName(InValuePtr, OutVariant);

		case EVaCuusFieldKind::ObjectPath:
			return GetObjectPath(InValuePtr, OutVariant);

		case EVaCuusFieldKind::Array:
			// Unreachable by shape: pass 1 never builds a scalar definition FOR an Array
			// field (its underlying is FVaCuusArrayDefinition), and an ELEMENT's kind is
			// never Array either -- the desc-build scan refuses nested containers
			// (VaCuusModelLayout.cpp, BuildLevel's array interception).
			checkNoEntry();
			return false;
	}

	checkNoEntry();
	return false;
}

bool FVaCuusScalarDefinition::GetEnumName(const void* InValuePtr, Rml::Variant& OutVariant)
{
	// TWO SHAPES, one kind: FEnumProperty wraps an underlying integer property, while a
	// TEnumAsByte is an FByteProperty that merely CARRIES a UEnum (UnrealType.h:2195, 2253).
	// Only the first has GetUnderlyingProperty().
	const UEnum* Enum = nullptr;
	int64 Value = 0;

	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		Enum = EnumProperty->GetEnum();
		Value = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(InValuePtr);
	}
	else
	{
		const FNumericProperty* Numeric = CastFieldChecked<FNumericProperty>(Property);
		Enum = Numeric->GetIntPropertyEnum();
		Value = static_cast<int64>(Numeric->GetUnsignedIntPropertyValue(InValuePtr));
	}

	// FindAuthoredNameStringByValue rather than GetAuthoredNameStringByValue, because the
	// latter feeds an INDEX_NONE from GetIndexByValue straight into GetNameStringByIndex and
	// hands back an empty string (Enum.cpp:933-937) -- an out-of-range enum would render as
	// nothing at all, with no diagnostic anywhere. The Find form reports the miss
	// (Enum.cpp:939-949).
	//
	// It is the NAME and not the value: exposing both would need two legal wire names per
	// property and collide with the name-legality rule for no clear gain (spec 3.4), and the
	// number stays reachable by binding the underlying integer property.
	FString Name;
	if (Enum == nullptr || !Enum->FindAuthoredNameStringByValue(Name, Value))
	{
		if (!bUnreadableLogged)
		{
			bUnreadableLogged = true;
			UE_LOG(LogVaCuus, Warning, TEXT("VaCuus model: '%s' holds %lld, which is not a value of its enum; it will read as empty"),
				*DiagnosticPath, Value);
		}
		return false;
	}

	OutVariant = ToRmlString(Name);
	return true;
}

bool FVaCuusScalarDefinition::GetObjectPath(const void* InValuePtr, Rml::Variant& OutVariant)
{
	// SOFT REFERENCES ONLY, AND THE RESTRICTION IS THREADING, NOT TASTE.
	//
	// An FSoftObjectPtr already IS a path: ToString() is GetUniqueID().ToString()
	// (SoftObjectPtr.h:96-105), i.e. two FNames and a subpath string, with no resolution, no
	// GUObjectArray read and nothing the collector can move. Safe to read on the UI thread.
	//
	// An FWeakObjectPtr is an index/serial pair with no path in it, so producing one means
	// resolving the object -- and WeakObjectPtr.h:295-296 states outright that a weak
	// pointer cannot be tested from another thread, because the mark phase of GC makes it
	// read as dead. Resolving would then walk the object's Outer chain for GetPathName()
	// while the purge phase may be destroying it. That is exactly the use-after-free the
	// shadow buffer exists to prevent, so FVaCuusModelLayout refuses FWeakObjectProperty and
	// this branch is the belt to that braces.
	if (const FSoftObjectProperty* SoftProperty = CastField<FSoftObjectProperty>(Property))
	{
		OutVariant = ToRmlString(SoftProperty->GetPropertyValue(InValuePtr).ToString());
		return true;
	}

	if (!bUnreadableLogged)
	{
		bUnreadableLogged = true;
		UE_LOG(LogVaCuus, Error,
			TEXT("VaCuus model: '%s' is an object reference that cannot be read off the game thread; it will read as empty"),
			*DiagnosticPath);
	}
	return false;
}

bool FVaCuusScalarDefinition::Set(void* InValuePtr, const Rml::Variant& Variant)
{
	// THE WRITE ROUTER'S SEAM (M4 Task 9, spec 3.10). With the router registered -- the
	// UI thread registers it in Init() -- a write whose value pointer lands in a
	// PRODUCTION-BOUND model's storage is attributed, marshalled to the game thread and
	// revert-dirtied by FVaCuusWriteRouter (its class comment carries the design), and
	// the refusal below is replaced by the router's Verbose "routed" line: it is the
	// legal channel now. Still `return false` -- the shadow is never written, both RmlUi
	// call sites still skip their DirtyVariable, I3 stands. With NO router registered
	// (an M3 configuration), this branch is one predicted-false compare; with the router
	// up but the write unattributable (every M3a/M3b fixture binds its shadow directly,
	// bypassing the registry), TryRouteScalarSet declines and the refusal runs verbatim
	// -- which is what keeps those suites' expected messages true in this binary. The
	// Get lambda is the echo rule's comparison source (the router's class comment);
	// it runs only for ATTRIBUTED writes, so the counted evaluation it costs never
	// lands on an idle or M3-shaped path.
	if (FVaCuusWriteRouter::IsRouterRegistered()
		&& FVaCuusWriteRouter::TryRouteScalarSet(DiagnosticPath, InValuePtr, Variant,
			[this, InValuePtr](Rml::Variant& OutCurrent) { return Get(InValuePtr, OutCurrent); }))
	{
		return false;
	}

	// THE REFUSAL. Spec 4/I3 -- the header's GetNumRefusedSets() comment carries the whole
	// argument. In one line: a write here would land in the shadow with no game-thread
	// participation, the differ would then compare the live struct against its own shadow,
	// see no change and set no bit, and the two shadows would diverge permanently.
	//
	// THIS IS THE ONLY REFUSAL SITE, and that is by shape rather than by discipline: the
	// property definition's Set forwards to its underlying definition
	// (DataVariable.cpp:145-150), so every write to a leaf arrives here; and a write aimed at
	// a struct level never gets this far, because VariableDefinition::Set's base
	// implementation already logs and returns false (DataVariable.cpp:35-39).
	++GNumRefusedSets;

	if (!bRefusalLogged)
	{
		bRefusalLogged = true;
		UE_LOG(LogVaCuus, Warning,
			TEXT("VaCuus model: refused a document write to '%s'. M3 binding is one-way -- writing here would leave the UI's "
				 "shadow and the game's shadow permanently disagreeing, with nothing on screen to show it. Reported once per "
				 "field"),
			*DiagnosticPath);
	}

	return false;
}

int FVaCuusScalarDefinition::Size(void* /*InValuePtr*/)
{
	// `data-for` over a leaf, which DataViewFor::Update reaches with no Type() check at all
	// (DataViewDefault.cpp:498-503). The behaviour is RmlUi's own base contract -- 0, so the
	// loop iterates no rows (DataVariable.cpp:40-44) -- but the base warning ("Tried to get
	// the size from a non-array data type.") names nothing a UE-side author can find.
	// LATCHED for the usual reason: a data-for target is re-resolved every time its root is
	// dirtied, and the document is exactly as wrong on the ten-thousandth evaluation.
	if (!bSizeMissLogged)
	{
		bSizeMissLogged = true;
		UE_LOG(LogVaCuus, Warning,
			TEXT("VaCuus model: data-for over '%s', which is not an array; it yields no rows. Reported once per field"),
			*DiagnosticPath);
	}

	return 0;
}

// ---------------------------------------------------------------------------------------
// FVaCuusPropertyDefinition
// ---------------------------------------------------------------------------------------

FVaCuusPropertyDefinition::FVaCuusPropertyDefinition(const FProperty* InProperty, Rml::VariableDefinition* InUnderlying)
	: Rml::BasePointerDefinition(InUnderlying)
	, Property(InProperty)
{
	check(InProperty != nullptr);
	check(InUnderlying != nullptr);
}

void* FVaCuusPropertyDefinition::DereferencePointer(void* InContainerPtr)
{
	// The one call. ContainerPtrToValuePtr is `(uint8*)Container + Offset_Internal`
	// (UnrealType.h:733-745) -- the same arithmetic in the shadow buffer as in the live
	// struct, precisely because the shadow is a real UScriptStruct instance (spec 3.1).
	return Property->ContainerPtrToValuePtr<void>(InContainerPtr);
}

// ---------------------------------------------------------------------------------------
// FVaCuusStructDefinition
// ---------------------------------------------------------------------------------------

FVaCuusStructDefinition::FVaCuusStructDefinition(FString InDiagnosticPath)
	: Rml::VariableDefinition(Rml::DataVariableType::Struct)
	, DiagnosticPath(MoveTemp(InDiagnosticPath))
{
}

void FVaCuusStructDefinition::AddLeaf(const FString& Segment, FVaCuusPropertyDefinition* Definition, int32 ContainerOffsetFromTypeBase)
{
	check(Definition != nullptr);

	FMember& Member = Members.AddDefaulted_GetRef();
	Member.Segment = ToRmlString(Segment);
	Member.Definition = Definition;
	Member.ContainerOffsetFromTypeBase = ContainerOffsetFromTypeBase;
	Member.bNested = false;
}

void FVaCuusStructDefinition::AddNested(const FString& Segment, FVaCuusStructDefinition* Definition)
{
	check(Definition != nullptr);

	FMember& Member = Members.AddDefaulted_GetRef();
	Member.Segment = ToRmlString(Segment);
	Member.Definition = Definition;

	// ZERO, AND THERE IS NO PARAMETER TO GET IT WRONG WITH. A nested struct definition is
	// handed the base of the definition set's own type unchanged; the offset is applied
	// exactly once, when the leaf below it is handed out. See the file comment's invariant.
	Member.ContainerOffsetFromTypeBase = 0;
	Member.bNested = true;
}

const FVaCuusStructDefinition::FMember* FVaCuusStructDefinition::Find(const Rml::String& Segment) const
{
	for (const FMember& Member : Members)
	{
		if (Member.Segment == Segment)
		{
			return &Member;
		}
	}

	return nullptr;
}

Rml::DataVariable FVaCuusStructDefinition::Child(void* InBase, const Rml::DataAddressEntry& Address)
{
	// AN INDEXED ENTRY ON A STRUCT. DataAddressEntry(int) leaves `name` empty
	// (DataTypes.h:36-41), so `{{Origin[0]}}` arrives here with index >= 0 and no name.
	//
	// LATCHED, like every other diagnostic on this path: a document does not stop being wrong,
	// so an unthrottled line here is one log write per re-evaluation per frame, forever.
	if (Address.index >= 0)
	{
		if (!bIndexedMissLogged)
		{
			bIndexedMissLogged = true;
			UE_LOG(LogVaCuus, Warning,
				TEXT("VaCuus model: '%s' is a struct and was indexed as an array ([%d]); a struct member is reached by name, "
					 "never by index. Reported once per struct"),
				*DiagnosticPath, Address.index);
		}

		return Rml::DataVariable();
	}

	if (const FMember* Member = Find(Address.name))
	{
		return Rml::DataVariable(Member->Definition, static_cast<uint8*>(InBase) + Member->ContainerOffsetFromTypeBase);
	}

	// A MISS IS A DIAGNOSTIC, NEVER A NULL DEREFERENCE -- and the diagnostic has to be ours.
	// RmlUi's own StructDefinition::Child logs here (DataVariable.cpp:88), but the two things
	// a caller might rely on to notice are both absent: DataVariable::Get/Set/Size/Child do
	// not null-check `definition` at all (DataVariable.cpp:5-28), and every RMLUI_ASSERT in
	// the library is compiled out of every configuration this plugin builds (spec 8).
	//
	// `.size` lands here too, and deliberately: RmlUi handles `address.name == "size"` INSIDE
	// ArrayDefinition::Child (DataVariable.h:151-152), not in the core, so a struct has no
	// such member and `{{Origin.size}}` is a genuine mistake rather than a missing feature.
	// Returning an empty DataVariable is safe -- every caller in Core tests
	// `explicit operator bool` first (DataModel.cpp:285-290, :319; DataControllerDefault.cpp:57;
	// DataExpression.cpp:1188).
	//
	// LATCHED, AND THE STRING BUILD IS INSIDE THE LATCH. This is not a once-at-startup path: a
	// missing member is re-resolved every time the expression re-evaluates, i.e. every time its
	// ROOT variable is dirtied. `{{Target.Desgination}}` against a Target the game writes each
	// frame is therefore one warning AND one member-list rebuild per UI frame, at UI frame rate,
	// for the life of the process -- on a path spec 9 budgets in microseconds. The list is why
	// the build has to be INSIDE rather than merely the log line: it is O(members) FString
	// appends, so latching only the UE_LOG would leave every allocation behind.
	if (!bMemberMissLogged)
	{
		bMemberMissLogged = true;

		FString Available;
		for (const FMember& Member : Members)
		{
			Available += (Available.IsEmpty() ? TEXT("") : TEXT(", "));
			Available += UTF8_TO_TCHAR(Member.Segment.c_str());
		}

		UE_LOG(LogVaCuus, Warning, TEXT("VaCuus model: '%s' has no member '%s'. It has: %s. Reported once per struct"),
			*DiagnosticPath, UTF8_TO_TCHAR(Address.name.c_str()), Available.IsEmpty() ? TEXT("(nothing)") : *Available);
	}

	return Rml::DataVariable();
}

Rml::StringList FVaCuusStructDefinition::ReflectMemberNames()
{
	Rml::StringList Names;
	Names.reserve(Members.Num());
	for (const FMember& Member : Members)
	{
		Names.push_back(Member.Segment);
	}

	return Names;
}

int FVaCuusStructDefinition::Size(void* /*InBase*/)
{
	// `data-for` over a struct level -- the scalar definition's Size() carries the whole
	// argument (base contract at DataVariable.cpp:40-44, unchecked caller at
	// DataViewDefault.cpp:498-503); this override only changes whose name is in the line.
	if (!bSizeMissLogged)
	{
		bSizeMissLogged = true;
		UE_LOG(LogVaCuus, Warning,
			TEXT("VaCuus model: data-for over '%s', which is a struct, not an array; it yields no rows. Reported once per struct"),
			*DiagnosticPath);
	}

	return 0;
}

const FVaCuusStructDefinition* FVaCuusStructDefinition::FindNested(const FString& Segment) const
{
	const FMember* Member = Find(ToRmlString(Segment));
	return (Member != nullptr && Member->bNested) ? static_cast<const FVaCuusStructDefinition*>(Member->Definition) : nullptr;
}

// ---------------------------------------------------------------------------------------
// FVaCuusArrayDefinition
// ---------------------------------------------------------------------------------------

FVaCuusArrayDefinition::FVaCuusArrayDefinition(
	const FArrayProperty* InArrayProperty, Rml::VariableDefinition* InElementDefinition, FString InDiagnosticPath)
	: Rml::VariableDefinition(Rml::DataVariableType::Array)
	, ArrayProperty(InArrayProperty)
	, ElementDefinition(InElementDefinition)
	, DiagnosticPath(MoveTemp(InDiagnosticPath))
{
	// DataVariableType::Array is decoration today -- nothing compiled keys on Type() ==
	// Array, DataViewFor calls Size() with no type check -- but the Debugger relays switch
	// on it and may be compiled in later, and it is one token (m3b-rmlui-arrays.md 2).
	check(InArrayProperty != nullptr);
	check(InElementDefinition != nullptr);
}

int FVaCuusArrayDefinition::Size(void* InValuePtr)
{
	// Counted before the guard, like the scalar Get: an arrival is an evaluation.
	++GNumArraySizes;

	// Same guard, same reason as FVaCuusScalarDefinition::Get -- BasePointerDefinition::Size
	// already null-checks (DataVariable.cpp:152-156), so this is today-unreachable belt.
	if (InValuePtr == nullptr)
	{
		return 0;
	}

	// COMPUTED FROM THE INCOMING POINTER AT CALL TIME, NEVER CACHED -- the class comment's
	// invariant. The helper takes the VALUE pointer, i.e. the TArray's own address, not the
	// containing struct (UnrealType.h:4285-4288), which is exactly what DereferencePointer
	// handed over.
	return FScriptArrayHelper(ArrayProperty, InValuePtr).Num();
}

Rml::DataVariable FVaCuusArrayDefinition::Child(void* InValuePtr, const Rml::DataAddressEntry& Address)
{
	++GNumArrayChilds;

	if (InValuePtr == nullptr)
	{
		return Rml::DataVariable();
	}

	FScriptArrayHelper Helper(ArrayProperty, InValuePtr);
	const int32 Num = Helper.Num();

	// NAME FIRST, BOUNDS SECOND -- deliberately NOT RmlUi's order. Its ArrayDefinition::Child
	// checks bounds first and matches "size" inside the out-of-bounds branch
	// (DataVariable.h:143-163, the match at :151-152), which works because a named entry can
	// never be in bounds -- the name constructor pins index to -1 (DataTypes.h:38-39) and
	// ParseAddress emits only {index >= 0} or {non-empty name, index == -1}, rejecting
	// negatives and empties outright (DataModel.cpp:19-20, :34-36). Same partition, so the
	// two orders are equivalent; this one exists so the two failure modes cannot share a
	// message -- RmlUi reuses its misleading "Data array index out of bounds." for a named
	// miss (DataVariable.h:154).
	if (Address.index < 0)
	{
		// THE "size" CASE IS THE ONE A HAND-ROLLED ARRAY DEFINITION BREAKS BY OMISSION:
		// RmlUi implements `{{Arr.size}}` inside ArrayDefinition::Child, not in the core
		// (DataVariable.h:151-152), so nothing else would answer it. MakeLiteralIntVariable
		// encodes the int in the DataVariable's ptr against a static definition
		// (DataVariable.cpp:57-72; declared RMLUICORE_API at DataVariable.h:67).
		if (Address.name == "size")
		{
			return Rml::MakeLiteralIntVariable(Num);
		}

		if (!bNamedMissLogged)
		{
			bNamedMissLogged = true;
			UE_LOG(LogVaCuus, Warning,
				TEXT("VaCuus model: '%s' is an array and has no child '%s'%s -- only an index and the literal name 'size' resolve "
					 "against it. Reported once per array"),
				*DiagnosticPath, UTF8_TO_TCHAR(Address.name.c_str()),
				Address.name.empty() ? TEXT(" (an EMPTY child name: ParseAddress cannot emit one, so this entry was hand-built)")
									 : TEXT(""));
		}

		return Rml::DataVariable();
	}

	if (Address.index >= Num)
	{
		if (!bOutOfBoundsLogged)
		{
			bOutOfBoundsLogged = true;
			UE_LOG(LogVaCuus, Warning,
				TEXT("VaCuus model: '%s' was indexed out of bounds ([%d] with %d elements). Reported once per array"),
				*DiagnosticPath, Address.index, Num);
		}

		return Rml::DataVariable();
	}

	// GetRawPtr is call-time arithmetic -- GetData() + Index * ElementSize
	// (UnrealType.h:4324-4333) -- and its result lives for exactly this expression: it rides
	// out inside the returned DataVariable, is consumed by the very next walk step or Get
	// (DataModel.cpp:285-290), and is stored by nothing (spec 2(c)).
	return Rml::DataVariable(ElementDefinition, Helper.GetRawPtr(Address.index));
}

// ---------------------------------------------------------------------------------------
// FVaCuusModelDefinitions
// ---------------------------------------------------------------------------------------

FVaCuusModelDefinitions::FVaCuusModelDefinitions(const FVaCuusModelLayout& Layout)
{
	Struct.Reset(Layout.GetStruct());

	const FString ModelName = Layout.GetStruct()->GetName();

	// PASS 1: one underlying definition -- scalar for every value kind, array for
	// Kind::Array -- and one property definition per LEAF, filed either as a root-level
	// variable or as a member of the struct definition for its dotted prefix.
	TMap<FString, FVaCuusPropertyDefinition*> RootLeaves;

	for (const FVaCuusModelField& Field : Layout.GetFields())
	{
		int32 LastDot = INDEX_NONE;
		Field.WireName.FindLastChar(TEXT('.'), LastDot);

		const FString Prefix = (LastDot == INDEX_NONE) ? FString() : Field.WireName.Left(LastDot + 1);
		const FString Segment = (LastDot == INDEX_NONE) ? Field.WireName : Field.WireName.Mid(LastDot + 1);

		const FString FieldPath = ModelName + TEXT(".") + Field.WireName;

		Rml::VariableDefinition* Underlying = nullptr;
		if (Field.Kind == EVaCuusFieldKind::Array)
		{
			// AN ARRAY LEAF DIFFERS ONLY IN WHAT THE PROPERTY DEFINITION WRAPS. Its element
			// definition, per spec 3.5:
			//
			//  - scalar elements reuse FVaCuusScalarDefinition UNMODIFIED: it already
			//    operates on value pointers -- BasePointerDefinition offsets before
			//    forwarding on the field path -- and GetRawPtr(i) IS a value pointer
			//    (spec 2(d)). The "[]" suffix keeps its diagnostics distinguishable from a
			//    same-named field's.
			//
			//  - struct elements borrow the ELEMENT TYPE's root struct definition, fetched
			//    through the registry keyed on the element UScriptStruct -- so two models
			//    sharing a row type share its definitions, which is exactly why the array
			//    definition must stay stateless (its class comment). The nested GetOrCreate
			//    is safe re-entrancy: the outer call holds no pointer into the map while
			//    this constructor runs and inserts only after it returns (GetOrCreate
			//    below); and the recursion cannot loop, because the desc build refused
			//    nested containers and container cycles before ever building the desc
			//    (VaCuusModelLayout.cpp, BuildLevel's array interception).
			const FVaCuusModelArrayDesc* Desc = Field.ArrayDesc;
			check(Desc != nullptr);

			Rml::VariableDefinition* ElementDefinition = nullptr;
			if (Desc->IsStructElement())
			{
				const FVaCuusModelDefinitions* ElementDefinitions = FVaCuusDefinitionRegistry::GetOrCreate(*Desc->ElementLayout);
				check(ElementDefinitions != nullptr);	 // element layouts are valid and non-empty by desc-build refusal
				ElementDefinition = ElementDefinitions->GetRootStruct();
			}
			else
			{
				ElementDefinition = ScalarDefinitions
										.Add_GetRef(MakeUnique<FVaCuusScalarDefinition>(
											Desc->Inner, Desc->ElementKind, FieldPath + TEXT("[]")))
										.Get();
			}

			Underlying =
				ArrayDefinitions.Add_GetRef(MakeUnique<FVaCuusArrayDefinition>(Desc->ArrayProperty, ElementDefinition, FieldPath))
					.Get();
		}
		else
		{
			Underlying = ScalarDefinitions.Add_GetRef(MakeUnique<FVaCuusScalarDefinition>(Field.Property, Field.Kind, FieldPath)).Get();
		}

		// The property definition reports its underlying's type -- BasePointerDefinition's
		// constructor copies it (DataVariable.cpp:134-136) -- so a wrapped array reads back
		// as DataVariableType::Array without being told.
		FVaCuusPropertyDefinition* PropertyDefinition =
			PropertyDefinitions.Add_GetRef(MakeUnique<FVaCuusPropertyDefinition>(Field.Property, Underlying)).Get();

		if (Prefix.IsEmpty())
		{
			RootLeaves.Add(Field.WireName, PropertyDefinition);
		}
		else
		{
			GetOrCreateStructFor(Prefix)->AddLeaf(Segment, PropertyDefinition, Field.ContainerOffset);
		}
	}

	// PASS 2: one bindable variable per TOP-LEVEL NAME, in the layout's order. A nested
	// struct contributes ONE variable however many leaves hang off it, because that is the
	// only granularity DirtyVariable accepts.
	TopLevelVariables.Reserve(Layout.GetTopLevelNames().Num());
	for (const FString& Name : Layout.GetTopLevelNames())
	{
		if (FVaCuusPropertyDefinition** Leaf = RootLeaves.Find(Name))
		{
			TopLevelVariables.Add({Name, *Leaf});
			continue;
		}

		if (FVaCuusStructDefinition** Nested = StructsByPrefix.Find(Name + TEXT(".")))
		{
			TopLevelVariables.Add({Name, *Nested});
			continue;
		}

		// Unreachable through FVaCuusModelLayout for every kind: scalar and struct names
		// roll back when their subtree contributed nothing, and an Array field always
		// leaves a definition behind since pass 1's array branch (before the M3b adapter,
		// arrays were skipped there and reached this line by design -- the loud
		// placeholder; nothing does now). Logged rather than checked because the two are
		// built independently and a silent absence is this milestone's signature failure.
		UE_LOG(LogVaCuus, Error, TEXT("VaCuus model '%s': top-level name '%s' has no field behind it and will not be bound"),
			*ModelName, *Name);
	}

	// PASS 3: the WHOLE TYPE as one struct definition, for its life as an ARRAY ELEMENT --
	// another model's array of this type resolves {{Rows[i].Member}} by walking THIS
	// definition from GetRawPtr(i), the "base of the definition set's own type" the file
	// comment's invariant names. Built eagerly rather than lazily because the leaf/nested
	// split it needs (RootLeaves) is a local of this constructor; the model-root path above
	// neither uses it nor changed for it.
	RootStruct = StructDefinitions.Add_GetRef(MakeUnique<FVaCuusStructDefinition>(ModelName)).Get();
	for (const FString& Name : Layout.GetTopLevelNames())
	{
		if (FVaCuusPropertyDefinition** Leaf = RootLeaves.Find(Name))
		{
			// Offset 0, and not looked up: a top-level leaf's ContainerOffset is 0 by
			// construction (BuildLevel starts at BaseOffset 0), and the property definition
			// applies Offset_Internal itself in DereferencePointer.
			RootStruct->AddLeaf(Name, *Leaf, /*ContainerOffsetFromTypeBase=*/0);
		}
		else if (FVaCuusStructDefinition** Nested = StructsByPrefix.Find(Name + TEXT(".")))
		{
			// The nested definition's leaves carry offsets absolute from this type's base
			// already, so it plugs in here exactly as it does at pass 2's top level.
			RootStruct->AddNested(Name, *Nested);
		}
	}
}

FVaCuusStructDefinition* FVaCuusModelDefinitions::GetOrCreateStructFor(const FString& Prefix)
{
	// Prefix is a dotted path ending in '.': "Origin." , "A.B." .
	check(!Prefix.IsEmpty() && Prefix.EndsWith(TEXT(".")));

	if (FVaCuusStructDefinition** Existing = StructsByPrefix.Find(Prefix))
	{
		return *Existing;
	}

	const FString Body = Prefix.LeftChop(1);

	int32 LastDot = INDEX_NONE;
	Body.FindLastChar(TEXT('.'), LastDot);
	const FString ParentPrefix = (LastDot == INDEX_NONE) ? FString() : Body.Left(LastDot + 1);
	const FString Segment = (LastDot == INDEX_NONE) ? Body : Body.Mid(LastDot + 1);

	FVaCuusStructDefinition* Created =
		StructDefinitions.Add_GetRef(MakeUnique<FVaCuusStructDefinition>(Struct->GetName() + TEXT(".") + Body)).Get();
	StructsByPrefix.Add(Prefix, Created);

	// A level below the root attaches to its parent here; a top-level one is picked up by
	// pass 2 instead. Recursive, and it terminates because each step drops one segment.
	if (!ParentPrefix.IsEmpty())
	{
		GetOrCreateStructFor(ParentPrefix)->AddNested(Segment, Created);
	}

	return Created;
}

Rml::VariableDefinition* FVaCuusModelDefinitions::FindTopLevel(const FString& Name) const
{
	const FTopLevelVariable* Found =
		TopLevelVariables.FindByPredicate([&Name](const FTopLevelVariable& Variable) { return Variable.Name == Name; });
	return Found != nullptr ? Found->Definition : nullptr;
}

// ---------------------------------------------------------------------------------------
// FVaCuusDefinitionRegistry
// ---------------------------------------------------------------------------------------

const FVaCuusModelDefinitions* FVaCuusDefinitionRegistry::GetOrCreate(const FVaCuusModelLayout& Layout)
{
	// THE ASSERT THAT MAKES THE PROCESS-WIDE MAP LEGAL. Everything below is plain
	// non-atomic state read and written on the RmlUi thread, and the definitions it hands
	// out are consumed inside Context::Update().
	check(FVaCuusUIThread::IsInUIThread());

	if (!Layout.IsValid())
	{
		UE_LOG(LogVaCuus, Error, TEXT("VaCuus model: cannot build RmlUi definitions from an invalid layout"));
		return nullptr;
	}

	const UScriptStruct* Key = Layout.GetStruct();
	if (TUniquePtr<FVaCuusModelDefinitions>* Existing = GDefinitionsByStruct.Find(Key))
	{
		if (!(*Existing)->bStaleFromRecompile)
		{
			return Existing->Get();
		}

		// EVICT-ON-NEXT-USE, the second half of MarkStale's contract (VaCuus-akj.16): every
		// FProperty* in this set was deleted by the recompile, and the caller is holding a
		// layout freshly resolved against the NEW chain -- exactly the input a rebuild needs.
		// Freeing here is legal because nothing can still reach the old set: every model
		// whose evaluation went through it (as root or as a borrowed element definition) was
		// condemned and dropped in the same PreChange transaction that marked it, and the
		// mark command FIFO-precedes any re-bind. The destructor frees only what this set
		// OWNS -- a borrowed element-definition pointer in another (equally stale, equally
		// unreachable) set is not chased.
		UE_LOG(LogVaCuus, Log,
			TEXT("VaCuus definitions for '%s' were stale after a Blueprint struct recompile; rebuilt over the new property chain"),
			*Key->GetName());
		GDefinitionsByStruct.Remove(Key);
		GNumStaleEvictions.fetch_add(1, std::memory_order_relaxed);
	}

	// `new` rather than MakeUnique: the constructor is private with this class as its only
	// friend, which is what stops a definition set being built anywhere the UI-thread assert
	// above does not run.
	TUniquePtr<FVaCuusModelDefinitions> Created(new FVaCuusModelDefinitions(Layout));
	return GDefinitionsByStruct.Add(Key, MoveTemp(Created)).Get();
}

int32 FVaCuusDefinitionRegistry::Num()
{
	check(FVaCuusUIThread::IsInUIThread());
	return GDefinitionsByStruct.Num();
}

uint64 FVaCuusDefinitionRegistry::GetNumStaleEvictions()
{
	// Any thread, relaxed: a test comparing before/after counts across its own fence.
	return GNumStaleEvictions.load(std::memory_order_relaxed);
}

void FVaCuusDefinitionRegistry::MarkStale(const UScriptStruct* RecompiledStruct, const FString& StructName)
{
	check(FVaCuusUIThread::IsInUIThread());

	// A raw-pointer KEY comparison and a flag write -- RecompiledStruct is never dereferenced,
	// so a type that was collected between enqueue and drain is simply a miss.
	if (TUniquePtr<FVaCuusModelDefinitions>* Existing = GDefinitionsByStruct.Find(RecompiledStruct))
	{
		(*Existing)->bStaleFromRecompile = true;
		UE_LOG(LogVaCuus, Log,
			TEXT("VaCuus definitions for '%s' marked stale (Blueprint struct recompile); the next bind over the type rebuilds them"),
			*StructName);
	}
}

int32 FVaCuusDefinitionRegistry::ReleaseAll()
{
	check(FVaCuusUIThread::IsInUIThread());

	// THE REASON THIS EXISTS IS STATIC DESTRUCTION, NOT MEMORY. GDefinitionsByStruct is a
	// namespace-scope global (top of this file) whose values hold a
	// TStrongObjectPtr<const UScriptStruct> (VaCuusDataVariable.h, FVaCuusModelDefinitions::
	// Struct). Left alone the map is destroyed by the C++ runtime AFTER main returns, and
	// TStrongObjectPtr's destructor calls UObjectBase::ReleaseRef into a UObject system --
	// GUObjectArray and its reference-count table -- that by then may already be gone. That is
	// an at-exit crash whose stack points at a global nobody thinks of as code.
	//
	// FVaCuusUIThread::Exit() IS THE POINT WHERE THIS IS PROVABLE. By the time it calls here
	// every host has been Shutdown(), Rml::Core is down, and Hosts/RetiredHosts/Models are all
	// empty -- so no Rml::DataModel holds a VariableDefinition* into these sets any more, and
	// nothing but this map still refers to a definition or pins its UScriptStruct. Exit() also
	// runs ON the UI thread, which is what the assert above needs and what a destructor running
	// on the owner's thread could not offer.
	//
	// RE-ENTRY IS FINE: a later UI thread rebuilds on first use, which is what GetOrCreate
	// already does for a type it has not seen. The cost is one rebuild per model type per
	// UI-thread lifetime, and outside the test suite a process has exactly one.
	const int32 NumReleased = GDefinitionsByStruct.Num();
	GDefinitionsByStruct.Empty();

	return NumReleased;
}

// ---------------------------------------------------------------------------------------
// VaCuusData
// ---------------------------------------------------------------------------------------

int32 VaCuusData::BindModelVariables(Rml::DataModelConstructor& Constructor, const FVaCuusModelLayout& Layout, FVaCuusModelShadow& Shadow)
{
	check(FVaCuusUIThread::IsInUIThread());

	// THE SHADOW IS TAKEN AS A SHADOW, NOT AS A void*, so that "the pointer RmlUi retains
	// addresses a UI-owned instance of exactly this type" is checkable at the one place it is
	// established. RmlUi stores that pointer once, at bind time, and never revalidates it
	// (research 2.1) -- there is no second chance to notice.
	if (!Shadow.IsValid() || Shadow.GetStruct() != Layout.GetStruct())
	{
		UE_LOG(LogVaCuus, Error, TEXT("VaCuus model: the shadow buffer ('%s') does not match the layout ('%s'); nothing is bound"),
			Shadow.GetStruct() != nullptr ? *Shadow.GetStruct()->GetName() : TEXT("none"),
			Layout.GetStruct() != nullptr ? *Layout.GetStruct()->GetName() : TEXT("none"));
		return 0;
	}

	const FVaCuusModelDefinitions* Definitions = FVaCuusDefinitionRegistry::GetOrCreate(Layout);
	if (Definitions == nullptr)
	{
		return 0;
	}

	int32 NumBound = 0;
	for (const FVaCuusModelDefinitions::FTopLevelVariable& Variable : Definitions->GetTopLevelVariables())
	{
		// EVERY variable is bound with the SAME pointer, the shadow's base -- the invariant
		// the whole adapter rests on. A leaf applies its own offset in DereferencePointer; a
		// struct definition passes the base down untouched.
		if (Constructor.BindCustomDataVariable(ToRmlString(Variable.Name), Rml::DataVariable(Variable.Definition, Shadow.GetData())))
		{
			++NumBound;
		}
		else
		{
			// Reachable only if the name is already taken in this model, since
			// FVaCuusModelLayout already applied RmlUi's legality rule. RmlUi's own message is
			// an LT_WARNING (DataModel.cpp:135) and does reach the log through
			// FVaCuusSystemInterface, but it does not name the model.
			UE_LOG(LogVaCuus, Error, TEXT("VaCuus model '%s': RmlUi refused to bind the variable '%s'"),
				*Layout.GetStruct()->GetName(), *Variable.Name);
		}
	}

	return NumBound;
}

int32 VaCuusData::GetNumRefusedSets()
{
	check(FVaCuusUIThread::IsInUIThread());
	return GNumRefusedSets;
}

int32 VaCuusData::GetNumScalarGets()
{
	check(FVaCuusUIThread::IsInUIThread());
	return GNumScalarGets;
}

int32 VaCuusData::GetNumArraySizes()
{
	check(FVaCuusUIThread::IsInUIThread());
	return GNumArraySizes;
}

int32 VaCuusData::GetNumArrayChilds()
{
	check(FVaCuusUIThread::IsInUIThread());
	return GNumArrayChilds;
}
