// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusLiveReload.h"

#include "VaCuusBundleMount.h"
#include "VaCuusContentPaths.h"
#include "VaCuusDefines.h"
#include "VaCuusSubsystem.h"

#include "DirectoryWatcherModule.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

namespace VaCuusLiveReloadPrivate
{
/** Extensions a document reload actually re-reads. */
static bool IsWatchedExtension(const FString& Extension)
{
	// Compared lowercase: FPaths::GetExtension preserves case and a file saved as
	// '.RCSS' is the same file to RmlUi's loader.
	//
	// js/mjs (M4 Task 7, spec 3.7): a script edit rides the SAME full-document
	// reload as an .rcss edit -- no second mechanism -- and that reload is
	// sufficient BY CONSTRUCTION of the Task 6 path: the reload is a replace,
	// the replace recycles the view's JSContext (FVaCuusJsScriptHost::
	// OnDocumentReady), and everything a stale script could hide in dies with
	// that context -- globals, timers, AND the module cache, which is
	// per-context state freed inside JS_FreeContext (ctx->loaded_modules,
	// quickjs.c:532, swept at :2605). The fresh context then re-reads every
	// <script src> and re-loads every module from disk (nothing above the
	// context caches script bytes: VaCuusJsScriptSource reads through
	// IPlatformFile on each call). Before this line, script edits reloaded
	// nothing at all -- the extension whitelist was the only gate.
	return Extension.Equals(TEXT("rml"), ESearchCase::IgnoreCase) ||
		   Extension.Equals(TEXT("rcss"), ESearchCase::IgnoreCase) ||
		   Extension.Equals(TEXT("js"), ESearchCase::IgnoreCase) ||
		   Extension.Equals(TEXT("mjs"), ESearchCase::IgnoreCase);
}

/**
 * Names a SECOND working tree of this plugin when one is provably present, because that is
 * the shape in which live reload fails with no error at all (bead VaCuus-akj.6.22).
 *
 * THE FAILURE. VaCuus is developed as its own git repository, and the checkout a host
 * project builds is not necessarily the checkout that holds `docs/` and the issue database.
 * Two checkouts are two sets of INODES, and inotify watches inodes: the Linux backend hands
 * the resolved directory to inotify_add_watch (DirectoryWatchRequestLinux.cpp:266) and the
 * kernel then reports events for that inode's children. So editing
 * `Content/DevUI/foo.rcss` in the OTHER tree produces no event, no reload, no log line --
 * not even "reloaded 0 view(s)", because nothing ever flushes. Nothing anywhere reports an
 * error, which is exactly the outcome the content-location decision (D19) exists to prevent
 * and which it cannot prevent, since code cannot merge two clones.
 *
 * WHY A GIT REMOTE IS THE EVIDENCE. If this plugin directory is a git checkout whose
 * `.git/config` names a remote whose url is a LOCAL ABSOLUTE PATH, and that path exists and
 * has its own Content/DevUI, then a second working tree demonstrably exists on this machine
 * with documents in it. That is a fact read off disk, not a heuristic. An ordinary remote
 * (`git@github.com:...`, `https://...`) is not an absolute path and is skipped; a bare
 * mirror has no Content/DevUI and is skipped too; a plugin SYMLINKED into the project
 * resolves to the canonical checkout's own config, whose remote is the real upstream, so
 * the one arrangement that has a single set of inodes stays silent -- correctly.
 *
 * WHAT IT CANNOT SEE, said plainly so nobody reads silence as safety: two clones that both
 * point upstream rather than at each other, or a copy made with `cp -r`. Finding those means
 * sweeping the disk for directories that happen to contain Content/DevUI -- unbounded and
 * imprecise, i.e. the fragile thing not to build. The startup line below therefore states
 * the consequence UNCONDITIONALLY, and README.md's Development section carries the
 * arrangement itself.
 *
 * A BIND MOUNT of one tree onto the other would warn spuriously (two paths, one inode, and
 * no engine API exposes an inode to compare). Accepted: the message names both directories,
 * so a reader in that situation can see it does not apply.
 */
static void WarnAboutSecondWorkingTree(const TArray<FString>& WatchedRoots)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("VaCuus"));
	if (!Plugin.IsValid())
	{
		return;
	}

	const FString PluginDir = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
	const FString OtherDevUI =
		FVaCuusLiveReload::FindSecondWorkingTreeDevUI(PluginDir / TEXT(".git") / TEXT("config"), PluginDir);
	if (OtherDevUI.IsEmpty())
	{
		return;
	}

	UE_LOG(LogVaCuus, Warning,
		TEXT("Live reload: a SECOND VaCuus working tree exists at '%s' -- this plugin is a git checkout ")
		TEXT("whose remote points there, and it has its own Content/DevUI. It is NOT watched, and it cannot ")
		TEXT("be: inotify watches inodes, not paths, so an edit made there produces no event, no reload and ")
		TEXT("no error. The watched copy is '%s'. Edit that one, or make the two a single tree."),
		*OtherDevUI, *FString::Join(WatchedRoots, TEXT(" | ")));
}
}	 // namespace VaCuusLiveReloadPrivate

