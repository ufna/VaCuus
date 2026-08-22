// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"

#include "VaCuusTexDemoModel.generated.h"

/**
 * The engine-texture demo's model (spec 2026-08-22 §6): what `vacuus.TexDemo` drives into
 * DevUI/tex_demo.rml.
 *
 * WHY THE COUNTERS ARE ON SCREEN AT ALL. The demo's three tiles look identical whether the
 * refresh modes work or not — a live portrait and a frozen one are the same still image in a
 * screenshot, and a static icon that secretly republishes sixty times a second looks exactly
 * like one that costs nothing. The claim this demo exists to make is about COST, and cost has
 * no pixels: recorded-versus-published is the only way to read it, so it goes in the same
 * frame as the thing it describes (CLAUDE.md — an invariant with no observable will rot).
 *
 * In a header unconditionally, for the FVaCuusDemoModel reason: UnrealHeaderTool reflects .h
 * files only and does not consult the preprocessor.
 */
USTRUCT()
struct FVaCuusTexDemoModel
{
	GENERATED_BODY()

	/** UI frames the view has RECORDED — the clock the idle gate is measured against. */
	UPROPERTY(EditAnywhere, Category = "VaCuus")
	int32 Recorded = 0;

	/** Of those, the ones that reached the render thread. */
	UPROPERTY(EditAnywhere, Category = "VaCuus")
	int32 Published = 0;

	/** Recorded - Published: what the idle gate saved. */
	UPROPERTY(EditAnywhere, Category = "VaCuus")
	int32 Withheld = 0;

	/** MarkTextureDirty calls the minimap has made, so its 1 Hz beat is visible as a number. */
	UPROPERTY(EditAnywhere, Category = "VaCuus")
	int32 Refreshes = 0;

	/**
	 * Whether the portrait key is currently registered live.
	 *
	 * BOUND RATHER THAN WRITTEN INTO THE MARKUP, because the first version of this demo
	 * spelled the portrait's caption as static text and `vacuus.TexDemo.Live 0` left it
	 * reading LIVE next to a verdict line saying STATIC — a screenshot that contradicted
	 * itself. The document drives its class off this (data-class-live / data-class-static),
	 * so the caption cannot disagree with the registration again.
	 */
	UPROPERTY(EditAnywhere, Category = "VaCuus")
	bool PortraitLive = true;

	/** The caption text that goes with PortraitLive. */
	UPROPERTY(EditAnywhere, Category = "VaCuus")
	FString PortraitMode;

	/** One line naming what the numbers currently mean; see PumpTexDemoModel. */
	UPROPERTY(EditAnywhere, Category = "VaCuus")
	FString Verdict;
};
