// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusDefines.h"
#include "VaCuusDemoModel.h"
#include "VaCuusInteractiveSnapshot.h"
#include "VaCuusView.h"
#include "VaCuusWorldComponent.h"
#include "VaCuusWorldInputProcessor.h"
#include "VaCuusWorldSink.h"

#include "Components/InputComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Layout/WidgetPath.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/CoreDelegates.h"
#include "UObject/StrongObjectPtr.h"
#include "UnrealClient.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SViewport.h"

/**
 * The M5 world-space runtime proofs.
 *
 * TASK 6 (`vacuus.WorldDemo`): the M1 HUD document on a quad in front of the
 * camera, next to (optionally) the screen-space composite of the same document via
 * vacuus.M1HUD.
 *
 *  - WS-GAMMA: `vacuus.WorldDemo.Decode 0|1` flips the preset's VaCuusDecodeSRGB
 *    scalar on the live MID; screenshot both against the screen composite and pick
 *    by parity (the decision is recorded in the preset's default and
 *    docs/research/proofs/m5-t6-worldspace/).
 *  - WS-COPY-COST: run at `vacuus.WorldDemo 1024 1024`, enable vacuus.M1HUD.PerfLog;
 *    the WorldCopy (RT) line's per-window sample count IS the copy count, its
 *    avg/p99 the per-copy render-thread cost. `vacuus.WorldDemo.Stats` prints the
 *    sink's cumulative arrival/copy/skip counters on demand.
 *
 * TASK 7 (`vacuus.M5World` + `vacuus.M5World.InputSmoke`): m4_demo.rml on the quad
 * with JS and data binding LIVE (BindModel before LoadDocument, the m4 ordering
 * contract; a per-frame model pump; the OnModelWrite ear), and the PIE-shaped
 * functional test of the raycast input path. The smoke runs in a headless `-game`
 * session and drives SYNTHETIC EVENTS THROUGH FSlateApplication::Process* -- the M2
 * acceptance route (VaCuusRender.cpp's MoveMouseTo/ClickWhereThePointerIs argument):
 * that path runs the REAL preprocessor chain (Process* consults InputPreProcessors
 * first, SlateApplication.cpp:5324, :6148, :6401), the real hit-test grid, the real
 * routing to the viewport -- the only synthesized thing is the position. What a
 * -game run cannot exercise vs. real PIE: an editor world wrapper and OS-generated
 * events; the routing, tracing, consumption and write-router legs are the
 * production ones.
 */
namespace VaCuusWorldDemo
{
static const TCHAR* GDocumentVfsPath = TEXT("m1_hud.rml");
static const TCHAR* GInteractiveDocumentVfsPath = TEXT("m4_demo.rml");

/** The demo model's name -- must match m4_demo.rml's data-model="hud". A bare string for BindModel (the FName-pool casing trap, VaCuus-akj.23). */
static const TCHAR* GModelName = TEXT("hud");

struct FState
{
	TWeakObjectPtr<AActor> Actor;
	TWeakObjectPtr<UVaCuusWorldComponent> Component;
	FDelegateHandle WorldTearDownHandle;

	//~ The Task 7 interactive half (vacuus.M5World only).

	/** True when the panel shows m4_demo.rml with the model driver and write ear armed. */
	bool bInteractiveDemo = false;

	/**
	 * True when THIS panel was spawned by vacuus.M5Demo (Task 9.1). The flag exists so
	 * the acceptance demo's teardown retires only its own quad: TearDownM5HudQuad on a
	 * user-launched vacuus.WorldDemo/vacuus.M5World panel must be a no-op.
	 */
	bool bM5DemoQuad = false;

	/** The live struct the pump publishes; Ammo is written ONLY by the OnModelWrite handler (I3: the UI never writes the shadow, the game applies and echoes). */
	FVaCuusDemoModel Model;

	FDelegateHandle ModelPumpHandle;
	double ModelStartSeconds = 0.0;

	/** The OnModelWrite / OnJsEvent adapter (dynamic delegates need a UObject; see UVaCuusDemoWriteListener). */
	TStrongObjectPtr<UVaCuusDemoWriteListener> Listener;

	/** Observables the smoke asserts. Game thread only. */
	uint64 NumAcceptedWrites = 0;
	uint64 NumHoverOn = 0;
	uint64 NumHoverOff = 0;
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

// ---------------------------------------------------------------------------
// The input smoke's state (declared before TearDown so teardown can clean it up)
// ---------------------------------------------------------------------------

struct FSmoke
{
	int32 NumPassed = 0;
	int32 NumFailed = 0;

	//~ Baselines captured immediately before the action they measure.
	uint64 BaseWrites = 0;
	uint64 BaseConsumed = 0;
	uint64 BaseDeferred = 0;
	uint64 BasePassed = 0;
	uint64 BaseLeaves = 0;
	uint64 BaseHoverOn = 0;
	uint64 BaseHoverOff = 0;
	uint64 BaseGameClicks = 0;

	/** Incremented by the pass-through receiver (the PC input-stack ear). */
	uint64 NumGameClicks = 0;

	/** Incremented by the occlusion overlay's OnClicked. */
	uint64 NumOverlayClicks = 0;

	FVector2D ButtonDesktopPos = FVector2D::ZeroVector;
	FVector2D ButtonViewportPos = FVector2D::ZeroVector;