FString FVaCuusLiveReload::FindSecondWorkingTreeDevUI(const FString& GitConfigPath, const FString& PluginDir)
{
	// FileExists, not DirectoryExists on `.git`: it is a FILE ("gitdir: ...") in a linked
	// worktree or a submodule, and this deliberately does not chase that -- a git worktree is
	// still a separate checkout with separate inodes, but its config lives elsewhere and
	// following the pointer would be guessing at a layout this has no way to test.
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *GitConfigPath))
	{
		return FString();
	}

	// NormalizeDirectoryName as well as ConvertRelativePathToFull, and the second one is not
	// tidiness: ConvertRelativePathToFull KEEPS a trailing slash, so a perfectly ordinary
	// `url = /path/to/this/` compared unequal to the plugin's own directory and this reported
	// the plugin as its own second working tree. Caught by VaCuus.LiveReload.SecondTree.
	const auto NormalizeDir = [](const FString& In)
	{
		FString Out = FPaths::ConvertRelativePathToFull(In);
		FPaths::NormalizeDirectoryName(Out);
		return Out;
	};

	const FString NormalizedPluginDir = NormalizeDir(PluginDir);

	for (const FString& Line : Lines)
	{
		// Parsed by hand rather than through FConfigFile: git's section headers are
		// `[remote "origin"]`, which is not UE's ini grammar, and every remote's URL is
		// equally interesting -- so the section does not need to be tracked at all, only the
		// two keys that hold one. `pushurl` is included because a fetch-from-upstream,
		// push-to-local-clone setup is still two working trees, and missing it would be a
		// blind spot in the one check that exists to remove a blind spot. Compared
		// case-insensitively, as git compares its own key names; and a `#`/`;` comment cannot
		// be mistaken for either, because the key would then be `# url` and this is exact.
		FString Key;
		FString Value;
		if (!Line.TrimStartAndEnd().Split(TEXT("="), &Key, &Value))
		{
			continue;
		}

		Key.TrimEndInline();
		if (!Key.Equals(TEXT("url"), ESearchCase::IgnoreCase) && !Key.Equals(TEXT("pushurl"), ESearchCase::IgnoreCase))
		{
			continue;
		}

		// The whole filter. FPaths::IsRelative is true for anything that does not begin with
		// a path root, which is every URL form git accepts -- `git@host:path`, `https://...`,
		// `ssh://...` -- so this keeps only local absolute paths.
		const FString Url = Value.TrimStartAndEnd();
		if (Url.IsEmpty() || FPaths::IsRelative(Url))
		{
			continue;
		}

		const FString OtherDir = NormalizeDir(Url);
		if (OtherDir == NormalizedPluginDir)
		{
			continue;
		}

		// The last gate, and the one that keeps this a FACT rather than a guess: a bare
		// mirror or an unrelated local repo has no Content/DevUI, so there is nothing there
		// anyone could edit and expect to see on screen.
		const FString OtherDevUI = OtherDir / TEXT("Content") / TEXT("DevUI");
		if (IFileManager::Get().DirectoryExists(*OtherDevUI))
		{
			return OtherDevUI;
		}
	}

	return FString();
}

