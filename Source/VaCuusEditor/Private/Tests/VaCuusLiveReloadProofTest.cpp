// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusDefines.h"
#include "VaCuusProofCommon.h"

#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusLiveReloadProof
{
//~ Timings. Generous rather than tight: PIE start, the first HUD layout and a shader
//~ warm-up all happen in here, and a proof that is racy is not a proof.
static constexpr float SecondsBeforeHud = 6.0f;
static constexpr float SecondsBeforeFirstShot = 5.0f;
static constexpr float SecondsOfEditWindow = 14.0f;
static constexpr float SecondsAfterSecondShot = 2.0f;

/** One frame's grace between a screenshot request and the next thing that may change pixels. */
static constexpr float SecondsAfterScreenshot = 1.0f;

static const TCHAR* GBeforeShot = TEXT("m2t10_before");
static const TCHAR* GAfterShot = TEXT("m2t10_after");
}	 // namespace VaCuusLiveReloadProof

/**
 * THE LIVE PROOF HARNESS for editor live reload (M2 Task 10). It needs a HUMAN OR A SCRIPT to
 * edit a file from OUTSIDE this process while it is running, so it is opt-in and does nothing
 * at all unless asked for -- see below.
 *
 * WHY IT HAS TO BE A PIE SESSION IN A REAL EDITOR PROCESS. Two independent, both binding
 * facts: (1) in a running session only UEditorEngine::Tick pumps IDirectoryWatcher
 * (EditorEngine.cpp:1948), so a `-game` run -- how every earlier M1/M2 screenshot was taken --
 * receives no file-change event, and VaCuusEditor is not even loaded there (EHostType::Editor
 * requires GIsEditor); (2) a VaCuus view only exists on a game instance, and the editor world
 * has none. PIE is the only configuration where both are true at once.
 *
 * (That is a statement about who ticks the watcher in a live session, NOT about testability:
 * Tick(-1.0f) fires the delegates inline, which is how VaCuus.LiveReload.WatcherEvent asserts
 * the same link headlessly. What only PIE gives is a REAL view on screen to photograph.)
 *
 * WHY IT IS OPT-IN AND NOT MERELY OUT OF THE SUITE'S NAMESPACE: the "Proof." prefix keeps it
 * out of `Automation RunTests VaCuus`, and nothing more. A human pressing "Run All" in the
 * Session Frontend would otherwise start PIE and block ~28 seconds waiting for an edit that is
 * never coming -- and then pass. So the body runs only under `-vacuusproof`, and when it does
 * run it ends by asserting that both screenshots exist (see FVaCuusProofVerifyScreenshotsCommand):
 * queueing latent commands proves nothing on its own.
 *
 * HOW TO DRIVE IT:
 *
 *   UnrealEditor <YourProject>.uproject -RenderOffscreen -resx=1920 -resy=1080 -ForceRes \
 *     -nosplash -vacuusproof -ExecCmds="Automation RunTests Proof.LiveReload.PIE" \
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

	if (!VaCuusProof::IsProofRequested())
	{
		// No PIE, no waiting, no screenshots -- and no false green either, because nothing is
		// claimed. The instructions are the whole output.
		AddInfo(TEXT("Skipped: this harness needs a file edited from outside the process while it runs. "
					 "Re-run the editor with -vacuusproof (and -RenderOffscreen for the screenshots), "
					 "wait for VACUUS_PROOF_EDIT_NOW in the log, then save Content/DevUI/m1_hud.rcss."));
		return true;
	}

	AddInfo(FString::Printf(TEXT("Screenshots will be written to '%s'"), *VaCuusProof::ScreenshotDir()));

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(/*bSimulateInEditor=*/false));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(SecondsBeforeHud));

	// The same debug toggle every earlier task photographed, so the "before" frame is
	// comparable with M1/M2's screenshots rather than a new thing.
	ADD_LATENT_AUTOMATION_COMMAND(FExecStringLatentCommand(TEXT("vacuus.M1HUD")));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(SecondsBeforeFirstShot));

	// One optional extra console command once the HUD is up and arranged; see
	// QueueProofExtraExec() for why -ExecCmds cannot do this job.
	QueueProofExtraExec(*this);

	ADD_LATENT_AUTOMATION_COMMAND(FVaCuusProofScreenshotCommand(GBeforeShot));

	// One frame's grace, then the marker: the screenshot is resolved at the end of the
	// frame it was requested in, and the edit must not land in that same frame.
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(SecondsAfterScreenshot));
	ADD_LATENT_AUTOMATION_COMMAND(FVaCuusProofMarkerCommand(VaCuusProof::GEditNowMarker));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(SecondsOfEditWindow));

	ADD_LATENT_AUTOMATION_COMMAND(FVaCuusProofScreenshotCommand(GAfterShot));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(SecondsAfterSecondShot));

	ADD_LATENT_AUTOMATION_COMMAND(FVaCuusProofMarkerCommand(VaCuusProof::GDoneMarker));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());

	// LAST, and after FEndPlayMapCommand so the pixels are on disk rather than pending: a
	// harness that cannot fail is not a test. If PIE never came up, if vacuus.M1HUD errored,
	// if the reload never landed -- one of these two files is missing and this goes red.
	ADD_LATENT_AUTOMATION_COMMAND(
		FVaCuusProofVerifyScreenshotsCommand(this, TArray<FString>({GBeforeShot, GAfterShot})));
	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
