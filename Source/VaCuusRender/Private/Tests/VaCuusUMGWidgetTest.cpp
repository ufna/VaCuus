// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusEngine.h"
#include "VaCuusSubsystem.h"
#include "VaCuusUIThread.h"
#include "VaCuusUMGWidget.h"
#include "VaCuusView.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SWidget.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusUMGWidgetTest
{
/** One UI frame at a time; the wake event coalesces, so N triggers are not N frames. */
static bool RunFrames(FVaCuusUIThread& UIThread, int32 NumFrames)
{
	for (int32 Index = 0; Index < NumFrames; ++Index)
	{
		const uint64 Before = UIThread.GetFrameCount();
		UIThread.Trigger();
		if (!UIThread.WaitForFrameCount(Before + 1, 5.0))
		{
			return false;
		}
	}

	return true;
}
}	 // namespace VaCuusUMGWidgetTest

/**
 * The UMG wrapper's lifecycle: TakeWidget() builds a Slate widget AND a view,
 * ReleaseSlateResources() retires both, and a rebuild after a release produces exactly
 * one new view rather than a second one alongside the old.
 *
 * WHY THE GAME INSTANCE IS BUILT THE HARD WAY: UVaCuusWidget::RebuildWidget() asks the
 * owning game instance for its UVaCuusSubsystem, and a bare NewObject<UGameInstance>()
 * (which is all VaCuus.Input.SlateRouting needs, because it wires its view by hand) has
 * an EMPTY subsystem collection -- GetSubsystem() returns null and the widget would
 * correctly refuse to build. UGameInstance::InitializeStandalone() is the public engine
 * call that creates a world context, a world and then runs Init(), which is what
 * initializes the collection. It also gives the widget a real UWorld, so the test
 * exercises the production ResolveGameInstance() path (GetWorld()->GetGameInstance())
 * rather than the outer-chain fallback.
 *
 * NOT COVERED HERE, deliberately and stated rather than implied: the mouse-capture
 * release inside ReleaseSlateResources(). Slate only has a captor when a real
 * FWidgetPath from a real window captured it, and a headless run has neither a window
 * nor a hit-test grid to build one -- FSlateUser::SetCursorCaptor needs the path, not
 * just the widget. That half is verified by vacuus.UMGDemo's teardown in a live session
 * (no ensure at SlateApplication.cpp:5558 on the next mouse-up).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusUMGWidgetTest, "VaCuus.UMG.Widget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusUMGWidgetTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusUMGWidgetTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no worker thread to drive"));
		return true;
	}

	if (!FSlateApplication::IsInitialized())
	{
		// UGameInstance::Init() calls FSlateApplication::Get() unconditionally to register
		// its console command listener, so there is no way to build an initialized game
		// instance without Slate.
		AddInfo(TEXT("Skipped: no FSlateApplication, so a game instance cannot be initialized"));
		return true;
	}

	if (!TestNotNull(TEXT("GEngine"), GEngine))
	{
		return false;
	}

	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	FVaCuusModule& Module = FVaCuusModule::Get();
	FVaCuusUIThread* UIThread = Module.GetOrStartUIThread();
	if (!TestNotNull(TEXT("UI thread"), UIThread))
	{
		return false;
	}

	// Declared first, so it runs LAST: the world teardown below deinitializes the
	// subsystem, which still wants a UI thread to enqueue its view removals into.
	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	// Rooted by hand: nothing in the engine references an ad-hoc game instance, and
	// being outered to GEngine does not keep it alive.
	TStrongObjectPtr<UGameInstance> GameInstance(NewObject<UGameInstance>(GEngine));
	GameInstance->InitializeStandalone();

	ON_SCOPE_EXIT
	{
		// Shutdown() clears the world context pointer, so the world has to be taken first.
		UWorld* World = GameInstance->GetWorld();
		GameInstance->Shutdown();
		if (World)
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}
	};

	if (!TestNotNull(TEXT("The standalone game instance has a world"), GameInstance->GetWorld()))
	{
		return false;
	}

	UVaCuusSubsystem* Subsystem = GameInstance->GetSubsystem<UVaCuusSubsystem>();
	if (!TestNotNull(TEXT("UVaCuusSubsystem on the initialized game instance"), Subsystem))
	{
		return false;
	}

	// Outered to the game instance, exactly as vacuus.UMGDemo does it. Rooted for the
	// same reason as the game instance.
	TStrongObjectPtr<UVaCuusWidget> Widget(NewObject<UVaCuusWidget>(GameInstance.Get()));

	TestNull(TEXT("A freshly constructed widget has no view yet"), Widget->GetView());

	// 1. TakeWidget(): the one call UMG makes on a child widget. It runs RebuildWidget()
	// and then SynchronizeProperties() on the newly-created path.
	TSharedPtr<SWidget> SlateWidget = Widget->TakeWidget();
	if (!TestTrue(TEXT("TakeWidget() returned a widget"), SlateWidget.IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("...which is not the null widget"), SlateWidget.Get() == &SNullWidget::NullWidget.Get());
	TestEqual(TEXT("...and is the VaCuus Slate widget"), SlateWidget->GetType(), FName(TEXT("SVaCuusWidget")));
	TestTrue(TEXT("...and is what the UWidget cached"), SlateWidget == Widget->GetCachedWidget());

	// 2. Building the Slate widget created a view.
	UVaCuusView* View = Widget->GetView();
	if (!TestNotNull(TEXT("Building the widget created a view"), View))
	{
		return false;
	}
	TestTrue(TEXT("The view handle is valid"), View->IsViewValid());

	// Rooted separately so the assertions after the release still have something to ask.
	TStrongObjectPtr<UVaCuusView> ViewKeepAlive(View);
	const uint32 FirstViewId = View->GetViewId();

	// 3. The UI thread actually registered it.
	if (!TestTrue(TEXT("UI frames ran"), RunFrames(*UIThread, 2)))
	{
		return false;
	}
	TestEqual(TEXT("The UI thread has exactly one view"), UIThread->GetNumViews(), 1);
	TestTrue(TEXT("RmlUi booted with the UI thread"), FVaCuusEngine::Get().IsInitialized());

	// 4. LoadDocument() forwards to the view and updates the exposed property.
	const FString DocumentDiskPath = FPaths::ProjectContentDir() / TEXT("DevUI") / TEXT("m1_hud.rml");
	if (FPaths::FileExists(DocumentDiskPath))
	{
		const uint64 SerialBefore = View->GetLastRequestedLoadSerial();
		Widget->LoadDocument(TEXT("m1_hud.rml"));

		TestEqual(TEXT("LoadDocument() updates the exposed Document property"),
			Widget->DocumentPath, FString(TEXT("m1_hud.rml")));
		TestEqual(TEXT("LoadDocument() reached the view"),
			int64(View->GetLastRequestedLoadSerial()), int64(SerialBefore + 1));

		if (TestTrue(TEXT("UI frames ran for the load"), RunFrames(*UIThread, 2)))
		{
			TestEqual(TEXT("The UI thread completed the load the widget asked for"),
				int64(View->GetLastCompletedLoadSerial()), int64(SerialBefore + 1));
		}

		// Close() forwards too; a closed view stays alive and can load again.
		Widget->Close();
		TestTrue(TEXT("UI frames ran after the close"), RunFrames(*UIThread, 1));
		TestTrue(TEXT("The view survives closing its document"), View->IsViewValid());
	}
	else
	{
		AddInfo(FString::Printf(
			TEXT("Skipped the LoadDocument()/Close() assertions: '%s' is not in this project"), *DocumentDiskPath));
	}

	// 5. THE TEARDOWN OBLIGATION. ReleaseSlateResources() must retire the view, not just
	// drop the shared pointer -- a view that outlived its widget would keep an RmlUi
	// context and keep publishing frames to an element nothing draws.
	TWeakPtr<SWidget> WeakSlateWidget = SlateWidget;

	Widget->ReleaseSlateResources(/*bReleaseChildren=*/true);

	TestNull(TEXT("Releasing the widget forgets the view"), Widget->GetView());
	TestFalse(TEXT("...and invalidates the view handle"), ViewKeepAlive->IsViewValid());

	// Nothing leaks: this test held the only remaining reference to the Slate widget, so
	// dropping it must destroy it. A surviving widget would mean the wrapper (or the
	// view) is still holding one.
	SlateWidget.Reset();
	TestFalse(TEXT("The Slate widget is destroyed once the last reference goes"), WeakSlateWidget.IsValid());

	if (!TestTrue(TEXT("UI frames ran after the release"), RunFrames(*UIThread, 2)))
	{
		return false;
	}
	TestEqual(TEXT("The UI thread has no views left"), UIThread->GetNumViews(), 0);

	// 6. THE DUPLICATE-VIEW GUARD. RebuildWidget() fires again here, because the cached
	// Slate widget is gone -- which is exactly what a re-add to the viewport does. One
	// new view must appear, not one new view plus the old one.
	TSharedPtr<SWidget> RebuiltSlateWidget = Widget->TakeWidget();
	TestTrue(TEXT("A rebuild produces a widget again"), RebuiltSlateWidget.IsValid());

	UVaCuusView* RebuiltView = Widget->GetView();
	if (TestNotNull(TEXT("The rebuild created a new view"), RebuiltView))
	{
		TestTrue(TEXT("The new view handle is valid"), RebuiltView->IsViewValid());
		TestNotEqual(TEXT("...with a different view id"), RebuiltView->GetViewId(), FirstViewId);
	}

	if (TestTrue(TEXT("UI frames ran after the rebuild"), RunFrames(*UIThread, 2)))
	{
		TestEqual(TEXT("The rebuild left exactly one view, not two"), UIThread->GetNumViews(), 1);
	}

	// Leave nothing behind for the next test in the suite.
	Widget->ReleaseSlateResources(/*bReleaseChildren=*/true);
	RebuiltSlateWidget.Reset();
	if (TestTrue(TEXT("UI frames ran after the final release"), RunFrames(*UIThread, 2)))
	{
		TestEqual(TEXT("No views survive the widget"), UIThread->GetNumViews(), 0);
	}

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
