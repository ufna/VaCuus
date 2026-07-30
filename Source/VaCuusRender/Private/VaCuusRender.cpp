// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "SVaCuusWidget.h"
#include "VaCuusDefines.h"
#include "VaCuusRmlDocumentHost.h"
#include "VaCuusSlateElement.h"
#include "VaCuusSubsystem.h"
#include "VaCuusView.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Input/Events.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "RenderingThread.h"
#include "ShaderCore.h"
#include "UnrealClient.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

namespace VaCuusM1HUD
{
/**
 * VFS path of the M1 HUD document; FVaCuusFileInterface resolves relative
 * paths against <Project>/Content/DevUI.
 */
static const TCHAR* GHudDocumentVfsPath = TEXT("m1_hud.rml");

/**
 * Inline fallback document, used when Content/DevUI/m1_hud.rml is missing OR
 * when it exists but fails to load (a broken RML/RCSS edit is exactly when a
 * working fallback matters most).
 * The pure red and pure blue divs are the channel-order probe: if the left
 * div renders blue, an RGBA/BGRA swap crept into the vertex or texture path.
 */
static const TCHAR* GTestDocumentRml = TEXT(R"(<rml>
<head>
<title>VaCuus M1</title>
<style>
body
{
	display: block;
	font-family: LatoLatin;
	font-size: 24px;
	width: 100%;
	height: 100%;
}
div { display: block; position: absolute; }
#red   { left: 60px;  top: 60px;  width: 160px; height: 120px; background-color: #FF0000; }
#blue  { left: 260px; top: 60px;  width: 160px; height: 120px; background-color: #0000FF; }
#panel { left: 60px;  top: 220px; width: 480px; height: 100px; background-color: #000000A0; }
#label { left: 80px;  top: 252px; color: #FFFFFF; }
</style>
</head>
<body>
	<div id="red"/>
	<div id="blue"/>
	<div id="panel"/>
	<div id="label">VaCuus M1</div>
</body>
</rml>)");

/**
 * Puts the local player controller into an input mode that lets Slate route pointer
 * events to viewport overlays at all, and takes it back out again.
 *
 * NOT A NICETY -- WITHOUT IT NO INPUT REACHES THE UI. Under the default
 * FInputModeGameOnly the game viewport holds mouse capture, so
 * FSlateApplication routes every pointer event straight to the captor (the SViewport)
 * and never runs a hit test; SVaCuusWidget's handlers are simply never called, no
 * matter what its visibility says. This was observed, not assumed: with GameOnly, a
 * synthesized move over the demo button reported "handled by a widget" (the captor)
 * and RmlUi's hover never changed.
 *
 * GameAndUI rather than UIOnly, because Task 6's contract is that clicks the UI does
 * not claim still reach the game -- which is exactly what GameAndUI means.
 *
 * This belongs to the debug toggle, not to the plugin: a real game decides its own
 * input mode, and a UMG-hosted VaCuus widget (Task 8) inherits whatever the game
 * already set.
 */
static void SetUIInputMode(UWorld* World, bool bEnable)
{
	APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr;
	if (!PlayerController)
	{
		UE_LOG(LogVaCuus, Warning,
			TEXT("M1 HUD: no player controller, so the input mode is unchanged; pointer input will not reach the UI"));
		return;
	}

	if (bEnable)
	{
		FInputModeGameAndUI Mode;
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		Mode.SetHideCursorDuringCapture(false);
		PlayerController->SetInputMode(Mode);
		PlayerController->SetShowMouseCursor(true);
	}
	else
	{
		PlayerController->SetInputMode(FInputModeGameOnly());
		PlayerController->SetShowMouseCursor(false);
	}

	UE_LOG(LogVaCuus, Log, TEXT("M1 HUD: input mode is now %s"),
		bEnable ? TEXT("GameAndUI (pointer events reach viewport overlays)") : TEXT("GameOnly"));
}

/**
 * Everything the HUD toggle owns while it is ON.
 *
 * The view is owned by UVaCuusSubsystem and the UI thread by FVaCuusModule; this
 * state is only the toggle's own bookkeeping.
 */
struct FState
{
	TWeakObjectPtr<UVaCuusSubsystem> Subsystem;
	TWeakObjectPtr<UVaCuusView> View;
	TSharedPtr<FVaCuusSlateElement> Element;
	TSharedPtr<SVaCuusWidget> Widget;
	TWeakObjectPtr<UGameViewportClient> Viewport;
	FDelegateHandle WorldTearDownHandle;

