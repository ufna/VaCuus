// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusInteractiveSnapshot.h"
#include "VaCuusTestDocumentHost.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "HAL/PlatformProcess.h"
#include "Misc/ScopeExit.h"

#include <RmlUi/Core.h>

#include <atomic>

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusSnapshotTest
{
/**
 * A document host that does everything the production one does about layout and
 * the snapshot, and nothing at all about rendering.
 *
 * WHY A PROBE HOST: the snapshot has to be produced by a real Rml::Context that a
 * real UI frame updated -- the DFS reads cached absolute offsets and boxes, which
 * only Context::Update() makes valid -- but none of that needs an RHI, a viewport
 * or PIE. So the shared FVaCuusTestDocumentHost creates a context against the engine's
 * global (null) render interface -- the base's default, i.e. no CreateRenderInterface()
 * override, which is the one thing VaCuusMultiViewTest's probe adds -- and this class
 * updates it, builds the snapshot and publishes it through the same FVaCuusViewStatus
 * the game thread would read.
 */
class FProbeHost : public FVaCuusTestDocumentHost
{
public:
	FProbeHost()
		: FVaCuusTestDocumentHost(TEXT("vacuus_snapshot_view"), "vacuus://snapshot.rml", Rml::FocusFlag::Auto)
	{
	}

	virtual void SetVisible(bool bVisible) override
	{
		check(FVaCuusUIThread::IsInUIThread());
		if (!RmlDocument)
		{
			return;
		}

		if (bVisible)
		{
			RmlDocument->Show();
		}
		else
		{
			RmlDocument->Hide();
		}
	}

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		// Exactly the production order: Update() first (that is what leaves offsets
		// and boxes clean), then the snapshot, then publish. No Render() -- this host
		// has nothing to render into.
		Context->Update();

		FVaCuusInteractiveSnapshot& Snapshot = Status->GetSnapshotWriteBuffer();
		const FVaCuusSnapshotBuildStats Stats =
			BuildVaCuusInteractiveSnapshot(*Context, ViewSize, ++SnapshotGeneration, Snapshot);
		Status->PublishSnapshot();

		LastElementsVisited.store(Stats.NumElementsVisited, std::memory_order_relaxed);
		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

	/** Test-thread readable: how big the walked tree was on the last frame. */
	std::atomic<int32> LastElementsVisited{0};

private:
	uint64 SnapshotGeneration = 0;
};

/** Runs the snapshot before Render, as production does; the next frame sees its transforms. */
class FFragmentProbeHost final : public FProbeHost
{
public:
	virtual void RecordAndPublishFrame() override
	{
		FProbeHost::RecordAndPublishFrame();
		FragmentCorners.Reset();
		bHasTransform = false;
		if (Rml::Element* Element = RmlDocument->GetElementById("wrapped"))
		{
			bHasTransform = Element->GetTransformState() != nullptr;
			const Rml::Vector2f Position = Element->GetAbsoluteOffset(Rml::BoxArea::Border);
			for (int32 Index = 0; Index < Element->GetNumBoxes(); ++Index)
			{
				Rml::Vector2f Offset;
				const Rml::Box& Box = Element->GetBox(Index, Offset);
				const Rml::Vector2f Min = Position + Offset;
				const Rml::Vector2f Max = Min + Box.GetSize(Rml::BoxArea::Border);
				FragmentCorners.Append({FVector2f(Min.x, Min.y), FVector2f(Max.x, Min.y),
					FVector2f(Max.x, Max.y), FVector2f(Min.x, Max.y)});
			}
		}
		Context->Render();
	}

	// Read only after RunFrames' acquire of the UI frame counter, like the shared probe base.
	TArray<FVector2f> FragmentCorners;
	bool bHasTransform = false;
};

/**
 * Runs exactly NumFrames UI frames, one trigger at a time.
 *
 * Triggering N times and waiting for N frames does NOT work: the wake event is an
 * auto-reset binary latch, so triggers arriving while a frame is in flight
 * coalesce. Waiting for each frame before asking for the next is the only way to
 * count frames from outside.
 */
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
 * The document under test. 400x300 view; no text anywhere, so the test does not
 * depend on a font being installed.
 *
 *  #btn        a 100x40 <button> at (20,20), tab-index:auto  -> reported, FOCUSABLE
 *  #ghost      tab-index:auto, but pointer-events:none        -> NOT reported
 *  #ghost-kid  a button inside #ghost                         -> reported anyway
 *  #pass       tab-index:auto under `vacuus-passthrough`      -> NOT reported
 *  #pass-kid   a button inside the pass-through region        -> NOT reported
 *  #clip       overflow:hidden, 100x50 at (200,20)            -> not interactive
 *  #clipped    a 100x200 button inside it                     -> clipped to #clip
 *  #marked     a plain <div> with `vacuus-interactive`         -> reported, NOT focusable
 *  #pass-mixed tab-index:auto under `vacuus-PassThrough`       -> NOT reported
 *  #mark-mixed a plain <div> with `VACUUS-INTERACTIVE`         -> reported
 *  #field      an <input type=text>                            -> reported, FOCUSABLE
 *  #noFocus    focus:none around a tab-index:auto button       -> reported, NOT focusable
 *  #noFocusKid the button inside it                            -> reported, NOT focusable
 *  #evclick    a plain <div> with `data-event-click`           -> reported, NOT focusable
 *  #onpress    a plain <div> with `onmousedown`                -> reported
 *  #evhover    a plain <div> with `data-event-mouseover` only  -> NOT reported
 *  #measured   pointer-events:none div, `vacuus-measure="7"`   -> MEASURED, not covered
 *  #measured-clipped  `VACUUS-MEASURE="8"` in #clip, past it   -> measured UNCLIPPED
 *  #measured-bad      `vacuus-measure="x"`                     -> not measured
 *  #pass-measured     `vacuus-measure="9"` under #pass         -> not measured (pruned)
 *
 * #marked, #field, #noFocus and #noFocusKid are controller decision D11's assertions:
 * interactive and focusable are DIFFERENT properties of the same rect. #marked proves
 * the direction that matters -- an opt-in marker makes a div take clicks without
 * making it take the keyboard -- and #field proves the other one without any RCSS
 * at all, because RmlUi's form controls set `tab-index: auto` on themselves
 * (ElementFormControl.cpp:8). #noFocus is the `focus: none` subtree prune, which is
 * the one focusability rule that is inherited rather than per element
 * (ElementDocument.cpp:34-38).
 *
 * #pass-mixed and #mark-mixed are the case guard: RmlUi lowercases TAG names while
 * parsing but leaves attribute names exactly as the author typed them
 * (BaseXMLParser.cpp:306-345) and HasAttribute is a case-sensitive find
 * (Element.cpp:861-864), so a lowercase-only marker lookup silently ignores both
 * of them -- which for the opt-OUT means eating clicks meant for the game.
 *
 * #evclick, #onpress and #evhover are bead VaCuus-w87.2: a press handler the author
 * wrote IN THE MARKUP is visible to the attribute pass -- `data-event-*` attributes stay on
 * the element after ApplyDataViewsControllers parses them (ElementUtilities.cpp:415-463), and
 * an inline `on*` handler is an attribute by definition -- so the element takes the press
 * without anyone having to know about `vacuus-interactive`. Hover-only handlers do not: an
 * overlay that lights up under the cursor must not eat the click aimed at the game.
 *
 * The #measured entries are `vacuus-measure` (5d): a box published under its key, NOT
 * clipped and independent of pointer coverage, its attribute name matched the same
 * case-insensitive way as the two markers, and nothing published from a pass-through
 * subtree, which the walk never enters.
 *
 * Coordinates are absolute and border-box-sized (no borders, no padding) so the
 * expected rects are exactly what the RCSS says.
 */
static const TCHAR* GDocument = TEXT(R"(<rml>
<head>
<style>
body { display: block; width: 100%; height: 100%; }
div, button { display: block; position: absolute; }
#btn        { left: 20px;  top: 20px;  width: 100px; height: 40px; tab-index: auto; }
#ghost      { left: 20px;  top: 100px; width: 100px; height: 40px; tab-index: auto; pointer-events: none; }
#ghost-kid  { left: 10px;  top: 10px;  width: 20px;  height: 20px; pointer-events: auto; }
#pass       { left: 20px;  top: 200px; width: 100px; height: 40px; tab-index: auto; }
#pass-kid   { left: 10px;  top: 10px;  width: 20px;  height: 20px; }
#clip       { left: 200px; top: 20px;  width: 100px; height: 50px; overflow: hidden; }
#clipped    { left: 0px;   top: 0px;   width: 100px; height: 200px; }
#marked     { left: 200px; top: 200px; width: 60px;  height: 30px; }
#pass-mixed { left: 20px;  top: 250px; width: 100px; height: 40px; tab-index: auto; }
#mark-mixed { left: 280px; top: 250px; width: 60px;  height: 30px; }
#field      { display: block; position: absolute; left: 140px; top: 20px; width: 40px; height: 20px; }
#noFocus    { left: 300px; top: 120px; width: 80px;  height: 40px; focus: none; }
#noFocusKid { left: 0px;   top: 0px;   width: 40px;  height: 20px; tab-index: auto; }
#evclick    { left: 140px; top: 70px;  width: 40px;  height: 30px; }
#onpress    { left: 140px; top: 110px; width: 40px;  height: 30px; }
#evhover    { left: 140px; top: 150px; width: 40px;  height: 30px; }
#measured   { left: 200px; top: 150px; width: 60px;  height: 30px; pointer-events: none; }
#measured-clipped { left: 0px; top: 30px; width: 50px; height: 40px; }
#measured-bad     { left: 140px; top: 200px; width: 40px; height: 20px; }
#pass-measured    { left: 40px; top: 10px; width: 20px; height: 20px; }
</style>
</head>
<body>
	<button id="btn"/>
	<div id="ghost"><button id="ghost-kid"/></div>
	<div id="pass" vacuus-passthrough><button id="pass-kid"/><div id="pass-measured" vacuus-measure="9"/></div>
	<div id="clip"><button id="clipped"/><div id="measured-clipped" VACUUS-MEASURE="8"/></div>
	<div id="measured" vacuus-measure="7"/>
	<div id="measured-bad" vacuus-measure="x"/>
	<div id="marked" vacuus-interactive/>
	<div id="pass-mixed" vacuus-PassThrough/>
	<div id="mark-mixed" VACUUS-INTERACTIVE/>
	<input id="field" type="text"/>
	<div id="noFocus" vacuus-interactive><button id="noFocusKid"/></div>
	<div id="evclick" data-event-click="Pick = 1"/>
	<div id="onpress" onmousedown="pick"/>
	<div id="evhover" data-event-mouseover="Hot = 1"/>
</body>
</rml>)");
}	 // namespace VaCuusSnapshotTest

