// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"

#include "VaCuusModelLayoutTestTypes.generated.h"

/*
 * FIXTURE TYPES FOR VaCuus.Model.Layout.
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
 * Offset_Internal points at its storage integer and GetElementSize() is that integer's
 * size (UnrealType.h:2595-2603), so bBitfieldBool and bBitfieldTwo share BOTH: anything
 * keyed on (offset, size) aliases them, and only a pair makes that observable. The
 * native bool is the control -- FieldMask 255 and ByteOffset 0 rather than an isolated
 * bit (PropertyBool.cpp:80-91).
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
