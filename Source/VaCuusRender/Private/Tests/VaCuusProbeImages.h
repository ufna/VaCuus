// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Writes an RGBA8 PNG of distinct byte values with Alpha stamped into every
 * texel's A byte, and hands back the exact pre-premultiply pixels so the caller
 * can predict what LoadTexture must produce. PNG is lossless and stores
 * straight (non-premultiplied) alpha, so the decoded bytes are these bytes.
 *
 * SHARED RATHER THAN COPIED (it lived in VaCuusRecorderTest.cpp until the unsized-drain
 * test needed the same probe): two copies would be two definitions of the byte pattern the
 * decode path is asserted to reproduce, and the whole value of the pattern is that both
 * tests can predict it.
 */
inline bool SaveVaCuusProbePng(const FString& Path, FIntPoint Size, uint8 Alpha, TArray<uint8>& OutPixels)
{
	OutPixels.SetNumUninitialized(Size.X * Size.Y * 4);
	for (int32 Index = 0; Index < OutPixels.Num(); ++Index)
	{
		OutPixels[Index] = (Index % 4 == 3) ? Alpha : uint8(Index * 7 + 3);
	}

	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>("ImageWrapper");
	const TSharedPtr<IImageWrapper> Encoder = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
	if (!Encoder.IsValid() ||
		!Encoder->SetRaw(OutPixels.GetData(), OutPixels.Num(), Size.X, Size.Y, ERGBFormat::RGBA, 8))
	{
		return false;
	}

	return FFileHelper::SaveArrayToFile(Encoder->GetCompressed(), *Path);
}

#endif	// WITH_DEV_AUTOMATION_TESTS
