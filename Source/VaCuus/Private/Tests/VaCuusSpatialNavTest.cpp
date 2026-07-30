// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusInputEvent.h"
#include "VaCuusInteractiveSnapshot.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "HAL/PlatformProcess.h"
#include "InputCoreTypes.h"

#include <RmlUi/Core.h>

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusSpatialNavTest
{
/**
 * A document host that reports where RmlUi's focus went and whether the checkbox
 * was clicked.
 *
 * WHY A PROBE HOST: spatial navigation lives entirely inside
 * ElementDocument::ProcessDefaultAction, so proving it needs a live Rml::Context
 * with a laid-out document and a real focus chain -- and nothing else. No RHI, no
 * viewport, no pad.
 *
 * THE ONE PRODUCTION DETAIL IT MIRRORS DELIBERATELY: Show(ModalFlag::None,
 * FocusFlag::Document), the same call FVaCuusRmlDocumentHost makes. Without focus
 * INSIDE a document, Context::ProcessKeyDown dispatches to the context root, which
 * is not an ElementDocument and has no default action, and every arrow key is
 * silently dropped (Context.cpp:533-537).
 *
 * THREAD HAND-OFF: plain members written on the UI thread, read on the test thread
 * only after WaitForFrameCount() saw the frame counter advance -- which the UI
 * thread stores with release ordering after RunFrame() returns.
 */
class FProbeHost final : public IVaCuusDocumentHost
{
public:
	virtual bool Initialize(uint32 InViewId, const TSharedRef<FVaCuusViewStatus>& InStatus) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		ViewId = InViewId;
		Status = InStatus;
		ContextName = FString::Printf(TEXT("vacuus_spatialnav_view_%u"), ViewId);

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
			Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(*RmlSource)), "vacuus://spatialnav.rml");
		if (!NewDocument)
		{
			Report(LoadSerial, /*bSuccess=*/false);
			return;
		}

		CloseDocument();
		RmlDocument = NewDocument;

		// The production call, verbatim (FVaCuusRmlDocumentHost::AdoptDocument).
		RmlDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document);
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
		if (RmlDocument)
		{
			bVisible ? RmlDocument->Show() : RmlDocument->Hide();
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

		Context->Update();

		FVaCuusInteractiveSnapshot& Snapshot = Status->GetSnapshotWriteBuffer();
		BuildVaCuusInteractiveSnapshot(*Context, ViewSize, ++SnapshotGeneration, Snapshot);
		Status->PublishSnapshot();

		Rml::Element* const Focus = Context->GetFocusElement();
		FocusId = Focus ? FString(UTF8_TO_TCHAR(Focus->GetId().c_str())) : FString();
		bFocusIsDocument = Focus != nullptr && Focus == RmlDocument;

		// The checkbox's `checked` attribute IS the click observation: RmlUi's
		// InputTypeCheckbox::ProcessDefaultAction toggles it on EventId::Click
		// (InputTypeCheckbox.cpp:40-48), and Click() is exactly what
		// ElementDocument::ProcessDefaultAction calls for KI_RETURN on a focusable
		// element (ElementDocument.cpp:641-648). No script, no JS runtime.
		bCheckboxChecked = false;
		if (RmlDocument)
		{
			if (Rml::Element* Checkbox = RmlDocument->GetElementById("chk"))
			{
				bCheckboxChecked = Checkbox->HasAttribute("checked");
			}
		}

		Status->FramesPublished.fetch_add(1, std::memory_order_release);
	}

	//~ Post-frame observations; see the class comment for why plain members are safe.
	FString FocusId;
	bool bFocusIsDocument = false;
	bool bCheckboxChecked = false;

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
 * A 2x2 grid of focusables plus a checkbox, 400x300 view.
 *
 *      #a (20,20)     #b (140,20)                #chk (280,20)
 *      #c (20,120)    #d (140,120)
 *
 * EVERY nav-* RULE IS PER ELEMENT, and that is the point of the two rules below:
 * nav-* is registered with inherited=false (StyleSheetSpecification.cpp:378-381) and
 * ElementDocument reads it with GetLocalProperty (ElementDocument.cpp:626), which
 * sees only inline styles and the element's OWN matched definition. A single
 * `body { nav: auto; }` would therefore do nothing for the buttons.
 *
 * `body { nav: auto; }` is not redundant either -- it is the BOOTSTRAP. At load the
 * document element itself holds focus (FocusFlag::Document), so the first arrow key
 * reads the nav property off the document, and FindNextNavigationElement short-cuts
 * `current_element == this` to FindNextTabElement (ElementDocument.cpp:795-796):
 * the first press enters the grid in tab order, later ones navigate spatially.
 * Without it the first press is a silent no-op and the UI looks dead to a pad.
 *
 * DOM order is a, b, c, d, chk, which is also the tab order Tab/Shift-Tab follow.
 */
