// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
// CoreMinimal only FORWARD-declares FAnsiString/FUtf8String (ContainersFwd.h:24); the
// members below need the complete types. Surfaced by the M6 BuildPlugin -StrictIncludes
// leg — unity/PCH had been supplying them by accident.
#include "Containers/AnsiString.h"
#include "Containers/Utf8String.h"
#include "UObject/ObjectMacros.h"

#include "VaCuusModelLayoutTestTypes.generated.h"

/*
 * FIXTURE TYPES FOR THE VaCuus.Model.* TESTS -- layout, shadow, channel and sampler.
 *
 * IN A HEADER, NOT NEXT TO THE TEST, because UnrealHeaderTool only parses .h files --
 * a USTRUCT in a .cpp is never reflected and the test would have nothing to walk. In
 * Private/ because nothing outside the test uses them, and UHT parses a module's
 * Private/ tree as readily as its Public/ one (Runtime/Engine/Private/Tests/
 * PieFixupTestObjects.h is the engine doing the same thing).
 *
 * UNCONDITIONALLY COMPILED, not wrapped in WITH_DEV_AUTOMATION_TESTS: UHT parses this
 * header without the preprocessor's answer, so a guarded USTRUCT emits reflection code
 * that then fails to compile in the configurations where the guard is off. They are
 * small structs.
 */

/** One value of each shape a native enum can take, for the Enum field kind. */
UENUM()
enum class EVaCuusTestColour : uint8
{
	Red,
	Green,
	Blue
};

/**
 * Nested by FVaCuusLayoutTestModel. Two leaves, so a test can tell "flattened the
 * nested struct" from "emitted one entry for the struct itself".
 */
USTRUCT()
struct FVaCuusTestPoint
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	float X = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float Y = 0.f;
};

/**
 * The supported set, one field per kind, in declaration order.
 *
 * There are TWO adjacent bitfields and one native bool, on purpose. A bitfield's
 * Offset_Internal is the offset of the BYTE its bit landed in -- DetermineBitfieldOffsetAndMask
 * sets the bit in a scratch instance and scans for it (PropertyBool.cpp:98-117), and the
 * result goes straight to SetOffset_Internal (:40) -- while ElementSize comes from the
 * declared type (:41). So bBitfieldBool and bBitfieldTwo share BOTH: anything keyed on
 * (offset, size) aliases them, and only a pair makes that observable. The native bool is the
 * control -- FieldMask 255 and ByteOffset 0 rather than an isolated bit (PropertyBool.cpp:76-93).
 *
 * Utf8Str and AnsiStr are EditAnywhere rather than BlueprintReadWrite because they
 * CANNOT be Blueprint-visible: UhtUtf8StrProperty.cs:46-47 and UhtAnsiStrProperty.cs
 * leave IsMemberSupportedByBlueprint commented out of their PropertyCaps. They are the
 * concrete reason the exposure rule is `CPF_BlueprintVisible || CPF_Edit` and not
 * BlueprintVisible alone.
 */
USTRUCT()
struct FVaCuusLayoutTestModel
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	float Ratio = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 Score = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	uint8 Level = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	bool bNativeBool = false;

	UPROPERTY(EditAnywhere, Category = "Test")
	uint8 bBitfieldBool : 1;

	UPROPERTY(EditAnywhere, Category = "Test")
	uint8 bBitfieldTwo : 1;

	UPROPERTY(EditAnywhere, Category = "Test")
	FString Title;

	UPROPERTY(EditAnywhere, Category = "Test")
	FName Tag;

	UPROPERTY(EditAnywhere, Category = "Test")
	FText Caption;

	UPROPERTY(EditAnywhere, Category = "Test")
	FUtf8String Utf8Note;

	UPROPERTY(EditAnywhere, Category = "Test")
	FAnsiString AnsiNote;

	UPROPERTY(EditAnywhere, Category = "Test")
	EVaCuusTestColour Colour = EVaCuusTestColour::Red;

	UPROPERTY(EditAnywhere, Category = "Test")
	TSoftObjectPtr<UObject> Icon;

	UPROPERTY(EditAnywhere, Category = "Test")
	FVaCuusTestPoint Origin;

	FVaCuusLayoutTestModel()
		: bBitfieldBool(0)
		, bBitfieldTwo(0)
	{
	}
};

