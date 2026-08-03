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
 * fires the delegates -- and the only thing that ticks it in a NORMAL RUNNING EDITOR
 * SESSION is UEditorEngine::Tick (EditorEngine.cpp:1948). There are 14 call sites outside
 * the DirectoryWatcher module itself; the other 13 are all editor, commandlet or Program
 * code, and NOT all one-shot flushes -- SlateFileDlgWindow.cpp:1297 is a per-frame Slate
 * widget Tick (only while that file dialog is open) and UserInterfaceCommand.cpp:117 is
 * UnrealFrontend's own main loop. What matters is that none of them exists in a packaged
 * game or a `-game` standalone process, so nothing there ticks the watcher at all.
 * Registering a watch from a Runtime module would therefore appear to work (a valid handle
 * comes back) and never deliver a single event. The watcher is editor-only by
 * construction, not by policy, so it lives in the module that only exists in the editor.
 *
 * NOT the same claim as "no test can drive it": Tick(-1.0f) drains inotify and fires the
 * delegates INLINE on the calling thread, and engine code does exactly that when it needs
 * changes now (AssetRegistry.cpp:2039, WorldPartitionEditorModule.cpp:717-719,
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
	FVaCuusLiveReload() = default;
	~FVaCuusLiveReload();

	//~ NEITHER COPYABLE NOR MOVABLE, and not for tidiness: Start() hands raw `this` to
	//~ IDirectoryWatcher::FDirectoryChanged::CreateRaw and ArmDebounce() captures it in an
	//~ FTSTicker lambda, so a copy would give two objects the same two registrations and the
	//~ second Shutdown() (or destructor, which calls it) would unregister a watch and remove
	//~ a ticker the first one already dropped. Nothing copies one today -- the editor module
	//~ holds a TSharedPtr, the tests hold locals and one member -- which is exactly when to
	//~ make it impossible rather than after somebody does.
	FVaCuusLiveReload(const FVaCuusLiveReload&) = delete;
	FVaCuusLiveReload& operator=(const FVaCuusLiveReload&) = delete;

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
	 * ('../../../YourProject/Content/DevUI/x.rml') -- while a path that cannot be made relative
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
	 * Flushes the pending batch as of the given time, whatever the debounce thinks, and
	 * returns how many views reloaded. Game thread. THE ONLY FLUSH: TickDebounceAt() calls
	 * this directly, and so does the one test that flushes out of band.
	 *
	 * The clock is a parameter for the same reason NoteChangeAt()'s is, plus one of its own:
	 * the flush REPORTS how long the debounce held the batch, and a flush driven by injected
	 * timestamps but timed against the wall clock logs a nonsense figure into the one
	 * diagnostic line live reload has.
	 *
	 * DISARMS THE DEBOUNCE FIRST, so "flush" means now: an armed ticker left behind would
	 * keep polling and then flush an empty batch. Safe when the caller IS that ticker's
	 * delegate, which TickDebounceAt() is -- see the note at the top of the definition.
	 *
	 * There was a FlushNow() wrapper here that read FPlatformTime::Seconds() for you. It had
	 * no production caller (the ticker calls this) and one test caller, so it was a second
	 * documented entry point to the same code; deleted rather than kept for symmetry.
	 */
	int32 FlushAt(double Now);

	/**
	 * D21's dispatch, which is now one call to
	 * UVaCuusSubsystem::ClearAssetCachesAndReloadAllViews() -- read that declaration for
	 * what a whole reload is and why the cache clear and the fan-out are inseparable.
	 *
	 * GRANULARITY IS "EVERY VIEW WITH A FILE DOCUMENT", not "views whose document is the
	 * changed file", and that is a correctness choice rather than the lazy one: the
	 * commonest edit by far is to an .rcss, which is never equal to any view's document
	 * path -- RmlUi pulls it in through a <link> the game thread never sees. Matching on
	 * the path would therefore make the main use case silently do nothing. Reloading a
	 * document that did not change costs one re-parse and re-layout of one document. That
	 * rule is the WATCHER's, which is why it is documented here and not on the runtime
	 * entry point: what FlushAt() knows is a set of changed files, and it deliberately
	 * throws that set away.
	 *
	 * NO LONGER THE ONLY THING IN THE TREE THAT DOES A WHOLE RELOAD, and that was the fix
	 * (bead VaCuus-akj.6.34): while it was, a runtime hook that wanted a reload had nothing
	 * correct to call.
	 *
	 * Returns how many views were reloaded. Static: it holds no state. (vacuus.ReloadUI no
	 * longer routes through here -- the command moved to the runtime module,
	 * VaCuusSubsystem.cpp, bead VaCuus-akj.6.18 -- so the callers left are the watcher's
	 * flush and the tests.)
	 */
	static int32 ReloadAllLiveViews(const TCHAR* Reason);

	/**
	 * The Content/DevUI of a SECOND working tree of this plugin, if the git config at
	 * GitConfigPath proves one exists; empty otherwise. See WarnAboutSecondWorkingTree in the
	 * .cpp for why a local-path git remote is evidence and what this deliberately cannot see.
	 *
	 * PUBLIC AND PARAMETERISED ONLY SO IT CAN BE TESTED, like ShouldTrackChange above: the
	 * git-config grammar is the one fragile part of the check, and a detector that silently
	 * stopped matching would restore exactly the silent failure it exists to end.
	 */
	static FString FindSecondWorkingTreeDevUI(const FString& GitConfigPath, const FString& PluginDir);

	//~ The two debounce THRESHOLDS. Public because VaCuus.LiveReload.Debounce asserts the
	//~ timing contract against these very numbers rather than a hard-coded copy of them --
	//~ the poll INTERVAL is not one of them and is private below.

	/**
	 * Quiet time before a flush. One editor save is a BURST of IN_MODIFY events.
	 *
	 * 0.15 s IS THE DECISION, RE-TAKEN AND DELIBERATELY UNCHANGED (bead VaCuus-akj.6.24). M2's
	 * acceptance step asked for "within ~200 ms" and the proof measured 191
	 * (docs/research/proofs/m2-t10-live-reload/README.md), and 9 ms of apparent headroom
	 * invites a trim. It should not be trimmed, for three reasons that are all checkable:
	 *
	 *  - THE MEASUREMENT IS NOT THIS NUMBER PLUS SLACK, it is this number plus the burst.
	 *    FlushAt() reports Now - FirstChangeSeconds, while the earliest a batch may flush is
	 *    LastChange + QuietSeconds -- so the save the proof recorded, 4 inotify events spanning
	 *    ~40 ms, cannot report under 190 ms however fast the rest of the machine is. Trimming
	 *    the window buys 1:1 against the 150 and nothing against the 40.
	 *    VaCuus.LiveReload.Debounce (e) pins that arithmetic to these constants.
	 *  - THE JITTER IS WIDER THAN THE HEADROOM. The armed ticker asks every
	 *    DebouncePollSeconds (50 ms) and FTSTicker only fires from the engine loop, so the same
	 *    code on the same machine legitimately reports anywhere across a ~50 ms band plus a
	 *    frame. A 9 ms margin measured once is inside its own measurement's noise; treating it
	 *    as headroom is a number chasing itself.
	 *  - AND A SHORTER WINDOW DOES NOT FAIL "slightly early". It fails by flushing BETWEEN the
	 *    writes of one save and reloading a half-written file, which surfaces as a parse error
	 *    on a document the developer just fixed.
	 *
	 * "~200 ms" was never a budget either: it is a plan step's own wording, with a tilde on it.
	 * No spec row, passport row or test states a live-reload latency at all -- and the honest
	 * place to spend effort, if the figure ever does matter, is the 50 ms poll rather than the
	 * quiet window, because that part is pure latency with no correctness attached.
	 */
	static constexpr double QuietSeconds = 0.15;

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
	/**
	 * How often the armed ticker asks "is it quiet yet". Private, unlike the two thresholds
	 * above: it is a sampling rate, not part of the contract. No test asserts it and none
	 * should -- TickDebounceAt() is judged at times the test chooses, so the interval at
	 * which the real ticker happens to call it cannot change any answer, only the latency of
	 * getting it. Its one use is the AddTicker() call in ArmDebounce().
	 */
	static constexpr float DebouncePollSeconds = 0.05f;

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
