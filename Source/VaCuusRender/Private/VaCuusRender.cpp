// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "SVaCuusWidget.h"
#include "VaCuusContentPaths.h"
#include "VaCuusDefines.h"
#include "VaCuusDemoModel.h"
#include "VaCuusRecordingRenderInterface.h"
#include "VaCuusRmlDocumentHost.h"
#include "VaCuusSlateElement.h"
#include "VaCuusSubsystem.h"
#include "VaCuusView.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Input/Events.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "RenderingThread.h"
#include "ShaderCore.h"
#include "UnrealClient.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

namespace VaCuusM1HUD
{
/**
 * The demo documents this toggle can bring up. FVaCuusFileInterface resolves relative
 * paths against the ordered DevUI roots (plugin's Content/DevUI first -- D19).
 */
static const TCHAR* GM1HudVfsPath = TEXT("m1_hud.rml");
static const TCHAR* GM2DemoVfsPath = TEXT("m2_demo.rml");
static const TCHAR* GM3DemoVfsPath = TEXT("m3_demo.rml");

/**
 * The name m3_demo.rml's `data-model` attribute writes, and the name `vacuus.DumpModel hud`
 * takes. One constant so the document, the bind and the dump cannot drift apart -- a mismatch
 * here produces an inert document whose only complaint is RmlUi's own `[Rml] Could not locate
 * data model ...` at Error (Element.cpp:2218 via FVaCuusSystemInterface::LogMessage): a real
 * log line, but one that names the element rather than the two constants that disagreed.
 */
static const TCHAR* GM3ModelName = TEXT("hud");

/**
 * Which document the toggle is showing, or will show next.
 *
 * A VARIABLE RATHER THAN TWO COPIES OF EVERYTHING (Task 14): vacuus.M2Demo differs from
 * vacuus.M1HUD in exactly one respect, the file name, and every sub-command below --
 * .Mouse, .Nav, .NavShot, .Type, .TypeShot, .HoverShot, .PassThroughKey, and the
 * .AutoShot/.PerfLog cvars in other files -- is about the view rather than the document.
 * Duplicating the toggle would have doubled the fallback, live-reload re-arm and teardown
 * logic for a string, and the second copy is where the two would have drifted.
 *
 * Only ever written by Toggle(), i.e. on the game thread from a console command.
 */
static const TCHAR* GDocumentVfsPath = GM1HudVfsPath;

/**
 * Inline fallback document, used when DevUI/m1_hud.rml is missing under every root OR
 * when it exists but fails to load (a broken RML/RCSS edit is exactly when a
 * working fallback matters most).
 * The pure red and pure blue divs are the channel-order probe: if the left
 * div renders blue, an RGBA/BGRA swap crept into the vertex or texture path.
 */
static const TCHAR* GTestDocumentRml = TEXT(R"(<rml>
<head>
<title>VaCuus M1</title>
<style>
body
{
	display: block;
	font-family: LatoLatin;
	font-size: 24px;
	width: 100%;
	height: 100%;
}
div { display: block; position: absolute; }
#red   { left: 60px;  top: 60px;  width: 160px; height: 120px; background-color: #FF0000; }
#blue  { left: 260px; top: 60px;  width: 160px; height: 120px; background-color: #0000FF; }
#panel { left: 60px;  top: 220px; width: 480px; height: 100px; background-color: #000000A0; }
#label { left: 80px;  top: 252px; color: #FFFFFF; }
</style>
</head>
<body>
	<div id="red"/>
	<div id="blue"/>
	<div id="panel"/>
	<div id="label">VaCuus M1</div>
</body>
</rml>)");

/**
 * Puts the local player controller into an input mode that lets Slate route pointer
 * events to viewport overlays at all, and takes it back out again.
 *
 * NOT A NICETY -- WITHOUT IT NO INPUT REACHES THE UI. Under the default
 * FInputModeGameOnly the game viewport holds mouse capture, so
 * FSlateApplication routes every pointer event straight to the captor (the SViewport)
 * and never runs a hit test; SVaCuusWidget's handlers are simply never called, no
 * matter what its visibility says. This was observed, not assumed: with GameOnly, a
 * synthesized move over the demo button reported "handled by a widget" (the captor)
 * and RmlUi's hover never changed.
 *
 * GameAndUI rather than UIOnly, because Task 6's contract is that clicks the UI does
 * not claim still reach the game -- which is exactly what GameAndUI means.
 *
 * This belongs to the debug toggle, not to the plugin: a real game decides its own
 * input mode, and a UMG-hosted VaCuus widget (Task 8) inherits whatever the game
 * already set.
 */
static void SetUIInputMode(UWorld* World, bool bEnable)
{
	APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr;
	if (!PlayerController)
	{
		UE_LOG(LogVaCuus, Warning,
			TEXT("VaCuus demo: no player controller, so the input mode is unchanged; pointer input will not reach the UI"));
		return;
	}

	if (bEnable)
	{
		FInputModeGameAndUI Mode;
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		Mode.SetHideCursorDuringCapture(false);
		PlayerController->SetInputMode(Mode);
		PlayerController->SetShowMouseCursor(true);
	}
	else
	{
		PlayerController->SetInputMode(FInputModeGameOnly());
		PlayerController->SetShowMouseCursor(false);
	}

	UE_LOG(LogVaCuus, Log, TEXT("VaCuus demo: input mode is now %s"),
		bEnable ? TEXT("GameAndUI (pointer events reach viewport overlays)") : TEXT("GameOnly"));
}

/**
 * Everything the HUD toggle owns while it is ON.
 *
 * The view is owned by UVaCuusSubsystem and the UI thread by FVaCuusModule; this
 * state is only the toggle's own bookkeeping.
 */
struct FState
{
	TWeakObjectPtr<UVaCuusSubsystem> Subsystem;
	TWeakObjectPtr<UVaCuusView> View;
	TSharedPtr<FVaCuusSlateElement> Element;
	TSharedPtr<SVaCuusWidget> Widget;
	TWeakObjectPtr<UGameViewportClient> Viewport;
	FDelegateHandle WorldTearDownHandle;

	/** World whose player controller the toggle put into GameAndUI; restored on teardown. */
	TWeakObjectPtr<UWorld> InputWorld;

	/** True while the pending load is the VFS document, i.e. while a fallback is possible. */
	bool bLoadingFromFile = false;

	/** One shot only per attempt: a failing inline fallback must not loop. */
	bool bTriedInlineFallback = false;

	/**
	 * True once ANY document has loaded successfully into this view.
	 *
	 * Only a log line depends on it, and it is worth a bool: AdoptDocument keeps the
	 * previous document up when a load fails (VaCuusRmlDocumentHost.cpp:176-184), so
	 * "the HUD stays empty" is true for a first load and false for every re-load -- and
	 * telling someone who just saved a typo that their HUD is empty when it is showing
	 * the last good version sends them looking in the wrong place.
	 */
	bool bAnyDocumentShown = false;

	/** Subscription to UVaCuusSubsystem::OnDocumentsReloadRequested; see OnDocumentsReloadRequested. */
	FDelegateHandle ReloadRequestedHandle;

	//~ ------------------------------------------------------- M3a data binding (plan Task 9.1)

	/**
	 * The live gameplay struct `vacuus.M3Demo` drives, standing in for whatever a real game
	 * would push. Owned by the toggle, on the game thread, and never seen by the UI thread:
	 * UpdateModel diffs it into the model's own shadow and only the shadow crosses over.
	 */
	FVaCuusDemoModel DemoModel;

	/** OnBeginFrame subscription that pumps DemoModel; only bound while the M3 demo is up. */
	FDelegateHandle ModelDriverHandle;

	/** Seconds the driver has been running, so the animation is wall-clock and not frame-rate. */
	double ModelDriverStartSeconds = 0.0;

	/**
	 * `vacuus.M3Demo.Freeze 1`: keep calling UpdateModel every frame with UNCHANGED data.
	 *
	 * THAT IS SPEC 9's IDLE ROW, ON A REAL SCREEN. "Nobody called UpdateModel" would be the
	 * weaker case and the one that proves nothing; this is a game pushing its HUD struct every
	 * tick, unchanged, which must cost zero published frames and zero applied fields. Both
	 * numbers are readable while it is frozen -- UVaCuusView::GetFramesPublished through
	 * vacuus.M1HUD.PerfLog, and the applied-field count through vacuus.DumpModel.
	 *
	 * The killfeed freezes with everything else, by construction rather than by a check of its
	 * own: PumpDemoModel returns from the frozen branch before any mutation, so the array rides
	 * the same byte-identical struct -- an array field that kept appending under Freeze would
	 * publish every 1.5 s and quietly destroy the idle row this switch exists to prove.
	 */
	bool bModelFrozen = false;

	/**
	 * The 1.5-second beat (floor(Elapsed / 1.5)) of the last appended killfeed row. Kept as a
	 * beat rather than a timestamp because Elapsed itself is what Freeze's thaw rebases to zero
	 * -- FreezeDemoModel resets this alongside ModelDriverStartSeconds, or the pump would
	 * append nothing until the new clock caught up with the old beat count.
	 */
	int32 LastKillfeedBeat = 0;

