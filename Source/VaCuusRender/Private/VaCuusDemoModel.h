// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"

#include "VaCuusDemoModel.generated.h"

/**
 * The M3a acceptance demo's model (spec 9, plan Task 9.1): one USTRUCT carrying one property of
 * every kind M3a binds, driven per frame by `vacuus.M3Demo` into DevUI/m3_demo.rml.
 *
 * WHY IT IS THE WHOLE TYPE TABLE AND NOT TWO FIELDS. The acceptance question is not "does a
 * float reach the screen" -- VaCuus.Model.View.Idle already answers that through a real
 * Rml::Context. It is whether every kind the layout claims to carry survives the whole pipeline
 * (diff rule -> channel -> UI shadow -> RmlUi variant -> DOM), and each of the sharp ones has a
 * plausible wrong implementation that a float-only demo would pass: FText compares by identity
 * in a cooked build, FName by comparison index, an enum ships its NAME rather than its number,
 * and a nested struct's leaf dirties its ROOT's variable rather than its own. One screenshot
 * with all of them on it is the cheapest possible check that none of those broke.
 *
 * IN A HEADER, AND UNCONDITIONALLY: UnrealHeaderTool parses .h files only -- a USTRUCT in a .cpp
 * is never reflected -- and it emits reflection code without consulting the preprocessor, so
 * wrapping this in a build-configuration guard would break the generated code rather than
 * exclude it. Private/ is enough; nothing outside this module names these types.
 */
UENUM()
enum class EVaCuusDemoStance : uint8
{
	Standing,
	Crouched,
	Prone
};

/** The nested half, so the demo exercises a flattened leaf and its dotted wire name. */
USTRUCT()
struct FVaCuusDemoTarget
{
	GENERATED_BODY()

	/** Wire name `Target.Name`; dirties the top-level variable `Target`, never `Name`. */
	UPROPERTY(EditAnywhere, Category = "VaCuus")
	FString Designation;

	UPROPERTY(EditAnywhere, Category = "VaCuus")
	int32 Distance = 0;
};

USTRUCT()
struct FVaCuusDemoModel
{
	GENERATED_BODY()

	/** String -- shipped byte for byte, and diffed case-SENSITIVELY (spec 5). */
	UPROPERTY(EditAnywhere, Category = "VaCuus")
	FString CallSign;

	/**
	 * FloatingPoint. Also the one field the document turns into GEOMETRY rather than text:
	 * `data-style-width="Health + '%'"` resizes the bar, so a stuck value is visible at a glance
	 * on a screenshot in a way a wrong number is not.
	 */
	UPROPERTY(EditAnywhere, Category = "VaCuus")
	float Health = 100.f;

	/** SignedInt. */
	UPROPERTY(EditAnywhere, Category = "VaCuus")
	int32 Ammo = 30;

	/** SignedInt, and the demo's liveness proof: it counts published updates, not frames. */
	UPROPERTY(EditAnywhere, Category = "VaCuus")
	int32 Tick = 0;

	/** Bool, driven onto a class through `data-class-alert`. */
	UPROPERTY(EditAnywhere, Category = "VaCuus")
	bool bAlert = false;

	/** Enum -- ships the AUTHORED NAME, which is why the document prints a word and not a number. */
	UPROPERTY(EditAnywhere, Category = "VaCuus")
	EVaCuusDemoStance Stance = EVaCuusDemoStance::Standing;

	/** Name -- diffed by display index so a case change is not lost (spec 5). */
	UPROPERTY(EditAnywhere, Category = "VaCuus")
	FName Zone;

	/** Text -- shadowed and diffed as its DISPLAY STRING, frozen culture-invariant at sample time. */
	UPROPERTY(EditAnywhere, Category = "VaCuus")
	FText Objective;

	/** The nested struct: two leaves, one top-level name. */
	UPROPERTY(EditAnywhere, Category = "VaCuus")
	FVaCuusDemoTarget Target;
};
