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

/**
 * The whole resolve-and-read for a VFS-relative script path, BUNDLE-FIRST (M6):
 * probes the mounted bundles exactly as FVaCuusFileInterface::Open does -- same
 * normalization, same first-hit-wins order -- and only then the loose DevUI
 * roots via ResolveExistingDocument + ReadScriptFile. This function exists
 * because scripts do NOT flow through RmlUi's FileInterface (RmlUi never sees
 * <script src> bytes; this module reads them itself), so without it a
 * bundle-only Shipping build has no loose file to read and every script dies --
 * which is exactly how the first packaged Development gate run failed: the
 * bundle-aware resolver answered with a bundle:// pseudo-path and the raw
 * IPlatformFile read of that pseudo-path could only miss.
 *
 * OutServedFrom names what answered -- the bundle pseudo-path or the disk path
 * -- for the caller's SourceName/backtrace. UI thread (the mount lookup is an
 * immutable snapshot; the bundle read is a span decode).
 */
bool ReadScriptByVfsPath(const FString& VfsPath, FString& OutSource, FString* OutServedFrom = nullptr);
}
