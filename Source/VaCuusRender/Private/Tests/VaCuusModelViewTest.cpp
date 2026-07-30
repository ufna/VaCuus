// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusEngine.h"
#include "VaCuusRmlDocumentHost.h"
#include "VaCuusSlateElement.h"
#include "VaCuusSubsystem.h"
#include "VaCuusUIThread.h"
#include "VaCuusView.h"

#include "VaCuusModelViewTestTypes.h"

#include "Engine/GameInstance.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * SPEC 9'S IDLE ROW, THROUGH THE REAL RECORDER: a bound, unchanging model publishes NOTHING.
 *
 * This is the correctness gate wearing a performance costume. If merely HAVING a model made a
 * view publish every frame, M2's central result would be silently undone -- on a static HUD
 * that result is ~13,000 recorded frames to 1 published, and the difference is invisible on
 * screen. The failure mode has no log line and no wrong pixel; only these two counters show it.
 *
 * WHY IT LIVES HERE AND NOT WITH THE OTHER MODEL TESTS. Everything that decides "published" is
 * private to VaCuusRender -- FVaCuusRecordingRenderInterface's content hash and its four
 * resource-delta arrays, and FVaCuusRmlDocumentHost which drives them -- while
 * FVaCuusBoundModel is private to VaCuus. The two meet only at UVaCuusView's public API, which
 * is exactly what this test drives.
 *
 * WHAT IT DOES NOT SHOW, MEASURED RATHER THAN ASSUMED. The zero below is defended TWICE, and
 * this test cannot tell the two layers apart. Make the VaCuus side maximally sloppy -- force
 * FVaCuusBoundModel::PublishPending to MarkEveryFieldDirty() first, so every frame publishes,
 * applies and dirties every variable -- and this test still passes with the same numbers (1
 * published, 11 settle frames), while VaCuus.Model.Apply fails seven assertions. The reason is
 * RmlUi's own: DataViewText::Update and DataViewAttribute::Update only touch the DOM when the
 * evaluated value actually CHANGED -- `if (result && entry.value != value)` before SetText
 * (DataViewDefault.cpp:354) and `if (!attribute || attribute->Get<String>() != value)` before
 * SetAttribute (:79) -- so re-dirtying an unchanged variable writes nothing, moves no geometry
 * and cannot fail the idle gate.
 *
 * So this asserts the END RESULT -- a bound model costs zero published frames, which is what
 * the M2 result and the render/composite budget actually depend on -- and its sibling
 * VaCuus.Model.Apply asserts the VaCuus-side half (no publish, no apply, no DOM change) where
 * it can see the model directly. Note also that "zero published frames" is not "zero work":
 * re-dirtying every frame would still pay spec 9's re-evaluation cost inside Context::Update()
 * and nothing here would notice.
 *
 * The game struct's `Title` reaches the screen as TEXT, with a font-family that resolves, so
 * the positive control at the end is a real glyph-geometry change: RmlUi has no way to edit a
 * compiled geometry, so a changed string is a release plus a fresh compile -- resource traffic
 * AND a moved content hash, i.e. a publish by both of the idle gate's legs.
 */
namespace VaCuusModelViewTest
{
static const FName GModelName(TEXT("hud"));
static const FIntPoint GViewSize(400, 300);

/**
 * font-family is not decoration here. FVaCuusEngine::Initialize loads
 * Content/DevUI/fonts/LatoLatin-Regular.ttf and RmlUi registers it under the family
 * 'LatoLatin' (the same name m1_hud.rcss and m2_demo.rcss use). Without it RmlUi logs "No font
 * face defined", lays out no text, and emits no geometry for the label -- so a changed
 * {{Title}} would produce a byte-identical frame and the positive control below would assert
 * nothing.
 */
static const TCHAR* GDocument = TEXT(R"(<rml>
<head><style>
body { display: block; width: 100%; height: 100%; font-family: LatoLatin; font-size: 20px; color: #FFFFFF; }
div { display: block; }
</style></head>
<body data-model="hud">
	<div id="title">{{Title}}</div>
</body>
</rml>)");

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

/**
 * A view built the production way, on the production host.
 *
 * The game instance is never Init()ed -- it exists only because UGameInstanceSubsystem declares
 * `Within = GameInstance` (GameInstanceSubsystem.h:15) and StaticAllocateObject ensures on an
 * outer of the wrong class. Nothing on this path reads it.
 */
struct FFixture
{
	TStrongObjectPtr<UGameInstance> GameInstance;
	TStrongObjectPtr<UVaCuusSubsystem> Subsystem;
	TSharedPtr<FVaCuusSlateElement> Element;
	UVaCuusView* View = nullptr;

	FFixture()
		: GameInstance(NewObject<UGameInstance>(GetTransientPackage()))
		, Subsystem(NewObject<UVaCuusSubsystem>(GameInstance.Get()))
		, Element(MakeShared<FVaCuusSlateElement>())
	{
		// A REAL Slate element, never painted: nothing here draws, so published buffers simply
		// queue up on it and Draw_RenderThread never runs. The production host requires one.
		View = Subsystem->CreateView(MakeUnique<FVaCuusRmlDocumentHost>(Element.ToSharedRef()), GViewSize);
	}

	FFixture(const FFixture&) = delete;
	FFixture& operator=(const FFixture&) = delete;

	/** One game frame: publish whatever UpdateModel marked, then run UI frames to apply it. */
	bool Frame(FVaCuusUIThread& UIThread, int32 NumUIFrames = 1)
	{
		Subsystem->Tick(0.016f);
		return RunFrames(UIThread, NumUIFrames);
	}
};
}	 // namespace VaCuusModelViewTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelViewIdleTest, "VaCuus.Model.View.Idle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelViewIdleTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelViewTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no UI thread to drive"));
		return true;
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

	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	FFixture Fixture;
	if (!TestNotNull(TEXT("the subsystem created a view"), Fixture.View))
	{
		return false;
	}

