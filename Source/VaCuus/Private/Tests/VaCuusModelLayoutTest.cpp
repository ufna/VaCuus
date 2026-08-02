// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusModelLayout.h"
#include "VaCuusModelLayoutTestTypes.h"
#include "VaCuusModelShadow.h"

#include "StructUtils/UserDefinedStruct.h"
#include "UObject/Package.h" // complete UPackage for NewObject(GetTransientPackage()) — UObjectGlobals.h only forward-declares it
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusModelLayoutTest
{
struct FFieldExpectation
{
	const TCHAR* WireName;
	const TCHAR* TopLevelName;
	EVaCuusFieldKind Kind;
};

/** Reads a field back out of a live struct instance through the layout's own addressing. */
template <typename T>
static T ReadThroughLayout(const FVaCuusModelField& Field, const void* Base)
{
	return *static_cast<const T*>(Field.Property->ContainerPtrToValuePtr<void>(Field.ContainerPtr(Base)));
}
}	 // namespace VaCuusModelLayoutTest

/**
 * THE WALK: one entry per bound leaf, in declaration order, with the right kind, and
 * nested structs flattened rather than emitted as a field of their own.
 *
 * Every row below is a silent-wrong-value bug if it is wrong, not a crash: a missing
 * entry is a variable that is simply absent from the document, and a wrong kind reads
 * the right bytes through the wrong accessor. Neither produces a log line on its own.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelLayoutFieldsTest, "VaCuus.Model.LayoutFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelLayoutFieldsTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelLayoutTest;

	const FVaCuusModelLayout Layout(FVaCuusLayoutTestModel::StaticStruct());

	if (!TestTrue(TEXT("the layout resolved its struct"), Layout.IsValid()))
	{
		return false;
	}

	// Declaration order, because PropertyLink is built by walking TFieldIterator in its
	// default order (Class.cpp:1036-1099) and ChildProperties head-to-tail is C++
	// declaration order (UObjectGlobals.cpp:6499-6507 iterates UHT's array backwards
	// while Class.cpp:723-727 prepends).
	//
	// Origin is a nested FVaCuusTestPoint and contributes TWO entries, not one: that is
	// what "flattened at build time" means, and both dirty the top-level name `Origin`.
	const FFieldExpectation Expected[] = {
		{TEXT("Ratio"), TEXT("Ratio"), EVaCuusFieldKind::FloatingPoint},
		{TEXT("Score"), TEXT("Score"), EVaCuusFieldKind::SignedInt},
		{TEXT("Level"), TEXT("Level"), EVaCuusFieldKind::UnsignedInt},
		{TEXT("bNativeBool"), TEXT("bNativeBool"), EVaCuusFieldKind::Bool},
		{TEXT("bBitfieldBool"), TEXT("bBitfieldBool"), EVaCuusFieldKind::Bool},
		{TEXT("bBitfieldTwo"), TEXT("bBitfieldTwo"), EVaCuusFieldKind::Bool},
		{TEXT("Title"), TEXT("Title"), EVaCuusFieldKind::String},
		{TEXT("Tag"), TEXT("Tag"), EVaCuusFieldKind::Name},
		{TEXT("Caption"), TEXT("Caption"), EVaCuusFieldKind::Text},
		{TEXT("Utf8Note"), TEXT("Utf8Note"), EVaCuusFieldKind::Utf8String},
		{TEXT("AnsiNote"), TEXT("AnsiNote"), EVaCuusFieldKind::AnsiString},
		{TEXT("Colour"), TEXT("Colour"), EVaCuusFieldKind::Enum},
		{TEXT("Icon"), TEXT("Icon"), EVaCuusFieldKind::ObjectPath},
		{TEXT("Origin.X"), TEXT("Origin"), EVaCuusFieldKind::FloatingPoint},
		{TEXT("Origin.Y"), TEXT("Origin"), EVaCuusFieldKind::FloatingPoint},
	};

	const TConstArrayView<FVaCuusModelField> Fields = Layout.GetFields();
	if (!TestEqual(TEXT("one entry per bound leaf, nested struct flattened"), Fields.Num(), int32(UE_ARRAY_COUNT(Expected))))
	{
		for (const FVaCuusModelField& Field : Fields)
		{
			AddInfo(FString::Printf(TEXT("  got %s (%s)"), *Field.WireName, LexToString(Field.Kind)));
		}
		return false;
	}

	const TConstArrayView<FString> TopLevelNames = Layout.GetTopLevelNames();
	for (int32 Index = 0; Index < Fields.Num(); ++Index)
	{
		const FVaCuusModelField& Field = Fields[Index];
		const FFieldExpectation& Want = Expected[Index];

		TestEqual(FString::Printf(TEXT("entry %d wire name"), Index), Field.WireName, FString(Want.WireName));
		TestEqual(FString::Printf(TEXT("%s kind"), Want.WireName), FString(LexToString(Field.Kind)), FString(LexToString(Want.Kind)));

		if (TestTrue(FString::Printf(TEXT("%s has a top-level name index"), Want.WireName),
				TopLevelNames.IsValidIndex(Field.TopLevelNameIndex)))
		{
			TestEqual(FString::Printf(TEXT("%s dirties the right top-level name"), Want.WireName),
				TopLevelNames[Field.TopLevelNameIndex], FString(Want.TopLevelName));
		}

		// The property is the LEAF, never the containing struct: an FStructProperty here
		// would mean the nested struct was emitted rather than flattened, and every
		// downstream read would go through FStructProperty::Identical.
		TestNull(FString::Printf(TEXT("%s is a leaf, not a struct"), Want.WireName),
			CastField<FStructProperty>(Field.Property));
	}

	// A top-level name appears once however many leaves hang off it. DirtyVariable takes
	// these and nothing finer, so a duplicate would dirty the same subtree twice.
	TestEqual(TEXT("top-level names are deduplicated"), TopLevelNames.Num(), int32(UE_ARRAY_COUNT(Expected)) - 1);

	// ADDRESSING. ContainerOffset is what makes flattening usable at all: a nested leaf's
	// Offset_Internal is relative to FVaCuusTestPoint, not to the model, so reading it
	// from the model's base without the offset lands on Ratio.
	FVaCuusLayoutTestModel Instance;
	Instance.Ratio = 0.25f;
	Instance.Score = -7;
	Instance.Level = 200;
	Instance.bNativeBool = true;
	Instance.bBitfieldBool = 1;
	Instance.Origin.X = 11.f;
	Instance.Origin.Y = 22.f;

	const FVaCuusModelField* OriginX = Layout.FindField(TEXT("Origin.X"));
	const FVaCuusModelField* OriginY = Layout.FindField(TEXT("Origin.Y"));
	const FVaCuusModelField* Ratio = Layout.FindField(TEXT("Ratio"));
	if (TestNotNull(TEXT("Origin.X is addressable"), OriginX) && TestNotNull(TEXT("Origin.Y is addressable"), OriginY)
		&& TestNotNull(TEXT("Ratio is addressable"), Ratio))
	{
		TestEqual(TEXT("Origin.X reads through the layout"), ReadThroughLayout<float>(*OriginX, &Instance), 11.f);
		TestEqual(TEXT("Origin.Y reads through the layout"), ReadThroughLayout<float>(*OriginY, &Instance), 22.f);
		TestEqual(TEXT("Ratio reads through the layout"), ReadThroughLayout<float>(*Ratio, &Instance), 0.25f);
		TestEqual(TEXT("a top-level field has no container offset"), Ratio->ContainerOffset, 0);
		TestTrue(TEXT("a nested leaf has one"), OriginX->ContainerOffset > 0);
	}

	// THE BITFIELD TRAP, made observable. bBitfieldBool and bBitfieldTwo are two separate
	// FBoolPropertys that address the SAME BYTE -- Offset_Internal points at the storage
	// integer, not at the bit (UnrealType.h:2595-2603) -- so anything that identifies a
	// field by (offset, size), or that memcmps the byte, cannot tell them apart. The only
	// correct read is through the mask.
	const FVaCuusModelField* NativeBool = Layout.FindField(TEXT("bNativeBool"));
	const FVaCuusModelField* BitOne = Layout.FindField(TEXT("bBitfieldBool"));
	const FVaCuusModelField* BitTwo = Layout.FindField(TEXT("bBitfieldTwo"));
	if (TestNotNull(TEXT("bNativeBool is addressable"), NativeBool) && TestNotNull(TEXT("bBitfieldBool is addressable"), BitOne)
		&& TestNotNull(TEXT("bBitfieldTwo is addressable"), BitTwo))
	{
		const FBoolProperty* NativeProp = CastField<FBoolProperty>(NativeBool->Property);
		const FBoolProperty* OneProp = CastField<FBoolProperty>(BitOne->Property);
		const FBoolProperty* TwoProp = CastField<FBoolProperty>(BitTwo->Property);
		if (TestNotNull(TEXT("bNativeBool is an FBoolProperty"), NativeProp)
			&& TestNotNull(TEXT("bBitfieldBool is an FBoolProperty"), OneProp)
			&& TestNotNull(TEXT("bBitfieldTwo is an FBoolProperty"), TwoProp))
		{
			TestTrue(TEXT("the native bool is native"), NativeProp->IsNativeBool());
			TestFalse(TEXT("the first bitfield is not"), OneProp->IsNativeBool());
			TestFalse(TEXT("the second bitfield is not"), TwoProp->IsNativeBool());

			// The aliasing itself: same address, same element size, different masks.
			TestTrue(TEXT("the two bitfields share one storage address"),
				OneProp->ContainerPtrToValuePtr<void>(BitOne->ContainerPtr(&Instance))
					== TwoProp->ContainerPtrToValuePtr<void>(BitTwo->ContainerPtr(&Instance)));
			TestTrue(TEXT("and one element size"), OneProp->GetElementSize() == TwoProp->GetElementSize());
			TestTrue(TEXT("but not one mask"), OneProp->GetFieldMask() != TwoProp->GetFieldMask());

			// So a mask-aware read tells them apart where a byte compare could not.
			Instance.bNativeBool = true;
			Instance.bBitfieldBool = 0;
			Instance.bBitfieldTwo = 1;
			TestTrue(TEXT("the native bool reads true"), NativeProp->GetPropertyValue_InContainer(NativeBool->ContainerPtr(&Instance)));
			TestFalse(TEXT("the first bitfield reads false"), OneProp->GetPropertyValue_InContainer(BitOne->ContainerPtr(&Instance)));
			TestTrue(TEXT("the second reads true from the same byte"),
				TwoProp->GetPropertyValue_InContainer(BitTwo->ContainerPtr(&Instance)));

			Instance.bNativeBool = false;
			Instance.bBitfieldBool = 1;
			Instance.bBitfieldTwo = 0;
			TestFalse(TEXT("the native bool reads false"), NativeProp->GetPropertyValue_InContainer(NativeBool->ContainerPtr(&Instance)));
			TestTrue(TEXT("the first bitfield reads true"), OneProp->GetPropertyValue_InContainer(BitOne->ContainerPtr(&Instance)));
			TestFalse(TEXT("the second reads false from the same byte"),
				TwoProp->GetPropertyValue_InContainer(BitTwo->ContainerPtr(&Instance)));
		}
	}

	// A null type is a caller bug: diagnosed, not crashed and not silently empty.
	AddExpectedMessagePlain(TEXT("VaCuus model layout: no struct type was given"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);
	const FVaCuusModelLayout NullLayout(nullptr);
	TestFalse(TEXT("a null struct yields an invalid layout"), NullLayout.IsValid());
	TestEqual(TEXT("and no fields"), NullLayout.GetFields().Num(), 0);

	return true;
}

/**
 * THE REFUSALS. Every kind the model does not carry is skipped with a diagnostic naming
 * the property and the reason -- never silently. The failure this guards against is a
 * designer writing {{Lookup}} and getting an inert document with nothing anywhere
 * saying why, which is the milestone's signature failure mode.
 *
 * TArray left this fixture in M3b: arrays bind now, and their own coverage -- acceptance
 * and the array-specific refusals -- lives in VaCuus.Model.LayoutArrays and
 * VaCuus.Model.LayoutArrayRefusals below.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelLayoutRefusalsTest, "VaCuus.Model.LayoutRefusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelLayoutRefusalsTest::RunTest(const FString& Parameters)
{
	// REGISTERED, NOT MERELY TOLERATED. An unmatched expectation fails the test
	// (AutomationTest.cpp:1376 ANDs in HasMetExpectedMessages), so each line below is the
	// assertion that this refusal was diagnosed -- exactly once, at Warning, naming the
	// property. Without them "skipped it" and "skipped it silently" are the same result.
	for (const TCHAR* Refused : {TEXT("Lookup"), TEXT("Names"), TEXT("Owner"), TEXT("Watcher"), TEXT("Fixed")})
	{
		AddExpectedMessagePlain(FString::Printf(TEXT("property '%s'"), Refused), ELogVerbosity::Warning,
			EAutomationExpectedMessageFlags::Contains, 1);
	}

	const FVaCuusModelLayout Layout(FVaCuusLayoutTestRefusedModel::StaticStruct());

	TestTrue(TEXT("the layout resolved its struct"), Layout.IsValid());

	// Exactly the one supported field survives. Hidden is opted out rather than
	// unsupported (neither CPF_BlueprintVisible nor CPF_Edit), so it is skipped quietly.
	if (TestEqual(TEXT("only the supported field is bound"), Layout.GetFields().Num(), 1))
	{
		TestEqual(TEXT("and it is the right one"), Layout.GetFields()[0].WireName, FString(TEXT("Kept")));
	}

	TestNull(TEXT("TMap is not bound"), Layout.FindField(TEXT("Lookup")));
	TestNull(TEXT("TSet is not bound"), Layout.FindField(TEXT("Names")));
	TestNull(TEXT("a hard object reference is not bound"), Layout.FindField(TEXT("Owner")));

	// AND NEITHER IS A WEAK ONE, which corrects spec 3.4's "projected to a path string at
	// sample time". There is nowhere to project it to: the shadow is a real instance of the
	// model type (spec 3.1), so the field is still an FWeakObjectPtr in it -- an index/serial
	// pair carrying no path. Producing one means resolving the object on the UI thread, which
	// WeakObjectPtr.h:295-296 rules out. A soft reference has no such problem, and stays.
	TestNull(TEXT("a weak object reference is not bound"), Layout.FindField(TEXT("Watcher")));
	TestNull(TEXT("a fixed-size C array is not bound"), Layout.FindField(TEXT("Fixed")));
	TestNull(TEXT("an unexposed property is not bound"), Layout.FindField(TEXT("Hidden")));

	return true;
}

/**
 * NAME LEGALITY (spec 3.3). RmlUi's legal names are a strict subset of UE's, and `Size`
 * -- about as ordinary as a UPROPERTY name gets -- is reserved. The policy is to refuse
 * loudly and omit, because renaming silently would put a name in the document that
 * appears nowhere in the C++.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelLayoutNamesTest, "VaCuus.Model.LayoutNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelLayoutNamesTest::RunTest(const FString& Parameters)
{
	// The rule itself, against RmlUi's LegalVariableName (DataModel.cpp:51-74).
	TestNull(TEXT("Health is legal"), VaCuusWireName::ValidateTopLevel(TEXT("Health")));
	TestNull(TEXT("under_scores and digits are legal after the first character"),
		VaCuusWireName::ValidateTopLevel(TEXT("Health_2_Max")));
	TestNotNull(TEXT("an empty name is not"), VaCuusWireName::ValidateTopLevel(TEXT("")));
	TestNotNull(TEXT("a leading digit is not"), VaCuusWireName::ValidateTopLevel(TEXT("2Health")));
	TestNotNull(TEXT("a leading underscore is not"), VaCuusWireName::ValidateTopLevel(TEXT("_Health")));
	TestNotNull(TEXT("a hyphen is not -- UE allows it in an FName, RmlUi does not"),
		VaCuusWireName::ValidateTopLevel(TEXT("Health-Max")));
	TestNotNull(TEXT("a dot is not"), VaCuusWireName::ValidateTopLevel(TEXT("Health.Max")));

	// Reserved, and RmlUi lowercases the whole name before testing, so the check is
	// case-insensitive in both directions.
	for (const TCHAR* Reserved : {TEXT("it"), TEXT("it_index"), TEXT("ev"), TEXT("true"), TEXT("false"), TEXT("size"), TEXT("literal")})
	{
		TestNotNull(FString::Printf(TEXT("%s is reserved"), Reserved), VaCuusWireName::ValidateTopLevel(Reserved));
		TestNotNull(FString::Printf(TEXT("%s is reserved case-insensitively"), Reserved),
			VaCuusWireName::ValidateTopLevel(FString(Reserved).ToUpper()));
	}

	// The nested rule is weaker, and deliberately so: RmlUi applies LegalVariableName
	// only in BindVariable, i.e. only to the first segment of an address.
	TestNull(TEXT("a nested segment may be a reserved word"), VaCuusWireName::ValidateNested(TEXT("Size")));
	TestNull(TEXT("a nested segment may start with a digit"), VaCuusWireName::ValidateNested(TEXT("2X")));
	TestNotNull(TEXT("a nested segment may not be empty"), VaCuusWireName::ValidateNested(TEXT("")));
	TestNotNull(TEXT("a nested segment may not contain a hyphen"), VaCuusWireName::ValidateNested(TEXT("My-Field")));
	TestNotNull(TEXT("a nested segment may not contain a dot"), VaCuusWireName::ValidateNested(TEXT("My.Field")));

	// And the layout enforces it: two reserved top-level names refused, the legal one kept.
	// At ERROR, not Warning -- an unsupported KIND is a limit of M3a, but an illegal name
	// is a field the author asked for that will never appear, and nothing else says so.
	AddExpectedMessagePlain(
		TEXT("cannot be bound under the name 'Size'"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(
		TEXT("cannot be bound under the name 'Literal'"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 1);

	const FVaCuusModelLayout Layout(FVaCuusLayoutTestIllegalNameModel::StaticStruct());
	if (TestEqual(TEXT("the illegally named fields are omitted"), Layout.GetFields().Num(), 1))
	{
		TestEqual(TEXT("and the legal one survives"), Layout.GetFields()[0].WireName, FString(TEXT("Legal")));
	}
	TestNull(TEXT("Size is refused"), Layout.FindField(TEXT("Size")));
	TestNull(TEXT("Literal is refused"), Layout.FindField(TEXT("Literal")));

	// The same name NESTED is bound, because RmlUi resolves it through Child() and never
	// through BindVariable. Refusing it would refuse a field that works.
	const FVaCuusModelLayout NestedLayout(FVaCuusLayoutTestNestedReservedModel::StaticStruct());
	if (TestEqual(TEXT("a reserved word nested under a legal name is bound"), NestedLayout.GetFields().Num(), 1))
	{
		TestEqual(TEXT("under its dotted path"), NestedLayout.GetFields()[0].WireName, FString(TEXT("Panel.Size")));
	}

	return true;
}

/**
 * A nested struct that contributes nothing must say so. Without this the struct is
 * simply absent from the model, which reads identically to "I forgot to expose it".
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelLayoutEmptyNestedTest, "VaCuus.Model.LayoutEmptyNested",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelLayoutEmptyNestedTest::RunTest(const FString& Parameters)
{
	AddExpectedMessagePlain(
		TEXT("nested struct property 'Empty'"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	const FVaCuusModelLayout Layout(FVaCuusLayoutTestEmptyNestedModel::StaticStruct());

	if (TestEqual(TEXT("the empty nested struct contributes nothing"), Layout.GetFields().Num(), 1))
	{
		TestEqual(TEXT("and its sibling survives"), Layout.GetFields()[0].WireName, FString(TEXT("Kept")));
	}

	// No orphan: a top-level name with no field behind it would be dirtied forever and
	// resolve to nothing.
	TestEqual(TEXT("no top-level name is left behind"), Layout.GetTopLevelNames().Num(), 1);

	return true;
}

/**
 * BLUEPRINT WIRE NAMES. A UUserDefinedStruct's members are named
 * `<Base>_<UniqueId>_<32hexGUID>` (StructureEditorUtils.cpp:256-264) and a designer
 * writing {{Health}} must not have to type the GUID.
 *
 * ASSERTED AS A CHOP OF GetName(), NOT AGAINST A LITERAL, and the reason is specific:
 * UUserDefinedStruct::GetAuthoredNameForField has two separate implementations
 * (UserDefinedStruct.cpp:281-314) -- an editor branch that asks the struct's EditorData
 * for a friendly name, and a string-chopping fallback below it that runs in a cooked
 * build. They agree for a renamed member and disagree for a never-renamed one (editor
 * `MemberVar_2`, cooked `MemberVar`). A literal expectation would bake in whichever one
 * this test happened to run under.
 *
 * The struct here is built by hand with no EditorData, so `Cast<UUserDefinedStructEditorDataBase>`
 * returns null and the fallback runs -- which means this editor test does exercise the
 * COOKED path. What it cannot exercise is the editor branch, and the prefix assertion
 * below is exactly the claim that holds under both.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelLayoutAuthoredNameTest, "VaCuus.Model.LayoutAuthoredName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelLayoutAuthoredNameTest::RunTest(const FString& Parameters)
{
	// A plausible mangled name: base `Health`, unique id 2, then '_' and 32 hex digits.
	static const TCHAR* MangledName = TEXT("Health_2_9F3C1A084F6E4B2D8E7A5C0B1D2E3F40");

	TStrongObjectPtr<UUserDefinedStruct> Struct(NewObject<UUserDefinedStruct>(GetTransientPackage(), NAME_None, RF_Transient));
	Struct->Guid = FGuid::NewGuid();
	Struct->Status = EUserDefinedStructureStatus::UDSS_UpToDate;

	// Owned by the struct: AddCppProperty prepends to ChildProperties (Class.cpp:723-727)
	// and the struct destroys its own FProperties, so this is not a leak.
	FIntProperty* Prop = new FIntProperty(Struct.Get(), FName(MangledName));
	Prop->SetPropertyFlags(CPF_BlueprintVisible);
	Struct->AddCppProperty(Prop);
	Struct->Bind();
	Struct->StaticLink(/*bRelinkExistingProperties=*/true);

	const FString RawName = Prop->GetName();
	const FString AuthoredName = Prop->GetAuthoredName();

	TestEqual(TEXT("the raw name is the mangled one"), RawName, FString(MangledName));
	TestTrue(TEXT("the authored name is a chop of the raw name"), RawName.StartsWith(AuthoredName));
	TestTrue(TEXT("and strictly shorter than it"), AuthoredName.Len() < RawName.Len());
	TestFalse(TEXT("so the GUID is gone"), AuthoredName.Contains(TEXT("9F3C1A08")));

	const FVaCuusModelLayout Layout(Struct.Get());
	if (TestEqual(TEXT("the Blueprint member is bound"), Layout.GetFields().Num(), 1))
	{
		// The point of the whole test: the wire name is the authored name, not GetName().
		TestEqual(TEXT("under its authored name"), Layout.GetFields()[0].WireName, AuthoredName);
		TestTrue(TEXT("which is not the mangled name"), Layout.GetFields()[0].WireName != RawName);
	}

	return true;
}

