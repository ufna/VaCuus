// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusDefines.h"

#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"
#include "UnrealClient.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusLiveReloadProof
{
/** Where the before/after pair is written. Absolute, so FScreenshotRequest uses it verbatim. */
static FString ScreenshotDir()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VaCuusProof"));
}

/** Logged verbatim so an external process can wait for the edit window instead of guessing. */
static const TCHAR* GEditNowMarker = TEXT("VACUUS_PROOF_EDIT_NOW");
static const TCHAR* GDoneMarker = TEXT("VACUUS_PROOF_DONE");

//~ Timings. Generous rather than tight: PIE start, the first HUD layout and a shader
//~ warm-up all happen in here, and a proof that is racy is not a proof.
static constexpr float SecondsBeforeHud = 6.0f;
static constexpr float SecondsBeforeFirstShot = 5.0f;
static constexpr float SecondsOfEditWindow = 14.0f;
static constexpr float SecondsAfterSecondShot = 2.0f;
}	 // namespace VaCuusLiveReloadProof

/** Takes a screenshot of the running PIE viewport, UI included, at a known absolute path. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FVaCuusProofScreenshotCommand, FString, BaseName);
bool FVaCuusProofScreenshotCommand::Update()
{
	const FString Path = VaCuusLiveReloadProof::ScreenshotDir() / BaseName;

	// bAddFilenameSuffix=false so the name is exactly what the report cites; bInShowUI=true
	// because the whole point is the RmlUi overlay.
	FScreenshotRequest::RequestScreenshot(Path, /*bInShowUI=*/true, /*bAddFilenameSuffix=*/false);

	UE_LOG(LogVaCuus, Log, TEXT("Live-reload proof: screenshot requested at '%s.png'"), *Path);
	return true;
}

/** Prints a marker the driving shell greps for. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FVaCuusProofMarkerCommand, FString, Marker);
bool FVaCuusProofMarkerCommand::Update()
{
	UE_LOG(LogVaCuus, Log, TEXT("%s"), *Marker);
	return true;
}

/**
 * THE LIVE PROOF HARNESS for editor live reload (M2 Task 10), and it is a harness rather
 * than a test: it needs a HUMAN OR A SCRIPT to edit a file from OUTSIDE this process while
 * it is running, so it can assert nothing on its own.
 *
 * WHY IT HAS TO BE A PIE SESSION IN A REAL EDITOR PROCESS. The two facts are independent
 * and both binding: (1) IDirectoryWatcher is only ticked by UEditorEngine::Tick, so a
 * `-game` run -- which is how every earlier M1/M2 screenshot was taken -- can never receive
 * a file-change event, and VaCuusEditor is not even loaded there (EHostType::Editor
 * requires GIsEditor); (2) a VaCuus view only exists on a game instance, and the editor
 * world has none. PIE is the only configuration where both are true at once.
 *
 * WHY THE NAME IS NOT UNDER "VaCuus.": the project's suite command is
 * `Automation RunTests VaCuus`, and this must never be picked up by it -- it starts PIE and
 * then waits ~25 seconds for an edit that a headless CI run will not make.
 *
 * HOW TO DRIVE IT:
 *
 *   UnrealEditor VcHost.uproject -RenderOffscreen -resx=1920 -resy=1080 -ForceRes \
 *     -nosplash -ExecCmds="Automation RunTests Proof.LiveReload.PIE" \
 *     -testexit="Automation Test Queue Empty"
 *
 * then wait for VACUUS_PROOF_EDIT_NOW in the log and edit
 * Plugins/VaCuus/Content/DevUI/m1_hud.rcss from another process. The pair lands in
 * <Project>/Saved/VaCuusProof/.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusLiveReloadProofTest, "Proof.LiveReload.PIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusLiveReloadProofTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusLiveReloadProof;

	AddInfo(FString::Printf(TEXT("Screenshots will be written to '%s'"), *ScreenshotDir()));

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(/*bSimulateInEditor=*/false));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(SecondsBeforeHud));

	// The same debug toggle every earlier task photographed, so the "before" frame is
	// comparable with M1/M2's screenshots rather than a new thing.
	ADD_LATENT_AUTOMATION_COMMAND(FExecStringLatentCommand(TEXT("vacuus.M1HUD")));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(SecondsBeforeFirstShot));

	// One optional extra console command once the HUD is up and arranged, supplied as
	// -vacuusproofexec="...". It exists because `-ExecCmds` cannot serve this purpose: every
	// command there runs on one early tick, long before PIE or the widget's first layout, so
	// a click issued from it hits an empty hit-test grid. Task 10 uses it to focus the chat
	// field before the reload (`vacuus.M1HUD.TypeShot <x> <y> <text>`), which is what makes
	// the IME teardown path (controller decision D22) observable.
	FString ExtraExec;
	if (FParse::Value(FCommandLine::Get(), TEXT("-vacuusproofexec="), ExtraExec) && !ExtraExec.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("Extra exec before the edit window: '%s'"), *ExtraExec));
		ADD_LATENT_AUTOMATION_COMMAND(FExecStringLatentCommand(ExtraExec));
		ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(4.0f));
	}

	ADD_LATENT_AUTOMATION_COMMAND(FVaCuusProofScreenshotCommand(TEXT("m2t10_before")));

	// One frame's grace, then the marker: the screenshot is resolved at the end of the
	// frame it was requested in, and the edit must not land in that same frame.
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(1.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FVaCuusProofMarkerCommand(GEditNowMarker));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(SecondsOfEditWindow));

	ADD_LATENT_AUTOMATION_COMMAND(FVaCuusProofScreenshotCommand(TEXT("m2t10_after")));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(SecondsAfterSecondShot));

	ADD_LATENT_AUTOMATION_COMMAND(FVaCuusProofMarkerCommand(GDoneMarker));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