	TSharedPtr<SWidget> Overlay;
};

static TUniquePtr<FSmoke> GSmoke;

static void CleanupSmokeOverlay()
{
	if (GSmoke && GSmoke->Overlay.IsValid() && GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->RemoveViewportWidgetContent(GSmoke->Overlay.ToSharedRef());
	}
	if (GSmoke)
	{
		GSmoke->Overlay.Reset();
	}
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static void TearDown()
{
	if (!GState)
	{
		return;
	}
	TUniquePtr<FState> State = MoveTemp(GState);
	FWorldDelegates::OnWorldBeginTearDown.Remove(State->WorldTearDownHandle);

	CleanupSmokeOverlay();
	GSmoke.Reset();

	if (State->ModelPumpHandle.IsValid())
	{
		FCoreDelegates::OnBeginFrame.Remove(State->ModelPumpHandle);
	}

	if (UVaCuusWorldComponent* Component = State->Component.Get())
	{
		if (UVaCuusView* View = Component->GetView())
		{
			if (State->Listener.IsValid())
			{
				View->OnModelWrite.RemoveDynamic(State->Listener.Get(), &UVaCuusDemoWriteListener::HandleModelWrite);
				View->OnJsEvent.RemoveDynamic(State->Listener.Get(), &UVaCuusDemoWriteListener::HandleJsEvent);
			}
		}
		if (TSharedPtr<FVaCuusWorldSink> Sink = Component->GetWorldSink())
		{
			UE_LOG(LogVaCuus, Log, TEXT("World demo sink totals: %llu buffer(s) arrived, %llu copie(s), %llu extent skip(s)"),
				Sink->GetNumArrivals(), Sink->GetNumCopies(), Sink->GetNumExtentSkips());
		}
	}

	if (State->Listener.IsValid())
	{
		State->Listener->OnWrite = nullptr;
		State->Listener->OnEvent = nullptr;
		State->Listener.Reset();
	}

	// Destroying the actor unregisters the component, which retires the view --
	// the whole teardown path under test (including the input processor's
	// last-panel uninstall).
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

/**
 * The per-frame model pump for the interactive demo -- the M4 driver's shape
 * reduced to what the smoke needs: gameplay-fed fields flow every frame, and Ammo
 * carries the accepted-write echo. OnBeginFrame rather than a ticker for the M4
 * driver's exact reason: it broadcasts BEFORE UVaCuusSubsystem::Tick publishes
 * (LaunchEngineLoop.cpp:5682 vs :5859), so an update never sits a frame in the
 * channel.
 */
static void PumpInteractiveModel()
{
	if (!GState || !GState->bInteractiveDemo)
	{
		return;
	}
	UVaCuusWorldComponent* Component = GState->Component.Get();
	UVaCuusView* View = Component ? Component->GetView() : nullptr;
	if (!View)
	{
		return;
	}

	const double T = FPlatformTime::Seconds() - GState->ModelStartSeconds;
	FVaCuusDemoModel& Model = GState->Model;
	Model.Tick += 1;
	Model.Health = 55.0f + 45.0f * static_cast<float>(FMath::Sin(T * 0.6));
	Model.CallSign = TEXT("WORLD-1");
	Model.Zone = FName(TEXT("Quad"));
	Model.Objective = FText::AsCultureInvariant(TEXT("Click me by raycast"));
	Model.Target.Designation = TEXT("M5-T7");
	Model.Target.Distance = 220;

	View->UpdateModel(FName(GModelName), FVaCuusDemoModel::StaticStruct(), &Model);
}

static void SpawnPanel(FIntPoint Size, float Scale, FString DocumentPath, bool bInteractive)
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
	// The interactive demo loads MANUALLY, after BindModel: RmlUi resolves
	// `data-model` exactly once, in Element::SetParent when the body is parented
	// (Element.cpp:2202-2219), so the model must exist before the load is enqueued.
	// The queue is FIFO from this one producer, which is the whole ordering
	// guarantee (the M4 StartModelDriver argument, VaCuusRender.cpp:662-672).
	Component->DocumentPath = DocumentPath;
	Component->bAutoLoadDocument = !bInteractive;
	Actor->SetRootComponent(Component);
	Component->RegisterComponent();

	// The quad's +X normal points back at the camera; world size = pixels * scale.
	Actor->SetActorLocationAndRotation(PanelLoc, (-Forward).Rotation());
	Component->SetWorldScale3D(FVector(Scale));

	GState = MakeUnique<FState>();
	GState->Actor = Actor;
	GState->Component = Component;
	GState->WorldTearDownHandle = FWorldDelegates::OnWorldBeginTearDown.AddStatic(&OnWorldBeginTearDown);

	UVaCuusView* View = Component->GetView();

	if (bInteractive && View)
	{
		GState->bInteractiveDemo = true;

		if (!View->BindModel(GModelName, FVaCuusDemoModel::StaticStruct()))
		{
			UE_LOG(LogVaCuus, Error,
				TEXT("vacuus.M5World: the data model could not be bound; m4_demo.rml will load and show nothing"));
		}

		GState->Listener = TStrongObjectPtr<UVaCuusDemoWriteListener>(NewObject<UVaCuusDemoWriteListener>());
		GState->Listener->OnWrite = [](FName Model, const FString& Path, const FVaCuusJsValue& Value)
		{
			if (!GState || !GState->bInteractiveDemo)
			{
				return;
			}
			if (Model != FName(GModelName) || Path != TEXT("Ammo") || Value.Kind != EVaCuusJsValueKind::Number)
			{
				UE_LOG(LogVaCuus, Warning, TEXT("vacuus.M5World: routed write to '%s' path '%s' (kind %d) ignored"),
					*Model.ToString(), *Path, int32(Value.Kind));
				return;
			}

			// Clamped, not trusted -- the value came from a document expression (the
			// M4 driver's argument verbatim).
			GState->Model.Ammo = FMath::Max(0, FMath::RoundToInt32(Value.Number));
			++GState->NumAcceptedWrites;
			UE_LOG(LogVaCuus, Display,
				TEXT("vacuus.M5World: accepted routed write Ammo = %d (write #%llu); the next pump echoes it back"),
				GState->Model.Ammo, GState->NumAcceptedWrites);
		};
		GState->Listener->OnEvent = [](FName Name, const TArray<FVaCuusJsKeyValue>& Payload)
		{
			if (!GState || Name != FName(TEXT("smoke_hover")))
			{
				return;
			}
			bool bOn = false;
			for (const FVaCuusJsKeyValue& Pair : Payload)
			{
				if (Pair.Key == TEXT("on"))
				{
					bOn = Pair.Value.Kind == EVaCuusJsValueKind::Number && Pair.Value.Number != 0.0;
				}
			}
			++(bOn ? GState->NumHoverOn : GState->NumHoverOff);
		};
		View->OnModelWrite.AddDynamic(GState->Listener.Get(), &UVaCuusDemoWriteListener::HandleModelWrite);
		View->OnJsEvent.AddDynamic(GState->Listener.Get(), &UVaCuusDemoWriteListener::HandleJsEvent);

		GState->Model = FVaCuusDemoModel();
		GState->ModelStartSeconds = FPlatformTime::Seconds();
		GState->ModelPumpHandle = FCoreDelegates::OnBeginFrame.AddStatic(&PumpInteractiveModel);

		View->LoadDocument(DocumentPath);
	}

	UE_LOG(LogVaCuus, Log, TEXT("World demo on: %dx%d px at scale %.2f (view %s, document '%s'%s)"),
		Size.X, Size.Y, Scale,
		View ? *FString::Printf(TEXT("%u"), View->GetViewId()) : TEXT("none"), *Component->DocumentPath,
		bInteractive ? TEXT(", interactive") : TEXT(""));
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
		[Size, Scale, Document = MoveTemp(Document)] { SpawnPanel(Size, Scale, Document, /*bInteractive=*/false); });
}

/** vacuus.M5World [delaySeconds] [sizeX] [sizeY] [scale]: the Task 7 interactive panel. */
static void ToggleInteractive(const TArray<FString>& Args)
{
	if (GState)
	{
		TearDown();
		return;
	}

	const float DelaySeconds = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 0.0f;
	const FIntPoint Size(
		Args.Num() > 1 ? FCString::Atoi(*Args[1]) : 1024,
		Args.Num() > 2 ? FCString::Atoi(*Args[2]) : 1024);
	const float Scale = Args.Num() > 3 ? FCString::Atof(*Args[3]) : 0.12f;

	ScheduleAfter(DelaySeconds,
		[Size, Scale] { SpawnPanel(Size, Scale, GInteractiveDocumentVfsPath, /*bInteractive=*/true); });
}

/**
 * The M5 acceptance demo's quad (plan Task 9.1, spec §8: "the same HUD on a world
 * quad"): the SAME document vacuus.M5Demo composites on screen, on an interactive
 * world panel — model 'hud' bound before the load and pumped (the quad's health
 * bar is the model-fed one), the raycast path live so its Simulate button clicks.
 * Placed a few degrees RIGHT of the camera's initial heading so vacuus.M5Demo's
 * oscillating pan sweeps the scene under the screen HUD's glass while the quad
 * stays in frame. Reuses vacuus.M5World's whole lifetime (SpawnPanel/TearDown);
 * bM5DemoQuad scopes the acceptance demo's teardown to its own panel.
 */
void SpawnM5HudQuad(const TCHAR* DocumentVfsPath, float DelaySeconds)
{
	const FString Document(DocumentVfsPath);
	ScheduleAfter(DelaySeconds,
		[Document]
		{
			if (GState)
			{
				UE_LOG(LogVaCuus, Warning,
					TEXT("vacuus.M5Demo: a world panel is already up; the demo quad is not spawned"));
				return;
			}

			SpawnPanel(FIntPoint(512, 512), /*Scale=*/0.22f, Document, /*bInteractive=*/true);
			if (!GState)
			{
				return;	   // SpawnPanel already logged why
			}
			GState->bM5DemoQuad = true;

			// Re-place off-center: 16 degrees right at 300 units, still facing the camera.
			UWorld* World = GEngine->GameViewport->GetWorld();
			APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
			AActor* Actor = GState->Actor.Get();
			if (PC != nullptr && Actor != nullptr)
			{
				FVector CamLoc;
				FRotator CamRot;
				PC->GetPlayerViewPoint(CamLoc, CamRot);
				const FVector Direction = CamRot.Vector().RotateAngleAxis(16.0f, FVector::UpVector);
				Actor->SetActorLocationAndRotation(CamLoc + Direction * 300.0f, (-Direction).Rotation());
			}
		});
}

/** vacuus.M5Demo's teardown half: retire the demo quad IF it is the one up (see bM5DemoQuad). */
void TearDownM5HudQuad()
{
	if (GState && GState->bM5DemoQuad)
	{
		TearDown();
	}
}

// ---------------------------------------------------------------------------
// vacuus.M5World.InputSmoke -- the PIE-shaped functional test (plan Task 7.2)
// ---------------------------------------------------------------------------

static void SmokeCheck(bool bCondition, const TCHAR* What)
{
	if (!GSmoke)
	{
		return;
	}
	if (bCondition)
	{
		++GSmoke->NumPassed;
		UE_LOG(LogVaCuus, Display, TEXT("InputSmoke PASS: %s"), What);
	}
	else
	{
		++GSmoke->NumFailed;
		UE_LOG(LogVaCuus, Error, TEXT("InputSmoke FAIL: %s"), What);
	}
}

/**
 * Synthetic pointer events through FSlateApplication::Process* -- the M2 route (see
 * the file comment): the whole REAL pipeline runs, preprocessor included; only the
 * position is synthesized. SetCursorPos first so anything reading GetCursorPos()
 * later agrees with the event.
 */
static void SmokeMove(const FVector2D& Position)
{
	FSlateApplication& Slate = FSlateApplication::Get();
	Slate.SetCursorPos(Position);
	const FPointerEvent MoveEvent(FSlateApplicationBase::CursorPointerIndex, Position, Position, TSet<FKey>(),
		FKey(), /*WheelDelta=*/0.0f, FModifierKeysState());
	Slate.ProcessMouseMoveEvent(MoveEvent);
}

static void SmokeClick(const FVector2D& Position)
{
	FSlateApplication& Slate = FSlateApplication::Get();

	// The pressed-button sets differ between down and up, faithfully:
	// FSlateApplication removes the released button before constructing the up
	// event (SlateApplication.cpp:6144-6147), the asymmetry the processor's latch
	// release depends on.
	const TSet<FKey> LeftOnly = {EKeys::LeftMouseButton};
	const FPointerEvent DownEvent(FSlateApplicationBase::CursorPointerIndex, Position, Position, LeftOnly,
		EKeys::LeftMouseButton, 0.0f, FModifierKeysState());
	const FPointerEvent UpEvent(FSlateApplicationBase::CursorPointerIndex, Position, Position, TSet<FKey>(),
		EKeys::LeftMouseButton, 0.0f, FModifierKeysState());

	Slate.ProcessMouseButtonDownEvent(nullptr, DownEvent);
	Slate.ProcessMouseButtonUpEvent(UpEvent);
}

/**
 * A view pixel -> desktop coordinates, through the inverse of the processor's own
 * chain: pixel -> component-local (the GetLocalHitLocation inverse) -> world ->
 * ProjectWorldLocationToScreen (viewport pixels) -> the viewport geometry's
 * LocalToAbsolute (desktop). Both directions share the geometry convention, so a
 * disagreement IS a hit-math bug -- which makes the smoke's click itself an
 * assertion on the math.
 */
static bool ViewPixelToDesktop(UVaCuusWorldComponent* Component, FVector2D ViewPixel, FVector2D& OutDesktop, FVector2D& OutViewport)
{
	UWorld* World = GEngine->GameViewport->GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	const TSharedPtr<SViewport> ViewportWidget = GEngine->GameViewport->GetGameViewportWidget();
	if (!PC || !ViewportWidget.IsValid())
	{
		return false;
	}

	const FIntPoint Size = Component->GetCurrentDrawSize();
	const FVector2D Pivot = Component->Pivot;
	const FVector Local(0.0, -(ViewPixel.X - Size.X * Pivot.X), -(ViewPixel.Y - Size.Y * Pivot.Y));
	const FVector WorldPos = Component->GetComponentTransform().TransformPosition(Local);

	FVector2D ViewportPos;
	if (!PC->ProjectWorldLocationToScreen(WorldPos, ViewportPos))
	{
		return false;
	}

	const FGeometry Geometry = ViewportWidget->GetCachedGeometry();
	OutDesktop = FVector2D(Geometry.LocalToAbsolute(ViewportPos / Geometry.Scale));
	OutViewport = ViewportPos;
	return true;
}

static void SmokeSummary()
{
	if (!GSmoke)
	{
		return;
	}
	const TSharedPtr<FVaCuusWorldInputProcessor> Processor = FVaCuusWorldInputProcessor::Get();
	UE_LOG(LogVaCuus, Display,
		TEXT("InputSmoke processor counters: consumed=%llu deferred-to-Slate=%llu passed-to-game=%llu leaves=%llu"),
		Processor ? Processor->GetNumConsumed() : 0, Processor ? Processor->GetNumDeferredToSlate() : 0,
		Processor ? Processor->GetNumPassedToGame() : 0, Processor ? Processor->GetNumLeavesSent() : 0);
	if (GSmoke->NumFailed == 0)
	{
		UE_LOG(LogVaCuus, Display, TEXT("vacuus.M5World.InputSmoke: all %d assertion(s) passed"), GSmoke->NumPassed);
	}
	else
	{
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.M5World.InputSmoke: %d of %d assertion(s) FAILED"),
			GSmoke->NumFailed, GSmoke->NumFailed + GSmoke->NumPassed);
	}
	GSmoke.Reset();
}

/**
 * The measurement block: the spec 9 risk row's occlusion-query cost and
 * WS-STALE-RAY's re-trace cost, timed where they run in production shape. The
 * numbers are recorded in FVaCuusWorldInputProcessor's header comment.
 */
static void SmokeMeasure(const FVector2D& DesktopPos, const FVector2D& ViewportPos)
{
	FSlateApplication& Slate = FSlateApplication::Get();
	const TSharedPtr<SViewport> ViewportWidget = GEngine->GameViewport->GetGameViewportWidget();
	UWorld* World = GEngine->GameViewport->GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!ViewportWidget.IsValid() || !PC)
	{
		return;
	}