FVaCuusLiveReload::~FVaCuusLiveReload()
{
	// The module calls Shutdown() explicitly; this is the net for anything that does not.
	Shutdown();
}

void FVaCuusLiveReload::Start()
{
	check(IsInGameThread());

	if (WatchHandles.Num() > 0)
	{
		return;
	}

	if (IsRunningCommandlet())
	{
		// A commandlet has no editor tick, so nothing would ever pump the watcher, and it
		// has no PIE world to reload into either.
		UE_LOG(LogVaCuus, Verbose, TEXT("Live reload not started: running as a commandlet"));
		return;
	}

	// GetModulePtr, not IsModuleLoaded + LoadModule: DirectoryWatcher is a
	// PrivateDependencyModuleNames entry of this module, so it is loaded before
	// StartupModule() runs and a load attempt here could never do anything.
	FDirectoryWatcherModule* WatcherModule = FModuleManager::GetModulePtr<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
	if (WatcherModule == nullptr)
	{
		UE_LOG(LogVaCuus, Warning, TEXT("Live reload unavailable: the DirectoryWatcher module is not loaded"));
		return;
	}

	IDirectoryWatcher* Watcher = WatcherModule->Get();
	if (Watcher == nullptr)
	{
		// The proxy is never null once the module started, but engine consumers all
		// null-check it and an unsupported platform is a real configuration.
		UE_LOG(LogVaCuus, Warning, TEXT("Live reload unavailable: this platform has no directory watcher"));
		return;
	}

	for (const FString& Root : VaCuusContentPaths::GetDocumentRoots())
	{
		// THE EXISTENCE CHECK IS THE LOAD-BEARING LINE ON LINUX, not a tidiness one.
		// FDirectoryWatchRequestLinux::Init fails only on an empty directory string
		// (DirectoryWatchRequestLinux.cpp:81-85) and on inotify_init1 (:99-110); for a
		// non-existent but non-empty directory it walks the tree and returns true
		// (:114-116), because inotify_add_watch failures only warn (:268-282). So
		// RegisterDirectoryChangedCallback_Handle hands back a VALID HANDLE for a directory
		// that does not exist and live reload silently never fires. Windows returns false
		// there because CreateFile fails, which is exactly why the bool below cannot be
		// trusted as a health check.
		if (!IFileManager::Get().DirectoryExists(*Root))
		{
			UE_LOG(LogVaCuus, Log,
				TEXT("Live reload skips '%s': the directory does not exist (registering it anyway would produce a ")
				TEXT("valid handle and no events on Linux)"),
				*Root);
			continue;
		}

		FDelegateHandle Handle;
		const bool bRegistered = Watcher->RegisterDirectoryChangedCallback_Handle(
			Root,
			IDirectoryWatcher::FDirectoryChanged::CreateRaw(this, &FVaCuusLiveReload::OnDirectoryChanged),
			Handle,
			// Flags 0: recurse into subdirectories (img/, fonts/) and do not report
			// directory-level adds/removes, which is what live reload wants.
			0);

		// One case, not two, and nothing is unregistered here: the mixed state is
		// unreachable. FDirectoryWatcherLinux assigns OutHandle and returns true in the same
		// two statements (DirectoryWatcherLinux.cpp:72-74), so a false return leaves nothing
		// registered to hand back. Do not "fix" this into an unregister call.
		if (!bRegistered || !Handle.IsValid())
		{
			UE_LOG(LogVaCuus, Warning, TEXT("Live reload could not watch '%s'"), *Root);
			continue;
		}

		WatchedRoots.Add(Root);
		WatchHandles.Add(Handle);
	}

	if (WatchedRoots.Num() == 0)
	{
		UE_LOG(LogVaCuus, Log, TEXT("Live reload is not watching anything (no DevUI root exists); vacuus.ReloadUI still works"));
		return;
	}

	// THE CONSEQUENCE IS PART OF THE LINE, not left to be inferred from the paths. This is
	// the one place a developer looks when live reload "does nothing", and the commonest
	// cause is that the file they saved is a different INODE from the one being watched --
	// a second checkout of this plugin, or a stray copy under the project's own Content.
	// Telling them the rule here is what turns a silent failure into a readable one.
	UE_LOG(LogVaCuus, Log,
		TEXT("Live reload watching %d root(s): %s (debounce: %.0f ms of quiet, %.0f ms cap). ")
		TEXT("ONLY these directories: inotify watches inodes, not paths, so saving a COPY of a document ")
		TEXT("anywhere else on disk produces no event, no reload and no error -- if live reload seems dead, ")
		TEXT("check that the file you edited is under a root named above."),
		WatchedRoots.Num(), *FString::Join(WatchedRoots, TEXT(" | ")), QuietSeconds * 1000.0, MaxDeferSeconds * 1000.0);

	VaCuusLiveReloadPrivate::WarnAboutSecondWorkingTree(WatchedRoots);
}