/**
 * The input contract: one UI frame produces a snapshot the game thread can answer
 * Slate from, with the interactive predicate, the pass-through opt-out and clipping
 * all behaving as documented on FVaCuusInteractiveSnapshot.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusSnapshotTest, "VaCuus.Input.Snapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusSnapshotTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusSnapshotTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no worker thread to drive"));
		return true;
	}

	// The UI thread boots RmlUi and claims ownership of it, so nothing else may hold
	// the library when this starts.
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

	const FIntPoint ViewSize(400, 300);
	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();

	// The snapshot before anything was published: the answer must already be safe.
	{
		const FVaCuusInteractiveSnapshot& Initial = Status->AcquireSnapshot();
		TestTrue(TEXT("No snapshot published yet: generation 0"), Initial.Generation == 0);
		TestEqual(TEXT("No snapshot published yet: no rects"), Initial.InteractiveRects.Num(), 0);
		TestFalse(TEXT("No snapshot published yet: nothing contains anything"),
			Initial.Contains(FIntPoint(70, 40)));
	}

	TUniquePtr<FProbeHost> OwnedHost = MakeUnique<FProbeHost>();
	FProbeHost* Host = OwnedHost.Get();

	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), ViewSize, Status);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GDocument, /*LoadSerial=*/1);

	// Two frames: the first drains AddView + the load and snapshots the fresh layout,
	// the second proves the steady state (and that Generation advances).
	if (!TestTrue(TEXT("UI frames ran"), RunFrames(*UIThread, 2)))
	{
		return false;
	}

	if (!TestTrue(TEXT("Document loaded"),
			Status->LoadCompletedSerial.load(std::memory_order_acquire) == 1 &&
				Status->LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded)))
	{
		return false;
	}

	const FVaCuusInteractiveSnapshot Snapshot = Status->AcquireSnapshot();

	TestTrue(TEXT("Generation advanced past the first frame"), Snapshot.Generation >= 2);
	TestTrue(TEXT("Snapshot carries the view size"), Snapshot.ViewSize == ViewSize);
	TestTrue(TEXT("The DFS walked a non-trivial tree"),
		Host->LastElementsVisited.load(std::memory_order_relaxed) >= 8);

	AddInfo(FString::Printf(TEXT("Snapshot: generation %llu, %d rect(s), %d element(s) visited"),
		Snapshot.Generation, Snapshot.InteractiveRects.Num(),
		Host->LastElementsVisited.load(std::memory_order_relaxed)));
	for (int32 Index = 0; Index < Snapshot.InteractiveRects.Num(); ++Index)
	{
		const FIntRect& Rect = Snapshot.InteractiveRects[Index];
		const bool bFocusable = Snapshot.RectFlags.IsValidIndex(Index) &&
			EnumHasAnyFlags(Snapshot.RectFlags[Index], EVaCuusRectFlags::Focusable);
		AddInfo(FString::Printf(TEXT("  rect (%d,%d)-(%d,%d)%s"), Rect.Min.X, Rect.Min.Y, Rect.Max.X, Rect.Max.Y,
			bFocusable ? TEXT(" focusable") : TEXT("")));
	}

	// 1. A 100x40 button at (20,20): its rect is present, and a point inside it is
	// covered. Half-open bounds, so the far edges (120, 60) are NOT covered.
	TestTrue(TEXT("The button's exact rect is reported"),
		Snapshot.InteractiveRects.Contains(FIntRect(20, 20, 120, 60)));
	TestTrue(TEXT("A point inside the button is covered"), Snapshot.Contains(FIntPoint(70, 40)));
	TestTrue(TEXT("The button's min corner is covered"), Snapshot.Contains(FIntPoint(20, 20)));
	TestFalse(TEXT("The button's max corner is not covered (half-open)"), Snapshot.Contains(FIntPoint(120, 60)));
	TestFalse(TEXT("A point just outside the button is not covered"), Snapshot.Contains(FIntPoint(19, 40)));

	// 2. pointer-events:none is a per-element skip, never a subtree prune: #ghost is
	// focusable but opted out, while its interactive descendant is still hit by RmlUi
	// (Context.cpp:1838 checks pointer-events only after descending) and so must
	// still be reported.
	TestFalse(TEXT("pointer-events:none element is not reported"),
		Snapshot.Contains(FIntPoint(90, 130)));
	TestTrue(TEXT("An interactive descendant of a pointer-events:none element still is"),
		Snapshot.Contains(FIntPoint(40, 120)));

	// 3. vacuus-passthrough prunes the element AND its subtree.
	TestFalse(TEXT("A pass-through region is not covered"), Snapshot.Contains(FIntPoint(70, 220)));
	TestFalse(TEXT("A button inside a pass-through region is not covered either"),
		Snapshot.Contains(FIntPoint(40, 220)));

	// 4. A rect nested in an overflow:hidden container is clipped to the container:
	// #clipped is 100x200 but #clip only shows 100x50 of it.
	TestTrue(TEXT("The clipped button's rect is cut to its container"),
		Snapshot.InteractiveRects.Contains(FIntRect(200, 20, 300, 70)));
	TestTrue(TEXT("A point inside the visible part of the clipped button is covered"),
		Snapshot.Contains(FIntPoint(250, 50)));
	TestFalse(TEXT("A point in the clipped-away part is not covered"),
		Snapshot.Contains(FIntPoint(250, 100)));

	// 5. The explicit opt-in makes a plain div interactive.
	TestTrue(TEXT("A vacuus-interactive div is reported"),
		Snapshot.InteractiveRects.Contains(FIntRect(200, 200, 260, 230)));

	// 5a. A press handler written in the markup is enough (bead VaCuus-w87.2). Before it, every
	// one of these divs looked right and passed its press to the game -- the field report's
	// "a clickable div did nothing", found only by a headless click driver. Strip the press
	// events from the attribute pass and the first two assertions go red.
	TestTrue(TEXT("A div with data-event-click is reported"),
		Snapshot.InteractiveRects.Contains(FIntRect(140, 70, 180, 100)));
	TestFalse(TEXT("but it is a pointer target, not a keyboard one"), Snapshot.IsFocusableAt(FIntPoint(160, 85)));
	TestTrue(TEXT("A div with an inline onmousedown handler is reported"),
		Snapshot.InteractiveRects.Contains(FIntRect(140, 110, 180, 140)));
	TestFalse(TEXT("A div with only a hover handler is NOT reported -- it must not eat the game's click"),
		Snapshot.Contains(FIntPoint(160, 165)));

	// 5b. Marker names are matched case-insensitively. RmlUi keeps attribute names
	// verbatim, so without this an author's `vacuus-PassThrough` would be ignored --
	// and an ignored opt-out means the UI eats clicks the author routed to the game.
	TestFalse(TEXT("A mixed-case vacuus-PassThrough opts out too"), Snapshot.Contains(FIntPoint(70, 270)));
	TestTrue(TEXT("An upper-case VACUUS-INTERACTIVE div is reported"),
		Snapshot.InteractiveRects.Contains(FIntRect(280, 250, 340, 280)));

	// 5c. CONTROLLER DECISION D11: every rect carries flags, in a parallel array whose
	// indices line up with the rects. The invariant first, because every assertion below
	// depends on it and a silent mismatch would make IsFocusableAt() answer about the
	// wrong rectangle.
	TestEqual(TEXT("There is exactly one flags entry per rect"),
		Snapshot.RectFlags.Num(), Snapshot.InteractiveRects.Num());
	{
		bool bAllInteractive = true;
		for (const EVaCuusRectFlags Flags : Snapshot.RectFlags)
		{
			bAllInteractive &= EnumHasAnyFlags(Flags, EVaCuusRectFlags::Interactive);
		}
		TestTrue(TEXT("Every reported rect is flagged Interactive (it is why it exists)"), bAllInteractive);
	}

	// A <button> with tab-index:auto: takes clicks AND takes focus, so one click on it
	// gives the widget Slate focus immediately (the bug D11 fixes).
	TestTrue(TEXT("A tab-index:auto button is focusable"), Snapshot.IsFocusableAt(FIntPoint(70, 40)));

	// An <input>: focusable with no RCSS help at all, because RmlUi's form controls set
	// tab-index:auto on themselves (ElementFormControl.cpp:8).
	TestTrue(TEXT("A text input is covered"), Snapshot.Contains(FIntPoint(160, 30)));
	TestTrue(TEXT("A text input is focusable without any RCSS"), Snapshot.IsFocusableAt(FIntPoint(160, 30)));

	// The distinction that matters: `vacuus-interactive` is an opt-in for POINTER
	// coverage only. It must not make a plain div take the keyboard, or every marked
	// region would steal focus from the game on the first click.
	TestTrue(TEXT("A vacuus-interactive div is covered"), Snapshot.Contains(FIntPoint(230, 215)));
	TestFalse(TEXT("A vacuus-interactive div is NOT focusable"), Snapshot.IsFocusableAt(FIntPoint(230, 215)));

	// `focus: none` is the one focusability rule that prunes a SUBTREE rather than a
	// single element (CanFocus::NoAndNoChildren, ElementDocument.cpp:34-38): the button
	// inside carries tab-index:auto and is still unfocusable, exactly as RmlUi's own
	// Tab and nav searches would find it.
	TestTrue(TEXT("A focus:none region still takes pointer events"), Snapshot.Contains(FIntPoint(320, 130)));
	TestFalse(TEXT("A focus:none region is not focusable"), Snapshot.IsFocusableAt(FIntPoint(320, 130)));
	TestTrue(TEXT("A button inside a focus:none region is still covered"), Snapshot.Contains(FIntPoint(310, 125)));
	TestFalse(TEXT("A tab-index:auto button inside a focus:none region is not focusable"),
		Snapshot.IsFocusableAt(FIntPoint(310, 125)));

	// And a point nothing covers is neither.
	TestFalse(TEXT("An uncovered point is not focusable"), Snapshot.IsFocusableAt(FIntPoint(370, 290)));

	// 5d. `vacuus-measure` publishes a box, independently of coverage. #measured is
	// pointer-events:none and still measured -- measuring is about layout, not clicks --
	// and it adds no coverage of its own.
	{
		const FIntRect* const Measured = Snapshot.FindMeasuredRect(7);
		TestNotNull(TEXT("A vacuus-measure element is measured"), Measured);
		if (Measured != nullptr)
		{
			TestTrue(TEXT("The measured box is the element's border box"), *Measured == FIntRect(200, 150, 260, 180));
		}
		TestFalse(TEXT("Measuring adds no pointer coverage"), Snapshot.Contains(FIntPoint(230, 165)));

		// UNCLIPPED: #measured-clipped runs 20px past #clip's bottom edge (70), and the
		// published box keeps them -- a caller asking for a size wants the whole box.
		// The upper-case attribute name rides the same case-insensitive match as the
		// two coverage markers.
		const FIntRect* const Clipped = Snapshot.FindMeasuredRect(8);
		TestNotNull(TEXT("An upper-case VACUUS-MEASURE element is measured"), Clipped);
		if (Clipped != nullptr)
		{
			TestTrue(TEXT("A measured box is not cut to its clipping ancestor"), *Clipped == FIntRect(200, 50, 250, 90));
		}

		TestNull(TEXT("A pass-through subtree is not walked, so nothing in it is measured"),
			Snapshot.FindMeasuredRect(9));

		// "x" is the only non-integer key in the document; were it published it would
		// sit next to 7 and 8 under whatever GetInto left in the key.
		TestEqual(TEXT("A key that is not an integer is not published"), Snapshot.MeasuredRects.Num(), 2);
	}

	// 6. Controller decision D9: a document element holding focus is NOT "the UI wants
	// the keyboard". Show() focuses the document itself when nothing inside it carries
	// `autofocus` (FocusFlag::Auto), which is the case here -- so the document holds
	// focus with no real focus target, and a click must not steal Slate focus from the
	// game on that basis.
	//
	// Note how this and 5c answer different questions about the SAME frame: there are
	// focusable rects (a click on one WOULD take focus) while nothing focusable HOLDS
	// focus. That gap is precisely why D11 needed per-rect flags rather than this bool.
	// The positive case (a genuinely focused element flips this to true) is proven in
	// VaCuus.Input.Routing, which can move focus by clicking.
	TestFalse(TEXT("A document that only focuses itself does not want the keyboard"),
		Snapshot.bWantsKeyboardFocus);

	// 7. Hiding the document empties the snapshot: IsVisible() prunes at the root, so
	// the game thread stops claiming coverage on the very next frame.
	UIThread->EnqueueSetVisible(ViewId, /*bVisible=*/false);
	if (!TestTrue(TEXT("UI frames ran after hiding"), RunFrames(*UIThread, 2)))
	{
		return false;
	}

	const FVaCuusInteractiveSnapshot& Hidden = Status->AcquireSnapshot();
	TestTrue(TEXT("A newer snapshot arrived after hiding"), Hidden.Generation > Snapshot.Generation);
	TestEqual(TEXT("A hidden document reports no interactive rects"), Hidden.InteractiveRects.Num(), 0);
	TestEqual(TEXT("A hidden document reports no rect flags either"), Hidden.RectFlags.Num(), 0);
	TestEqual(TEXT("A hidden document measures nothing"), Hidden.MeasuredRects.Num(), 0);
	TestFalse(TEXT("A hidden document covers nothing"), Hidden.Contains(FIntPoint(70, 40)));
	TestFalse(TEXT("A hidden document is focusable nowhere"), Hidden.IsFocusableAt(FIntPoint(70, 40)));

	return true;
}

