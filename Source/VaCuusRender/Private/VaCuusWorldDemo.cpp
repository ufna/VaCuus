// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusDefines.h"
#include "VaCuusView.h"
#include "VaCuusWorldComponent.h"
#include "VaCuusWorldSink.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UnrealClient.h"

/**
 * The M5 Task 6 runtime proof and the experiments' harness: the M1 HUD document on
 * a quad in front of the camera, next to (optionally) the screen-space composite of
 * the same document via vacuus.M1HUD.
 *
 *  - WS-GAMMA: `vacuus.WorldDemo.Decode 0|1` flips the preset's VaCuusDecodeSRGB
 *    scalar on the live MID; screenshot both against the screen composite and pick
 *    by parity (the decision is recorded in the preset's default and
 *    docs/research/proofs/m5-t6-worldspace/).
 *  - WS-COPY-COST: run at `vacuus.WorldDemo 1024 1024`, enable vacuus.M1HUD.PerfLog;
 *    the WorldCopy (RT) line's per-window sample count IS the copy count, its
 *    avg/p99 the per-copy render-thread cost. `vacuus.WorldDemo.Stats` prints the
 *    sink's cumulative arrival/copy/skip counters on demand.
 */
namespace VaCuusWorldDemo
{
static const TCHAR* GDocumentVfsPath = TEXT("m1_hud.rml");

struct FState
{
	TWeakObjectPtr<AActor> Actor;
	TWeakObjectPtr<UVaCuusWorldComponent> Component;
	FDelegateHandle WorldTearDownHandle;
};

static TUniquePtr<FState> GState;

static void ScheduleAfter(float DelaySeconds, TFunction<void()> Work)
{
	if (DelaySeconds <= 0.0f)
	{
		Work();
		return;
	}
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[Work = MoveTemp(Work)](float)
		{
			Work();
			return false;
		}),
		DelaySeconds);
}

static void TearDown()
{
	if (!GState)
	{
		return;
	}
	TUniquePtr<FState> State = MoveTemp(GState);
	FWorldDelegates::OnWorldBeginTearDown.Remove(State->WorldTearDownHandle);

	if (UVaCuusWorldComponent* Component = State->Component.Get())
	{
		if (TSharedPtr<FVaCuusWorldSink> Sink = Component->GetWorldSink())
		{
			UE_LOG(LogVaCuus, Log, TEXT("World demo sink totals: %llu buffer(s) arrived, %llu copie(s), %llu extent skip(s)"),
				Sink->GetNumArrivals(), Sink->GetNumCopies(), Sink->GetNumExtentSkips());
		}
	}

	// Destroying the actor unregisters the component, which retires the view --
	// the whole teardown path under test.
	if (AActor* Actor = State->Actor.Get())
	{
		Actor->Destroy();
	}
	UE_LOG(LogVaCuus, Log, TEXT("World demo off"));
}

static void OnWorldBeginTearDown(UWorld* World)
{
	if (GState)
	{
		UE_LOG(LogVaCuus, Log, TEXT("World demo: world tear-down, switching off"));
		TearDown();
	}
}

static void SpawnPanel(FIntPoint Size, float Scale, FString DocumentPath)
{
	if (!GEngine || !GEngine->GameViewport)
	{
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.WorldDemo needs a game viewport (PIE or -game)"));
		return;
	}
	UWorld* World = GEngine->GameViewport->GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.WorldDemo: no player controller to place the panel in front of"));
		return;
	}

	FVector CamLoc;
	FRotator CamRot;
	PC->GetPlayerViewPoint(CamLoc, CamRot);
	const FVector Forward = CamRot.Vector();
	const FVector PanelLoc = CamLoc + Forward * 220.0f;
	UE_LOG(LogVaCuus, Log, TEXT("World demo placement: camera at %s rot %s, panel at %s"),
		*CamLoc.ToCompactString(), *CamRot.ToCompactString(), *PanelLoc.ToCompactString());

	AActor* Actor = World->SpawnActor<AActor>();
	if (!Actor)
	{
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.WorldDemo: SpawnActor failed"));
		return;
	}

	UVaCuusWorldComponent* Component = NewObject<UVaCuusWorldComponent>(Actor);
	Component->DrawSize = Size;
	Component->DocumentPath = MoveTemp(DocumentPath);
	Component->bAutoLoadDocument = true;
	Actor->SetRootComponent(Component);
	Component->RegisterComponent();

	// The quad's +X normal points back at the camera; world size = pixels * scale.
	Actor->SetActorLocationAndRotation(PanelLoc, (-Forward).Rotation());
	Component->SetWorldScale3D(FVector(Scale));

	GState = MakeUnique<FState>();
	GState->Actor = Actor;
	GState->Component = Component;
	GState->WorldTearDownHandle = FWorldDelegates::OnWorldBeginTearDown.AddStatic(&OnWorldBeginTearDown);

	const UVaCuusView* View = Component->GetView();
	UE_LOG(LogVaCuus, Log, TEXT("World demo on: %dx%d px at scale %.2f (view %s, document '%s')"),
		Size.X, Size.Y, Scale,
		View ? *FString::Printf(TEXT("%u"), View->GetViewId()) : TEXT("none"), *Component->DocumentPath);
}