	/** World whose player controller the toggle put into GameAndUI; restored on teardown. */
	TWeakObjectPtr<UWorld> InputWorld;

	/** True while the pending load is the VFS document, i.e. while a fallback is possible. */
	bool bLoadingFromFile = false;

	/** One shot only: a failing inline fallback must not loop. */
	bool bTriedInlineFallback = false;
};

static TUniquePtr<FState> GState;

static void TearDown()
{
	if (!GState)
	{
		return;
	}
	TUniquePtr<FState> State = MoveTemp(GState);

	FWorldDelegates::OnWorldBeginTearDown.Remove(State->WorldTearDownHandle);

	// Hand the mouse back to the game. Before the widget goes away, so nothing can
	// arrive at a half-detached widget on the way out.
	SetUIInputMode(State->InputWorld.Get(), /*bEnable=*/false);

	// Spec §4 teardown order:
	//
	// 1. Stop accepting commands. Detaching first means no resize command can land
	// behind the view removal below; pulling the widget out of the viewport then
	// stops the paints. On PIE/world shutdown the viewport (or its widget tree) may
	// already be gone — weak ptr guards that.
	if (State->Widget.IsValid())
	{
		State->Widget->DetachView();

		// Hand mouse capture back before the widget can be destroyed. Slate stores its
		// captor as a WEAK WIDGET PATH whose IsValid() only tests Num() > 0, so it does
		// not notice that the captured leaf has gone: toggling the HUD off mid-drag
		// leaves a captor pointing at nothing and the next mouse-up trips
		// `ensureMsgf(MouseCaptorPath.Widgets.Num() > 0, ...)` at
		// SlateApplication.cpp:5558. ReleaseAllPointerCapture() is the supported way out
		// and is a no-op when nothing is captured.
		//
		// Task 8's UMG wrapper needs the same care in ReleaseSlateResources(): any path
		// that can drop an SVaCuusWidget while it holds capture has to release first.
		if (FSlateApplication::IsInitialized() && State->Widget->HasMouseCapture())
		{
			UE_LOG(LogVaCuus, Log, TEXT("M1 HUD: releasing mouse capture before the widget goes away"));
			FSlateApplication::Get().ReleaseAllPointerCapture();
		}

		if (UGameViewportClient* Viewport = State->Viewport.Get())
		{
			Viewport->RemoveViewportWidgetContent(State->Widget.ToSharedRef());
		}
		State->Widget.Reset();
	}

	// 2. Retire the view. The UI thread closes the document, drops the context and
	// releases the view's render resources from its own thread (so that release is
	// ordered after its last publish) — all without stopping the shared thread,
	// which other views and other PIE clients may still be using. If the subsystem
	// is already gone (world/engine teardown got here first) it has done that for us.
	if (UVaCuusSubsystem* Subsystem = State->Subsystem.Get())
	{
		Subsystem->DestroyView(State->View.Get());
	}

	// 3. Drop our own element reference. Note what this does NOT establish: step 2 is
	// asynchronous, so the host (and its own reference) lives on inside the UI
	// thread's view map until it drains the RemoveView, and the release command it
	// enqueues then may still be in flight. None of that needs ordering — the element
	// is a thread-safe shared pointer, whichever of the game, UI or render thread
	// happens to drop the last reference destroys it, and the render-side release is
	// ordered after the view's last publish because the UI thread enqueues both.
	State->Element.Reset();

	UE_LOG(LogVaCuus, Log, TEXT("M1 HUD off"));
}

/**
 * Minimal PIE-stop guard: any game-world tear-down while the HUD is active
 * turns it off through the normal path, before the viewport and RmlUi teardown
 * can race each other. Good enough for a debug toggle in M1.
 */
static void OnWorldBeginTearDown(UWorld* World)
{
	if (GState)
	{
		UE_LOG(LogVaCuus, Log, TEXT("M1 HUD: world tear-down, switching HUD off"));
		TearDown();
	}
}

/**
 * The missing M1 fallback (VaCuus-akj.6.7): the toggle used to fall back to the
 * inline document only when the VFS file was MISSING. Loading happens on the UI
 * thread now, so a file that exists but fails to parse reports back here through
 * UVaCuusView::OnLoadCompleted, and we retry with the inline document.
 */
static void OnViewLoadCompleted(UVaCuusView* View, bool bSuccess)
{
	if (!GState || GState->View.Get() != View)
	{
		return;
	}

	if (bSuccess)
	{
		GState->bLoadingFromFile = false;
		return;
	}

	if (!GState->bLoadingFromFile || GState->bTriedInlineFallback)
	{
		UE_LOG(LogVaCuus, Error, TEXT("M1 HUD: document load failed and no fallback is left; the HUD stays empty"));
		return;
	}

	GState->bLoadingFromFile = false;
	GState->bTriedInlineFallback = true;

	UE_LOG(LogVaCuus, Warning,
		TEXT("M1 HUD: '%s' exists but failed to load; falling back to the inline document"),
		GHudDocumentVfsPath);
	View->LoadDocumentFromMemory(GTestDocumentRml);
}

static void Toggle()
{
	if (GState)
	{
		TearDown();
		return;
	}

	if (!GEngine || !GEngine->GameViewport)
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("vacuus.M1HUD needs a game viewport (PIE or -game); it does nothing in a pure editor session"));
		return;
	}

	UGameViewportClient* Viewport = GEngine->GameViewport;
	UWorld* World = Viewport->GetWorld();
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	UVaCuusSubsystem* Subsystem = GameInstance ? GameInstance->GetSubsystem<UVaCuusSubsystem>() : nullptr;
	if (!Subsystem)
	{
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.M1HUD: no UVaCuusSubsystem on this game instance"));
		return;
	}

	const FIntPoint InitialViewSize =
		Viewport->Viewport ? Viewport->Viewport->GetSizeXY() : FIntPoint(1280, 720);

	// Prefer the real document from the project's DevUI content; fall back to
	// the inline probe document so the toggle keeps working on a bare project.
	const FString DocumentDiskPath = FPaths::ProjectContentDir() / TEXT("DevUI") / GHudDocumentVfsPath;
	const bool bLoadFromFile = FPaths::FileExists(DocumentDiskPath);
	if (bLoadFromFile)
	{
		UE_LOG(LogVaCuus, Log, TEXT("M1 HUD: loading document via VFS path '%s' ('%s')"),
			GHudDocumentVfsPath, *DocumentDiskPath);
	}
	else
	{
		UE_LOG(LogVaCuus, Log, TEXT("M1 HUD: '%s' not found, using the inline fallback document"),
			*DocumentDiskPath);
	}

	TSharedRef<FVaCuusSlateElement> Element = MakeShared<FVaCuusSlateElement>();

	// The subsystem hands the host to the process-wide UI thread, which boots it
	// (context creation and everything RmlUi-affine) on its own thread; the very
	// first view in the process is also what starts that thread and RmlUi.
	UVaCuusView* View = Subsystem->CreateView(MakeUnique<FVaCuusRmlDocumentHost>(Element), InitialViewSize);
	if (!View)
	{
		// Logged in detail by the subsystem/module; the element never touched the RHI,
		// so letting everything die here is safe.
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.M1HUD: no view could be created; HUD not shown"));
		return;
	}

	GState = MakeUnique<FState>();
	GState->Subsystem = Subsystem;
	GState->View = View;
	GState->Element = Element;
	GState->Viewport = Viewport;
	GState->InputWorld = World;
	GState->bLoadingFromFile = bLoadFromFile;
	GState->WorldTearDownHandle = FWorldDelegates::OnWorldBeginTearDown.AddStatic(&OnWorldBeginTearDown);

	// Bound before the load is queued: the completion is polled from the
	// subsystem's next tick, never earlier, but there is no reason to race it.
	View->OnLoadCompleted.AddStatic(&OnViewLoadCompleted);

	// Asynchronous by design: the document is loaded by the UI thread on its first
	// frame. The view size rides along so the very first layout is at the right size.
	if (bLoadFromFile)
	{
		View->LoadDocument(GHudDocumentVfsPath);
	}
	else
	{
		View->LoadDocumentFromMemory(GTestDocumentRml);
	}

	TSharedRef<SVaCuusWidget> Widget = SNew(SVaCuusWidget, View, Element);
	Viewport->AddViewportWidgetContent(Widget, /*ZOrder=*/100);
	GState->Widget = Widget;

	// After the widget is in the tree, so the input mode change lands on a viewport
	// that already has something to route to.
	SetUIInputMode(World, /*bEnable=*/true);

	UE_LOG(LogVaCuus, Log, TEXT("M1 HUD on (view %u, initial view %dx%d)"),
		View->GetViewId(), InitialViewSize.X, InitialViewSize.Y);
}