	/**
	 * Rows appended since the demo came up, monotonic across Freeze/thaw. This is what the row
	 * CONTENT derives from, and it is deliberately not the beat: the beat resets on thaw, and a
	 * feed that replayed the same kills after a thaw would look exactly like the stale-array
	 * bug the demo exists to rule out.
	 */
	int32 KillfeedSerial = 0;
};

static TUniquePtr<FState> GState;

static void TearDown()
{
	if (!GState)
	{
		return;
	}
	TUniquePtr<FState> State = MoveTemp(GState);

	// FIRST OF ALL, because it is the one subscription that runs every frame and reads GState:
	// OnBeginFrame broadcasts from LaunchEngineLoop and would otherwise fire once more, after
	// GState was moved out, on a view this function is about to retire.
	if (State->ModelDriverHandle.IsValid())
	{
		FCoreDelegates::OnBeginFrame.Remove(State->ModelDriverHandle);
		State->ModelDriverHandle.Reset();
	}

	FWorldDelegates::OnWorldBeginTearDown.Remove(State->WorldTearDownHandle);

	// Before anything else: a live-reload flush landing after this point must not find a
	// handler that reads GState (already moved out) or a view that is about to be retired.
	if (UVaCuusSubsystem* Subsystem = State->Subsystem.Get())
	{
		Subsystem->OnDocumentsReloadRequested.Remove(State->ReloadRequestedHandle);
	}

	// Hand the mouse back to the game. Before the widget goes away, so nothing can
	// arrive at a half-detached widget on the way out.
	SetUIInputMode(State->InputWorld.Get(), /*bEnable=*/false);

	// Spec §4 teardown order:
	//
	// 1. Stop accepting commands. Detaching first means no resize command can land
	// behind the view removal below; pulling the widget out of the viewport then
	// stops the paints. On PIE/world shutdown the viewport (or its widget tree) may
	// already be gone — weak ptr guards that.
	if (State->Widget.IsValid())
	{
		State->Widget->DetachView();

		// Hand mouse capture back before the widget can be destroyed -- see
		// SVaCuusWidget::ReleaseOwnPointerCapture for why (a dangling captor path trips
		// `ensureMsgf(MouseCaptorPath.Widgets.Num() > 0, ...)` at
		// SlateApplication.cpp:5558) and for why the release is PER USER rather than
		// FSlateApplication::ReleaseAllPointerCapture() (bead VaCuus-akj.6.16).
		//
		// Task 8's UMG wrapper needs the same care in ReleaseSlateResources(): any path
		// that can drop an SVaCuusWidget while it holds capture has to release first, and
		// it calls the same helper.
		State->Widget->ReleaseOwnPointerCapture(TEXT("VaCuus demo teardown"));

		if (UGameViewportClient* Viewport = State->Viewport.Get())
		{
			Viewport->RemoveViewportWidgetContent(State->Widget.ToSharedRef());
		}
		State->Widget.Reset();
	}

	// 2. Retire the view. The UI thread closes the document, drops the context and
	// releases the view's render resources from its own thread (so that release is
	// ordered after its last publish) — all without stopping the shared thread,
	// which other views and other PIE clients may still be using. If the subsystem
	// is already gone (world/engine teardown got here first) it has done that for us.
	if (UVaCuusSubsystem* Subsystem = State->Subsystem.Get())
	{
		Subsystem->DestroyView(State->View.Get());
	}

	// 3. Drop our own element reference. Note what this does NOT establish: step 2 is
	// asynchronous, so the host (and its own reference) lives on inside the UI
	// thread's view map until it drains the RemoveView, and the release command it
	// enqueues then may still be in flight. None of that needs ordering — the element
	// is a thread-safe shared pointer, whichever of the game, UI or render thread
	// happens to drop the last reference destroys it, and the render-side release is
	// ordered after the view's last publish because the UI thread enqueues both.
	State->Element.Reset();

	UE_LOG(LogVaCuus, Log, TEXT("VaCuus demo off"));
}

/**
 * Minimal PIE-stop guard: any game-world tear-down while the HUD is active
 * turns it off through the normal path, before the viewport and RmlUi teardown
 * can race each other. Good enough for a debug toggle in M1.
 */
static void OnWorldBeginTearDown(UWorld* World)
{
	if (GState)
	{
		UE_LOG(LogVaCuus, Log, TEXT("VaCuus demo: world tear-down, switching HUD off"));
		TearDown();
	}
}

/**
 * The missing M1 fallback (VaCuus-akj.6.7): the toggle used to fall back to the
 * inline document only when the VFS file was MISSING. Loading happens on the UI
 * thread now, so a file that exists but fails to parse reports back here through
 * UVaCuusView::OnLoadCompleted, and we retry with the inline document.
 */
static void OnViewLoadCompleted(UVaCuusView* View, bool bSuccess)
{
	if (!GState || GState->View.Get() != View)
	{
		return;
	}

	if (bSuccess)
	{
		GState->bLoadingFromFile = false;
		GState->bAnyDocumentShown = true;
		return;
	}

	if (!GState->bLoadingFromFile || GState->bTriedInlineFallback)
	{
		// Live reload made this branch reachable (it used to need a broken file AND a
		// broken inline document): a fan-out reload of a document that now has a typo in it
		// lands here, because bLoadingFromFile was cleared when the file last loaded fine.
		UE_LOG(LogVaCuus, Error, TEXT("VaCuus demo: document load failed and no fallback is left; %s"),
			GState->bAnyDocumentShown
				? TEXT("the reload failed and the previous document is still up")
				: TEXT("the HUD stays empty"));
		return;
	}

	GState->bLoadingFromFile = false;
	GState->bTriedInlineFallback = true;

	UE_LOG(LogVaCuus, Warning,
		TEXT("VaCuus demo: '%s' exists but failed to load; falling back to the inline document"),
		GDocumentVfsPath);
	View->LoadDocumentFromMemory(GTestDocumentRml);
}

/**
 * Live reload's other entrance, and the one that matters when things are broken
 * (bead VaCuus-akj.6.x review finding I4).
 *
 * THE HOLE IT FILLS: a view showing the inline fallback has no DocumentPath -- that is
 * the view's invariant, DocumentPath describes what is SHOWING -- so
 * UVaCuusSubsystem::ClearAssetCachesAndReloadAllViews() cannot reload it, and neither can
 * vacuus.ReloadUI. Without this, "vacuus.M1HUD says m1_hud.rml is missing, so create it
 * and save" reports `reloaded 0 view(s)` and nothing changes until the toggle is cycled
 * off and on. Iterating on a document that is missing or broken is the single most
 * valuable thing live reload does, so the owner of the fallback -- which is the only
 * thing that knows which file it fell back FROM -- re-arms it here.
 *
 * Re-arming also resets bTriedInlineFallback, so a still-broken file falls back again
 * rather than being reported as unrecoverable. That cannot loop: nothing re-arms except
 * an explicit reload request.
 */
static void OnDocumentsReloadRequested(int32& InOutNumReloaded)
{
	if (!GState)
	{
		return;
	}

	UVaCuusView* View = GState->View.Get();
	if (!View)
	{
		return;
	}

	if (!View->GetDocumentPath().IsEmpty())
	{
		// A view with a file document was already reloaded by the fan-out that broadcast
		// this; re-issuing here would parse the same document twice.
		return;
	}

	if (GState->bLoadingFromFile)
	{
		// A file load is still in flight (its OnLoadCompleted has not been polled yet).
		// Re-issuing now would race the fallback decision that completion is about to make.
		return;
	}

	const FString DocumentDiskPath = VaCuusContentPaths::ResolveExistingDocument(GDocumentVfsPath);
	if (DocumentDiskPath.IsEmpty())
	{
		// Verbose, not Warning: every flush while the file is still absent would say this.
		UE_LOG(LogVaCuus, Verbose,
			TEXT("VaCuus demo: reload requested, but '%s' still does not exist under any DevUI root"),
			GDocumentVfsPath);
		return;
	}

	GState->bLoadingFromFile = true;
	GState->bTriedInlineFallback = false;

	UE_LOG(LogVaCuus, Log,
		TEXT("VaCuus demo: '%s' ('%s') is loadable again; re-loading it over the inline fallback"),
		GDocumentVfsPath, *DocumentDiskPath);

	View->LoadDocument(GDocumentVfsPath);

	// Counted, because the flush's log line reports "reloaded N view(s)" and this is one.
	++InOutNumReloaded;
}

/**
 * One frame of the M3 demo: move the live struct, then hand it to the view.
 *
 * DRIVEN FROM WALL CLOCK, NOT FROM THE FRAME COUNTER, so the same elapsed time produces the
 * same picture whatever the frame rate -- which is what lets a headless run schedule a
 * screenshot at t+3s and assert what should be on it.
 *
 * Tick is the exception and counts CALLS: it is the liveness signal, and a HUD whose numbers
 * happen to be stationary is exactly when you want to know the pump is still running.
 */
static void PumpDemoModel()
{
	if (!GState || !GState->View.IsValid())
	{
		return;
	}

	// FUNCTION-LOCAL STATIC, not a file-scope FName: FName's global table is built during
	// module startup, so a file-scope FName constructed from a literal would depend on static
	// initialisation order. This also keeps the per-frame cost to a load rather than a hash.
	static const FName ModelName(GM3ModelName);

	FVaCuusDemoModel& Model = GState->DemoModel;
	++Model.Tick;

	// UPDATED EVEN WHILE FROZEN, and that is the point of the frozen mode rather than an
	// oversight: the idle row is about a game that pushes its struct every frame with nothing
	// changed. Tick is rolled back so the struct really is byte-identical to the last one.
	if (GState->bModelFrozen)
	{
		--Model.Tick;
		GState->View->UpdateModel(ModelName, FVaCuusDemoModel::StaticStruct(), &Model);
		return;
	}

	const double Elapsed = FPlatformTime::Seconds() - GState->ModelDriverStartSeconds;

	// A 10-second sweep down and back up, so the bar is always moving and the alert class
	// switches twice a cycle.
	const double Phase = FMath::Fmod(Elapsed, 10.0);
	Model.Health = float(Phase < 5.0 ? 100.0 - Phase * 20.0 : (Phase - 5.0) * 20.0);
	Model.bAlert = Model.Health < 30.f;

	Model.Ammo = 30 - (int32(Elapsed) % 31);

	static const TCHAR* StanceNames[] = {TEXT("Standing"), TEXT("Crouched"), TEXT("Prone")};
	const int32 StanceIndex = int32(Elapsed / 2.0) % UE_ARRAY_COUNT(StanceNames);
	Model.Stance = static_cast<EVaCuusDemoStance>(StanceIndex);

	// The three string-shaped kinds move on a slower beat than the numbers, so a screenshot
	// catches a stable word rather than a blur -- and so that the FText and FName diff rules
	// (spec 5) are exercised by a real change rather than by a value that never moves.
	const int32 Beat = int32(Elapsed / 3.0);
	Model.CallSign = FString::Printf(TEXT("VACUUS-%02d"), Beat % 100);
	Model.Zone = FName(*FString::Printf(TEXT("Sector_%c"), TEXT('A') + (Beat % 4)));
	Model.Objective = FText::AsCultureInvariant(FString::Printf(TEXT("Hold the line (beat %d)"), Beat));

	Model.Target.Designation = FString::Printf(TEXT("BOGEY-%d"), Beat % 8);
	Model.Target.Distance = 120 + (Beat % 9) * 35;

	// The M3b killfeed: one row per 1.5-second beat, front-trimmed above 6. Slower than every
	// other field on purpose -- rows have to be readable as ROWS on a screenshot, and a feed
	// that scrolled per frame would photograph as noise. `>` rather than `!=` so a thaw's beat
	// reset (see FreezeDemoModel) can only pause the feed, never replay it.
	const int32 KillBeat = int32(Elapsed / 1.5);
	if (KillBeat > GState->LastKillfeedBeat)
	{
		GState->LastKillfeedBeat = KillBeat;
		const int32 Serial = GState->KillfeedSerial++;

		// Deterministic from the serial, so any two runs of the same length show the same feed
		// -- which is what lets a headless screenshot be checked against an expectation instead
		// of merely glanced at. Pool sizes 5, 7 and 4 are pairwise coprime, so the
		// (Killer, Victim, Weapon) triple does not repeat for 140 rows.
		static const TCHAR* Killers[] = {TEXT("RAPTOR"), TEXT("VIPER"), TEXT("GHOST"), TEXT("NOMAD"), TEXT("HAVOC")};
		static const TCHAR* Weapons[] = {TEXT("railgun"), TEXT("SMG"), TEXT("DMR"), TEXT("knife")};

		FVaCuusDemoKillfeedRow Row;
		Row.Killer = Killers[Serial % UE_ARRAY_COUNT(Killers)];
		Row.Victim = FString::Printf(TEXT("BOGEY-%02d"), Serial % 7);
		Row.Weapon = Weapons[Serial % UE_ARRAY_COUNT(Weapons)];
		Row.bHeadshot = (Serial % 3) == 0;
		Model.Killfeed.Add(MoveTemp(Row));

		// FROM THE FRONT, which is the expensive direction and the point (spec 7): removing
		// element 0 shifts every survivor, so each trim makes RmlUi re-evaluate every row --
		// the all-rows path -- where trimming the back would only ever delete the last one.
		// A while, not an if, only so a lowered cap could never strand extra rows.
		while (Model.Killfeed.Num() > 6)
		{
			Model.Killfeed.RemoveAt(0);
		}
	}

	// ONE CALL WITH THE WHOLE STRUCT. The diff is inside: UpdateModel marks only what moved, and
	// UVaCuusSubsystem::Tick turns whatever was marked into ONE publish for the frame.
	GState->View->UpdateModel(ModelName, FVaCuusDemoModel::StaticStruct(), &Model);
}

/**
 * Binds the demo model to a freshly created view and starts the pump.
 *
 * CALLED BEFORE THE VIEW'S FIRST LoadDocument, WHICH IS THE WHOLE CONTRACT. RmlUi reads
 * `data-model` exactly once, in Element::SetParent (Element.cpp:2202-2219); a model created
 * afterwards attaches to nothing and every `{{Field}}` resolves against no model. The only
 * complaint is one Log::LT_ERROR at load time -- which does reach LogVaCuus (see
 * FVaCuusSystemInterface::LogMessage), but names the element rather than the ordering. The
 * queue is FIFO from a single producer, so a BindModel enqueued here is drained before the load
 * Toggle() enqueues next -- the ordering survives the thread boundary because of that and
 * nothing else.
 *
 * FROM OnBeginFrame RATHER THAN A TICKER, for the reason HoverShot spells out at length:
 * OnBeginFrame broadcasts at LaunchEngineLoop.cpp:5682, BEFORE GEngine->Tick (:5859) where
 * UVaCuusSubsystem::Tick publishes. A ticker fires at :6103, after that publish, so every
 * update would sit in the channel for a frame -- one frame of latency between a gameplay write
 * and the screen, permanently, for no reason.
 */
static void StartModelDriver(UVaCuusView* View)
{
	if (!GState || View == nullptr)
	{
		return;
	}

	if (!View->BindModel(FName(GM3ModelName), FVaCuusDemoModel::StaticStruct()))
	{
		// Already logged in detail by BindModel. The document will still load and will still be
		// laid out; it will simply show nothing, which is precisely the failure this milestone
		// is built against -- so say so here too rather than let the demo look merely empty.
		UE_LOG(LogVaCuus, Error,
			TEXT("vacuus.M3Demo: the data model could not be bound, so m3_demo.rml will load and show NOTHING. ")
			TEXT("Run vacuus.DumpModel to see which side has the model"));
		return;
	}

	GState->DemoModel = FVaCuusDemoModel();
	GState->ModelDriverStartSeconds = FPlatformTime::Seconds();
	GState->bModelFrozen = false;
	GState->ModelDriverHandle = FCoreDelegates::OnBeginFrame.AddStatic(&PumpDemoModel);
}

/**
 * Brings the named document up, or takes it down if it is already the one showing.
 *
 * SWITCHING BETWEEN THE TWO DOCUMENTS IS A TEARDOWN PLUS A BRING-UP, not a reload, and
 * that is deliberate: the two documents want different view sizes and different input
 * modes in principle, and one live view at a time is what makes the sub-commands
 * (.Mouse, .Nav, .Rects, ...) unambiguous -- they all reach GState, which holds exactly
 * one view.
 */
static void Toggle(const TCHAR* DocumentVfsPath)
{
	if (GState)
	{
		// Compared by CONTENT, not by pointer: the two call sites pass the file-static
		// literals so a pointer compare would work today, but it would silently start
		// answering "different document" the day someone passes a computed path.
		const bool bSwitchingDocument = FCString::Strcmp(GDocumentVfsPath, DocumentVfsPath) != 0;

		TearDown();
		if (!bSwitchingDocument)
		{
			return;
		}
	}

	GDocumentVfsPath = DocumentVfsPath;

	if (!GEngine || !GEngine->GameViewport)
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("The VaCuus demo toggles need a game viewport (PIE or -game); they do nothing in a pure editor session"));
		return;
	}

	UGameViewportClient* Viewport = GEngine->GameViewport;
	UWorld* World = Viewport->GetWorld();
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	UVaCuusSubsystem* Subsystem = GameInstance ? GameInstance->GetSubsystem<UVaCuusSubsystem>() : nullptr;
	if (!Subsystem)
	{
		UE_LOG(LogVaCuus, Error, TEXT("VaCuus demo: no UVaCuusSubsystem on this game instance"));
		return;
	}

	const FIntPoint InitialViewSize =
		Viewport->Viewport ? Viewport->Viewport->GetSizeXY() : FIntPoint(1280, 720);

	// Prefer the real document from the DevUI roots (plugin first, then project -- D19);
	// fall back to the inline probe document so the toggle keeps working on a bare project.
	const FString DocumentDiskPath = VaCuusContentPaths::ResolveExistingDocument(GDocumentVfsPath);
	const bool bLoadFromFile = !DocumentDiskPath.IsEmpty();
	if (bLoadFromFile)
	{
		UE_LOG(LogVaCuus, Log, TEXT("VaCuus demo: loading document via VFS path '%s' ('%s')"),
			GDocumentVfsPath, *DocumentDiskPath);
	}
	else
	{
		UE_LOG(LogVaCuus, Log, TEXT("VaCuus demo: '%s' not found under any DevUI root (%s), using the inline fallback document"),
			GDocumentVfsPath, *FString::Join(VaCuusContentPaths::GetDocumentRoots(), TEXT(" | ")));
	}

	TSharedRef<FVaCuusSlateElement> Element = MakeShared<FVaCuusSlateElement>();

	// The subsystem hands the host to the process-wide UI thread, which boots it
	// (context creation and everything RmlUi-affine) on its own thread; the very
	// first view in the process is also what starts that thread and RmlUi.
	UVaCuusView* View = Subsystem->CreateView(MakeUnique<FVaCuusRmlDocumentHost>(Element), InitialViewSize);
	if (!View)
	{
		// Logged in detail by the subsystem/module; the element never touched the RHI,
		// so letting everything die here is safe.
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.M1HUD: no view could be created; HUD not shown"));
		return;
	}

	GState = MakeUnique<FState>();
	GState->Subsystem = Subsystem;
	GState->View = View;
	GState->Element = Element;
	GState->Viewport = Viewport;
	GState->InputWorld = World;
	GState->bLoadingFromFile = bLoadFromFile;
	GState->WorldTearDownHandle = FWorldDelegates::OnWorldBeginTearDown.AddStatic(&OnWorldBeginTearDown);

	// Bound before the load is queued: the completion is polled from the
	// subsystem's next tick, never earlier, but there is no reason to race it.
	View->OnLoadCompleted.AddStatic(&OnViewLoadCompleted);

	// Subscribed unconditionally, not just when the load below falls back: a document that
	// loads fine now can be broken by the next save, fall back, and then need re-arming.
	GState->ReloadRequestedHandle =
		Subsystem->OnDocumentsReloadRequested.AddStatic(&OnDocumentsReloadRequested);

	// BEFORE THE LOAD BELOW, AND THAT ORDER IS RmlUi's REQUIREMENT RATHER THAN A PREFERENCE --
	// see StartModelDriver. Guarded on the document rather than done unconditionally because a
	// model bound to a view whose document has no matching `data-model` is a model whose values
	// go nowhere, and this file's other two documents have none.
	if (FCString::Strcmp(GDocumentVfsPath, GM3DemoVfsPath) == 0)
	{
		StartModelDriver(View);
	}

	// Asynchronous by design: the document is loaded by the UI thread on its first
	// frame. The view size rides along so the very first layout is at the right size.
	if (bLoadFromFile)
	{
		View->LoadDocument(GDocumentVfsPath);
	}
	else
	{
		View->LoadDocumentFromMemory(GTestDocumentRml);
	}

	TSharedRef<SVaCuusWidget> Widget = SNew(SVaCuusWidget, View, Element);
	Viewport->AddViewportWidgetContent(Widget, /*ZOrder=*/100);
	GState->Widget = Widget;

	// After the widget is in the tree, so the input mode change lands on a viewport
	// that already has something to route to.
	SetUIInputMode(World, /*bEnable=*/true);

	UE_LOG(LogVaCuus, Log, TEXT("VaCuus demo on: '%s' (view %u, initial view %dx%d)"),
		GDocumentVfsPath, View->GetViewId(), InitialViewSize.X, InitialViewSize.Y);
}

