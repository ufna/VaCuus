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

/**
 * A MOUSE event naming one button, with the pressed-button set given explicitly so a
 * test can build the two-button drag the latch has to survive: FSlateApplication
 * supplies the POST-press / POST-release live set here (OnMouseDown adds and OnMouseUp
 * removes before constructing the value copy, SlateApplication.cpp:5269-5275,
 * :6100-6107), so an up naming Left while Right is still held carries {Right}.
 */
static FPointerEvent MakeMouseButtonEvent(const FVector2D& Position, FKey EffectingButton, const TSet<FKey>& PressedAfter)
{
	return FPointerEvent(FSlateApplicationBase::CursorPointerIndex, Position, Position, PressedAfter, EffectingButton,
		/*WheelDelta=*/0.0f, FModifierKeysState());
}

/**
 * A TOUCH event -- the shape this suite could not express before bead VaCuus-61d.
 * Every other synthesized FPointerEvent under Tests/ uses CursorPointerIndex, i.e. is
 * a mouse, so nothing ever exercised the touch field layout at all.
 *
 * Built through a TOUCH constructor, so the fields that decide behaviour are not
 * approximations:
 *   bIsTouchEvent   true
 *   PointerIndex    the finger -- mouse events always carry CursorPointerIndex instead
 *   EffectingButton LeftMouseButton, for every finger (Events.h:909)
 *   PressedButtons  FTouchKeySet::StandardSet, i.e. {LeftMouseButton} (Events.h:908;
 *                   Events.cpp:15) -- a CONSTANT, byte-identical on press and release
 *
 * WHICH touch constructor, since there are two live ones: the user-index overload
 * (Events.h:893-920), not the FInputDeviceId overload FSlateApplication itself calls
 * (OnTouchStarted, SlateApplication.cpp:6781-6787; OnTouchEnded, :6837-6843). They
 * differ in one thing only -- how UserIndex is derived -- and every field above is
 * initialised by the identical expression in both (compare Events.h:908-913 with
 * :938-943). The device-id form additionally needs IPlatformInputDeviceMapper from
 * ApplicationCore, which VaCuusRender does not link, and a device-mapper default is
 * not something a headless run should have to have populated for a latch test.
 *
 * bPressLeftMouseButton is passed true for the RELEASE as well as the press, because
 * that is what OnTouchEnded does (SlateApplication.cpp:6843). It is the whole reason
 * a touch release cannot be recognised by its button set, so a fixture that quietly
 * passed false here would test a device that does not exist.
 */
