// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusBundle.h"
#include "VaCuusBundleMount.h"
#include "VaCuusContentPaths.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusLiveReload.h"
#include "VaCuusSubsystem.h"
#include "VaCuusTestNullDocumentHost.h"
#include "VaCuusUIThread.h"
#include "VaCuusView.h"
#include "VaCuusViewStatus.h"

#include "DirectoryWatcherModule.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusLiveReloadTest
{
/**
 * Captures GLog across a scope so a test can assert what a line SAID, not merely that one
 * arrived. AddExpectedMessagePlain answers "did a Warning matching X occur"; it cannot answer
 * "did it name this path and this bundle", which is the whole content of the shadow warning.
 *
 * A near-twin of VaCuusBundlePackTest's, and deliberately not shared: that one exists because
 * its line is Log-level and therefore invisible to the automation machinery
 * (AutomationTest.cpp:233 routes only Error/Warning/Display), this one because the assertion
 * is about the text. Lifting them into a common test header would make one helper answer to
 * two different arguments, and neither file would carry its own reason any more.
 */
class FScopedLiveReloadLogCapture final : public FOutputDevice
{
public:
	FScopedLiveReloadLogCapture() { GLog->AddOutputDevice(this); }
	virtual ~FScopedLiveReloadLogCapture() override { GLog->RemoveOutputDevice(this); }

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		FScopeLock Lock(&Mutex);
		Lines.Add(V);
	}

	bool Contains(const TCHAR* Fragment) { return ContainsAllOnOneLine({Fragment}); }

	/**
	 * True when ONE captured line contains every fragment.
	 *
	 * The one-line part is the assertion, not a convenience. Checking fragments independently
	 * let this file's shadow test pass a control it should have failed: the changed path also
	 * appears in the flush's ordinary "flushed N changed path(s)" diagnostic, so "the capture
	 * contains the path" stayed true with the shadow warning deleted. Found by deleting it.
	 */
	bool ContainsAllOnOneLine(std::initializer_list<const TCHAR*> Fragments)
	{
		// Threaded logging delivers from the log thread; the flush is what makes "emitted
		// before this line" a checkable claim rather than a race.
		GLog->FlushThreadedLogs();
		FScopeLock Lock(&Mutex);
		for (const FString& Line : Lines)
		{
			bool bAll = true;
			for (const TCHAR* Fragment : Fragments)
			{
				bAll = bAll && Line.Contains(Fragment);
			}
			if (bAll)
			{
				return true;
			}
		}
		return false;
	}

private:
	FCriticalSection Mutex;
	TArray<FString> Lines;
};

//~ EVERY VIEW BELOW IS BUILT ON FVaCuusTestNullDocumentHost -- no context, no RmlUi -- and that
//~ is enough deliberately: what the dispatch tests assert is a GAME-THREAD fact ("the reload
//~ dispatcher re-issued a load for this view"), observed through
//~ UVaCuusView::GetLastRequestedLoadSerial(), which the view stamps before the command ever
//~ reaches the UI thread. Giving the host a real context would add an RmlUi boot and a document
//~ parse to a test that would assert nothing more. (VaCuusEditor could not have a real-context
//~ host anyway: it does not link VaCuusRml.)

/**
 * A standalone UGameInstance carrying a live UVaCuusSubsystem, torn down in the order the
 * subsystem needs (Shutdown() clears the world context pointer, so the world is taken
 * first).
 *
 * BUILT THE HARD WAY for the reason VaCuus.UMG.Widget spells out: a bare
 * NewObject<UGameInstance>() has an EMPTY subsystem collection, so there would be no
 * UVaCuusSubsystem to find. UGameInstance::InitializeStandalone() creates a world CONTEXT
 * (EWorldType::Game -- GameInstance.cpp:193), a world, and then runs Init(), which
 * initializes the collection. That world type is also the whole reason
 * FVaCuusLiveReload::ReloadAllLiveViews accepts Game as well as PIE: it is what lets these
 * tests exercise the real GEngine->GetWorldContexts() walk rather than a hand-fed subsystem.
 */
struct FStandaloneInstance
{
	TStrongObjectPtr<UGameInstance> GameInstance;
	UVaCuusSubsystem* Subsystem = nullptr;

	FStandaloneInstance()
	{
		GameInstance.Reset(NewObject<UGameInstance>(GEngine));
		GameInstance->InitializeStandalone();
		Subsystem = GameInstance->GetSubsystem<UVaCuusSubsystem>();
	}

	~FStandaloneInstance()
	{
		UWorld* World = GameInstance->GetWorld();
		GameInstance->Shutdown();
		if (World)
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
	}

	FStandaloneInstance(const FStandaloneInstance&) = delete;
	FStandaloneInstance& operator=(const FStandaloneInstance&) = delete;
};

/**
 * The preconditions every test below shares. Returns a reason to skip, or empty.
 *
 * Skips rather than fails: a session without threads or without Slate cannot host a game
 * instance at all, and reporting that as a live-reload failure would be a lie.
 */
static FString WhySkip()
{
	if (!FPlatformProcess::SupportsMultithreading())
	{
		return TEXT("no multithreading support, so there is no worker thread to drive");
	}
	if (!FSlateApplication::IsInitialized())
	{
		// UGameInstance::Init() calls FSlateApplication::Get() unconditionally to register
		// its console command listener.
		return TEXT("no FSlateApplication, so a game instance cannot be initialized");
	}
	if (GEngine == nullptr)
	{
		return TEXT("no GEngine");
	}
	return FString();
}

/**
 * The precondition every test that starts a UI thread shares, and a FAILURE rather than a
 * skip: it describes a session that is perfectly capable of running the test, in which
 * somebody else has taken the one resource it needs.
 *
 * FVaCuusModule::GetOrStartUIThread() boots RmlUi on the worker it spawns and claims
 * ownership of the library there (FVaCuusUIThread::Init), so a session that already holds
 * it -- a PIE game with vacuus.M1HUD up, which is exactly the state somebody runs these
 * from in the Session Frontend -- makes that boot fail. The test then gets a null thread
 * and reports "UI thread" instead of the real reason, and its ON_SCOPE_EXIT
 * Module.StopUIThread() would stop a thread it did not start.
 */
static bool TestRmlUiIsDown(FAutomationTestBase& Test)
{
	return Test.TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized());
}
}	 // namespace VaCuusLiveReloadTest

