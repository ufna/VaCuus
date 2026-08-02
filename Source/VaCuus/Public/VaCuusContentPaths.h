// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * The ONE place that knows where a VaCuus UI document lives on disk.
 *
 * WHY IT EXISTS (controller decision D19, bead VaCuus-akj.6.3): the plugin ships
 * `Plugins/VaCuus/Content/DevUI/` and it is the git-tracked copy, but the file
 * interface used to resolve relative paths against `<Project>/Content/DevUI/` only.
 * Every task therefore hand-copied the documents into the host project, and twice
 * (M2 tasks 7 and 9) a change was made to one copy and verified against the other.
 * Live reload makes that fatal rather than merely annoying: inotify watches inodes,
 * so a watcher on one copy cannot see an edit to the other.
 *
 * WHICH DOES NOT MEAN DUPLICATES ARE NOW IMPOSSIBLE, and the distinction matters on a
 * machine where the plugin is developed as its own repository: `<Plugin>/Content/DevUI`
 * below resolves to the CLONE THAT IS MOUNTED IN THIS PROJECT, which is not necessarily
 * the clone that holds `docs/`. Two separate clones (rather than a symlink) are two sets
 * of inodes, so an edit made in the other one produces no event, no log line and nothing
 * on screen. The log line every reload emits names the absolute root it resolved to --
 * read it before concluding that live reload is broken. Code cannot fix two clones.
 *
 * THE ROOT ORDER IS PLUGIN-FIRST, and that is the decision, not an accident:
 *
 *   1. <Plugin>/Content/DevUI   -- canonical, git-tracked, what the watcher watches
 *   2. <Project>/Content/DevUI  -- optional, for documents a project adds itself
 *
 * A CONSEQUENCE WORTH STATING PLAINLY: with the plugin first, a project CANNOT
 * shadow a document the plugin ships by putting a same-named file in its own
 * Content/DevUI -- the plugin copy wins. The project root is an EXTENSION point, not
 * an override point. That is the right trade for M2: a project-first order would let
 * exactly the stale-duplicate bug this decision exists to kill come back (a forgotten
 * project copy would silently shadow the plugin document the watcher is watching, and
 * live reload would appear broken). Per-document overriding, if it is ever wanted,
 * belongs in a project setting rather than in path precedence.
 *
 * NOT EDITOR-ONLY: IPlugin::GetContentDir() is `FPaths::GetPath(FileName)/Content`
 * (PluginManager.cpp:406-409) in the Runtime `Projects` module, so a packaged game
 * resolves the same root. Loose DevUI files still have to be STAGED for that to find
 * anything, and they ARE: the RuntimeDependencies block in Source/VaCuus/VaCuus.Build.cs
 * stages them, with the whole receipt-to-staging chain cited there -- including the one
 * trap that survives it (a document added since the last makefile generation can be left
 * out of the receipt). Config/FilterPlugin.ini is NOT part of that and never was: its only
 * consumer in the engine is `RunUAT BuildPlugin`, which builds a redistributable plugin zip
 * rather than a cooked game, and that command's default filter already includes /Content/...
 * (BuildPluginCommand.Automation.cs:465, read at :472). Read VaCuus.Build.cs, not this
 * paragraph, when the question is "does it ship".
 */
namespace VaCuusContentPaths
{
/**
 * The ordered DevUI roots, absolute and normalised, plugin first (see above).
 *
 * Resolved once, on first call, and cached: FindPlugin() is a map lookup into state
 * that is fixed after plugin discovery, but it is not documented as thread-safe and
 * this is called from the UI thread. FVaCuusModule::StartupModule() primes the cache
 * on the game thread so the first UI-thread call can never be the one that races
 * plugin mounting.
 *
 * A root is listed whether or not it exists on disk; callers that need existence say
 * so (ResolveExistingDocument, or IFileManager::DirectoryExists for a watch root).
 */
VACUUS_API const TArray<FString>& GetDocumentRoots();

/**
 * Where VfsPath would be served from, or an empty string when nowhere: the mounted
 * bundles first (M6 -- the same bundle-first precedence FVaCuusFileInterface::Open
 * resolves with, so this answer and the open that follows it cannot disagree), then
 * the first loose root that actually contains it. A bundle hit returns a
 * `bundle://<BundleName>/<path>` PSEUDO-path -- truthy for the "does it exist"
 * question every caller asks, loggable, but NOT openable through the platform file
 * layer; the file interface serves the actual bytes from the mounted span. Without
 * this probe, every existence-gated caller (the demo bootstraps, the default-font
 * check) would report "missing" in a bundle-only Shipping build whose loose files
 * are deliberately not staged.
 *
 * An absolute VfsPath is answered about itself (existence-checked, no root or bundle
 * involved), which is what keeps the file interface's absolute passthrough honest.
 * OutRoot, when supplied, receives the root that satisfied the request
 * (`bundle://<BundleName>` for a bundle hit, empty for the absolute case) so callers
 * can log WHICH copy they got -- the whole point of D19.
 *
 * bIncludeMountedBundles = false restricts the answer to the loose roots; the file
 * interface's own fallback uses it because its bundle probe already ran against its
 * own lookup snapshot.
 */
VACUUS_API FString ResolveExistingDocument(const FString& VfsPath, FString* OutRoot = nullptr,
	bool bIncludeMountedBundles = true);
}	 // namespace VaCuusContentPaths