/** One rect's flags as a readable list, for the dump below. */
static FString DescribeRectFlags(EVaCuusRectFlags Flags)
{
	TArray<FString, TInlineAllocator<3>> Names;
	if (EnumHasAnyFlags(Flags, EVaCuusRectFlags::Interactive))
	{
		Names.Add(TEXT("Interactive"));
	}
	if (EnumHasAnyFlags(Flags, EVaCuusRectFlags::Focusable))
	{
		Names.Add(TEXT("Focusable"));
	}
	if (EnumHasAnyFlags(Flags, EVaCuusRectFlags::TextInput))
	{
		Names.Add(TEXT("TextInput"));
	}

	return Names.IsEmpty() ? FString(TEXT("None")) : FString::Join(Names, TEXT("|"));
}

/**
 * The navigation-entry mask as a readable list.
 *
 * NAMED PER DIRECTION rather than printed as "yes/no", because the whole point of the mask
 * is that `body { nav: vertical; }` answers Up|Down and NOT Left|Right -- a boolean line
 * here would hide the exact distinction an acceptance run is checking.
 */
static FString DescribeNavDirections(EVaCuusNavDirection Directions)
{
	TArray<FString, TInlineAllocator<4>> Names;
	if (EnumHasAnyFlags(Directions, EVaCuusNavDirection::Up))
	{
		Names.Add(TEXT("Up"));
	}
	if (EnumHasAnyFlags(Directions, EVaCuusNavDirection::Down))
	{
		Names.Add(TEXT("Down"));
	}
	if (EnumHasAnyFlags(Directions, EVaCuusNavDirection::Left))
	{
		Names.Add(TEXT("Left"));
	}
	if (EnumHasAnyFlags(Directions, EVaCuusNavDirection::Right))
	{
		Names.Add(TEXT("Right"));
	}

	return Names.IsEmpty() ? FString(TEXT("none")) : FString::Join(Names, TEXT("|"));
}