/**
 * A wrapped inline has several border boxes, not one (InlineBox.cpp:117-126).
 * The original measurement published only the first; the fixed-size block fixtures above
 * cannot see that. Check native fragment corners against known 2D transforms, independently
 * of the projection helper used by the publisher. The short last line also leaves a gap:
 * rotating the untransformed union would include space that no fragment occupies.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMeasuredFragmentsTest, "VaCuus.Input.MeasuredFragments",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusMeasuredFragmentsTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusSnapshotTest;
	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support"));
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

	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
	TUniquePtr<FFragmentProbeHost> OwnedHost = MakeUnique<FFragmentProbeHost>();
	FFragmentProbeHost* Host = OwnedHost.Get();
	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), FIntPoint(800, 800), Status);

	struct FCase
	{
		const TCHAR* Transform;
		float XX, XY, YX, YY;
		bool bTransformed;
	};
	const float Cos = FMath::Cos(PI / 6.0f);
	const float Sin = FMath::Sin(PI / 6.0f);
	const FCase Cases[] = {
		{TEXT("none"), 1, 0, 0, 1, false},
		{TEXT("scale(2)"), 2, 0, 0, 2, true},
		{TEXT("rotate(30deg)"), Cos, -Sin, Sin, Cos, true},
	};
	uint64 Serial = 0;
	for (const FCase& Case : Cases)
	{
		const FString Document = FString::Printf(TEXT(R"(<rml><head><style>
body { display: block; font-family: LatoLatin; font-size: 16px; }
#holder { display: block; position: absolute; left: 20.25px; top: 20.25px; width: 60px;
  transform-origin: 0px 0px; transform: %s; }
span { display: inline; padding: 2px 3px; border: 1px solid white; }
</style></head><body><div id="holder"><span id="wrapped" vacuus-measure="11">word word word word word word word word word word word i</span></div></body></rml>)"), Case.Transform);
		UIThread->EnqueueLoadDocumentFromMemory(ViewId, Document, ++Serial);
		// Transform state is computed by Render, after the first snapshot, just as in production.
		if (!TestTrue(TEXT("Wrapped layout and transform frames ran"), RunFrames(*UIThread, 3)))
		{
			return false;
		}
		if (!TestTrue(TEXT("Wrapped document loaded"), Status->LoadCompletedSerial.load() == Serial
			&& Status->LoadResult.load() == uint8(EVaCuusLoadResult::Succeeded)))
		{
			return false;
		}
		if (!TestTrue(TEXT("The inline element has multiple native border boxes"), Host->FragmentCorners.Num() > 4))
		{
			return false;
		}
		TestEqual(TEXT("The transform branch is exercised"), Host->bHasTransform, Case.bTransformed);

		// Stay off exact pixel boundaries: the orthographic projection rounds in float,
		// whereas this independent affine oracle does not make that round trip.
		const FVector2f Origin(20.25f, 20.25f);
		FVector2f Min(TNumericLimits<float>::Max(), TNumericLimits<float>::Max());
		FVector2f Max(-TNumericLimits<float>::Max(), -TNumericLimits<float>::Max());
		for (const FVector2f& Corner : Host->FragmentCorners)
		{
			const FVector2f Offset = Corner - Origin;
			const FVector2f Projected = Origin + FVector2f(
				Case.XX * Offset.X + Case.XY * Offset.Y, Case.YX * Offset.X + Case.YY * Offset.Y);
			Min.X = FMath::Min(Min.X, Projected.X);
			Min.Y = FMath::Min(Min.Y, Projected.Y);
			Max.X = FMath::Max(Max.X, Projected.X);
			Max.Y = FMath::Max(Max.Y, Projected.Y);
		}
		const FIntRect Expected(FMath::FloorToInt(Min.X), FMath::FloorToInt(Min.Y),
			FMath::CeilToInt(Max.X), FMath::CeilToInt(Max.Y));
		const FVaCuusInteractiveSnapshot& Snapshot = Status->AcquireSnapshot();
		TestEqual(TEXT("Only the marked element is measured"), Snapshot.MeasuredRects.Num(), 1);
		if (const FIntRect* Measured = Snapshot.FindMeasuredRect(11))
		{
			TestTrue(FString::Printf(TEXT("%s: measured %s includes every fragment (%s)"),
				Case.Transform, *Measured->ToString(), *Expected.ToString()), *Measured == Expected);
		}
		else
		{
			AddError(TEXT("The wrapped element was not measured"));
		}
	}
	return true;
}

namespace VaCuusSnapshotTest
{
/**
 * One case per document, each a MINIMAL shape with exactly one thing to say about
 * bTabEntersFocus. Every one of them is loaded into the same probe host in turn, and in
 * every one the DOCUMENT ELEMENT is what holds focus -- Show() with FocusFlag::Auto focuses
 * the document itself unless a tabbable element carries `autofocus`
 * (ElementDocument.cpp:355-358, :369-401), and none of these do. That is bTabEntersFocus's
 * other precondition, so these documents isolate the one under test.
 */
