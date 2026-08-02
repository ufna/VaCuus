// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"

#include "VaCuusRefHudModel.generated.h"

/**
 * The reference HUD's data model (M6 Task 4, spec 2(g)/(h)): what `vacuus.RefHud`
 * binds as model 'refhud' and drives every frame into RefHud/refhud.rml.
 *
 * THE TWO ARRAYS ARE THE DESIGN, NOT A CONVENIENCE (spec 2(h)): RmlUi dirties
 * TOP-LEVEL names only (DataModel.cpp:325-331), so TeamAlpha and TeamBravo are
 * independent dirty scopes -- a stat bump in one panel re-evaluates only that
 * panel's 24 x 8 bindings (~0.10 ms at the measured ~0.53 us/binding), where a
 * single 48-row array would re-evaluate all 384 on any change. The automation
 * twin (VaCuus.RefHud.DirtyScope) proves the independence by exact eval-counter
 * deltas against a test-local mirror of this shape.
 *
 * In a header unconditionally for the FVaCuusDemoModel reason: UnrealHeaderTool
 * reflects .h files only, and it does not consult the preprocessor.
 */
USTRUCT()
struct FVaCuusRefHudScoreRow
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

	/** Also the row's GEOMETRY binding: the ping meter's data-style-width reads it. */
	UPROPERTY(EditAnywhere, Category = "VaCuus")
	int32 Ping = 0;
};

USTRUCT()
struct FVaCuusRefHudModel
{
	GENERATED_BODY()

	/** Panel A's 24 rows -- its own top-level name, its own dirty scope. */
	UPROPERTY(EditAnywhere, Category = "VaCuus")
	TArray<FVaCuusRefHudScoreRow> TeamAlpha;

	/** Panel B's 24 rows -- the OTHER dirty scope. */
	UPROPERTY(EditAnywhere, Category = "VaCuus")
	TArray<FVaCuusRefHudScoreRow> TeamBravo;

	/** Plate bar: text and data-style-width geometry both. */
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
