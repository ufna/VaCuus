// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "SVaCuusHUDWidget.h"
#include "VaCuusDefines.h"
#include "VaCuusEngine.h"
#include "VaCuusRmlDocumentHost.h"
#include "VaCuusSlateElement.h"
#include "VaCuusUIThread.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
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
 * Inline fallback document, used when Content/DevUI/m1_hud.rml is missing.
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
 * Everything the HUD toggle owns while it is ON.
 *
 * The UI thread lives here for Task 3 only; Task 4 moves ownership to
 * UVaCuusSubsystem and this console command becomes a thin client of it.
 */
struct FState
{
	TUniquePtr<FVaCuusUIThread> UIThread;
	TSharedPtr<FVaCuusSlateElement> Element;
	TSharedPtr<SVaCuusHUDWidget> Widget;
	TWeakObjectPtr<UGameViewportClient> Viewport;
	FDelegateHandle WorldTearDownHandle;
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

	// Spec §4 teardown order:
	//
	// 1. Stop accepting commands. Detaching first means no resize command and no
	// trigger can land behind the drain below; pulling the widget out of the
	// viewport then stops the paints. On PIE/world shutdown the viewport (or its
	// widget tree) may already be gone — weak ptr guards that.
	State->Widget->DetachUIThread();
	if (UGameViewportClient* Viewport = State->Viewport.Get())
	{
		Viewport->RemoveViewportWidgetContent(State->Widget.ToSharedRef());
	}
	State->Widget.Reset();

	// 2. Drain and stop the UI thread. The destructor requests the stop, joins, and
	// the worker's Exit() closes the document, removes the context and shuts RmlUi
	// down — all on the UI thread, which is the only thread allowed to.
	State->UIThread.Reset();

	// 3. Render-side teardown: release the replayer's RHI resources on the
	// render thread, then let the element ref die with the lambda so its
	// destruction happens after the release. Ordering against the UI thread's own
	// publishes is guaranteed by step 2 having joined it.
	ENQUEUE_RENDER_COMMAND(VaCuusM1HUDRelease)(
		[Element = MoveTemp(State->Element)](FRHICommandListImmediate&)
		{
			Element->ReleaseResources_RenderThread();
		});

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

	if (FVaCuusEngine::Get().IsInitialized())
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("vacuus.M1HUD: RmlUi is already initialized, but the recorder must be installed pre-init. ")
			TEXT("Wait for the other user (e.g. a running test) to shut RmlUi down, then retry"));
		return;
	}

	UGameViewportClient* Viewport = GEngine->GameViewport;
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

	// The UI thread boots the document host inside its own Init(), so RmlUi comes up
	// on that thread and never on this one. A failure there (RmlUi already
	// initialized, context creation, no multithreading support) makes Start() return
	// false with the thread already gone.
	TUniquePtr<FVaCuusUIThread> UIThread = MakeUnique<FVaCuusUIThread>();
	UIThread->SetDocumentHost(MakeUnique<FVaCuusRmlDocumentHost>(Element));
	if (!UIThread->Start())
	{
		// Logged inside Start(); the element never touched the RHI, so letting
		// everything die here is safe.
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.M1HUD: the UI thread did not start; HUD not shown"));
		return;
	}

	// Asynchronous by design: the document is loaded by the UI thread on its first
	// frame. The view size rides along so the very first layout is at the right size.
	if (bLoadFromFile)
	{
		UIThread->EnqueueLoadDocumentFile(GHudDocumentVfsPath, InitialViewSize);
	}
	else
	{
		UIThread->EnqueueLoadDocumentFromMemory(GTestDocumentRml, InitialViewSize);
	}

	TSharedRef<SVaCuusHUDWidget> Widget = SNew(SVaCuusHUDWidget, UIThread.Get(), Element);
	Viewport->AddViewportWidgetContent(Widget, /*ZOrder=*/100);

	GState = MakeUnique<FState>();
	GState->UIThread = MoveTemp(UIThread);
	GState->Element = Element;
	GState->Widget = Widget;
	GState->Viewport = Viewport;
	GState->WorldTearDownHandle = FWorldDelegates::OnWorldBeginTearDown.AddStatic(&OnWorldBeginTearDown);

	UE_LOG(LogVaCuus, Log, TEXT("M1 HUD on (initial view %dx%d)"), InitialViewSize.X, InitialViewSize.Y);
}

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
		// Engine shutdown with the HUD still on: tear down while the RHI and
		// render thread are alive rather than leaking RmlUi + replayer state.
		VaCuusM1HUD::TearDown();
	}
	//~ End IModuleInterface
};

IMPLEMENT_MODULE(FVaCuusRenderModule, VaCuusRender)