struct FTabReachCase
{
	const TCHAR* Name;
	const TCHAR* Rml;
	bool bExpectTabEnters;
};

static const FTabReachCase GTabReachCases[] = {
	// The control: the flag is not stuck in either position.
	{TEXT("a plain focusable button"),
		TEXT(R"(<rml><head><style>
body { width: 100%; height: 100%; }
button { display: block; width: 100px; height: 40px; tab-index: auto; }
</style></head><body><button id="btn"/></body></rml>)"),
		true},

	{TEXT("no focusable anywhere"),
		TEXT(R"(<rml><head><style>
body { width: 100%; height: 100%; }
div { display: block; width: 100px; height: 40px; }
</style></head><body><div id="plain"/></body></rml>)"),
		false},

	// THE FIRST BUG (bead VaCuus-akj.6.36). `vacuus-passthrough` is a POINTER-coverage
	// opt-out; RmlUi's tab search has never heard of the attribute, so Tab walks straight
	// into this button. Accumulating focusability inside the rect DFS -- which prunes the
	// subtree here -- answered "Tab cannot enter", so SVaCuusWidget let the entering press
	// through to the game as well.
	{TEXT("the only focusable is inside a vacuus-passthrough wrapper"),
		TEXT(R"(<rml><head><style>
body { width: 100%; height: 100%; }
div, button { display: block; }
#btn { width: 100px; height: 40px; tab-index: auto; }
</style></head><body><div id="pass" vacuus-passthrough><button id="btn"/></div></body></rml>)"),
		true},

	// THE SECOND BUG, same cause. A zero-area clip container means no DESCENDANT RECT can
	// be hit, which is why the DFS prunes there -- and says nothing at all about whether
	// the button can be tabbed to. CanFocusElement does not look at boxes
	// (ElementDocument.cpp:30-44).
	{TEXT("the only focusable is inside a collapsed overflow:hidden container"),
		TEXT(R"(<rml><head><style>
body { width: 100%; height: 100%; }
div, button { display: block; }
#box { overflow: hidden; width: 0px; height: 0px; }
#btn { width: 100px; height: 40px; tab-index: auto; }
</style></head><body><div id="box"><button id="btn"/></div></body></rml>)"),
		true},

	// THE PRUNE THAT MUST SURVIVE, and the reason the fix is a MIRROR of RmlUi rather than
	// "stop pruning": CanFocusElement returns NoAndNoChildren for `focus: none`
	// (ElementDocument.cpp:37-38), so RmlUi will not tab in here either. A walk that
	// accepted everything would be just as wrong, in the direction that consumes the
	// player's key for nothing.
	{TEXT("the only focusable is under focus:none"),
		TEXT(R"(<rml><head><style>
body { width: 100%; height: 100%; }
div, button { display: block; }
#blocked { focus: none; }
#btn { width: 100px; height: 40px; tab-index: auto; }
</style></head><body><div id="blocked"><button id="btn"/></div></body></rml>)"),
		false},

	// The same for the other subtree prune both walks agree on.
	{TEXT("the only focusable is inside a display:none subtree"),
		TEXT(R"(<rml><head><style>
body { width: 100%; height: 100%; }
div, button { display: block; }
#gone { display: none; }
#btn { width: 100px; height: 40px; tab-index: auto; }
</style></head><body><div id="gone"><button id="btn"/></div></body></rml>)"),
		false},

	// THE FALSE POSITIVE, which the reverse mistake would produce. <progress> appends its
	// `fill` as a NON-DOM child (ElementProgress.cpp:24-27) and `tab-index` is an ordinary
	// registered property (StyleSheetSpecification.cpp:375) that matches non-DOM elements
	// like any other -- so one line of RCSS builds an element RmlUi's tab search provably
	// cannot reach, because SearchFocusSubtreeChildren asks for the DOM child count
	// (ElementDocument.cpp:741). The rect DFS passes include_non_dom_elements=true on
	// purpose (scrollbars take drags), which is exactly why focusability cannot ride on it.
	{TEXT("the only tab-index:auto element is a non-DOM child"),
		TEXT(R"(<rml><head><style>
body { width: 100%; height: 100%; }
progress { display: block; width: 100px; height: 20px; }
fill { tab-index: auto; }
</style></head><body><progress id="bar" value="0.5"/></body></rml>)"),
		false},
};
}	 // namespace VaCuusSnapshotTest