void FVaCuusLiveReload::Shutdown()
{
	// Asserted like every other entry point here, including the destructor's path: module
	// unload runs on the game thread, and Unwatch/RemoveTicker below both require it
	// (DirectoryWatchRequestLinux.cpp:300 checkf's it outright).
	check(IsInGameThread());

	if (WatchHandles.Num() > 0)
	{
		// IsModuleLoaded + GetModuleChecked, never LoadModuleChecked: during shutdown the
		// watcher module may already be unloading and reloading it would be worse than
		// leaking a watch that is about to be destroyed anyway (precedent:
		// StringTableRegistry.cpp:45).
		if (FModuleManager::Get().IsModuleLoaded(TEXT("DirectoryWatcher")))
		{
			FDirectoryWatcherModule& WatcherModule =
				FModuleManager::GetModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
			if (IDirectoryWatcher* Watcher = WatcherModule.Get())
			{
				for (int32 Index = 0; Index < WatchHandles.Num(); ++Index)
				{
					Watcher->UnregisterDirectoryChangedCallback_Handle(WatchedRoots[Index], WatchHandles[Index]);
				}
			}
		}

		WatchHandles.Empty();
		WatchedRoots.Empty();
	}

	if (DebounceTickerHandle.IsValid())
	{
		// Static in 5.8, and it blocks if the delegate is mid-execution -- so once it
		// returns, nothing can call back into a half-destroyed this.
		FTSTicker::RemoveTicker(DebounceTickerHandle);
		DebounceTickerHandle.Reset();
	}

	PendingChanges.Empty();
}

bool FVaCuusLiveReload::ShouldTrackChange(FFileChangeData::EFileChangeAction Action, const FString& Filename)
{
	if (Action != FFileChangeData::FCA_Added && Action != FFileChangeData::FCA_Modified)
	{
		// FCA_Removed is the temp half of a write-then-rename (and a genuine delete, which
		// has nothing to reload to); FCA_RescanRequired carries no usable filename, and the
		// Linux backend never produces it anyway -- IN_Q_OVERFLOW is skipped outright by
		// ProcessAllINotifyChanges (DirectoryWatchRequestLinux.cpp:496; the string
		// FCA_RescanRequired appears nowhere in that backend), which is why vacuus.ReloadUI
		// exists.
		return false;
	}

	const FString BaseFilename = FPaths::GetCleanFilename(Filename);
	if (BaseFilename.IsEmpty())
	{
		return false;
	}

	// THE ONE TEMP-FILE SHAPE THE EXTENSION WHITELIST BELOW CANNOT CATCH: a dotfile whose
	// extension is perfectly good ('.m1_hud.rcss' -- what vim leaves behind, and anything
	// else hidden). Every other shape is already rejected down there, because the extension
	// is the LAST dot-suffix: 'm1_hud.rcss~' has extension 'rcss~', '.m1_hud.rcss.swp' has
	// 'swp', 'm1_hud.rcss.tmp' has 'tmp'. A trailing-tilde test was here too and was
	// provably dead for the same reason -- a name ending in '~' can never have extension
	// 'rml' or 'rcss'.
	if (BaseFilename.StartsWith(TEXT("."), ESearchCase::CaseSensitive))
	{
		return false;
	}

	return VaCuusLiveReloadPrivate::IsWatchedExtension(FPaths::GetExtension(BaseFilename));
}

FString FVaCuusLiveReload::NormalizeChangedPath(const FString& Filename)
{
	return FPaths::ConvertRelativePathToFull(Filename);
}