/** Every kind the layout must refuse, one per reason. */
USTRUCT()
struct FVaCuusLayoutTestRefusedModel
{
	GENERATED_BODY()

	/** Bound, so a test can prove the refusals below did not take the good field with them. */
	UPROPERTY(EditAnywhere, Category = "Test")
	int32 Kept = 0;

	/** No RmlUi map view, and FMapProperty::Identical is O(n^2). */
	UPROPERTY(EditAnywhere, Category = "Test")
	TMap<FName, int32> Lookup;

	UPROPERTY(EditAnywhere, Category = "Test")
	TSet<FName> Names;

	/** Hard reference: the shadow buffer is invisible to GC (spec 3.1). */
	UPROPERTY(EditAnywhere, Category = "Test")
	TObjectPtr<UObject> Owner;

	/**
	 * Weak reference: an index/serial pair with no path in it, so the UI thread would have
	 * to RESOLVE it to render anything -- and WeakObjectPtr.h:295-296 says a weak pointer
	 * cannot be tested from another thread. A soft reference is the supported form because
	 * it already carries its path.
	 */
	UPROPERTY(EditAnywhere, Category = "Test")
	TWeakObjectPtr<UObject> Watcher;

	/** Fixed-size C array: one FProperty with ArrayDim 4, and only element 0 is addressable. */
	UPROPERTY(EditAnywhere, Category = "Test")
	int32 Fixed[4];

	/** Exposed to neither Blueprint nor the details panel: opted out, not unsupported. */
	UPROPERTY()
	int32 Hidden = 0;

	FVaCuusLayoutTestRefusedModel()
	{
		FMemory::Memzero(Fixed);
	}
};

/** Two RmlUi-illegal names and one legal control (spec 3.3). */
USTRUCT()
struct FVaCuusLayoutTestIllegalNameModel
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 Legal = 0;

	/** Reserved in RmlUi, checked case-insensitively -- and about as ordinary as UE gets. */
	UPROPERTY(EditAnywhere, Category = "Test")
	int32 Size = 0;

	/** Also reserved. */
	UPROPERTY(EditAnywhere, Category = "Test")
	int32 Literal = 0;
};

/**
 * A nested member whose name is reserved at top level but is NOT a top-level name here.
 * RmlUi applies LegalVariableName only in BindVariable, i.e. only to the name a variable
 * is registered under (DataModel.cpp:119-124); `{{Panel.Size}}` resolves through
 * DataVariable::Child and never meets that check.
 */
USTRUCT()
struct FVaCuusLayoutTestNestedReservedInner
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 Size = 0;
};

USTRUCT()
struct FVaCuusLayoutTestNestedReservedModel
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	FVaCuusLayoutTestNestedReservedInner Panel;
};

/**
 * Construction/destruction counters for FVaCuusShadowProbeModel below.
 *
 * THE INVARIANT THIS EXISTS TO MAKE OBSERVABLE: FVaCuusModelShadow really runs
 * UScriptStruct::InitializeStruct and UScriptStruct::DestroyStruct on its buffer. Without
 * a counter the only symptom of a missing DestroyStruct is a leak of every FString /
 * FText / FSoftObjectPtr in the model -- no crash, no log, and nothing an automation test
 * can see. `inline` at namespace scope so the header needs no .cpp of its own.
 *
 * Read as DELTAS, never as absolutes: the reflection system is free to construct a
 * throwaway instance of any type while linking it, so the count at test entry is not
 * knowable.
 */
namespace VaCuusShadowProbe
{
inline int32 NumConstructed = 0;
inline int32 NumDestructed = 0;
}	 // namespace VaCuusShadowProbe

/**
 * The shadow-buffer fixture: counted construction and destruction, a non-zero default, a
 * heap-owning member and a nested struct.
 *
 * Ratio's 0.5f default is the "it is a real instance, not a memzeroed block" assertion:
 * UScriptStruct::InitializeStruct memzeroes and THEN calls the C++ constructor
 * (Class.cpp:3783, :3798), so a buffer that was only zeroed reads 0.f here.
 *
 * The explicit copy constructor exists only so a copy is counted too -- an implicitly
 * generated one would leave NumConstructed behind and make the destruction balance look
 * wrong for a reason that has nothing to do with the shadow.
 */
