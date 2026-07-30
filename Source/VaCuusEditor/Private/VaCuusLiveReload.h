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
 * fires the delegates -- and the only thing that ticks it in a NORMAL RUNNING SESSION is
 * UEditorEngine::Tick (EditorEngine.cpp:1948; the other ~16 callers are one-shot flushes
 * from asset-registry, build and commandlet code). Nothing ticks it in a packaged game or
 * in a `-game` standalone process. Registering a watch from a Runtime module would
 * therefore appear to work (a valid handle comes back) and never deliver a single event.
 * The watcher is editor-only by construction, not by policy, so it lives in the module
 * that only exists in the editor.
 *
 * NOT the same claim as "no test can drive it": Tick(-1.0f) drains inotify and fires the
 * delegates INLINE on the calling thread, and engine code does exactly that when it needs
 * changes now (AssetRegistry.cpp:2039, WorldPartitionEditorModule.cpp:719,
 * EditorBuildUtils.cpp:1113). VaCuus.LiveReload.WatcherEvent uses it.
 *
 * THREADING: everything here is game thread. The Linux backend checkf(IsInGameThread())
 * in Init (DirectoryWatchRequestLinux.cpp:79), ProcessNotifications (:144),
 * WatchDirectoryTree (:210) and UnwatchDirectoryTree (:300), so registration,
 * unregistration AND the callback are all game-thread by assertion -- which also means
 * the handler needs no marshaling hop, and adding one "for safety" would only delay the
 * reload by a frame.
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

	/**
	 * Roots this instance actually registered a watch on (absolute). Empty until Start().
	 *
	 * This is how the existence-check skip in Start() is asserted directly rather than by
	 * inference (VaCuus.LiveReload.WatcherEvent), and it is where that test writes its
	 * probe file -- watching a root nobody watches would prove nothing.
	 */
	const TArray<FString>& GetWatchedRoots() const { return WatchedRoots; }

	//~ The debounce/filter half, split out from the watcher so most of it is testable
	//~ without one -- synchronously, with no latent command and no real file I/O.
	//~ The link to a REAL inotify event is covered separately and does need a latent
	//~ command (VaCuus.LiveReload.WatcherEvent): delivery is asynchronous and the debounce
	//~ is time-based, so that test writes a file, pumps IDirectoryWatcher::Tick(-1.0f)
	//~ until the change shows up, and then waits out the quiet period.

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
	 * to the engine root stays absolute (FPaths::CreateStandardFilename, which
	 * MakeStandardFilename delegates to: Paths.cpp:1445-1455, called from :1510-1513). Both
	 * forms turn up in the same session here, because the plugin root is outside the project
	 * tree and the project root is inside it, so every incoming name goes through this.
	 */
	static FString NormalizeChangedPath(const FString& Filename);

	/**
	 * Filters + records one change and arms the debounce. Returns true if it was tracked.
	 * Game thread. Exposed so a test can drive the coalescing without a watcher.
	 */
	bool NoteChange(const FFileChangeData& Change);

	/**
	 * NoteChange with the clock injected. Game thread.
	 *
	 * The pair with TickDebounceAt() is what makes the debounce TIMING testable rather than
	 * only its set behaviour: a batch's meaning is entirely in the distance between the
	 * timestamps recorded here and the one a tick is judged at, so a test that cannot choose
	 * both cannot assert the quiet window, the hard cap or the batch boundary without
	 * sleeping -- and a sleeping test asserts the scheduler, not this arithmetic.
	 */
	bool NoteChangeAt(const FFileChangeData& Change, double Now);

	/** Changed paths collected but not yet flushed. */
	int32 GetNumPendingChanges() const { return PendingChanges.Num(); }

	/** True while the debounce ticker is armed. */
	bool IsDebouncePending() const { return DebounceTickerHandle.IsValid(); }

	/**
	 * Flushes the pending batch now, whatever the debounce thinks. Returns views reloaded.
	 * Game thread.
	 *
	 * DISARMS THE DEBOUNCE TOO, so "now" means now: an armed ticker left behind would keep
	 * polling and then flush an empty batch. Safe even when the caller IS that ticker's
	 * delegate -- see the note at the top of the definition.
	 */
	int32 FlushNow();

	/**
	 * FlushNow with the clock injected -- the third of the trio with NoteChangeAt() and
	 * TickDebounceAt(), and needed for the same reason plus one: the flush REPORTS how long
	 * the debounce held the batch, and a flush driven by injected timestamps but timed
	 * against the wall clock logs a nonsense figure into the one diagnostic line live reload
	 * has.
	 */
	int32 FlushAt(double Now);

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

	//~ Debounce parameters. Public so VaCuus.LiveReload.Debounce can assert the timing
	//~ contract against the same numbers instead of hard-coding a copy of them.

	/** Quiet time before a flush. One editor save is a BURST of IN_MODIFY events. */
	static constexpr double QuietSeconds = 0.15;

	/** How often the armed ticker asks "is it quiet yet". */
	static constexpr float DebouncePollSeconds = 0.05f;

	/**
	 * Hard cap from the FIRST change of a batch, so a file being rewritten continuously
	 * (a generator, a long `git checkout`) still reaches the screen.
	 */
	static constexpr double MaxDeferSeconds = 1.0;

	/**
	 * The ticker body, with the clock injected: flushes once the batch has been quiet for
	 * QuietSeconds, or once MaxDeferSeconds have passed since its FIRST change. Returns
	 * "keep ticking". Game thread.
	 *
	 * Public, unlike the ticker that calls it, because it holds the only arithmetic in this
	 * feature and the whole timing contract is asserted through it (see NoteChangeAt).
	 */
	bool TickDebounceAt(double Now);

private:
	/** The FDirectoryChanged delegate body. Game thread (Linux asserts it). */
	void OnDirectoryChanged(const TArray<FFileChangeData>& Changes);

	/** Arms the debounce ticker if it is not already armed. */
	void ArmDebounce();

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