bool FVaCuusLiveReload::NoteChange(const FFileChangeData& Change)
{
	return NoteChangeAt(Change, FPlatformTime::Seconds());
}

bool FVaCuusLiveReload::NoteChangeAt(const FFileChangeData& Change, double Now)
{
	check(IsInGameThread());

	if (!ShouldTrackChange(Change.Action, Change.Filename))
	{
		return false;
	}

	if (PendingChanges.Num() == 0)
	{
		FirstChangeSeconds = Now;
	}
	LastChangeSeconds = Now;

	PendingChanges.Add(NormalizeChangedPath(Change.Filename));
	ArmDebounce();
	return true;
}

void FVaCuusLiveReload::OnDirectoryChanged(const TArray<FFileChangeData>& Changes)
{
	// Game thread by assertion inside the Linux backend, so this is a statement of the
	// contract rather than a hope.
	check(IsInGameThread());

	int32 NumTracked = 0;
	for (const FFileChangeData& Change : Changes)
	{
		NumTracked += NoteChange(Change) ? 1 : 0;
	}

	UE_LOG(LogVaCuus, Verbose,
		TEXT("Live reload saw %d file change event(s), tracked %d; %d distinct path(s) pending"),
		Changes.Num(), NumTracked, PendingChanges.Num());
}

void FVaCuusLiveReload::ArmDebounce()
{
	if (DebounceTickerHandle.IsValid())
	{
		return;
	}

	// The real clock is read HERE and nowhere below, which is what makes the whole
	// debounce contract testable: TickDebounceAt() is pure arithmetic over the timestamps
	// NoteChangeAt() recorded, and the test drives both with numbers it chose.
	DebounceTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		TEXT("VaCuus.LiveReload.Debounce"), DebouncePollSeconds,
		[this](float) { return TickDebounceAt(FPlatformTime::Seconds()); });
}

bool FVaCuusLiveReload::TickDebounceAt(double Now)
{
	const bool bQuiet = (Now - LastChangeSeconds) >= QuietSeconds;
	const bool bDeferredTooLong = (Now - FirstChangeSeconds) >= MaxDeferSeconds;

	if (!bQuiet && !bDeferredTooLong)
	{
		return true;
	}

	// FlushAt() disarms us, so returning false is only belt and braces.
	FlushAt(Now);
	return false;
}

int32 FVaCuusLiveReload::FlushAt(double Now)
{
	check(IsInGameThread());

	// DISARMED FIRST, unconditionally, and it is three things at once:
	//  - it makes this function's name honest -- an armed ticker left behind would keep
	//    polling and then flush an empty batch;
	//  - it happens BEFORE the reload, because the reload enqueues commands and logs, and
	//    anything in there that calls back into ArmDebounce() must be able to arm a FRESH
	//    batch rather than find a handle that is about to be dropped;
	//  - it keeps "the handle is valid exactly when a ticker is registered" true even when
	//    this is called from inside that very ticker's own delegate, which is what
	//    TickDebounceAt() does. FTSTicker::RemoveTicker handles self-removal explicitly: it
	//    spin-waits for a mid-flight delegate only when that delegate is on ANOTHER thread
	//    (Ticker.cpp:50-55), so this cannot deadlock. Without it, an out-of-band
	//    TickDebounceAt() would leave a registered ticker holding a raw `this` that nothing
	//    has a handle to unregister.
	if (DebounceTickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(DebounceTickerHandle);
		DebounceTickerHandle.Reset();
	}

	if (PendingChanges.Num() == 0)
	{
		return 0;
	}

	// DIAGNOSTIC ONLY, and worth saying because the shape invites the opposite reading:
	// this set does NOT drive which views reload. ReloadAllLiveViews() takes a reason
	// string and reloads every view that has a file document -- the granularity rule and
	// the reason for it live on that function's declaration.
	TArray<FString> Changed = PendingChanges.Array();
	Changed.Sort();
	PendingChanges.Empty();

	// THE BUNDLE SHADOW TRAP MADE LOUD (M6, spec 2(d)): with a bundle mounted --
	// `vacuus.Bundle.Enable 1` in PIE -- the VFS serves the PACKED copy of anything
	// the bundle contains, so the reload below will re-read the edited file and then
	// show the stale bundled bytes anyway, with no error anywhere. Live reload never
	// applies to bundle-served content (the watcher watches loose roots only; that is
	// documented, not "fixed"), so the one honest thing this flush can do is name the
	// shadow per changed file.
	for (const FString& ChangedPath : Changed)
	{
		for (const FString& Root : WatchedRoots)
		{
			if (!ChangedPath.StartsWith(Root + TEXT("/")))
			{
				continue;
			}
			FString BundleName;
			if (FVaCuusBundleMountTable::ContainsPath(ChangedPath.Mid(Root.Len() + 1), &BundleName))
			{
				UE_LOG(LogVaCuus, Warning,
					TEXT("Live reload: '%s' is SHADOWED by mounted bundle '%s' -- the reload will show the bundle's ")
					TEXT("packed copy, not this edit. `vacuus.Bundle.Enable 0` unmounts (loose files serve again); ")
					TEXT("`1` re-packs the tree with the edit in it"),
					*ChangedPath, *BundleName);
			}
			break;
		}
	}

	const double WaitedMs = (Now - FirstChangeSeconds) * 1000.0;

	// The one line the live proof reads: what changed, how long the debounce held it, and
	// how many views it reached.
	const int32 NumReloaded = ReloadAllLiveViews(TEXT("file change"));
	UE_LOG(LogVaCuus, Log,
		TEXT("Live reload flushed %d changed path(s) after %.0f ms and reloaded %d view(s): %s"),
		Changed.Num(), WaitedMs, NumReloaded, *FString::Join(Changed, TEXT(", ")));

	return NumReloaded;
}