	constexpr int32 NumSamples = 500;

	// (b)'s query: locate + deepest-widget compare.
	double MaxOcclusionUs = 0.0;
	const double OcclusionStart = FPlatformTime::Seconds();
	for (int32 Index = 0; Index < NumSamples; ++Index)
	{
		const double SampleStart = FPlatformTime::Seconds();
		const FWidgetPath Path =
			Slate.LocateWindowUnderMouse(DesktopPos, Slate.GetInteractiveTopLevelWindows(), false, INDEX_NONE);
		const bool bTerminates = Path.IsValid() && &Path.GetLastWidget().Get() == ViewportWidget.Get();
		(void)bTerminates;
		MaxOcclusionUs = FMath::Max(MaxOcclusionUs, (FPlatformTime::Seconds() - SampleStart) * 1e6);
	}
	const double OcclusionAvgUs = (FPlatformTime::Seconds() - OcclusionStart) * 1e6 / NumSamples;

	// WS-STALE-RAY's added cost: the button-event re-trace.
	double MaxTraceUs = 0.0;
	const double TraceStart = FPlatformTime::Seconds();
	for (int32 Index = 0; Index < NumSamples; ++Index)
	{
		const double SampleStart = FPlatformTime::Seconds();
		FHitResult Hit;
		PC->GetHitResultAtScreenPosition(ViewportPos, ECC_Visibility, /*bTraceComplex=*/true, Hit);
		MaxTraceUs = FMath::Max(MaxTraceUs, (FPlatformTime::Seconds() - SampleStart) * 1e6);
	}
	const double TraceAvgUs = (FPlatformTime::Seconds() - TraceStart) * 1e6 / NumSamples;