/**
 * The filter, the path normalisation and the debounce COALESCING -- the set behaviour.
 *
 * Its two neighbours cover the rest: VaCuus.LiveReload.Debounce asserts the TIMING
 * contract (the same batch, judged at chosen times), and VaCuus.LiveReload.WatcherEvent
 * asserts that a real inotify event arrives at all. This one drives NoteChange() directly,
 * exactly as the delegate does, and needs neither a watcher nor a clock.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusLiveReloadFilterTest, "VaCuus.LiveReload.Filter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusLiveReloadFilterTest::RunTest(const FString& Parameters)
{
	using EAction = FFileChangeData::EFileChangeAction;

	//~ Actions: only Added and Modified are reloadable events.
	TestTrue(TEXT("Modified .rcss is tracked"),
		FVaCuusLiveReload::ShouldTrackChange(EAction::FCA_Modified, TEXT("DevUI/m1_hud.rcss")));
	TestTrue(TEXT("Added .rml is tracked (write-then-rename arrives as IN_MOVED_TO -> FCA_Added)"),
		FVaCuusLiveReload::ShouldTrackChange(EAction::FCA_Added, TEXT("DevUI/m1_hud.rml")));
	TestFalse(TEXT("Removed is not tracked"),
		FVaCuusLiveReload::ShouldTrackChange(EAction::FCA_Removed, TEXT("DevUI/m1_hud.rcss")));
	TestFalse(TEXT("RescanRequired is not tracked (it carries no usable filename)"),
		FVaCuusLiveReload::ShouldTrackChange(EAction::FCA_RescanRequired, TEXT("DevUI/m1_hud.rcss")));
	TestFalse(TEXT("Unknown is not tracked"),
		FVaCuusLiveReload::ShouldTrackChange(EAction::FCA_Unknown, TEXT("DevUI/m1_hud.rcss")));

	//~ Editor temp/swap files. Each of these is a shape a real editor produces next to the
	//~ file you actually saved, and reloading on one costs a re-parse for nothing. Labelled
	//~ by WHICH RULE rejects each one, because three of the four never reach the dotfile
	//~ test: the extension is the last dot-suffix, so 'rcss~', 'tmp' and 'swp' all fail the
	//~ whitelist first. Only the dotfile with a good extension exercises the dotfile rule --
	//~ which is why that rule is the one that has to exist.
	TestFalse(TEXT("Emacs/gedit backup is skipped -- extension is 'rcss~', not on the whitelist"),
		FVaCuusLiveReload::ShouldTrackChange(EAction::FCA_Modified, TEXT("DevUI/m1_hud.rcss~")));
	TestFalse(TEXT(".tmp is skipped -- extension is 'tmp', not on the whitelist"),
		FVaCuusLiveReload::ShouldTrackChange(EAction::FCA_Modified, TEXT("DevUI/m1_hud.rcss.tmp")));
	TestFalse(TEXT("vim swap file is skipped -- extension is 'swp', not on the whitelist"),
		FVaCuusLiveReload::ShouldTrackChange(EAction::FCA_Added, TEXT("DevUI/.m1_hud.rcss.swp")));
	TestFalse(TEXT("THE DOTFILE RULE: a hidden file whose extension IS on the whitelist is still skipped"),
		FVaCuusLiveReload::ShouldTrackChange(EAction::FCA_Modified, TEXT("DevUI/.m1_hud.rcss")));

	//~ Extensions. Images are excluded on purpose: textures are not released on reload
	//~ (see FVaCuusLiveReload's header note), so tracking a .png would promise something
	//~ the reload does not deliver.
	TestFalse(TEXT(".png is not tracked (textures are not released on reload)"),
		FVaCuusLiveReload::ShouldTrackChange(EAction::FCA_Modified, TEXT("DevUI/img/avatar.png")));
	TestFalse(TEXT(".ttf is not tracked (fonts are loaded once at boot)"),
		FVaCuusLiveReload::ShouldTrackChange(EAction::FCA_Modified, TEXT("DevUI/fonts/LatoLatin-Regular.ttf")));
	TestTrue(TEXT("Extension match is case-insensitive"),
		FVaCuusLiveReload::ShouldTrackChange(EAction::FCA_Modified, TEXT("DevUI/M1_HUD.RCSS")));
	TestFalse(TEXT("An empty filename is not tracked"),
		FVaCuusLiveReload::ShouldTrackChange(EAction::FCA_Modified, FString()));

	//~ Scripts (M4 Task 7): a .js/.mjs edit triggers the same full-document reload as an
	//~ .rcss one -- the replace recycles the JSContext and the per-context module cache
	//~ dies with it (IsWatchedExtension's comment has the whole argument). Before Task 7
	//~ these two returned false and script edits reloaded nothing.
	TestTrue(TEXT(".js is tracked (Task 7: script edits ride the document reload)"),
		FVaCuusLiveReload::ShouldTrackChange(EAction::FCA_Modified, TEXT("DevUI/hud_logic.js")));
	TestTrue(TEXT(".mjs is tracked (Task 7: the module-entry convention)"),
		FVaCuusLiveReload::ShouldTrackChange(EAction::FCA_Modified, TEXT("DevUI/hud_main.mjs")));

	//~ Normalisation. FFileChangeData's constructor runs FPaths::MakeStandardFilename, so
	//~ what the delegate receives is usually relative -- comparing or reloading it as-is
	//~ is the bug this exists to prevent.
	{
		const FString Relative = TEXT("../../../YourProject/Content/DevUI/m1_hud.rcss");
		const FString Normalized = FVaCuusLiveReload::NormalizeChangedPath(Relative);
		TestFalse(TEXT("A relative change path normalises to absolute"), FPaths::IsRelative(Normalized));
		TestTrue(TEXT("Normalisation keeps the file name"), Normalized.EndsWith(TEXT("m1_hud.rcss")));

		const FString Absolute = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("x.rcss"));
		TestEqual(TEXT("An already-absolute change path is unchanged"),
			FVaCuusLiveReload::NormalizeChangedPath(Absolute), Absolute);
	}

	//~ Debounce coalescing: one save is a BURST of IN_MODIFY events for the same file, and
	//~ the point of the set is that the burst costs one reload.
	{
		FVaCuusLiveReload Reload;
		TestEqual(TEXT("Nothing pending initially"), Reload.GetNumPendingChanges(), 0);
		TestFalse(TEXT("Debounce is not armed initially"), Reload.IsDebouncePending());

		const FString Rcss = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("DevUI/m1_hud.rcss"));
		const FString Rml = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("DevUI/m1_hud.rml"));

		for (int32 Index = 0; Index < 5; ++Index)
		{
			TestTrue(TEXT("Repeat modification is tracked"),
				Reload.NoteChange(FFileChangeData(Rcss, EAction::FCA_Modified)));
		}
		TestEqual(TEXT("Five events on one file coalesce to one pending path"), Reload.GetNumPendingChanges(), 1);
		TestTrue(TEXT("Debounce armed by the first tracked change"), Reload.IsDebouncePending());

		Reload.NoteChange(FFileChangeData(Rml, EAction::FCA_Modified));
		TestEqual(TEXT("A second distinct file adds one pending path"), Reload.GetNumPendingChanges(), 2);

		TestFalse(TEXT("A filtered event is not tracked"),
			Reload.NoteChange(FFileChangeData(Rcss + TEXT("~"), EAction::FCA_Modified)));
		TestEqual(TEXT("A filtered event does not add a pending path"), Reload.GetNumPendingChanges(), 2);

		// No PIE world here, so this reloads nothing -- what is being asserted is that the
		// flush CONSUMES the batch (a batch left behind would reload forever). The real
		// clock rather than an injected one because nothing here depends on the value: the
		// TIMING contract is VaCuus.LiveReload.Debounce's, driven through TickDebounceAt().
		Reload.FlushAt(FPlatformTime::Seconds());
		TestEqual(TEXT("Flush consumes the batch"), Reload.GetNumPendingChanges(), 0);
		TestEqual(TEXT("Flushing an empty batch is a no-op"), Reload.FlushAt(FPlatformTime::Seconds()), 0);

		Reload.Shutdown();
		TestFalse(TEXT("Shutdown drops the debounce ticker"), Reload.IsDebouncePending());
	}

	//~ THE LINUX PITFALL, verified rather than quoted: RegisterDirectoryChangedCallback_Handle
	//~ hands back a VALID HANDLE for a directory that does not exist, because
	//~ FDirectoryWatchRequestLinux::Init rejects only an empty path and an inotify_init1
	//~ failure (DirectoryWatchRequestLinux.cpp:81-85, :99-110) -- for a non-existent
	//~ directory it walks the tree and returns true (:114-116), and inotify_add_watch
	//~ failures only warn (:268-282). That is why FVaCuusLiveReload::Start() calls
	//~ DirectoryExists() itself and never treats this bool as a health check. If a future
	//~ engine version fixes this, this test is where it shows up.
	{
		FDirectoryWatcherModule& WatcherModule =
			FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
		IDirectoryWatcher* Watcher = WatcherModule.Get();
		if (TestNotNull(TEXT("Directory watcher available"), Watcher))
		{
			const FString Missing = FPaths::ConvertRelativePathToFull(
				FPaths::ProjectSavedDir() / TEXT("VaCuusLiveReload_NoSuchDirectory"));
			TestFalse(TEXT("The probe directory really is absent"), IFileManager::Get().DirectoryExists(*Missing));

			FDelegateHandle Handle;
			const bool bRegistered = Watcher->RegisterDirectoryChangedCallback_Handle(
				Missing, IDirectoryWatcher::FDirectoryChanged::CreateLambda([](const TArray<FFileChangeData>&) {}),
				Handle, 0);

			AddInfo(FString::Printf(
				TEXT("Registering a watch on a NON-EXISTENT directory returned %s with a %s handle"),
				bRegistered ? TEXT("true") : TEXT("false"), Handle.IsValid() ? TEXT("valid") : TEXT("invalid")));

#if PLATFORM_LINUX
			TestTrue(TEXT("Linux registers a watch on a missing directory and reports success (the pitfall)"), bRegistered);
			TestTrue(TEXT("Linux hands back a valid handle for a missing directory (the pitfall)"), Handle.IsValid());
#endif

			if (Handle.IsValid())
			{
				Watcher->UnregisterDirectoryChangedCallback_Handle(Missing, Handle);
			}
		}
	}

	return true;
}

/**
 * THE SECOND-WORKING-TREE DETECTOR (bead VaCuus-akj.6.22), which exists because the failure
 * it reports is otherwise completely silent: two checkouts of this plugin are two sets of
 * inodes, inotify watches inodes, and an edit made in the tree the editor did NOT load
 * produces no event, no reload and no log line at all.
 *
 * WHAT IS TESTED IS THE GRAMMAR, which is the only fragile part. The detector's evidence is
 * a git remote whose url is a local absolute path to a directory that has its own
 * Content/DevUI -- a fact read off disk -- but reading it means parsing git's config format
 * by hand, and a parser that quietly stopped matching would restore the exact silence the
 * check was added to end, while still logging nothing. So every discriminating case gets an
 * assertion, in BOTH directions.
 *
 * Real directories, not mocks: the last gate is DirectoryExists, so a fake path would make
 * every positive case unreachable and the test would pass by never testing anything.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusLiveReloadSecondTreeTest, "VaCuus.LiveReload.SecondTree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusLiveReloadSecondTreeTest::RunTest(const FString& Parameters)
{
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VaCuusSecondTreeTest"));
	const FString OtherTree = Root / TEXT("other");
	const FString OtherDevUI = OtherTree / TEXT("Content") / TEXT("DevUI");
	const FString BareMirror = Root / TEXT("mirror");
	const FString ThisTree = Root / TEXT("this");
	const FString ConfigPath = Root / TEXT("config");

	IFileManager& Files = IFileManager::Get();
	Files.DeleteDirectory(*Root, /*bRequireExists=*/false, /*bTree=*/true);
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*Root, /*bRequireExists=*/false, /*bTree=*/true);
	};

	// The other tree has documents; the mirror exists but has none; ThisTree stands in for
	// the plugin directory itself and has documents, so the only thing that can exclude it is
	// the self-comparison.
	if (!TestTrue(TEXT("Fixture directories created"),
			Files.MakeDirectory(*OtherDevUI, /*Tree=*/true) && Files.MakeDirectory(*BareMirror, /*Tree=*/true) &&
				Files.MakeDirectory(*(ThisTree / TEXT("Content") / TEXT("DevUI")), /*Tree=*/true)))
	{
		return false;
	}

	const auto WriteConfig = [&ConfigPath](const FString& Body) { return FFileHelper::SaveStringToFile(Body, *ConfigPath); };
	const auto Detect = [&ConfigPath, &ThisTree]() { return FVaCuusLiveReload::FindSecondWorkingTreeDevUI(ConfigPath, ThisTree); };

	//~ A missing config is the ordinary case for a plugin that is not its own checkout.
	TestTrue(TEXT("No git config at all: nothing to report"),
		FVaCuusLiveReload::FindSecondWorkingTreeDevUI(Root / TEXT("no_such_config"), ThisTree).IsEmpty());

	//~ Ordinary upstream remotes. None of these is a second working tree, and answering
	//~ otherwise would put a false warning in front of every user of the plugin.
	if (!TestTrue(TEXT("Config written"),
			WriteConfig(FString::Printf(TEXT("[remote \"origin\"]\n\turl = git@github.com:ufna/VaCuus.git\n")
										TEXT("[remote \"https\"]\n\turl = https://github.com/ufna/VaCuus.git\n")
										TEXT("[remote \"ssh\"]\n\turl = ssh://git@host/srv/VaCuus.git\n")))))
	{
		return false;
	}
	TestTrue(TEXT("A git@/https/ssh remote is not a local working tree"), Detect().IsEmpty());

	//~ THE CASE THIS EXISTS FOR: a local clone-of-a-clone.
	WriteConfig(FString::Printf(TEXT("[remote \"origin\"]\n\turl = %s\n"), *OtherTree));
	TestEqual(TEXT("A local remote with its own Content/DevUI is the second tree"), Detect(), OtherDevUI);

	//~ ...and it is found among several remotes, not only as the first line.
	WriteConfig(FString::Printf(TEXT("[core]\n\tbare = false\n[remote \"upstream\"]\n\turl = git@github.com:ufna/VaCuus.git\n")
								TEXT("[remote \"local\"]\n\turl = %s\n"),
		*OtherTree));
	TestEqual(TEXT("It is found on any remote, not just the first"), Detect(), OtherDevUI);

	//~ A bare mirror is a legitimate local remote with nothing anyone can edit.
	WriteConfig(FString::Printf(TEXT("[remote \"origin\"]\n\turl = %s\n"), *BareMirror));
	TestTrue(TEXT("A local remote with no Content/DevUI is not a working tree"), Detect().IsEmpty());

	//~ A remote pointing at this very directory is not a SECOND anything.
	WriteConfig(FString::Printf(TEXT("[remote \"origin\"]\n\turl = %s\n"), *ThisTree));
	TestTrue(TEXT("A remote pointing at the plugin itself is not a second tree"), Detect().IsEmpty());

	//~ A trailing slash is the same directory; ConvertRelativePathToFull normalises it away.
	WriteConfig(FString::Printf(TEXT("[remote \"origin\"]\n\turl = %s/\n"), *ThisTree));
	TestTrue(TEXT("...with a trailing slash too"), Detect().IsEmpty());

	//~ A commented-out url must not match: the key is then '# url', and the compare is exact.
	WriteConfig(FString::Printf(TEXT("[remote \"origin\"]\n\t# url = %s\n\t; url = %s\n"), *OtherTree, *OtherTree));
	TestTrue(TEXT("A commented-out url is not a remote"), Detect().IsEmpty());

	//~ pushurl IS a remote URL: fetch from upstream, push to a local clone is still two
	//~ working trees. Case-insensitively, because that is how git compares its key names.
	WriteConfig(FString::Printf(TEXT("[remote \"origin\"]\n\turl = git@github.com:ufna/VaCuus.git\n\tpushurl = %s\n"),
		*OtherTree));
	TestEqual(TEXT("A local pushurl counts as well"), Detect(), OtherDevUI);
	WriteConfig(FString::Printf(TEXT("[remote \"origin\"]\n\tURL = %s\n"), *OtherTree));
	TestEqual(TEXT("Key names are matched case-insensitively"), Detect(), OtherDevUI);

	//~ A key that merely CONTAINS 'url' is not one of the two.
	WriteConfig(FString::Printf(TEXT("[remote \"origin\"]\n\turlx = %s\n"), *OtherTree));
	TestTrue(TEXT("'urlx' is neither 'url' nor 'pushurl'"), Detect().IsEmpty());

	return true;
}

