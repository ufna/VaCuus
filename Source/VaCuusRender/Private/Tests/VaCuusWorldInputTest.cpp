// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusSubsystem.h"
#include "VaCuusUIThread.h"
#include "VaCuusView.h"
#include "VaCuusWorldComponent.h"
#include "VaCuusWorldInputProcessor.h"
#include "VaCuusWorldSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformProcess.h"
#include "InputCoreTypes.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusWorldInputTest
{
/** The world-component test scaffold (VaCuusWorldComponentTest.cpp:64-91's argument): a real EWorldType::Game world so both subsystems are the production ones. */
struct FWorldFixture
{
	TStrongObjectPtr<UGameInstance> GameInstance;

	UWorld* GetWorld() const { return GameInstance ? GameInstance->GetWorld() : nullptr; }

	bool Init(FAutomationTestBase& Test)
	{
		GameInstance = TStrongObjectPtr<UGameInstance>(NewObject<UGameInstance>(GEngine));
		GameInstance->InitializeStandalone();
		return Test.TestNotNull(TEXT("The standalone game instance has a world"), GetWorld());
	}

	~FWorldFixture()
	{
		if (GameInstance)
		{
			UWorld* World = GameInstance->GetWorld();
			GameInstance->Shutdown();
			if (World)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(/*bInformEngineOfWorld=*/false);
			}
		}
	}
};

static bool SuiteViable(FAutomationTestBase& Test)
{
	if (!FPlatformProcess::SupportsMultithreading())
	{
		Test.AddInfo(TEXT("Skipped: no multithreading support, so there is no worker thread to drive"));
		return false;
	}
	if (!FSlateApplication::IsInitialized())
	{
		Test.AddInfo(TEXT("Skipped: no FSlateApplication, so a game instance cannot be initialized"));
		return false;
	}
	return GEngine != nullptr;
}

/** A pointer event at a desktop position; the routing-test constructor shape (VaCuusSlateRoutingTest.cpp:353-356). */
static FPointerEvent MakePointerEvent(const FVector2D& Position, bool bLeftDown)
{
	const TSet<FKey> Pressed = bLeftDown ? TSet<FKey>{EKeys::LeftMouseButton} : TSet<FKey>();
	return FPointerEvent(FSlateApplicationBase::CursorPointerIndex, Position, Position, Pressed,
		bLeftDown ? EKeys::LeftMouseButton : FKey(), /*WheelDelta=*/0.0f, FModifierKeysState());
}
}	 // namespace VaCuusWorldInputTest