/**
 * Puts the pointer at a window position and lets Slate route the move.
 *
 * DELIBERATELY GOES THROUGH FSlateApplication rather than calling the widget: this
 * way the whole real path is exercised -- hit-test grid, bubble path,
 * SVaCuusWidget::OnMouseMove, the snapshot test, the queue -- and the only thing
 * synthesized is the position, which is the one part a headless session cannot
 * produce. ProcessMouseMoveEvent is public engine API (SlateApplication.h:1292) and
 * the same call the platform layer makes; nothing here is a test hook into VaCuus.
 */
static bool MoveMouseTo(const FVector2D& Position)
{
	if (!FSlateApplication::IsInitialized())
	{
		UE_LOG(LogVaCuus, Error, TEXT("Synthesizing a mouse move needs Slate (nothing to do under -nullrhi -unattended)"));
		return false;
	}

	FSlateApplication& Slate = FSlateApplication::Get();

	// Moves the platform cursor too, so anything that reads GetCursorPos() later
	// (OnCursorQuery, tooltips) agrees with the event we are about to send.
	Slate.SetCursorPos(Position);

	const FPointerEvent MouseEvent(
		FSlateApplicationBase::CursorPointerIndex,
		Position,
		Position,
		TSet<FKey>(),
		// A move affects no button. FKey() rather than EKeys::Invalid: the latter is
		// declared in InputCoreTypes.h but not exported, so it does not link.
		FKey(),
		/*WheelDelta=*/0.0f,
		FModifierKeysState());

	const bool bHandled = Slate.ProcessMouseMoveEvent(MouseEvent);

	UE_LOG(LogVaCuus, Log, TEXT("Pointer moved to (%.0f, %.0f); Slate reports the event %s"),
		Position.X, Position.Y, bHandled ? TEXT("handled by a widget") : TEXT("unhandled (it fell through)"));
	return bHandled;
}