/**
 * THE DEBOUNCE TIMING CONTRACT -- the only arithmetic in this feature, and until this test
 * existed the only part of it that was unexercised (VaCuus.LiveReload.Filter asserts the
 * SET behaviour, which is a different thing).
 *
 * NO SLEEPING, and that is the point: the clock is injected through NoteChangeAt() and
 * TickDebounceAt(), so what is asserted is the arithmetic rather than the scheduler. A test
 * that slept for QuietSeconds would pass on a loaded machine only by luck and would say
 * nothing about the batch boundary at all.
 *
 * Times are relative to a T0 far from zero, so nothing here can pass by accident because a
 * timestamp defaulted to 0.0.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusLiveReloadDebounceTest, "VaCuus.LiveReload.Debounce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusLiveReloadDebounceTest::RunTest(const FString& Parameters)
{
	using EAction = FFileChangeData::EFileChangeAction;

	const FString Rcss = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("DevUI/m1_hud.rcss"));
	const FString Rml = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("DevUI/m1_hud.rml"));
	const double T0 = 10000.0;

	// Nudges a "just past the boundary" time past it. The thresholds are >=, but T0 + a + b
	// is not exactly T0 + (a + b) in doubles, so landing exactly ON a boundary would assert
	// the rounding of one addition rather than the rule.
	const double Epsilon = 0.001;

	//~ (a) NOT YET QUIET: the ticker keeps polling and the batch is still pending. This is
	//~ the case a burst of IN_MODIFY events for one save lands in.
	{
		FVaCuusLiveReload Reload;
		ON_SCOPE_EXIT
		{
			Reload.Shutdown();
		};

		TestTrue(TEXT("(a) The change is tracked"), Reload.NoteChangeAt(FFileChangeData(Rcss, EAction::FCA_Modified), T0));
		TestTrue(TEXT("(a) A tracked change arms the ticker"), Reload.IsDebouncePending());

		const double NotQuietYet = T0 + FVaCuusLiveReload::QuietSeconds * 0.5;
		TestTrue(TEXT("(a) Still inside the quiet window: keep ticking"), Reload.TickDebounceAt(NotQuietYet));
		TestEqual(TEXT("(a) ...and the batch is untouched"), Reload.GetNumPendingChanges(), 1);
		TestTrue(TEXT("(a) ...and the ticker is still armed"), Reload.IsDebouncePending());
	}

	//~ (b) QUIET: flushes exactly once, consumes the batch, and retires the ticker.
	{
		FVaCuusLiveReload Reload;
		ON_SCOPE_EXIT
		{
			Reload.Shutdown();
		};

		Reload.NoteChangeAt(FFileChangeData(Rcss, EAction::FCA_Modified), T0);
		Reload.NoteChangeAt(FFileChangeData(Rml, EAction::FCA_Modified), T0 + 0.01);
		TestEqual(TEXT("(b) Two distinct paths in one batch"), Reload.GetNumPendingChanges(), 2);

		const double Quiet = T0 + 0.01 + FVaCuusLiveReload::QuietSeconds + Epsilon;
		TestFalse(TEXT("(b) Quiet: the ticker retires itself"), Reload.TickDebounceAt(Quiet));
		TestEqual(TEXT("(b) ...having consumed the whole batch"), Reload.GetNumPendingChanges(), 0);
		TestFalse(TEXT("(b) ...and disarmed"), Reload.IsDebouncePending());

		// Once, not once per poll: a second tick has nothing left to flush. (A batch left
		// behind would reload forever, which is what VaCuus.LiveReload.Filter also guards.)
		TestFalse(TEXT("(b) A tick after the flush finds nothing and retires"), Reload.TickDebounceAt(Quiet + 1.0));
		TestEqual(TEXT("(b) Nothing pending after that either"), Reload.GetNumPendingChanges(), 0);
	}

	//~ (c) THE HARD CAP, which is the case the quiet window alone cannot serve: a file being
	//~ rewritten continuously (a generator, a long `git checkout`) never goes quiet, so
	//~ MaxDeferSeconds from the FIRST change of the batch forces it to the screen.
	{
		FVaCuusLiveReload Reload;
		ON_SCOPE_EXIT
		{
			Reload.Shutdown();
		};

		double Now = T0;
		Reload.NoteChangeAt(FFileChangeData(Rcss, EAction::FCA_Modified), Now);

		// Changes keep streaming in at half the quiet window, so it is never quiet.
		const double Step = FVaCuusLiveReload::QuietSeconds * 0.5;
		int32 NumTicks = 0;
		while (Now < T0 + FVaCuusLiveReload::MaxDeferSeconds - Step)
		{
			Now += Step;
			Reload.NoteChangeAt(FFileChangeData(Rcss, EAction::FCA_Modified), Now);
			if (!Reload.TickDebounceAt(Now))
			{
				break;
			}
			++NumTicks;
		}

		TestTrue(TEXT("(c) It never went quiet, so nothing flushed early"), Reload.GetNumPendingChanges() == 1);
		TestTrue(TEXT("(c) ...and it really did keep ticking"), NumTicks > 0);

		// Still not quiet -- the change is 1 ms old -- but the batch is now over the cap.
		Now = T0 + FVaCuusLiveReload::MaxDeferSeconds;
		Reload.NoteChangeAt(FFileChangeData(Rcss, EAction::FCA_Modified), Now);
		TestFalse(TEXT("(c) Over the cap while changes are still streaming: flush anyway"),
			Reload.TickDebounceAt(Now + 0.001));
		TestEqual(TEXT("(c) ...and the batch is consumed"), Reload.GetNumPendingChanges(), 0);
	}

	//~ (d) THE BATCH BOUNDARY, and the subtle one: a change arriving long after a flush must
	//~ be judged against ITS OWN batch start. If FirstChangeSeconds were left at the previous
	//~ batch's value, this change would look "deferred too long" the instant it arrived and
	//~ every later edit would flush with no debounce at all.
	{
		FVaCuusLiveReload Reload;
		ON_SCOPE_EXIT
		{
			Reload.Shutdown();
		};

		Reload.NoteChangeAt(FFileChangeData(Rcss, EAction::FCA_Modified), T0);
		const double FirstFlush = T0 + FVaCuusLiveReload::QuietSeconds + Epsilon;
		TestFalse(TEXT("(d) First batch flushes"), Reload.TickDebounceAt(FirstFlush));

		// Far more than MaxDeferSeconds later, so a stale batch start would be decisive.
		const double Later = FirstFlush + FVaCuusLiveReload::MaxDeferSeconds * 5.0;
		Reload.NoteChangeAt(FFileChangeData(Rml, EAction::FCA_Modified), Later);
		TestTrue(TEXT("(d) The new change re-arms the ticker"), Reload.IsDebouncePending());
		TestTrue(TEXT("(d) It is judged against a FRESH batch start, so it still debounces"),
			Reload.TickDebounceAt(Later + FVaCuusLiveReload::QuietSeconds * 0.5));
		TestEqual(TEXT("(d) ...and is still pending"), Reload.GetNumPendingChanges(), 1);

		TestFalse(TEXT("(d) ...until its own quiet window passes"),
			Reload.TickDebounceAt(Later + FVaCuusLiveReload::QuietSeconds + Epsilon));
		TestEqual(TEXT("(d) ...then it flushes"), Reload.GetNumPendingChanges(), 0);
	}

	//~ (e) THE PROOF'S 190 ms, AS ARITHMETIC (bead VaCuus-akj.6.24). M2 measured 191 ms against
	//~ an acceptance step that said "~200 ms"; this reproduces the measured figure from the
	//~ constants, so the number stops being a one-off observation someone can read 9 ms of
	//~ headroom into. The batch is the shape the proof recorded -- one save, FOUR inotify
	//~ events for one file, spanning 40 ms -- and the earliest legal flush is therefore
	//~ LastChange + QuietSeconds, i.e. FirstChange + 190 ms. Trim QuietSeconds and this fails,
	//~ pointing at docs/research/proofs/m2-t10-live-reload/README.md, which is where the
	//~ trade-off (a shorter window reloads half-written files) is written down.
	{
		FVaCuusLiveReload Reload;
		ON_SCOPE_EXIT
		{
			Reload.Shutdown();
		};

		const double BurstSpread = 0.040;
		const double BurstAt[] = {T0, T0 + 0.013, T0 + 0.026, T0 + BurstSpread};
		for (const double At : BurstAt)
		{
			Reload.NoteChangeAt(FFileChangeData(Rcss, EAction::FCA_Modified), At);
		}
		TestEqual(TEXT("(e) One save's four events are one pending path"), Reload.GetNumPendingChanges(), 1);

		// The burst is NOT over the cap, so the quiet window is what decides -- which is the
		// case the proof was in and the case every ordinary save is in.
		TestTrue(TEXT("(e) The burst is far short of the hard cap"),
			BurstSpread + FVaCuusLiveReload::QuietSeconds < FVaCuusLiveReload::MaxDeferSeconds);

		const double EarliestFlush = T0 + BurstSpread + FVaCuusLiveReload::QuietSeconds;

		// THE ABSOLUTE NUMBER, SPELLED OUT, and it is the only line here that pins anything:
		// every other assertion in this case is computed from QuietSeconds and would follow it
		// wherever it went -- a tautology dressed as a regression test. 190 is the figure the
		// M2 proof recorded and the figure the "~200 ms" step was judged against, so THIS is
		// what fails if the constant moves, and the failure is the prompt to re-read the
		// trade-off in that proof rather than to re-baseline the number.
		// RoundToInt32, not RoundToInt: the latter's return width makes the TestEqual overload
		// set ambiguous against an int literal.
		const int32 FloorMs = FMath::RoundToInt32((EarliestFlush - T0) * 1000.0);
		TestEqual(TEXT("(e) The floor for this burst is the proof's 190 ms"), FloorMs, 190);

		TestTrue(TEXT("(e) A tick one ms before that does NOT flush"),
			Reload.TickDebounceAt(EarliestFlush - Epsilon));
		TestEqual(TEXT("(e) ...the batch is still pending"), Reload.GetNumPendingChanges(), 1);

		TestFalse(TEXT("(e) ...and at the boundary it flushes"), Reload.TickDebounceAt(EarliestFlush + Epsilon));
		TestEqual(TEXT("(e) ...consuming the batch"), Reload.GetNumPendingChanges(), 0);

		// The reported figure, which is what FlushAt() prints and what the proof recorded.
		AddInfo(FString::Printf(
			TEXT("(e) Earliest reportable latency for a %.0f ms save burst: %.0f ms ")
			TEXT("(= QuietSeconds %.0f + burst %.0f), before poll granularity. M2's proof recorded 190 ms."),
			BurstSpread * 1000.0, (EarliestFlush - T0) * 1000.0,
			FVaCuusLiveReload::QuietSeconds * 1000.0, BurstSpread * 1000.0));
	}

	return true;
}

/**
 * The dispatch path, end to end on the game thread: a live view with a file document is
 * found through the world contexts and its load is re-issued.
 *
 * The game instance comes from FStandaloneInstance -- see its comment for why it is built
 * the hard way and why its EWorldType::Game context is what makes this a real
 * GetWorldContexts() walk rather than a hand-fed subsystem.
 *
 * NOT COVERED: multi-client PIE (several contexts at once). The GetWorldContexts() loop is
 * the same code either way; what a second context would add is only a second iteration.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusLiveReloadDispatchTest, "VaCuus.LiveReload.Dispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusLiveReloadDispatchTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusLiveReloadTest;

	const FString SkipReason = WhySkip();
	if (!SkipReason.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("Skipped: %s"), *SkipReason));
		return true;
	}

	if (!TestRmlUiIsDown(*this))
	{
		return false;
	}

	FVaCuusModule& Module = FVaCuusModule::Get();
	FVaCuusUIThread* UIThread = Module.GetOrStartUIThread();
	if (!TestNotNull(TEXT("UI thread"), UIThread))
	{
		return false;
	}

	// Declared first so it runs LAST: the world teardown below deinitializes the
	// subsystem, which still wants a UI thread to enqueue its view removals into.
	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	FStandaloneInstance Instance;
	UVaCuusSubsystem* Subsystem = Instance.Subsystem;
	if (!TestNotNull(TEXT("UVaCuusSubsystem on the standalone game instance"), Subsystem))
	{
		return false;
	}

	UVaCuusView* FileView = Subsystem->CreateView(MakeUnique<FVaCuusTestNullDocumentHost>(), FIntPoint(320, 200));
	UVaCuusView* EmptyView = Subsystem->CreateView(MakeUnique<FVaCuusTestNullDocumentHost>(), FIntPoint(320, 200));
	if (!TestNotNull(TEXT("View with a document"), FileView) || !TestNotNull(TEXT("View without one"), EmptyView))
	{
		return false;
	}

	TestTrue(TEXT("A view starts with no document path"), FileView->GetDocumentPath().IsEmpty());
	TestFalse(TEXT("Reloading a view with no document does nothing"), FileView->ReloadDocument());

	FileView->LoadDocument(TEXT("m1_hud.rml"));
	TestEqual(TEXT("The document path is remembered"), FileView->GetDocumentPath(), FString(TEXT("m1_hud.rml")));

	const uint64 SerialBefore = FileView->GetLastRequestedLoadSerial();
	if (!TestTrue(TEXT("The first load took a serial"), SerialBefore > 0))
	{
		return false;
	}

	// THE ASSERTION THIS TEST EXISTS FOR: the dispatcher found this view by walking the
	// world contexts, and the load was re-issued.
	const int32 NumReloaded = FVaCuusLiveReload::ReloadAllLiveViews(TEXT("automation"));
	TestEqual(TEXT("Exactly the view with a file document was reloaded"), NumReloaded, 1);
	TestEqual(TEXT("The reload advanced the requested load serial by one"),
		FileView->GetLastRequestedLoadSerial(), SerialBefore + 1);
	TestEqual(TEXT("The reload re-used the same document path"),
		FileView->GetDocumentPath(), FString(TEXT("m1_hud.rml")));
	TestTrue(TEXT("The view without a document was left alone"), EmptyView->GetDocumentPath().IsEmpty());

	// An inline document has no file behind it, so the FAN-OUT must not reach it and must not
	// resurrect the path it fell back FROM. (Its owner still can, deliberately and knowing
	// which file it wants back -- that is VaCuus.LiveReload.Rearm.)
	FileView->LoadDocumentFromMemory(TEXT("<rml><body/></rml>"));
	TestTrue(TEXT("An in-memory load clears the document path"), FileView->GetDocumentPath().IsEmpty());
	TestEqual(TEXT("The fan-out reloads nothing after an in-memory load"),
		FVaCuusLiveReload::ReloadAllLiveViews(TEXT("automation")), 0);

	// Closing has the same effect, for the same reason.
	FileView->LoadDocument(TEXT("m1_hud.rml"));
	FileView->Close();
	TestTrue(TEXT("Close() clears the document path"), FileView->GetDocumentPath().IsEmpty());
	TestEqual(TEXT("Nothing is reloadable after Close()"),
		FVaCuusLiveReload::ReloadAllLiveViews(TEXT("automation")), 0);

	// Every view with a document, not just the first one found. Asked through the runtime
	// door rather than the subsystem's own fan-out, which is private now (bead
	// VaCuus-akj.6.34); one standalone instance exists, so the two answer identically.
	FileView->LoadDocument(TEXT("m1_hud.rml"));
	EmptyView->LoadDocument(TEXT("m1_hud.rml"));
	TestEqual(TEXT("The fan-out reloads every view that has a document"),
		FVaCuusLiveReload::ReloadAllLiveViews(TEXT("automation")), 2);

	// An invalidated view is inert, so a flush racing PIE teardown cannot reach a dead one.
	Subsystem->DestroyView(FileView);
	TestFalse(TEXT("A destroyed view does not reload"), FileView->ReloadDocument());
	TestEqual(TEXT("Only the surviving view reloads"),
		FVaCuusLiveReload::ReloadAllLiveViews(TEXT("automation")), 1);

	return true;
}

/**
 * A VIEW THAT FELL BACK TO AN INLINE DOCUMENT MUST BECOME RELOADABLE AGAIN once its file
 * appears -- through its owner, which is the only thing that knows which file it fell back
 * FROM (review finding I4).
 *
 * The bug this replaces: vacuus.M1HUD reports 'm1_hud.rml not found, using the inline
 * fallback'; you create the file and save it; the watcher fires, the flush says
 * `reloaded 0 view(s)`, and nothing reaches the screen until the toggle is cycled. Iterating
 * on a missing or broken document is the single most valuable thing live reload does.
 *
 * The handler here mimics vacuus.M1HUD's (VaCuusRender.cpp's OnDocumentsReloadRequested),
 * which cannot be driven headlessly: it needs GEngine->GameViewport, i.e. PIE. What is
 * asserted is the mechanism the real handler hangs on, including that a re-arm is COUNTED --
 * the flush's log line reports how many views a reload reached, and a silent re-arm would
 * make it say 0 while a load was in flight.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusLiveReloadRearmTest, "VaCuus.LiveReload.Rearm",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusLiveReloadRearmTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusLiveReloadTest;

	const FString SkipReason = WhySkip();
	if (!SkipReason.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("Skipped: %s"), *SkipReason));
		return true;
	}

	if (!TestRmlUiIsDown(*this))
	{
		return false;
	}

	FVaCuusModule& Module = FVaCuusModule::Get();
	if (!TestNotNull(TEXT("UI thread"), Module.GetOrStartUIThread()))
	{
		return false;
	}

	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	FStandaloneInstance Instance;
	UVaCuusSubsystem* Subsystem = Instance.Subsystem;
	if (!TestNotNull(TEXT("UVaCuusSubsystem on the standalone game instance"), Subsystem))
	{
		return false;
	}

	UVaCuusView* View = Subsystem->CreateView(MakeUnique<FVaCuusTestNullDocumentHost>(), FIntPoint(320, 200));
	if (!TestNotNull(TEXT("View"), View))
	{
		return false;
	}

	// The fallback: the file load was tried and lost, so the view is showing inline RML and
	// its document path is empty by design.
	View->LoadDocument(TEXT("m1_hud.rml"));
	View->LoadDocumentFromMemory(TEXT("<rml><body/></rml>"));
	TestTrue(TEXT("The fallback left no document path"), View->GetDocumentPath().IsEmpty());
	TestEqual(TEXT("So the fan-out on its own reaches nothing"),
		UVaCuusSubsystem::ClearAssetCachesAndReloadAllViews(TEXT("automation")), 0);

	// The owner's re-arm, exactly as vacuus.M1HUD does it: only when the view is showing the
	// fallback, and counted.
	int32 NumHandlerRuns = 0;
	FDelegateHandle Handle = Subsystem->OnDocumentsReloadRequested.AddLambda(
		[View, &NumHandlerRuns](int32& InOutNumReloaded)
		{
			++NumHandlerRuns;
			if (View->GetDocumentPath().IsEmpty())
			{
				View->LoadDocument(TEXT("m1_hud.rml"));
				++InOutNumReloaded;
			}
		});

	ON_SCOPE_EXIT
	{
		Subsystem->OnDocumentsReloadRequested.Remove(Handle);
	};

	const uint64 SerialBefore = View->GetLastRequestedLoadSerial();
	TestEqual(TEXT("An owner re-arm is reported as a reload"),
		UVaCuusSubsystem::ClearAssetCachesAndReloadAllViews(TEXT("automation")), 1);
	TestEqual(TEXT("...the handler ran once"), NumHandlerRuns, 1);
	TestTrue(TEXT("...a load was actually issued"), View->GetLastRequestedLoadSerial() > SerialBefore);
	TestEqual(TEXT("...and the view is describing the file again"),
		View->GetDocumentPath(), FString(TEXT("m1_hud.rml")));

	// And now the ordinary path takes over: the fan-out reloads it, the owner stands down, so
	// the same flush cannot load one document twice.
	const uint64 SerialAfterRearm = View->GetLastRequestedLoadSerial();
	TestEqual(TEXT("Once re-armed, the fan-out reloads it exactly once"),
		UVaCuusSubsystem::ClearAssetCachesAndReloadAllViews(TEXT("automation")), 1);
	TestEqual(TEXT("...the handler ran again but did nothing"), NumHandlerRuns, 2);
	TestEqual(TEXT("...so exactly one load was issued"),
		View->GetLastRequestedLoadSerial(), SerialAfterRearm + 1);

	return true;
}

namespace VaCuusLiveReloadTest
{
/**
 * Everything VaCuus.LiveReload.WatcherEvent's latent commands share, kept alive by a
 * TSharedPtr the commands copy.
 *
 * Teardown belongs to the LAST command, not to RunTest(): RunTest only queues the commands
 * and returns, so an ON_SCOPE_EXIT there would tear the world down before the first Update().
 */
