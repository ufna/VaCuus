// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "VaCuusEngine.h"

#include <RmlUi/Core.h>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * BEAD VaCuus-6gj — A `transition` WHOSE VALUE CONTAINS var() MUST STILL RUN, and before the
 * vendored patch it did not: the whole declaration was discarded with no warning, no error and
 * no log line of any kind. Found in the 2d6 demo, where every transition in the project took its
 * duration from a custom property, so not one of ~30 transitions had ever run across four
 * component batches and three screens — reviewed on screen at every step, and never noticed,
 * because the failure mode is "the element simply snaps".
 *
 * THE MECHANISM, both halves opened. A value containing var() is stored UNPARSED as
 * Property{value, Unit::VAR_EXPRESSION} (PropertySpecification.cpp:260-267) and resolved at
 * compute time. Every other property therefore works with var() precisely BECAUSE it is read
 * late — but ElementStyle::TransitionPropertyChanges reads `transition` EARLY and on purpose
 * ("to intercept property changes even before the computed values are ready",
 * ElementStyle.cpp:388), so it saw a String variant, failed its `!= Variant::TRANSITIONLIST`
 * test and returned. Silently. That asymmetry is invisible from outside a debugger.
 *
 * `animation` IS NOT AFFECTED, which is why this test asserts it as loudly as the broken half.
 * The bug report named both; the source does not. Animations are read through
 * ComputedValues::animation(), which goes via GetLocalPropertyWithResolvedVariables and
 * therefore resolves (ComputedValues.cpp:8-16), and the measured behaviour agrees — with patch
 * #4 fully reverted, #anim-variable still animates. Pinning it here stops a future reader from
 * "fixing" a half that was never broken, or from deleting the animation path in the belief that
 * it shares the defect.
 *
 * WHY THE OBSERVABLE IS TIME-INDEPENDENT where it needs to be. Element::StartTransition writes
 * the START value as a local property the moment the transition is added (Element.cpp:2688-2689),
 * so one Context::Update() after the class toggle separates the two outcomes with no clock
 * involved: a transition that started reads 1.0, a transition that was dropped reads the target
 * 0.25. The clock is then used ONCE, deliberately, to prove the DURATION came from the variable
 * and not from some default — #quick (0.05s from --fast) must have finished after 250 ms while
 * #variable (20s from --slow) must have barely moved.
 *
 * RESTORE THE BUG: revert patch #4 in Source/ThirdParty/RmlUi/VENDORED_TAG.txt (drop the
 * VAR_EXPRESSION resolution from ElementStyle::TransitionPropertyChanges) and the "(6gj)"
 * transition assertions below report 0.2500 where they demand 1.0000 — the snap, exactly as the
 * demo saw it. Every control keeps passing, which is what makes the failure legible.
 */
