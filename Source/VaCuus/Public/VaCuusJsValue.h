// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"

#include "VaCuusJsValue.generated.h"

/**
 * What a routed document value IS (M4 Task 9, spec 3.10).
 *
 * THREE VALUE KINDS PLUS NULL, AND THE SET IS THE WIRE'S, NOT OURS. Everything that can
 * arrive at a routed write is an Rml::Variant produced by RmlUi's own controllers -- a
 * checkbox's change event carries a bool ("data-binding-override-value",
 * InputTypeCheckbox.cpp:30), a text input's a string, and a data-event assignment's
 * r-value comes out of the expression interpreter, which computes in bool/double/string
 * (DataExpression.cpp:1007-1016). The same three kinds are exactly what JavaScript
 * primitives coerce to (vacuus.emit payload values). Null is the read surface's miss
 * answer, never a routed write's payload.
 */
UENUM(BlueprintType)
enum class EVaCuusJsValueKind : uint8
{
	Null,
	Bool,
	Number,
	String,
};

/**
 * One routed value: a tagged union in USTRUCT clothing, because a real union cannot
 * cross a dynamic delegate. Only the member the Kind names is meaningful; the others
 * hold their defaults. Blueprint reads the Kind and branches -- the same shape an
 * FVaCuusModelUpdate's dirty bits impose one layer down ("only the fields whose bit is
 * set are current").
 */
USTRUCT(BlueprintType)
struct VACUUS_API FVaCuusJsValue
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "VaCuus")
	EVaCuusJsValueKind Kind = EVaCuusJsValueKind::Null;

	UPROPERTY(BlueprintReadOnly, Category = "VaCuus")
	bool bBool = false;

	UPROPERTY(BlueprintReadOnly, Category = "VaCuus")
	double Number = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "VaCuus")
	FString String;

	static FVaCuusJsValue MakeNull()
	{
		return FVaCuusJsValue();
	}

	static FVaCuusJsValue MakeBool(bool bInValue)
	{
		FVaCuusJsValue Value;
		Value.Kind = EVaCuusJsValueKind::Bool;
		Value.bBool = bInValue;
		return Value;
	}

	static FVaCuusJsValue MakeNumber(double InValue)
	{
		FVaCuusJsValue Value;
		Value.Kind = EVaCuusJsValueKind::Number;
		Value.Number = InValue;
		return Value;
	}

	static FVaCuusJsValue MakeString(FString InValue)
	{
		FVaCuusJsValue Value;
		Value.Kind = EVaCuusJsValueKind::String;
		Value.String = MoveTemp(InValue);
		return Value;
	}
};

/** One `vacuus.emit` payload entry. An array of these, not a TMap: dynamic delegates cannot carry a TMap parameter. */
USTRUCT(BlueprintType)
struct VACUUS_API FVaCuusJsKeyValue
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "VaCuus")
	FString Key;

	UPROPERTY(BlueprintReadOnly, Category = "VaCuus")
	FVaCuusJsValue Value;
};