	UE_LOG(LogVaCuus, Display,
		TEXT("InputSmoke MEASURE: occlusion query avg %.2f us (max %.2f); WS-STALE-RAY re-trace avg %.2f us (max %.2f); %d samples each"),
		OcclusionAvgUs, MaxOcclusionUs, TraceAvgUs, MaxTraceUs, NumSamples);
}

/**
 * The scripted sequence. All steps are scheduled up front at fixed offsets; the
 * gaps cover the asynchronous legs (game -> UI-thread dispatch -> RmlUi -> router
 * queue -> subsystem drain is at most a few frames; the gaps are hundreds).
 */
static void StartInputSmoke(const TArray<FString>& Args)
{
	const float StartDelay = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 0.0f;

	ScheduleAfter(StartDelay, []
	{
		if (!GState || !GState->bInteractiveDemo || !GState->Component.IsValid() ||
			GState->Component->GetView() == nullptr)
		{
			UE_LOG(LogVaCuus, Error, TEXT("vacuus.M5World.InputSmoke: run vacuus.M5World first (and let it spawn)"));
			return;
		}
		if (!FSlateApplication::IsInitialized() || !GEngine || !GEngine->GameViewport)
		{
			UE_LOG(LogVaCuus, Error, TEXT("vacuus.M5World.InputSmoke needs Slate and a game viewport"));
			return;
		}
		if (!FVaCuusWorldInputProcessor::Get().IsValid())
		{
			UE_LOG(LogVaCuus, Error, TEXT("vacuus.M5World.InputSmoke: no world input processor installed"));
			return;
		}

		GSmoke = MakeUnique<FSmoke>();
		UE_LOG(LogVaCuus, Display, TEXT("vacuus.M5World.InputSmoke: starting"));

		// STEP 0 -- input mode, the pass-through ear, the hover probe.
		UWorld* World = GEngine->GameViewport->GetWorld();
		APlayerController* PC = World->GetFirstPlayerController();
		if (PC)
		{
			// Without this, the first pass-through click would put the viewport into
			// PERMANENT Slate capture (game-only mode's default) and rule (a) would
			// rightly defer everything after -- the same constraint every Slate
			// overlay lives under (slate-input.md's closing note: game-and-UI input
			// mode is what makes a shipped game's overlays clickable at all).
			PC->bShowMouseCursor = true;
			FInputModeGameAndUI InputMode;
			InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
			InputMode.SetHideCursorDuringCapture(false);
			PC->SetInputMode(InputMode);

			// THE SPEC'S NAMED PASS-THROUGH RECEIVER (spec 3.4): the game-side click
			// counter. Bound on the player controller's input component -- the same
			// input stack a pawn binding sits in, fed by the identical route (Slate
			// declines -> SViewport -> viewport client -> PlayerInput).
			if (PC->InputComponent)
			{
				FInputKeyBinding Binding(FInputChord(EKeys::LeftMouseButton), IE_Pressed);
				Binding.bConsumeInput = false;
				Binding.KeyDelegate.GetDelegateForManualSet().BindLambda([]()
				{
					if (GSmoke)
					{
						++GSmoke->NumGameClicks;
					}
				});
				PC->InputComponent->KeyBindings.Add(Binding);
			}
		}

		// The hover probe: mouseover/mouseout ARE the DOM-side readback of RmlUi's
		// hover chain (mouseout fires exactly when ProcessMouseLeave clears it), and
		// vacuus.emit carries the observation to the game thread (OnJsEvent).
		GState->Component->GetView()->ExecuteScript(TEXT(
			"(function(){var b=document.getElementById('write-btn');if(!b){return;}"
			"b.addEventListener('mouseover',function(){vacuus.emit('smoke_hover',{on:1});});"
			"b.addEventListener('mouseout',function(){vacuus.emit('smoke_hover',{on:0});});})();"));
	});

	// STEP 1 -- the full-stack click: trace -> UV -> SendInput -> RmlUi hit ->
	// data-event -> write router -> OnModelWrite.
	ScheduleAfter(StartDelay + 1.0f, []
	{
		if (!GSmoke || !GState || !GState->Component.IsValid())
		{
			return;
		}
		UVaCuusWorldComponent* Component = GState->Component.Get();
		UVaCuusView* View = Component->GetView();
		const TSharedPtr<FVaCuusWorldInputProcessor> Processor = FVaCuusWorldInputProcessor::Get();
		if (!View || !Processor.IsValid())
		{
			return;
		}

		// m4_demo.rml's one interactive element is #write-btn (`button` is a known
		// interactive tag, VaCuusInteractiveSnapshot.cpp:109-114; nothing else in
		// the document takes input), so the snapshot names its rect without any
		// document-side probing.
		const FVaCuusInteractiveSnapshot& Snapshot = View->GetSnapshot();
		SmokeCheck(Snapshot.InteractiveRects.Num() == 1,
			TEXT("the published snapshot carries exactly the button's rect"));
		if (Snapshot.InteractiveRects.Num() == 0)
		{
			UE_LOG(LogVaCuus, Error, TEXT("InputSmoke: no interactive rects; is the document published yet? Aborting"));
			SmokeSummary();
			return;
		}

		const FIntRect Rect = Snapshot.InteractiveRects[0];
		const FVector2D Center((Rect.Min.X + Rect.Max.X) * 0.5, (Rect.Min.Y + Rect.Max.Y) * 0.5);
		FVector2D Desktop, Viewport;
		if (!ViewPixelToDesktop(Component, Center, Desktop, Viewport))
		{
			UE_LOG(LogVaCuus, Error, TEXT("InputSmoke: could not project the button; aborting"));
			SmokeSummary();
			return;
		}
		GSmoke->ButtonDesktopPos = Desktop;
		GSmoke->ButtonViewportPos = Viewport;
		UE_LOG(LogVaCuus, Display, TEXT("InputSmoke: button rect (%d,%d)-(%d,%d), desktop (%.0f, %.0f)"),
			Rect.Min.X, Rect.Min.Y, Rect.Max.X, Rect.Max.Y, Desktop.X, Desktop.Y);

		GSmoke->BaseWrites = GState->NumAcceptedWrites;
		GSmoke->BaseConsumed = Processor->GetNumConsumed();
		GSmoke->BaseGameClicks = GSmoke->NumGameClicks;

		SmokeMove(Desktop);
		SmokeClick(Desktop);
	});

	// STEP 2 -- assert the write landed; raise the occlusion overlay.
	ScheduleAfter(StartDelay + 2.0f, []
	{
		if (!GSmoke || !GState)
		{
			return;
		}
		const TSharedPtr<FVaCuusWorldInputProcessor> Processor = FVaCuusWorldInputProcessor::Get();
		if (!Processor.IsValid())
		{
			return;
		}

		SmokeCheck(GState->NumAcceptedWrites == GSmoke->BaseWrites + 1,
			TEXT("the raycast click fired OnModelWrite exactly once (the full stack)"));
		SmokeCheck(Processor->GetNumConsumed() > GSmoke->BaseConsumed,
			TEXT("the processor consumed the panel click"));
		SmokeCheck(GSmoke->NumGameClicks == GSmoke->BaseGameClicks,
			TEXT("the consumed click never reached the game's input"));
		SmokeCheck(GState->NumHoverOn > 0, TEXT("moving onto the button raised mouseover (hover armed)"));

		// The occlusion overlay: a viewport-filling Slate button ABOVE the game
		// (AddViewportWidgetContent's overlay sits over the SViewport in the same
		// window), so the widget path at the button's position now terminates at IT,
		// not at the game viewport.
		GSmoke->BaseWrites = GState->NumAcceptedWrites;
		GSmoke->BaseConsumed = Processor->GetNumConsumed();
		GSmoke->BaseDeferred = Processor->GetNumDeferredToSlate();
		GSmoke->Overlay = SNew(SButton).OnClicked_Lambda([]()
		{
			if (GSmoke)
			{
				++GSmoke->NumOverlayClicks;
			}
			return FReply::Handled();
		});
		GEngine->GameViewport->AddViewportWidgetContent(GSmoke->Overlay.ToSharedRef(), /*ZOrder=*/1000);
	});

	// STEP 3 -- click the same position, now occluded.
	ScheduleAfter(StartDelay + 2.6f, []
	{
		if (!GSmoke)
		{
			return;
		}
		SmokeMove(GSmoke->ButtonDesktopPos);
		SmokeClick(GSmoke->ButtonDesktopPos);
	});

	// STEP 4 -- assert the overlay won; drop it.
	ScheduleAfter(StartDelay + 3.4f, []
	{
		if (!GSmoke || !GState)
		{
			return;
		}
		const TSharedPtr<FVaCuusWorldInputProcessor> Processor = FVaCuusWorldInputProcessor::Get();
		if (!Processor.IsValid())
		{
			return;
		}

		SmokeCheck(GSmoke->NumOverlayClicks >= 1,
			TEXT("occlusion: the overlaying Slate button received the click"));
		SmokeCheck(Processor->GetNumDeferredToSlate() > GSmoke->BaseDeferred,
			TEXT("occlusion: the processor deferred (path no longer terminates at the game viewport)"));
		SmokeCheck(Processor->GetNumConsumed() == GSmoke->BaseConsumed,
			TEXT("occlusion: the processor consumed nothing while occluded"));
		SmokeCheck(GState->NumAcceptedWrites == GSmoke->BaseWrites,
			TEXT("occlusion: no write reached the router while occluded"));

		CleanupSmokeOverlay();
	});

	// STEP 5 -- the pass-through click: on the quad, missing every interactive rect.
	ScheduleAfter(StartDelay + 4.0f, []
	{
		if (!GSmoke || !GState || !GState->Component.IsValid())
		{
			return;
		}
		UVaCuusWorldComponent* Component = GState->Component.Get();
		UVaCuusView* View = Component->GetView();
		const TSharedPtr<FVaCuusWorldInputProcessor> Processor = FVaCuusWorldInputProcessor::Get();
		if (!View || !Processor.IsValid())
		{
			return;
		}

		// A pixel on the quad but outside every rect; the corners are panel chrome
		// in m4_demo (title text, no listeners). Verified against the live snapshot
		// rather than assumed.
		const FIntPoint Size = Component->GetCurrentDrawSize();
		const FVaCuusInteractiveSnapshot& Snapshot = View->GetSnapshot();
		const FIntPoint Candidates[] = {
			FIntPoint(8, 8), FIntPoint(Size.X - 8, 8), FIntPoint(Size.X / 2, 8), FIntPoint(Size.X - 8, Size.Y - 8)};
		const FIntPoint* Miss = nullptr;
		for (const FIntPoint& Candidate : Candidates)
		{
			if (!Snapshot.Contains(Candidate))
			{
				Miss = &Candidate;
				break;
			}
		}
		if (Miss == nullptr)
		{
			SmokeCheck(false, TEXT("pass-through: found a non-interactive pixel on the quad"));
			return;
		}

		FVector2D Desktop, Viewport;
		if (!ViewPixelToDesktop(Component, FVector2D(Miss->X, Miss->Y), Desktop, Viewport))
		{
			SmokeCheck(false, TEXT("pass-through: projected the miss pixel"));
			return;
		}

		GSmoke->BaseGameClicks = GSmoke->NumGameClicks;
		GSmoke->BaseWrites = GState->NumAcceptedWrites;
		GSmoke->BasePassed = Processor->GetNumPassedToGame();
		GSmoke->BaseConsumed = Processor->GetNumConsumed();

		SmokeMove(Desktop);
		SmokeClick(Desktop);
	});

	// STEP 6 -- assert the game heard it; start the hover/leave phase.
	ScheduleAfter(StartDelay + 4.8f, []
	{
		if (!GSmoke || !GState)
		{
			return;
		}
		const TSharedPtr<FVaCuusWorldInputProcessor> Processor = FVaCuusWorldInputProcessor::Get();
		if (!Processor.IsValid())
		{
			return;
		}

		SmokeCheck(GSmoke->NumGameClicks == GSmoke->BaseGameClicks + 1,
			TEXT("pass-through: the game's input heard exactly one press (the named receiver)"));
		SmokeCheck(Processor->GetNumPassedToGame() > GSmoke->BasePassed,
			TEXT("pass-through: the processor traced, forwarded and declined (absence of coverage)"));
		SmokeCheck(GState->NumAcceptedWrites == GSmoke->BaseWrites,
			TEXT("pass-through: no write on a miss"));

		// Hover phase: re-enter the button...
		GSmoke->BaseHoverOn = GState->NumHoverOn;
		GSmoke->BaseHoverOff = GState->NumHoverOff;
		GSmoke->BaseLeaves = Processor->GetNumLeavesSent();
		SmokeMove(GSmoke->ButtonDesktopPos);
	});

	// STEP 7 -- ...then move the ray OFF the quad entirely.
	ScheduleAfter(StartDelay + 5.4f, []
	{
		if (!GSmoke || !GState || !GState->Component.IsValid())
		{
			return;
		}
		SmokeCheck(GState->NumHoverOn > GSmoke->BaseHoverOn, TEXT("hover: mouseover fired on re-entry"));

		// A desktop point whose ray misses the panel: probe the trace at the
		// viewport corners until one misses (deterministic against the live scene,
		// not assumed from layout).
		UWorld* World = GEngine->GameViewport->GetWorld();
		APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		const TSharedPtr<SViewport> ViewportWidget = GEngine->GameViewport->GetGameViewportWidget();
		if (!PC || !ViewportWidget.IsValid())
		{
			return;
		}
		FVector2D ViewportSize;
		GEngine->GameViewport->GetViewportSize(ViewportSize);
		const FGeometry Geometry = ViewportWidget->GetCachedGeometry();

		const FVector2D Fractions[] = {
			FVector2D(0.04, 0.04), FVector2D(0.96, 0.04), FVector2D(0.04, 0.96), FVector2D(0.96, 0.96)};
		bool bMoved = false;
		for (const FVector2D& Fraction : Fractions)
		{
			const FVector2D ViewportPos = ViewportSize * Fraction;
			FHitResult Hit;
			const bool bHitSomething = PC->GetHitResultAtScreenPosition(ViewportPos, ECC_Visibility, true, Hit);
			if (!bHitSomething || Hit.Component.Get() != GState->Component.Get())
			{
				SmokeMove(FVector2D(Geometry.LocalToAbsolute(ViewportPos / Geometry.Scale)));
				bMoved = true;
				break;
			}
		}
		SmokeCheck(bMoved, TEXT("hover: found a screen point whose ray misses the quad"));
	});

	// STEP 8 -- assert MouseLeave un-hovered; measure; summarize.
	ScheduleAfter(StartDelay + 6.2f, []
	{
		if (!GSmoke || !GState)
		{
			return;
		}
		const TSharedPtr<FVaCuusWorldInputProcessor> Processor = FVaCuusWorldInputProcessor::Get();
		if (Processor.IsValid())
		{
			SmokeCheck(Processor->GetNumLeavesSent() > GSmoke->BaseLeaves,
				TEXT("MouseLeave: the processor sent it when the ray left the panel"));
		}
		SmokeCheck(GState->NumHoverOff > GSmoke->BaseHoverOff,
			TEXT("MouseLeave: mouseout fired (RmlUi's hover chain cleared -- :hover does not stick)"));

		SmokeMeasure(GSmoke->ButtonDesktopPos, GSmoke->ButtonViewportPos);
		SmokeSummary();
	});
}