static const TCHAR* GDocument = TEXT(R"(<rml>
<head>
<style>
body { display: block; width: 100%; height: 100%; nav: auto; }
button { display: block; position: absolute; nav: auto; tab-index: auto; }
input { display: block; position: absolute; nav: auto; tab-index: auto; }
#a   { left: 20px;  top: 20px;  width: 80px; height: 40px; }
#b   { left: 140px; top: 20px;  width: 80px; height: 40px; }
#c   { left: 20px;  top: 120px; width: 80px; height: 40px; }
#d   { left: 140px; top: 120px; width: 80px; height: 40px; }
#chk { left: 280px; top: 20px;  width: 16px; height: 16px; }
</style>
</head>
<body>
	<button id="a"/>
	<button id="b"/>
	<button id="c"/>
	<button id="d"/>
	<input id="chk" type="checkbox"/>
</body>
</rml>)");

/**
 * The Back regression document: a focusable NESTED INSIDE A PLAIN WRAPPER DIV, which is
 * how real content is shaped and what the first version of Back got wrong.
 *
 * Deliberately mirrors the shipped HUD's `<div id="ability-bar">` wrapping `.slot`:
 * #wrap is an ordinary div with no tab-index, and #inner is the focusable inside it.
 * Both other documents in this suite put their focusables as direct children of <body>,
 * which is exactly why the bug survived them -- with the focusable one level down,
 * Element::Blur()'s single hop to the immediate parent lands on the wrapper, not on the
 * document, and the wrapper accepts focus because `focus` is inherited and defaults to
 * auto while Element::Focus() has no tab-index gate.
 *
 * Two levels of nesting rather than one, so the assertion also rules out a fix that just
 * hops twice.
 */
static const TCHAR* GNestedDocument = TEXT(R"(<rml>
<head>
<style>
body { display: block; width: 100%; height: 100%; nav: auto; }
div { display: block; }
button { display: block; position: absolute; nav: auto; tab-index: auto; }
#wrap  { position: absolute; left: 10px; top: 10px; width: 200px; height: 200px; }
#group { position: absolute; left: 5px;  top: 5px;  width: 180px; height: 180px; }
#inner { left: 10px; top: 10px; width: 80px; height: 40px; }
</style>
</head>
<body>
	<div id="wrap"><div id="group"><button id="inner"/></div></div>
</body>
</rml>)");

/** Sends one synthesized gamepad/keyboard press and runs the frame that consumes it. */
static bool PressKey(FVaCuusUIThread& UIThread, uint32 ViewId, const FKey& Key,
	const FVaCuusModifierState& Modifiers = FVaCuusModifierState())
{
	UIThread.EnqueueInput(ViewId, FVaCuusInputEvent::KeyEvent(/*bDown=*/true, Key, Modifiers));
	UIThread.EnqueueInput(ViewId, FVaCuusInputEvent::KeyEvent(/*bDown=*/false, Key, Modifiers));
	return RunFrames(UIThread, 1);
}
}	 // namespace VaCuusSpatialNavTest

