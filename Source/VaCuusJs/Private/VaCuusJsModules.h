// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * The module namespace's string logic (M4 Task 7, spec 3.7), shared by the
 * normalize thunk (VaCuusJsModules.cpp) and the script host's module-entry path
 * (RunCapturedScripts) so both mint IDENTICAL canonical names -- the name is the
 * per-context module-cache key (js_find_loaded_module walks ctx->loaded_modules
 * by name atom, quickjs.c:29985-29996), and two spellings of one file would be
 * two cache entries executing twice.
 *
 * THE NAME SHAPE: `vfs://<canonical-root-relative-path>`. Honest on purpose --
 * the path names a slot in the ordered DevUI roots (VaCuusContentPaths), not a
 * disk location; WHICH root (or pak) answers is decided at load time, exactly
 * like every other VFS read. import.meta.url carries this same string.
 */
namespace VaCuusJsModules
{
/** The scheme every module name wears. Stripping it before root resolution is load-bearing: FPaths::IsRelative treats "vfs://x" as relative, so an unstripped scheme probes "<Root>/vfs://x" and misses (plugin-integration.md section 3, the recorded trap). */
inline constexpr const TCHAR* VfsScheme = TEXT("vfs://");

/** InName without a leading "vfs://" (returned unchanged when it wears none). */
FString StripVfsScheme(const FString& InName);

/**
 * Segment-walk canonicalization: backslashes unified, empty and "." segments
 * dropped, ".." popping its parent. False when a ".." would climb ABOVE the
 * root -- such a path escapes the document-roots sandbox and must not resolve.
 * Hand-rolled rather than FPaths::CollapseRelativeDirectories because that
 * helper neither strips a leading "./" nor promises anything about a bare
 * leading ".." (Paths.cpp), and the cache-key argument above needs exactness.
 */
bool CanonicalizeVfsRelativePath(const FString& InPath, FString& OutCanonical);

/**
 * "vfs://" + canonical(InRootRelativePath): the module name for an ENTRY the
 * host resolves itself (the loader mints names for imports via the normalize
 * thunk, which ends in the same two calls). Falls back to the input path,
 * scheme'd, when canonicalization refuses -- the loader will then miss and name
 * the path in its Error, which is a better diagnostic than a silent drop here.
 */
FString MakeModuleName(const FString& InRootRelativePath);
}	 // namespace VaCuusJsModules
