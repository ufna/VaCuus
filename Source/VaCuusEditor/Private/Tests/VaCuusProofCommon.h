// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaCuusDefines.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"
#include "UnrealClient.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Shared plumbing for the LIVE PROOF HARNESSES -- the automation tests that drive a real PIE
 * session in a real editor process to photograph behaviour a headless test cannot reach.
 *
 * WHY THESE LIVE IN A HEADER RATHER THAN NEXT TO ONE TEST: none of it is specific to the task
 * that happened to need it first. The screenshot pair, the log markers an external script
 * greps for, and the "-vacuusproofexec=" hook are the same three needs every proof has, and
 * the third one in particular was written for Task 10's harness while its only documented
 * purpose (a click that must land after the first layout) belongs to Task 9's IME work.
 *
 * EVERY PROOF MUST BE OPT-IN. See IsProofRequested(): a harness that starts PIE and waits for
 * a human is a trap in a "Run All", and one that returns true without asserting anything is
 * worse than no test.
 */
namespace VaCuusProof
{
/** Where before/after pairs are written. Absolute, so FScreenshotRequest uses it verbatim. */
inline FString ScreenshotDir()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VaCuusProof"));
}

/** Logged verbatim so an external process can wait for the edit window instead of guessing. */
inline const TCHAR* GEditNowMarker = TEXT("VACUUS_PROOF_EDIT_NOW");
inline const TCHAR* GDoneMarker = TEXT("VACUUS_PROOF_DONE");

/**
 * The command-line opt-in every proof harness gates its body on: `-vacuusproof`.
 *
 * NOT EAutomationTestFlags::Disabled, which is the obvious-looking alternative and is wrong:
 * FAutomationTestFramework::GetValidTestNames() skips a Disabled test outright
 * (AutomationTest.cpp:870-871, flag at AutomationTest.h:111), and that is the same
 * enumeration the automation worker answers a controller's test-list request with
 * (AutomationWorkerModule.cpp:388) -- so the explicit `Automation RunTests Proof.<name>`
 * invocation each harness documents would stop resolving at all. A run-time gate keeps the
 * test discoverable and callable while making a "Run All" cost nothing.
 */
inline bool IsProofRequested()
{
	return FParse::Param(FCommandLine::Get(), TEXT("vacuusproof"));
}

/** How long to let an extra exec settle (a click needs a layout pass, then a frame). */
inline constexpr float SecondsAfterExtraExec = 4.0f;
}	 // namespace VaCuusProof

/** Takes a screenshot of the running PIE viewport, UI included, at a known absolute path. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FVaCuusProofScreenshotCommand, FString, BaseName);
inline bool FVaCuusProofScreenshotCommand::Update()
{
	const FString Path = VaCuusProof::ScreenshotDir() / BaseName;

	// bAddFilenameSuffix=false so the name is exactly what the report cites; bInShowUI=true
	// because the whole point is the RmlUi overlay.
	FScreenshotRequest::RequestScreenshot(Path, /*bInShowUI=*/true, /*bAddFilenameSuffix=*/false);

	UE_LOG(LogVaCuus, Log, TEXT("VaCuus proof: screenshot requested at '%s.png'"), *Path);
	return true;
}

/** Prints a marker the driving shell greps for. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FVaCuusProofMarkerCommand, FString, Marker);
inline bool FVaCuusProofMarkerCommand::Update()
{
	UE_LOG(LogVaCuus, Log, TEXT("%s"), *Marker);
	return true;
}

/**
 * Fails the test unless every named screenshot (base name, no extension) exists.
 *
 * THE POINT OF IT: without this a proof harness returns true the moment its latent commands
 * are QUEUED -- PIE never coming up, the console command erroring and no image being written
 * would all still be green. Queued is not proved.
 */
DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
	FVaCuusProofVerifyScreenshotsCommand, FAutomationTestBase*, Test, TArray<FString>, BaseNames);
inline bool FVaCuusProofVerifyScreenshotsCommand::Update()
{
	for (const FString& BaseName : BaseNames)
	{
		const FString Path = VaCuusProof::ScreenshotDir() / BaseName + TEXT(".png");
		Test->TestTrue(FString::Printf(TEXT("The proof screenshot '%s' was written"), *Path),
			IFileManager::Get().FileExists(*Path));
	}
	return true;
}

/**
 * Queues the optional extra console command supplied as -vacuusproofexec="...", plus time for
 * it to settle. Returns true if one was queued.
 *
 * IT EXISTS BECAUSE `-ExecCmds` CANNOT SERVE THIS PURPOSE: every command there runs on one
 * early tick, long before PIE or the widget's first layout, so a click issued from it hits an
 * empty hit-test grid. Task 10's harness uses it to focus the chat field before the reload
 * (`vacuus.M1HUD.TypeShot <x> <y> <text>`), which is what makes Task 9's IME teardown path
 * (controller decision D22) observable at all.
 */
inline bool QueueProofExtraExec(FAutomationTestBase& Test)
{
	FString ExtraExec;
	if (!FParse::Value(FCommandLine::Get(), TEXT("-vacuusproofexec="), ExtraExec) || ExtraExec.IsEmpty())
	{
		return false;
	}

	Test.AddInfo(FString::Printf(TEXT("Extra exec before the edit window: '%s'"), *ExtraExec));
	ADD_LATENT_AUTOMATION_COMMAND(FExecStringLatentCommand(ExtraExec));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(VaCuusProof::SecondsAfterExtraExec));
	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
