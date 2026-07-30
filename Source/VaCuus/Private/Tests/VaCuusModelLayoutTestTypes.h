// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
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
 * eight small structs.
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

	/** M3b, not a gap. */
	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<int32> Numbers;

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

/** A nested struct with no exposed member: contributes nothing, and must say so. */
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
