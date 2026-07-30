// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusContentPaths.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusLiveReload.h"
#include "VaCuusSubsystem.h"
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
 * A document host that does nothing at all -- no context, no RmlUi.
 *
 * ENOUGH, AND DELIBERATELY SO: what the dispatch test asserts is a GAME-THREAD fact
 * ("the reload dispatcher re-issued a load for this view"), observed through
 * UVaCuusView::GetLastRequestedLoadSerial(), which the view stamps before the command
 * ever reaches the UI thread. Giving the host a real context would add an RmlUi boot and
 * a document parse to a test that would assert nothing more.
 */
class FStubHost final : public IVaCuusDocumentHost
{
public:
	virtual bool Initialize(uint32 InViewId, const TSharedRef<FVaCuusViewStatus>& InStatus) override { return true; }
	virtual void Shutdown() override {}
	virtual void SetViewSize(FIntPoint ViewSize) override {}
	virtual void LoadDocumentFromFile(const FString& VfsPath, uint64 LoadSerial) override {}
	virtual void LoadDocumentFromMemory(const FString& RmlSource, uint64 LoadSerial) override {}
	virtual void CloseDocument() override {}
	virtual void SetVisible(bool bVisible) override {}
	virtual bool HasView() const override { return false; }
	virtual Rml::Context* GetContext() const override { return nullptr; }
	virtual void RecordAndPublishFrame() override {}
};

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

	//~ Normalisation. FFileChangeData's constructor runs FPaths::MakeStandardFilename, so
	//~ what the delegate receives is usually relative -- comparing or reloading it as-is
	//~ is the bug this exists to prevent.
	{
		const FString Relative = TEXT("../../../VcHost/Content/DevUI/m1_hud.rcss");
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
		// flush CONSUMES the batch (a batch left behind would reload forever).
		Reload.FlushNow();
		TestEqual(TEXT("Flush consumes the batch"), Reload.GetNumPendingChanges(), 0);
		TestEqual(TEXT("Flushing an empty batch is a no-op"), Reload.FlushNow(), 0);

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

	UVaCuusView* FileView = Subsystem->CreateView(MakeUnique<FStubHost>(), FIntPoint(320, 200));
	UVaCuusView* EmptyView = Subsystem->CreateView(MakeUnique<FStubHost>(), FIntPoint(320, 200));
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

	// The subsystem's own fan-out, which is what the dispatcher calls per game instance.
	FileView->LoadDocument(TEXT("m1_hud.rml"));
	EmptyView->LoadDocument(TEXT("m1_hud.rml"));
	TestEqual(TEXT("The subsystem reloads every view that has a document"), Subsystem->ReloadAllDocuments(), 2);

	// An invalidated view is inert, so a flush racing PIE teardown cannot reach a dead one.
	Subsystem->DestroyView(FileView);
	TestFalse(TEXT("A destroyed view does not reload"), FileView->ReloadDocument());
	TestEqual(TEXT("Only the surviving view reloads"), Subsystem->ReloadAllDocuments(), 1);

	return true;
}