/**
 * "WHERE IS THE ELEMENT WITH THIS ID", ANSWERED BY THE UI THREAD (bead VaCuus-w87.15). The
 * observable half of vacuus.Click: a headless driver used to aim with coordinates measured by
 * hand off a screenshot, and they moved with every data-if and every resize. The query rides
 * the command queue like everything else that touches the tree, and answers through a shared
 * result the game thread polls -- no per-frame cost when nobody asks.
 *
 * Against GDocument: #btn is a 100x40 border box at (20,20); #nope does not exist; and a
 * hidden element is FOUND but reported not visible, so a driver can refuse to click it.
 *
 * RESTORE-THE-BUG: leave the command unhandled (the query stays Pending) and the first wait
 * times out; drop the IsVisible() read and the hidden element reads visible.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusElementBoundsTest, "VaCuus.Input.ElementBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusElementBoundsTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusSnapshotTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no worker thread to drive"));
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

	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MakeUnique<FProbeHost>(), FIntPoint(400, 300), Status);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GDocument, /*LoadSerial=*/1);
	if (!TestTrue(TEXT("UI frames ran"), RunFrames(*UIThread, 2)))
	{
		return false;
	}

	// One query per case, all in flight at once: each carries its own result.
	auto Ask = [UIThread, ViewId](const TCHAR* Id)
	{
		TSharedRef<FVaCuusElementBoundsQuery> Query = MakeShared<FVaCuusElementBoundsQuery>();
		Query->ElementId = Id;
		UIThread->EnqueueQueryElementBounds(ViewId, Query);
		return Query;
	};
	auto Answered = [](const FVaCuusElementBoundsQuery& Query)
	{ return Query.State.load(std::memory_order_acquire) != FVaCuusElementBoundsQuery::EState::Pending; };
	auto WaitFor = [UIThread, &Answered](const TArray<TSharedRef<FVaCuusElementBoundsQuery>>& Queries)
	{
		for (int32 Frame = 0; Frame < 10; ++Frame)
		{
			if (Queries.FindByPredicate([&Answered](const TSharedRef<FVaCuusElementBoundsQuery>& Query)
					{ return !Answered(*Query); }) == nullptr)
			{
				return;
			}
			RunFrames(*UIThread, 1);
		}
	};

	// IN TWO STEPS, AND THE ORDER IS THE CONTRACT: an answer reads the tree at the END of the
	// frame that drained the query, so a hide drained in that same frame is already in it. The
	// first run of this test enqueued the hide beside the first queries and was green alone and
	// red in the full suite -- whether the UI thread woke between the enqueues decided it.
	const TSharedRef<FVaCuusElementBoundsQuery> Button = Ask(TEXT("btn"));
	const TSharedRef<FVaCuusElementBoundsQuery> Missing = Ask(TEXT("nope"));
	WaitFor({Button, Missing});

	UIThread->EnqueueSetVisible(ViewId, /*bVisible=*/false);
	const TSharedRef<FVaCuusElementBoundsQuery> Hidden = Ask(TEXT("btn"));
	WaitFor({Hidden});

	if (TestTrue(TEXT("the button query was answered"), Answered(*Button))
		&& TestTrue(TEXT("and found it"), Button->State.load() == FVaCuusElementBoundsQuery::EState::Found))
	{
		TestTrue(TEXT("its border box, in view pixels"), Button->Rect == FIntRect(20, 20, 120, 60));
		TestTrue(TEXT("visible while the document is shown"), Button->bVisible);
	}
	TestTrue(TEXT("an id nothing carries is answered Missing, not left pending"),
		Missing->State.load() == FVaCuusElementBoundsQuery::EState::Missing);
	if (TestTrue(TEXT("the query after the hide was answered Found"),
			Hidden->State.load() == FVaCuusElementBoundsQuery::EState::Found))
	{
		TestFalse(TEXT("and reported NOT visible, so a driver can refuse to click it"), Hidden->bVisible);
	}

	UIThread->EnqueueRemoveView(ViewId);
	RunFrames(*UIThread, 1);
	return true;
}

