// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusEngine.h"
#include "VaCuusRmlDocumentHost.h"
#include "VaCuusSlateElement.h"
#include "VaCuusSubsystem.h"
#include "VaCuusUIThread.h"
#include "VaCuusView.h"

#include "VaCuusDataStyleIdleTestTypes.h"

#include "Engine/GameInstance.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h" // complete UPackage for NewObject(GetTransientPackage()) — UObjectGlobals.h only forward-declares it
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * BEAD VaCuus-akj.17 — A UNIT-BEARING data-style BINDING MUST GO IDLE, and before the vendored
 * patch it could not.
 *
 * THE DEFECT, and it is one line of RmlUi. DataViewStyle::Update decides whether the bound
 * value needs re-applying by comparing it against `p->Get<String>()`, and Property::Get<T>
 * forwards straight to the property's value Variant (Property.h:41-45), which does not carry
 * the unit — that is a separate member (Property.h:50-51). The property parsed out of "4px"
 * therefore reads back as "4", never equals the string that produced it, and SetProperty fires
 * on every dirty of the bound variable. Nothing downstream absorbs the write itself:
 * ElementStyle::SetProperty does not compare, it just dirties (ElementStyle.cpp:606-618), and
 * ElementStyle::ComputeValues returns the DIRTY set, not a changed-value set (:1022-1026), so
 * Element::OnPropertyChange runs as if the value had really moved.
 *
 * WHICH PROPERTY IS BOUND DECIDES WHETHER THAT COSTS A PUBLISHED FRAME, and the first version
 * of this test got it wrong — it bound `width` only, and passed with the patch REVERTED (100
 * dirty frames, 0 published). Two RmlUi shortcuts absorb the layout path: an unchanged box
 * never reaches OnResize, so no background is dirtied, and a text element reuses its compiled
 * geometry when the regenerated mesh compares equal (ElementText.cpp:530-536). The border and
 * background path has no such check — ElementBackgroundBorder::GenerateGeometry releases and
 * re-makes the mesh unconditionally (ElementBackgroundBorder.cpp:131-137) — so a no-op write
 * to `border-left-width` is a fresh handle plus resource traffic, which is both legs of the
 * publish gate. That is the leg the assertion below rests on; `width` rides along to keep the
 * document honest and to pin the absorbed case if RmlUi ever drops its mesh comparison.
 *
 * WHY THE MODEL IS NESTED. The differ only dirties a field whose value really changed, so the
 * defect needs a variable that is dirtied while the bound value stands still — and RmlUi gives
 * that away for free: a nested leaf dirties its ROOT's name (VaCuusBoundModel.cpp:379-387) and
 * views are matched by the FIRST name of their address (DataExpression.cpp:1144-1153), so
 * `Bar.Tick` moving re-evaluates both bindings. The shipped reference HUD hits the same shape
 * per row: refhud.rml:128 binds `data-style-width="row.Ping * 0.3 + 'px'"` inside a `data-for`,
 * where ANY field of ANY row dirties the whole array name.
 *
 * WHAT MAKES THIS A TEST AND NOT A CLAIM. Revert the patch (Source/ThirdParty/RmlUi/
 * VENDORED_TAG.txt, patch #1 — restore `p->Get<String>() != value`) and the assertion below
 * reports 100 published frames instead of 0. The idle window is measured, not asserted about;
 * the controls around it are what make the zero mean "idle" rather than "broken":
 *
 *   1. the model really is dirtied on every one of those frames (NumOutstandingModelFields);
 *   2. a REAL change to EACH of the two bindings still publishes;
 *   3. and it settles back to zero afterwards.
 *
 * VaCuus.Model.View.Idle is the sibling that pins the unchanged-model case; this one pins the
 * changing-model case where the STYLE values are what stand still.
 */
namespace VaCuusDataStyleIdleTest
{
static const TCHAR* GModelName = TEXT("hud");
static const FIntPoint GViewSize(400, 300);

/**
 * The label is not decoration: with real glyphs on the document (FVaCuusEngine::Initialize
 * registers Content/DevUI/fonts/LatoLatin-Regular.ttf as 'LatoLatin'), the relayout that a
 * no-op `width` write provokes has something to regenerate, so the absorbed case documented
 * above is really being exercised rather than merely asserted.
 *
 * border-color is explicit because a border with no colour emits no visible geometry, and the
 * publishing leg needs the border to be real.
 */
static const TCHAR* GDocument = TEXT(R"(<rml>
<head><style>
body { display: block; width: 100%; height: 100%; font-family: LatoLatin; font-size: 20px; color: #FFFFFF; }
div { display: block; }
#track { width: 200px; height: 12px; background-color: #303030; }
#fill { height: 12px; background-color: #C04040; border-color: #FFFFFF; }
</style></head>
<body data-model="hud">
	<div id="label">STATIC LABEL</div>
	<div id="track"><div id="fill" data-style-width="Bar.Width" data-style-border-left-width="Bar.BorderWidth"/></div>
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
 * A view built the production way, on the production host — the same fixture shape as
 * VaCuus.Model.View.Idle, deliberately not shared with it: that test's fixture lives in its own
 * translation unit, and a shared one would couple two tests whose documents and models have
 * nothing in common.
 *
 * The game instance is never Init()ed — it exists only because UGameInstanceSubsystem declares
 * `Within = GameInstance` (GameInstanceSubsystem.h:15) and StaticAllocateObject ensures on an
 * outer of the wrong class.
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
		// A real Slate element, never painted: published buffers simply queue up on it and
		// Draw_RenderThread never runs. The production host requires one.
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
}	 // namespace VaCuusDataStyleIdleTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusDataStyleIdleTest, "VaCuus.Model.View.DataStyleIdle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusDataStyleIdleTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusDataStyleIdleTest;

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

	const UScriptStruct* Type = FVaCuusDataStyleTestModel::StaticStruct();

	if (!TestTrue(TEXT("BindModel succeeded"), Fixture.View->BindModel(GModelName, Type)))
	{
		return false;
	}

	FVaCuusDataStyleTestModel Live;
	Live.Bar.Width = TEXT("120px");
	Live.Bar.BorderWidth = TEXT("4px");
	Fixture.View->UpdateModel(GModelName, Type, &Live);

	// THE VALUES REACH THE UI SHADOW BEFORE THE DOCUMENT DOES, and the order is not cosmetic.
	// A data view evaluates once the moment it is instanced; with the shadow still holding
	// default-constructed FStrings that first evaluation is SetProperty("width", "") and RmlUi
	// logs `Syntax error parsing inline property declaration 'width: ;'`. Harmless — the real
	// values land a frame later — but it is a fixture artefact in a test whose whole subject
	// is spurious property writes, so it is spent here rather than explained.
	if (!TestTrue(TEXT("the bound values reached the UI shadow"), Fixture.Frame(*UIThread, 2)))
	{
		return false;
	}

	Fixture.View->LoadDocumentFromMemory(GDocument);

	// ---- 1. Settle. ----
	//
	// Load, first apply, glyph atlas and first layout all genuinely publish. Note that the
	// defect cannot show itself here: nothing dirties the model during settle, so the style
	// views are never re-evaluated and a broken build settles exactly like a fixed one.
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

	// ---- 2. A HUNDRED DIRTY FRAMES WITH STANDING-STILL STYLE VALUES. ----
	//
	// `Bar.Tick` moves every frame and reaches no view; `Bar.Width` and `Bar.BorderWidth` never
	// move. All three dirty the variable `Bar`, so RmlUi re-evaluates both bindings a hundred
	// times against values that never changed. Every one of those re-evaluations used to call
	// SetProperty, and the border one used to cost a published frame.
	int32 DirtyFrames = 0;
	for (int32 Frame = 0; Frame < 100; ++Frame)
	{
		++Live.Bar.Tick;
		Fixture.View->UpdateModel(GModelName, Type, &Live);

		// CONTROL 1, and the zero below is worthless without it: a test that quietly stopped
		// dirtying the model would report a perfect idle ratio while measuring nothing.
		if (Fixture.View->NumOutstandingModelFields(GModelName) > 0)
		{
			++DirtyFrames;
		}

		if (!TestTrue(TEXT("a hundred frames ran"), Fixture.Frame(*UIThread)))
		{
			return false;
		}
	}

	const uint64 RecordedAfter = Fixture.View->GetFramesRecorded();
	const uint64 PublishedAfter = Fixture.View->GetFramesPublished();

	AddInfo(FString::Printf(TEXT("100 dirty frames: %llu published, %llu recorded, %d frames carried a dirty field"),
		PublishedAfter - PublishedBefore, RecordedAfter - RecordedBefore, DirtyFrames));

	TestEqual(TEXT("every one of the hundred frames really dirtied the model"), DirtyFrames, 100);
	TestTrue(TEXT("the view really did record those hundred frames"), RecordedAfter >= RecordedBefore + 100);
	TestEqual(TEXT("and the unit-bearing data-style bindings published NOT ONE of them (akj.17)"),
		int32(PublishedAfter - PublishedBefore), 0);

	// ---- 3. CONTROL 2: both bindings are alive, one at a time. ----
	//
	// Without this the zero above would also be produced by a dead pipeline — an unresolved
	// `data-model`, views that never instanced, a document that never loaded. A real change to
	// each bound property must still reach the screen.
	Live.Bar.BorderWidth = TEXT("8px");
	Fixture.View->UpdateModel(GModelName, Type, &Live);
	if (!TestTrue(TEXT("frames ran"), Fixture.Frame(*UIThread, 3)))
	{
		return false;
	}
	const uint64 PublishedAfterBorder = Fixture.View->GetFramesPublished();
	TestTrue(TEXT("a changed border-left-width publishes again"), PublishedAfterBorder > PublishedAfter);

	Live.Bar.Width = TEXT("160px");
	Fixture.View->UpdateModel(GModelName, Type, &Live);
	if (!TestTrue(TEXT("frames ran"), Fixture.Frame(*UIThread, 3)))
	{
		return false;
	}
	TestTrue(TEXT("a changed width publishes again"), Fixture.View->GetFramesPublished() > PublishedAfterBorder);

	// ---- 4. CONTROL 3: and it settles at the new values. ----

	const uint64 PublishedAfterChange = Fixture.View->GetFramesPublished();
	for (int32 Frame = 0; Frame < 20; ++Frame)
	{
		++Live.Bar.Tick;
		Fixture.View->UpdateModel(GModelName, Type, &Live);
		if (!TestTrue(TEXT("frames ran"), Fixture.Frame(*UIThread)))
		{
			return false;
		}
	}
	TestEqual(TEXT("the new values settle back to zero published frames"),
		int32(Fixture.View->GetFramesPublished() - PublishedAfterChange), 0);

	Fixture.Subsystem->DestroyView(Fixture.View);
	RunFrames(*UIThread, 1);

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