struct FWatcherEventState
{
	FAutomationTestBase* Test = nullptr;
	IDirectoryWatcher* Watcher = nullptr;

	/** Our own watcher+debounce instance, separate from the editor module's live one. */
	FVaCuusLiveReload Reload;

	TUniquePtr<FStandaloneInstance> Instance;
	UVaCuusView* View = nullptr;

	/** Absolute path of the probe file, deleted by the last command. */
	FString ProbePath;

	uint64 SerialBefore = 0;
	int32 NumWatcherTicks = 0;
	bool bChangeSeen = false;
	bool bReloadSeen = false;

	/** Absolute FPlatformTime::Seconds() budgets, generous: a miss must not hang the suite. */
	double PumpDeadline = 0.0;
	double FlushDeadline = 0.0;
};

/**
 * Pumps IDirectoryWatcher until the probe write turns into a tracked change.
 *
 * Tick(-1.0f) reads the inotify fd and fires every FDirectoryChanged delegate INLINE on the
 * calling thread -- the Linux backend ignores DeltaSeconds entirely
 * (DirectoryWatcherLinux.cpp:111-123 -> FDirectoryWatchRequestLinux::ProcessNotifications).
 * Engine code does exactly this when it needs changes now: AssetRegistry.cpp:2039,
 * WorldPartitionEditorModule.cpp:717-719 (the call is at :719, the comment 'Force a
 * directory watcher tick for the asset registry to get notified of the changes' at :717),
 * EditorBuildUtils.cpp:1113.
 *
 * Latent rather than a loop inside RunTest because inotify DELIVERY is asynchronous: the
 * kernel has to queue the event before any tick can read it.
 */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FVaCuusPumpWatcherCommand, TSharedPtr<FWatcherEventState>, State);