	const UScriptStruct* Type = FVaCuusModelViewTestModel::StaticStruct();

	if (!TestTrue(TEXT("BindModel succeeded"), Fixture.View->BindModel(GModelName, Type)))
	{
		return false;
	}
	Fixture.View->LoadDocumentFromMemory(GDocument);

	FVaCuusModelViewTestModel Live;
	Live.Title = TEXT("Alpha");
	Fixture.View->UpdateModel(GModelName, Type, &Live);

	// ---- 1. Let the view reach its steady state. ----
	//
	// A view's first frames genuinely do publish: the first one has nothing to compare against,
	// and the label's glyphs arrive as new geometry plus a font-atlas texture. What the idle
	// row is about is what happens AFTER that.
	int32 SettleFrames = 0;
	uint64 LastPublished = 0;
	int32 StableFrames = 0;
	while (SettleFrames < 200 && StableFrames < 10)
	{
		if (!TestTrue(TEXT("frames ran"), Fixture.Frame(*UIThread)))
		{
			return false;
		}
		++SettleFrames;

		const uint64 Published = Fixture.View->GetFramesPublished();
		StableFrames = (Published == LastPublished) ? StableFrames + 1 : 0;
		LastPublished = Published;
	}

	if (!TestTrue(TEXT("the view reached a steady state"), StableFrames >= 10))
	{
		AddError(FString::Printf(TEXT("still publishing after %d frames (%llu published, %llu recorded)"), SettleFrames,
			Fixture.View->GetFramesPublished(), Fixture.View->GetFramesRecorded()));
		return false;
	}

	const uint64 PublishedBefore = Fixture.View->GetFramesPublished();
	const uint64 RecordedBefore = Fixture.View->GetFramesRecorded();
	AddInfo(FString::Printf(TEXT("settled after %d frames: %llu published, %llu recorded"), SettleFrames, PublishedBefore,
		RecordedBefore));

	// ---- 2. A HUNDRED FRAMES OF A BOUND, UNCHANGING MODEL. ----
	//
	// UpdateModel IS CALLED EVERY FRAME, which is the case the row is actually about: a game
	// that pushes its HUD struct every tick, unchanged. "Nobody called UpdateModel" would be a
	// much weaker test -- it would pass even if a publish dirtied every variable.
	for (int32 Frame = 0; Frame < 100; ++Frame)
	{
		Fixture.View->UpdateModel(GModelName, Type, &Live);
		if (!TestTrue(TEXT("a hundred frames ran"), Fixture.Frame(*UIThread)))
		{
			return false;
		}
	}

	const uint64 RecordedAfter = Fixture.View->GetFramesRecorded();
	const uint64 PublishedAfter = Fixture.View->GetFramesPublished();

	TestTrue(TEXT("the view really did record those hundred frames"), RecordedAfter >= RecordedBefore + 100);
	TestEqual(TEXT("and published NOT ONE of them (spec 9's idle row)"), int32(PublishedAfter - PublishedBefore), 0);

	// The differ found nothing, so the channel never swapped and the UI thread was never given
	// anything to apply. That is the chain the zero above rests on.
	TestEqual(TEXT("and nothing was ever outstanding"), Fixture.View->NumOutstandingModelFields(GModelName), 0);

	// ---- 3. The positive control, and it is the whole reason the zero above means anything. ----
	//
	// Every assertion so far would also pass on a view whose pipeline was DEAD -- a model that
	// never reached its document, or a document that never resolved `data-model`, publishes
	// nothing either. This is what separates "idle" from "broken".
	Live.Title = TEXT("Beta");
	Fixture.View->UpdateModel(GModelName, Type, &Live);
	TestEqual(TEXT("one field changed"), Fixture.View->NumOutstandingModelFields(GModelName), 1);

	if (!TestTrue(TEXT("frames ran"), Fixture.Frame(*UIThread, 3)))
	{
		return false;
	}

	// THE END-TO-END EVIDENCE AT THIS LEVEL: a game-thread struct field changed and a command
	// buffer left for the render thread. RmlUi cannot edit a compiled geometry in place, so a
	// changed string is a release plus a fresh compile -- resource traffic and a moved content
	// hash, both of which fail the idle gate.
	TestTrue(TEXT("a changed field publishes a frame again"), Fixture.View->GetFramesPublished() > PublishedAfter);
	TestEqual(TEXT("and the echo came back"), Fixture.View->NumOutstandingModelFields(GModelName), 0);

	// ---- 4. And it settles again. ----

	const uint64 PublishedAfterChange = Fixture.View->GetFramesPublished();
	for (int32 Frame = 0; Frame < 20; ++Frame)
	{
		Fixture.View->UpdateModel(GModelName, Type, &Live);
		if (!TestTrue(TEXT("frames ran"), Fixture.Frame(*UIThread)))
		{
			return false;
		}
	}
	TestEqual(TEXT("the change settles back to zero published frames"),
		int32(Fixture.View->GetFramesPublished() - PublishedAfterChange), 0);

	Fixture.Subsystem->DestroyView(Fixture.View);
	RunFrames(*UIThread, 1);

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
