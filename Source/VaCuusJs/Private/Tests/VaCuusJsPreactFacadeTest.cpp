// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "VaCuusJsDomTestRig.h"

#include "VaCuusContentPaths.h"

#include "Misc/FileHelper.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Factory.h>

/*
 * M5 TASK 1 -- THE FACADE PREACT NEEDS (spec 2(j), 3.1; preact-contract.md;
 * ep-observations.md is the experiments' record). Two layers of test:
 *
 *  - per-gap facade tests (text nodes G1, traversal G2, childNodes G3,
 *    nodeType G4, localName, the style camelCase mapping, the attributes
 *    snapshot) driven as plain JS through the M4 rig;
 *  - the two committed stock-preact fixtures (Content/DevUI/Tests, preact
 *    10.29.7, provenance in each file's header) re-running the decisive E-P
 *    scenarios against the real facade on every suite run -- cold mount,
 *    adoption into non-empty DOM, keyed reversal, and the counter's
 *    create-once-then-`data` text path.
 *
 * THE BRACE-INJECTION TEST is this file's restore-the-bug centerpiece: user
 * text containing '{{Health}}' written through createTextNode renders the
 * LITERAL braces, because the facade sets text via ElementText::SetText and
 * never lets the parser's brace scanner (Factory.cpp:344-392) near user
 * strings. The red half routes the SAME string through the parser's entry,
 * Factory::InstanceElementText, and watches the data binding evaluate user
 * data -- the injection the bypass exists to prevent.
 */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsDomTextNodesTest, "VaCuus.Js.Dom.TextNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsDomTraversalTest, "VaCuus.Js.Dom.Traversal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsDomStyleCamelCaseTest, "VaCuus.Js.Dom.StyleCamelCase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsDomBraceInjectionTest, "VaCuus.Js.Dom.BraceInjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsPreactDomContractTest, "VaCuus.Js.Preact.DomContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsPreactCounterTextPathTest, "VaCuus.Js.Preact.CounterTextPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace VaCuusJsPreactFacadeTest
{
using namespace VaCuusJsDomTest;

/** Reads a committed fixture bundle from the plugin's canonical DevUI root. */
inline bool LoadFixture(FAutomationTestBase& Test, const TCHAR* Name, FString& OutBundle)
{
	const TArray<FString>& Roots = VaCuusContentPaths::GetDocumentRoots();
	if (Roots.IsEmpty())
	{
		Test.AddError(TEXT("no DevUI document roots exist"));
		return false;
	}
	const FString Path = Roots[0] / TEXT("Tests") / Name;
	if (!FFileHelper::LoadFileToString(OutBundle, *Path))
	{
		Test.AddError(FString::Printf(TEXT("could not read the committed fixture '%s'"), *Path));
		return false;
	}
	return true;
}

/** The rig boot + document + bind dance every test opens with. */
inline bool BootWithDocument(FAutomationTestBase& Test, FDomTestRig& Rig, FDomProbeHost*& OutProbe, uint32& OutViewId,
	const TCHAR* Prefix, const TCHAR* Document)
{
	OutProbe = nullptr;
	OutViewId = Rig.AddViewWithDocument(OutProbe, Prefix, Document);

	bool bBound = false;
	FDomProbeHost* Probe = OutProbe;
	const uint32 ViewId = OutViewId;
	Rig.RunOnUI([&bBound, Probe, ViewId]()
		{
			Rml::ElementDocument* Document = Probe->GetDocument();
			bBound = Document != nullptr;
			FWrappedDomHost::Inner->BindDocumentForTest(ViewId, Document);
		});
	return Test.TestTrue(TEXT("the document loaded and bound"), bBound);
}
}	 // namespace VaCuusJsPreactFacadeTest

/**
 * G1 + G3 + G4: createTextNode mints a detached-owning #text wrapper whose
 * text was set through ElementText::SetText -- never an RML parse; nodeValue
 * and `data` alias Get/SetText and update the rendered text; nodeType answers
 * 1/3/9 off the same discriminators the wrapping uses; childNodes keeps text
 * children while `children` filters them (the deliberate difference); the
 * ownership matrix (append / removeChild / re-append / remove) covers text
 * wrappers unchanged, and a dead text wrapper reads null everywhere.
 */
bool FVaCuusJsDomTextNodesTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsPreactFacadeTest;

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	// LatoLatin: registered by FVaCuusEngine::Initialize; a fontless text
	// element would log "No font face defined" per layout pass (the
	// VaCuusDataForTest.cpp precedent).
	static const TCHAR* GDocument = TEXT(R"(<rml>
<head><style>body { display: block; font-family: LatoLatin; font-size: 16px; } div { display: block; } b { display: inline; }</style></head>
<body><div id="mount"/></body>
</rml>)");

	FDomProbeHost* Probe = nullptr;
	uint32 ViewId = 0;
	if (!BootWithDocument(*this, Rig, Probe, ViewId, TEXT("vacuus_js_textnodes"), GDocument))
	{
		return false;
	}

	// A: birth, discrimination, aliasing, the children/childNodes split.
	TestEqual(TEXT("createTextNode: discrimination + data/nodeValue + childNodes"),
		Rig.Eval(ViewId,
			"const mount = document.getElementById('mount');"
			"const t = document.createTextNode('hello');"
			"globalThis.t = t;"
			"const r0 = [t.nodeType, t.tagName, t.localName, t.nodeValue, t.data, t.parentNode,"
			"            document.nodeType, mount.nodeType, document.body.nodeType];"
			"mount.appendChild(t);"
			"const r1 = [t.parentNode === mount, mount.innerRML];"
			"t.data = 'world';"
			"const r2 = [t.nodeValue, mount.innerRML];"
			"t.nodeValue = 'again';"
			"const r3 = [t.data];"
			"const el = document.createElement('b');"
			"mount.appendChild(el);"
			"const r4 = [mount.children.length, mount.childNodes.length,"
			"            mount.childNodes[0] === t, mount.children[0] === el, el.nodeType];"
			"el.data = 'nope'; el.nodeValue = 'nope';"	  // non-text: silent no-op, reads null
			"const r5 = [el.data, el.nodeValue];"
			"r0.concat(r1, r2, r3, r4, r5).map(String).join('|')"),
		FString(TEXT("3|#text|#text|hello|hello|null|9|1|9|true|hello|world|world|again|1|2|true|true|1|null|null")));

	// The DOM-side probe: the rendered text is ElementText::GetText, read on
	// the raw element (the M3 InnerRML-is-the-text-observable convention).
	FString ProbedText, ProbedTag, ProbedInner;
	Rig.RunOnUI([&, Probe]()
		{
			Rml::Element* Mount = Probe->GetDocument()->GetElementById("mount");
			Rml::Element* Child = Mount != nullptr ? Mount->GetChild(0) : nullptr;
			if (Child != nullptr)
			{
				ProbedTag = UTF8_TO_TCHAR(Child->GetTagName().c_str());
				if (Child->GetTagName() == "#text")
				{
					ProbedText = UTF8_TO_TCHAR(static_cast<Rml::ElementText*>(Child)->GetText().c_str());
				}
				ProbedInner = UTF8_TO_TCHAR(Mount->GetInnerRML().c_str());
			}
		});
	TestEqual(TEXT("the probe sees a #text child"), ProbedTag, FString(TEXT("#text")));
	TestEqual(TEXT("the probe reads the JS-written text"), ProbedText, FString(TEXT("again")));
	TestEqual(TEXT("the parent's serialization carries it"), ProbedInner, FString(TEXT("again<b />")));

	// B: the ownership matrix on a text wrapper -- detach keeps identity and
	// text, re-append works, remove() kills, dead reads null everywhere.
	// `mount` and `t` persist from the first eval: global-scope `const`
	// bindings survive across JS_Eval calls on one context.
	TestEqual(TEXT("text-wrapper lifecycle + death"),
		Rig.Eval(ViewId,
			"const back = mount.removeChild(t);"
			"const s0 = [back === t, t.data, t.parentNode, mount.childNodes.length];"
			"mount.appendChild(t);"
			"const s1 = [mount.childNodes.length, mount.childNodes[1] === t, mount.innerRML];"
			"t.remove();"
			"const s2 = [t.data, t.nodeValue, t.nodeType, t.parentNode, mount.childNodes.length];"
			"s0.concat(s1, s2).map(String).join('|')"),
		FString(TEXT("true|again|null|1|2|true|<b />again|null|null|null|null|1")));

	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