int32 FVaCuusLiveReload::ReloadAllLiveViews(const TCHAR* Reason)
{
	check(IsInGameThread());

	// THE DISPATCH ITSELF IS NOT HERE ANY MORE, and moving it was the point rather than
	// tidying: the cache clear and the fan-out have to happen together (bead
	// VaCuus-akj.6.34), and while the only thing that paired them was this EDITOR-only
	// function, a runtime reload hook that called UVaCuusSubsystem's per-instance fan-out
	// because it read as the sanctioned entry point re-shipped the M2 bug verbatim -- RML
	// edits apply, RCSS edits silently do not. The pairing now lives in the runtime module
	// with the fan-out private behind it, so that call cannot be written at all.
	//
	// What is left here is what is genuinely editor-shaped: the watcher, the debounce, and
	// vacuus.ReloadUI. This wrapper stays because both of those call it by name and because
	// "live views" is the editor's word for the same set.
	return UVaCuusSubsystem::ClearAssetCachesAndReloadAllViews(Reason);
}

// `vacuus.ReloadUI` IS NO LONGER REGISTERED HERE (bead VaCuus-akj.6.18): the command MOVED to
// the runtime module (VaCuusSubsystem.cpp, beside vacuus.DumpModel), because an editor-only
// registration left `-game` and packaged Development builds -- the venues with no watcher at
// all -- without the one manual reload door. Moved rather than copied: IConsoleManager keeps
// the FIRST registration of a name and complains about the second, so a same-name twin here
// would silently shadow or be shadowed depending on module load order.
//
// The escape-hatch argument that used to justify the command travels with it and still names
// this watcher: the Linux backend skips IN_Q_OVERFLOW events outright instead of turning them
// into FCA_RescanRequired (DirectoryWatchRequestLinux.cpp:496 -- `(Event->wd != -1) &&
// (Event->mask & IN_Q_OVERFLOW) == 0`, with no log; FCA_RescanRequired appears nowhere in that
// backend), so after a bulk operation -- a git checkout, a tool that rewrites the whole DevUI
// tree -- changes are silently lost. The watcher is not lossless and must not be presented as
// such; the runtime command is what still works then, and it also covers the other gap that is
// OURS rather than the engine's: Start() skips a watch root that did not exist yet and never
// retries it. (The backend itself keeps up with a GROWING tree: on IN_CREATE|IN_ISDIR it calls
// WatchDirectoryTree on the new subtree and synthesizes FCA_Added for everything under it,
// :391-400, :225, :246-249.)