USTRUCT()
struct FVaCuusShadowProbeModel
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	float Ratio = 0.5f;

	UPROPERTY(EditAnywhere, Category = "Test")
	FString Title;

	UPROPERTY(EditAnywhere, Category = "Test")
	FVaCuusTestPoint Origin;

	FVaCuusShadowProbeModel() { ++VaCuusShadowProbe::NumConstructed; }

	FVaCuusShadowProbeModel(const FVaCuusShadowProbeModel& Other)
		: Ratio(Other.Ratio)
		, Title(Other.Title)
		, Origin(Other.Origin)
	{
		++VaCuusShadowProbe::NumConstructed;
	}

	FVaCuusShadowProbeModel& operator=(const FVaCuusShadowProbeModel&) = default;

	~FVaCuusShadowProbeModel() { ++VaCuusShadowProbe::NumDestructed; }
};

/**
 * A struct with no exposed member, worn two ways: NESTED it contributes nothing and must
 * say so (the empty-nested test); as an array ROW TYPE it refuses the whole array field
 * (FVaCuusArrayRefusalModel::BarrenRows). One type for both because the two diagnostics
 * must stay distinguishable for the same input.
 */
USTRUCT()
struct FVaCuusLayoutTestEmptyInner
{
	GENERATED_BODY()

	UPROPERTY()
	int32 NotExposed = 0;
};

USTRUCT()
struct FVaCuusLayoutTestEmptyNestedModel
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	FVaCuusLayoutTestEmptyInner Empty;

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 Kept = 0;
};

/**
 * DEFAULTS THAT DIFFER FROM ZERO, for spec 4's invariant I1.
 *
 * Every field's default is something a memzeroed buffer would not produce, and the test
 * hands the sampler a DEFAULT-CONSTRUCTED live instance -- so the differ correctly concludes
 * that nothing has changed and marks nothing. What reaches the UI on frame 1 is then decided
 * entirely by whether the channel forces a full first publish.
 *
 * bFlagged is a bitfield rather than a second native bool so that I1 is proven for the one
 * kind whose "unchanged" is a masked read rather than a byte compare.
 */
USTRUCT()
struct FVaCuusSamplerDefaultsModel
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	float Health = 100.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 Ammo = 30;

	UPROPERTY(EditAnywhere, Category = "Test")
	bool bAlive = true;

	UPROPERTY(EditAnywhere, Category = "Test")
	uint8 bFlagged : 1;

	UPROPERTY(EditAnywhere, Category = "Test")
	FString Title;

	UPROPERTY(EditAnywhere, Category = "Test")
	FName Tag;

	UPROPERTY(EditAnywhere, Category = "Test")
	FText Caption;

	UPROPERTY(EditAnywhere, Category = "Test")
	EVaCuusTestColour Colour = EVaCuusTestColour::Blue;

	FVaCuusSamplerDefaultsModel()
		: bFlagged(1)
		, Title(TEXT("Ready"))
		, Tag(TEXT("hp"))
		, Caption(FText::FromString(TEXT("Ready")))
	{
	}
};

/**
 * SIXTY-FOUR SCALAR FIELDS, for the spec 9 budget row ("game-thread sample + diff, 64
 * scalar fields <= 0.02 ms").
 *
 * A MIX RATHER THAN 64 FLOATS, because the number that matters is a frame's, and the kinds
 * do not cost the same: a float diff is two virtual accessor calls and a 64-bit compare,
 * while an FString is a length check plus a Strcmp and an FText adds a Rebuild() call on
 * each side. Sixty-four floats would measure a struct nobody writes.
 *
 * The proportions are a plausible HUD: mostly numbers, a few labels, two localised strings.
 */