/**
 * Prints the interactive-region snapshot the game thread is currently answering Slate
 * from (Task 14).
 *
 * WHY THIS EXISTS RATHER THAN COORDINATES IN A SCRIPT: every other headless check here
 * takes an <x> <y> the author read off a stylesheet, and that is a guess about layout --
 * it is wrong the moment a padding value changes, and when it is wrong the failure looks
 * like "input is broken" rather than "the coordinate missed". This prints the rects RmlUi
 * actually produced, with their flags, so an acceptance run can aim at measured geometry
 * and can assert the two facts a screenshot cannot show: that a region IS covered and
 * flagged the way the document intended, and that the `vacuus-passthrough` region is NOT
 * in the list at all.
 *
 * READS THE VIEW'S CACHED SNAPSHOT, i.e. exactly what SVaCuusWidget's handlers see this
 * frame (UVaCuusView::GetSnapshot), not a fresh query -- there is no such thing as a
 * fresh query from this thread, which is the whole design.
 */
/**
 * Runs Work after DelaySeconds, or immediately when it is not positive.
 *
 * WHY THE DELAY EXISTS AT ALL: every `-ExecCmds` command runs on the SAME early tick, before
 * the widget has ever been arranged and before any frame has been published -- so a rect dump
 * issued there prints an empty default snapshot and a hit test answers "no" for every point.
 * HoverShot, NavShot and TypeShot all solve this with a timed second step; this is the same
 * pattern for the two read-only commands, so a headless acceptance run can put the query on
 * the same command line as the toggle.
 */
static void ScheduleAfter(float DelaySeconds, TFunction<void()> Work)
{
	if (DelaySeconds <= 0.0f)
	{
		Work();
		return;
	}

	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[Work = MoveTemp(Work)](float)
		{
			Work();
			return false;
		}),
		DelaySeconds);
}

static void DumpRects()
{
	if (!GState || !GState->View.IsValid())
	{
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.M2Demo.Rects needs a demo to be on"));
		return;
	}

	const UVaCuusView* View = GState->View.Get();
	const FVaCuusInteractiveSnapshot& Snapshot = View->GetSnapshot();

	UE_LOG(LogVaCuus, Log,
		TEXT("Rects: view %u '%s' generation=%llu viewSize=%dx%d rects=%d cursor=%d ")
		TEXT("wantsKeyboard=%s tabEnters=%s directionEnters=%s textInputFocused=%s"),
		View->GetViewId(), GDocumentVfsPath, Snapshot.Generation, Snapshot.ViewSize.X, Snapshot.ViewSize.Y,
		Snapshot.InteractiveRects.Num(), int32(Snapshot.Cursor),
		Snapshot.bWantsKeyboardFocus ? TEXT("yes") : TEXT("no"),
		Snapshot.bTabEntersFocus ? TEXT("yes") : TEXT("no"),
		*DescribeNavDirections(Snapshot.DirectionsEnteringFocus),
		Snapshot.bTextInputFocused ? TEXT("yes") : TEXT("no"));

	// Min() rather than either count: the two arrays are an invariant, not a guarantee a
	// debug dump may assume, and printing past the end of the shorter one would be the
	// least useful possible way to find out they had diverged.
	const int32 NumRects = FMath::Min(Snapshot.InteractiveRects.Num(), Snapshot.RectFlags.Num());
	for (int32 Index = 0; Index < NumRects; ++Index)
	{
		const FIntRect& Rect = Snapshot.InteractiveRects[Index];
		UE_LOG(LogVaCuus, Log, TEXT("Rects:   [%2d] (%4d,%4d)-(%4d,%4d) centre (%4d,%4d) %s"),
			Index, Rect.Min.X, Rect.Min.Y, Rect.Max.X, Rect.Max.Y,
			(Rect.Min.X + Rect.Max.X) / 2, (Rect.Min.Y + Rect.Max.Y) / 2, *DescribeRectFlags(Snapshot.RectFlags[Index]));
	}

	if (Snapshot.RectFlags.Num() != Snapshot.InteractiveRects.Num())
	{
		UE_LOG(LogVaCuus, Error, TEXT("Rects: the parallel arrays disagree (%d rects, %d flags)"),
			Snapshot.InteractiveRects.Num(), Snapshot.RectFlags.Num());
	}
}

/**
 * Answers "is this point covered, and how" the way the widget's handlers would.
 *
 * The complement of the dump above: the dump says what the UI claims, this says what a
 * click at one place would do -- which is the assertion an acceptance run wants for the
 * pass-through region, because "not in the list" and "Contains() is false" are two
 * different statements and only the second is what SVaCuusWidget returns Unhandled from.
 */
static void Rects(const TArray<FString>& Args)
{
	ScheduleAfter(Args.Num() > 0 ? FCString::Atof(*Args[0]) : 0.0f, &DumpRects);
}

static void HitTestAt(FIntPoint Point)
{
	if (!GState || !GState->View.IsValid())
	{
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.M2Demo.Hit needs a demo to be on"));
		return;
	}

	const FVaCuusInteractiveSnapshot& Snapshot = GState->View->GetSnapshot();

	UE_LOG(LogVaCuus, Log,
		TEXT("Hit (%d,%d): covered=%s focusable=%s textInput=%s -- so a press there would be answered %s"),
		Point.X, Point.Y,
		Snapshot.Contains(Point) ? TEXT("yes") : TEXT("no"),
		Snapshot.IsFocusableAt(Point) ? TEXT("yes") : TEXT("no"),
		Snapshot.IsTextInputAt(Point) ? TEXT("yes") : TEXT("no"),
		Snapshot.Contains(Point) ? TEXT("Handled (the UI takes it)") : TEXT("Unhandled (it reaches the game)"));
}

