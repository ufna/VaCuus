// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusDefines.h"
#include "VaCuusJsScriptHost.h"
#include "VaCuusJsValue.h"
#include "VaCuusJsViewContext.h"

#include "Tests/VaCuusJsDomTestRig.h"

#if WITH_DEV_AUTOMATION_TESTS

//~ EvalString, the read-back channel every JS test uses, lives in the DOM rig's namespace.
//~ INSIDE THE GUARD, and it has to be: VaCuusJsDomTestRig.h declares that namespace within
//~ its own `#if WITH_DEV_AUTOMATION_TESTS` (:19-341), so in a Shipping build the namespace
//~ does not exist and a `using` above this line is a hard compile error -- which is exactly
//~ how BuildPlugin's third leg (UnrealGame Shipping) found it on 2026-08-09.
using VaCuusJsDomTest::EvalString;

namespace VaCuusJsCallFunctionTest
{
static constexpr uint32 TestViewId = 41;

/**
 * The argument that decides whether this whole seam is safe: a string a game could
 * plausibly hold (a player name, a chat line, a quest title) that is ALSO a complete
 * JavaScript escape from a naive `f('%s')`.
 *
 * Read it as the interpolation sees it: the leading quote closes the literal, the paren
 * closes the call, the semicolon ends the statement, `globalThis.pwned = 1` is the payload,
 * and `//` comments out whatever the format string appended. Nothing exotic -- an apostrophe
 * and a bracket -- which is the point.
 */
static const TCHAR* GInjection = TEXT("');globalThis.pwned = 1;//");

/** A second one, for the escaping-is-not-enough half: quotes, backslash, newline, NUL-ish. */
static const TCHAR* GAwkward = TEXT("he said \"hi\" \\ then\nnewline 'and' `ticks` ${notATemplate}");
}	 // namespace VaCuusJsCallFunctionTest

/**
 * CALLING JS FROM THE GAME WITHOUT WRITING JS (VaCuus-asv).
 *
 * The four things a caller is promised: the function runs, the arguments arrive as the
 * values they were, `this` is what a hand-written call would have given, and a path that
 * resolves to nothing is a Warning rather than a throw.
 */
//~ ".Arguments", NOT plain "VaCuus.Js.CallFunction", and the suffix is load-bearing. Written
//~ the short way first, this test SILENTLY NEVER RAN: a test whose full name is a strict
//~ prefix of another's is shadowed by it in the automation controller's tree -- the run
//~ reported only ".Refusals" and a green 66/66, with no skip line anywhere. Both names are
//~ leaves under a shared group now.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsCallFunctionArgumentsTest, "VaCuus.Js.CallFunction.Arguments",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusJsCallFunctionArgumentsTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsCallFunctionTest;

	FVaCuusJsScriptHost Host;
	Host.OnViewAdded(TestViewId);

	Host.ExecuteScript(TestViewId,
		TEXT("globalThis.log = [];")
		TEXT("globalThis.nested = { deep: { fn(...a) { log.push(JSON.stringify(a)); return 1; } } };")
		TEXT("globalThis.thisTag = null;")
		TEXT("vacuus.onThing = function(...a) { log.push(JSON.stringify(a)); thisTag = (this === vacuus) ? 'vacuus' : 'other'; };")
		TEXT("globalThis.bare = function() { thisTag = (this === globalThis) ? 'global' : 'other'; };")
		TEXT("globalThis.notAFunction = 7;"),
		TEXT("call-fn-setup"));

	// ---- 1. It calls, and `this` is the owner of the last segment. ----
	Host.CallFunction(TestViewId, TEXT("vacuus.onThing"), {});
	TestEqual(TEXT("a dotted path calls the function"), EvalString(Host, TestViewId, "log.length"), FString(TEXT("1")));
	TestEqual(TEXT("...with `this` bound to the owner, as a written call would"),
		EvalString(Host, TestViewId, "thisTag"), FString(TEXT("vacuus")));

	Host.CallFunction(TestViewId, TEXT("bare"), {});
	TestEqual(TEXT("a single-segment path gets globalThis, as a bare call would"),
		EvalString(Host, TestViewId, "thisTag"), FString(TEXT("global")));

	// TWO, not three: `bare` above sets thisTag and pushes nothing, so this is entry number
	// two in the log. (Written as three first, which the test caught -- the assertion is about
	// the three-segment path resolving, and it has to count what actually logs.)
	Host.CallFunction(TestViewId, TEXT("nested.deep.fn"), {});
	TestEqual(TEXT("a three-segment path resolves and calls"), EvalString(Host, TestViewId, "log.length"),
		FString(TEXT("2")));

	// ---- 2. All four value kinds arrive as themselves, in order. ----
	Host.ExecuteScript(TestViewId, TEXT("log.length = 0;"), TEXT("reset"));
	Host.CallFunction(TestViewId, TEXT("vacuus.onThing"),
		{FVaCuusJsValue::MakeBool(true), FVaCuusJsValue::MakeNumber(42.5), FVaCuusJsValue::MakeString(TEXT("hello")),
			FVaCuusJsValue::MakeNull()});
	TestEqual(TEXT("bool, number, string and null cross as themselves and in order"),
		EvalString(Host, TestViewId, "log[0]"), FString(TEXT("[true,42.5,\"hello\",null]")));
	TestEqual(TEXT("...and their JS types are the primitive ones, not strings of them"),
		EvalString(Host, TestViewId,
			"(function(){const a=JSON.parse(log[0]);return typeof a[0]+','+typeof a[1]+','+typeof a[2]+','+(a[3]===null);})()"),
		FString(TEXT("boolean,number,string,true")));

	// ---- 3. THE INJECTION, which is the reason this API exists. ----
	Host.ExecuteScript(TestViewId, TEXT("log.length = 0; delete globalThis.pwned;"), TEXT("reset-injection"));
	Host.CallFunction(TestViewId, TEXT("vacuus.onThing"), {FVaCuusJsValue::MakeString(GInjection)});

	TestEqual(TEXT("the injection payload did NOT execute"), EvalString(Host, TestViewId, "typeof globalThis.pwned"),
		FString(TEXT("undefined")));
	TestEqual(TEXT("...it arrived as ONE string argument"), EvalString(Host, TestViewId, "JSON.parse(log[0]).length"),
		FString(TEXT("1")));
	TestEqual(TEXT("...byte for byte what was sent"), EvalString(Host, TestViewId, "JSON.parse(log[0])[0]"),
		FString(GInjection));

	// Quotes, a backslash, a newline, an apostrophe and a template-looking sequence: the
	// characters an escaping scheme has to get right one at a time, and that this path never
	// has to escape at all.
	Host.ExecuteScript(TestViewId, TEXT("log.length = 0;"), TEXT("reset-awkward"));
	Host.CallFunction(TestViewId, TEXT("vacuus.onThing"), {FVaCuusJsValue::MakeString(GAwkward)});
	TestEqual(TEXT("a string of every character an escaper fears survives verbatim"),
		EvalString(Host, TestViewId, "JSON.parse(log[0])[0]"), FString(GAwkward));

	// ---- 4. RESTORE-THE-BUG: the idiom this replaces really is injectable. ----
	//
	// Not a hypothetical, and not a claim this file should make without showing: the SAME
	// string, through the SAME view, via the interpolation VaCuusRender.cpp used to write, sets
	// the flag that CallFunction refused to set. If this half ever stops failing to protect,
	// the fixture below is wrong and the assertion above means nothing.
	Host.ExecuteScript(TestViewId, TEXT("delete globalThis.pwned;"), TEXT("reset-oldidiom"));
	Host.ExecuteScript(TestViewId,
		FString::Printf(TEXT("if (typeof vacuus.onThing === 'function') vacuus.onThing('%s');"), GInjection),
		TEXT("the-old-idiom"));
	TestEqual(TEXT("the OLD idiom executes the payload -- which is why the new one exists"),
		EvalString(Host, TestViewId, "globalThis.pwned"), FString(TEXT("1")));

	Host.Shutdown();
	return true;
}