static void SimulateMouseMove(const TArray<FString>& Args)
{
	if (Args.Num() < 2)
	{
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.M1HUD.Mouse expects two arguments: <x> <y> in window pixels"));
		return;
	}

	MoveMouseTo(FVector2D(FCString::Atof(*Args[0]), FCString::Atof(*Args[1])));
}

static FAutoConsoleCommand GSimulateMouseCommand(
	TEXT("vacuus.M1HUD.Mouse"),
	TEXT("Move the pointer to <x> <y> (window pixels) through Slate's real routing. Headless hover verification."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&SimulateMouseMove));

/** When HoverShot parks the pointer, and when it shoots. Ordered, and both after the widget exists. */
static constexpr float GHoverShotMoveSeconds = 2.0f;
static constexpr float GHoverShotShotSeconds = 3.0f;

/**
 * Headless hover verification in one command: park the pointer at <x> <y>, then
 * request a UI screenshot a second later.
 *
 * WHY IT SCHEDULES INSTEAD OF DOING: every `-ExecCmds` command runs on the same
 * early tick, before the widget has ever been arranged -- so a mouse move issued
 * there hits an empty hit-test grid and hovers nothing, and a screenshot taken there
 * catches a HUD that has not laid out. Two timed steps make the ordering explicit
 * (park, then shoot) instead of racing the frame rate, which is what counting
 * published frames would do.
 *
 * With no arguments it only shoots, which is the "not hovering anything" half of the
 * pair.
 */