static void HitTest(const TArray<FString>& Args)
{
	if (Args.Num() < 2)
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("vacuus.M2Demo.Hit expects <x> <y> [delaySeconds]: the point in VIEW pixels"));
		return;
	}

	const FIntPoint Point(FCString::Atoi(*Args[0]), FCString::Atoi(*Args[1]));
	const float DelaySeconds = Args.Num() > 2 ? FCString::Atof(*Args[2]) : 0.0f;

	ScheduleAfter(DelaySeconds, [Point] { HitTestAt(Point); });
}

/**
 * Puts the pointer at a window position and lets Slate route the move.
 *
 * DELIBERATELY GOES THROUGH FSlateApplication rather than calling the widget: this
 * way the whole real path is exercised -- hit-test grid, bubble path,
 * SVaCuusWidget::OnMouseMove, the snapshot test, the queue -- and the only thing
 * synthesized is the position, which is the one part a headless session cannot
 * produce. ProcessMouseMoveEvent is public engine API (SlateApplication.h:1292) and
 * the same call the platform layer makes; nothing here is a test hook into VaCuus.
 */
static bool MoveMouseTo(const FVector2D& Position)
{
	if (!FSlateApplication::IsInitialized())
	{
		UE_LOG(LogVaCuus, Error, TEXT("Synthesizing a mouse move needs Slate (nothing to do under -nullrhi -unattended)"));
		return false;
	}

	FSlateApplication& Slate = FSlateApplication::Get();

	// Moves the platform cursor too, so anything that reads GetCursorPos() later
	// (OnCursorQuery, tooltips) agrees with the event we are about to send.
	Slate.SetCursorPos(Position);

	const FPointerEvent MouseEvent(
		FSlateApplicationBase::CursorPointerIndex,
		Position,
		Position,
		TSet<FKey>(),
		// A move affects no button. FKey() rather than EKeys::Invalid: the latter is
		// declared in InputCoreTypes.h but not exported, so it does not link.
		FKey(),
		/*WheelDelta=*/0.0f,
		FModifierKeysState());

	const bool bHandled = Slate.ProcessMouseMoveEvent(MouseEvent);

	UE_LOG(LogVaCuus, Log, TEXT("Pointer moved to (%.0f, %.0f); Slate reports the event %s"),
		Position.X, Position.Y, bHandled ? TEXT("handled by a widget") : TEXT("unhandled (it fell through)"));
	return bHandled;
}

/**
 * Presses and releases the left mouse button where the pointer already is.
 *
 * THROUGH FSlateApplication, for the same reason MoveMouseTo does: the whole point of a
 * headless click is that it survives the parts a unit test cannot reach -- the hit-test
 * grid, the bubble path, and SVaCuusWidget's own FReply (which is what takes Slate focus
 * and, for Task 9, activates the platform IME context on the first click).
 *
 * ProcessMouseButtonDownEvent takes the PLATFORM WINDOW so it can build a widget path;
 * passing a null one makes Slate resolve the path from the cursor position instead, which
 * is exactly what we want after MoveMouseTo. ProcessMouseButtonUpEvent needs no window at
 * all -- by then the captor (if any) is known.
 */
static bool ClickWhereThePointerIs(const FVector2D& Position)
{
	if (!FSlateApplication::IsInitialized())
	{
		UE_LOG(LogVaCuus, Error, TEXT("Synthesizing a click needs Slate (nothing to do under -nullrhi -unattended)"));
		return false;
	}

	FSlateApplication& Slate = FSlateApplication::Get();

	// The pressed-button sets differ between down and up, and faithfully so: OnMouseUp
	// REMOVES the released button before constructing its event (SlateApplication.cpp:6098-6106),
	// which is the exact asymmetry SVaCuusWidget's capture release depends on.
	const TSet<FKey> LeftOnly = {EKeys::LeftMouseButton};
	const TSet<FKey> NoButtons;

	const FPointerEvent DownEvent(FSlateApplicationBase::CursorPointerIndex, Position, Position, LeftOnly,
		EKeys::LeftMouseButton, /*WheelDelta=*/0.0f, FModifierKeysState());
	const FPointerEvent UpEvent(FSlateApplicationBase::CursorPointerIndex, Position, Position, NoButtons,
		EKeys::LeftMouseButton, /*WheelDelta=*/0.0f, FModifierKeysState());

	const bool bDownHandled = Slate.ProcessMouseButtonDownEvent(nullptr, DownEvent);
	Slate.ProcessMouseButtonUpEvent(UpEvent);

	UE_LOG(LogVaCuus, Log, TEXT("Left click at (%.0f, %.0f); Slate reports the press %s"),
		Position.X, Position.Y, bDownHandled ? TEXT("handled by a widget") : TEXT("unhandled (it fell through)"));
	return bDownHandled;
}

/**
 * Turns the wheel where the pointer is, through Slate's real routing.
 *
 * THE MOVE FIRST IS NOT A CONVENIENCE. Rml::Context::ProcessMouseWheel resolves its scroll
 * target from the HOVER element and returns immediately when there is none
 * (Context.cpp:811-814), and hover is only ever set by a processed mouse move
 * (Context.cpp:1311). A wheel event delivered to a context that has never seen a position
 * scrolls nothing, silently -- so the pair is what the real path looks like anyway.
 *
 * Delta is in WHEEL NOTCHES, positive up, which is UE's convention;
 * FVaCuusUIThread::DispatchInputEvent flips the sign and maps it onto RmlUi's
 * UNIT_SCROLL_LENGTH unit.
 */
static bool WheelAt(const FVector2D& Position, float Delta)
{
	if (!FSlateApplication::IsInitialized())
	{
		UE_LOG(LogVaCuus, Error, TEXT("Synthesizing a wheel turn needs Slate"));
		return false;
	}

	MoveMouseTo(Position);

	FSlateApplication& Slate = FSlateApplication::Get();
	const FPointerEvent WheelEvent(FSlateApplicationBase::CursorPointerIndex, Position, Position, TSet<FKey>(),
		EKeys::MouseWheelAxis, Delta, FModifierKeysState());

	const bool bHandled = Slate.ProcessMouseWheelOrGestureEvent(WheelEvent, /*InGestureEvent=*/nullptr);

	UE_LOG(LogVaCuus, Log, TEXT("Wheel %+.1f at (%.0f, %.0f); Slate reports the event %s"),
		Delta, Position.X, Position.Y,
		bHandled ? TEXT("handled by a widget") : TEXT("unhandled (it fell through)"));
	return bHandled;
}

/**
 * Presses at one point, drags to another in steps, and releases -- the gesture a scrollbar
 * needs and the one case the snapshot deliberately cannot answer on its own.
 *
 * WHY THE INTERMEDIATE MOVES MATTER: a press-then-jump-then-release is not a drag to RmlUi
 * either -- WidgetScroll positions its bar from the mouse position it sees while the bar is
 * :active -- and it is not a drag to SVaCuusWidget's capture bookkeeping, which is the thing
 * being demonstrated: the moves must keep answering Handled after the pointer leaves the
 * rect the press started in.
 *
 * The pressed-button set is LEFT for the press and every move, and EMPTY for the release,
 * mirroring FSlateApplication::OnMouseUp, which removes the released button before building
 * the event (SlateApplication.cpp:6098-6106).
 */
static void DragFromTo(const FVector2D& From, const FVector2D& To, int32 NumSteps)
{
	if (!FSlateApplication::IsInitialized())
	{
		UE_LOG(LogVaCuus, Error, TEXT("Synthesizing a drag needs Slate"));
		return;
	}

	FSlateApplication& Slate = FSlateApplication::Get();
	const TSet<FKey> LeftOnly = {EKeys::LeftMouseButton};
	const TSet<FKey> NoButtons;

	MoveMouseTo(From);

	const FPointerEvent DownEvent(FSlateApplicationBase::CursorPointerIndex, From, From, LeftOnly,
		EKeys::LeftMouseButton, /*WheelDelta=*/0.0f, FModifierKeysState());
	const bool bDownHandled = Slate.ProcessMouseButtonDownEvent(nullptr, DownEvent);

	// WHOSE press was it? ProcessMouseButtonDownEvent's return cannot say: it is true if ANY
	// widget on the bubble path handled the event, and SViewport -- the game's own widget, and
	// our ancestor (GameViewportClient.cpp:1326) -- handles what we decline. So a pass-through
	// press and a UI press BOTH report "handled", which is exactly the distinction an acceptance
	// run needs and the one Slate will not give.
	//
	// Capture does give it. SVaCuusWidget takes Slate's mouse capture on the first press it
	// answers Handled (OnMouseButtonDown), and takes none at all when the snapshot does not
	// cover the point -- so this flag, sampled between the press and the release, is a direct
	// readout of the FReply the widget produced.
	const bool bVaCuusTookPress = GState.IsValid() && GState->Widget.IsValid() &&
		GState->Widget->IsTrackingMouseCapture_Debug();

	const int32 Steps = FMath::Max(NumSteps, 1);
	int32 NumMovesHandled = 0;
	for (int32 Step = 1; Step <= Steps; ++Step)
	{
		const FVector2D Position = FMath::Lerp(From, To, double(Step) / double(Steps));
		Slate.SetCursorPos(Position);

		const FPointerEvent MoveEvent(FSlateApplicationBase::CursorPointerIndex, Position, Position, LeftOnly,
			FKey(), /*WheelDelta=*/0.0f, FModifierKeysState());
		NumMovesHandled += Slate.ProcessMouseMoveEvent(MoveEvent) ? 1 : 0;
	}

	const FPointerEvent UpEvent(FSlateApplicationBase::CursorPointerIndex, To, To, NoButtons,
		EKeys::LeftMouseButton, /*WheelDelta=*/0.0f, FModifierKeysState());
	Slate.ProcessMouseButtonUpEvent(UpEvent);

	UE_LOG(LogVaCuus, Log,
		TEXT("Dragged (%.0f, %.0f) -> (%.0f, %.0f) in %d step(s); Slate says the press was %s, ")
		TEXT("and the press was taken by %s (%d of %d move(s) handled by someone)"),
		From.X, From.Y, To.X, To.Y, Steps,
		bDownHandled ? TEXT("handled") : TEXT("unhandled"),
		bVaCuusTookPress ? TEXT("THE UI (VaCuus captured the mouse)")
						 : TEXT("THE GAME (VaCuus declined, so it bubbled past us to SViewport)"),
		NumMovesHandled, Steps);
}