/**
 * THE REFUSALS, all of which are Warnings and none of which throw.
 *
 * The old idiom wrapped every call in `typeof f === 'function'` for exactly one reason: a
 * document that never registered the callback (or failed to load) must not turn a console
 * command into a JS exception. That guard is the API's now, so it has to be tested here or
 * every call site is back to writing it by hand.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsCallFunctionRefusalTest, "VaCuus.Js.CallFunction.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusJsCallFunctionRefusalTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsCallFunctionTest;

	AddExpectedMessagePlain(TEXT("CallJs('vacuus.neverRegistered')"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("CallJs('notAFunction')"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("CallJs('notAFunction.deeper')"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("CallJs on view"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("CallJs('vacuus.onThing') for unknown view"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);

	FVaCuusJsScriptHost Host;
	Host.OnViewAdded(TestViewId);
	Host.ExecuteScript(TestViewId,
		TEXT("globalThis.canary = 0;")
		TEXT("globalThis.notAFunction = 7;")
		TEXT("vacuus.onThing = function() { canary++; };"),
		TEXT("refusal-setup"));

	// A name nobody registered: the case the hand-written guard existed for.
	Host.CallFunction(TestViewId, TEXT("vacuus.neverRegistered"), {FVaCuusJsValue::MakeBool(true)});

	// Resolves, but to a number.
	Host.CallFunction(TestViewId, TEXT("notAFunction"), {});

	// A non-object in the MIDDLE of the path: `7.deeper` is not a lookup that can succeed.
	Host.CallFunction(TestViewId, TEXT("notAFunction.deeper"), {});

	// An empty path at all.
	Host.CallFunction(TestViewId, TEXT(""), {});

	// NONE of it threw, and none of it ran anything: the context is still healthy and the one
	// real function is still callable afterwards.
	TestEqual(TEXT("no refusal ran a function"), EvalString(Host, TestViewId, "canary"), FString(TEXT("0")));
	TestEqual(TEXT("no refusal left an exception pending"), EvalString(Host, TestViewId, "1 + 1"), FString(TEXT("2")));
	Host.CallFunction(TestViewId, TEXT("vacuus.onThing"), {});
	TestEqual(TEXT("...and the context still calls what does exist"), EvalString(Host, TestViewId, "canary"),
		FString(TEXT("1")));

	// An unknown VIEW is the loud one -- Error, not Warning -- because that is a wiring
	// mistake in the game, not a document that happens not to listen. ExecuteScript's rule.
	Host.CallFunction(TestViewId + 999, TEXT("vacuus.onThing"), {});

	Host.Shutdown();
	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
