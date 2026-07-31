// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * The script-source read path (M4 Task 6; Task 7's module loader reuses it):
 * reads AbsolutePath through IPlatformFile::OpenRead -- the FVaCuusFileInterface
 * pattern (VaCuusFileInterface.cpp), so files inside a pak open transparently --
 * and decodes UTF-8 (BOM tolerated). False when the file cannot be opened or
 * read; the caller owns the error line, because only it can name the document
 * (or importing module) that asked.
 */
namespace VaCuusJsScriptSource
{
bool ReadScriptFile(const FString& AbsolutePath, FString& OutSource);
}
