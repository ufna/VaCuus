// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

/**
 * vacuus.LobbyDemo's refusal contract (the honest test for a content-dependent
 * command): the demo's documents live in the HOST PROJECT's Content/DevUI, so in this
 * plugin's own automation host they are absent by construction -- and the command must
 * answer that with ONE Error naming the missing document and create NOTHING, before it
 * ever asks for a viewport or a subsystem. A crash, a viewport error, or a silent
 * return here would each be a different bug.
 */

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "VaCuusContentPaths.h"

#include "Engine/Engine.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusLobbyDemoRefusalTest, "VaCuus.Render.LobbyDemo.RefusesWithoutContent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusLobbyDemoRefusalTest::RunTest(const FString& Parameters)
{
	// The precondition IS the point: this test only means something where the demo's
	// content is genuinely absent. A host project that ships a chrome.rml turns the
	// refusal into a viewport complaint, which is a different (and legitimate) path.
	if (!VaCuusContentPaths::ResolveExistingDocument(TEXT("chrome.rml")).IsEmpty())
	{
		AddInfo(TEXT("chrome.rml is served by a DevUI root here; the refusal path is not reachable. Skipping."));
		return true;
	}

	if (GEngine == nullptr)
	{
		AddInfo(TEXT("No GEngine; console commands cannot be dispatched. Skipping."));
		return true;
	}

	// Exactly one Error, and it must NAME the document: "the demo is broken" without
	// the file name is the failure mode the named refusal exists to prevent.
	AddExpectedError(TEXT("vacuus.LobbyDemo: 'chrome.rml' is not served by any DevUI root"),
		EAutomationExpectedErrorFlags::Contains, 1);

	// Through the console dispatcher, the way a user reaches it. In this headless host
	// there is no game viewport either -- so if the content check were missing or
	// ordered after the viewport check, the Error above would never appear and the
	// expected-error accounting would fail the test.
	GEngine->Exec(nullptr, TEXT("vacuus.LobbyDemo"));

	// The refusal must also be a TOGGLE no-op: a second invocation must refuse again
	// (nothing was half-created that a second call would tear down or trip over).
	AddExpectedError(TEXT("vacuus.LobbyDemo: 'chrome.rml' is not served by any DevUI root"),
		EAutomationExpectedErrorFlags::Contains, 1);
	GEngine->Exec(nullptr, TEXT("vacuus.LobbyDemo"));

	return true;
}

/**
 * vacuus.LobbyDemo.Backdrop's own refusals. The command creates and destroys a VIEW now,
 * so "no live demo" is the difference between a no-op and a view with no stack to sit
 * under -- and the argument check is what stops `Backdrop` (no argument at all) from being
 * read as `Backdrop 0` by FCString::Atoi, which answers 0 for any unparseable string.
 *
 * Both are reachable in this host with no demo running, which is the only state automation
 * has: there is no game viewport here, so no demo can exist to be found.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusLobbyDemoBackdropRefusalTest, "VaCuus.Render.LobbyDemo.BackdropRefuses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusLobbyDemoBackdropRefusalTest::RunTest(const FString& Parameters)
{
	if (GEngine == nullptr)
	{
		AddInfo(TEXT("No GEngine; console commands cannot be dispatched. Skipping."));
		return true;
	}

	// No argument: refused on the ARGUMENT, before anything looks for a demo. The two
	// errors are distinct on purpose -- an argument-less call that reported "no live lobby
	// demo" would send the reader looking for the wrong problem.
	//
	// PLAIN, not the regex form the rest of this file uses: AddExpectedError treats its
	// pattern as a regex by default (AutomationTest.h:1854, `bool IsRegex = true`), and
	// this message's literal `[delaySeconds]` would be read as a character class matching
	// one letter. AddExpectedErrorPlain (:1866) compares the text as text.
	AddExpectedErrorPlain(TEXT("vacuus.LobbyDemo.Backdrop expects <0|1> [delaySeconds]"),
		EAutomationExpectedErrorFlags::Contains, 1);
	GEngine->Exec(nullptr, TEXT("vacuus.LobbyDemo.Backdrop"));

	// Argument present, no demo: refused on the STATE. Both directions, because create and
	// destroy take different paths to the same answer and only one of them used to exist.
	AddExpectedError(TEXT("vacuus.LobbyDemo.Backdrop: no live lobby demo"),
		EAutomationExpectedErrorFlags::Contains, 2);
	GEngine->Exec(nullptr, TEXT("vacuus.LobbyDemo.Backdrop 1"));
	GEngine->Exec(nullptr, TEXT("vacuus.LobbyDemo.Backdrop 0"));

	return true;
}

#endif	  // WITH_AUTOMATION_TESTS
