// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "SVaCuusHUDWidget.h"
#include "VaCuusDefines.h"
#include "VaCuusEngine.h"
#include "VaCuusM1Harness.h"
#include "VaCuusSlateElement.h"

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
 * Inline placeholder document until Task 9 ships m1_hud.rml. The pure red and
 * pure blue divs are the channel-order probe: if the left div renders blue,
 * an RGBA/BGRA swap crept into the vertex or texture path.
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

/** Everything the HUD toggle owns while it is ON. */
struct FState
{
	TSharedPtr<FVaCuusM1Harness> Harness;
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

	// 1. Pull the widget out of the viewport. On PIE/world shutdown the
	// viewport (or its widget tree) may already be gone — weak ptr guards that.
	if (UGameViewportClient* Viewport = State->Viewport.Get())
	{
		Viewport->RemoveViewportWidgetContent(State->Widget.ToSharedRef());
	}
	State->Widget.Reset();

	// 2. Game-side teardown, spec §4 order: close document -> RemoveContext ->
	// engine Shutdown() -> drop recorder (all inside the harness).
	State->Harness->Shutdown();
	State->Harness.Reset();

	// 3. Render-side teardown: release the replayer's RHI resources on the
	// render thread, then let the element ref die with the lambda so its
	// destruction happens after the release. In-flight Slate batches may
	// briefly co-own the element; Draw after release is a safe no-op.
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

	TSharedRef<FVaCuusSlateElement> Element = MakeShared<FVaCuusSlateElement>();
	TSharedRef<FVaCuusM1Harness> Harness = MakeShared<FVaCuusM1Harness>(Element);
	if (!Harness->Boot(InitialViewSize, FString(GTestDocumentRml)))
	{
		// Boot logged and rolled back; the element never touched the RHI, so
		// letting both die here is safe.
		return;
	}

	TSharedRef<SVaCuusHUDWidget> Widget = SNew(SVaCuusHUDWidget, Harness, Element);
	Viewport->AddViewportWidgetContent(Widget, /*ZOrder=*/100);

	GState = MakeUnique<FState>();
	GState->Harness = Harness;
	GState->Element = Element;
	GState->Widget = Widget;
	GState->Viewport = Viewport;
	GState->WorldTearDownHandle = FWorldDelegates::OnWorldBeginTearDown.AddStatic(&OnWorldBeginTearDown);

	UE_LOG(LogVaCuus, Log, TEXT("M1 HUD on (initial view %dx%d)"), InitialViewSize.X, InitialViewSize.Y);
}

static FAutoConsoleCommand GToggleCommand(
	TEXT("vacuus.M1HUD"),
	TEXT("Toggle the M1 render-spike HUD: records an inline RmlUi document each frame and composites it over the game viewport."),
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
