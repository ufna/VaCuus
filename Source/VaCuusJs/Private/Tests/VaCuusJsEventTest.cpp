// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "VaCuusJsDomTestRig.h"
#include "VaCuusJsEventListener.h"

#include "VaCuusInputEvent.h"

/*
 * EVENTS OVER THE FACADE (M4 Task 5 spec 3.9/2(g) / plan 5.2): the same
 * boot-the-real-UI-thread rig as the DOM tests (VaCuusJsDomTestRig.h), because
 * dispatch IS RmlUi -- capture/bubble ordering, the detach guarantees and the
 * three death orders only mean anything against the real EventDispatcher and
 * the real input path.
 *
 * OBSERVABLES (the house rule -- an invariant with no observable rots):
 *  - FVaCuusJsRuntime::GetNumListenerRefs() -- the JS-function-ref gauge, the
 *    three-death-orders proof;
 *  - FVaCuusJsEventListener::GetNumLiveShells() -- the C++ shells, which
 *    outlive the refs by design in death order (3);
 *  - FVaCuusJsViewContext::GetLiveListenerCount() -- the per-view registry.
 */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsEventDispatchTest, "VaCuus.Js.Events.Dispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsEventInputPathTest, "VaCuus.Js.Events.InputPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsEventDeathOrdersTest, "VaCuus.Js.Events.DeathOrders",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsEventOnAttributeTest, "VaCuus.Js.Events.OnAttribute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * The listener surface against RmlUi's real dispatcher: capture-before-target-
 * before-bubble with DOM phase numbers, target/currentTarget/this identity
 * through the wrapper cache, the DOM duplicate-registration rule, the pinned
 * removeEventListener capture-flag exactness, stopPropagation /
 * stopImmediatePropagation / preventDefault (its stopPropagation mapping
 * observed through dispatchEvent's return), a throwing listener that does not
 * skip its sibling, and the dispatchEvent parameter round-trip through a
 * custom (auto-registered) event type.
 */
bool FVaCuusJsEventDispatchTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsDomTest;

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	static const TCHAR* GDocument = TEXT(R"(<rml>
<head><style>body { display: block; } div { display: block; }</style></head>
<body><div id="wrap"><div id="kid"/></div><div id="lone"/></body>
</rml>)");

	FDomProbeHost* Probe = nullptr;
	const uint32 ViewId = Rig.AddViewWithDocument(Probe, TEXT("vacuus_jsevt_dispatch"), GDocument);

	bool bBound = false;
	Rig.RunOnUI([&bBound, Probe, ViewId]()
		{
			Rml::ElementDocument* Document = Probe->GetDocument();
			bBound = Document != nullptr;
			FWrappedDomHost::Inner->BindDocumentForTest(ViewId, Document);
		});
	if (!TestTrue(TEXT("the document loaded and bound"), bBound))
	{
		return false;
	}

	// A: ordering and identity. The capture listener on the ancestor fires
	// before the target's listener, the bubble one after (the collected chain's
	// stable sort, EventDispatcher.cpp:101-139); phases carry DOM's numbering
	// (1/2/3); target === the getElementById wrapper (cache identity), and
	// `this` inside a handler is the currentTarget.
	TestEqual(TEXT("capture -> target -> bubble, with identity"),
		Rig.Eval(ViewId,
			"globalThis.log = [];"
			"globalThis.wrap = document.getElementById('wrap');"
			"globalThis.kid = document.getElementById('kid');"
			"wrap.addEventListener('click', function(ev){ log.push('cap:' + ev.eventPhase); }, true);"
			"wrap.addEventListener('click', function(ev){ log.push('bub:' + ev.eventPhase); });"
			"kid.addEventListener('click', function(ev){"
			"  log.push(['tgt', ev.eventPhase, ev.type, ev.target === kid, ev.currentTarget === kid,"
			"            this === kid].join(':'));"
			"});"
			"kid.dispatchEvent('click');"
			"log.join('|')"),
		FString(TEXT("cap:1|tgt:2:click:true:true:true|bub:3")));

	// B: the DOM duplicate rule and the PINNED removeEventListener semantics --
	// the same (type, fn, capture) removes, a different capture flag does NOT
	// (the registry key and RmlUi's entry identity both carry the flag,
	// EventDispatcher.cpp:54-58).
	TestEqual(TEXT("duplicates ignored; removal is capture-exact (pinned)"),
		Rig.Eval(ViewId,
			"globalThis.hits = 0;"
			"globalThis.fnA = function(){ hits++; };"
			"globalThis.lone = document.getElementById('lone');"
			"lone.addEventListener('click', fnA, true);"
			"lone.addEventListener('click', fnA, true);"	  // duplicate: one entry, one fire
			"lone.dispatchEvent('click');"					  // 1
			"lone.removeEventListener('click', fnA, false);"  // WRONG phase: removes nothing
			"lone.dispatchEvent('click');"					  // 2
			"lone.removeEventListener('click', fnA, true);"	  // exact: removes
			"lone.dispatchEvent('click');"					  // still 2
			"String(hits)"),
		FString(TEXT("2")));

	// C: the stop family. mousedown/mouseup/dblclick are interruptible+bubbles
	// in the built-in table (EventSpecification.cpp:13/22/24), so each stop has
	// something real to stop; preventDefault is observed as its documented
	// stopPropagation mapping -- dispatchEvent returns RmlUi's "still
	// propagating", false once stopped.
	TestEqual(TEXT("stopPropagation / stopImmediatePropagation / preventDefault"),
		Rig.Eval(ViewId,
			"globalThis.log = [];"
			"kid.addEventListener('mousedown', function(ev){ log.push('k1'); ev.stopPropagation(); });"
			"wrap.addEventListener('mousedown', function(){ log.push('w-md'); });"
			"kid.dispatchEvent('mousedown');"
			"log.push('--');"
			"kid.addEventListener('mouseup', function(ev){ log.push('m1'); ev.stopImmediatePropagation(); });"
			"kid.addEventListener('mouseup', function(){ log.push('m2'); });"
			"wrap.addEventListener('mouseup', function(){ log.push('w-mu'); });"
			"kid.dispatchEvent('mouseup');"
			"log.push('--');"
			"kid.addEventListener('dblclick', function(ev){ ev.preventDefault(); });"
			"wrap.addEventListener('dblclick', function(){ log.push('w-dc'); });"
			"log.push('r:' + kid.dispatchEvent('dblclick'));"
			"log.join('|')"),
		FString(TEXT("k1|--|m1|--|r:false")));

	// D: sibling isolation under a throw. The throw is consumed at the listener
	// boundary (counted, logged); the dispatcher's loop never sees it and the
	// sibling runs.
	AddExpectedMessagePlain(TEXT("Error: boom-sibling"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);
	TestEqual(TEXT("a throwing listener does not skip its sibling"),
		Rig.Eval(ViewId,
			"globalThis.log = [];"
			"lone.addEventListener('keydown', function(){ throw new Error('boom-sibling'); });"
			"lone.addEventListener('keydown', function(){ log.push('sibling-ran'); });"
			"lone.dispatchEvent('keydown');"
			"log.join('|')"),
		FString(TEXT("sibling-ran")));

	// E: dispatchEvent round-trip. The custom type auto-registers with
	// interruptible=true, bubbles=true (EventSpecification.cpp:125-133);
	// string/number/bool parameters survive the JS -> Variant -> JS trip via
	// `ev.params`; a nested object is skipped by the documented conversion.
	TestEqual(TEXT("dispatchEvent round-trips params through a custom type"),
		Rig.Eval(ViewId,
			"globalThis.seen = null;"
			"lone.addEventListener('vacuus_custom', function(ev){"
			"  seen = [ev.type, ev.params.damage, ev.params.who, ev.params.crit,"
			"          typeof ev.params.skipped].join('|');"
			"});"
			"const ok = lone.dispatchEvent('vacuus_custom', {damage: 12.5, who: 'bot', crit: true,"
			"                                                skipped: {nested: 1}});"
			"seen + '~' + ok"),
		FString(TEXT("vacuus_custom|12.5|bot|true|undefined~true")));

	// F: a stashed event object goes DEAD, not dangling: past its dispatch the
	// stop methods no-op while the data properties keep answering.
	TestEqual(TEXT("a stashed event object is dead but harmless"),
		Rig.Eval(ViewId,
			"globalThis.stash = null;"
			"lone.addEventListener('vacuus_stash', function(ev){ stash = ev; });"
			"lone.dispatchEvent('vacuus_stash');"
			"stash.stopPropagation();"	  // no-op, no crash
			"stash.preventDefault();"	  // no-op, no crash
			"[stash.type, stash.target === lone].join('|')"),
		FString(TEXT("vacuus_stash|true")));

	TestEqual(TEXT("exactly the one deliberate error"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(1));
	return true;
}

/**
 * Spec 3.9's key/gamepad discharge (plan 5.5): NO new machinery -- a mouse
 * click, keydown/keyup with modifiers, textinput and a synthesized GAMEPAD
 * press all reach JS listeners as ordinary RmlUi events through the M2
 * production input path (EnqueueInput -> DrainInput -> Context::Process*),
 * with the parameters mapped per BuildEventObject's alias table. The gamepad
 * proof mirrors the SpatialNav test's premise: RmlUi has zero gamepad support,
 * the M2 translation turns pad keys into arrow KeyIdentifiers
 * (VaCuusInputMap.cpp:126-150), so DPad_Down arrives as keydown KI_DOWN.
 */
bool FVaCuusJsEventInputPathTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsDomTest;

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	// The routing test's shape: an absolutely positioned target with real
	// geometry, so hover -> mousedown -> mouseup -> click resolves at (30,30).
	static const TCHAR* GDocument = TEXT(R"(<rml>
<head><style>
body { display: block; width: 100%; height: 100%; }
div { display: block; position: absolute; }
#btn { left: 20px; top: 20px; width: 100px; height: 40px; }
</style></head>
<body><div id="btn"/></body>
</rml>)");

	FDomProbeHost* Probe = nullptr;
	const uint32 ViewId = Rig.AddViewWithDocument(Probe, TEXT("vacuus_jsevt_input"), GDocument);

	bool bBound = false;
	Rig.RunOnUI([&bBound, Probe, ViewId]()
		{
			Rml::ElementDocument* Document = Probe->GetDocument();
			bBound = Document != nullptr;
			FWrappedDomHost::Inner->BindDocumentForTest(ViewId, Document);
		});
	if (!TestTrue(TEXT("the document loaded and bound"), bBound))
	{
		return false;
	}

	// Listeners first (a closure frame, which also runs the layout the click
	// needs -- RecordAndPublishFrame's Context::Update follows the pump).
	TestEqual(TEXT("listeners installed"),
		Rig.Eval(ViewId,
			"globalThis.log = [];"
			"globalThis.btn = document.getElementById('btn');"
			"btn.addEventListener('click', function(ev){"
			"  log.push(['click', ev.mouseX, ev.mouseY, ev.button, ev.target === btn].join(':'));"
			"});"
			// Key events land on the focus element -- Show(FocusFlag::Document)
			// focused the document, and keydown/keyup/textinput dispatch there
			// (Context.cpp:527-545, :568-578); the document wrapper hears them
			// at target phase.
			"document.addEventListener('keydown', function(ev){"
			"  log.push(['kd', ev.keyIdentifier, ev.shiftKey, ev.ctrlKey].join(':'));"
			"});"
			"document.addEventListener('keyup', function(ev){ log.push(['ku', ev.keyIdentifier].join(':')); });"
			"document.addEventListener('textinput', function(ev){ log.push(['ti', ev.text].join(':')); });"
			"'ok'"),
		FString(TEXT("ok")));

	// The production enqueues, verbatim from the routing test's pattern.
	const FVaCuusModifierState NoModifiers;
	FVaCuusModifierState ShiftDown;
	ShiftDown.bShiftDown = true;
	const FIntPoint ClickPoint(30, 30);

	Rig.Thread->EnqueueInput(ViewId, FVaCuusInputEvent::MouseMove(ClickPoint, NoModifiers));
	Rig.Thread->EnqueueInput(ViewId,
		FVaCuusInputEvent::MouseButton(/*bDown=*/true, ClickPoint, EKeys::LeftMouseButton, NoModifiers));
	Rig.Thread->EnqueueInput(ViewId,
		FVaCuusInputEvent::MouseButton(/*bDown=*/false, ClickPoint, EKeys::LeftMouseButton, NoModifiers));
	Rig.Thread->EnqueueInput(ViewId, FVaCuusInputEvent::KeyEvent(/*bDown=*/true, EKeys::A, ShiftDown));
	Rig.Thread->EnqueueInput(ViewId, FVaCuusInputEvent::KeyEvent(/*bDown=*/false, EKeys::A, ShiftDown));
	Rig.Thread->EnqueueInput(ViewId, FVaCuusInputEvent::TextInput(uint32('V'), NoModifiers));
	Rig.Thread->EnqueueInput(ViewId, FVaCuusInputEvent::KeyEvent(/*bDown=*/true, EKeys::Gamepad_DPad_Down, NoModifiers));
	Rig.Thread->EnqueueInput(ViewId, FVaCuusInputEvent::KeyEvent(/*bDown=*/false, EKeys::Gamepad_DPad_Down, NoModifiers));

	// The input queue drains BEFORE the pump in the same frame, so one closure
	// frame later the log is complete. Numeric expectations are RmlUi's own
	// identifiers: KI_A = 12, KI_DOWN = 93 (Input.h:26, :122); left button = 0;
	// the click coordinates are the enqueued screen point (mouse_x/mouse_y,
	// Context.cpp:1538-1539).
	TestEqual(TEXT("mouse, keyboard, textinput and GAMEPAD all reached JS with their parameters"),
		Rig.Eval(ViewId, "log.join('|')"),
		FString(TEXT("click:30:30:0:true|kd:12:true:false|ku:12|ti:V|kd:93:false:false|ku:93")));

	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

/**
 * THE THREE DEATH ORDERS (spec 2(g)) and the mid-dispatch self-removal hazard
 * (hud-demo-patterns.md section 3), each with the ref/shell/registry gauges
 * asserted back to baseline:
 *  (1) direct element destruction -- remove() -> destroy-then-detach
 *      (Element.cpp:99 -> :112);
 *  (2) document unload -- Close() + Update() -> detach-then-destroy
 *      (Context.cpp:1557-1567);
 *  (3) context death first -- EnqueueRemoveView: OnViewRemoved's neuter walk
 *      frees the refs against the still-attached tree, then the SAME command's
 *      host Shutdown destroys tree+context and RmlUi's detach reclaims the
 *      neutered shells (VaCuusUIThread.cpp RemoveView's ordering comment).
 *
 * RESTORE-THE-BUG (plan 5.2): delete the two release lines in
 * FVaCuusJsEventListener::NeuterFromContext (the JS_FreeValue +
 * NoteListenerRefReleased pair) and orders (1)/(2) stay green while order
 * (3)'s "refs back to zero" goes red -- and the runtime destructor's
 * live-byte checkf names the leaked bytes at rig teardown. Both outputs are
 * recorded in the task report; the lines are load-bearing.
 */
bool FVaCuusJsEventDeathOrdersTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsDomTest;

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	static const TCHAR* GDocument = TEXT(R"(<rml>
<head><style>body { display: block; } div { display: block; }</style></head>
<body><div id="wrap"><div id="x"/><div id="a"/><div id="b"/></div></body>
</rml>)");

	FDomProbeHost* Probe = nullptr;
	const uint32 ViewId = Rig.AddViewWithDocument(Probe, TEXT("vacuus_jsevt_death"), GDocument);

	bool bBound = false;
	Rig.RunOnUI([&bBound, Probe, ViewId]()
		{
			Rml::ElementDocument* Document = Probe->GetDocument();
			bBound = Document != nullptr;
			FWrappedDomHost::Inner->BindDocumentForTest(ViewId, Document);
		});
	if (!TestTrue(TEXT("the document loaded and bound"), bBound))
	{
		return false;
	}

	//~ The gauges. Refs live on the runtime, shells are process-wide statics,
	//~ the registry count is per view; every read goes through a UI closure so
	//~ the closure-serial handshake orders it after the JS that moved it.
	const auto Refs = [&Rig]() -> int64
	{
		int64 Value = -1;
		Rig.RunOnUI([&Value]() { Value = FWrappedDomHost::Inner->GetRuntime()->GetNumListenerRefs(); });
		return Value;
	};
	const auto Registry = [&Rig, ViewId]() -> int32
	{
		int32 Value = -1;
		Rig.RunOnUI([&Value, ViewId]()
			{
				FVaCuusJsViewContext* View = FWrappedDomHost::Inner->FindViewContext(ViewId);
				Value = View != nullptr ? View->GetLiveListenerCount() : 0;
			});
		return Value;
	};

	const int32 BaseShells = FVaCuusJsEventListener::GetNumLiveShells();

	// --- SELF-REMOVAL DURING DISPATCH (the demo's latent hazard, run for real).
	// The handler destroys its own element mid-call: RemoveChild's discarded
	// ElementPtr -> ~Element -> ~EventDispatcher -> OUR OnDetach, below our own
	// ProcessEvent. The handler keeps running afterwards (the dup'd fn), the
	// ancestor's listener still fires (the chain was collected up front with
	// ObserverPtrs, EventDispatcher.cpp:81-99), and the shell is reclaimed at
	// dispatch exit -- no crash, nothing leaked.
	TestEqual(TEXT("self-removal during dispatch survives, handler completes, ancestor still fires"),
		Rig.Eval(ViewId,
			"globalThis.log = [];"
			"globalThis.wrap = document.getElementById('wrap');"
			"const x = document.getElementById('x');"
			"x.addEventListener('click', function(ev){"
			"  log.push('in');"
			"  ev.target.remove();"
			"  log.push('alive:' + (x.id === null));"
			"});"
			"globalThis.wf = function(){ log.push('wrap'); };"
			"wrap.addEventListener('click', wf);"
			"x.dispatchEvent('click');"
			"log.join('|')"),
		FString(TEXT("in|alive:true|wrap")));
	TestEqual(TEXT("self-removal: x's ref released, wrap's remains"), Refs(), int64(1));
	TestEqual(TEXT("self-removal: x's shell reclaimed at dispatch exit"),
		FVaCuusJsEventListener::GetNumLiveShells(), BaseShells + 1);
	TestEqual(TEXT("self-removal: registry holds only wrap's entry"), Registry(), 1);

	Rig.Eval(ViewId, "wrap.removeEventListener('click', wf); 'ok'");
	TestEqual(TEXT("clean slate before the orders"), Refs(), int64(0));
	TestEqual(TEXT("clean slate shells"), FVaCuusJsEventListener::GetNumLiveShells(), BaseShells);

	// --- ORDER (1): direct element destruction, destroy-then-detach.
	Rig.Eval(ViewId,
		"globalThis.f1 = function(){};"
		"document.getElementById('b').addEventListener('click', f1);"
		"'ok'");
	TestEqual(TEXT("order 1 armed: one ref"), Refs(), int64(1));
	Rig.Eval(ViewId, "document.getElementById('b').remove(); 'ok'");
	TestEqual(TEXT("order 1: ref released via ~Element's detach"), Refs(), int64(0));
	TestEqual(TEXT("order 1: shell reclaimed"), FVaCuusJsEventListener::GetNumLiveShells(), BaseShells);
	TestEqual(TEXT("order 1: registry empty"), Registry(), 0);

	// --- ORDER (2): document unload, detach-then-destroy. Close() only queues
	// (ElementDocument.cpp:421-425); the demo teardown's own recipe -- Close()
	// plus one Context::Update -- runs ReleaseUnloadedDocuments, which detaches
	// the WHOLE tree before destroying it (Context.cpp:1557-1567). Driven
	// explicitly because the rig's record loop skips document-less hosts.
	Rig.Eval(ViewId,
		"document.getElementById('a').addEventListener('click', function(){});"
		"wrap.addEventListener('click', function(){});"
		"'ok'");
	TestEqual(TEXT("order 2 armed: two refs"), Refs(), int64(2));
	Rig.RunOnUI([Probe]()
		{
			Probe->CloseDocument();
			Probe->GetContext()->Update();
		});
	TestEqual(TEXT("order 2: refs released via the unload sweep"), Refs(), int64(0));
	TestEqual(TEXT("order 2: shells reclaimed"), FVaCuusJsEventListener::GetNumLiveShells(), BaseShells);
	TestEqual(TEXT("order 2: registry empty"), Registry(), 0);

	// --- ORDER (3): the context dies FIRST, the tree still attached. A fresh
	// document, fresh listeners; EnqueueRemoveView then runs, in one command:
	// OnViewRemoved (context destructor -> the NEUTER WALK frees both refs
	// against the live tree) and only then the probe host's Shutdown
	// (CloseDocument + RemoveContext -> tree destroyed -> RmlUi's OnDetach
	// finds neutered shells and reclaims them, touching nothing dead).
	Rig.Thread->EnqueueLoadDocumentFromMemory(ViewId, GDocument, /*LoadSerial=*/2);
	bool bRebound = false;
	Rig.RunOnUI([&bRebound, Probe, ViewId]()
		{
			Rml::ElementDocument* Document = Probe->GetDocument();
			bRebound = Document != nullptr;
			FWrappedDomHost::Inner->BindDocumentForTest(ViewId, Document);
		});
	if (!TestTrue(TEXT("the order-3 document loaded and bound"), bRebound))
	{
		return false;
	}
	Rig.Eval(ViewId,
		"document.getElementById('a').addEventListener('click', function(){});"
		"document.getElementById('wrap').addEventListener('keydown', function(){}, true);"
		"'ok'");
	TestEqual(TEXT("order 3 armed: two refs, two shells"), Refs(), int64(2));
	TestEqual(TEXT("order 3 armed shells"), FVaCuusJsEventListener::GetNumLiveShells(), BaseShells + 2);

	Rig.Thread->EnqueueRemoveView(ViewId);
	if (!TestTrue(TEXT("the removal frame ran"), PumpRealFrames(*Rig.Thread, 1)))
	{
		return false;
	}
	// Reads off the UI thread, deliberately: the view is gone, the frame that
	// removed it has completed (WaitForFrameCount), and both gauges are atomics.
	// The UE_LOG doubles the gauges into the MAIN log immediately -- in the red
	// state (the neuter walk's frees deleted) the run dies in the rig teardown's
	// live-byte checkf BEFORE the framework prints recorded expectations, and
	// this line is what still shows refs=2 in the crash log.
	UE_LOG(LogVaCuusJS, Display, TEXT("order 3 gauges after view removal: refs=%lld, shells=%d (baseline %d)"),
		FWrappedDomHost::Inner->GetRuntime()->GetNumListenerRefs(), FVaCuusJsEventListener::GetNumLiveShells(),
		BaseShells);
	TestEqual(TEXT("order 3: the neuter walk released every ref"),
		FWrappedDomHost::Inner->GetRuntime()->GetNumListenerRefs(), int64(0));
	TestEqual(TEXT("order 3: RmlUi's detach reclaimed the neutered shells"),
		FVaCuusJsEventListener::GetNumLiveShells(), BaseShells);

	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

/**
 * The on*-attribute path (spec 3.9, plan 5.1's instancer): a parsed
 * onclick="..." compiles LAZILY against the owning view's context at first
 * fire (it shares globalThis with the view's scripts; `this` is the element,
 * `event` the event object); a broken snippet produces exactly ONE Error
 * naming attribute, element and document, then stays inert; an attribute on a
 * document bound to NO JS view warns once and stays inert; and a
 * setAttribute-written onclick goes through the same instancer path live.
 */
bool FVaCuusJsEventOnAttributeTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsDomTest;

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	// The onclick attributes are INSTANCED during this parse (OnAttributeChange
	// -> Factory::InstanceEventListener, Element.cpp:1724-1749) -- before any
	// JS context exists for the view, which is the whole reason the listener
	// compiles lazily.
	static const TCHAR* GDocument = TEXT(R"(<rml>
<head><style>body { display: block; } div { display: block; }</style></head>
<body>
	<div id="ok" onclick="globalThis.attrLog = (globalThis.attrLog || []); attrLog.push('fired:' + this.id + ':' + event.type);"/>
	<div id="bad" onclick="this is ( not js"/>
</body>
</rml>)");

	FDomProbeHost* Probe = nullptr;
	const uint32 ViewId = Rig.AddViewWithDocument(Probe, TEXT("vacuus_jsevt_attr"), GDocument);

	bool bBound = false;
	Rig.RunOnUI([&bBound, Probe, ViewId]()
		{
			Rml::ElementDocument* Document = Probe->GetDocument();
			bBound = Document != nullptr;
			FWrappedDomHost::Inner->BindDocumentForTest(ViewId, Document);
		});
	if (!TestTrue(TEXT("the document loaded and bound"), bBound))
	{
		return false;
	}

	// A: the success path. First fire resolves + compiles + runs; the second
	// fire reuses the compiled function. Shared globalThis proves it compiled
	// against THE VIEW's context, `this.id` proves the element binding.
	TestEqual(TEXT("onclick fires against the view context, twice"),
		Rig.Eval(ViewId,
			"document.getElementById('ok').dispatchEvent('click');"
			"document.getElementById('ok').dispatchEvent('click');"
			"globalThis.attrLog.join('|')"),
		FString(TEXT("fired:ok:click|fired:ok:click")));

	// B: the named error path, ONE-shot. The single Error carries attribute,
	// element address and document URL; the second dispatch stays silent
	// (bResolveAttempted latches).
	AddExpectedMessagePlain(TEXT("onclick attribute of 'div#bad' in vacuus://js_dom_test.rml"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);
	TestEqual(TEXT("a broken snippet dispatches without crashing, twice"),
		Rig.Eval(ViewId,
			"document.getElementById('bad').dispatchEvent('click');"
			"document.getElementById('bad').dispatchEvent('click');"
			"'ok'"),
		FString(TEXT("ok")));
	TestEqual(TEXT("the compile error was counted exactly once"),
		FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(1));

	// C: a live setAttribute goes through the same instancer (OnAttributeChange
	// fires synchronously, Element.inl:15-23) -- and can be fired immediately.
	TestEqual(TEXT("setAttribute('onclick', ...) instances and fires"),
		Rig.Eval(ViewId,
			"const ok = document.getElementById('ok');"
			"ok.setAttribute('onmouseup', 'attrLog.push(\"viaSetAttribute\");');"
			"ok.dispatchEvent('mouseup');"
			"attrLog[attrLog.length - 1]"),
		FString(TEXT("viaSetAttribute")));

	// D: the unbound-document Warning, once. A SECOND view's document is loaded
	// but never bound to any JS context; its onclick fires through the C++
	// dispatch path and must warn exactly once, then stay inert.
	static const TCHAR* GUnboundDocument = TEXT(R"(<rml>
<head><style>body { display: block; } div { display: block; }</style></head>
<body><div id="u" onclick="globalThis.neverRuns = true;"/></body>
</rml>)");
	FDomProbeHost* UnboundProbe = nullptr;
	Rig.AddViewWithDocument(UnboundProbe, TEXT("vacuus_jsevt_attr_unbound"), GUnboundDocument);

	AddExpectedMessagePlain(TEXT("not bound to any JS view"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	bool bUnboundDispatched = false;
	Rig.RunOnUI([&bUnboundDispatched, UnboundProbe]()
		{
			Rml::ElementDocument* Document = UnboundProbe->GetDocument();
			if (Document != nullptr)
			{
				if (Rml::Element* Element = Document->GetElementById("u"))
				{
					Element->DispatchEvent("click", Rml::Dictionary());
					Element->DispatchEvent("click", Rml::Dictionary());	   // the Warning must NOT repeat
					bUnboundDispatched = true;
				}
			}
		});
	TestTrue(TEXT("the unbound document dispatched"), bUnboundDispatched);

	// The bound view's context never saw the unbound snippet run.
	TestEqual(TEXT("the unbound snippet never executed anywhere"),
		Rig.Eval(ViewId, "String(globalThis.neverRuns)"), FString(TEXT("undefined")));

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