/**
 * Types a string one character at a time, exactly as the platform keyboard would.
 *
 * ONE FCharacterEvent PER UTF-16 UNIT, because that is what Slate delivers -- TCHAR is
 * char16_t on Unix, so anything above the BMP arrives as two calls and SVaCuusWidget
 * recombines the surrogate pair. Iterating the FString's units rather than its code points
 * is therefore not a shortcut, it is the faithful thing.
 *
 * ProcessKeyCharEvent routes along the FOCUS path, so whatever holds Slate focus receives
 * it; the caller is expected to have focused the HUD widget (SendNavSequence and the click
 * above both do). This is the whole Linux text path -- OnKeyChar -> the input queue ->
 * Rml::Context::ProcessTextInput -- driven from outside VaCuus.
 */
static void TypeTextIntoFocusedWidget(const FString& Text)
{
	if (!FSlateApplication::IsInitialized())
	{
		UE_LOG(LogVaCuus, Error, TEXT("Synthesizing text needs Slate (nothing to do under -nullrhi -unattended)"));
		return;
	}

	FSlateApplication& Slate = FSlateApplication::Get();
	const int32 UserIndex = Slate.GetUserIndexForKeyboard();

	int32 NumConsumed = 0;
	for (int32 Index = 0; Index < Text.Len(); ++Index)
	{
		const FCharacterEvent CharEvent(Text[Index], FModifierKeysState(), UserIndex, /*bIsRepeat=*/false);
		if (Slate.ProcessKeyCharEvent(CharEvent))
		{
			++NumConsumed;
		}
	}

	// TWO DIFFERENT FACTS, and the earlier version of this line conflated them into a
	// "0 handled by a widget" that read as "nothing happened" while the text visibly landed in
	// the field. What Slate reports is only whether the FReply CONSUMED the character, and
	// SVaCuusWidget::OnKeyChar answers that from the cached snapshot's bWantsKeyboardFocus --
	// which is one game frame stale. So a synthesized click-then-type burst inside a single
	// frame is answered Unconsumed for every character even though every one of them was
	// FORWARDED: OnKeyChar queues unconditionally and only the FReply depends on the snapshot.
	// Consumption decides whether the game ALSO sees the key; delivery to the UI is unrelated.
	UE_LOG(LogVaCuus, Log,
		TEXT("Typed '%s': %d UTF-16 unit(s) forwarded to the focused widget, %d consumed by it ")
		TEXT("(unconsumed means the character still reached the UI queue but also fell through to the game -- ")
		TEXT("SVaCuusWidget::OnKeyChar answers Slate from the one-frame-stale snapshot, and a click plus typing in ")
		TEXT("the SAME frame is answered before that snapshot knows a field took focus)"),
		*Text, Text.Len(), NumConsumed);
}

static void SimulateMouseMove(const TArray<FString>& Args)
{
	if (Args.Num() < 2)
	{
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.M1HUD.Mouse expects two arguments: <x> <y> in window pixels"));
		return;
	}

	MoveMouseTo(FVector2D(FCString::Atof(*Args[0]), FCString::Atof(*Args[1])));
}

static FAutoConsoleCommand GSimulateMouseCommand(
	TEXT("vacuus.M1HUD.Mouse"),
	TEXT("Move the pointer to <x> <y> (window pixels) through Slate's real routing. Headless hover verification."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&SimulateMouseMove));

/**
 * Sends one key press to the focused widget through Slate's real routing.
 *
 * LIKE MoveMouseTo, THIS GOES THROUGH FSlateApplication ON PURPOSE: the whole point
 * of a headless nav check is that the key survives the parts a unit test cannot
 * reach -- Slate's navigation config (which would otherwise swallow every arrow
 * before OnKeyDown, controller decision D12), the focus path, and the widget's own
 * pass-through set. ProcessKeyDownEvent/ProcessKeyUpEvent are public engine API and
 * the same calls the platform layer makes.
 */
static bool SendKeyToFocusedWidget(const FKey& Key)
{
	if (!FSlateApplication::IsInitialized())
	{
		UE_LOG(LogVaCuus, Error, TEXT("Synthesizing a key needs Slate (nothing to do under -nullrhi -unattended)"));
		return false;
	}

	FSlateApplication& Slate = FSlateApplication::Get();
	const FKeyEvent KeyEvent(
		Key, FModifierKeysState(), Slate.GetUserIndexForKeyboard(), /*bIsRepeat=*/false, /*CharacterCode=*/0, /*KeyCode=*/0);

	const bool bHandled = Slate.ProcessKeyDownEvent(KeyEvent);
	Slate.ProcessKeyUpEvent(KeyEvent);

	UE_LOG(LogVaCuus, Log, TEXT("Key '%s' sent; Slate reports the press %s"),
		*Key.ToString(), bHandled ? TEXT("handled by a widget") : TEXT("unhandled (it fell through)"));
	return bHandled;
}

/**
 * Gives the HUD widget Slate's keyboard focus and sends a sequence of keys.
 *
 * The focus step is not optional: OnKeyDown only reaches a widget that holds user
 * focus, and in a headless session nothing has clicked anything.
 */
static void SendNavSequence(const TArray<FString>& KeyNames)
{
	if (!GState || !GState->Widget.IsValid() || !FSlateApplication::IsInitialized())
	{
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.M1HUD.Nav needs the HUD to be on"));
		return;
	}

	FSlateApplication& Slate = FSlateApplication::Get();
	Slate.SetUserFocus(Slate.GetUserIndexForKeyboard(), GState->Widget, EFocusCause::SetDirectly);
	UE_LOG(LogVaCuus, Log, TEXT("vacuus.M1HUD.Nav: HUD widget has Slate focus; navigation config overridden: %s"),
		GState->Widget->IsNavigationConfigOverridden_Debug() ? TEXT("yes") : TEXT("no"));

	for (const FString& KeyName : KeyNames)
	{
		const FKey Key(*KeyName);
		if (!Key.IsValid())
		{
			UE_LOG(LogVaCuus, Error, TEXT("vacuus.M1HUD.Nav: '%s' is not a known FKey name"), *KeyName);
			continue;
		}

		SendKeyToFocusedWidget(Key);
	}
}

static void Nav(const TArray<FString>& Args)
{
	if (Args.Num() == 0)
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("vacuus.M1HUD.Nav expects one or more FKey names, e.g. 'Gamepad_DPad_Right Gamepad_DPad_Right'"));
		return;
	}

	SendNavSequence(Args);
}

static FAutoConsoleCommand GNavCommand(
	TEXT("vacuus.M1HUD.Nav"),
	TEXT("Focus the HUD widget and send FKeys to it through Slate's real routing, e.g. ")
	TEXT("'vacuus.M1HUD.Nav Gamepad_DPad_Right Gamepad_DPad_Right'. Headless navigation verification."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&Nav));

/**
 * Adds or removes a pass-through key at runtime (controller decision D12: the set is
 * a member the console can extend, not a hard-coded constant).
 */
static void PassThroughKey(const TArray<FString>& Args)
{
	if (!GState || !GState->Widget.IsValid())
	{
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.M1HUD.PassThroughKey needs the HUD to be on"));
		return;
	}

	if (Args.Num() == 0)
	{
		TArray<FString> Names;
		for (const FKey& Key : GState->Widget->GetPassThroughKeys())
		{
			Names.Add(Key.ToString());
		}
		Names.Sort();

		UE_LOG(LogVaCuus, Log, TEXT("Pass-through keys (%d): %s"), Names.Num(), *FString::Join(Names, TEXT(", ")));
		return;
	}

	const FKey Key(*Args[0]);
	if (!Key.IsValid())
	{
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.M1HUD.PassThroughKey: '%s' is not a known FKey name"), *Args[0]);
		return;
	}

	// Second argument absent means "add"; explicit 0 removes.
	const bool bAdd = Args.Num() < 2 || FCString::Atoi(*Args[1]) != 0;
	if (bAdd)
	{
		GState->Widget->AddPassThroughKey(Key);
		UE_LOG(LogVaCuus, Log, TEXT("'%s' now passes through to the game"), *Key.ToString());
	}
	else
	{
		const bool bRemoved = GState->Widget->RemovePassThroughKey(Key);
		UE_LOG(LogVaCuus, Log, TEXT("'%s' %s"), *Key.ToString(),
			bRemoved ? TEXT("no longer passes through; the UI may consume it") : TEXT("was not in the pass-through set"));
	}
}

static FAutoConsoleCommand GPassThroughKeyCommand(
	TEXT("vacuus.M1HUD.PassThroughKey"),
	TEXT("List the keys the UI never consumes, or add/remove one: <KeyName> [0 to remove]."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&PassThroughKey));

/** When HoverShot parks the pointer, and when it shoots. Ordered, and both after the widget exists. */
static constexpr float GHoverShotMoveSeconds = 2.0f;
static constexpr float GHoverShotShotSeconds = 3.0f;