USTRUCT()
struct FVaCuusSamplerCostModel
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	float F00 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F01 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F02 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F03 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F04 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F05 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F06 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F07 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F08 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F09 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F10 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F11 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F12 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F13 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F14 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F15 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F16 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F17 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F18 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F19 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F20 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F21 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F22 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F23 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F24 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F25 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F26 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F27 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F28 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F29 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F30 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float F31 = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 I00 = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 I01 = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 I02 = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 I03 = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 I04 = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 I05 = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 I06 = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 I07 = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 I08 = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 I09 = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 I10 = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 I11 = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 I12 = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 I13 = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 I14 = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 I15 = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	bool bNative0 = false;

	UPROPERTY(EditAnywhere, Category = "Test")
	bool bNative1 = false;

	UPROPERTY(EditAnywhere, Category = "Test")
	bool bNative2 = false;

	UPROPERTY(EditAnywhere, Category = "Test")
	bool bNative3 = false;

	UPROPERTY(EditAnywhere, Category = "Test")
	uint8 bBit0 : 1;

	UPROPERTY(EditAnywhere, Category = "Test")
	uint8 bBit1 : 1;

	UPROPERTY(EditAnywhere, Category = "Test")
	uint8 bBit2 : 1;

	UPROPERTY(EditAnywhere, Category = "Test")
	uint8 bBit3 : 1;

	UPROPERTY(EditAnywhere, Category = "Test")
	FString S0;

	UPROPERTY(EditAnywhere, Category = "Test")
	FString S1;

	UPROPERTY(EditAnywhere, Category = "Test")
	FString S2;

	UPROPERTY(EditAnywhere, Category = "Test")
	FString S3;

	UPROPERTY(EditAnywhere, Category = "Test")
	FName N0;

	UPROPERTY(EditAnywhere, Category = "Test")
	FName N1;

	UPROPERTY(EditAnywhere, Category = "Test")
	FText T0;

	UPROPERTY(EditAnywhere, Category = "Test")
	FText T1;

	FVaCuusSamplerCostModel()
		: bBit0(0)
		, bBit1(0)
		, bBit2(0)
		, bBit3(0)
	{
	}
};

/**
 * One row of the M3b killfeed shape (spec 1): strings, a bool and a nested struct, so a
 * struct element exercises flattening INSIDE the element layout.
 *
 * bHeadshot is a native bool and could not be anything else: a bitfield cannot exist
 * inside a container, and UHT refuses bool static arrays outright (UhtProperty.cs:
 * 2395-2398) -- which is why an element compare never needs the mask-aware read (spec 2(e)).
 */
USTRUCT()
struct FVaCuusTestKillfeedRow
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	FString Killer;

	UPROPERTY(EditAnywhere, Category = "Test")
	FString Victim;

	UPROPERTY(EditAnywhere, Category = "Test")
	bool bHeadshot = false;

	UPROPERTY(EditAnywhere, Category = "Test")
	FVaCuusTestPoint Impact;
};

/**
 * An array INSIDE a nested struct (the Panel.Items shape, spec 3.1): the array leaf is
 * legal anywhere a leaf is, and must compose ContainerOffset like any other nested leaf.
 */
USTRUCT()
struct FVaCuusTestArrayPanel
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<int32> Items;
};

/**
 * The supported array shapes, one field per element category (spec 3.2), plus a scalar
 * control so exactly-one-bit assertions can catch a neighbour marked by mistake.
 */
USTRUCT()
struct FVaCuusArrayTestModel
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 Scalar = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<int32> Numbers;

	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<FString> Labels;

	/** double, not float: the NaN element case asserts on the exact stored bit pattern. */
	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<double> Ratios;

	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<FVaCuusTestKillfeedRow> Killfeed;

	UPROPERTY(EditAnywhere, Category = "Test")
	FVaCuusTestArrayPanel Panel;
};

/**
 * A row whose NESTED struct member is named `Size` -- the one spelling of an element-level
 * "size" the shipped name rules can express, and the fixture behind the adapter's
 * size-collision tests (VaCuus.Model.ArrayBinding).
 *
 * WHY THE NESTING IS LOAD-BEARING AND NOT DECORATION. Spec 3.6 promises "an element member
 * named `size` is reachable -- Arr[0].size routes the name to the element struct's Child,
 * not the array's". The ROUTING half is true and tested (the element struct's own
 * member-miss Warning fires for Arr[0].size, so the array's special case never saw the
 * name). But under the shared-layout rule an element TOP-LEVEL member named Size cannot
 * exist -- element top-level names obey the full root rule, and `size` is RmlUi-reserved
 * (spec 3.1, VaCuusWireName::ValidateTopLevel) -- so nothing is reachable AT Arr[0].size.
 * The reachable spelling is one level down, Arr[0].Panel.Size, because ValidateNested
 * applies no reserved-word rule (VaCuusModelLayout.h's two-rules comment). The adapter
 * test narrows spec 3.6's sentence to exactly that.
 */
