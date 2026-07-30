// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusModelLayout.h"
#include "VaCuusModelLayoutTestTypes.h"
#include "VaCuusModelShadow.h"

#include "StructUtils/UserDefinedStruct.h"
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
 * THE REFUSALS. Every kind M3a does not carry is skipped with a diagnostic naming the
 * property and the reason -- never silently. The failure this guards against is a
 * designer writing {{Numbers}} and getting an inert document with nothing anywhere
 * saying why, which is the milestone's signature failure mode.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelLayoutRefusalsTest, "VaCuus.Model.LayoutRefusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelLayoutRefusalsTest::RunTest(const FString& Parameters)
{
	// REGISTERED, NOT MERELY TOLERATED. An unmatched expectation fails the test
	// (AutomationTest.cpp:1376 ANDs in HasMetExpectedMessages), so each line below is the
	// assertion that this refusal was diagnosed -- exactly once, at Warning, naming the
	// property. Without them "skipped it" and "skipped it silently" are the same result.
	for (const TCHAR* Refused : {TEXT("Numbers"), TEXT("Lookup"), TEXT("Names"), TEXT("Owner"), TEXT("Fixed")})
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

	TestNull(TEXT("TArray is not bound (M3b)"), Layout.FindField(TEXT("Numbers")));
	TestNull(TEXT("TMap is not bound"), Layout.FindField(TEXT("Lookup")));
	TestNull(TEXT("TSet is not bound"), Layout.FindField(TEXT("Names")));
	TestNull(TEXT("a hard object reference is not bound"), Layout.FindField(TEXT("Owner")));
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

#endif	  // WITH_DEV_AUTOMATION_TESTS