namespace VaCuusModelLayoutTest
{
/**
 * A Blueprint-style member name: `<Base>_<UniqueId>_<32hexGUID>`, which is what
 * FStructureEditorUtils::AddVariable produces (StructureEditorUtils.cpp:256-264) and what
 * UUserDefinedStruct::GetAuthoredNameForField chops back down (UserDefinedStruct.cpp:300-313).
 */
static FString MangledMemberName(const TCHAR* Base, int32 UniqueId, const TCHAR* Guid32)
{
	return FString::Printf(TEXT("%s_%d_%s"), Base, UniqueId, Guid32);
}

/**
 * An empty transient UUserDefinedStruct, ready for hand-built members; the caller links.
 *
 * PrepareCppStructOps() UP FRONT, because linking is not per-struct: FStructProperty::
 * LinkInternal asks the MEMBER type for its ops (`Struct->GetCppStructOps()`,
 * PropertyStruct.cpp:115-118), and GetCppStructOps asserts bPrepareCppStructOpsCompleted
 * (Class.h:2369) -- which the plain NewObject constructor path never sets. A pair of
 * structs that reference each other therefore cannot be linked in ANY order unless both
 * were prepared first; for a Blueprint struct the editor's serialize path does this
 * (Class.cpp:3328), and this call is that step for a hand-built one. A no-op result for a
 * UDS -- no native ops exist to find -- but the completed flag is the point.
 */
static UUserDefinedStruct* NewUserStruct()
{
	UUserDefinedStruct* Struct = NewObject<UUserDefinedStruct>(GetTransientPackage(), NAME_None, RF_Transient);
	Struct->Guid = FGuid::NewGuid();
	Struct->Status = EUserDefinedStructureStatus::UDSS_UpToDate;
	Struct->PrepareCppStructOps();
	return Struct;
}

/** Appends `int32 <Name>`, Blueprint-visible, to an un-linked user struct. */
static void AddIntMember(UUserDefinedStruct* Struct, const TCHAR* Name)
{
	FIntProperty* Prop = new FIntProperty(Struct, FName(Name));
	Prop->SetPropertyFlags(CPF_BlueprintVisible);
	Struct->AddCppProperty(Prop);
}

/** Appends `<MemberType> <Name>` by value, Blueprint-visible. */
static void AddStructMember(UUserDefinedStruct* Struct, const TCHAR* Name, UScriptStruct* MemberType)
{
	FStructProperty* Prop = new FStructProperty(Struct, FName(Name));
	Prop->Struct = MemberType;
	Prop->SetPropertyFlags(CPF_BlueprintVisible);
	Struct->AddCppProperty(Prop);
}

/**
 * Appends `TArray<ElementType> <Name>`, Blueprint-visible. The same two-property shape the
 * engine builds from UHT output: the inner FStructProperty is OWNED by the array property
 * (its FFieldVariant owner), and FArrayProperty::AddCppProperty is the blessed way to seat
 * it as Inner (PropertyArray.cpp:1252-1258). ElementType may still be un-linked here --
 * FStructProperty::LinkInternal only reads its PropertiesSize (PropertyStruct.cpp:94-115),
 * so a cross-referencing pair links in either order; a not-yet-linked element just leaves
 * the inner's size 0, which nothing in a LAYOUT build reads.
 */
static void AddArrayOfStructMember(UUserDefinedStruct* Struct, const TCHAR* Name, UScriptStruct* ElementType)
{
	FArrayProperty* ArrayProp = new FArrayProperty(Struct, FName(Name));
	FStructProperty* InnerProp = new FStructProperty(ArrayProp, FName(*(FString(Name) + TEXT("_ElementProp"))));
	InnerProp->Struct = ElementType;
	ArrayProp->AddCppProperty(InnerProp);
	ArrayProp->SetPropertyFlags(CPF_BlueprintVisible);
	Struct->AddCppProperty(ArrayProp);
}

/** A linked UUserDefinedStruct carrying the given int32 members, by raw (mangled) name. */
static UUserDefinedStruct* MakeUserStruct(const TArray<FString>& MemberNames)
{
	UUserDefinedStruct* Struct = NewUserStruct();

	for (const FString& MemberName : MemberNames)
	{
		AddIntMember(Struct, *MemberName);
	}

	Struct->Bind();
	Struct->StaticLink(/*bRelinkExistingProperties=*/true);
	return Struct;
}
}	 // namespace VaCuusModelLayoutTest

