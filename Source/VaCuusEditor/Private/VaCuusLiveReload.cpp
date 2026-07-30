// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusLiveReload.h"

#include "VaCuusContentPaths.h"
#include "VaCuusDefines.h"
#include "VaCuusSubsystem.h"
#include "VaCuusView.h"

#include "DirectoryWatcherModule.h"
#include "Engine/Engine.h"
#include "Engine/EngineTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
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

	if (!FModuleManager::Get().IsModuleLoaded(TEXT("DirectoryWatcher")) &&
		!FModuleManager::Get().LoadModule(TEXT("DirectoryWatcher")))
	{
		UE_LOG(LogVaCuus, Warning, TEXT("Live reload unavailable: the DirectoryWatcher module could not be loaded"));
		return;
	}

	FDirectoryWatcherModule& WatcherModule =
		FModuleManager::GetModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
	IDirectoryWatcher* Watcher = WatcherModule.Get();
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
		// FDirectoryWatchRequestLinux::Init returns true unconditionally and only warns when
		// inotify_add_watch fails (DirectoryWatchRequestLinux.cpp:114-117, :279), so
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
	// No IsInGameThread() check: this also runs from the destructor on a module-unload
	// path, and asserting there would turn a tidy-up into a crash.

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
		// Linux backend never produces it anyway -- IN_Q_OVERFLOW is dropped outright
		// (DirectoryWatchRequestLinux.cpp:542-543), which is why vacuus.ReloadUI exists.
		return false;
	}

	const FString BaseFilename = FPaths::GetCleanFilename(Filename);
	if (BaseFilename.IsEmpty())
	{
		return false;
	}

	// Editor temp/swap files, in the three shapes that actually turn up: a dotfile
	// (vim's .name.swp, and anything hidden), a trailing tilde (emacs, gedit, kate
	// backups), and a .tmp/.swp/#name# temp which the extension whitelist below rejects
	// on its own. Checked by name rather than by extension because '.m1_hud.rcss' has a
	// perfectly good extension and is still not the file anyone edited.
	if (BaseFilename.StartsWith(TEXT("."), ESearchCase::CaseSensitive) || BaseFilename.EndsWith(TEXT("~")))
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
	check(IsInGameThread());

	if (!ShouldTrackChange(Change.Action, Change.Filename))
	{
		return false;
	}

	const double Now = FPlatformTime::Seconds();
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

	DebounceTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		TEXT("VaCuus.LiveReload.Debounce"), DebouncePollSeconds, [this](float) { return TickDebounce(); });
}

bool FVaCuusLiveReload::TickDebounce()
{
	const double Now = FPlatformTime::Seconds();
	const bool bQuiet = (Now - LastChangeSeconds) >= QuietSeconds;
	const bool bDeferredTooLong = (Now - FirstChangeSeconds) >= MaxDeferSeconds;

	if (!bQuiet && !bDeferredTooLong)
	{
		return true;
	}

	// Cleared BEFORE the flush: the reload enqueues commands and can log, and re-entering
	// ArmDebounce() from anything that happens in there must be able to arm a fresh batch
	// rather than find a handle that is about to be reset.
	DebounceTickerHandle.Reset();
	FlushNow();
	return false;
}

int32 FVaCuusLiveReload::FlushNow()
{
	check(IsInGameThread());

	if (PendingChanges.Num() == 0)
	{
		return 0;
	}

	TArray<FString> Changed = PendingChanges.Array();
	Changed.Sort();
	PendingChanges.Empty();

	const double WaitedMs = (FPlatformTime::Seconds() - FirstChangeSeconds) * 1000.0;

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

	if (GEngine == nullptr)
	{
		return 0;
	}

	int32 NumReloaded = 0;
	int32 NumSubsystems = 0;

	// GetWorldContexts(), not GEditor->PlayWorld or GetPIEWorldContext(): both of those
	// see only PIE instance 0 (EditorEngine.cpp:6401-6412 and its own doc comment), so a
	// multi-client PIE session would get one window reloaded and the others left stale.
	// Re-resolved on every flush rather than cached, because a game instance and its
	// subsystems are destroyed on EndPIE and a kept pointer would dangle into the next
	// session.
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		// Game as well as PIE: `-game` in the editor process and the standalone game
		// instance an automation test builds with InitializeStandalone() are both
		// EWorldType::Game, and there is no reason a live view there should not reload.
		if (Context.WorldType != EWorldType::PIE && Context.WorldType != EWorldType::Game)
		{
			continue;
		}

		UVaCuusSubsystem* Subsystem = UGameInstance::GetSubsystem<UVaCuusSubsystem>(Context.OwningGameInstance);
		if (Subsystem == nullptr)
		{
			// Legitimate: a context can exist before or after its world during PIE
			// start/teardown, and the subsystem may simply not have been created.
			continue;
		}

		++NumSubsystems;
		NumReloaded += Subsystem->ReloadAllDocuments();
	}

	UE_LOG(LogVaCuus, Verbose, TEXT("Live reload (%s): %d view(s) across %d game instance(s)"),
		Reason, NumReloaded, NumSubsystems);
	return NumReloaded;
}

namespace VaCuusLiveReloadPrivate
{
/**
 * THE ESCAPE HATCH, and it is not optional: the Linux backend drops IN_Q_OVERFLOW events
 * outright instead of turning them into FCA_RescanRequired
 * (DirectoryWatchRequestLinux.cpp:542-543), so after a bulk operation -- a git checkout, a
 * tool that rewrites the whole DevUI tree -- changes are silently lost. The watcher is not
 * lossless and must not be presented as such.
 *
 * Unconditional by design: it does not consult the pending set, the debounce or the
 * watcher at all, so it is also the thing that still works when the watch root was created
 * after the editor started (Linux registers a watch per directory at registration time).
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