bool FVaCuusPumpWatcherCommand::Update()
{
	++State->NumWatcherTicks;
	State->Watcher->Tick(-1.0f);

	// "Tracked" is read from EITHER the pending batch or an already-advanced load serial: the
	// debounce ticker runs between our Updates, so a slow frame can let it flush the batch
	// before we look at it -- and a flush is proof the change was tracked.
	if (State->Reload.GetNumPendingChanges() > 0 || State->View->GetLastRequestedLoadSerial() > State->SerialBefore)
	{
		State->bChangeSeen = true;
		return true;
	}

	return FPlatformTime::Seconds() > State->PumpDeadline;
}

/** Waits out the debounce's quiet window and for a view to actually reload. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FVaCuusAwaitReloadCommand, TSharedPtr<FWatcherEventState>, State);
bool FVaCuusAwaitReloadCommand::Update()
{
	if (State->View->GetLastRequestedLoadSerial() > State->SerialBefore)
	{
		State->bReloadSeen = true;
		return true;
	}

	return FPlatformTime::Seconds() > State->FlushDeadline;
}

/** Asserts what the two commands above observed, then tears everything down in order. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FVaCuusVerifyWatcherEventCommand, TSharedPtr<FWatcherEventState>, State);
bool FVaCuusVerifyWatcherEventCommand::Update()
{
	FAutomationTestBase& Test = *State->Test;

	Test.AddInfo(FString::Printf(TEXT("Pumped IDirectoryWatcher::Tick(-1.0f) %d time(s)"), State->NumWatcherTicks));

	Test.TestTrue(TEXT("A REAL inotify event reached the debounce (the whole feature rests on this link)"),
		State->bChangeSeen);
	Test.TestTrue(TEXT("...and the flush it triggered re-issued a live view's load"), State->bReloadSeen);

	IFileManager::Get().Delete(*State->ProbePath, /*bRequireExists=*/false);

	// Order matters: the watch goes first (its delegate points into this state), then the
	// world (its subsystem enqueues view removals), then the thread they went to.
	State->Reload.Shutdown();
	State->Instance.Reset();
	FVaCuusModule::Get().StopUIThread();
	return true;
}
}	 // namespace VaCuusLiveReloadTest

