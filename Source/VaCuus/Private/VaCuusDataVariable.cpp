// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusDataVariable.h"

#include "VaCuusDefines.h"
#include "VaCuusModelShadow.h"
#include "VaCuusUIThread.h"

#include "UObject/AnsiStrProperty.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/StrProperty.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include "UObject/Utf8StrProperty.h"

#include <RmlUi/Core/DataModelHandle.h>

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

bool FVaCuusScalarDefinition::Set(void* /*InValuePtr*/, const Rml::Variant& /*Variant*/)
{
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

void FVaCuusStructDefinition::AddLeaf(const FString& Segment, FVaCuusPropertyDefinition* Definition, int32 ContainerOffsetFromModelBase)
{
	check(Definition != nullptr);

	FMember& Member = Members.AddDefaulted_GetRef();
	Member.Segment = ToRmlString(Segment);
	Member.Definition = Definition;
	Member.ContainerOffsetFromModelBase = ContainerOffsetFromModelBase;
	Member.bNested = false;
}

void FVaCuusStructDefinition::AddNested(const FString& Segment, FVaCuusStructDefinition* Definition)
{
	check(Definition != nullptr);

	FMember& Member = Members.AddDefaulted_GetRef();
	Member.Segment = ToRmlString(Segment);
	Member.Definition = Definition;

	// ZERO, AND THERE IS NO PARAMETER TO GET IT WRONG WITH. A nested struct definition is
	// handed the MODEL BASE unchanged; the offset is applied exactly once, when the leaf
	// below it is handed out. See the file comment's invariant.
	Member.ContainerOffsetFromModelBase = 0;
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

Rml::DataVariable FVaCuusStructDefinition::Child(void* InModelBase, const Rml::DataAddressEntry& Address)
{
	// AN INDEXED ENTRY ON A STRUCT. DataAddressEntry(int) leaves `name` empty
	// (DataTypes.h:36-41), so `{{Origin[0]}}` arrives here with index >= 0 and no name.
	if (Address.index >= 0)
	{
		UE_LOG(LogVaCuus, Warning, TEXT("VaCuus model: '%s' is a struct and was indexed as an array ([%d]); arrays are M3b"),
			*DiagnosticPath, Address.index);
		return Rml::DataVariable();
	}

	if (const FMember* Member = Find(Address.name))
	{
		return Rml::DataVariable(Member->Definition, static_cast<uint8*>(InModelBase) + Member->ContainerOffsetFromModelBase);
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
	FString Available;
	for (const FMember& Member : Members)
	{
		Available += (Available.IsEmpty() ? TEXT("") : TEXT(", "));
		Available += UTF8_TO_TCHAR(Member.Segment.c_str());
	}

	UE_LOG(LogVaCuus, Warning, TEXT("VaCuus model: '%s' has no member '%s'. It has: %s"), *DiagnosticPath,
		UTF8_TO_TCHAR(Address.name.c_str()), Available.IsEmpty() ? TEXT("(nothing)") : *Available);

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

const FVaCuusStructDefinition* FVaCuusStructDefinition::FindNested(const FString& Segment) const
{
	const FMember* Member = Find(ToRmlString(Segment));
	return (Member != nullptr && Member->bNested) ? static_cast<const FVaCuusStructDefinition*>(Member->Definition) : nullptr;
}

// ---------------------------------------------------------------------------------------
// FVaCuusModelDefinitions
// ---------------------------------------------------------------------------------------

FVaCuusModelDefinitions::FVaCuusModelDefinitions(const FVaCuusModelLayout& Layout)
{
	Struct.Reset(Layout.GetStruct());

	const FString ModelName = Layout.GetStruct()->GetName();

	// PASS 1: one scalar definition and one property definition per LEAF, filed either as a
	// root-level variable or as a member of the struct definition for its dotted prefix.
	TMap<FString, FVaCuusPropertyDefinition*> RootLeaves;

	for (const FVaCuusModelField& Field : Layout.GetFields())
	{
		int32 LastDot = INDEX_NONE;
		Field.WireName.FindLastChar(TEXT('.'), LastDot);

		const FString Prefix = (LastDot == INDEX_NONE) ? FString() : Field.WireName.Left(LastDot + 1);
		const FString Segment = (LastDot == INDEX_NONE) ? Field.WireName : Field.WireName.Mid(LastDot + 1);

		const FString FieldPath = ModelName + TEXT(".") + Field.WireName;

		FVaCuusScalarDefinition* Scalar =
			ScalarDefinitions.Add_GetRef(MakeUnique<FVaCuusScalarDefinition>(Field.Property, Field.Kind, FieldPath)).Get();
		FVaCuusPropertyDefinition* PropertyDefinition =
			PropertyDefinitions.Add_GetRef(MakeUnique<FVaCuusPropertyDefinition>(Field.Property, Scalar)).Get();

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

		// Unreachable through FVaCuusModelLayout, which rolls back a top-level name whose
		// nested struct contributed nothing. Logged rather than checked because the two are
		// built independently and a silent absence is this milestone's signature failure.
		UE_LOG(LogVaCuus, Error, TEXT("VaCuus model '%s': top-level name '%s' has no field behind it and will not be bound"),
			*ModelName, *Name);
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
		return Existing->Get();
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
