// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"

#include "VaCuusRefHudTestTypes.generated.h"

/**
 * The reference HUD model's TEST TWIN (M6 Task 4). The driver's own struct
 * (FVaCuusRefHudModel, VaCuusRender/Private/VaCuusRefHudModel.h) is unreachable
 * from this module -- VaCuusRender depends on VaCuus, never the reverse -- so the
 * RefHud tests bind this mirror instead. That is SAFE, not fragile: binding is
 * by FIELD NAME against the real RefHud/refhud.rml, so if the driver's shape and
 * the document ever drift from this mirror, the data-for panels come up empty and
 * the count test's [1,650, 1,850] window fails loudly -- the document is the
 * arbiter, and both structs answer to it.
 *
 * Field names are therefore refhud.rml's bindings, verbatim.
 */
USTRUCT()
struct FVaCuusRefHudTestRow
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "VaCuus")
	int32 Rank = 0;

	UPROPERTY(EditAnywhere, Category = "VaCuus")
	FString Name;

	UPROPERTY(EditAnywhere, Category = "VaCuus")
	int32 Kills = 0;

	UPROPERTY(EditAnywhere, Category = "VaCuus")
	int32 Deaths = 0;

	UPROPERTY(EditAnywhere, Category = "VaCuus")
	int32 Assists = 0;

	UPROPERTY(EditAnywhere, Category = "VaCuus")
	int32 Score = 0;

	UPROPERTY(EditAnywhere, Category = "VaCuus")
	int32 Ping = 0;
};

USTRUCT()
struct FVaCuusRefHudTestModel
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "VaCuus")
	TArray<FVaCuusRefHudTestRow> TeamAlpha;

	UPROPERTY(EditAnywhere, Category = "VaCuus")
	TArray<FVaCuusRefHudTestRow> TeamBravo;

	UPROPERTY(EditAnywhere, Category = "VaCuus")
	float Health = 100.f;

	UPROPERTY(EditAnywhere, Category = "VaCuus")
	float Mana = 100.f;

	UPROPERTY(EditAnywhere, Category = "VaCuus")
	int32 Ammo = 30;

	UPROPERTY(EditAnywhere, Category = "VaCuus")
	int32 AmmoReserve = 120;

	UPROPERTY(EditAnywhere, Category = "VaCuus")
	int32 Level = 17;

	UPROPERTY(EditAnywhere, Category = "VaCuus")
	FString PlayerName;

	UPROPERTY(EditAnywhere, Category = "VaCuus")
	FString Objective;
};