static void Toggle(const TArray<FString>& Args)
{
	if (GState)
	{
		TearDown();
		return;
	}

	const FIntPoint Size(
		Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 1024,
		Args.Num() > 1 ? FCString::Atoi(*Args[1]) : 512);
	const float Scale = Args.Num() > 2 ? FCString::Atof(*Args[2]) : 0.15f;
	FString Document = Args.Num() > 4 ? Args[4] : FString(GDocumentVfsPath);

	// Optional delay for -ExecCmds runs, where everything executes on one early tick
	// before the camera (or even the player controller) exists.
	ScheduleAfter(Args.Num() > 3 ? FCString::Atof(*Args[3]) : 0.0f,
		[Size, Scale, Document = MoveTemp(Document)] { SpawnPanel(Size, Scale, Document); });
}

static void SetDecode(const TArray<FString>& Args)
{
	if (Args.Num() < 1)
	{
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.WorldDemo.Decode expects <0|1> [delaySeconds]"));
		return;
	}
	const float Value = FCString::Atof(*Args[0]);
	ScheduleAfter(Args.Num() > 1 ? FCString::Atof(*Args[1]) : 0.0f,
		[Value]
		{
			UVaCuusWorldComponent* Component = GState ? GState->Component.Get() : nullptr;
			UMaterialInstanceDynamic* Mid = Component ? Component->GetMaterialInstance() : nullptr;
			if (!Mid)
			{
				UE_LOG(LogVaCuus, Error, TEXT("vacuus.WorldDemo.Decode: no live world demo panel"));
				return;
			}
			Mid->SetScalarParameterValue(TEXT("VaCuusDecodeSRGB"), Value);
			UE_LOG(LogVaCuus, Log, TEXT("World demo: VaCuusDecodeSRGB = %.2f"), Value);
		});
}

static void Shot(const TArray<FString>& Args)
{
	ScheduleAfter(Args.Num() > 0 ? FCString::Atof(*Args[0]) : 0.0f,
		[]
		{
			UE_LOG(LogVaCuus, Log, TEXT("vacuus.WorldDemo.Shot: requesting a screenshot"));
			FScreenshotRequest::RequestScreenshot(/*bInShowUI=*/true);
		});
}

static void Stats(const TArray<FString>& Args)
{
	ScheduleAfter(Args.Num() > 0 ? FCString::Atof(*Args[0]) : 0.0f,
		[]
		{
			UVaCuusWorldComponent* Component = GState ? GState->Component.Get() : nullptr;
			TSharedPtr<FVaCuusWorldSink> Sink = Component ? Component->GetWorldSink() : nullptr;
			if (!Sink)
			{
				UE_LOG(LogVaCuus, Display, TEXT("vacuus.WorldDemo.Stats: no live world demo panel"));
				return;
			}
			UE_LOG(LogVaCuus, Display, TEXT("World demo sink: %llu buffer(s) arrived, %llu copie(s), %llu extent skip(s)"),
				Sink->GetNumArrivals(), Sink->GetNumCopies(), Sink->GetNumExtentSkips());
			UE_LOG(LogVaCuus, Display,
				TEXT("World demo panel: at %s, bounds origin %s extent %s, registered=%d, proxy=%d, visible=%d, RT=%dx%d"),
				*Component->GetComponentLocation().ToCompactString(), *Component->Bounds.Origin.ToCompactString(),
				*Component->Bounds.BoxExtent.ToCompactString(), Component->IsRegistered() ? 1 : 0,
				Component->SceneProxy != nullptr ? 1 : 0, Component->IsVisible() ? 1 : 0,
				Component->GetRenderTarget() ? Component->GetRenderTarget()->SizeX : 0,
				Component->GetRenderTarget() ? Component->GetRenderTarget()->SizeY : 0);
		});
}

static FAutoConsoleCommand GToggleCommand(
	TEXT("vacuus.WorldDemo"),
	TEXT("Toggle a UVaCuusWorldComponent quad in front of the camera. ")
	TEXT("[sizeX] [sizeY] [scale] [delaySeconds] [docVfsPath]; defaults 1024 512 0.15 0 m1_hud.rml. Run vacuus.M1HUD ")
	TEXT("beside it for the WS-GAMMA screen-composite reference; an animated document (m4_demo.rml) for WS-COPY-COST."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&Toggle));

static FAutoConsoleCommand GDecodeCommand(
	TEXT("vacuus.WorldDemo.Decode"),
	TEXT("Set the live panel MID's VaCuusDecodeSRGB scalar (<0|1> [delaySeconds]) — the WS-GAMMA A/B knob."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&SetDecode));

static FAutoConsoleCommand GShotCommand(
	TEXT("vacuus.WorldDemo.Shot"),
	TEXT("Take a screenshot after [delaySeconds] — the WS-GAMMA beats."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&Shot));

static FAutoConsoleCommand GStatsCommand(
	TEXT("vacuus.WorldDemo.Stats"),
	TEXT("Print the live panel's sink counters: buffers arrived, copies issued, extent skips (WS-COPY-COST)."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&Stats));
}	 // namespace VaCuusWorldDemo