static FPointerEvent MakeTouchEvent(const FVector2D& Position, int32 FingerIndex, int32 SlateUserIndex = 0)
{
	return FPointerEvent(static_cast<uint32>(SlateUserIndex), static_cast<uint32>(FingerIndex), Position, Position,
		/*InForce=*/1.0f, /*bPressLeftMouseButton=*/true, /*bInIsForceChanged=*/false, /*bInIsFirstMove=*/false,
		FModifierKeysState());
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
 * THE LATCH RELEASE RULE (bead VaCuus-61d), against real touch events.
 *
 * THE BUG THIS EXISTS FOR: the release condition was
 * MouseEvent.GetPressedButtons().IsEmpty(). That can never be true for a touch,
 * because OnTouchEnded constructs its event with bPressLeftMouseButton = true
 * (SlateApplication.cpp:6837-6843) and the touch constructor bakes
 * PressedButtons = FTouchKeySet::StandardSet = {LeftMouseButton} into the value copy
 * (Events.h:938; Events.cpp:15). So the first tap latched forever and the processor
 * -- which sits ahead of ALL Slate routing -- consumed every pointer event in the
 * application from then on. Nothing caught it: no Slate capture is held, so the
 * engine's touch net (FSlateUser::NotifyPointerReleased, SlateUser.cpp:1284-1290) has
 * nothing of ours to release, and the whole of IInputProcessor (IInputProcessor.h
 * :20-53) has no touch hook and no capture-lost hook.
 *
 * WHAT IT ASSERTS BEYOND "IT RELEASES NOW". A rule that simply released on every up
 * would pass a one-row test and break two real gestures, so both are rows here: a
 * two-button mouse drag must survive the first button's release (C5), and a
 * two-finger gesture must survive the first finger's (C2). A foreign pointer's
 * release must not end our gesture at all (C3, D2). And the whole point is the state
 * AFTER release, so D drives a further event through the real handlers and requires
 * it to be handed back to Slate -- a release that cleared a flag but left the
 * processor consuming would still be the reported bug.
 *
 * RESTORE-THE-BUG, all three run and recorded. Three different wrong release
 * conditions, caught by three different sets of rows -- which is the evidence that
 * this test constrains the RULE and not just the one line that was wrong:
 *   1. the reported bug (the up handler's condition back to
 *      `GetPressedButtons().IsEmpty()`) fails, and ONLY fails,
 *      "D: THE BEAD -- and the latch is no longer held",
 *      "D: THE SYMPTOM -- the NEXT event is handed back to Slate, not consumed" and
 *      "D: ...and is counted as deferred" (expected 1, was 0);
 *   2. the naive over-eager fix (the up handler calling Latch.Drop() unconditionally)
 *      passes all of D and fails "D2: ...and the gesture is still held" and
 *      "D2: ...so the next event is still ours";
 *   3. the same over-eagerness moved into the rule itself (NoteRelease resetting the
 *      whole set instead of removing one press) fails "C2: lifting one finger keeps
 *      the gesture", "C3: a FOREIGN finger's release does not end the gesture",
 *      "C5: releasing Left with Right still held keeps the drag", and both D2 rows.
 * With the real rule in, all rows pass.
 *
 * ALSO THE NO-REGRESS CHECK FOR THE TOUCH FORWARDING (bead VaCuus-ujm), which changed the
 * handlers D drives: HandleMouseMoveEvent and HandleMouseButtonUpEvent now translate a touch
 * into EVaCuusInputEventKind::TouchMove/TouchEnd rather than into a mouse move and release
 * (VaCuusWorldInput::MakeForwardedMove / MakeForwardedPress). Every row of D goes through that
 * branch, because every event D synthesizes is a real touch. What D still cannot see is WHICH
 * kind came out: ResolveLatchedPixel declines without a game viewport (see SeedLatchForTest),
 * so nothing is enqueued to assert on, and an editor automation session has no viewport. The
 * kind itself is proved on the Slate side instead, end to end into RmlUi, by
 * VaCuus.Input.TouchRouting.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusWorldInputLatchReleaseTest, "VaCuus.World.InputLatchRelease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusWorldInputLatchReleaseTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusWorldInputTest;

	const FVector2D At(120, 80);
	const TSet<FKey> NoButtons;
	const TSet<FKey> RightOnly = {EKeys::RightMouseButton};

	// -- A. The fixture says what it claims to say. If these fail, every row below is
	//       testing some other device. --
	const FPointerEvent TouchDown0 = MakeTouchEvent(At, /*FingerIndex=*/0);
	const FPointerEvent TouchUp0 = MakeTouchEvent(At, /*FingerIndex=*/0);
	const FPointerEvent TouchUp1 = MakeTouchEvent(At, /*FingerIndex=*/1);

	TestTrue(TEXT("A: the fixture builds a touch event"), TouchDown0.IsTouchEvent());
	TestEqual(TEXT("A: ...on the finger it was asked for"), static_cast<int32>(TouchDown0.GetPointerIndex()), 0);
	TestTrue(TEXT("A: ...which is not the mouse's pointer index"),
		TouchDown0.GetPointerIndex() != FSlateApplicationBase::CursorPointerIndex);
	TestTrue(TEXT("A: ...naming LeftMouseButton, as touch always does"),
		TouchDown0.GetEffectingButton() == EKeys::LeftMouseButton);

	// THE MECHANISM, asserted rather than described: the release's button set is
	// non-empty and identical to the press's, so no predicate over it can tell them
	// apart. This row is what the old code got wrong.
	TestFalse(TEXT("A: a touch RELEASE's pressed-button set is NOT empty (the bug)"),
		TouchUp0.GetPressedButtons().IsEmpty());
	TestEqual(TEXT("A: ...and is the same size as the press's"),
		TouchUp0.GetPressedButtons().Num(), TouchDown0.GetPressedButtons().Num());

	// -- B. Press identity: what the latch keys on. --
	TestTrue(TEXT("B: a touch press and its release name the SAME press"),
		FVaCuusPointerPress::FromEvent(TouchDown0) == FVaCuusPointerPress::FromEvent(TouchUp0));
	TestFalse(TEXT("B: two fingers name different presses"),
		FVaCuusPointerPress::FromEvent(TouchDown0) == FVaCuusPointerPress::FromEvent(TouchUp1));
	TestFalse(TEXT("B: two users' same finger name different presses"),
		FVaCuusPointerPress::FromEvent(TouchDown0) ==
			FVaCuusPointerPress::FromEvent(MakeTouchEvent(At, /*FingerIndex=*/0, /*SlateUserIndex=*/1)));
	TestFalse(TEXT("B: two mouse buttons name different presses"),
		FVaCuusPointerPress::FromEvent(MakeMouseButtonEvent(At, EKeys::LeftMouseButton, NoButtons)) ==
			FVaCuusPointerPress::FromEvent(MakeMouseButtonEvent(At, EKeys::RightMouseButton, NoButtons)));
	TestFalse(TEXT("B: a mouse press is never a touch press"),
		FVaCuusPointerPress::FromEvent(TouchDown0) ==
			FVaCuusPointerPress::FromEvent(MakeMouseButtonEvent(At, EKeys::LeftMouseButton, NoButtons)));

	// -- C. The latch table. --
	{
		// C1: THE BEAD. One finger down, that finger up, released.
		FVaCuusPointerLatch Latch;
		Latch.NotePress(nullptr, FVaCuusPointerPress::FromEvent(TouchDown0));
		TestTrue(TEXT("C1: a touch press holds the latch"), Latch.IsHeld());
		Latch.NoteRelease(FVaCuusPointerPress::FromEvent(TouchUp0));
		TestFalse(TEXT("C1: THE BEAD -- the matching touch release ENDS the latch"), Latch.IsHeld());
	}
	{
		// C2: two fingers -- the first release must NOT end the gesture.
		FVaCuusPointerLatch Latch;
		Latch.NotePress(nullptr, FVaCuusPointerPress::FromEvent(TouchDown0));
		Latch.NotePress(nullptr, FVaCuusPointerPress::FromEvent(MakeTouchEvent(At, /*FingerIndex=*/1)));
		TestEqual(TEXT("C2: two fingers are two held presses"), Latch.NumHeldPresses(), 2);
		Latch.NoteRelease(FVaCuusPointerPress::FromEvent(TouchUp0));
		TestTrue(TEXT("C2: lifting one finger keeps the gesture"), Latch.IsHeld());
		Latch.NoteRelease(FVaCuusPointerPress::FromEvent(TouchUp1));
		TestFalse(TEXT("C2: lifting the last finger ends it"), Latch.IsHeld());
	}
	{
		// C3: a finger we never took a press for must not be able to end our gesture.
		FVaCuusPointerLatch Latch;
		Latch.NotePress(nullptr, FVaCuusPointerPress::FromEvent(TouchDown0));
		Latch.NoteRelease(FVaCuusPointerPress::FromEvent(TouchUp1));
		TestTrue(TEXT("C3: a FOREIGN finger's release does not end the gesture"), Latch.IsHeld());
	}
	{
		// C4: the plain mouse baseline.
		FVaCuusPointerLatch Latch;
		Latch.NotePress(nullptr, FVaCuusPointerPress::FromEvent(MakeMouseButtonEvent(At, EKeys::LeftMouseButton, NoButtons)));
		TestTrue(TEXT("C4: a mouse press holds the latch"), Latch.IsHeld());
		Latch.NoteRelease(FVaCuusPointerPress::FromEvent(MakeMouseButtonEvent(At, EKeys::LeftMouseButton, NoButtons)));
		TestFalse(TEXT("C4: its release ends the latch"), Latch.IsHeld());
	}
	{
		// C5: the two-button mouse drag the old `IsEmpty()` rule existed to protect.
		// This is the regression row: it must still pass, and `Num() <= 1` or "release
		// on any up" would fail it.
		FVaCuusPointerLatch Latch;
		Latch.NotePress(nullptr, FVaCuusPointerPress::FromEvent(MakeMouseButtonEvent(At, EKeys::LeftMouseButton, NoButtons)));
		Latch.NotePress(nullptr, FVaCuusPointerPress::FromEvent(MakeMouseButtonEvent(At, EKeys::RightMouseButton, NoButtons)));
		TestEqual(TEXT("C5: two buttons are two held presses"), Latch.NumHeldPresses(), 2);
		Latch.NoteRelease(FVaCuusPointerPress::FromEvent(MakeMouseButtonEvent(At, EKeys::LeftMouseButton, RightOnly)));
		TestTrue(TEXT("C5: releasing Left with Right still held keeps the drag"), Latch.IsHeld());
		Latch.NoteRelease(FVaCuusPointerPress::FromEvent(MakeMouseButtonEvent(At, EKeys::RightMouseButton, NoButtons)));
		TestFalse(TEXT("C5: releasing the last button ends it"), Latch.IsHeld());
	}
	{
		// C6: a repeated press is idempotent (a TSet, not a counter) -- otherwise a
		// re-reported press would leak a hold nothing can ever release.
		FVaCuusPointerLatch Latch;
		Latch.NotePress(nullptr, FVaCuusPointerPress::FromEvent(TouchDown0));
		Latch.NotePress(nullptr, FVaCuusPointerPress::FromEvent(TouchDown0));
		TestEqual(TEXT("C6: the same press twice is one held press"), Latch.NumHeldPresses(), 1);
		Latch.NoteRelease(FVaCuusPointerPress::FromEvent(TouchUp0));
		TestFalse(TEXT("C6: one release still ends it"), Latch.IsHeld());

		// And Drop is unconditional -- the panel-died path.
		Latch.NotePress(nullptr, FVaCuusPointerPress::FromEvent(TouchDown0));
		Latch.Drop();
		TestFalse(TEXT("C6: Drop ends a held gesture outright"), Latch.IsHeld());
	}

	// -- D. End to end through the real handlers, latched onto a REAL panel. --
	//
	// The panel has to be real. With a null one every handler takes its panel-died
	// branch and drops the latch on the way past, which would make the assertions
	// below pass with the bug still in: the point of D is that the processor stops
	// consuming, and only a live latch can be seen to stop.
	if (!SuiteViable(*this))
	{
		return true;
	}
	if (GEngine->GameViewport != nullptr)
	{
		AddInfo(TEXT("Skipped D: a game viewport exists (interactive editor?), so the post-release event would trace"));
		return true;
	}

	FVaCuusModule& Module = FVaCuusModule::Get();
	if (!TestNotNull(TEXT("D: UI thread"), Module.GetOrStartUIThread()))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	FWorldFixture Fixture;
	if (!Fixture.Init(*this))
	{
		return false;
	}
	UWorld* World = Fixture.GetWorld();
	AActor* Actor = World->SpawnActor<AActor>();

	UVaCuusWorldComponent* Panel = NewObject<UVaCuusWorldComponent>(Actor);
	Panel->DrawSize = FIntPoint(64, 64);
	Panel->bAutoLoadDocument = false;
	Panel->RegisterComponentWithWorld(World);
	if (!TestNotNull(TEXT("D: the panel has a view to latch onto"), Panel->GetView()))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Panel->UnregisterComponent();
	};

	FSlateApplication& Slate = FSlateApplication::Get();
	const FPointerEvent TouchMove = MakeTouchEvent(FVector2D(121, 81), /*FingerIndex=*/0);

	{
		const TSharedRef<FVaCuusWorldInputProcessor> Processor = MakeShared<FVaCuusWorldInputProcessor>();
		Processor->SeedLatchForTest(TouchDown0, Panel);
		if (!TestTrue(TEXT("D: the seeded touch press holds the latch"), Processor->GetLatch().IsHeld()))
		{
			return false;
		}

		// While latched every event is ours, coverage or not -- the capture rule.
		TestTrue(TEXT("D: a latched move is consumed"), Processor->HandleMouseMoveEvent(Slate, TouchMove));

		TestTrue(TEXT("D: the touch release is consumed (its press was ours)"),
			Processor->HandleMouseButtonUpEvent(Slate, TouchUp0));
		TestFalse(TEXT("D: THE BEAD -- and the latch is no longer held"), Processor->GetLatch().IsHeld());

		// THE SYMPTOM. Before the fix this stayed true forever and no other widget in
		// the application ever saw a pointer event again.
		const int64 DeferredBefore = static_cast<int64>(Processor->GetNumDeferredToSlate());
		TestFalse(TEXT("D: THE SYMPTOM -- the NEXT event is handed back to Slate, not consumed"),
			Processor->HandleMouseMoveEvent(Slate, TouchMove));
		TestEqual(TEXT("D: ...and is counted as deferred, so it took the un-latched path"),
			static_cast<int64>(Processor->GetNumDeferredToSlate()), DeferredBefore + 1);
	}
	{
		// D2: the negative. A foreign finger's release through the REAL handler leaves
		// the gesture running -- so D is not passing merely because everything releases.
		const TSharedRef<FVaCuusWorldInputProcessor> Processor = MakeShared<FVaCuusWorldInputProcessor>();
		Processor->SeedLatchForTest(TouchDown0, Panel);
		TestTrue(TEXT("D2: a foreign finger's release is still consumed"),
			Processor->HandleMouseButtonUpEvent(Slate, TouchUp1));
		TestTrue(TEXT("D2: ...and the gesture is still held"), Processor->GetLatch().IsHeld());
		TestTrue(TEXT("D2: ...so the next event is still ours"), Processor->HandleMouseMoveEvent(Slate, TouchMove));
	}

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