// ---------------------------------------------------------------------------
// The Task 6 sub-commands (unchanged)
// ---------------------------------------------------------------------------

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

static void SetMips(const TArray<FString>& Args)
{
	if (Args.Num() < 1)
	{
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.WorldDemo.Mips expects <0|1> [delaySeconds]"));
		return;
	}
	const bool bEnabled = FCString::Atoi(*Args[0]) != 0;
	ScheduleAfter(Args.Num() > 1 ? FCString::Atof(*Args[1]) : 0.0f,
		[bEnabled]
		{
			UVaCuusWorldComponent* Component = GState ? GState->Component.Get() : nullptr;
			if (!Component)
			{
				UE_LOG(LogVaCuus, Error, TEXT("vacuus.WorldDemo.Mips: no live world demo panel"));
				return;
			}
			Component->SetGenerateMips(bEnabled);
			UE_LOG(LogVaCuus, Log, TEXT("World demo: bGenerateMips = %d (RT now %d mip(s))"), bEnabled ? 1 : 0,
				Component->GetRenderTarget() ? Component->GetRenderTarget()->GetNumMips() : 0);
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
			UE_LOG(LogVaCuus, Display,
				TEXT("World demo sink: %llu buffer(s) arrived, %llu copie(s), %llu extent skip(s), %llu mip generation(s)"),
				Sink->GetNumArrivals(), Sink->GetNumCopies(), Sink->GetNumExtentSkips(), Sink->GetNumMipGenerations());
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

static FAutoConsoleCommand GInteractiveCommand(
	TEXT("vacuus.M5World"),
	TEXT("Toggle the M5 Task 7 world panel: m4_demo.rml on a quad before the camera with JS + data binding live ")
	TEXT("(model driver + OnModelWrite ear armed). [delaySeconds] [sizeX] [sizeY] [scale]; defaults 0 1024 1024 0.12. ")
	TEXT("Click it by raycast, or run vacuus.M5World.InputSmoke."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&ToggleInteractive));

static FAutoConsoleCommand GInputSmokeCommand(
	TEXT("vacuus.M5World.InputSmoke"),
	TEXT("The Task 7 functional input test against the live vacuus.M5World panel: click->OnModelWrite, occlusion ")
	TEXT("(a Slate overlay wins), pass-through (the game click counter), MouseLeave un-hovers, and the ")
	TEXT("occlusion-query/re-trace cost measurements. [startDelaySeconds]. PASS/FAIL lines land in the log."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&StartInputSmoke));

static FAutoConsoleCommand GDecodeCommand(
	TEXT("vacuus.WorldDemo.Decode"),
	TEXT("Set the live panel MID's VaCuusDecodeSRGB scalar (<0|1> [delaySeconds]) — the WS-GAMMA A/B knob."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&SetDecode));

static FAutoConsoleCommand GMipsCommand(
	TEXT("vacuus.WorldDemo.Mips"),
	TEXT("Set the live panel's bGenerateMips (<0|1> [delaySeconds]) — the minification A/B knob: 0 restores the ")
	TEXT("single-mip strobe, 1 restores the chain."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&SetMips));

static FAutoConsoleCommand GShotCommand(
	TEXT("vacuus.WorldDemo.Shot"),
	TEXT("Take a screenshot after [delaySeconds] — the WS-GAMMA beats."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&Shot));

static FAutoConsoleCommand GStatsCommand(
	TEXT("vacuus.WorldDemo.Stats"),
	TEXT("Print the live panel's sink counters: buffers arrived, copies issued, extent skips (WS-COPY-COST)."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&Stats));
}	 // namespace VaCuusWorldDemo
