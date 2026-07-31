// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"

#include "VaCuusJsValue.h"

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

/**
 * One killfeed line -- the M3b spec 1 row shape (3 FStrings + a bool), because that is the shape
 * the 200-row cost table was measured on and the shape a real shooter's feed actually has.
 * bHeadshot is a native bool and could not be anything else: a bitfield cannot exist inside a
 * container (the test fixture at VaCuusModelLayoutTestTypes.h:561-568 carries the argument).
 */
USTRUCT()
struct FVaCuusDemoKillfeedRow
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "VaCuus")
	FString Killer;

	UPROPERTY(EditAnywhere, Category = "VaCuus")
	FString Victim;

	UPROPERTY(EditAnywhere, Category = "VaCuus")
	FString Weapon;

	UPROPERTY(EditAnywhere, Category = "VaCuus")
	bool bHeadshot = false;
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

	/**
	 * The M3b array: struct rows through `data-for`, on the same model and the same one-call
	 * UpdateModel as everything above. The pump appends a row every ~1.5 s and TRIMS FROM THE
	 * FRONT above 6 rows -- deliberately the expensive direction (spec 3.6): removing element 0
	 * shifts every survivor, so each trim exercises the all-rows re-render path on screen,
	 * where an append-only feed would only ever exercise row creation.
	 *
	 * The M4 demo binds this same struct but leaves the field UNTOUCHED and unbound in its
	 * document: m4_demo.rml's killfeed is JS-built DOM (spec 9), and a second feed here would
	 * be a second truth.
	 */
	UPROPERTY(EditAnywhere, Category = "VaCuus")
	TArray<FVaCuusDemoKillfeedRow> Killfeed;
};

/**
 * The M4 demo's game-thread ear for routed document writes (M4 spec 3.10, plan Task 10.1):
 * `UVaCuusView::OnModelWrite` is a DYNAMIC multicast delegate, which binds only to a
 * UFUNCTION on a UObject -- the same constraint the router tests document
 * (VaCuusJsRouterTestTypes.h) -- and the demo driver is file-static code in
 * VaCuusRender.cpp. This class is the smallest possible adapter: one UFUNCTION forwarding
 * into a plain TFunction the demo installs at bind time, so the accepted-write logic stays
 * next to the driver it mutates.
 *
 * Game thread only, like the delegate that calls it; kept alive by the demo state's
 * TStrongObjectPtr for exactly as long as the binding exists.
 */
UCLASS()
class UVaCuusDemoWriteListener : public UObject
{
	GENERATED_BODY()

public:
	/** Installed by StartModelDriver, cleared by TearDown; never called after either. */
	TFunction<void(FName, const FString&, const FVaCuusJsValue&)> OnWrite;

	UFUNCTION()
	void HandleModelWrite(FName Model, const FString& Path, const FVaCuusJsValue& Value)
	{
		if (OnWrite)
		{
			OnWrite(Model, Path, Value);
		}
	}
};