/**
 * bTabEntersFocus IS RmlUi's TAB REACHABILITY, not a by-product of the pointer-coverage
 * walk (bead VaCuus-akj.6.36).
 *
 * The header used to call the flag "EXACTLY RmlUi's OWN PRECONDITIONS" while it was
 * accumulated inside the interactive-rect DFS, which prunes on `vacuus-passthrough` and on
 * an empty child clip -- neither of which appears anywhere in ElementDocument's focus
 * search -- and which descends into non-DOM children, which that search cannot reach. So it
 * could answer no when RmlUi says yes (the press then double-acts: consumed by the game AND
 * moving UI focus) and yes when RmlUi says no (the press is eaten for nothing).
 *
 * Each case below is one document, in the state the flag is defined for. Together they pin
 * both directions: the two prunes RmlUi does NOT do, the two it DOES, the non-DOM
 * over-report, and a positive and negative control so a stuck answer cannot pass.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusTabReachTest, "VaCuus.Input.TabReach",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusTabReachTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusSnapshotTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no worker thread to drive"));
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

	const FIntPoint ViewSize(400, 300);
	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();

	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MakeUnique<FProbeHost>(), ViewSize, Status);

	uint64 LoadSerial = 0;
	for (const FTabReachCase& Case : GTabReachCases)
	{
		++LoadSerial;
		UIThread->EnqueueLoadDocumentFromMemory(ViewId, Case.Rml, LoadSerial);

		// Two frames: the first drains the load and snapshots the fresh layout, the second
		// makes sure the answer is the steady state and not a first-frame artefact.
		if (!TestTrue(FString::Printf(TEXT("UI frames ran for '%s'"), Case.Name), RunFrames(*UIThread, 2)))
		{
			return false;
		}

		if (!TestTrue(FString::Printf(TEXT("'%s' loaded"), Case.Name),
				Status->LoadCompletedSerial.load(std::memory_order_acquire) == LoadSerial &&
					Status->LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded)))
		{
			return false;
		}

		const FVaCuusInteractiveSnapshot& Snapshot = Status->AcquireSnapshot();

		// The precondition, asserted rather than assumed: if Show() had focused something
		// INSIDE the document, bTabEntersFocus would be false for a reason that has nothing
		// to do with what this test is about, and every negative case would pass by accident.
		if (!TestFalse(FString::Printf(TEXT("'%s': the document itself holds focus"), Case.Name),
				Snapshot.bWantsKeyboardFocus))
		{
			return false;
		}

		TestEqual(FString::Printf(TEXT("Tab enters '%s'"), Case.Name),
			Snapshot.bTabEntersFocus, Case.bExpectTabEnters);
	}

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
