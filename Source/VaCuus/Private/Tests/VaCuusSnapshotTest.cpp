// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusInteractiveSnapshot.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "HAL/PlatformProcess.h"

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
 * or PIE. So this creates a context against the engine's global (null) render
 * interface, updates it, builds the snapshot and publishes it through the same
 * FVaCuusViewStatus the game thread would read. Same pattern as
 * VaCuusMultiViewTest's FProbeHost, minus the recorder.
 */
class FProbeHost final : public IVaCuusDocumentHost
{
public:
	virtual bool Initialize(uint32 InViewId, const TSharedRef<FVaCuusViewStatus>& InStatus) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		ViewId = InViewId;
		Status = InStatus;
		ContextName = FString::Printf(TEXT("vacuus_snapshot_view_%u"), ViewId);

		// Null render interface: FVaCuusEngine installs one globally when nobody
		// supplied a real one, and Rml::CreateContext falls back to it.
		Context = Rml::CreateContext(Rml::String(TCHAR_TO_UTF8(*ContextName)), Rml::Vector2i(1, 1));
		return Context != nullptr;
	}

	virtual void Shutdown() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		if (RmlDocument)
		{
			RmlDocument->Close();
			RmlDocument = nullptr;
		}
		if (Context)
		{
			Rml::RemoveContext(Rml::String(TCHAR_TO_UTF8(*ContextName)));
			Context = nullptr;
		}
	}

	virtual void SetViewSize(FIntPoint InViewSize) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		if (InViewSize == ViewSize || InViewSize.X <= 0 || InViewSize.Y <= 0)
		{
			return;
		}

		ViewSize = InViewSize;
		if (Context)
		{
			Context->SetDimensions(Rml::Vector2i(ViewSize.X, ViewSize.Y));
		}
	}

	virtual void LoadDocumentFromFile(const FString& VfsPath, uint64 LoadSerial) override
	{
		// Not exercised: this test only ever loads from memory.
		Report(LoadSerial, /*bSuccess=*/false);
	}

	virtual void LoadDocumentFromMemory(const FString& RmlSource, uint64 LoadSerial) override
	{
		check(FVaCuusUIThread::IsInUIThread());
		if (!Context)
		{
			Report(LoadSerial, /*bSuccess=*/false);
			return;
		}

		Rml::ElementDocument* NewDocument =
			Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(*RmlSource)), "vacuus://snapshot.rml");
		if (!NewDocument)
		{
			Report(LoadSerial, /*bSuccess=*/false);
			return;
		}

		CloseDocument();
		RmlDocument = NewDocument;
		RmlDocument->Show();
		Report(LoadSerial, /*bSuccess=*/true);
	}

	virtual void CloseDocument() override
	{
		check(FVaCuusUIThread::IsInUIThread());
		if (RmlDocument)
		{
			RmlDocument->Close();
			RmlDocument = nullptr;
		}
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

	virtual bool HasView() const override
	{
		check(FVaCuusUIThread::IsInUIThread());
		return Context != nullptr && RmlDocument != nullptr && ViewSize.X > 0 && ViewSize.Y > 0;
	}

	virtual Rml::Context* GetContext() const override
	{
		check(FVaCuusUIThread::IsInUIThread());
		return Context;
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
		Status->FramesPublished.fetch_add(1, std::memory_order_release);
	}

	/** Test-thread readable: how big the walked tree was on the last frame. */
	std::atomic<int32> LastElementsVisited{0};

private:
	void Report(uint64 LoadSerial, bool bSuccess)
	{
		if (Status.IsValid() && LoadSerial != 0)
		{
			Status->LoadResult.store(
				static_cast<uint8>(bSuccess ? EVaCuusLoadResult::Succeeded : EVaCuusLoadResult::Failed),
				std::memory_order_relaxed);
			Status->LoadCompletedSerial.store(LoadSerial, std::memory_order_release);
		}
	}

	TSharedPtr<FVaCuusViewStatus> Status;
	FString ContextName;
	Rml::Context* Context = nullptr;
	Rml::ElementDocument* RmlDocument = nullptr;
	FIntPoint ViewSize = FIntPoint::ZeroValue;
	uint32 ViewId = 0;
	uint64 SnapshotGeneration = 0;
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
 *
 * The last five are controller decision D11's assertions: interactive and focusable
 * are DIFFERENT properties of the same rect. #marked proves the direction that
 * matters -- an opt-in marker makes a div take clicks without making it take the
 * keyboard -- and #field proves the other one without any RCSS at all, because
 * RmlUi's form controls set `tab-index: auto` on themselves
 * (ElementFormControl.cpp:8). #noFocus is the `focus: none` subtree prune, which is
 * the one focusability rule that is inherited rather than per element
 * (ElementDocument.cpp:34-38).
 *
 * The last two are the case guard: RmlUi lowercases TAG names while parsing but
 * leaves attribute names exactly as the author typed them
 * (BaseXMLParser.cpp:306-345) and HasAttribute is a case-sensitive find
 * (Element.cpp:861-864), so a lowercase-only marker lookup silently ignores both
 * of them -- which for the opt-OUT means eating clicks meant for the game.
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
</style>
</head>
<body>
	<button id="btn"/>
	<div id="ghost"><button id="ghost-kid"/></div>
	<div id="pass" vacuus-passthrough><button id="pass-kid"/></div>
	<div id="clip"><button id="clipped"/></div>
	<div id="marked" vacuus-interactive/>
	<div id="pass-mixed" vacuus-PassThrough/>
	<div id="mark-mixed" VACUUS-INTERACTIVE/>
	<input id="field" type="text"/>
	<div id="noFocus" vacuus-interactive><button id="noFocusKid"/></div>
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
	// (Context.cpp:1442 checks pointer-events only after descending) and so must
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
	TestFalse(TEXT("A hidden document covers nothing"), Hidden.Contains(FIntPoint(70, 40)));
	TestFalse(TEXT("A hidden document is focusable nowhere"), Hidden.IsFocusableAt(FIntPoint(70, 40)));

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
