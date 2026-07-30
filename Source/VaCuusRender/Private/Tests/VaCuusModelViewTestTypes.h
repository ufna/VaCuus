// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"

#include "VaCuusModelViewTestTypes.generated.h"

/*
 * FIXTURE TYPE FOR VaCuus.Model.View.Idle.
 *
 * ITS OWN, RATHER THAN VaCuus's FVaCuusSamplerDefaultsModel, because that one lives in a
 * Private/ header of the VaCuus module and this test is in VaCuusRender. Deliberately small:
 * the test is about published FRAMES, not about field kinds, and every extra field is another
 * thing the differ has to be right about for a reason this test does not care about.
 *
 * In a header because UnrealHeaderTool only parses .h files -- a USTRUCT in a .cpp is never
 * reflected -- and unconditionally compiled rather than wrapped in WITH_DEV_AUTOMATION_TESTS,
 * because UHT emits reflection code without the preprocessor's answer.
 */
USTRUCT()
struct FVaCuusModelViewTestModel
{
	GENERATED_BODY()

	/** Rendered as text through `{{Title}}`, so a change really is new glyph geometry. */
	UPROPERTY(EditAnywhere, Category = "Test")
	FString Title;

	UPROPERTY(EditAnywhere, Category = "Test")
	float Health = 100.f;
};