/**
 * Gamepad-driven spatial navigation, end to end: a synthesized pad button reaches
 * Rml::Context, moves the focus RmlUi's own nav-* graph says it should, and activates
 * the focused element.
 *
 * WHAT IS SYNTHETIC AND WHAT IS NOT: only the FKeys are synthesized -- everything
 * else is the production path (the real input queue, the real FKey -> KeyIdentifier
 * map, a real Rml::Context, a real document with real RCSS). RmlUi has ZERO gamepad
 * support at this SHA, so "the pad works" can only ever mean "the embedder's
 * synthesized keys work", which is exactly what this asserts.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusSpatialNavTest, "VaCuus.Input.SpatialNav",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusSpatialNavTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusSpatialNavTest;

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

	TUniquePtr<FProbeHost> OwnedHost = MakeUnique<FProbeHost>();
	FProbeHost* Host = OwnedHost.Get();

	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), ViewSize, Status);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GDocument, /*LoadSerial=*/1);

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

	// 0. Baseline: FocusFlag::Document put focus on the document itself, which is what
	// makes ProcessDefaultAction run at all -- and, per controller decision D9, is
	// deliberately NOT "the UI wants the keyboard".
	{
		const FVaCuusInteractiveSnapshot Snapshot = Status->AcquireSnapshot();
		TestTrue(TEXT("The document holds focus after Show(FocusFlag::Document)"), Host->bFocusIsDocument);
		TestFalse(TEXT("A document-only focus does not want the keyboard"), Snapshot.bWantsKeyboardFocus);
	}

	// 1. The bootstrap press. The document holds focus and carries `nav: auto`, so
	// FindNextNavigationElement short-cuts to tab order and the first focusable in DOM
	// order takes focus.
	if (!TestTrue(TEXT("UI frame ran after DPad right (bootstrap)"),
			PressKey(*UIThread, ViewId, EKeys::Gamepad_DPad_Right)))
	{
		return false;
	}
	if (!TestEqual(TEXT("The first DPad press enters the grid at #a"), Host->FocusId, FString(TEXT("a"))))
	{
		// Everything below navigates from here; a wrong start makes the rest meaningless.
		return false;
	}

	// A real focusable element holds focus now, so the view claims the keyboard.
	{
		const FVaCuusInteractiveSnapshot Snapshot = Status->AcquireSnapshot();
		TestTrue(TEXT("A focused element makes the view want the keyboard"), Snapshot.bWantsKeyboardFocus);
	}

	// 2. Spatial right: #b is 40px to the right of #a on the same row, #d is 40px right
	// and 60px down. RmlUi multiplies the cross-axis miss by CrossAxisFactor = 10'000
	// (ElementDocument.cpp:57-68), so the same-row neighbour wins by six orders of
	// magnitude -- this is the assertion that the nav-* graph, not tab order, is running.
	TestTrue(TEXT("UI frame ran after DPad right"), PressKey(*UIThread, ViewId, EKeys::Gamepad_DPad_Right));
	TestEqual(TEXT("DPad right navigates to the element to the right"), Host->FocusId, FString(TEXT("b")));

	// 3. Spatial down: from #b the only element straight below is #d.
	TestTrue(TEXT("UI frame ran after DPad down"), PressKey(*UIThread, ViewId, EKeys::Gamepad_DPad_Down));
	TestEqual(TEXT("DPad down navigates to the element below"), Host->FocusId, FString(TEXT("d")));

	// 4. Shift-Tab, which is really an assertion about the MODIFIER MASK: RmlUi picks
	// the direction from the "shift_key" event parameter (ElementDocument.cpp:581), which
	// only exists if KM_SHIFT was OR-ed into key_modifier_state. Drop that bit and this
	// lands on #chk (forward) instead of #c (backward), while forward Tab keeps working.
	FVaCuusModifierState ShiftDown;
	ShiftDown.bShiftDown = true;
	TestTrue(TEXT("UI frame ran after Shift-Tab"), PressKey(*UIThread, ViewId, EKeys::Tab, ShiftDown));
	TestEqual(TEXT("Shift-Tab moves focus backwards in tab order"), Host->FocusId, FString(TEXT("c")));

	// 5. Forward Tab twice to reach the checkbox: c -> d -> chk in DOM order.
	TestTrue(TEXT("UI frame ran after Tab"), PressKey(*UIThread, ViewId, EKeys::Tab));
	TestEqual(TEXT("Tab moves focus forwards"), Host->FocusId, FString(TEXT("d")));
	TestTrue(TEXT("UI frame ran after the second Tab"), PressKey(*UIThread, ViewId, EKeys::Tab));
	if (!TestEqual(TEXT("Tab reaches the checkbox"), Host->FocusId, FString(TEXT("chk"))))
	{
		return false;
	}
	TestFalse(TEXT("The checkbox starts unchecked"), Host->bCheckboxChecked);

	// 6. THE ACCEPT PATH. FaceButton_Bottom maps to KI_RETURN, and RmlUi's handler
	// calls focus_node->Click() when the focus leaf has tab-index:auto
	// (ElementDocument.cpp:641-648) -- so a toggled checkbox is proof of a real click
	// event, not of a key that merely arrived.
	TestTrue(TEXT("UI frame ran after the accept button"),
		PressKey(*UIThread, ViewId, EKeys::Gamepad_FaceButton_Bottom));
	TestTrue(TEXT("The accept button clicks the focused checkbox"), Host->bCheckboxChecked);

	// And again, because a click is a toggle: this rules out "the attribute was set by
	// something other than a click".
	TestTrue(TEXT("UI frame ran after the second accept"),
		PressKey(*UIThread, ViewId, EKeys::Gamepad_FaceButton_Bottom));
	TestFalse(TEXT("A second accept clicks it again and unchecks it"), Host->bCheckboxChecked);

	// 7. BACK. FaceButton_Right is deliberately NOT in the key map -- RmlUi has no
	// identifier for "cancel" -- so the widget turns it into an event of its own, and
	// what the UI thread does with it for M2 is blur the focused element. The observable
	// consequence is the one that matters to a player: with nothing focusable focused,
	// the view stops claiming the keyboard and keys reach the game again.
	UIThread->EnqueueInput(ViewId, FVaCuusInputEvent::NavigateBack());
	if (!TestTrue(TEXT("UI frame ran after Back"), RunFrames(*UIThread, 1)))
	{
		return false;
	}
	TestTrue(TEXT("Back blurs the focused element back onto the document"), Host->bFocusIsDocument);
	{
		const FVaCuusInteractiveSnapshot Snapshot = Status->AcquireSnapshot();
		TestFalse(TEXT("After Back the view no longer wants the keyboard"), Snapshot.bWantsKeyboardFocus);
	}

	// 8. And navigation still works afterwards -- Back releases the keyboard, it does not
	// break the document. The bootstrap runs again from the document's own `nav: auto`.
	TestTrue(TEXT("UI frame ran after Back + DPad right"),
		PressKey(*UIThread, ViewId, EKeys::Gamepad_DPad_Right));
	TestEqual(TEXT("Navigation re-enters the grid after Back"), Host->FocusId, FString(TEXT("a")));

	// 9. A second Back with only the document focused is a no-op, not a slide into the
	// context root (where ProcessKeyDown has no default action and navigation would be
	// dead for good).
	UIThread->EnqueueInput(ViewId, FVaCuusInputEvent::NavigateBack());
	UIThread->EnqueueInput(ViewId, FVaCuusInputEvent::NavigateBack());
	TestTrue(TEXT("UI frame ran after two Backs"), RunFrames(*UIThread, 1));
	TestTrue(TEXT("Back leaves focus on the document rather than on the context root"), Host->bFocusIsDocument);
	TestTrue(TEXT("UI frame ran after the final DPad right"),
		PressKey(*UIThread, ViewId, EKeys::Gamepad_DPad_Right));
	TestEqual(TEXT("Navigation still works after a redundant Back"), Host->FocusId, FString(TEXT("a")));

	UIThread->EnqueueRemoveView(ViewId);
	TestTrue(TEXT("UI frames survive the removal"), RunFrames(*UIThread, 2));

	return true;
}