/**
 * DUPLICATE MEMBER (spec 8) -- the diagnostic that had no test.
 *
 * WHY IT IS REACHABLE AT ALL, WHICH IS THE WHOLE POINT. In C++ it is not: UHT refuses a
 * property that shadows one in the super chain outright ("shadowing is not allowed",
 * UhtProperty.cs:2357), so no native USTRUCT can produce two members with one name. The
 * collision is created by the WIRE NAME rule instead. `GetAuthoredName()` chops a Blueprint
 * member's `<Base>_<UniqueId>_<32hexGUID>` down to `<Base>`, and two never-renamed members --
 * `Health_2_...` and `Health_5_...` -- both chop to `Health`
 * (UserDefinedStruct.cpp:300-313). Without the check the layout would carry two entries under
 * one name: FindField() would answer with whichever came first, DirtyVariable would fire twice
 * for one variable, and the document would show one of the two values with nothing anywhere
 * saying which.
 *
 * THIS IS THE COOKED PATH, RUN IN THE EDITOR, and that is deliberate rather than a compromise.
 * GetAuthoredNameForField asks EditorData for a friendly name FIRST (UserDefinedStruct.cpp:289-298)
 * and a real Blueprint struct would answer `Health_2` -- no collision. The structs below are
 * built by hand with no EditorData, so `Cast<UUserDefinedStructEditorDataBase>` returns null and
 * the chopping fallback runs, which is exactly what a packaged build does. Spec 10 notes that an
 * editor automation test only exercises the editor branch; this one reaches the other.
 *
 * BOTH BRANCHES OF THE CHECK, because it is written as a ternary over two different lookups: a
 * top-level name is tested against TopLevelNames, a nested one against FindField().
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelLayoutDuplicateNameTest, "VaCuus.Model.LayoutDuplicateName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelLayoutDuplicateNameTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelLayoutTest;

	// REGISTERED, NOT MERELY TOLERATED: an unmatched expectation fails the test, so each of
	// these IS the assertion that the collision was diagnosed exactly once, at Error, naming the
	// wire name that was refused.
	AddExpectedMessagePlain(
		TEXT("cannot be bound under the name 'Health' --"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("cannot be bound under the name 'Panel.Health' --"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);

	const FString FirstHealth = MangledMemberName(TEXT("Health"), 2, TEXT("9F3C1A084F6E4B2D8E7A5C0B1D2E3F40"));
	const FString SecondHealth = MangledMemberName(TEXT("Health"), 5, TEXT("11223344556677889900AABBCCDDEEFF"));

	const TArray<FString> Members = {FirstHealth, SecondHealth};
	TStrongObjectPtr<UUserDefinedStruct> Inner(MakeUserStruct(Members));

	// The premise, asserted rather than assumed: two DIFFERENT properties whose authored names
	// are the SAME string. If UUserDefinedStruct ever stops chopping, this test stops testing
	// what it says it tests, and this is the line that would say so.
	const FProperty* FirstProp = Inner->FindPropertyByName(FName(*FirstHealth));
	const FProperty* SecondProp = Inner->FindPropertyByName(FName(*SecondHealth));
	if (!TestNotNull(TEXT("the first member exists"), FirstProp) || !TestNotNull(TEXT("the second member exists"), SecondProp))
	{
		return false;
	}
	TestTrue(TEXT("they are two distinct properties"), FirstProp != SecondProp);
	TestEqual(TEXT("whose authored names collide"), FirstProp->GetAuthoredName(), SecondProp->GetAuthoredName());
	TestEqual(TEXT("on 'Health'"), FirstProp->GetAuthoredName(), FString(TEXT("Health")));

	// ---- Top-level branch: TopLevelNames.Contains(WireName). ----
	{
		const FVaCuusModelLayout Layout(Inner.Get());

		if (TestEqual(TEXT("only one of the two colliding members is bound"), Layout.GetFields().Num(), 1))
		{
			TestEqual(TEXT("under the shared authored name"), Layout.GetFields()[0].WireName, FString(TEXT("Health")));
		}

		// A duplicate must not leave a second top-level name behind either: TopLevelNames is
		// what the bind step iterates, so an extra entry would bind a second RmlUi variable
		// against the same shadow. RmlUi's own refusal of the repeat is an LT_WARNING from
		// BindVariable's `already exists` arm (DataModel.cpp:133-137 -- NOT the name-legality
		// arm at :119-124), and it does reach the log through FVaCuusSystemInterface. It is
		// still the wrong place to find out: it fires per bind, on the UI thread, naming only
		// the wire name, while this asserts the layout never produced the duplicate at all.
		TestEqual(TEXT("and exactly one top-level name"), Layout.GetTopLevelNames().Num(), 1);
	}

	// ---- Nested branch: FindField(WireName) != nullptr. ----
	//
	// The same two members reached through a struct member, so the colliding wire name is
	// dotted and the check runs down its other arm.
	{
		TStrongObjectPtr<UUserDefinedStruct> Outer(
			NewObject<UUserDefinedStruct>(GetTransientPackage(), NAME_None, RF_Transient));
		Outer->Guid = FGuid::NewGuid();
		Outer->Status = EUserDefinedStructureStatus::UDSS_UpToDate;

		FStructProperty* PanelProp =
			new FStructProperty(Outer.Get(), FName(*MangledMemberName(TEXT("Panel"), 0, TEXT("0123456789ABCDEF0123456789ABCDEF"))));
		PanelProp->Struct = Inner.Get();
		PanelProp->SetPropertyFlags(CPF_BlueprintVisible);
		Outer->AddCppProperty(PanelProp);
		Outer->Bind();
		Outer->StaticLink(/*bRelinkExistingProperties=*/true);

		const FVaCuusModelLayout Layout(Outer.Get());

		if (TestEqual(TEXT("the nested collision leaves one leaf"), Layout.GetFields().Num(), 1))
		{
			TestEqual(TEXT("under its dotted path"), Layout.GetFields()[0].WireName, FString(TEXT("Panel.Health")));
		}
		TestEqual(TEXT("and one top-level name, the struct's"), Layout.GetTopLevelNames().Num(), 1);
	}

	return true;
}

