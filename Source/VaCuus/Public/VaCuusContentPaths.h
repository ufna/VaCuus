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
/**
 * VaCuusContentPaths::ProbeImage's verdict. Ok is the only value a caller may proceed on.
 */
enum class EVaCuusImageProbe : uint8
{
	/** Resolved, readable, and the bytes begin with a known image signature. */
	Ok,

	/** No mounted bundle and no loose root serves it. */
	Missing,

	/** Present and readable, but the bytes are a Git-LFS pointer -- `git lfs pull`. */
	GitLfsPointer,

	/** Present and readable, but neither a pointer nor a known image signature. */
	NotAnImage,

	/** Resolved to something that could not be opened or read at all. */
	Unreadable,
};

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

/**
 * Classifies ONE image the way a person would if they read the bytes, so a content-
 * dependent bootstrap can refuse by name instead of drawing blank rectangles.
 *
 * WHY THIS IS NOT `ResolveExistingDocument(...).IsEmpty()`, and it is the whole point
 * (bead VaCuus-akj.28): art goes missing in TWO ways that look identical on screen, and
 * an existence check catches only one.
 *
 *   1. The file is absent -- Content/DevUI/img/ gitignored, or simply never staged.
 *   2. The file is PRESENT AND IS A ~130-BYTE GIT-LFS POINTER. Both this plugin and
 *      VaCuusDemo carry `*.png filter=lfs`, so any clone made on a machine without
 *      git-lfs installed writes pointer text in place of every image. Existence
 *      passes, the open succeeds, and the failure moves down into the PNG decoder.
 *
 * Both render as the same flat colour blocks. The incident this was written for cost an
 * hour on a plausible wrong hypothesis (a race in the async decode path, which exists
 * and had just produced a real bug) before anyone read the log. Naming the repair is
 * therefore the deliverable, not merely detecting the fault.
 *
 * ONE FILE IS ENOUGH for a whole art directory: images arrive together or not at all --
 * a checkout writes pointers for every LFS-tracked path or for none, and a gitignored
 * directory omits all of it. Callers probe a single representative image.
 *
 * SIGNATURES, NOT EXTENSIONS: PNG and JPEG (the two raster formats .gitattributes
 * LFS-tracks and RmlUi's decoders accept here). A file whose bytes match neither is
 * NotAnImage rather than Ok, so an Ok verdict is a positive statement about content
 * rather than "not obviously a pointer" -- which is what makes a truncated or
 * half-smudged file visible too.
 *
 * BUNDLES ARE PROBED IN PLACE. A bundle hit cannot go through the platform file layer
 * (ResolveExistingDocument returns a deliberately unopenable `bundle://` pseudo-path,
 * see above), so the bytes are read from the mounted span instead. Without that this
 * would report Unreadable for every image in a bundle-only Shipping build and turn a
 * healthy package into a refusal.
 *
 * OutDiagnosis, when supplied, receives a full human sentence naming the file, the
 * fault and the repair -- empty on Ok. Callers log it verbatim; keeping the wording
 * here is what stops each call site from inventing a weaker message.
 *
 * Any thread (the mount lookup is a snapshot; the loose read is plain file IO).
 */
VACUUS_API EVaCuusImageProbe ProbeImage(const FString& VfsPath, FString* OutDiagnosis = nullptr);
}	 // namespace VaCuusContentPaths
