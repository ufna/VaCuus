// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusLiveReload.h"

#include "VaCuusContentPaths.h"
#include "VaCuusDefines.h"
#include "VaCuusSubsystem.h"

#include "DirectoryWatcherModule.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

namespace VaCuusLiveReloadPrivate
{
/** Extensions RmlUi actually re-reads when a document is loaded again. */
static bool IsWatchedExtension(const FString& Extension)
{
	// Compared lowercase: FPaths::GetExtension preserves case and a file saved as
	// '.RCSS' is the same file to RmlUi's loader.
	return Extension.Equals(TEXT("rml"), ESearchCase::IgnoreCase) ||
		   Extension.Equals(TEXT("rcss"), ESearchCase::IgnoreCase);
}
}	 // namespace VaCuusLiveReloadPrivate

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

	UE_LOG(LogVaCuus, Log,
		TEXT("Live reload watching %d root(s): %s (debounce: %.0f ms of quiet, %.0f ms cap)"),
		WatchedRoots.Num(), *FString::Join(WatchedRoots, TEXT(" | ")), QuietSeconds * 1000.0, MaxDeferSeconds * 1000.0);
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

namespace VaCuusLiveReloadPrivate
{
/**
 * THE ESCAPE HATCH, and it is not optional: the Linux backend skips IN_Q_OVERFLOW events
 * outright instead of turning them into FCA_RescanRequired
 * (DirectoryWatchRequestLinux.cpp:496 -- `(Event->wd != -1) && (Event->mask &
 * IN_Q_OVERFLOW) == 0`, with no log; FCA_RescanRequired appears nowhere in that backend),
 * so after a bulk operation -- a git checkout, a tool that rewrites the whole DevUI tree --
 * changes are silently lost. The watcher is not lossless and must not be presented as such.
 *
 * Unconditional by design: it does not consult the pending set, the debounce or the watcher
 * at all, so it is also the thing that still works when a watch ROOT did not exist when
 * Start() ran. That gap is OURS, not the engine's -- Start() skips a missing root and never
 * retries it. The backend itself keeps up with a growing tree: on IN_CREATE|IN_ISDIR it
 * calls WatchDirectoryTree on the new subtree and synthesizes FCA_Added for the directory
 * and everything under it (:391-400, :225, :246-249), so a SUBDIRECTORY created while the
 * editor runs is watched.
 */
static void ReloadUICommand()
{
	const int32 NumReloaded = FVaCuusLiveReload::ReloadAllLiveViews(TEXT("vacuus.ReloadUI"));
	UE_LOG(LogVaCuus, Log, TEXT("vacuus.ReloadUI reloaded %d view(s)"), NumReloaded);
}

static FAutoConsoleCommand GReloadUICommand(
	TEXT("vacuus.ReloadUI"),
	TEXT("Re-load the current document of every live VaCuus view, dropping RmlUi's stylesheet/template caches first. ")
	TEXT("The manual counterpart to the file watcher, which is not lossless."),
	FConsoleCommandDelegate::CreateStatic(&ReloadUICommand));
}	 // namespace VaCuusLiveReloadPrivate