/**
 * THE CACHE CLEAR, which is what actually makes an RCSS edit visible -- and which used to
 * ride as a flag on a per-view load command, behind the drain's FindHost() gate.
 *
 * The two properties that bug violated, and that this test exists to keep:
 *
 *  1. A FLUSH THAT REACHES ZERO LIVE VIEWS STILL CLEARS. RmlUi's parsed-stylesheet and
 *     template caches are process-global statics keyed on file name, and they outlive a PIE
 *     session (UVaCuusSubsystem::Deinitialize leaves the UI thread running). So "stop PIE,
 *     edit the .rcss, press Play" must not re-load the RML from disk and then take the
 *     PREVIOUS session's stylesheet. With the clear on the load command, that flush enqueued
 *     nothing and cleared nothing -- silently, and only for RCSS, which is the edit people
 *     make most.
 *  2. ONE FAN-OUT IS ONE CLEAR, not one per view. Three reloaded views used to clear three
 *     times, and clears 2 and 3 discarded the stylesheet view 1 had just re-parsed.
 *
 * Observed through FVaCuusUIThread::GetNumAssetCacheClears() because RmlUi offers nothing to
 * ask about its caches. Trigger()+WaitForFrameCount() rather than a sleep: nothing else wakes
 * the UI thread here (no world is ticking the subsystem).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusLiveReloadAssetCachesTest, "VaCuus.LiveReload.AssetCaches",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusLiveReloadAssetCachesTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusLiveReloadTest;

	const FString SkipReason = WhySkip();
	if (!SkipReason.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("Skipped: %s"), *SkipReason));
		return true;
	}

	FVaCuusModule& Module = FVaCuusModule::Get();
	FVaCuusUIThread* UIThread = Module.GetOrStartUIThread();
	if (!TestNotNull(TEXT("UI thread"), UIThread))
	{
		return false;
	}

	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	// Drives one UI frame and returns how many clears it applied since Before.
	const auto ClearsAfterOneFrame = [this, UIThread](uint64 Before) -> uint64
	{
		const uint64 FrameBefore = UIThread->GetFrameCount();
		UIThread->Trigger();
		if (!TestTrue(TEXT("A UI frame ran"), UIThread->WaitForFrameCount(FrameBefore + 1, 10.0)))
		{
			return 0;
		}
		return UIThread->GetNumAssetCacheClears() - Before;
	};

	//~ (1) NO VIEWS AT ALL. No game instance exists yet, so the fan-out finds nothing --
	//~ which is exactly the "edited the CSS between PIE sessions" case.
	{
		const uint64 ClearsBefore = UIThread->GetNumAssetCacheClears();
		TestEqual(TEXT("(1) With no game instance, no view is reloaded"),
			FVaCuusLiveReload::ReloadAllLiveViews(TEXT("automation")), 0);
		TestEqual(TEXT("(1) ...and the RmlUi asset caches are dropped anyway"),
			ClearsAfterOneFrame(ClearsBefore), static_cast<uint64>(1));
	}

	//~ (2) TWO VIEWS, ONE CLEAR.
	{
		FStandaloneInstance Instance;
		UVaCuusSubsystem* Subsystem = Instance.Subsystem;
		if (!TestNotNull(TEXT("UVaCuusSubsystem on the standalone game instance"), Subsystem))
		{
			return false;
		}

		UVaCuusView* FirstView = Subsystem->CreateView(MakeUnique<FStubHost>(), FIntPoint(320, 200));
		UVaCuusView* SecondView = Subsystem->CreateView(MakeUnique<FStubHost>(), FIntPoint(320, 200));
		if (!TestNotNull(TEXT("First view"), FirstView) || !TestNotNull(TEXT("Second view"), SecondView))
		{
			return false;
		}

		FirstView->LoadDocument(TEXT("m1_hud.rml"));
		SecondView->LoadDocument(TEXT("m1_hud.rml"));

		const uint64 ClearsBefore = UIThread->GetNumAssetCacheClears();
		TestEqual(TEXT("(2) Both views reloaded"),
			FVaCuusLiveReload::ReloadAllLiveViews(TEXT("automation")), 2);
		TestEqual(TEXT("(2) ...at the cost of exactly one cache clear, not one per view"),
			ClearsAfterOneFrame(ClearsBefore), static_cast<uint64>(1));
	}

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

	UVaCuusView* View = Subsystem->CreateView(MakeUnique<FStubHost>(), FIntPoint(320, 200));
	if (!TestNotNull(TEXT("View"), View))
	{
		return false;
	}

	// The fallback: the file load was tried and lost, so the view is showing inline RML and
	// its document path is empty by design.
	View->LoadDocument(TEXT("m1_hud.rml"));
	View->LoadDocumentFromMemory(TEXT("<rml><body/></rml>"));
	TestTrue(TEXT("The fallback left no document path"), View->GetDocumentPath().IsEmpty());
	TestEqual(TEXT("So the fan-out on its own reaches nothing"), Subsystem->ReloadAllDocuments(), 0);

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
	TestEqual(TEXT("An owner re-arm is reported as a reload"), Subsystem->ReloadAllDocuments(), 1);
	TestEqual(TEXT("...the handler ran once"), NumHandlerRuns, 1);
	TestTrue(TEXT("...a load was actually issued"), View->GetLastRequestedLoadSerial() > SerialBefore);
	TestEqual(TEXT("...and the view is describing the file again"),
		View->GetDocumentPath(), FString(TEXT("m1_hud.rml")));

	// And now the ordinary path takes over: the fan-out reloads it, the owner stands down, so
	// the same flush cannot load one document twice.
	const uint64 SerialAfterRearm = View->GetLastRequestedLoadSerial();
	TestEqual(TEXT("Once re-armed, the fan-out reloads it exactly once"), Subsystem->ReloadAllDocuments(), 1);
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
 * (DirectoryWatcherLinux.cpp:111-124 -> FDirectoryWatchRequestLinux::ProcessNotifications).
 * Engine code does exactly this when it needs changes now: AssetRegistry.cpp:2039,
 * WorldPartitionEditorModule.cpp:719 ('Force a directory watcher tick'),
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
 * watcher. Both halves are false: there are ~17 callers of IDirectoryWatcher::Tick, and
 * Tick(-1.0f) fires the delegates inline on the calling thread -- the project's own research
 * note documents it as 'Force a synchronous watcher flush'. What IS true is that this needs a
 * LATENT command: inotify delivery is asynchronous and the debounce is time-based, so the
 * test writes, polls, and then waits out the quiet window.
 *
 * It writes a REAL .rcss, because the filter must accept it -- the '.tmptest' extension
 * VaCuus.Core.FileInterface uses to stay invisible to the watcher would prove the opposite of
 * what this asserts. Consequence in an interactive editor: this test causes one genuine
 * reload of whatever is on screen. That is a file changing under a watched root, which is
 * what live reload is for.
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

	State->View = Subsystem->CreateView(MakeUnique<FStubHost>(), FIntPoint(320, 200));
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

#endif	// WITH_DEV_AUTOMATION_TESTS