/**
 * THE ONE LINK THE WHOLE FEATURE RESTS ON: a file written under a watched root becomes a
 * real inotify event, becomes a tracked change, and re-issues a live view's load.
 *
 * This was previously asserted to be untestable ("no automation test can make a real
 * DirectoryWatcher event arrive"), on the grounds that only UEditorEngine::Tick pumps the
 * watcher. Both halves are false: IDirectoryWatcher::Tick has 14 call sites outside the
 * DirectoryWatcher module (EditorEngine.cpp:1948 is only the per-frame editor one), and
 * Tick(-1.0f) fires the delegates inline on the calling thread. What IS true is that this
 * needs a LATENT command: inotify delivery is asynchronous and the debounce is time-based,
 * so the test writes, polls, and then waits out the quiet window.
 *
 * It writes a REAL .rcss, because the filter must accept it -- the '.tmptest' extension
 * VaCuus.Core.ContentRoots uses to stay invisible to the watcher would prove the opposite of
 * what this asserts. Two consequences, both stated rather than hidden:
 *  - in an interactive editor this test causes one genuine reload of whatever is on screen.
 *    That is a file changing under a watched root, which is what live reload is for;
 *  - the probe lands in a GIT-TRACKED directory, so its name is in the plugin's .gitignore.
 *    The last latent command deletes it and RunTest() pre-deletes, but a run aborted in
 *    between would otherwise leave an untracked file for someone's `git add -A`.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusLiveReloadWatcherEventTest, "VaCuus.LiveReload.WatcherEvent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusLiveReloadWatcherEventTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusLiveReloadTest;

	const FString SkipReason = WhySkip();
	if (!SkipReason.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("Skipped: %s"), *SkipReason));
		return true;
	}

	FDirectoryWatcherModule* WatcherModule = FModuleManager::GetModulePtr<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
	IDirectoryWatcher* Watcher = WatcherModule ? WatcherModule->Get() : nullptr;
	if (Watcher == nullptr)
	{
		AddInfo(TEXT("Skipped: this platform has no directory watcher"));
		return true;
	}

	TSharedPtr<FWatcherEventState> State = MakeShared<FWatcherEventState>();
	State->Test = this;
	State->Watcher = Watcher;

	//~ WHICH ROOTS ARE WATCHED, asserted directly rather than inferred. This is the only
	//~ check of Start()'s existence skip that does not go through "no events arrived", and it
	//~ is also what makes the probe path below meaningful: writing into a root nobody watches
	//~ would prove nothing.
	State->Reload.Start();
	const TArray<FString>& WatchedRoots = State->Reload.GetWatchedRoots();
	const TArray<FString>& DocumentRoots = VaCuusContentPaths::GetDocumentRoots();

	int32 NumExistingRoots = 0;
	for (const FString& Root : DocumentRoots)
	{
		NumExistingRoots += IFileManager::Get().DirectoryExists(*Root) ? 1 : 0;
	}

	TestEqual(TEXT("Start() watches exactly the DevUI roots that exist"), WatchedRoots.Num(), NumExistingRoots);
	for (const FString& Root : WatchedRoots)
	{
		TestTrue(TEXT("A watched root exists on disk"), IFileManager::Get().DirectoryExists(*Root));
		TestTrue(TEXT("A watched root is one of the DevUI roots"), DocumentRoots.Contains(Root));
	}
	if (DocumentRoots.Num() > 0 && IFileManager::Get().DirectoryExists(*DocumentRoots[0]))
	{
		// Plugin-first precedence (D19), carried through into what the watcher watches.
		TestEqual(TEXT("The first watched root is the first DevUI root (the plugin's)"),
			WatchedRoots.Num() > 0 ? WatchedRoots[0] : FString(), DocumentRoots[0]);
	}

	if (WatchedRoots.Num() == 0)
	{
		AddInfo(TEXT("Skipped the event half: no DevUI root exists, so there is nothing to write into"));
		State->Reload.Shutdown();
		return true;
	}

	if (!TestRmlUiIsDown(*this))
	{
		return false;
	}

	FVaCuusModule& Module = FVaCuusModule::Get();
	if (!TestNotNull(TEXT("UI thread"), Module.GetOrStartUIThread()))
	{
		State->Reload.Shutdown();
		return false;
	}

	State->Instance = MakeUnique<FStandaloneInstance>();
	UVaCuusSubsystem* Subsystem = State->Instance->Subsystem;
	if (!TestNotNull(TEXT("UVaCuusSubsystem on the standalone game instance"), Subsystem))
	{
		State->Reload.Shutdown();
		State->Instance.Reset();
		Module.StopUIThread();
		return false;
	}

	State->View = Subsystem->CreateView(MakeUnique<FVaCuusTestNullDocumentHost>(), FIntPoint(320, 200));
	if (!TestNotNull(TEXT("View"), State->View))
	{
		State->Reload.Shutdown();
		State->Instance.Reset();
		Module.StopUIThread();
		return false;
	}

	State->View->LoadDocument(TEXT("m1_hud.rml"));
	State->SerialBefore = State->View->GetLastRequestedLoadSerial();

	// Written AFTER Start(), or the watch would not exist yet. Deleted first in case an
	// earlier crashed run left one behind -- a leftover would produce no IN_CREATE.
	State->ProbePath = WatchedRoots[0] / TEXT("vacuus_livereload_probe.rcss");
	IFileManager::Get().Delete(*State->ProbePath, /*bRequireExists=*/false);
	if (!TestTrue(TEXT("The probe .rcss was written under the watched root"),
			FFileHelper::SaveStringToFile(TEXT("/* VaCuus live-reload probe */\n"), *State->ProbePath)))
	{
		State->Reload.Shutdown();
		State->Instance.Reset();
		Module.StopUIThread();
		return false;
	}

	const double Now = FPlatformTime::Seconds();
	State->PumpDeadline = Now + 5.0;
	State->FlushDeadline = Now + 12.0;

	ADD_LATENT_AUTOMATION_COMMAND(FVaCuusPumpWatcherCommand(State));
	ADD_LATENT_AUTOMATION_COMMAND(FVaCuusAwaitReloadCommand(State));
	ADD_LATENT_AUTOMATION_COMMAND(FVaCuusVerifyWatcherEventCommand(State));
	return true;
}