/**
 * Back on NESTED content, which is the shape all real content has.
 *
 * THE BUG THIS GUARDS (found in review, reachable through our own shipped HUD):
 * Element::Blur() hands focus to the IMMEDIATE PARENT only (Element.cpp:2016-2031), and
 * Element::Focus() succeeds on any element whose computed `focus` is not none
 * (Element.cpp:2003-2008) -- there is no tab-index gate, and `focus` is inherited with a
 * default of auto (StyleSheetSpecification.cpp:375). So a single blur lands on whatever
 * plain <div> happens to wrap the focusable. D9 counts that wrapper as a real focus
 * element, because it excludes only the context root and document elements, so
 * bWantsKeyboardFocus stays TRUE and the UI keeps eating the player's keys -- one Back
 * press per level of nesting before the keyboard actually comes back.
 *
 * `m1_hud.rml` nests `.slot` inside `<div id="ability-bar">`, so this was reachable in
 * the demo; VaCuus.Input.SpatialNav missed it only because its focusables are direct
 * children of <body>, where one hop happens to be enough.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusNavBackNestedTest, "VaCuus.Input.NavBackNested",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusNavBackNestedTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusSpatialNavTest;

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

	TUniquePtr<FProbeHost> OwnedHost = MakeUnique<FProbeHost>();
	FProbeHost* Host = OwnedHost.Get();

	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), ViewSize, Status);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GNestedDocument, /*LoadSerial=*/1);

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

	// Focus the nested button. Tab reaches it because it is the only focusable in the
	// document; the wrappers are skipped precisely BECAUSE they have no tab-index -- which
	// is the asymmetry the bug exploited (RmlUi will not tab TO them, but it will happily
	// blur INTO them).
	if (!TestTrue(TEXT("UI frame ran after Tab"), PressKey(*UIThread, ViewId, EKeys::Tab)))
	{
		return false;
	}
	if (!TestEqual(TEXT("Tab focuses the nested button"), Host->FocusId, FString(TEXT("inner"))))
	{
		return false;
	}
	{
		const FVaCuusInteractiveSnapshot Snapshot = Status->AcquireSnapshot();
		TestTrue(TEXT("A nested focused element makes the view want the keyboard"),
			Snapshot.bWantsKeyboardFocus);
	}

	// ONE Back, and the keyboard must be back with the game. The two assertions are the
	// two halves of the bug: focus is on the DOCUMENT (not on '#group', which is where a
	// single blur would have left it), and the published snapshot says the view no longer
	// wants the keyboard.
	UIThread->EnqueueInput(ViewId, FVaCuusInputEvent::NavigateBack());
	if (!TestTrue(TEXT("UI frame ran after Back"), RunFrames(*UIThread, 1)))
	{
		return false;
	}

	AddInfo(FString::Printf(TEXT("Focus after one Back: '%s' (document: %s)"),
		*Host->FocusId, Host->bFocusIsDocument ? TEXT("yes") : TEXT("no")));

	TestEqual(TEXT("ONE Back leaves no wrapper element focused"), Host->FocusId, FString());
	TestTrue(TEXT("ONE Back puts focus on the document, however deeply nested the element was"),
		Host->bFocusIsDocument);
	{
		const FVaCuusInteractiveSnapshot Snapshot = Status->AcquireSnapshot();
		TestFalse(TEXT("ONE Back is enough for the view to stop wanting the keyboard"),
			Snapshot.bWantsKeyboardFocus);
	}

	// And the document is still navigable: Back releases the keyboard, it does not strand
	// focus somewhere ProcessDefaultAction cannot run from.
	TestTrue(TEXT("UI frame ran after Back + DPad right"),
		PressKey(*UIThread, ViewId, EKeys::Gamepad_DPad_Right));
	TestEqual(TEXT("Navigation re-enters the nested content after Back"),
		Host->FocusId, FString(TEXT("inner")));

	UIThread->EnqueueRemoveView(ViewId);
	TestTrue(TEXT("UI frames survive the removal"), RunFrames(*UIThread, 2));

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