static void HoverShot(const TArray<FString>& Args)
{
	const bool bMove = Args.Num() >= 2;
	const FVector2D Position = bMove
		? FVector2D(FCString::Atof(*Args[0]), FCString::Atof(*Args[1]))
		: FVector2D::ZeroVector;

	if (bMove)
	{
		// FROM OnBeginFrame, NOT FROM A TICKER, and that choice is the whole point of
		// the frame-ordering measurement (bead VaCuus-akj.6.13). OnBeginFrame broadcasts
		// at LaunchEngineLoop.cpp:5682, i.e. in the same window as the platform's own
		// dispatch: after nothing that matters and BEFORE GEngine->Tick (line 5859),
		// where UVaCuusSubsystem::Tick polls the snapshot. FTSTicker fires at line 6103,
		// long after that poll, so a move driven from there would measure the ticker's
		// position in the frame instead of input's and quietly invert the answer.
		static FDelegateHandle MoveHandle;
		static double MoveDeadline = 0.0;

		MoveDeadline = FPlatformTime::Seconds() + GHoverShotMoveSeconds;
		if (MoveHandle.IsValid())
		{
			FCoreDelegates::OnBeginFrame.Remove(MoveHandle);
		}

		MoveHandle = FCoreDelegates::OnBeginFrame.AddLambda(
			[Position]
			{
				if (FPlatformTime::Seconds() < MoveDeadline)
				{
					return;
				}

				FCoreDelegates::OnBeginFrame.Remove(MoveHandle);
				MoveHandle.Reset();
				MoveMouseTo(Position);
			});
	}

	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[](float)
		{
			UE_LOG(LogVaCuus, Log, TEXT("vacuus.M1HUD.HoverShot: requesting a UI screenshot"));
			FScreenshotRequest::RequestScreenshot(/*bInShowUI=*/true);
			return false;
		}),
		GHoverShotShotSeconds);

	UE_LOG(LogVaCuus, Log, TEXT("vacuus.M1HUD.HoverShot: %s at t+%.1fs, screenshot at t+%.1fs"),
		bMove ? *FString::Printf(TEXT("pointer to (%.0f, %.0f)"), Position.X, Position.Y) : TEXT("no pointer move"),
		GHoverShotMoveSeconds, GHoverShotShotSeconds);
}

static FAutoConsoleCommand GHoverShotCommand(
	TEXT("vacuus.M1HUD.HoverShot"),
	TEXT("Park the pointer at [x y] after 2s, then take a UI screenshot at 3s. No arguments: screenshot only."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&HoverShot));

static FAutoConsoleCommand GToggleCommand(
	TEXT("vacuus.M1HUD"),
	TEXT("Toggle the M1 render-spike HUD: records an RmlUi document (Content/DevUI/m1_hud.rml, or an inline fallback) ")
	TEXT("each frame and composites it over the game viewport."),
	FConsoleCommandDelegate::CreateStatic(&Toggle));
} // namespace VaCuusM1HUD

class FVaCuusRenderModule : public IModuleInterface
{
public:
	//~ Begin IModuleInterface
	virtual void StartupModule() override
	{
		// Map the plugin shader directory before global shader compilation kicks
		// in — the module loads at PostConfigInit for exactly this reason.
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("VaCuus"));
		checkf(Plugin.IsValid(), TEXT("VaCuus plugin descriptor not found"));
		const FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/VaCuus"), ShaderDir);
	}

	virtual void ShutdownModule() override
	{
		// Engine shutdown with the HUD still on: drop the widget and our own
		// references. The view itself is normally already gone (the game instance's
		// subsystem deinitializes before modules unload), and the UI thread was
		// stopped by VaCuus::ShutdownModule() ahead of this one.
		VaCuusM1HUD::TearDown();
	}
	//~ End IModuleInterface
};

IMPLEMENT_MODULE(FVaCuusRenderModule, VaCuusRender)