/**
 * THE SHADOW BUFFER: a real UScriptStruct instance, addressable through the layout,
 * constructed and destroyed exactly once.
 *
 * The three claims, each with its own observable:
 *
 *  1. It is a REAL INSTANCE, not a zeroed block -- proved by a field whose C++ default is
 *     non-zero. InitializeStruct memzeroes and then constructs (Class.cpp:3783, :3798), so
 *     "allocated and zeroed" and "allocated and constructed" differ exactly here.
 *  2. EVERY FProperty ACCESSOR WORKS AGAINST IT unchanged, which is the whole reason for
 *     spec 3.1 -- including a nested leaf reached through FVaCuusModelField::ContainerPtr,
 *     because Offset_Internal means the same thing in this buffer as in a stack instance.
 *  3. IT IS DESTROYED, ONCE. A missing DestroyStruct leaks every heap-owning member and is
 *     otherwise completely silent, so the counted destructor is the only way to see it.
 *
 * Counters are read as deltas: linking a type may construct a throwaway instance, so the
 * value at entry is not knowable.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelShadowTest, "VaCuus.Model.Shadow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelShadowTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelLayoutTest;

	const FVaCuusModelLayout Layout(FVaCuusShadowProbeModel::StaticStruct());
	if (!TestTrue(TEXT("the layout resolved the probe struct"), Layout.IsValid()))
	{
		return false;
	}

	const FVaCuusModelField* Ratio = Layout.FindField(TEXT("Ratio"));
	const FVaCuusModelField* Title = Layout.FindField(TEXT("Title"));
	const FVaCuusModelField* OriginY = Layout.FindField(TEXT("Origin.Y"));
	if (!TestNotNull(TEXT("Ratio is bound"), Ratio) || !TestNotNull(TEXT("Title is bound"), Title)
		|| !TestNotNull(TEXT("Origin.Y is bound"), OriginY))
	{
		return false;
	}

	const int32 ConstructedBefore = VaCuusShadowProbe::NumConstructed;
	const int32 DestructedBefore = VaCuusShadowProbe::NumDestructed;

	const FFloatProperty* RatioProperty = CastField<FFloatProperty>(Ratio->Property);
	const FStrProperty* TitleProperty = CastField<FStrProperty>(Title->Property);
	const FFloatProperty* OriginYProperty = CastField<FFloatProperty>(OriginY->Property);
	if (!TestNotNull(TEXT("Ratio is a float"), RatioProperty) || !TestNotNull(TEXT("Title is a string"), TitleProperty)
		|| !TestNotNull(TEXT("Origin.Y is a float"), OriginYProperty))
	{
		return false;
	}

	static const TCHAR* LongString = TEXT("a string long enough that FString cannot inline it");

	{
		FVaCuusModelShadow Shadow(FVaCuusShadowProbeModel::StaticStruct());

		if (!TestTrue(TEXT("the shadow is valid"), Shadow.IsValid()) || !TestNotNull(TEXT("and has a buffer"), Shadow.GetData()))
		{
			return false;
		}
		TestTrue(TEXT("it remembers its type"), Shadow.GetStruct() == FVaCuusShadowProbeModel::StaticStruct());

		// (1) InitializeStruct ran the C++ constructor, exactly once.
		TestEqual(TEXT("the instance was constructed once"), VaCuusShadowProbe::NumConstructed - ConstructedBefore, 1);
		TestEqual(TEXT("and destroyed no times yet"), VaCuusShadowProbe::NumDestructed - DestructedBefore, 0);

		// ...so the C++ default survived. A memzeroed block reads 0.f here.
		TestEqual(TEXT("a non-zero C++ default survived into the buffer"),
			RatioProperty->GetPropertyValue_InContainer(Ratio->ContainerPtr(Shadow.GetData())), 0.5f);

		// (2) Write and read back through the layout's own addressing -- the top-level
		// heap-owning field and the nested leaf, which is the one that needs ContainerOffset.
		TitleProperty->SetPropertyValue_InContainer(Title->ContainerPtr(Shadow.GetData()), LongString);
		TestEqual(TEXT("an FString written through the layout reads back"),
			TitleProperty->GetPropertyValue_InContainer(Title->ContainerPtr(Shadow.GetData())), FString(LongString));

		OriginYProperty->SetPropertyValue_InContainer(OriginY->ContainerPtr(Shadow.GetData()), 42.f);
		TestEqual(TEXT("a nested leaf written through the layout reads back"),
			OriginYProperty->GetPropertyValue_InContainer(OriginY->ContainerPtr(Shadow.GetData())), 42.f);

		// ...and did not land on top of its sibling: Origin.Y's Offset_Internal is relative
		// to FVaCuusTestPoint, so without ContainerOffset this write hits Ratio.
		TestEqual(TEXT("and did not overwrite a sibling"),
			RatioProperty->GetPropertyValue_InContainer(Ratio->ContainerPtr(Shadow.GetData())), 0.5f);

		// MOVE. The moved-from shadow must not destroy the buffer the moved-to now owns --
		// a double DestroyStruct on an FString member is a double free, not a leak.
		FVaCuusModelShadow Moved(MoveTemp(Shadow));
		TestFalse(TEXT("the moved-from shadow is empty"), Shadow.IsValid());
		TestNull(TEXT("and holds no buffer"), Shadow.GetData());
		TestTrue(TEXT("the moved-to shadow owns it"), Moved.IsValid());
		TestEqual(TEXT("with its values intact"),
			TitleProperty->GetPropertyValue_InContainer(Title->ContainerPtr(Moved.GetData())), FString(LongString));
		TestEqual(TEXT("and still exactly one live instance"), VaCuusShadowProbe::NumDestructed - DestructedBefore, 0);
	}

	// (3) Both shadows left scope; exactly one instance existed, so exactly one destruction.
	TestEqual(TEXT("the instance was destroyed exactly once"), VaCuusShadowProbe::NumDestructed - DestructedBefore, 1);
	TestEqual(TEXT("and no extra instance was constructed on the way"),
		VaCuusShadowProbe::NumConstructed - ConstructedBefore, 1);

	// A null type is a caller bug: diagnosed, empty, and safe to destroy.
	AddExpectedMessagePlain(TEXT("VaCuus model shadow: no struct type was given"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);
	{
		FVaCuusModelShadow Empty(nullptr);
		TestFalse(TEXT("a null type yields an empty shadow"), Empty.IsValid());
		TestNull(TEXT("with no buffer"), Empty.GetData());
	}

	return true;
}

/**
 * ARRAYS (M3b). An array is a LEAF WITH A SIDE TABLE -- one entry, one dirty bit, one
 * FVaCuusModelArrayDesc -- because leaf count is fixed at build time while element count
 * is per-instance, so flattening cannot apply (spec 3.1). Legal at top level and nested
 * inside a flattened struct; struct elements get a PLAIN FVaCuusModelLayout over the
 * element type, with the same flattening and kinds a model root would get.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelLayoutArraysTest, "VaCuus.Model.LayoutArrays",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelLayoutArraysTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelLayoutTest;

	const FVaCuusModelLayout Layout(FVaCuusArrayTestModel::StaticStruct());
	if (!TestTrue(TEXT("the layout resolved its struct"), Layout.IsValid()))
	{
		return false;
	}

	// Declaration order, arrays as leaves: Panel contributes its one array leaf under the
	// dotted path and no entry of its own, exactly like any other nested struct member.
	const FFieldExpectation Expected[] = {
		{TEXT("Scalar"), TEXT("Scalar"), EVaCuusFieldKind::SignedInt},
		{TEXT("Numbers"), TEXT("Numbers"), EVaCuusFieldKind::Array},
		{TEXT("Labels"), TEXT("Labels"), EVaCuusFieldKind::Array},
		{TEXT("Ratios"), TEXT("Ratios"), EVaCuusFieldKind::Array},
		{TEXT("Killfeed"), TEXT("Killfeed"), EVaCuusFieldKind::Array},
		{TEXT("Panel.Items"), TEXT("Panel"), EVaCuusFieldKind::Array},
	};

	const TConstArrayView<FVaCuusModelField> Fields = Layout.GetFields();
	if (!TestEqual(TEXT("one entry per field, arrays as leaves"), Fields.Num(), int32(UE_ARRAY_COUNT(Expected))))
	{
		for (const FVaCuusModelField& Field : Fields)
		{
			AddInfo(FString::Printf(TEXT("  got %s (%s)"), *Field.WireName, LexToString(Field.Kind)));
		}
		return false;
	}

	const TConstArrayView<FString> TopLevelNames = Layout.GetTopLevelNames();
	for (int32 Index = 0; Index < Fields.Num(); ++Index)
	{
		const FVaCuusModelField& Field = Fields[Index];
		const FFieldExpectation& Want = Expected[Index];

		TestEqual(FString::Printf(TEXT("entry %d wire name"), Index), Field.WireName, FString(Want.WireName));
		TestEqual(FString::Printf(TEXT("%s kind"), Want.WireName), FString(LexToString(Field.Kind)), FString(LexToString(Want.Kind)));
		if (TestTrue(FString::Printf(TEXT("%s has a top-level name index"), Want.WireName),
				TopLevelNames.IsValidIndex(Field.TopLevelNameIndex)))
		{
			TestEqual(FString::Printf(TEXT("%s dirties the right top-level name"), Want.WireName),
				TopLevelNames[Field.TopLevelNameIndex], FString(Want.TopLevelName));
		}
	}

	const FVaCuusModelField* Scalar = Layout.FindField(TEXT("Scalar"));
	const FVaCuusModelField* Numbers = Layout.FindField(TEXT("Numbers"));
	const FVaCuusModelField* Labels = Layout.FindField(TEXT("Labels"));
	const FVaCuusModelField* Killfeed = Layout.FindField(TEXT("Killfeed"));
	const FVaCuusModelField* PanelItems = Layout.FindField(TEXT("Panel.Items"));
	if (!TestNotNull(TEXT("Scalar resolved"), Scalar) || !TestNotNull(TEXT("Numbers resolved"), Numbers)
		|| !TestNotNull(TEXT("Labels resolved"), Labels) || !TestNotNull(TEXT("Killfeed resolved"), Killfeed)
		|| !TestNotNull(TEXT("Panel.Items resolved"), PanelItems))
	{
		return false;
	}

	// THE DESC. A non-array field has none; an array field has exactly one, fixed up to a
	// stable address after the build, carrying the Inner every element read goes through.
	TestNull(TEXT("a scalar field has no desc"), Scalar->ArrayDesc);
	TestEqual(TEXT("and no desc index"), Scalar->ArrayDescIndex, int32(INDEX_NONE));

	if (TestNotNull(TEXT("Numbers has a desc"), Numbers->ArrayDesc))
	{
		TestTrue(TEXT("whose array property is the field's own"),
			static_cast<const FProperty*>(Numbers->ArrayDesc->ArrayProperty) == Numbers->Property);
		TestNotNull(TEXT("and whose Inner is resolved"), Numbers->ArrayDesc->Inner);
		TestFalse(TEXT("an int32 element is not a struct"), Numbers->ArrayDesc->IsStructElement());
		TestEqual(TEXT("it is a signed integer"), FString(LexToString(Numbers->ArrayDesc->ElementKind)),
			FString(LexToString(EVaCuusFieldKind::SignedInt)));
	}
	if (TestNotNull(TEXT("Labels has a desc"), Labels->ArrayDesc))
	{
		TestEqual(TEXT("with String elements"), FString(LexToString(Labels->ArrayDesc->ElementKind)),
			FString(LexToString(EVaCuusFieldKind::String)));
	}

	// THE ELEMENT LAYOUT IS PLAIN (spec 3.1): same flattening, same kinds, same top-level
	// names as a model root -- the nested struct inside the row contributes dotted leaves.
	if (TestNotNull(TEXT("Killfeed has a desc"), Killfeed->ArrayDesc)
		&& TestTrue(TEXT("with struct elements"), Killfeed->ArrayDesc->IsStructElement()))
	{
		const FVaCuusModelLayout& Rows = *Killfeed->ArrayDesc->ElementLayout;
		TestTrue(TEXT("the element layout resolved the row type"), Rows.GetStruct() == FVaCuusTestKillfeedRow::StaticStruct());

		const FFieldExpectation RowExpected[] = {
			{TEXT("Killer"), TEXT("Killer"), EVaCuusFieldKind::String},
			{TEXT("Victim"), TEXT("Victim"), EVaCuusFieldKind::String},
			{TEXT("bHeadshot"), TEXT("bHeadshot"), EVaCuusFieldKind::Bool},
			{TEXT("Impact.X"), TEXT("Impact"), EVaCuusFieldKind::FloatingPoint},
			{TEXT("Impact.Y"), TEXT("Impact"), EVaCuusFieldKind::FloatingPoint},
		};
		if (TestEqual(TEXT("the row flattens like a root"), Rows.GetFields().Num(), int32(UE_ARRAY_COUNT(RowExpected))))
		{
			for (int32 Index = 0; Index < Rows.GetFields().Num(); ++Index)
			{
				TestEqual(FString::Printf(TEXT("row entry %d wire name"), Index), Rows.GetFields()[Index].WireName,
					FString(RowExpected[Index].WireName));
				TestEqual(FString::Printf(TEXT("row entry %d kind"), Index), FString(LexToString(Rows.GetFields()[Index].Kind)),
					FString(LexToString(RowExpected[Index].Kind)));
			}
		}
	}

	// NESTED PLACEMENT: the array leaf composes ContainerOffset like any other nested leaf.
	TestTrue(TEXT("Panel.Items has a container offset"), PanelItems->ContainerOffset > 0);
	TestNotNull(TEXT("and a desc"), PanelItems->ArrayDesc);

	// THE COPY FUNNEL. CopyValue must route an array field through the desc -- deep copy,
	// shrink included -- with nothing but the field in hand; that is what lets every
	// pipeline stage stay kind-agnostic.
	FVaCuusArrayTestModel Source;
	Source.Numbers = {1, 2, 3};
	Source.Labels = {TEXT("alpha"), TEXT("beta")};
	Source.Killfeed.AddDefaulted_GetRef().Killer = TEXT("Ada");
	Source.Killfeed[0].Impact.Y = 7.f;
	Source.Panel.Items = {42};

	TestTrue(TEXT("an array field reads back through the layout"),
		ReadThroughLayout<TArray<int32>>(*Numbers, &Source) == TArray<int32>({1, 2, 3}));

	FVaCuusArrayTestModel Dest;
	Dest.Numbers = {9, 9, 9, 9};	// Longer than the source: the copy must shrink, not merge.
	Numbers->CopyValue(&Dest, &Source);
	Labels->CopyValue(&Dest, &Source);
	Killfeed->CopyValue(&Dest, &Source);
	PanelItems->CopyValue(&Dest, &Source);

	TestTrue(TEXT("a shrinking copy arrives whole"), Dest.Numbers == TArray<int32>({1, 2, 3}));
	TestTrue(TEXT("string elements deep-copy"),
		Dest.Labels.Num() == 2 && Dest.Labels[0].Equals(TEXT("alpha"), ESearchCase::CaseSensitive)
			&& Dest.Labels[1].Equals(TEXT("beta"), ESearchCase::CaseSensitive));
	if (TestEqual(TEXT("struct rows arrive"), Dest.Killfeed.Num(), 1))
	{
		TestEqual(TEXT("with their strings"), Dest.Killfeed[0].Killer, FString(TEXT("Ada")));
		TestEqual(TEXT("and their nested leaves"), Dest.Killfeed[0].Impact.Y, 7.f);
	}
	TestTrue(TEXT("the nested array arrives"), Dest.Panel.Items == TArray<int32>({42}));

	// THE DUMP FORM: Num() + first 8 + an elision marker (spec 6).
	FVaCuusArrayTestModel Dump;
	TestEqual(TEXT("an empty array describes as empty"), Numbers->DescribeValue(&Dump), FString(TEXT("0 elements []")));
	for (int32 Value = 0; Value < 10; ++Value)
	{
		Dump.Numbers.Add(Value);
	}
	TestEqual(TEXT("ten ints describe as the first eight plus an elision"), Numbers->DescribeValue(&Dump),
		FString(TEXT("10 elements [0, 1, 2, 3, 4, 5, 6, 7, ... 2 more]")));
	Dump.Killfeed.AddDefaulted_GetRef().Victim = TEXT("Bob");
	TestTrue(TEXT("a struct row prints its leaves"), Killfeed->DescribeValue(&Dump).Contains(TEXT("Victim=Bob")));

	return true;
}

/**
 * ARRAY REFUSALS (spec 3.1/3.2). What the shared element layout cannot refuse, the desc
 * build refuses ON THE ARRAY FIELD -- one Warning naming the array property, the offending
 * member and the reason -- and the element layout itself applies the FULL root rules, so a
 * row member named `Size` gets the root Error. Nothing is silent, and every refusal takes
 * only its own field.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelLayoutArrayRefusalsTest, "VaCuus.Model.LayoutArrayRefusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelLayoutArrayRefusalsTest::RunTest(const FString& Parameters)
{
	// Registered, not merely tolerated: an unmatched expectation fails the test, so each
	// line IS the assertion that the refusal was diagnosed, once, naming what it names.
	// ONE pattern per message -- the framework consumes a log line at its first matching
	// expectation, so a second pattern aimed at the same line would count zero.
	AddExpectedMessagePlain(TEXT("array property 'Texts'"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("array property 'TextRows' (TArray) cannot be bound -- element member 'Label'"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("array property 'NestedRows' (TArray) cannot be bound -- element member 'Inner'"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	// The row member named `Size`: the element layout is a plain layout, so the ROOT rule
	// and the root Error apply (spec 3.1's stated price; the workaround is a rename).
	AddExpectedMessagePlain(
		TEXT("cannot be bound under the name 'Size'"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 1);

	// The map and set members inside a row: the shared classifier refuses each MEMBER, as
	// on a root -- two expectations because they are two different classifier branches.
	AddExpectedMessagePlain(TEXT("property 'Lookup'"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("property 'Tags'"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	// The zero-bindable row type: the Warning names the ARRAY property, not the row type --
	// and no root-flavored "no property could be bound" line fires for the row (an
	// unexpected Warning fails the test, so its absence is asserted by there being no
	// expectation for it).
	AddExpectedMessagePlain(TEXT("array property 'BarrenRows'"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);

	const FVaCuusModelLayout Layout(FVaCuusArrayRefusalModel::StaticStruct());
	TestTrue(TEXT("the layout resolved its struct"), Layout.IsValid());

	// Refused whole: Text elements, Text anywhere in the element subtree, a container
	// anywhere in the element subtree, and a row type with nothing bindable in it.
	TestNull(TEXT("TArray<FText> is refused"), Layout.FindField(TEXT("Texts")));
	TestNull(TEXT("a row with an FText member is refused whole"), Layout.FindField(TEXT("TextRows")));
	TestNull(TEXT("a row with a TArray member is refused whole"), Layout.FindField(TEXT("NestedRows")));
	TestNull(TEXT("a row type with no bindable member is refused whole"), Layout.FindField(TEXT("BarrenRows")));

	// Survivors: the scalar control and the two arrays whose offending MEMBERS -- not the
	// arrays themselves -- were refused.
	TestNotNull(TEXT("the scalar control survives"), Layout.FindField(TEXT("Kept")));
	TestEqual(TEXT("exactly the three survivors bind"), Layout.GetFields().Num(), 3);

	const FVaCuusModelField* ReservedRows = Layout.FindField(TEXT("ReservedRows"));
	if (TestNotNull(TEXT("a row member named Size does not refuse its array"), ReservedRows)
		&& TestNotNull(TEXT("its desc exists"), ReservedRows->ArrayDesc)
		&& TestTrue(TEXT("with a struct element layout"), ReservedRows->ArrayDesc->IsStructElement()))
	{
		TestNull(TEXT("the reserved member is refused inside the element layout"),
			ReservedRows->ArrayDesc->ElementLayout->FindField(TEXT("Size")));
		TestNotNull(TEXT("and its sibling binds"), ReservedRows->ArrayDesc->ElementLayout->FindField(TEXT("Kept")));
	}

	const FVaCuusModelField* MapRows = Layout.FindField(TEXT("MapRows"));
	if (TestNotNull(TEXT("a TMap or TSet member does not refuse its array"), MapRows)
		&& TestNotNull(TEXT("its desc exists"), MapRows->ArrayDesc)
		&& TestTrue(TEXT("with a struct element layout"), MapRows->ArrayDesc->IsStructElement()))
	{
		TestNull(TEXT("the map member is dropped by the shared classifier"),
			MapRows->ArrayDesc->ElementLayout->FindField(TEXT("Lookup")));
		TestNull(TEXT("the set member is dropped by the shared classifier"),
			MapRows->ArrayDesc->ElementLayout->FindField(TEXT("Tags")));
		TestNotNull(TEXT("and their sibling binds"), MapRows->ArrayDesc->ElementLayout->FindField(TEXT("Kept")));
	}

	return true;
}

/**
 * CONTAINER CYCLES (review finding on M3b Task 1). Without the build-stack guard, a
 * container-cyclic type graph recurses the layout build to a process-killing stack
 * overflow at BIND time -- the guard refuses the array field that closes the loop, names
 * the cycle, and takes nothing else with it.
 *
 * HAND-BUILT UUserDefinedStructs, BECAUSE NATIVE FIXTURES CANNOT EXIST: UHT refuses the
 * direct shape outright (`structProperty.ScriptStruct == outerStruct`,
 * UhtArrayProperty.cs:216-222) and every indirect shape at the forward reference it needs
 * ("the code generation hash is zero", UhtProperty.cs:3066-3071) -- the fixture header
 * records the attempt. That refusal is NOT protection: nothing validates the FProperty
 * graph itself, so the cycle arrives exactly the way this test builds it, and the layout
 * is the first thing that would walk into it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelLayoutCycleTest, "VaCuus.Model.LayoutCycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelLayoutCycleTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelLayoutTest;

	// ---- The mutual pair: A{TArray<B>} / B{TArray<A>}. The cycle closes one level down,
	// so the OUTER array binds and the refusal lands inside the element layout. ----
	AddExpectedMessagePlain(TEXT("array property 'As'"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	TStrongObjectPtr<UUserDefinedStruct> StructA(NewUserStruct());
	TStrongObjectPtr<UUserDefinedStruct> StructB(NewUserStruct());
	AddIntMember(StructA.Get(), TEXT("Kept"));
	AddArrayOfStructMember(StructA.Get(), TEXT("Bs"), StructB.Get());
	AddIntMember(StructB.Get(), TEXT("Kept"));
	AddArrayOfStructMember(StructB.Get(), TEXT("As"), StructA.Get());
	StructA->Bind();
	StructA->StaticLink(/*bRelinkExistingProperties=*/true);
	StructB->Bind();
	StructB->StaticLink(/*bRelinkExistingProperties=*/true);

	const FVaCuusModelLayout Layout(StructA.Get());
	TestTrue(TEXT("the layout resolved its struct"), Layout.IsValid());
	TestNotNull(TEXT("the root's bindable sibling survives"), Layout.FindField(TEXT("Kept")));

	const FVaCuusModelField* Bs = Layout.FindField(TEXT("Bs"));
	if (TestNotNull(TEXT("the outer array binds -- only the loop-closing edge is refused"), Bs)
		&& TestNotNull(TEXT("with a desc"), Bs->ArrayDesc)
		&& TestTrue(TEXT("whose elements are structs"), Bs->ArrayDesc->IsStructElement()))
	{
		const FVaCuusModelLayout& RowLayout = *Bs->ArrayDesc->ElementLayout;
		TestNull(TEXT("the cycling array is absent from the element layout"), RowLayout.FindField(TEXT("As")));
		TestNotNull(TEXT("and the row's bindable sibling survives"), RowLayout.FindField(TEXT("Kept")));
	}

	// ---- The by-value hop: Row{Sub} / Sub{TArray<Row>}. The cycle closes on the root type
	// itself, so the refused array is a member of THIS layout and its absence is assertable
	// at top level. Sub links first: Row's by-value member needs Sub's real size, while
	// Sub's array inner tolerates an un-linked Row (see AddArrayOfStructMember). ----
	AddExpectedMessagePlain(
		TEXT("array property 'Sub.Rows'"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	TStrongObjectPtr<UUserDefinedStruct> RowStruct(NewUserStruct());
	TStrongObjectPtr<UUserDefinedStruct> SubStruct(NewUserStruct());
	AddIntMember(SubStruct.Get(), TEXT("SubKept"));
	AddArrayOfStructMember(SubStruct.Get(), TEXT("Rows"), RowStruct.Get());
	AddIntMember(RowStruct.Get(), TEXT("RowKept"));
	AddStructMember(RowStruct.Get(), TEXT("Sub"), SubStruct.Get());
	SubStruct->Bind();
	SubStruct->StaticLink(/*bRelinkExistingProperties=*/true);
	RowStruct->Bind();
	RowStruct->StaticLink(/*bRelinkExistingProperties=*/true);

	const FVaCuusModelLayout RowRoot(RowStruct.Get());
	TestTrue(TEXT("the row-rooted layout resolved its struct"), RowRoot.IsValid());
	TestNull(TEXT("the loop-closing array is refused"), RowRoot.FindField(TEXT("Sub.Rows")));
	TestNotNull(TEXT("the root scalar survives"), RowRoot.FindField(TEXT("RowKept")));
	TestNotNull(TEXT("and so does the nested one"), RowRoot.FindField(TEXT("Sub.SubKept")));
	TestEqual(TEXT("exactly the two scalars bind"), RowRoot.GetFields().Num(), 2);

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
