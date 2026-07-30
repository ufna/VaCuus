// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
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
 * The filter, the path normalisation and the debounce coalescing -- the whole half of
 * live reload that can be tested at all headlessly.
 *
 * WHAT IS NOT COVERED HERE, AND WHY IT CANNOT BE: no automation test can make a real
 * DirectoryWatcher event arrive. IDirectoryWatcher::Tick is what reads the inotify fd
 * and fires the delegates, and the only engine caller of it is UEditorEngine::Tick
 * (EditorEngine.cpp:1948) -- which does not run inside a test, and cannot be provoked
 * into running one frame of itself. So this test drives NoteChange() directly, exactly
 * as the delegate does, and the "did an inotify event actually arrive" link is verified
 * live (see the Task 10 report) rather than here.
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
	//~ file you actually saved, and reloading on one costs a re-parse for nothing.
	TestFalse(TEXT("Trailing-tilde backup is skipped"),
		FVaCuusLiveReload::ShouldTrackChange(EAction::FCA_Modified, TEXT("DevUI/m1_hud.rcss~")));
	TestFalse(TEXT(".tmp is skipped"),
		FVaCuusLiveReload::ShouldTrackChange(EAction::FCA_Modified, TEXT("DevUI/m1_hud.rcss.tmp")));
	TestFalse(TEXT("vim swap file is skipped"),
		FVaCuusLiveReload::ShouldTrackChange(EAction::FCA_Added, TEXT("DevUI/.m1_hud.rcss.swp")));
	TestFalse(TEXT("A dotfile with a good extension is still skipped"),
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
	//~ FDirectoryWatchRequestLinux::Init returns true unconditionally and inotify_add_watch
	//~ failures only warn. That is why FVaCuusLiveReload::Start() calls DirectoryExists()
	//~ itself and never treats this bool as a health check. If a future engine version fixes
	//~ this, this test is where it shows up.
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

	// An inline document has no file behind it, so it must not become reloadable -- and
	// must not resurrect the path it fell back FROM (vacuus.M1HUD's fallback is that case).
	FileView->LoadDocumentFromMemory(TEXT("<rml><body/></rml>"));
	TestTrue(TEXT("An in-memory load clears the document path"), FileView->GetDocumentPath().IsEmpty());
	TestEqual(TEXT("Nothing is reloadable after an in-memory load"),
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

#endif	// WITH_DEV_AUTOMATION_TESTS
