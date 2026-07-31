// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"

#include "VaCuusJsValue.h"
#include "VaCuusModelLayoutTestTypes.h"

#include "VaCuusJsRouterTestTypes.generated.h"

/*
 * FIXTURES FOR THE M4 TASK 9 ROUTER TESTS (VaCuusJsRouterTest.cpp). In a header and
 * unconditionally compiled for VaCuusModelLayoutTestTypes.h's reasons, verbatim: UHT
 * only parses headers, and it parses them without the automation guard's answer.
 */

/**
 * The router/read-surface model: one field per read-surface kind the Task 9 tests
 * assert, plus the two array shapes the write tests click through. Reuses the M3
 * fixtures' nested point, killfeed row and enum so the registry keeps sharing them.
 */
USTRUCT()
struct FVaCuusRouterTestModel
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	bool bPaused = false;

	UPROPERTY(EditAnywhere, Category = "Test")
	float Health = 0.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 Ammo = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	FString Title;

	UPROPERTY(EditAnywhere, Category = "Test")
	FName Tag;

	UPROPERTY(EditAnywhere, Category = "Test")
	FText Caption;

	UPROPERTY(EditAnywhere, Category = "Test")
	EVaCuusTestColour Colour = EVaCuusTestColour::Red;

	UPROPERTY(EditAnywhere, Category = "Test")
	TSoftObjectPtr<UObject> Icon;

	UPROPERTY(EditAnywhere, Category = "Test")
	FVaCuusTestPoint Origin;

	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<int32> Numbers;

	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<FVaCuusTestKillfeedRow> Killfeed;
};

/**
 * The game-thread ear: what OnModelWrite / OnJsEvent broadcast INTO, because a dynamic
 * multicast delegate binds only to UFUNCTIONs on a UObject. Captures are plain members
 * -- everything here runs on the game thread, drain included.
 */
UCLASS()
class UVaCuusRouterTestListener : public UObject
{
	GENERATED_BODY()

public:
	struct FWrite
	{
		FName Model;
		FString Path;
		FVaCuusJsValue Value;
	};

	struct FEvent
	{
		FName Name;
		TArray<FVaCuusJsKeyValue> Payload;
	};

	UFUNCTION()
	void HandleModelWrite(FName Model, const FString& Path, const FVaCuusJsValue& Value)
	{
		Writes.Add(FWrite{Model, Path, Value});
	}

	UFUNCTION()
	void HandleJsEvent(FName Name, const TArray<FVaCuusJsKeyValue>& Payload)
	{
		Events.Add(FEvent{Name, Payload});
	}

	TArray<FWrite> Writes;
	TArray<FEvent> Events;
};