/**
 * THE BUNDLE SHADOW WARNING -- the one thing live reload can do about a trap it cannot fix.
 *
 * With a bundle mounted, the VFS serves the PACKED copy of anything the bundle contains, so
 * an edit to the loose file is re-read and then thrown away: the view shows the bundle's
 * bytes and nothing anywhere says why. Live reload does not (and should not) reach into the
 * mount table to fix that -- the watcher watches loose roots by design -- so the flush names
 * the shadow per changed file, and that Warning IS the feature.
 *
 * It had no test. It is step 4-5 of manual-matrix row 13, which is the row nobody had ever
 * executed on any platform (VaCuus-akj.10.10), and the reason given was that the row needs a
 * human in an editor. That is true of WATCHING A VIEW REPAINT and false of this: the shadow
 * check is a string test between a watched root and the mount table, so it needs neither a
 * watcher event nor a view. Synchronous, no file I/O, no latent command.
 *
 * THE PROBE FILE NEVER EXISTS ON DISK, deliberately. NoteChangeAt filters on the action and
 * the name (ShouldTrackChange is pure) and the flush's shadow loop compares strings, so a
 * real file would add I/O and a cleanup path to a test that asserts neither.
 *
 * BOTH DIRECTIONS, because "a Warning fired" is only half a claim: the same change with
 * nothing mounted must produce NO shadow line. Without that half this test would pass against
 * an implementation that warned unconditionally.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusLiveReloadBundleShadowTest, "VaCuus.LiveReload.BundleShadow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusLiveReloadBundleShadowTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusLiveReloadTest;

	FVaCuusLiveReload Reload;
	Reload.Start();
	ON_SCOPE_EXIT
	{
		Reload.Shutdown();
		FVaCuusBundleMountTable::UnmountAll();
	};

	const TArray<FString>& WatchedRoots = Reload.GetWatchedRoots();
	if (WatchedRoots.Num() == 0)
	{
		// Start() skips roots that do not exist, and the shadow loop iterates the ones it
		// registered -- with none there is nothing to shadow. Named rather than passed
		// silently: on a tree with no DevUI directory this test asserts nothing.
		AddInfo(TEXT("Skipped: no DevUI root exists, so nothing is watched and no path can be shadowed"));
		return true;
	}

	// A name no real document uses, under a root that IS watched. The extension has to be one
	// the filter accepts -- a probe the filter drops would test the filter, not the shadow.
	const FString RelativePath = TEXT("vacuus_bundle_shadow_probe.rcss");
	const FString ChangedPath = WatchedRoots[0] / RelativePath;
	const FFileChangeData Change(ChangedPath, FFileChangeData::FCA_Modified);

	// ---- 1. NOTHING MOUNTED: the same change must NOT produce a shadow line. ----
	{
		FScopedLiveReloadLogCapture Capture;
		TestTrue(TEXT("the control change was tracked"), Reload.NoteChangeAt(Change, 100.0));
		Reload.FlushAt(101.0);
		TestFalse(TEXT("with no bundle mounted, nothing claims the path is shadowed"),
			Capture.Contains(TEXT("is SHADOWED by mounted bundle")));
	}

	// ---- 2. MOUNTED AND CONTAINING THE PATH: the flush must name it. ----
	const TCHAR* BundleName = TEXT("<LiveReloadShadowProbe>");

	VaCuusBundleFormat::FCookedIndex Index;
	TArray64<uint8> Payload;
	{
		const ANSICHAR* Content = "/* the packed copy, which is the whole point */";
		const int64 Size = FCStringAnsi::Strlen(Content);
		Payload.Append(reinterpret_cast<const uint8*>(Content), Size);
		Index.Entries.Add(FVaCuusBundleEntry{VaCuusBundleFormat::NormalizePath(*RelativePath), 0, Size});
		Index.PayloadSize = Payload.Num();
	}
	if (!TestTrue(TEXT("the probe bundle mounted"),
			FVaCuusBundleMountTable::MountTransient(BundleName, TEXT("built by VaCuusLiveReloadTest"),
				MoveTemp(Index), MoveTemp(Payload))))
	{
		return false;
	}

	FString ServingBundle;
	TestTrue(TEXT("the mount table serves the probe path (the precondition the flush reads)"),
		FVaCuusBundleMountTable::ContainsPath(RelativePath, &ServingBundle));
	TestEqual(TEXT("...from the probe bundle"), ServingBundle, FString(BundleName));

	// Warning-level lines DO reach the automation machinery, so an undeclared one would fail
	// the run. Declaring it here is not suppression: the count makes the expectation an
	// assertion -- the test fails if the line does not appear exactly once.
	AddExpectedMessagePlain(TEXT("is SHADOWED by mounted bundle"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);

	{
		FScopedLiveReloadLogCapture Capture;
		TestTrue(TEXT("the shadowed change was tracked"), Reload.NoteChangeAt(Change, 200.0));
		Reload.FlushAt(201.0);

		// ONE LINE CARRYING ALL FOUR, not four independent Contains checks. Written the loose
		// way first, and the restore-the-bug run is what caught it: with the warning deleted,
		// "the capture contains the changed path" was STILL true, because the flush's ordinary
		// "flushed N changed path(s): <path>" diagnostic names it too. A reader needs the
		// sentence, the file, the bundle eating it and the way out -- together, in the line
		// they are reading -- or the warning is not actionable.
		TestTrue(TEXT("one Warning names the shadow, the file, the bundle and the way out"),
			Capture.ContainsAllOnOneLine({TEXT("is SHADOWED by mounted bundle"), *ChangedPath, BundleName,
				TEXT("vacuus.Bundle.Enable 0")}));
	}

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