USTRUCT()
struct FVaCuusTestSizeNameRow
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	FString Kept;

	UPROPERTY(EditAnywhere, Category = "Test")
	FVaCuusLayoutTestNestedReservedInner Panel;
};

/**
 * The RmlUi adapter tests' model (VaCuus.Model.ArrayBinding / .ArrayStateless). A separate
 * type rather than more fields on FVaCuusArrayTestModel, on purpose: the sampler and
 * channel suites treat that model's exact field mix as their contract, and the adapter
 * needs shapes they do not -- the nested-`Size` row above, and TWO scalar arrays so the
 * statelessness test can give its second view rows over one array while it deliberately
 * never calls Size() on the other (the cached-Num restore-the-bug needs a view whose own
 * update cannot refresh the poison). Killfeed reuses FVaCuusTestKillfeedRow so the
 * registry-sharing assertion -- two models, one row type, one definition set -- has a
 * second model to share with.
 */
USTRUCT()
struct FVaCuusArrayBindModel
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 Scalar = 7;

	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<int32> Numbers;

	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<FString> Labels;

	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<FVaCuusTestKillfeedRow> Killfeed;

	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<FVaCuusTestSizeNameRow> SizeRows;

	UPROPERTY(EditAnywhere, Category = "Test")
	FVaCuusTestArrayPanel Panel;
};

/** A row with an FText member: the WHOLE array field is refused at desc build (spec 3.1). */
USTRUCT()
struct FVaCuusTestTextRow
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	FText Label;

	UPROPERTY(EditAnywhere, Category = "Test")
	FString Kept;
};

/** A row with a TArray member: a nested container anywhere in the element subtree refuses the whole array field. */
USTRUCT()
struct FVaCuusTestNestedArrayRow
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<int32> Inner;

	UPROPERTY(EditAnywhere, Category = "Test")
	FString Kept;
};

/**
 * A row whose member is named `Size`, reserved at ROOT level. Element top-level member
 * names obey the FULL root rule because element layouts are plain shared layouts (spec
 * 3.1's stated price): the member is refused with the root Error, the row's other member
 * binds, and the array field itself survives.
 */
USTRUCT()
struct FVaCuusTestReservedNameRow
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 Size = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	FString Kept;
};

/**
 * A row with a TMap member AND a TSet member: the shared classifier drops each MEMBER,
 * exactly as it would on a model root, and the array binds without them -- unlike a nested
 * TArray, which refuses the whole field (spec 3.2: an inner array's cost would hide under
 * the one dirty bit; a map or set was never bindable to begin with). Both container kinds
 * are here because they are refused by two different classifier branches, and only a pair
 * proves the deviation holds for each.
 */
USTRUCT()
struct FVaCuusTestMapRow
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	TMap<FName, int32> Lookup;

	UPROPERTY(EditAnywhere, Category = "Test")
	TSet<FName> Tags;

	UPROPERTY(EditAnywhere, Category = "Test")
	FString Kept;
};

/** The array-specific refusal cases, plus a survivor to prove each refusal takes only its own field. */
USTRUCT()
struct FVaCuusArrayRefusalModel
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 Kept = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<FText> Texts;

	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<FVaCuusTestTextRow> TextRows;

	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<FVaCuusTestNestedArrayRow> NestedRows;

	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<FVaCuusTestReservedNameRow> ReservedRows;

	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<FVaCuusTestMapRow> MapRows;

	/**
	 * A row type with NO bindable member (the same struct the empty-nested test uses):
	 * with zero element leaves only Num() would ever be observable, so the whole array
	 * field is refused with a Warning naming THIS property -- and the element build stays
	 * silent about it, so the misleading root-flavored "no property could be bound" line
	 * never fires here.
	 */
	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<FVaCuusLayoutTestEmptyInner> BarrenRows;
};