/**
 * The hit math against hand-computed transforms (plan Task 7.3's unit half). Every
 * expected value below is derived on paper from the quad convention the proxy
 * builds (DrawSize world units, +X normal, pivot-offset -- VaCuusWorldComponent
 * .cpp:80-105) and the engine math being ported (GetLocalHitLocation,
 * WidgetComponent.cpp:2036-2054), NOT by running the function under test -- a
 * table computed by the implementation would prove only that it equals itself.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusWorldInputHitMathTest, "VaCuus.World.InputHitMath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusWorldInputHitMathTest::RunTest(const FString& Parameters)
{
	const FIntPoint DrawSize(200, 100);
	const FVector2D CenterPivot(0.5, 0.5);
	constexpr double Tolerance = 1e-4;

	auto TestWidget = [this, Tolerance](const TCHAR* What, FVector2D Actual, FVector2D Expected)
	{
		TestTrue(What, FMath::IsNearlyEqual(Actual.X, Expected.X, Tolerance) &&
			FMath::IsNearlyEqual(Actual.Y, Expected.Y, Tolerance));
		if (!FMath::IsNearlyEqual(Actual.X, Expected.X, Tolerance) ||
			!FMath::IsNearlyEqual(Actual.Y, Expected.Y, Tolerance))
		{
			AddInfo(FString::Printf(TEXT("  got (%f, %f), expected (%f, %f)"), Actual.X, Actual.Y, Expected.X, Expected.Y));
		}
	};

	// A. Identity, centered pivot: local (0,-60,-20) -> (-(-60)+100, -(-20)+50).
	TestWidget(TEXT("A: identity transform, centered pivot"),
		FVaCuusWorldHitMath::LocalHitToWidget(FTransform::Identity, FVector(0, -60, -20), DrawSize, CenterPivot),
		FVector2D(160, 70));

	// B. Pivot (0,0): the quad spans local Y in [-W, 0], Z in [-H, 0]; no offset.
	TestWidget(TEXT("B: zero pivot"),
		FVaCuusWorldHitMath::LocalHitToWidget(FTransform::Identity, FVector(0, -60, -20), DrawSize, FVector2D(0, 0)),
		FVector2D(60, 20));

	// C. Pure translation: the same local point moved to (100,-10,5).
	TestWidget(TEXT("C: translated component"),
		FVaCuusWorldHitMath::LocalHitToWidget(
			FTransform(FVector(100, 50, 25)), FVector(100, -10, 5), DrawSize, CenterPivot),
		FVector2D(160, 70));

	// D. Yaw 90: rotating local (0,-60,-20) about Z gives world (60, 0, -20)
	//    (X' = -Y*sin90 = 60, Y' = Y*cos90 = 0), so the hit at that world point maps
	//    back to the same widget position.
	TestWidget(TEXT("D: yawed component"),
		FVaCuusWorldHitMath::LocalHitToWidget(
			FTransform(FRotator(0, 90, 0), FVector::ZeroVector), FVector(60, 0, -20), DrawSize, CenterPivot),
		FVector2D(160, 70));

	// E. Uniform scale 0.15 + translation: world = T + local * 0.15 = (100, 41, 22).
	//    Pixels are SCALE-INVARIANT -- the quad scales with the component, so the
	//    inverse transform divides the scale back out (the processor comment's claim,
	//    asserted).
	TestWidget(TEXT("E: scaled component keeps pixel coordinates"),
		FVaCuusWorldHitMath::LocalHitToWidget(
			FTransform(FRotator::ZeroRotator, FVector(100, 50, 25), FVector(0.15)), FVector(100, 41, 22),
			DrawSize, CenterPivot),
		FVector2D(160, 70));

	// F..J: the latched-drag plane projection.
	FVector2D Widget;

	// F. Head-on ray through the same point.
	TestTrue(TEXT("F: head-on ray intersects"),
		FVaCuusWorldHitMath::RayToWidget(
			FTransform::Identity, FVector(500, -60, -20), FVector(-1, 0, 0), DrawSize, CenterPivot, Widget));
	TestWidget(TEXT("F: head-on ray position"), Widget, FVector2D(160, 70));

	// G. Oblique ray: origin (100,-160,-120) toward (0,-60,-20) is direction
	//    (-100,100,100); t = 1 lands exactly on the plane point. Unnormalized on
	//    purpose -- the function must not require a unit direction.
	TestTrue(TEXT("G: oblique unnormalized ray intersects"),
		FVaCuusWorldHitMath::RayToWidget(
			FTransform::Identity, FVector(100, -160, -120), FVector(-100, 100, 100), DrawSize, CenterPivot, Widget));
	TestWidget(TEXT("G: oblique ray position"), Widget, FVector2D(160, 70));

	// H. Parallel ray: no intersection to invent.
	TestFalse(TEXT("H: parallel ray refuses"),
		FVaCuusWorldHitMath::RayToWidget(
			FTransform::Identity, FVector(500, 0, 0), FVector(0, 1, 0), DrawSize, CenterPivot, Widget));

	// I. Plane behind the ray (camera turned past the panel): t < 0 refuses.
	TestFalse(TEXT("I: plane behind the ray refuses"),
		FVaCuusWorldHitMath::RayToWidget(
			FTransform::Identity, FVector(-10, 0, 0), FVector(-1, 0, 0), DrawSize, CenterPivot, Widget));

	// J. Scale 2 + translation (10,0,0): world origin (510,-120,-40) -> local
	//    (250,-60,-20); direction (-1,0,0) -> local (-0.5,0,0); t=500 lands on
	//    local (0,-60,-20) -> (160,70). Scale flows through origin AND direction.
	TestTrue(TEXT("J: scaled ray intersects"),
		FVaCuusWorldHitMath::RayToWidget(
			FTransform(FRotator::ZeroRotator, FVector(10, 0, 0), FVector(2.0)), FVector(510, -120, -40),
			FVector(-1, 0, 0), DrawSize, CenterPivot, Widget));
	TestWidget(TEXT("J: scaled ray position"), Widget, FVector2D(160, 70));

	// The front-face gate (WidgetComponent.cpp:186-189): from the front (trace
	// running against the +X normal) the dot is negative.
	TestTrue(TEXT("Front face: trace from the +X side interacts"),
		FVaCuusWorldHitMath::IsFrontFacing(FVector(1, 0, 0), FVector::ZeroVector, FVector(500, 0, 0)));
	TestFalse(TEXT("Back face: trace from the -X side is gated"),
		FVaCuusWorldHitMath::IsFrontFacing(FVector(1, 0, 0), FVector::ZeroVector, FVector(-500, 0, 0)));

	// Floor, not round: a pixel spans [n, n+1).
	TestEqual(TEXT("WidgetToPixel floors"), FVaCuusWorldHitMath::WidgetToPixel(FVector2D(160.7, 70.2)), FIntPoint(160, 70));
	TestEqual(TEXT("WidgetToPixel floors negatives"), FVaCuusWorldHitMath::WidgetToPixel(FVector2D(-0.5, -0.5)), FIntPoint(-1, -1));

	return true;
}

/**
 * The occlusion rule's decision table (spec 2(h)) -- the pure core, plus the two
 * defer rows a live editor Slate can honestly produce: with no game viewport
 * registered, every event must be deferred untouched and counted as such. The
 * engaged row cannot exist here (an editor automation session has no game
 * viewport) and is covered by vacuus.M5World.InputSmoke's occlusion/consume
 * assertions in the -game run.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusWorldInputOcclusionTableTest, "VaCuus.World.InputOcclusionTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusWorldInputOcclusionTableTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusWorldInputTest;

	// The table. Order matters in one place: a captor defers EVEN WHEN the path
	// terminates at the viewport -- the captor owns the stream (spec 2(h)(a)).
	TestFalse(TEXT("no captor, path elsewhere -> defer"), FVaCuusWorldInputProcessor::ShouldEngage(false, false));
	TestFalse(TEXT("captor, path elsewhere -> defer"), FVaCuusWorldInputProcessor::ShouldEngage(true, false));
	TestFalse(TEXT("captor, path at viewport -> DEFER (capture wins)"), FVaCuusWorldInputProcessor::ShouldEngage(true, true));
	TestTrue(TEXT("no captor, path at viewport -> engage"), FVaCuusWorldInputProcessor::ShouldEngage(false, true));

	// The live defer rows, against the real FSlateApplication.
	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Skipped live rows: no FSlateApplication"));
		return true;
	}
	if (GEngine == nullptr || GEngine->GameViewport != nullptr)
	{
		AddInfo(TEXT("Skipped live rows: a game viewport exists (interactive editor?); the -game smoke covers engaged rows"));
		return true;
	}

	const TSharedRef<FVaCuusWorldInputProcessor> Processor = MakeShared<FVaCuusWorldInputProcessor>();
	FSlateApplication& Slate = FSlateApplication::Get();

	const FPointerEvent Move = MakePointerEvent(FVector2D(3, 3), /*bLeftDown=*/false);
	const FPointerEvent Down = MakePointerEvent(FVector2D(3, 3), /*bLeftDown=*/true);

	TestFalse(TEXT("A move with no game viewport is deferred"), Processor->HandleMouseMoveEvent(Slate, Move));
	TestFalse(TEXT("A press with no game viewport is deferred"), Processor->HandleMouseButtonDownEvent(Slate, Down));
	TestEqual(TEXT("Both were counted as deferred-to-Slate"), static_cast<int64>(Processor->GetNumDeferredToSlate()), static_cast<int64>(2));
	TestEqual(TEXT("Nothing was consumed"), static_cast<int64>(Processor->GetNumConsumed()), static_cast<int64>(0));
	TestEqual(TEXT("Nothing engaged a trace"), static_cast<int64>(Processor->GetNumPassedToGame()), static_cast<int64>(0));

	return true;
}

