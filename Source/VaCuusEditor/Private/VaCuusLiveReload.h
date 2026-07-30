// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Containers/Ticker.h"
#include "IDirectoryWatcher.h"

/**
 * Watches the DevUI roots and re-loads live documents when a file under them changes.
 *
 * WHY IT LIVES IN THE EDITOR MODULE AND NOWHERE ELSE (controller decision D20):
 * IDirectoryWatcher is 100% pull-based -- its Tick() is what reads the inotify fd and
 * fires the delegates -- and the ONLY engine caller of that Tick is UEditorEngine::Tick
 * (EditorEngine.cpp:1948). Nothing ticks it in a packaged game or in a `-game`
 * standalone process. Registering a watch from a Runtime module would therefore appear
 * to work (a valid handle comes back) and never deliver a single event. The watcher is
 * editor-only by construction, not by policy, so it lives in the module that only
 * exists in the editor.
 *
 * THREADING: everything here is game thread. The Linux backend checkf(IsInGameThread())
 * in Init, WatchDirectoryTree, UnwatchDirectoryTree and ProcessNotifications
 * (DirectoryWatchRequestLinux.cpp:79, 144, 210, 300), so registration, unregistration
 * AND the callback are all game-thread by assertion -- which also means the handler
 * needs no marshaling hop, and adding one "for safety" would only delay the reload by a
 * frame.
 *
 * WHAT IS DELIBERATELY NOT DONE: textures are not released. Rml::ReleaseTextures() would
 * drop the font atlases along with the images, forcing a re-upload through a render
 * interface whose handles the render thread may still be holding from an in-flight
 * replay -- a real risk for a convenience nobody asked for. Consequence, stated rather
 * than hidden: editing a PNG under DevUI does not update a running UI. RML and RCSS
 * (the reason live reload exists) do.
 */
class FVaCuusLiveReload
{
public:
	~FVaCuusLiveReload();

	/**
	 * Registers a watch on every DevUI root that EXISTS. Game thread.
	 *
	 * Idempotent-ish: call once from the module's StartupModule(). Safe to call when the
	 * DirectoryWatcher module is unavailable (it says so and does nothing).
	 */
	void Start();

	/** Unregisters every watch and drops the debounce ticker. Game thread; safe twice. */
	void Shutdown();

	/** Roots this instance actually registered a watch on (absolute). Empty until Start(). */
	const TArray<FString>& GetWatchedRoots() const { return WatchedRoots; }

	//~ The debounce/filter half, split out from the watcher so it is testable without one:
	//~ nothing ticks IDirectoryWatcher outside UEditorEngine::Tick, so a headless
	//~ automation test can never make a real event arrive.

	/**
	 * Is this change one live reload cares about?
	 *
	 * Pure, and the whole filter in one place: the action must be Added or Modified
	 * (Removed is a rename-away, and RescanRequired carries no file), the name must not be
	 * an editor temp/swap file, and the extension must be one RmlUi re-reads on a document
	 * load. Filename may be relative -- this does not normalise, callers do.
	 */
	static bool ShouldTrackChange(FFileChangeData::EFileChangeAction Action, const FString& Filename);

	/**
	 * FFileChangeData::Filename -> absolute.
	 *
	 * NOT COSMETIC. The FFileChangeData constructor runs FPaths::MakeStandardFilename
	 * (IDirectoryWatcher.h:23), so what arrives is typically RELATIVE
	 * ('../../../VcHost/Content/DevUI/x.rml') -- while a path that cannot be made relative
	 * to the engine root stays absolute (Paths.cpp:1445-1455). Both forms turn up in the
	 * same session here, because the plugin root is outside the project tree and the
	 * project root is inside it, so every incoming name goes through this.
	 */
	static FString NormalizeChangedPath(const FString& Filename);

	/**
	 * Filters + records one change and arms the debounce. Returns true if it was tracked.
	 * Game thread. Exposed so a test can drive the coalescing without a watcher.
	 */
	bool NoteChange(const FFileChangeData& Change);

	/** Changed paths collected but not yet flushed. */
	int32 GetNumPendingChanges() const { return PendingChanges.Num(); }

	/** True while the debounce ticker is armed. */
	bool IsDebouncePending() const { return DebounceTickerHandle.IsValid(); }

	/** Flushes now, whatever the debounce thinks. Returns views reloaded. Game thread. */
	int32 FlushNow();

	/**
	 * D21's dispatch: reload every live view in every PIE (and `-game`-in-editor) world.
	 *
	 * GRANULARITY IS "EVERY VIEW WITH A FILE DOCUMENT", not "views whose document is the
	 * changed file", and that is a correctness choice rather than the lazy one: the
	 * commonest edit by far is to an .rcss, which is never equal to any view's document
	 * path -- RmlUi pulls it in through a <link> the game thread never sees. Matching on
	 * the path would therefore make the main use case silently do nothing. Reloading a
	 * document that did not change costs one re-parse and re-layout of one document.
	 *
	 * It also drops RmlUi's parsed stylesheet/template caches ONCE, before the fan-out and
	 * whether or not any view is found -- see FVaCuusUIThread::EnqueueClearAssetCaches() for
	 * why that cannot ride on a per-view load command.
	 *
	 * Returns how many views were reloaded. Static: it holds no state and vacuus.ReloadUI
	 * calls it directly.
	 */
	static int32 ReloadAllLiveViews(const TCHAR* Reason);

	//~ Debounce parameters. Public so the test can assert against the same numbers.

	/** Quiet time before a flush. One editor save is a BURST of IN_MODIFY events. */
	static constexpr double QuietSeconds = 0.15;

	/** How often the armed ticker asks "is it quiet yet". */
	static constexpr float DebouncePollSeconds = 0.05f;

	/**
	 * Hard cap from the FIRST change of a batch, so a file being rewritten continuously
	 * (a generator, a long `git checkout`) still reaches the screen.
	 */
	static constexpr double MaxDeferSeconds = 1.0;

private:
	/** The FDirectoryChanged delegate body. Game thread (Linux asserts it). */
	void OnDirectoryChanged(const TArray<FFileChangeData>& Changes);

	/** Arms the debounce ticker if it is not already armed. */
	void ArmDebounce();

	/** Ticker body: flushes once it has been quiet long enough. Returns "keep ticking". */
	bool TickDebounce();

	/**
	 * Absolute roots with a live watch, and the delegate handles for them.
	 *
	 * PARALLEL ARRAYS because Unregister needs BOTH, and it needs the root spelled exactly
	 * as it was registered (the proxy keys on ConvertRelativePathToFull + a trailing slash,
	 * the Linux backend on ConvertRelativePathToFull -- so the string we passed in is the
	 * only one guaranteed to match).
	 */
	TArray<FString> WatchedRoots;
	TArray<FDelegateHandle> WatchHandles;

	/** Absolute paths, deduped: a single save yields several FCA_Modified for one file. */
	TSet<FString> PendingChanges;

	/** FTSTicker::FDelegateHandle is a TWeakPtr, NOT the ::FDelegateHandle above. */
	FTSTicker::FDelegateHandle DebounceTickerHandle;

	/** FPlatformTime::Seconds() of the newest and the oldest change in the current batch. */
	double LastChangeSeconds = 0.0;
	double FirstChangeSeconds = 0.0;
};