/**
 * G2: the four step accessors walk elements AND text nodes in document order
 * (Element.h:418-429; Element.cpp:1095-1141 -- non-DOM extras excluded, #text
 * included), ends answer null, and the walk agrees with childNodes/children.
 */
bool FVaCuusJsDomTraversalTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsPreactFacadeTest;

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	// The parser-authored text child ("middle") arrives through RmlUi's own
	// InstanceElementText -- trusted authoring, exactly what the facade's
	// createTextNode bypass is NOT for.
	static const TCHAR* GDocument = TEXT(R"(<rml>
<head><style>body { display: block; font-family: LatoLatin; font-size: 16px; } div { display: block; } span { display: inline; } b { display: inline; }</style></head>
<body><div id="walk"><span id="s1"/>middle<b id="b1"/></div></body>
</rml>)");

	FDomProbeHost* Probe = nullptr;
	uint32 ViewId = 0;
	if (!BootWithDocument(*this, Rig, Probe, ViewId, TEXT("vacuus_js_traversal"), GDocument))
	{
		return false;
	}

	TestEqual(TEXT("the traversal chain, both directions, text included"),
		Rig.Eval(ViewId,
			"const walk = document.getElementById('walk');"
			"const f = walk.firstChild;"		 // span#s1
			"const m = f.nextSibling;"			 // #text 'middle'
			"const l = m.nextSibling;"			 // b#b1
			"[f.id, f.nodeType, m.nodeType, m.data, l.id, l.nextSibling,"
			" walk.firstChild === f, walk.lastChild === l,"
			" l.previousSibling === m, m.previousSibling === f, f.previousSibling,"
			" walk.childNodes.length, walk.children.length,"
			" walk.children[0] === f, walk.children[1] === l,"		// children skips the text node
			" walk.childNodes[1] === m,"
			" document.firstChild === walk,"	 // the document IS the body; walk is its only child
			" document.createElement('div').firstChild,"			// fresh detached: no children
			" document.createElement('div').nextSibling]"			// and no parent to walk
			".map(String).join('|')"),
		FString(TEXT("s1|1|3|middle|b1|null|true|true|true|true|null|3|2|true|true|true|true|null|null")));

	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

/**
 * The style camelCase->kebab mapping (E-P4): a camel write lands as the kebab
 * property and reads back through BOTH spellings; '--x' passes verbatim; a
 * BARE NUMBER is refused -- preact appends 'px' itself (props.js:23), so the
 * facade adds nothing and RmlUi's own parse warning is the loud refusal.
 */
