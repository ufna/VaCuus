// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"

#include "VaCuusDataStyleIdleTestTypes.generated.h"

/*
 * FIXTURE TYPE FOR VaCuus.Model.View.DataStyleIdle (bead VaCuus-akj.17).
 *
 * THE NESTING IS THE WHOLE POINT, not tidiness. FVaCuusBoundModel::ApplyUpdate dirties a
 * nested leaf's ROOT variable, never the leaf (VaCuusBoundModel.cpp:379-387, and the same
 * rule is why FVaCuusDemoTarget exists), and RmlUi maps dirty variables to views by the FIRST
 * name of each address (DataExpression.cpp:1144-1153). So `Tick` moving re-runs every view
 * that mentions `Bar` — including the data-style bindings whose values did not move. That is
 * the coarse re-evaluation the perf guide already documents at panel scale ("a one-panel
 * change re-evaluates 192 bindings"), reduced to three fields.
 *
 * A FLAT model would prove nothing here: the differ dirties a top-level scalar only when its
 * value really changed, so a flat `Width` would never be re-evaluated with an unchanged value
 * and the defect this test exists for would never get a chance to fire.
 *
 * In a header because UnrealHeaderTool only parses .h files, and unconditionally rather than
 * behind WITH_DEV_AUTOMATION_TESTS because UHT emits reflection code without consulting the
 * preprocessor.
 */
USTRUCT()
struct FVaCuusDataStyleTestBar
{
	GENERATED_BODY()

	/**
	 * THE LEG THE PUBLISH-RATIO ASSERTION RESTS ON. A no-op write to a border property runs
	 * ElementBackgroundBorder::GenerateGeometry, which releases and re-makes its mesh with no
	 * equality check at all (ElementBackgroundBorder.cpp:131-137) — a new handle and resource
	 * traffic, i.e. both legs of the publish gate. Unit-bearing, so it is exactly the case the
	 * bead names.
	 */
	UPROPERTY(EditAnywhere, Category = "Test")
	FString BorderWidth;

	/**
	 * The idiomatic health-bar binding, kept because it is what buyers write — but MEASURED
	 * not to publish on its own: a no-op `width` write costs a restyle and a whole-document
	 * relayout, and then the gate absorbs it, because the box comes out identical (no
	 * OnResize, so no background dirty) and RmlUi reuses a text element's compiled geometry
	 * when the regenerated mesh compares equal (ElementText.cpp:530-536). Its cost is UI-thread
	 * time, not published frames. Both bindings sit on one element so that one document says
	 * both halves.
	 */
	UPROPERTY(EditAnywhere, Category = "Test")
	FString Width;

	/**
	 * The sibling that moves. Rendered NOWHERE and mentioned by no view: its only job is to
	 * dirty `Bar` every frame, which is what makes the two bindings above re-evaluate values
	 * that stood still. If this ever starts reaching the screen the test stops measuring what
	 * it claims.
	 */
	UPROPERTY(EditAnywhere, Category = "Test")
	int32 Tick = 0;
};

USTRUCT()
struct FVaCuusDataStyleTestModel
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	FVaCuusDataStyleTestBar Bar;
};
