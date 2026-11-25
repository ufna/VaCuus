// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "VaCuusEngine.h"

#include <RmlUi/Core.h>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * VaCuus patch #23 -- TWO var() SHORTHANDS THAT OVERLAP RESOLVE IN CASCADE ORDER, NOT IN HASH ORDER.
 *
 * A shorthand whose value holds var() is kept unparsed under its ShorthandId and expanded at compute time, every one
 * into the same dictionary (ElementStyle::ExpandVarShorthands), so where two overlap the one expanded last wins. The
 * definition kept them in an UnorderedMap and the expansion walked it as it lay: which of `border` and `border-color`
 * won was the hash's choice, not the selectors'. The RmlUi update 1c94a96 (1.2.0) brought robin_hood 3.11.5, whose new
 * mixer walks `border-color` before `border`, and in a game whose vendored copy moved past it every hover, focus,
 * primary and checked frame -- a `border-color: var()` state over a `border: var() var()` component -- stopped
 * showing at once.
 *
 * The patch takes the cascade's answer instead of an order: every longhand's placeholder names the top-level shorthand
 * that won it, and an expansion carries over only the longhands that name it.
 *
 * EVERY CASE HAS ITS MIRROR, so the test fails whichever way a hash happens to walk: #lit wants the narrow shorthand
 * to win (it is the more specific), #wide wants the wide one to win (now IT is), and one hash order can satisfy only
 * one of the two. #side does the same with `border-left`, which overlaps `border` in two of its eight longhands and
 * must leave the other six alone. #same and #same-mirror hold both in one rule, where the specificity is equal and the
 * declaration written later wins, whichever is the wider -- no fixed order of the two can pass both. #inline and
 * #inline-mirror are the same pair in a style attribute. #rest and #literal are the controls: one var() shorthand
 * alone, and a literal state over a var() component, which never depended on the order at all.
 *
 * RESTORE THE BUG: in ExpandVarShorthands, carry every expanded longhand over whatever its placeholder names (or parse
 * straight into out_substituted_shorthands, as upstream does). Under either hash order #lit or #wide, #side, and one
 * of each same and inline pair then report the colour of the shorthand that lost the cascade.
 */
namespace VaCuusStyleVarShorthandOrderTest
{
static const char* GContextName = "vacuus_style_var_shorthand_order_test";

static const char* GDocument = R"(<rml>
<head><style>
body { display: block; width: 100%; height: 100%; --w: 2px; --line: #102030; --hover: #a0b0c0; --left: #405060; }
div { display: block; width: 60px; height: 20px; }
.button { border: var(--w) var(--line); }
.button.lit { border-color: var(--hover); }
.wide { border-color: var(--hover); }
.wide.lit { border: var(--w) var(--line); }
.side { border: var(--w) var(--line); }
.side.lit { border-left: var(--w) var(--left); }
.same { border: var(--w) var(--line); border-color: var(--hover); }
.same-mirror { border-color: var(--hover); border: var(--w) var(--line); }
.literal { border: var(--w) var(--line); }
.literal.lit { border-color: #a0b0c0; }
</style></head>
<body>
	<div id="rest" class="button"/>
	<div id="lit" class="button lit"/>
	<div id="wide" class="wide lit"/>
	<div id="side" class="side lit"/>
	<div id="same" class="same"/>
	<div id="same-mirror" class="same-mirror"/>
	<div id="inline" style="border: var(--w) var(--line); border-color: var(--hover);"/>
	<div id="inline-mirror" style="border-color: var(--hover); border: var(--w) var(--line);"/>
	<div id="inline-over-rule" class="button" style="border-left: var(--w);"/>
	<div id="literal" class="literal lit"/>
</body>
</rml>)";
}	 // namespace VaCuusStyleVarShorthandOrderTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusStyleVarShorthandOrderTest, "VaCuus.Core.Style.VarShorthandOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusStyleVarShorthandOrderTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusStyleVarShorthandOrderTest;

	// The precondition every test that takes the library states: a live UI thread owns RmlUi on its own thread.
	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	FVaCuusEngine& Engine = FVaCuusEngine::Get();
	if (!TestTrue(TEXT("Initialized"), Engine.Initialize()))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Engine.Shutdown();
	};

	Rml::Context* Context = Rml::CreateContext(GContextName, Rml::Vector2i(400, 300));
	if (!TestNotNull(TEXT("Context"), Context))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Rml::RemoveContext(GContextName);
	};

	AddExpectedError(TEXT("Property 'border-left-color' could not be resolved: Pending substitution from a shorthand."),
		EAutomationExpectedErrorFlags::Contains, 0);
	Rml::ElementDocument* Document = Context->LoadDocumentFromMemory(GDocument, "vacuus://style_var_shorthand_order.rml");
	if (!TestNotNull(TEXT("Document"), Document))
	{
		return false;
	}
	Document->Show();
	Context->Update();
	Context->Update();

	const Rml::Colourb Line(0x10, 0x20, 0x30);
	const Rml::Colourb Hover(0xa0, 0xb0, 0xc0);
	const Rml::Colourb Left(0x40, 0x50, 0x60);

	// Reports the colour that arrived, so a failure says which shorthand won.
	const auto TestColour = [this](const TCHAR* What, const Rml::Colourb Actual, const Rml::Colourb Expected)
	{
		if (Actual != Expected)
		{
			AddError(FString::Printf(TEXT("%s: expected #%02x%02x%02x, got #%02x%02x%02x"), What, Expected.red, Expected.green,
				Expected.blue, Actual.red, Actual.green, Actual.blue));
			return false;
		}
		AddInfo(FString::Printf(TEXT("%s: #%02x%02x%02x"), What, Actual.red, Actual.green, Actual.blue));
		return true;
	};

	const auto Values = [this, Document](const char* Id) -> const Rml::Style::ComputedValues*
	{
		Rml::Element* Element = Document->GetElementById(Id);
		if (!Element)
		{
			AddError(FString::Printf(TEXT("no element '%s' -- the fixture document did not load as written"), UTF8_TO_TCHAR(Id)));
			return nullptr;
		}
		return &Element->GetComputedValues();
	};

	const Rml::Style::ComputedValues* Rest = Values("rest");
	const Rml::Style::ComputedValues* Lit = Values("lit");
	const Rml::Style::ComputedValues* Wide = Values("wide");
	const Rml::Style::ComputedValues* Side = Values("side");
	const Rml::Style::ComputedValues* Same = Values("same");
	const Rml::Style::ComputedValues* SameMirror = Values("same-mirror");
	const Rml::Style::ComputedValues* Inline = Values("inline");
	const Rml::Style::ComputedValues* InlineMirror = Values("inline-mirror");
	const Rml::Style::ComputedValues* Literal = Values("literal");
	if (!Rest || !Lit || !Wide || !Side || !Same || !SameMirror || !Inline || !InlineMirror || !Literal)
	{
		return false;
	}

	TestColour(TEXT("CONTROL #rest, one var() border alone"), Rest->border_top_color(), Line);
	TestColour(TEXT("CONTROL #literal, a literal state colour over a var() border"), Literal->border_top_color(), Hover);

	TestColour(TEXT("#lit, the more specific border-color over border (#23)"), Lit->border_top_color(), Hover);
	TestColour(TEXT("#lit, every side of it"), Lit->border_left_color(), Hover);
	TestEqual(TEXT("#lit keeps the width the border gave it"), Lit->border_top_width(), 2.0f);

	TestColour(TEXT("#wide, the more specific border over border-color (#23)"), Wide->border_top_color(), Line);

	TestColour(TEXT("#side, the more specific border-left over border (#23)"), Side->border_left_color(), Left);
	TestColour(TEXT("#side, the sides border-left does not name keep the border's"), Side->border_top_color(), Line);

	TestColour(TEXT("#same, one rule: border-color written after the border refines it (#23)"), Same->border_top_color(), Hover);
	TestColour(TEXT("#same-mirror, one rule: the border written after border-color covers it (#23)"), SameMirror->border_top_color(), Line);
	TestEqual(TEXT("#same-mirror keeps the border's width"), SameMirror->border_top_width(), 2.0f);

	TestColour(TEXT("#inline, a style attribute: border-color after the border (#23)"), Inline->border_top_color(), Hover);
	TestColour(TEXT("#inline-mirror, a style attribute: the border after border-color (#23)"), InlineMirror->border_top_color(), Line);

	Rml::Element* InlineOverRule = Document->GetElementById("inline-over-rule");
	if (!TestNotNull(TEXT("inline shorthand over a stylesheet rule"), InlineOverRule))
	{
		return false;
	}
	const Rml::Style::ComputedValues& Mixed = InlineOverRule->GetComputedValues();
	const Rml::Style::ComputedValues Defaults(nullptr);
	TestColour(TEXT("an omitted inline shorthand color resets to its initial value"), Mixed.border_left_color(), Defaults.border_left_color());
	TestColour(TEXT("the stylesheet still supplies the other sides"), Mixed.border_right_color(), Line);

	InlineOverRule->RemoveProperty("border-left-color");
	Context->Update();
	TestColour(TEXT("removing the inline placeholder reveals the stylesheet shorthand"), Mixed.border_left_color(), Line);

	InlineOverRule->SetProperty("border-left", "var(--w)");
	Context->Update();
	InlineOverRule->SetProperty("border-left-color", "#a0b0c0");
	Context->Update();
	TestColour(TEXT("a literal can replace the inline shorthand placeholder"), Mixed.border_left_color(), Hover);
	InlineOverRule->RemoveProperty("border-left-color");
	Context->Update();
	TestColour(TEXT("removing the inline color reveals the stylesheet shorthand"), Mixed.border_left_color(), Line);

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