/**
 * Headless hover verification in one command: park the pointer at <x> <y>, then
 * request a UI screenshot a second later.
 *
 * WHY IT SCHEDULES INSTEAD OF DOING: every `-ExecCmds` command runs on the same
 * early tick, before the widget has ever been arranged -- so a mouse move issued
 * there hits an empty hit-test grid and hovers nothing, and a screenshot taken there
 * catches a HUD that has not laid out. Two timed steps make the ordering explicit
 * (park, then shoot) instead of racing the frame rate, which is what counting
 * published frames would do.
 *
 * With no arguments it only shoots, which is the "not hovering anything" half of the
 * pair.
 */
static void HoverShot(const TArray<FString>& Args)
{
	const bool bMove = Args.Num() >= 2;
	const FVector2D Position = bMove
		? FVector2D(FCString::Atof(*Args[0]), FCString::Atof(*Args[1]))
		: FVector2D::ZeroVector;

	if (bMove)
	{
		// FROM OnBeginFrame, NOT FROM A TICKER, and that choice is the whole point of
		// the frame-ordering measurement (bead VaCuus-akj.6.13). OnBeginFrame broadcasts
		// at LaunchEngineLoop.cpp:5682, i.e. in the same window as the platform's own
		// dispatch: after nothing that matters and BEFORE GEngine->Tick (line 5859),
		// where UVaCuusSubsystem::Tick polls the snapshot. FTSTicker fires at line 6103,
		// long after that poll, so a move driven from there would measure the ticker's
		// position in the frame instead of input's and quietly invert the answer.
		static FDelegateHandle MoveHandle;
		static double MoveDeadline = 0.0;

		MoveDeadline = FPlatformTime::Seconds() + GHoverShotMoveSeconds;
		if (MoveHandle.IsValid())
		{
			FCoreDelegates::OnBeginFrame.Remove(MoveHandle);
		}

		MoveHandle = FCoreDelegates::OnBeginFrame.AddLambda(
			[Position]
			{
				if (FPlatformTime::Seconds() < MoveDeadline)
				{
					return;
				}

				FCoreDelegates::OnBeginFrame.Remove(MoveHandle);
				MoveHandle.Reset();
				MoveMouseTo(Position);
			});
	}

	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[](float)
		{
			UE_LOG(LogVaCuus, Log, TEXT("vacuus.M1HUD.HoverShot: requesting a UI screenshot"));
			FScreenshotRequest::RequestScreenshot(/*bInShowUI=*/true);
			return false;
		}),
		GHoverShotShotSeconds);

	UE_LOG(LogVaCuus, Log, TEXT("vacuus.M1HUD.HoverShot: %s at t+%.1fs, screenshot at t+%.1fs"),
		bMove ? *FString::Printf(TEXT("pointer to (%.0f, %.0f)"), Position.X, Position.Y) : TEXT("no pointer move"),
		GHoverShotMoveSeconds, GHoverShotShotSeconds);
}

static FAutoConsoleCommand GHoverShotCommand(
	TEXT("vacuus.M1HUD.HoverShot"),
	TEXT("Park the pointer at [x y] after 2s, then take a UI screenshot at 3s. No arguments: screenshot only."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&HoverShot));

/**
 * The navigation twin of HoverShot: focus the HUD, send a key sequence, shoot.
 *
 * Same two-step timing and the same reason for it -- every `-ExecCmds` command runs on
 * one early tick, before the widget has ever been arranged, so keys sent there reach a
 * widget with no geometry and no published snapshot. The pointer is deliberately never
 * moved, which is what makes the resulting screenshot proof of NAVIGATION focus rather
 * than of hover.
 */
static void NavShot(const TArray<FString>& Args)
{
	static FDelegateHandle NavHandle;
	static double NavDeadline = 0.0;
	static TArray<FString> PendingKeys;

	// Default sequence: two steps right. The first enters the document in tab order
	// (the document element holds focus at load and its `nav: auto` short-cuts to tab
	// order), the second navigates spatially from there.
	PendingKeys = Args.Num() > 0 ? Args : TArray<FString>{TEXT("Gamepad_DPad_Right"), TEXT("Gamepad_DPad_Right")};
	NavDeadline = FPlatformTime::Seconds() + GHoverShotMoveSeconds;

	if (NavHandle.IsValid())
	{
		FCoreDelegates::OnBeginFrame.Remove(NavHandle);
	}

	NavHandle = FCoreDelegates::OnBeginFrame.AddLambda(
		[]
		{
			if (FPlatformTime::Seconds() < NavDeadline)
			{
				return;
			}

			FCoreDelegates::OnBeginFrame.Remove(NavHandle);
			NavHandle.Reset();
			SendNavSequence(PendingKeys);
		});

	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[](float)
		{
			UE_LOG(LogVaCuus, Log, TEXT("vacuus.M1HUD.NavShot: requesting a UI screenshot"));
			FScreenshotRequest::RequestScreenshot(/*bInShowUI=*/true);
			return false;
		}),
		GHoverShotShotSeconds);

	UE_LOG(LogVaCuus, Log, TEXT("vacuus.M1HUD.NavShot: keys [%s] at t+%.1fs, screenshot at t+%.1fs"),
		*FString::Join(PendingKeys, TEXT(" ")), GHoverShotMoveSeconds, GHoverShotShotSeconds);
}

/**
 * Types into whatever holds Slate focus, focusing the HUD widget first.
 *
 * The focus step is not optional: OnKeyChar only reaches a widget on the focus path, and in a
 * headless session nothing has clicked anything. Use vacuus.M1HUD.TypeShot instead when the
 * point is to type into a specific FIELD -- this one types wherever RmlUi's focus already is.
 */
static void Type(const TArray<FString>& Args)
{
	if (!GState || !GState->Widget.IsValid() || !FSlateApplication::IsInitialized())
	{
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.M1HUD.Type needs the HUD to be on"));
		return;
	}

	if (Args.Num() == 0)
	{
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.M1HUD.Type expects the text to type, e.g. 'vacuus.M1HUD.Type hello'"));
		return;
	}

	FSlateApplication& Slate = FSlateApplication::Get();
	Slate.SetUserFocus(Slate.GetUserIndexForKeyboard(), GState->Widget, EFocusCause::SetDirectly);

	// Rejoined with spaces: the console splits on whitespace, and "type a sentence" is the
	// normal case for a chat field.
	TypeTextIntoFocusedWidget(FString::Join(Args, TEXT(" ")));
}

static FAutoConsoleCommand GTypeCommand(
	TEXT("vacuus.M1HUD.Type"),
	TEXT("Focus the HUD widget and type <text> into it through Slate's real OnKeyChar routing. ")
	TEXT("Headless text-entry verification."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&Type));

/**
 * The TEXT-ENTRY twin of HoverShot and NavShot: park the pointer on a field, CLICK it, type,
 * then shoot (Task 9).
 *
 * Same two-step timing and the same reason for it -- every `-ExecCmds` command runs on one
 * early tick, before the widget has ever been arranged, so a click issued there hits an empty
 * hit-test grid and focuses nothing.
 *
 * THE CLICK IS THE POINT, not just a way to focus: it is what exercises controller decision
 * D14a end to end. The press lands on a rect carrying EVaCuusRectFlags::TextInput, which both
 * takes Slate focus (D11) and activates the platform IME context on that same first click --
 * and then the characters that follow go through the OnKeyChar path, which on this platform is
 * the ONLY text path there is (D16). The screenshot is proof that all of it agreed on the same
 * element.
 */
static void TypeShot(const TArray<FString>& Args)
{
	static FDelegateHandle TypeHandle;
	static double TypeDeadline = 0.0;
	static FVector2D PendingPosition = FVector2D::ZeroVector;
	static FString PendingText;

	if (Args.Num() < 2)
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("vacuus.M1HUD.TypeShot expects <x> <y> [text]: window pixels of the field, then what to type"));
		return;
	}

	PendingPosition = FVector2D(FCString::Atof(*Args[0]), FCString::Atof(*Args[1]));
	PendingText = Args.Num() > 2 ? FString::Join(TArray<FString>(Args.GetData() + 2, Args.Num() - 2), TEXT(" "))
								 : FString(TEXT("VaCuus M2"));
	TypeDeadline = FPlatformTime::Seconds() + GHoverShotMoveSeconds;

	if (TypeHandle.IsValid())
	{
		FCoreDelegates::OnBeginFrame.Remove(TypeHandle);
	}

	// FROM OnBeginFrame, NOT A TICKER, for the reason HoverShot spells out: this is the same
	// window the platform's own input dispatch runs in, i.e. before UVaCuusSubsystem::Tick polls
	// the snapshot -- which is exactly the ordering a real click has.
	TypeHandle = FCoreDelegates::OnBeginFrame.AddLambda(
		[]
		{
			if (FPlatformTime::Seconds() < TypeDeadline)
			{
				return;
			}

			FCoreDelegates::OnBeginFrame.Remove(TypeHandle);
			TypeHandle.Reset();

			// Move, then click, then type -- in that order and in one frame. The move is what
			// gives the click a hit-test position; the click focuses the field and activates the
			// IME; the characters then reach the field the click focused.
			MoveMouseTo(PendingPosition);
			ClickWhereThePointerIs(PendingPosition);
			TypeTextIntoFocusedWidget(PendingText);

			if (GState && GState->View.IsValid())
			{
				const UVaCuusView::FImeStatus ImeStatus = GState->View->GetImeStatus();
				UE_LOG(LogVaCuus, Log,
					TEXT("vacuus.M1HUD.TypeShot: IME bridge built=%s, platform system absent=%s, registered=%s, ")
					TEXT("context active=%s -- so the text above went through OnKeyChar -> ProcessTextInput"),
					ImeStatus.bHandlerBuilt ? TEXT("yes") : TEXT("no"),
					ImeStatus.bPlatformImeAbsent ? TEXT("yes") : TEXT("no"),
					ImeStatus.bRegistered ? TEXT("yes") : TEXT("no"),
					ImeStatus.bContextActive ? TEXT("yes") : TEXT("no"));
			}
		});

	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[](float)
		{
			UE_LOG(LogVaCuus, Log, TEXT("vacuus.M1HUD.TypeShot: requesting a UI screenshot"));
			FScreenshotRequest::RequestScreenshot(/*bInShowUI=*/true);
			return false;
		}),
		GHoverShotShotSeconds);

	UE_LOG(LogVaCuus, Log,
		TEXT("vacuus.M1HUD.TypeShot: click (%.0f, %.0f) and type '%s' at t+%.1fs, screenshot at t+%.1fs"),
		PendingPosition.X, PendingPosition.Y, *PendingText, GHoverShotMoveSeconds, GHoverShotShotSeconds);
}