namespace VaCuusStyleVariableTest
{
static const char* GContextName = "vacuus_style_variable_test";

/**
 * Five transition/animation shapes differing ONLY in whether the timing comes from a literal or
 * from a custom property, so a divergence between a pair can be nothing else.
 *
 * The tween keyword is `linear-in-out`, not `linear`: the keyword table holds eleven families
 * each with -in/-out/-in-out and no bare family name (PropertyParserAnimation.cpp:34-76), so
 * `transition: opacity 20s linear` is a syntax error that discards the declaration — the trap
 * documented as gotchas.md #3, and one this fixture walked into on its first run.
 */
static const char* GDocument = R"(<rml>
<head><style>
body { display: block; width: 100%; height: 100%; --slow: 20s; --fast: 0.05s; }
div { display: block; width: 100px; height: 20px; opacity: 1.0; }
#literal { transition: opacity 20s linear-in-out; }
#variable { transition: opacity var(--slow) linear-in-out; }
#quick { transition: opacity var(--fast) linear-in-out; }
#instant { }
.dim { opacity: 0.25; }
@keyframes creep { from { opacity: 0.25; } to { opacity: 1.0; } }
#anim-literal { animation: 20s linear-in-out creep; }
#anim-variable { animation: var(--slow) linear-in-out creep; }
</style></head>
<body>
	<div id="literal"/>
	<div id="variable"/>
	<div id="quick"/>
	<div id="instant"/>
	<div id="anim-literal"/>
	<div id="anim-variable"/>
</body>
</rml>)";
}	 // namespace VaCuusStyleVariableTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusStyleVariableTest, "VaCuus.Core.Style.TransitionVariable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusStyleVariableTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusStyleVariableTest;

	// Same precondition, same wording, as every other test that takes the library: a live UI
	// thread owns RmlUi on its own thread, and Initialize() from here would trip the
	// owner-thread check() rather than fail politely.
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

	Rml::ElementDocument* Document = Context->LoadDocumentFromMemory(GDocument, "vacuus://style_variable.rml");
	if (!TestNotNull(TEXT("Document"), Document))
	{
		return false;
	}
	Document->Show();
	Context->Update();
	Context->Update();

	// The cascaded value, which is where a running transition or animation deposits its current
	// frame: both write the interpolated value as a LOCAL property on the element
	// (Element::SetProperty from StartTransition, Element.cpp:2688-2689, and from
	// AdvanceAnimations, :2844-2848), and a local property wins the cascade.
	const auto Opacity = [this, Document](const char* Id) -> float
	{
		Rml::Element* Element = Document->GetElementById(Id);
		if (!Element)
		{
			AddError(FString::Printf(TEXT("no element '%s' — the fixture document did not load as written"), UTF8_TO_TCHAR(Id)));
			return -1.0f;
		}
		const Rml::Property* Property = Element->GetProperty(Rml::PropertyId::Opacity);
		return Property ? Property->Get<float>() : -1.0f;
	};

	// Reports the value it saw, because "expected 1.0" without the 0.25 that arrived does not
	// tell a reader which of the two failure shapes they are looking at.
	const auto TestOpacity = [this](const TCHAR* What, float Actual, float Expected, float Tolerance)
	{
		if (!FMath::IsNearlyEqual(Actual, Expected, Tolerance))
		{
			AddError(FString::Printf(TEXT("%s: expected opacity %.4f (±%.4f), got %.4f"), What, Expected, Tolerance, Actual));
			return false;
		}
		AddInfo(FString::Printf(TEXT("%s: opacity %.4f"), What, Actual));
		return true;
	};

	// ---- 1. AT REST: the animation half, and the reading that means "nothing is animating". ----
	//
	// The keyframes run 0.25 -> 1.0 while the elements' own rule says 1.0, so a running animation
	// pins them near 0.25 and a dropped one leaves them at 1.0. #instant carries neither and
	// establishes that 1.0 really is the not-animating reading.
	TestOpacity(TEXT("CONTROL #instant at rest, no animation and no transition"), Opacity("instant"), 1.0f, 0.001f);
	TestOpacity(TEXT("CONTROL #anim-literal, a literal-duration animation runs"), Opacity("anim-literal"), 0.25f, 0.01f);
	TestOpacity(TEXT("#anim-variable, animation with var() runs too — it was NEVER the broken half (6gj)"), Opacity("anim-variable"),
		0.25f, 0.01f);

	// ---- 2. THE TRANSITION HALF. ----

	for (const char* Id : {"literal", "variable", "quick", "instant"})
	{
		Document->GetElementById(Id)->SetClass("dim", true);
	}
	Context->Update();
	Context->Update();

	TestOpacity(TEXT("CONTROL #instant snaps to the new value with no transition declared"), Opacity("instant"), 0.25f, 0.001f);
	TestOpacity(TEXT("CONTROL #literal, a literal-duration transition holds its start value"), Opacity("literal"), 1.0f, 0.01f);
	TestOpacity(TEXT("#variable, transition with var() in its duration STARTS rather than snapping (6gj)"), Opacity("variable"), 1.0f,
		0.01f);

	// ---- 3. THE DURATION REALLY CAME FROM THE VARIABLE. ----
	//
	// Without this, "it started" would also be satisfied by a transition that took its duration
	// from somewhere else entirely. 250 ms of real time finishes --fast (0.05s) and is 1.25% of
	// --slow (20s), so the two var()-driven elements must land on opposite ends.
	FPlatformProcess::Sleep(0.25f);
	Context->Update();
	Context->Update();

	TestOpacity(TEXT("#quick, var(--fast) = 0.05s, has finished after 250 ms (6gj)"), Opacity("quick"), 0.25f, 0.01f);
	// The band is 0.89-1.01 rather than a point: it has to absorb whatever real time a loaded
	// machine spends between the sleep and the update (0.89 is 2.9 s of a 20 s tween), while
	// still separating decisively from the 0.25 that any too-short duration would produce.
	TestOpacity(TEXT("#variable, var(--slow) = 20s, has barely moved after 250 ms"), Opacity("variable"), 0.95f, 0.06f);
	TestOpacity(TEXT("CONTROL #literal, the literal 20s, has barely moved either"), Opacity("literal"), 0.95f, 0.06f);

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