/**
 * SPEC 9'S MEASURED SHAPE, ONE ROW: 4 fields, 3 FString + 1 bool -- "the killfeed shape".
 * The fixture behind VaCuus.Model.DataFor* (Task 5's end-to-end and the triple-idle window)
 * and REUSED by Task 6's cost harnesses, so spec 9's numbers are taken over exactly the
 * fixture the idle gate was proven exact on.
 */
USTRUCT()
struct FVaCuusCostKillfeedRow
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	FString Killer;

	UPROPERTY(EditAnywhere, Category = "Test")
	FString Victim;

	UPROPERTY(EditAnywhere, Category = "Test")
	FString Weapon;

	UPROPERTY(EditAnywhere, Category = "Test")
	bool bHeadshot = false;
};

/**
 * ONE array field and nothing beside it: what the 200-row measurements bind and diff. A
 * scalar control field would put a second compare inside every timed window for a neighbour
 * the killfeed rows do not have; the exactly-one-bit assertions that need a control already
 * have one in FVaCuusArrayTestModel.
 */
USTRUCT()
struct FVaCuusCostFeedModel
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<FVaCuusCostKillfeedRow> Killfeed;
};

/**
 * Deterministic row content, so every test (and Task 6's harnesses) can recompute an
 * expected row from its index alone -- which is what makes the front-trim's SHIFTED
 * expectation writable without recording anything. `inline` at namespace scope so the
 * header needs no .cpp, like VaCuusShadowProbe above.
 */
namespace VaCuusKillfeedFixture
{
inline FVaCuusCostKillfeedRow MakeRow(int32 Index)
{
	FVaCuusCostKillfeedRow Row;
	Row.Killer = FString::Printf(TEXT("Killer%03d"), Index);
	Row.Victim = FString::Printf(TEXT("Victim%03d"), Index);
	Row.Weapon = FString::Printf(TEXT("Weapon%03d"), Index);
	Row.bHeadshot = (Index % 2) == 1;
	return Row;
}

inline void Fill(FVaCuusCostFeedModel& Model, int32 NumRows)
{
	Model.Killfeed.Reset(NumRows);
	for (int32 Index = 0; Index < NumRows; ++Index)
	{
		Model.Killfeed.Add(MakeRow(Index));
	}
}
}	 // namespace VaCuusKillfeedFixture

/**
 * THE RESERVED-NAME COLLISION FIXTURE. `t` is the top-level name the live translation route
 * binds into every model (VaCuusTranslationVariable.h), so a struct that declares a field of
 * exactly that spelling must be refused for that ONE field and bound for every other.
 *
 * The lowercase spelling is the whole point and not a style slip: RmlUi resolves data addresses
 * byte-for-byte, so `T` would be a different variable and would not collide at all -- which is
 * itself asserted, through Title, by the test that uses this.
 */
USTRUCT()
struct FVaCuusReservedNameModel
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	FString t;

	UPROPERTY(EditAnywhere, Category = "Test")
	FString Title;
};

/*
 * THERE IS NO NATIVE CONTAINER-CYCLE FIXTURE, AND THAT IS UHT'S DOING, NOT A GAP. The
 * direct shape (TArray<FSelf> inside FSelf) is refused outright
 * (UhtArrayProperty.cs:216-222), and every indirect shape -- the mutual pair
 * FA{TArray<FB>}/FB{TArray<FA>}, the by-value hop FRow{FSub}/FSub{TArray<FRow>} -- needs a
 * forward reference to a type defined later, which UHT's code-generation hash refuses:
 * "references type ... but the code generation hash is zero" (UhtProperty.cs:3066-3071).
 * The cycle is still CONSTRUCTIBLE at the FProperty level, where nothing validates the
 * graph -- a hand-built UUserDefinedStruct pair proves it -- so the layout's cycle guard is
 * tested from exactly there: see FVaCuusModelLayoutCycleTest, which builds both shapes the
 * way FVaCuusModelLayoutDuplicateNameTest builds its structs.
 */