static FAutoConsoleCommand GTypeShotCommand(
	TEXT("vacuus.M1HUD.TypeShot"),
	TEXT("Click the field at <x> <y> after 2s, type [text] into it, then take a UI screenshot at 3s. ")
	TEXT("Headless text-entry proof."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&TypeShot));

static FAutoConsoleCommand GNavShotCommand(
	TEXT("vacuus.M1HUD.NavShot"),
	TEXT("Focus the HUD and send a key sequence after 2s, then take a UI screenshot at 3s. ")
	TEXT("No arguments: two DPad-right steps."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&NavShot));

static FAutoConsoleCommand GToggleCommand(
	TEXT("vacuus.M1HUD"),
	TEXT("Toggle the M1 render-spike HUD: records an RmlUi document (DevUI/m1_hud.rml, or an inline fallback) ")
	TEXT("each frame and composites it over the game viewport."),
	FConsoleCommandDelegate::CreateLambda([] { Toggle(GM1HudVfsPath); }));

static FAutoConsoleCommand GM2DemoCommand(
	TEXT("vacuus.M2Demo"),
	TEXT("Toggle the M2 interaction demo (DevUI/m2_demo.rml): buttons with :hover/:active/:focus, a ")
	TEXT("wheel-scrollable list, a text field, a vacuus-passthrough region and nav-annotated focusables. ")
	TEXT("Shares every vacuus.M1HUD.* sub-command, because those are about the view and not the document."),
	FConsoleCommandDelegate::CreateLambda([] { Toggle(GM2DemoVfsPath); }));

static FAutoConsoleCommand GM3DemoCommand(
	TEXT("vacuus.M3Demo"),
	TEXT("Toggle the M3 data-binding demo (DevUI/m3_demo.rml): a USTRUCT carrying one property of every bound kind, ")
	TEXT("driven every frame into a live RmlUi data model -- text substitution, a float as bar geometry, a flattened ")
	TEXT("nested struct, a bound class, a button whose write is refused, and a data-for killfeed over a TArray of ")
	TEXT("struct rows (a row per ~1.5s, front-trimmed above 6). Pair it with vacuus.DumpModel and vacuus.M3Demo.Freeze."),
	FConsoleCommandDelegate::CreateLambda([] { Toggle(GM3DemoVfsPath); }));

/**
 * Stops or restarts the demo's mutation while leaving UpdateModel running every frame.
 *
 * THE IDLE ROW, MADE OBSERVABLE ON A REAL SCREEN (spec 9). Frozen, the game thread pushes a
 * byte-identical struct every frame: the differ marks nothing, the channel never swaps, the UI
 * thread applies nothing, the DOM does not move, and the idle gate withholds every frame. The
 * two numbers that say so are on different sides -- published frames (vacuus.M1HUD.PerfLog) and
 * applied fields (vacuus.DumpModel) -- and spec 9 records why only both together discriminate.
 */
static void FreezeDemoModel(const TArray<FString>& Args)
{
	if (!GState || !GState->ModelDriverHandle.IsValid())
	{
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.M3Demo.Freeze needs the M3 demo to be on"));
		return;
	}

	// No argument means "freeze"; an explicit 0 thaws. Same convention as
	// vacuus.M1HUD.PassThroughKey.
	GState->bModelFrozen = Args.Num() == 0 || FCString::Atoi(*Args[0]) != 0;

	if (!GState->bModelFrozen)
	{
		// Rebased so the sweep resumes from the top rather than jumping to wherever wall clock
		// happens to be, which would look like a glitch rather than a resume.
		GState->ModelDriverStartSeconds = FPlatformTime::Seconds();

		// The killfeed beat counts on the clock the line above just rebased; left alone it
		// would sit above every new KillBeat and the feed would stay silent for as long as the
		// freeze lasted. KillfeedSerial is deliberately NOT reset -- content continues, only
		// the cadence restarts (see the FState members).
		GState->LastKillfeedBeat = 0;
	}

	UE_LOG(LogVaCuus, Display,
		TEXT("vacuus.M3Demo.Freeze: the demo model is now %s -- UpdateModel is still called every frame either way"),
		GState->bModelFrozen ? TEXT("FROZEN (nothing should publish, and nothing should be applied)") : TEXT("running"));
}

static FAutoConsoleCommand GM3FreezeCommand(
	TEXT("vacuus.M3Demo.Freeze"),
	TEXT("Freeze [1] or thaw [0] the M3 demo's values while UpdateModel keeps being called every frame. Frozen is ")
	TEXT("spec 9's idle row: 0 published frames AND 0 fields applied."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&FreezeDemoModel));

static void Wheel(const TArray<FString>& Args)
{
	if (Args.Num() < 3)
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("vacuus.M2Demo.Wheel expects <x> <y> <delta> [delaySeconds]: window pixels, then notches (+ is up)"));
		return;
	}

	const FVector2D Position(FCString::Atof(*Args[0]), FCString::Atof(*Args[1]));
	const float Delta = FCString::Atof(*Args[2]);
	const float DelaySeconds = Args.Num() > 3 ? FCString::Atof(*Args[3]) : 0.0f;

	ScheduleAfter(DelaySeconds, [Position, Delta] { WheelAt(Position, Delta); });
}

static void Drag(const TArray<FString>& Args)
{
	if (Args.Num() < 4)
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("vacuus.M2Demo.Drag expects <x0> <y0> <x1> <y1> [steps] [delaySeconds], in window pixels"));
		return;
	}

	const FVector2D From(FCString::Atof(*Args[0]), FCString::Atof(*Args[1]));
	const FVector2D To(FCString::Atof(*Args[2]), FCString::Atof(*Args[3]));
	const int32 Steps = Args.Num() > 4 ? FCString::Atoi(*Args[4]) : 8;
	const float DelaySeconds = Args.Num() > 5 ? FCString::Atof(*Args[5]) : 0.0f;

	ScheduleAfter(DelaySeconds, [From, To, Steps] { DragFromTo(From, To, Steps); });
}

static FAutoConsoleCommand GWheelCommand(
	TEXT("vacuus.M2Demo.Wheel"),
	TEXT("Move the pointer to <x> <y> and turn the wheel <delta> notches (+ is up) through Slate's real ")
	TEXT("routing. Optional [delaySeconds]. Headless scroll verification."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&Wheel));

static FAutoConsoleCommand GDragCommand(
	TEXT("vacuus.M2Demo.Drag"),
	TEXT("Press at <x0> <y0>, drag to <x1> <y1> in [steps] moves, release. Optional [delaySeconds]. ")
	TEXT("Headless drag verification -- the scrollbar case pointer capture exists for."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&Drag));

static FAutoConsoleCommand GRectsCommand(
	TEXT("vacuus.M2Demo.Rects"),
	TEXT("Print the interactive-region snapshot the game thread is answering Slate from: every rect, its ")
	TEXT("flags, and the view-level focus/entry facts. Optional [delaySeconds] for -ExecCmds use, where ")
	TEXT("everything runs before the first frame. Works for whichever demo is on."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&Rects));

static FAutoConsoleCommand GHitCommand(
	TEXT("vacuus.M2Demo.Hit"),
	TEXT("Ask the published snapshot what a press at <x> <y> (VIEW pixels) would be answered: covered, ")
	TEXT("focusable, text-input. Optional [delaySeconds]. The pass-through assertion."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&HitTest));
} // namespace VaCuusM1HUD

class FVaCuusRenderModule : public IModuleInterface
{
public:
	//~ Begin IModuleInterface
	virtual void StartupModule() override
	{
		// Map the plugin shader directory before global shader compilation kicks
		// in — the module loads at PostConfigInit for exactly this reason.
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("VaCuus"));
		checkf(Plugin.IsValid(), TEXT("VaCuus plugin descriptor not found"));
		const FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/VaCuus"), ShaderDir);

		// The one game-thread window in which the recorder's image path may touch the
		// module manager at all: LoadTexture runs on the UI thread, where loading a
		// module is refused outright. See CacheImageWrapperModule.
		FVaCuusRecordingRenderInterface::CacheImageWrapperModule();
	}

	virtual void ShutdownModule() override
	{
		// Engine shutdown with the HUD still on: drop the widget and our own
		// references. The view itself is normally already gone (the game instance's
		// subsystem deinitializes before modules unload), and the UI thread was
		// stopped by VaCuus::ShutdownModule() ahead of this one.
		VaCuusM1HUD::TearDown();
	}
	//~ End IModuleInterface
};

IMPLEMENT_MODULE(FVaCuusRenderModule, VaCuusRender)
