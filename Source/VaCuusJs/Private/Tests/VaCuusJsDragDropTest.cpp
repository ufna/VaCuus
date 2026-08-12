// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

/*
 * THE DRAG'N'DROP DEMO, END-TO-END (bead VaCuus-iim): the SHIPPED
 * Content/DevUI/drag_demo.rml -- not a mirror fixture -- loaded through the
 * production LoadDocumentFile command so its <link> and <script src> resolve
 * through the DevUI roots exactly as vacuus.DragDemo resolves them, then
 * driven through the production input path (EnqueueInput -> DrainInput ->
 * Context::Process*). What is being proven:
 *
 *  - RmlUi's drag machinery works through OUR event synthesis: dragstart on
 *    the first held move (no threshold, Context.cpp:1290), dragover/dragdrop
 *    on the TARGET, the release order dragdrop -> dragout -> dragend
 *    (Context.cpp:760-775), a cancelled drag still delivering dragend;
 *  - the demo's JS policy holds: typed slots accept and refuse correctly, the
 *    move happens (in dragend -- reparenting in dragdrop would cancel the drag
 *    through OnElementDetach, Context.cpp:1150-1159 -- the demo's header has
 *    the full argument), highlights come and go;
 *  - the SAME drag works by TOUCH (TouchStart/Move/End) -- the suite-wide gap
 *    the VaCuus-61d family named: RmlUi synthesises button 0 from touch verbs
 *    (Context.cpp:919), so one fixture covers both pointers;
 *  - listeners ride the node: an item moved by a drop is draggable again.
 *
 * COORDINATES are the fixed-pixel layout drag_demo.rcss commits to; the sheet
 * documents the arithmetic and this file self-checks it with a hover probe
 * before the first drag, so a re-layout fails by NAMING the moved coordinate
 * instead of by four beats of inexplicable silence.
 */

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "VaCuusJsDomTestRig.h"

#include "VaCuusContentPaths.h"
#include "VaCuusInputEvent.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsDragDropTest, "VaCuus.Js.DragDrop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace VaCuusJsDragDropTest
{
using namespace VaCuusJsDomTest;

/**
 * FJsDocProbeHost's seam calls (OnDocumentReady is what compiles and runs
 * drag_demo.js against this view's context) PLUS the RefHud probe's real file
 * load, because the shipped document is the fixture. FJsDocProbeHost itself is
 * final and memory-only -- this is the same shape one module over.
 */
class FDragProbeHost final : public FVaCuusTestDocumentHost
{
public:
	explicit FDragProbeHost(const TCHAR* InContextPrefix)
		: FVaCuusTestDocumentHost(InContextPrefix, "vacuus://drag_demo_test.rml", Rml::FocusFlag::Document)
	{
	}

	/** The production FVaCuusRmlDocumentHost::LoadDocumentFromFile shape: through
	 *  Rml::GetFileInterface(), so rcss and script src resolve against the DevUI roots. */
	virtual void LoadDocumentFromFile(const FString& VfsPath, uint64 LoadSerial) override
	{
		check(FVaCuusUIThread::IsInUIThread());
		if (Context == nullptr)
		{
			ReportLoadResult(LoadSerial, /*bSuccess=*/false);
			return;
		}
		AdoptDocument(Context->LoadDocument(Rml::String(TCHAR_TO_UTF8(*VfsPath))), LoadSerial);
	}

	virtual void OnDocumentAdopted() override
	{
		if (IVaCuusScriptHost* ScriptHost = FVaCuusUIThread::GetActiveScriptHost())
		{
			ScriptHost->OnDocumentReady(ViewId, RmlDocument);
		}
	}

	virtual void OnDocumentClosing() override
	{
		if (IVaCuusScriptHost* ScriptHost = FVaCuusUIThread::GetActiveScriptHost())
		{
			ScriptHost->OnDocumentClosing(ViewId);
		}
	}

	virtual void SetVisible(bool /*bVisible*/) override {}

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());
		Context->Update();
		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}
};

//~ Slot centres from drag_demo.rcss's committed arithmetic (its header spells
//~ out the derivation): stash slot (col,row) centre = (83 + col*72, 129 + row*72),
//~ equip slot i centre = (463, 129 + i*80).
static const FIntPoint GItRifle(83, 129);	  // st-0, weapon
static const FIntPoint GItPlate(155, 129);	  // st-1, armor
static const FIntPoint GItScope(227, 129);	  // st-2, gadget
static const FIntPoint GStEmpty(83, 201);	  // st-4, empty
static const FIntPoint GEqWeapon(463, 129);
static const FIntPoint GEqArmor(463, 209);
static const FIntPoint GGap(390, 129);		  // between the panels: body only
static const FIntPoint GNowhere(700, 240);	  // over no panel at all
}	 // namespace VaCuusJsDragDropTest

bool FVaCuusJsDragDropTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsDragDropTest;

	// The fixture is the shipped file; without a DevUI root on disk (a cooked
	// layout) there is nothing honest to test against.
	if (VaCuusContentPaths::ResolveExistingDocument(TEXT("drag_demo.rml")).IsEmpty())
	{
		AddInfo(TEXT("Skipped: drag_demo.rml does not resolve through the DevUI roots"));
		return true;
	}

	FDomTestRig Rig;
	if (const FDomTestRig::EBoot Boot = Rig.Boot(*this); Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	// A view big enough for the demo's fixed layout, loading the REAL document.
	FDragProbeHost* Probe = nullptr;
	TUniquePtr<FDragProbeHost> Owned = MakeUnique<FDragProbeHost>(TEXT("vacuus_dragdrop"));
	Probe = Owned.Get();
	const uint32 ViewId = Rig.Thread->AllocateViewId();
	Rig.Thread->EnqueueAddView(ViewId, MoveTemp(Owned), FIntPoint(800, 480), MakeShared<FVaCuusViewStatus>());
	Rig.Thread->EnqueueLoadDocumentFile(ViewId, TEXT("drag_demo.rml"), /*LoadSerial=*/1);
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}
	if (!TestEqual(TEXT("drag_demo.js booted against the view"), Rig.Eval(ViewId, "String(dragLog.length)"),
			FString(TEXT("0"))))
	{
		return false;
	}

	const FVaCuusModifierState NoMods;
	const auto Move = [&](FIntPoint P) { Rig.Thread->EnqueueInput(ViewId, FVaCuusInputEvent::MouseMove(P, NoMods)); };
	const auto Press = [&](FIntPoint P)
	{ Rig.Thread->EnqueueInput(ViewId, FVaCuusInputEvent::MouseButton(true, P, EKeys::LeftMouseButton, NoMods)); };
	const auto Release = [&](FIntPoint P)
	{ Rig.Thread->EnqueueInput(ViewId, FVaCuusInputEvent::MouseButton(false, P, EKeys::LeftMouseButton, NoMods)); };

	// --- 0. THE COORDINATE SELF-CHECK: before any drag, prove the rcss
	// arithmetic this file rests on still puts it-rifle under (83,129). A
	// re-layout fails HERE, by name, not in beat 1's event log.
	Rig.Eval(ViewId,
		"globalThis.overLog = [];"
		"globalThis.overProbe = function(ev){ overLog.push(ev.currentTarget.id); };"
		"document.getElementById('it-rifle').addEventListener('mouseover', overProbe);"
		"'ok'");
	Move(GItRifle);
	TestEqual(TEXT("the committed layout arithmetic holds: (83,129) hovers it-rifle"),
		Rig.Eval(ViewId,
			"document.getElementById('it-rifle').removeEventListener('mouseover', overProbe);"
			"overLog.join('|')"),
		FString(TEXT("it-rifle")));

	// --- 1. THE ACCEPTED DROP, mouse: it-rifle (weapon) -> eq-weapon (typed
	// weapon, empty). The expected order IS the assertion: dragstart on the
	// first held move; NO dragover for the gap (only slots listen); dragover on
	// entering the target; then RmlUi's release sequence dragdrop -> dragout ->
	// dragend, target twice, source last.
	Press(GItRifle);
	Move(GGap);
	Move(GEqWeapon);
	TestEqual(TEXT("mid-drag: the empty matching target shows drop-ok"),
		Rig.Eval(ViewId, "String(document.getElementById('eq-weapon').classList.contains('drop-ok'))"),
		FString(TEXT("true")));
	Release(GEqWeapon);
	TestEqual(TEXT("accepted drop: the full event order, target twice, source last"),
		Rig.Eval(ViewId, "globalThis.takeLog = dragLog.join('|'); dragLog.length = 0; takeLog"),
		FString(TEXT("dragstart:it-rifle|dragover:eq-weapon|dragdrop:eq-weapon|dragout:eq-weapon|dragend:it-rifle")));
	TestEqual(TEXT("the item MOVED: eq-weapon holds it, st-0 is empty, the status line names it"),
		Rig.Eval(ViewId,
			"[document.getElementById('eq-weapon').querySelector('.item').id,"
			" String(document.getElementById('st-0').querySelector('.item')),"
			" String(globalThis.lastStatus)].join('~')"),
		FString(TEXT("it-rifle~null~MOVED IT-RIFLE -> EQ-WEAPON")));

	// --- 2. THE REFUSED DROP, mouse: it-plate (armor) onto eq-weapon, which is
	// now OCCUPIED. The drop lands on the occupying ITEM and reaches the slot
	// by bubbling -- and the post-drop dragout goes to that item too, where the
	// slot's target filter drops it: no dragout in this log, BY DESIGN, and the
	// demo's dragend-clears-everything rule is what un-highlights instead.
	Press(GItPlate);
	Move(GGap);
	Move(GEqWeapon);
	TestEqual(TEXT("mid-drag: the occupied slot shows drop-bad"),
		Rig.Eval(ViewId, "String(document.getElementById('eq-weapon').classList.contains('drop-bad'))"),
		FString(TEXT("true")));
	Release(GEqWeapon);
	TestEqual(TEXT("refused drop: dragdrop still reaches the slot by bubbling; no slot dragout"),
		Rig.Eval(ViewId, "globalThis.takeLog = dragLog.join('|'); dragLog.length = 0; takeLog"),
		FString(TEXT("dragstart:it-plate|dragover:eq-weapon|dragdrop:eq-weapon|dragend:it-plate")));
	TestEqual(TEXT("nothing moved, the highlight is gone, the refusal is named"),
		Rig.Eval(ViewId,
			"[document.getElementById('st-1').querySelector('.item').id,"
			" document.getElementById('eq-weapon').querySelector('.item').id,"
			" String(document.getElementById('eq-weapon').classList.contains('drop-bad')),"
			" String(globalThis.lastStatus)].join('~')"),
		FString(TEXT("it-plate~it-rifle~false~REFUSED IT-PLATE x EQ-WEAPON")));

	// --- 3. THE CANCELLED DRAG, mouse: it-scope released over nothing. dragend
	// STILL fires (Context.cpp:772 is unconditional on the release path) -- the
	// contract the demo's cleanup rests on.
	Press(GItScope);
	Move(GNowhere);
	Release(GNowhere);
	TestEqual(TEXT("cancelled drag: dragstart and dragend only"),
		Rig.Eval(ViewId, "globalThis.takeLog = dragLog.join('|'); dragLog.length = 0; takeLog"),
		FString(TEXT("dragstart:it-scope|dragend:it-scope")));
	TestEqual(TEXT("the item stayed home and says so"),
		Rig.Eval(ViewId,
			"[document.getElementById('st-2').querySelector('.item').id,"
			" String(globalThis.lastStatus)].join('~')"),
		FString(TEXT("it-scope~RETURNED IT-SCOPE")));

	// --- 4. THE SAME DRAG BY TOUCH: it-plate -> eq-armor (typed armor, empty).
	// TouchStart synthesises the move + button-0 press (Context.cpp:916-919),
	// TouchMove the moves, TouchEnd the release -- so the log must be
	// byte-identical in shape to beat 1's. This is the pointer the suite had
	// never sent through a drag (the VaCuus-61d coverage gap).
	const uint64 TouchId = FVaCuusInputEvent::MakeTouchId(0, 0);
	Rig.Thread->EnqueueInput(ViewId, FVaCuusInputEvent::Touch(EVaCuusInputEventKind::TouchStart, TouchId, GItPlate, NoMods));
	Rig.Thread->EnqueueInput(ViewId, FVaCuusInputEvent::Touch(EVaCuusInputEventKind::TouchMove, TouchId, GGap, NoMods));
	Rig.Thread->EnqueueInput(ViewId, FVaCuusInputEvent::Touch(EVaCuusInputEventKind::TouchMove, TouchId, GEqArmor, NoMods));
	Rig.Thread->EnqueueInput(ViewId, FVaCuusInputEvent::Touch(EVaCuusInputEventKind::TouchEnd, TouchId, GEqArmor, NoMods));
	TestEqual(TEXT("touch drag: the same event order as the mouse"),
		Rig.Eval(ViewId, "globalThis.takeLog = dragLog.join('|'); dragLog.length = 0; takeLog"),
		FString(TEXT("dragstart:it-plate|dragover:eq-armor|dragdrop:eq-armor|dragout:eq-armor|dragend:it-plate")));
	TestEqual(TEXT("touch drag moved the item"),
		Rig.Eval(ViewId, "document.getElementById('eq-armor').querySelector('.item').id"),
		FString(TEXT("it-plate")));

	// --- 5. LISTENERS RIDE THE NODE: the item equipped in beat 1 is dragged
	// again, back to an empty stash slot -- the reparented element's own
	// dragstart/dragend listeners must still be attached.
	Press(GEqWeapon);
	Move(GGap);
	Move(GStEmpty);
	Release(GStEmpty);
	TestEqual(TEXT("an equipped item drags back out"),
		Rig.Eval(ViewId, "globalThis.takeLog = dragLog.join('|'); dragLog.length = 0; takeLog"),
		FString(TEXT("dragstart:it-rifle|dragover:st-4|dragdrop:st-4|dragout:st-4|dragend:it-rifle")));
	TestEqual(TEXT("the round trip landed"),
		Rig.Eval(ViewId,
			"[document.getElementById('st-4').querySelector('.item').id,"
			" String(document.getElementById('eq-weapon').querySelector('.item'))].join('~')"),
		FString(TEXT("it-rifle~null")));

	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