bool FVaCuusJsDomStyleCamelCaseTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsPreactFacadeTest;

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	static const TCHAR* GDocument = TEXT(R"(<rml>
<head><style>body { display: block; } div { display: block; }</style></head>
<body><div id="mount"/></body>
</rml>)");

	FDomProbeHost* Probe = nullptr;
	uint32 ViewId = 0;
	if (!BootWithDocument(*this, Rig, Probe, ViewId, TEXT("vacuus_js_stylecamel"), GDocument))
	{
		return false;
	}

	// The bare-number refusal is RmlUi's own inline-declaration warning,
	// exactly once ("width: 100" fails the length_percent parse -- width has
	// no NUMBER unit, StyleSheetSpecification.cpp:322).
	AddExpectedMessagePlain(TEXT("Syntax error parsing inline property declaration"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);

	TestEqual(TEXT("camelCase maps, --x passes, bare number refused"),
		Rig.Eval(ViewId,
			"const el = document.getElementById('mount');"
			"el.style.backgroundColor = 'red';"
			"const r0 = [el.style.backgroundColor, el.style['background-color']];"
			"const ok = el.style.setProperty('marginLeft', '5px');"
			"const bare = el.style.setProperty('width', 100);"	  // preact would have sent '100px'
			"el.style.setProperty('--x', '1');"
			"const r1 = [ok, bare, el.style['--x'], el.style.marginLeft];"
			"delete el.style.backgroundColor;"
			"const r2 = [el.style['background-color']];"
			"r0.concat(r1, r2).map(String).join('|')"),
		FString(TEXT("#ff0000|#ff0000|true|false|1|5px|#00000000")));

	// The DOM-side read of the kebab property the camel write landed on.
	FString MarginSeen, WidthSeen;
	Rig.RunOnUI([&, Probe]()
		{
			Rml::Element* Mount = Probe->GetDocument()->GetElementById("mount");
			const Rml::Property* Margin = Mount->GetProperty("margin-left");
			MarginSeen = Margin != nullptr ? UTF8_TO_TCHAR(Margin->ToString().c_str()) : TEXT("<null>");
			const Rml::Property* Width = Mount->GetProperty("width");
			WidthSeen = Width != nullptr ? UTF8_TO_TCHAR(Width->ToString().c_str()) : TEXT("<null>");
		});
	TestEqual(TEXT("the camel write landed on the kebab property"), MarginSeen, FString(TEXT("5px")));
	TestEqual(TEXT("the refused bare number left width at its default"), WidthSeen, FString(TEXT("auto")));

	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

namespace VaCuusJsPreactFacadeTest
{
/** The brace test's bound value: what the injection would leak. Constant; the UI thread only reads it. */
static int GBraceHealth = 100;
}	 // namespace VaCuusJsPreactFacadeTest

/**
 * THE BRACE INJECTION, RESTORE-THE-BUG (spec 2(j), 7). A data model with a
 * bound `Health` is live on the view and the mount div sits in its scope --
 * the exact configuration where RmlUi's parser turns '{{Health}}' text into a
 * DataViewText (Factory.cpp:391-392 tags `data-text`; insertion runs
 * ApplyDataViewsControllers via SetDataModel, Element.cpp:2162; the freshly
 * added view updates unconditionally next Context::Update, DataView.cpp:76-87).
 *
 * GREEN: the facade's createTextNode writes the same string through
 * ElementText::SetText -- no scan, no data-text, and after real frames the
 * text is STILL the literal braces.
 *
 * RED (the mechanism, in-test): Factory::InstanceElementText -- the parser's
 * entry a naive createTextNode would use -- gets the identical string, and
 * the binding evaluates it to the model's value. User data became an
 * expression. (The task-log restore run additionally routed the facade's own
 * createTextNode through this path and watched the GREEN half fail.)
 */
bool FVaCuusJsDomBraceInjectionTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsPreactFacadeTest;

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	static const TCHAR* GDocument = TEXT(R"(<rml>
<head><style>body { display: block; font-family: LatoLatin; font-size: 16px; } div { display: block; }</style></head>
<body><div id="mount" data-model="hud"/></body>
</rml>)");

	// The model must exist BEFORE the document loads (data-model="hud" resolves
	// at parse), and load commands drain ahead of closures in the same frame --
	// so: add the view bare, create the model in a closure, THEN enqueue the
	// load (next frame's drain), then bind.
	TUniquePtr<FDomProbeHost> Owned = MakeUnique<FDomProbeHost>(TEXT("vacuus_js_brace"));
	FDomProbeHost* Probe = Owned.Get();
	const uint32 ViewId = Rig.Thread->AllocateViewId();
	Rig.Thread->EnqueueAddView(ViewId, MoveTemp(Owned), FIntPoint(400, 300), MakeShared<FVaCuusViewStatus>());

	bool bModelMade = false;
	Rig.RunOnUI([&bModelMade, Probe]()
		{
			Rml::DataModelConstructor Constructor = Probe->GetContext()->CreateDataModel("hud");
			if (Constructor)
			{
				Constructor.Bind("Health", &GBraceHealth);
				bModelMade = true;
			}
		});
	if (!TestTrue(TEXT("the data model exists before the document"), bModelMade))
	{
		return false;
	}

	Rig.Thread->EnqueueLoadDocumentFromMemory(ViewId, GDocument, /*LoadSerial=*/1);
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

	// GREEN: literal braces in, literal braces rendered -- no data-text tag.
	TestEqual(TEXT("green: createTextNode('{{Health}}') is never scanned"),
		Rig.Eval(ViewId,
			"const mount = document.getElementById('mount');"
			"const t = document.createTextNode('{{Health}}');"
			"mount.appendChild(t);"
			"[t.data, mount.innerRML, t.getAttribute('data-text')].map(String).join('|')"),
		FString(TEXT("{{Health}}|{{Health}}|null")));

	// Real frames passed (each RunOnUI pumps, and the record loop's
	// Context::Update follows the pump within the frame) -- if a DataViewText
	// existed it would have evaluated by now. Still literal:
	TestEqual(TEXT("green: still literal after Context::Update ran"),
		Rig.Eval(ViewId, "[t.data, document.getElementById('mount').innerRML].map(String).join('|')"),
		FString(TEXT("{{Health}}|{{Health}}")));

	FString GreenText;
	bool bGreenTagged = true;
	Rig.RunOnUI([&, Probe]()
		{
			Rml::Element* Mount = Probe->GetDocument()->GetElementById("mount");
			Rml::Element* Child = Mount != nullptr ? Mount->GetChild(0) : nullptr;
			if (Child != nullptr && Child->GetTagName() == "#text")
			{
				GreenText = UTF8_TO_TCHAR(static_cast<Rml::ElementText*>(Child)->GetText().c_str());
				bGreenTagged = Child->GetAttribute("data-text") != nullptr;
			}
		});
	TestEqual(TEXT("green probe: the raw element holds the literal string"), GreenText, FString(TEXT("{{Health}}")));
	TestFalse(TEXT("green probe: no data-text attribute anywhere"), bGreenTagged);

	// RED: the SAME string through the parser's entry -- the path the bypass
	// refuses. The scanner tags it, insertion wires the DataViewText, and the
	// next Update writes the MODEL VALUE over user data.
	Rig.RunOnUI([Probe]()
		{
			Rml::Element* Mount = Probe->GetDocument()->GetElementById("mount");
			Rml::Factory::InstanceElementText(Mount, "{{Health}}");
		});

	FString RedText;
	bool bRedTagged = false;
	Rig.RunOnUI([&, Probe]()
		{
			Rml::Element* Mount = Probe->GetDocument()->GetElementById("mount");
			Rml::Element* Injected = Mount != nullptr ? Mount->GetChild(1) : nullptr;
			if (Injected != nullptr && Injected->GetTagName() == "#text")
			{
				RedText = UTF8_TO_TCHAR(static_cast<Rml::ElementText*>(Injected)->GetText().c_str());
				bRedTagged = Injected->GetAttribute("data-text") != nullptr;
			}
		});
	TestTrue(TEXT("red: the scanner tagged the parser-routed text"), bRedTagged);
	TestEqual(TEXT("red: the binding evaluated user data to the model value -- the injection, observed"), RedText,
		FString(TEXT("100")));

	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

/**
 * The committed stock-preact bundle (fixture-dom.js, provenance in its
 * header) re-runs the three decisive E-P scenarios natively on every suite
 * run: cold mount (createElementNS + firstChild + insertBefore), adoption
 * into non-empty DOM (childNodes + localName + attributes), and the keyed
 * reversal whose order CORRUPTED without nextSibling (E-P5's pre-G2 run:
 * row4,row2,row3,row1 -- ep-observations.md).
 */
bool FVaCuusJsPreactDomContractTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsPreactFacadeTest;

	FString Bundle;
	if (!LoadFixture(*this, TEXT("fixture-dom.js"), Bundle))
	{
		return false;
	}

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	static const TCHAR* GDocument = TEXT(R"(<rml>
<head><style>body { display: block; } div { display: block; } span { display: inline; }</style></head>
<body><div id="mount"/><div id="mount2"/><div id="prefilled"><div id="pre1" class="pre"/></div></body>
</rml>)");

	FDomProbeHost* Probe = nullptr;
	uint32 ViewId = 0;
	if (!BootWithDocument(*this, Rig, Probe, ViewId, TEXT("vacuus_js_preact_dom"), GDocument))
	{
		return false;
	}

	FString EvalResult;
	Rig.RunOnUI([&EvalResult, ViewId, &Bundle]()
		{ EvalResult = EvalString(*FWrappedDomHost::Inner, ViewId, TCHAR_TO_UTF8(*Bundle)); });
	TestEqual(TEXT("the fixture ran clean"), Rig.Eval(ViewId, "(globalThis.EPLOG || ['<no run>']).join(';') || 'clean'"),
		FString(TEXT("clean")));

	TestEqual(TEXT("cold mount: preact built the tree through the native facade"),
		Rig.Eval(ViewId, "globalThis.EPRUN.mount"), FString(TEXT("<div id=\"a\"><span id=\"b\" /></div>")));
	TestEqual(TEXT("adoption: the pre-existing div was claimed and re-propped"),
		Rig.Eval(ViewId, "globalThis.EPRUN.adopt"), FString(TEXT("<div id=\"f\" />")));
	TestEqual(TEXT("keyed mount order"), Rig.Eval(ViewId, "globalThis.EPRUN.mounted"),
		FString(TEXT("row1,row2,row3,row4")));
	TestEqual(TEXT("keyed reversal order -- nextSibling's load-bearing proof"),
		Rig.Eval(ViewId, "globalThis.EPRUN.reversed"), FString(TEXT("row4,row3,row2,row1")));

	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

/**
 * E-P3, pinned by the committed counter fixture: mount creates each text node
 * ONCE (two createTextNode calls -- 'count:' and the number), and every
 * update flows through the `data` setter alone -- SetText, the same
 * scanner-bypass as createTextNode. The count of createTextNode calls not
 * growing across bumps IS the create-once-then-data contract.
 */
bool FVaCuusJsPreactCounterTextPathTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsPreactFacadeTest;

	FString Bundle;
	if (!LoadFixture(*this, TEXT("fixture-counter.js"), Bundle))
	{
		return false;
	}

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	static const TCHAR* GDocument = TEXT(R"(<rml>
<head><style>body { display: block; font-family: LatoLatin; font-size: 16px; } div { display: block; }</style></head>
<body><div id="mount"/></body>
</rml>)");

	FDomProbeHost* Probe = nullptr;
	uint32 ViewId = 0;
	if (!BootWithDocument(*this, Rig, Probe, ViewId, TEXT("vacuus_js_preact_counter"), GDocument))
	{
		return false;
	}

	FString EvalResult;
	Rig.RunOnUI([&EvalResult, ViewId, &Bundle]()
		{ EvalResult = EvalString(*FWrappedDomHost::Inner, ViewId, TCHAR_TO_UTF8(*Bundle)); });

	TestEqual(TEXT("mount: two text nodes created, rendered count:0"),
		Rig.Eval(ViewId,
			"[globalThis.EPLOG.filter(l => l.startsWith('createTextNode')).join(';'),"
			" document.getElementById('ctr').innerRML].join('|')"),
		FString(TEXT("createTextNode(\"count:\");createTextNode(0)|count:0")));

	// The bump's setState commits in the same frame's job drain (debounced on
	// Promise.resolve().then, drained after this closure -- the M4 pump
	// order), so the next Eval reads the updated tree.
	Rig.Eval(ViewId, "globalThis.bump(); 'ok'");
	TestEqual(TEXT("bump 1: a data write, NO new text node"),
		Rig.Eval(ViewId,
			"[globalThis.EPLOG[globalThis.EPLOG.length - 1],"
			" globalThis.EPLOG.filter(l => l.startsWith('createTextNode')).length,"
			" document.getElementById('ctr').innerRML].join('|')"),
		FString(TEXT("data=1|2|count:1")));

	Rig.Eval(ViewId, "globalThis.bump(); 'ok'");
	TestEqual(TEXT("bump 2: still only data writes"),
		Rig.Eval(ViewId,
			"[globalThis.EPLOG[globalThis.EPLOG.length - 1],"
			" globalThis.EPLOG.filter(l => l.startsWith('createTextNode')).length,"
			" document.getElementById('ctr').innerRML].join('|')"),
		FString(TEXT("data=2|2|count:2")));

	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