/**
 * The refcounted install (plan Task 7.1): the first REGISTERED panel installs the
 * process-wide processor PreGame, further panels share it, the last one out
 * uninstalls -- and the refusal path never touches the refcount (a refused panel
 * releasing an un-taken ref would uninstall a processor other panels still need).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusWorldInputInstallTest, "VaCuus.World.InputProcessorInstall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusWorldInputInstallTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusWorldInputTest;
	if (!SuiteViable(*this))
	{
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

	if (!TestFalse(TEXT("No processor is installed before any panel exists"),
			FVaCuusWorldInputProcessor::Get().IsValid()))
	{
		return false;
	}

	FWorldFixture Fixture;
	if (!Fixture.Init(*this))
	{
		return false;
	}
	UWorld* World = Fixture.GetWorld();
	AActor* Actor = World->SpawnActor<AActor>();

	// The refusal path takes no ref.
	{
		UVaCuusWorldComponent* Refused = NewObject<UVaCuusWorldComponent>(Actor);
		Refused->DrawSize = FIntPoint(0, 0);
		Refused->bAutoLoadDocument = false;
		AddExpectedError(TEXT("refused: degenerate DrawSize"), EAutomationExpectedErrorFlags::Contains, 1);
		Refused->RegisterComponentWithWorld(World);
		TestFalse(TEXT("A refused panel installs nothing"), FVaCuusWorldInputProcessor::Get().IsValid());
		Refused->UnregisterComponent();
		Refused->DestroyComponent();
		TestFalse(TEXT("...and its unregister releases nothing"), FVaCuusWorldInputProcessor::Get().IsValid());
	}

	// First panel installs, PreGame.
	UVaCuusWorldComponent* First = NewObject<UVaCuusWorldComponent>(Actor);
	First->DrawSize = FIntPoint(64, 64);
	First->bAutoLoadDocument = false;
	First->RegisterComponentWithWorld(World);
	if (!TestNotNull(TEXT("The first panel has a view"), First->GetView()))
	{
		return false;
	}

	TSharedPtr<FVaCuusWorldInputProcessor> Installed = FVaCuusWorldInputProcessor::Get();
	if (!TestTrue(TEXT("The first panel installed the processor"), Installed.IsValid()))
	{
		return false;
	}
	TestNotEqual(TEXT("...registered with Slate in the PreGame bucket"),
		FSlateApplication::Get().FindInputPreProcessor(Installed, EInputPreProcessorType::PreGame), static_cast<int32>(INDEX_NONE));

	// Second panel shares the instance.
	UVaCuusWorldComponent* Second = NewObject<UVaCuusWorldComponent>(Actor);
	Second->DrawSize = FIntPoint(64, 64);
	Second->bAutoLoadDocument = false;
	Second->RegisterComponentWithWorld(World);
	TestEqual(TEXT("The second panel shares the same processor"), FVaCuusWorldInputProcessor::Get().Get(), Installed.Get());

	// Removing one keeps it; removing the last uninstalls.
	First->UnregisterComponent();
	TestTrue(TEXT("One panel remaining keeps the processor installed"), FVaCuusWorldInputProcessor::Get().IsValid());

	Second->UnregisterComponent();
	TestFalse(TEXT("The last panel out uninstalls the processor"), FVaCuusWorldInputProcessor::Get().IsValid());
	TestEqual(TEXT("...and Slate no longer lists it"),
		FSlateApplication::Get().FindInputPreProcessor(Installed, EInputPreProcessorType::PreGame), static_cast<int32>(INDEX_NONE));

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
