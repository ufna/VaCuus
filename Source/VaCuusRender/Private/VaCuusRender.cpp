// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "SVaCuusWidget.h"
#include "VaCuusContentPaths.h"
#include "VaCuusDefines.h"
#include "VaCuusDemoModel.h"
#include "VaCuusRefHudModel.h"
#include "VaCuusMaterialDraw.h"
#include "VaCuusRecordingRenderInterface.h"
#include "VaCuusRmlDocumentHost.h"
#include "VaCuusSlateElement.h"
#include "VaCuusStyleSet.h"
#include "VaCuusSubsystem.h"
#include "VaCuusUIShaders.h"
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
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h" // complete UPackage for NewObject(GetTransientPackage()) — UObjectGlobals.h only forward-declares it
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "RenderingThread.h"
#include "ShaderCore.h"
#include "UnrealClient.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

/** The M5 acceptance demo's world-quad half (plan Task 9.1), defined in VaCuusWorldDemo.cpp (same module). */
namespace VaCuusWorldDemo
{
void SpawnM5HudQuad(const TCHAR* DocumentVfsPath, float DelaySeconds);
void TearDownM5HudQuad();
}	 // namespace VaCuusWorldDemo

namespace VaCuusM1HUD
{
/**
 * The demo documents this toggle can bring up. FVaCuusFileInterface resolves relative
 * paths against the ordered DevUI roots (plugin's Content/DevUI first -- D19).
 */
static const TCHAR* GM1HudVfsPath = TEXT("m1_hud.rml");
static const TCHAR* GM2DemoVfsPath = TEXT("m2_demo.rml");
static const TCHAR* GM3DemoVfsPath = TEXT("m3_demo.rml");
static const TCHAR* GM4DemoVfsPath = TEXT("m4_demo.rml");
static const TCHAR* GM5GlassVfsPath = TEXT("m5_glass.rml");
static const TCHAR* GM5DecoVfsPath = TEXT("m5_deco.rml");
static const TCHAR* GM5DecoPlainVfsPath = TEXT("m5_deco_plain.rml");
static const TCHAR* GM5MatSpikeVfsPath = TEXT("m5_matspike.rml");
static const TCHAR* GM5HudVfsPath = TEXT("M5Hud/m5_hud.rml");
static const TCHAR* GRefHudVfsPath = TEXT("RefHud/refhud.rml");

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
 * synthesized move over the demo button was reported HANDLED -- by the captor -- while
 * RmlUi's hover never changed. That run is also why the move/wheel/key commands below no
 * longer name a culprit on their handled branch: Slate's answer there was true and useless.
 *
 * GameAndUI rather than UIOnly, because Task 6's contract is that clicks the UI does
 * not claim still reach the game -- which is exactly what GameAndUI means.
 *
 * This belongs to the debug toggle, not to the plugin: a real game decides its own
 * input mode, and a UMG-hosted VaCuus widget (Task 8) inherits whatever the game
 * already set.
 *
 * External linkage (not static): vacuus.LobbyDemo (VaCuusLobbyDemo.cpp, same module)
 * shares it by forward declaration, the SpawnM5HudQuad pattern in reverse.
 */
void SetUIInputMode(UWorld* World, bool bEnable)
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

	//~ ------------------------------------------------------- M4 JS demo (plan Task 10.1)

	/**
	 * True while the M4 demo document is the one up. It gates the REDUCED driver (spec 9):
	 * PumpDemoModel keeps feeding the gameplay fields -- Health, the scalar beats, the nested
	 * struct -- through UpdateModel exactly as M3, and stops feeding what JS now owns: the
	 * killfeed, Stance, and Ammo's animation (Ammo becomes the accepted-write loop's state,
	 * mutated only by the OnModelWrite handler below).
	 */
	bool bM4Demo = false;

	/**
	 * The OnModelWrite ear (see UVaCuusDemoWriteListener): the M3 refusal button's click now
	 * routes (hud, "Ammo", value) to the game thread, this handler applies it to DemoModel,
	 * and the next pump's UpdateModel echoes it back through binding -- the accepted-write
	 * loop, one write per click. Strong-pointed because nothing else roots the adapter.
	 */
	TStrongObjectPtr<UVaCuusDemoWriteListener> M4WriteListener;

	//~ ------------------------------------------------------- M5 glass demo (plan Task 3)

	/**
	 * OnBeginFrame subscription that slowly yaws the player camera while the M5 glass demo
	 * is up. THE POINT IS THE IDLE-FREEZE OBSERVATION (Exp-GLASS-IDLE-FREEZE): the glass
	 * document is deliberately STATIC — publishes go to ~zero after the settle — while the
	 * scene under it keeps moving, so two screenshots a few seconds apart show a CHANGED
	 * blurred backdrop under an idle HUD, which is exactly what composite-time glass buys
	 * and replay-baked glass cannot do.
	 */
	FDelegateHandle CameraPanHandle;
	double CameraPanStartSeconds = 0.0;
	float CameraPanBaseYaw = 0.0f;

	//~ ------------------------------------------------------- M5 acceptance demo (plan Task 9.1)

	/**
	 * True while the M5 acceptance demo (vacuus.M5Demo) is the document up. It changes two
	 * behaviors: PumpCameraPan OSCILLATES (±10°) instead of panning linearly — the scene
	 * still moves every frame under the glass, but the world quad SpawnM5HudQuad placed
	 * 16° right stays in frame for both screenshot beats — and TearDown retires that quad.
	 */
	bool bM5Demo = false;

	//~ ------------------------------------------------------- M5 material demo (plan Task 5b)

	/**
	 * The transient style set the material demo registers on the way up and unregisters
	 * on the way down — the production registration path, driven exactly as a game would
	 * drive it (an asset of keys -> the committed spike materials; the spike's
	 * vacuus.MatSpike.* console registry is retired). Strong-pointed because the
	 * registry roots only the MATERIALS, not the set object naming them.
	 */
	TStrongObjectPtr<UVaCuusStyleSet> DemoStyleSet;

	//~ ------------------------------------------------------- M6 reference HUD (plan Task 4)

	/** True while RefHud/refhud.rml is the document up; gates the RefHud pump below. */
	bool bRefHud = false;

	/**
	 * The reference HUD's live struct (model 'refhud'), the DemoModel pattern at
	 * reference scale: two 24-row team arrays with independent dirty scopes plus the
	 * plate/ammo scalars, all pushed through ONE UpdateModel per frame -- the diff
	 * inside marks only what moved, so an ordinary frame publishes plate scalars and
	 * NOTHING of either array.
	 */
	FVaCuusRefHudModel RefHudModel;

	/**
	 * The 2-second beat of the last sparse scoreboard bump (the LastKillfeedBeat
	 * shape): each beat mutates ONE row of ONE panel, panels strictly alternating --
	 * so per beat exactly one of the two arrays dirties, which is the
	 * independent-dirty-scope design doing its work on screen (spec 2(h)).
	 */
	int32 LastRefHudStatBeat = 0;
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

	// The M5 camera pan is the same per-frame subscription shape as the model driver and
	// leaves with it, for the same reason.
	if (State->CameraPanHandle.IsValid())
	{
		FCoreDelegates::OnBeginFrame.Remove(State->CameraPanHandle);
		State->CameraPanHandle.Reset();
	}

	// The M5 acceptance demo's world quad leaves with the screen half that spawned it.
	// A no-op for every other panel: bM5DemoQuad scopes the call to the demo's own.
	if (State->bM5Demo)
	{
		VaCuusWorldDemo::TearDownM5HudQuad();
	}

	// The M4 write ear goes with the driver it fed: unbind before the view is retired so a
	// write drained THIS tick cannot reach a handler whose state was just moved out, then
	// clear the forwarding function so a stale broadcast on any path forwards to nothing.
	if (State->M4WriteListener.IsValid())
	{
		if (UVaCuusView* View = State->View.Get())
		{
			View->OnModelWrite.RemoveDynamic(State->M4WriteListener.Get(), &UVaCuusDemoWriteListener::HandleModelWrite);
		}
		State->M4WriteListener->OnWrite = nullptr;
		State->M4WriteListener.Reset();
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

	// 4. The material demo's style set. After the view retirement above on purpose: the
	// view's teardown frames may still carry material draws, and unregistering first
	// would turn them into (harmless, but noisy) unresolved-id skips. The registry's
	// deferred-release fence handles whatever is still in flight.
	if (State->DemoStyleSet.IsValid())
	{
		FVaCuusStyleRegistry::UnregisterStyleSet(State->DemoStyleSet.Get());
		State->DemoStyleSet.Reset();
	}

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

	// THE M4 SPLIT (spec 9): the M4 demo keeps this driver for every gameplay-fed field and
	// hands three surfaces to JS. Ammo stops animating -- it is now the accepted-write loop's
	// state, written only by the OnModelWrite handler and echoed back through the UpdateModel
	// below (an animated Ammo would stomp every routed write one frame later). Stance stops
	// moving -- m4_demo.rml does not bind it; m4_hud_logic.js drives the stance pill through
	// classList on the same 2 s beat. The killfeed stops being pumped -- the M4 feed is
	// JS-built DOM with the same serial arithmetic, and two feeds would be two truths.
	if (!GState->bM4Demo)
	{
		Model.Ammo = 30 - (int32(Elapsed) % 31);

		static const TCHAR* StanceNames[] = {TEXT("Standing"), TEXT("Crouched"), TEXT("Prone")};
		const int32 StanceIndex = int32(Elapsed / 2.0) % UE_ARRAY_COUNT(StanceNames);
		Model.Stance = static_cast<EVaCuusDemoStance>(StanceIndex);
	}

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
	if (!GState->bM4Demo && KillBeat > GState->LastKillfeedBeat)
	{
		GState->LastKillfeedBeat = KillBeat;
		const int32 Serial = GState->KillfeedSerial++;

		// Deterministic from the serial, so two HITCH-FREE runs of the same length show the
		// same feed -- which is what lets a headless screenshot be checked against an
		// expectation instead of merely glanced at. A >1.5 s hitch narrows that guarantee:
		// this pump appends at most one row while LastKillfeedBeat jumps to the hitched
		// beat, so the skipped beat's row never exists -- the row COUNT then differs from a
		// hitch-free run, though the content stays serial-sequential (rows still come from
		// consecutive serials). Pool sizes 5, 7 and 4 are pairwise coprime, so the
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
 * One frame of the M5 glass demo's camera pan: a slow wall-clock yaw so the scene keeps
 * moving under a HUD that publishes nothing. See FState::CameraPanHandle for why the
 * motion is the demo's whole point. Wall clock, not frame count, so a screenshot at t+N
 * seconds photographs the same heading whatever the frame rate.
 */
static void PumpCameraPan()
{
	if (!GState || !GState->CameraPanHandle.IsValid())
	{
		return;
	}

	APlayerController* PlayerController =
		GState->InputWorld.IsValid() ? GState->InputWorld->GetFirstPlayerController() : nullptr;
	if (!PlayerController)
	{
		return;
	}

	const double Elapsed = FPlatformTime::Seconds() - GState->CameraPanStartSeconds;

	// 12 deg/s: slow enough that a 10-frame AutoShot is not motion-smeared, fast enough
	// that two beats a few seconds apart photograph visibly different backdrops.
	// The M5 acceptance demo OSCILLATES instead (±10° at 0.5 rad/s): the scene still
	// moves every frame — two beats still photograph different backdrops through the
	// glass — but the world quad placed 16° right of the base heading stays in frame
	// for the whole session, which a linear pan would sweep away in seconds.
	FRotator Rotation = PlayerController->GetControlRotation();
	Rotation.Yaw = GState->bM5Demo
					   ? GState->CameraPanBaseYaw + 10.0f * float(FMath::Sin(Elapsed * 0.5))
					   : GState->CameraPanBaseYaw + float(Elapsed) * 12.0f;
	PlayerController->SetControlRotation(Rotation);
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

	// The bare string, NEVER FName(GM3ModelName): in a cooked game an FName round-trip returns
	// the pool's first-registered casing -- the engine's 'HUD' (class AHUD) -- and the model
	// binds under a name `data-model="hud"` cannot reach (VaCuus-akj.23; the mechanism is on
	// UVaCuusView::BindModel).
	if (!View->BindModel(GM3ModelName, FVaCuusDemoModel::StaticStruct()))
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

	// The M4 half of the accepted-write loop (spec 3.10, plan Task 10.1): the button's
	// routed `Ammo = Ammo - 1` arrives here on the game thread -- Path carries the wire
	// path WITHOUT the model name (that is the Model parameter) -- the live struct takes
	// the value, and the next PumpDemoModel's UpdateModel is the echo. The write is heard
	// exactly ONCE per click: the apply that follows re-renders the control from the
	// shadow, and any write the re-render's own machinery would fire with the model's
	// current value is swallowed by the router's echo rule (spec 3.10).
	if (GState->bM4Demo)
	{
		GState->M4WriteListener = TStrongObjectPtr<UVaCuusDemoWriteListener>(NewObject<UVaCuusDemoWriteListener>());
		GState->M4WriteListener->OnWrite = [](FName Model, const FString& Path, const FVaCuusJsValue& Value)
		{
			if (!GState || !GState->bM4Demo)
			{
				return;
			}

			if (Model != FName(GM3ModelName) || Path != TEXT("Ammo") || Value.Kind != EVaCuusJsValueKind::Number)
			{
				UE_LOG(LogVaCuus, Warning,
					TEXT("vacuus.M4Demo: routed write to '%s' path '%s' (kind %d) has no demo meaning; ignored"),
					*Model.ToString(), *Path, int32(Value.Kind));
				return;
			}

			// Clamped, not trusted: the value came from a document expression, and a HUD that
			// can be clicked into negative ammo is a HUD that can be clicked into anything.
			GState->DemoModel.Ammo = FMath::Max(0, FMath::RoundToInt32(Value.Number));
			UE_LOG(LogVaCuus, Display,
				TEXT("vacuus.M4Demo: accepted routed write Ammo = %d; the next UpdateModel echoes it back through binding"),
				GState->DemoModel.Ammo);
		};
		View->OnModelWrite.AddDynamic(GState->M4WriteListener.Get(), &UVaCuusDemoWriteListener::HandleModelWrite);
	}
}

//~ ----------------------------------------------------------- M6 reference HUD (plan Task 4)

/** The reference HUD's model name -- the bare lowercase string, the akj.23 FName-casing watch. */
static const TCHAR* GRefHudModelName = TEXT("refhud");

/**
 * Seeds both team panels, serial-deterministic (no RNG anywhere in this demo): every
 * value is a pure function of (panel, row), so two boots show the same board and a
 * screenshot's row is computable. PanelSeed skews the two boards apart -- identical
 * panels would make a cross-panel binding mixup invisible on screen.
 */
static void SeedRefHudPanel(TArray<FVaCuusRefHudScoreRow>& Rows, int32 PanelSeed, const TCHAR* Prefix)
{
	static const TCHAR* Squads[] = {TEXT("RAPTOR"), TEXT("VIPER"), TEXT("GHOST"), TEXT("NOMAD"), TEXT("HAVOC")};

	Rows.Reset();
	Rows.Reserve(24);
	for (int32 Index = 0; Index < 24; ++Index)
	{
		FVaCuusRefHudScoreRow& Row = Rows.AddDefaulted_GetRef();
		Row.Rank = Index + 1;
		Row.Name = FString::Printf(TEXT("%s-%s-%02d"), Prefix, Squads[(Index + PanelSeed) % UE_ARRAY_COUNT(Squads)], Index + 1);
		Row.Kills = (37 * (Index + PanelSeed)) % 40;
		Row.Deaths = (23 * Index + PanelSeed) % 30;
		Row.Assists = (11 * Index + PanelSeed * 5) % 15;
		Row.Score = 2500 - Index * 85 - PanelSeed * 13;
		Row.Ping = 20 + (Index * 7 + PanelSeed * 3) % 90;
	}
}

/**
 * One frame of the reference HUD's C++ feed (spec 2(h)'s binding third): plate bars,
 * ammo and objective sweep on wall clock every frame; the scoreboard gets a SPARSE
 * stat bump -- one row, one panel, every 2 s, panels alternating -- so on an ordinary
 * frame neither array dirties, and on a beat frame exactly ONE does. That cadence is
 * the two-array design's showcase: the bump re-evaluates only its own panel's ~192
 * bindings (the DirtyScope test proves it by exact counter deltas).
 *
 * ONE UpdateModel WITH THE WHOLE STRUCT, the PumpDemoModel contract: the diff is
 * inside, and UVaCuusSubsystem::Tick coalesces whatever was marked into one publish.
 */
static void PumpRefHudModel()
{
	if (!GState || !GState->bRefHud || !GState->View.IsValid())
	{
		return;
	}

	// Function-local static for PumpDemoModel's reason: no FName construction from a
	// literal at static-init time, and a load instead of a hash per frame.
	static const FName ModelName(GRefHudModelName);

	FVaCuusRefHudModel& Model = GState->RefHudModel;
	const double Elapsed = FPlatformTime::Seconds() - GState->ModelDriverStartSeconds;

	// The M3 demo's 10 s triangle for HP; MP on a 16 s one so the two bars never lock
	// phase (a stuck pipeline would freeze them at a glance-checkable pair).
	const double HpPhase = FMath::Fmod(Elapsed, 10.0);
	Model.Health = float(HpPhase < 5.0 ? 100.0 - HpPhase * 20.0 : (HpPhase - 5.0) * 20.0);
	const double MpPhase = FMath::Fmod(Elapsed, 16.0);
	Model.Mana = float(MpPhase < 8.0 ? 100.0 - MpPhase * 12.5 : (MpPhase - 8.0) * 12.5);

	// Two rounds a second; every emptied magazine takes 30 from the reserve, which
	// refills on wrap -- all pure functions of elapsed time.
	const int32 RoundsFired = int32(Elapsed * 2.0);
	Model.Ammo = 30 - (RoundsFired % 31);
	Model.AmmoReserve = 120 - 30 * ((RoundsFired / 31) % 4);

	Model.PlayerName = TEXT("UFNA-01");
	Model.Objective = FString::Printf(TEXT("HOLD THE LINE // WAVE %02d"), int32(Elapsed / 5.0) % 100);

	// The sparse scoreboard bump. `>` not `!=`, the LastKillfeedBeat rule: time only
	// moves the board forward. Row hops by a 7-stride so consecutive bumps land far
	// apart on screen; Kills and Score move together like a real board's would.
	const int32 StatBeat = int32(Elapsed / 2.0);
	if (StatBeat > GState->LastRefHudStatBeat)
	{
		GState->LastRefHudStatBeat = StatBeat;
		TArray<FVaCuusRefHudScoreRow>& Panel = (StatBeat % 2 == 0) ? Model.TeamAlpha : Model.TeamBravo;
		if (Panel.Num() == 24)
		{
			FVaCuusRefHudScoreRow& Row = Panel[(StatBeat * 7) % 24];
			Row.Kills += 1;
			Row.Score += 100;
		}
	}

	GState->View->UpdateModel(ModelName, FVaCuusRefHudModel::StaticStruct(), &Model);
}

/**
 * Binds model 'refhud' and starts the reference HUD's per-frame feed. BEFORE the
 * view's first LoadDocument, which is RmlUi's requirement and not a preference --
 * StartModelDriver carries the full Element::SetParent argument; the single-producer
 * FIFO is what carries the ordering across the thread boundary.
 */
static void StartRefHudDriver(UVaCuusView* View)
{
	if (!GState || View == nullptr)
	{
		return;
	}

	if (!View->BindModel(GRefHudModelName, FVaCuusRefHudModel::StaticStruct()))
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("vacuus.RefHud: the data model could not be bound, so refhud.rml will load with empty panels. ")
			TEXT("Run vacuus.DumpModel to see which side has the model"));
		return;
	}

	GState->RefHudModel = FVaCuusRefHudModel();
	SeedRefHudPanel(GState->RefHudModel.TeamAlpha, /*PanelSeed=*/0, TEXT("ALFA"));
	SeedRefHudPanel(GState->RefHudModel.TeamBravo, /*PanelSeed=*/1, TEXT("BRVO"));
	GState->ModelDriverStartSeconds = FPlatformTime::Seconds();
	GState->LastRefHudStatBeat = 0;
	GState->ModelDriverHandle = FCoreDelegates::OnBeginFrame.AddStatic(&PumpRefHudModel);
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

	// A WARNING, NOT A REFUSAL, and the difference is this demo's charter: it has an inline
	// fallback precisely so the toggle works on a bare project, so bad art must not stop it.
	// But m1_hud.rml references exactly one image, and if that image is absent -- or is a
	// Git-LFS pointer, which this repo's `*.png filter=lfs` makes possible on any clone made
	// without git-lfs -- the HUD renders with an empty box where the avatar goes and nothing
	// says why. Bead VaCuus-akj.28; the lobby demo makes the same check fatal because it has
	// no fallback to fall back to.
	if (bLoadFromFile)
	{
		FString Diagnosis;
		if (VaCuusContentPaths::ProbeImage(TEXT("img/avatar.png"), &Diagnosis) != EVaCuusImageProbe::Ok)
		{
			UE_LOG(LogVaCuus, Warning,
				TEXT("VaCuus demo: %s. The HUD still loads; its avatar box will be empty"), *Diagnosis);
		}
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
		//
		// THE DOCUMENT IS INTERPOLATED, NOT SPELLED: this is Toggle()'s SHARED path, reached
		// by nine console toggles and two Shipping launch flags over ten documents, and its two
		// neighbours above already say the generic thing. A literal here goes stale the next
		// time a document is added, and it names the wrong one every time it fires today.
		UE_LOG(LogVaCuus, Error, TEXT("VaCuus demo: no view could be created; '%s' not shown"),
			GDocumentVfsPath);
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
	// go nowhere, and this file's other two documents have none. The M4 demo binds the SAME
	// model and drives it through the same pump; bM4Demo is what reduces the pump to the
	// gameplay-fed fields (spec 9) and arms the OnModelWrite ear.
	if (FCString::Strcmp(GDocumentVfsPath, GM3DemoVfsPath) == 0 ||
		FCString::Strcmp(GDocumentVfsPath, GM4DemoVfsPath) == 0)
	{
		GState->bM4Demo = FCString::Strcmp(GDocumentVfsPath, GM4DemoVfsPath) == 0;
		StartModelDriver(View);
	}

	// The M5 glass demo pans the camera instead of driving a model: the DOCUMENT stays
	// static (publishes ~zero after the settle) while the SCENE moves — the exact split
	// composite-time glass exists for. See PumpCameraPan.
	if (FCString::Strcmp(GDocumentVfsPath, GM5GlassVfsPath) == 0)
	{
		GState->CameraPanStartSeconds = FPlatformTime::Seconds();
		if (APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr)
		{
			GState->CameraPanBaseYaw = float(PlayerController->GetControlRotation().Yaw);
		}
		GState->CameraPanHandle = FCoreDelegates::OnBeginFrame.AddStatic(&PumpCameraPan);
	}

	// The M5 material demo (Task 5b, the production path end to end): register a style
	// set over the committed spike materials BEFORE the load below — the snapshot then
	// drains ahead of the document (FIFO from this one producer), so the document's
	// `decorator: shader(spike-*)` keys resolve on their very first compile. The
	// document is otherwise static: after the settle, every publish it makes is the
	// forced-republish remedy keeping the anim cell's Time-driven material moving.
	if (FCString::Strcmp(GDocumentVfsPath, GM5MatSpikeVfsPath) == 0)
	{
		static const TPair<const TCHAR*, const TCHAR*> DemoMaterials[] = {
			{TEXT("spike-translucent"), TEXT("/VaCuus/Spike/M_VaCuusSpike_Translucent.M_VaCuusSpike_Translucent")},
			{TEXT("spike-additive"), TEXT("/VaCuus/Spike/M_VaCuusSpike_Additive.M_VaCuusSpike_Additive")},
			{TEXT("spike-opaque"), TEXT("/VaCuus/Spike/M_VaCuusSpike_Opaque.M_VaCuusSpike_Opaque")},
			{TEXT("spike-mid"), TEXT("/VaCuus/Spike/M_VaCuusSpike_MID.M_VaCuusSpike_MID")},
			{TEXT("spike-anim"), TEXT("/VaCuus/Spike/M_VaCuusSpike_Anim.M_VaCuusSpike_Anim")}};

		UVaCuusStyleSet* StyleSet = NewObject<UVaCuusStyleSet>(GetTransientPackage(), TEXT("VaCuusM5MatDemoStyleSet"));
		for (const TPair<const TCHAR*, const TCHAR*>& Pair : DemoMaterials)
		{
			if (UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, Pair.Value))
			{
				StyleSet->Materials.Add(Pair.Key, Material);
			}
			else
			{
				UE_LOG(LogVaCuus, Warning, TEXT("vacuus.M5MatSpike: '%s' did not load; its cell will refuse"), Pair.Value);
			}
		}

		GState->DemoStyleSet = TStrongObjectPtr<UVaCuusStyleSet>(StyleSet);
		Subsystem->RegisterStyleSet(StyleSet);
	}

	// The M5 acceptance demo (plan Task 9.1, spec §8): the committed TSX bundle's
	// document on screen over glass + gradient + builtin decorators, model 'hud' fed
	// by the game, a sample translation table, the scene oscillating under the blur,
	// and the SAME document on a raycast-clickable world quad. ORDER IS THE CONTRACT
	// twice over: BindModel before LoadDocument (StartModelDriver's RmlUi argument),
	// and SetTranslationTable before it too — document parse translates RML text
	// against the INSTALLED snapshot (FVaCuusSystemInterface::TranslateString), and
	// the JS half reads the same snapshot; the drain is FIFO from this one producer,
	// which is what carries both ahead of the load below.
	if (FCString::Strcmp(GDocumentVfsPath, GM5HudVfsPath) == 0)
	{
		GState->bM5Demo = true;

		// Display, not Log, deliberately (the packaged-Shipping gate, plan 9.3b): the
		// demo's boot must stay observable in the most stripped configuration a buyer
		// ships — Display outranks every compile- and runtime-verbosity floor short of
		// logging-off entirely. One line, naming the milestone; everything else stays Log.
		UE_LOG(LogVaCuus, Display,
			TEXT("VaCuus M5 acceptance demo: TSX HUD + glass + decorators + world quad coming up (M5Hud/m5_hud.rml)"));

		// The localized string of spec §8: the killfeed pattern is a translate() KEY
		// whose params are user data. A screenshot row reading "Vex » Kilo" (not
		// "Vex downed Kilo") proves table hit + single-pass substitution end to end;
		// "Kill Feed" proves the RML-independent JS path on a plain string.
		TMap<FString, FString> Table;
		Table.Add(TEXT("{killer} downed {victim}"), TEXT("{killer} » {victim}"));
		Table.Add(TEXT("Killfeed"), TEXT("Kill Feed"));
		Subsystem->SetTranslationTable(Table);

		// Binds 'hud' — the bare lowercase string, the akj.23 FName-casing watch —
		// and pumps Health every frame; the TSX HUD reads it per rAF, change-gated.
		StartModelDriver(View);

		// The moving scene under the glass panels (the M5Glass shape, oscillating
		// per PumpCameraPan's M5 branch), and the world-quad half at +0.5 s so the
		// player controller and camera exist even in a -ExecCmds frame-0 run.
		GState->CameraPanStartSeconds = FPlatformTime::Seconds();
		if (APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr)
		{
			GState->CameraPanBaseYaw = float(PlayerController->GetControlRotation().Yaw);
		}
		GState->CameraPanHandle = FCoreDelegates::OnBeginFrame.AddStatic(&PumpCameraPan);

		VaCuusWorldDemo::SpawnM5HudQuad(GM5HudVfsPath, /*DelaySeconds=*/0.5f);
	}

	// The M6 reference HUD (plan Task 4): model 'refhud' bound and seeded BEFORE the
	// load below (the StartModelDriver ordering contract), then fed every frame --
	// the C++ third of the spec 2(h) driver split; refhud_logic.js and the RCSS
	// keyframes are the other two and need nothing from this side.
	if (FCString::Strcmp(GDocumentVfsPath, GRefHudVfsPath) == 0)
	{
		GState->bRefHud = true;
		StartRefHudDriver(View);
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
 * Runs Work after DelaySeconds, or immediately when it is not positive.
 *
 * WHY THE DELAY EXISTS AT ALL: every `-ExecCmds` command runs on the SAME early tick, before
 * the widget has ever been arranged and before any frame has been published -- so a rect dump
 * issued there prints an empty default snapshot and a hit test answers "no" for every point.
 * HoverShot, NavShot and TypeShot all solve this with a timed second step; this is the same
 * pattern for the two read-only commands, so a headless acceptance run can put the query on
 * the same command line as the toggle.
 *
 * External linkage: shared with VaCuusLobbyDemo.cpp by forward declaration.
 */
void ScheduleAfter(float DelaySeconds, TFunction<void()> Work)
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

static void Rects(const TArray<FString>& Args)
{
	ScheduleAfter(Args.Num() > 0 ? FCString::Atof(*Args[0]) : 0.0f, &DumpRects);
}

/**
 * Answers "is this point covered, and how" the way the widget's handlers would.
 *
 * The complement of the dump above: the dump says what the UI claims, this says what a
 * click at one place would do -- which is the assertion an acceptance run wants for the
 * pass-through region, because "not in the list" and "Contains() is false" are two
 * different statements and only the second is what SVaCuusWidget returns Unhandled from.
 */
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
 *
 * External linkage: shared with VaCuusLobbyDemo.cpp by forward declaration.
 */
bool MoveMouseTo(const FVector2D& Position)
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

	// NO CULPRIT ON THE HANDLED BRANCH, and that is the honest reading rather than a
	// downgrade: the return is true if ANY widget on the bubble path took the move, and
	// SViewport is our ancestor, so "handled" has always included the case where the UI never
	// saw it (the GameOnly observation at the top of this file). A move takes no capture, so
	// unlike a press there is nothing here that could attribute it -- see
	// SVaCuusWidget::GetMouseCaptureHolder_Debug and ClickWhereThePointerIs below.
	//
	// THE UNHANDLED BRANCH IS BYTE-IDENTICAL ON PURPOSE: docs/passport's macOS plan uses this
	// exact token as the coordinate self-check ("unhandled means fix the window offset and
	// re-run, not record a FAIL").
	UE_LOG(LogVaCuus, Log, TEXT("Pointer moved to (%.0f, %.0f); Slate reports the event %s"),
		Position.X, Position.Y,
		bHandled ? TEXT("handled somewhere on the bubble path (Slate does not say by whom)")
				 : TEXT("unhandled (it fell through)"));
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
 *
 * External linkage: shared with VaCuusLobbyDemo.cpp by forward declaration.
 */
bool ClickWhereThePointerIs(const FVector2D& Position)
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

	// WHOSE press was it? Not something bDownHandled can say -- the whole argument, and why
	// capture is the answer, is on SVaCuusWidget::GetMouseCaptureHolder_Debug.
	//
	// SAMPLED BETWEEN THE PRESS AND THE RELEASE, and the order is load-bearing: the release
	// hands capture back, so a sample after ProcessMouseButtonUpEvent always reads "nobody"
	// and every click would be blamed on the game.
	const bool bVaCuusTookPress = SVaCuusWidget::GetMouseCaptureHolder_Debug().IsValid();

	Slate.ProcessMouseButtonUpEvent(UpEvent);

	UE_LOG(LogVaCuus, Log,
		TEXT("Left click at (%.0f, %.0f); Slate says the press was %s, and the press was taken by %s"),
		Position.X, Position.Y,
		bDownHandled ? TEXT("handled") : TEXT("unhandled"),
		bVaCuusTookPress ? TEXT("THE UI (VaCuus captured the mouse)")
						 : TEXT("THE GAME (VaCuus declined, so it bubbled past us to SViewport)"));
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

	// Same reading as MoveMouseTo's: a wheel takes no capture either, so there is nothing here
	// that could name the widget, and the unhandled branch is byte-identical for the same
	// reason (the macOS coordinate self-check reads that token).
	UE_LOG(LogVaCuus, Log, TEXT("Wheel %+.1f at (%.0f, %.0f); Slate reports the event %s"),
		Delta, Position.X, Position.Y,
		bHandled ? TEXT("handled somewhere on the bubble path (Slate does not say by whom)")
				 : TEXT("unhandled (it fell through)"));
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

	// WHOSE press was it? The same question ClickWhereThePointerIs asks, answered from the same
	// place -- SVaCuusWidget::GetMouseCaptureHolder_Debug carries the argument, and ONE source
	// is the point: this used to reach GState->Widget, which the lobby demo does not have, so
	// the two commands could not have agreed even in principle.
	//
	// Sampled between the press and the release, because the release hands capture back.
	const bool bVaCuusTookPress = SVaCuusWidget::GetMouseCaptureHolder_Debug().IsValid();

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

	// A key press takes no mouse capture, so the same limit applies: ProcessKeyDownEvent's
	// return says the FOCUS path consumed it, not who did, and the pass-through key set
	// (D12) is a case where the widget declines on purpose. Unhandled branch byte-identical.
	UE_LOG(LogVaCuus, Log, TEXT("Key '%s' sent; Slate reports the press %s"),
		*Key.ToString(),
		bHandled ? TEXT("handled somewhere on the focus path (Slate does not say by whom)")
				 : TEXT("unhandled (it fell through)"));
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

static FAutoConsoleCommand GM4DemoCommand(
	TEXT("vacuus.M4Demo"),
	TEXT("Toggle the M4 JavaScript demo (DevUI/m4_demo.rml + m4_hud_logic.js): the M3 data-binding demo with its ")
	TEXT("killfeed, damage numbers, stance and a second health bar driven by JS (createElement/remove, timers, ")
	TEXT("classList, rAF + style proxy), while the game keeps feeding Health and the scalar panels through ")
	TEXT("UpdateModel -- and the M3 refusal button now lands in OnModelWrite and is echoed back (the accepted-write ")
	TEXT("loop). Pair it with vacuus.DumpModel and vacuus.M4Demo.Freeze."),
	FConsoleCommandDelegate::CreateLambda([] { Toggle(GM4DemoVfsPath); }));

/**
 * The M4 Freeze (spec 9: "Freeze freezes the JS clock inputs") -- BOTH halves, because the
 * M4 demo has two clocks where M3 had one:
 *
 *  - the C++ half is FreezeDemoModel verbatim: the driver keeps calling UpdateModel every
 *    frame with a byte-identical struct, which is the M3 idle row's proof shape;
 *  - the JS half is `vacuus.onFreeze(bool)`, dispatched through ExecuteScript: the script
 *    PAUSES its beats (clearInterval -- a fire-and-ignore interval would still count timer
 *    fires against the idle counters) and its rAF loop goes inert without disarming, the
 *    exact analogue of the byte-identical UpdateModel. m4_hud_logic.js documents its side.
 *
 * ExecuteScript is fire-and-forget FIFO into the same command queue as everything else, so
 * the JS pause lands on the UI thread's next frame -- one frame of skew between the two
 * halves, which no observable here can distinguish. Optional [delaySeconds] as the second
 * argument token (after the 1|0) for -ExecCmds runs, where every command executes before
 * the first frame.
 */
static void FreezeM4DemoModel(const TArray<FString>& Args)
{
	if (!GState || !GState->bM4Demo || !GState->ModelDriverHandle.IsValid())
	{
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.M4Demo.Freeze needs the M4 demo to be on"));
		return;
	}

	const float DelaySeconds = Args.Num() > 1 ? FCString::Atof(*Args[1]) : 0.0f;
	const TArray<FString> FreezeArgs = Args.Num() > 0 ? TArray<FString>{Args[0]} : TArray<FString>{};

	ScheduleAfter(DelaySeconds,
		[FreezeArgs]
		{
			if (!GState || !GState->bM4Demo || !GState->ModelDriverHandle.IsValid())
			{
				UE_LOG(LogVaCuus, Error, TEXT("vacuus.M4Demo.Freeze fired, but the M4 demo is no longer on"));
				return;
			}

			FreezeDemoModel(FreezeArgs);

			if (UVaCuusView* View = GState->View.Get())
			{
				// CallJs, NOT ExecuteScript + Printf, and this site is why VaCuus-asv exists: the
				// line here used to interpolate the argument into a JS source string behind a
				// hand-written `typeof ... === 'function'` guard. Both halves are now the API's:
				// the guard (a path that resolves to nothing warns instead of throwing) and, more
				// importantly, the argument -- which never becomes source text at all. A bool is
				// harmless to interpolate; the idiom was not, and it was the only idiom there was.
				View->CallJs(TEXT("vacuus.onFreeze"), {FVaCuusJsValue::MakeBool(GState->bModelFrozen)});
			}
		});
}

static FAutoConsoleCommand GM5DecoCommand(
	TEXT("vacuus.M5Deco"),
	TEXT("Toggle the M5 decorator demo (DevUI/m5_deco.rml): linear/repeating-linear/radial/conic gradient decorators, ")
	TEXT("the shader(glass-panel) builtin and a plain control panel. 'vacuus.M5Deco plain' loads the decorator-free ")
	TEXT("twin (m5_deco_plain.rml) — the PerfLog A/B control for the spec's gradient budget row. ")
	TEXT("Shares every vacuus.M1HUD.* sub-command."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{ Toggle(Args.Num() > 0 && Args[0] == TEXT("plain") ? GM5DecoPlainVfsPath : GM5DecoVfsPath); }));

static FAutoConsoleCommand GM5MatSpikeCommand(
	TEXT("vacuus.M5MatSpike"),
	TEXT("Toggle the M5 material-decorator demo (DevUI/m5_matspike.rml): registers a style set over the committed ")
	TEXT("spike materials and shows text cells carrying `decorator: shader(spike-*)` — the production tier end to ")
	TEXT("end (registry -> recorder -> replay), incl. the Time-driven cell the forced-republish remedy keeps ")
	TEXT("animating. Shares every vacuus.M1HUD.* sub-command."),
	FConsoleCommandDelegate::CreateLambda([] { Toggle(GM5MatSpikeVfsPath); }));

static FAutoConsoleCommand GM5DemoCommand(
	TEXT("vacuus.M5Demo"),
	TEXT("Toggle the M5 acceptance demo (DevUI/M5Hud/m5_hud.rml — the committed TSX bundle, spec §8): the Preact HUD ")
	TEXT("over glass (backdrop-filter) + gradient + shader(glass-panel) decorators, model 'hud' fed by the game every ")
	TEXT("frame, a sample translation table ('{killer} downed {victim}' -> '{killer} » {victim}'), an oscillating ")
	TEXT("camera so the scene moves under the blur, and the SAME document on a raycast-clickable world quad 16° ")
	TEXT("right. Shares every vacuus.M1HUD.* sub-command; vacuus.M5Glass.Shot works for the second beat."),
	FConsoleCommandDelegate::CreateLambda([] { Toggle(GM5HudVfsPath); }));

static FAutoConsoleCommand GRefHudCommand(
	TEXT("vacuus.RefHud"),
	TEXT("Toggle the M6 reference HUD (DevUI/RefHud/refhud.rml): ~1,750 nodes at declared steady state -- two 24-row ")
	TEXT("scoreboard panels over two independent data arrays (sparse C++ stat bumps), 64 rAF minimap blips writing one ")
	TEXT("transform each, a 52-row killfeed (12 live + 40 clipped history) with JS churn, a 24-slot timer-driven damage ")
	TEXT("pool, 18 keyframe-animated buffs, plate/ammo/objective via UpdateModel. Shares every vacuus.M1HUD.* ")
	TEXT("sub-command; vacuus.RefHud.Count logs the node count."),
	FConsoleCommandDelegate::CreateLambda([] { Toggle(GRefHudVfsPath); }));

/**
 * vacuus.RefHud.Count [delaySeconds]: the Exp-REF-COUNT field door -- see
 * UVaCuusView::DumpNodeCount. The optional delay is for -ExecCmds runs, where every
 * command executes at frame 0: a zero-delay count drains right behind the LOAD
 * command, BEFORE the frame's first Context::Update -- the data-for clones do not
 * exist yet and the answer would be the boot count spec 2(g) explicitly rejects.
 */
static FAutoConsoleCommand GRefHudCountCommand(
	TEXT("vacuus.RefHud.Count"),
	TEXT("Log the recursive node count of the demo view's document(s) after [delaySeconds], by the stated method: ")
	TEXT("elements + text nodes, hidden data-for templates excluded, RmlUi-generated scrollbars included. The answer ")
	TEXT("prints from the UI thread a frame later."),
	FConsoleCommandWithArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args)
		{
			const float DelaySeconds = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 0.0f;
			ScheduleAfter(DelaySeconds,
				[]
				{
					if (!GState || !GState->View.IsValid())
					{
						UE_LOG(LogVaCuus, Error, TEXT("vacuus.RefHud.Count needs a demo to be on"));
						return;
					}
					GState->View->DumpNodeCount();
				});
		}));

/**
 * The reference HUD's Shipping ignition (M6 Task 4) -- the -VaCuusM5Demo pattern below,
 * verbatim, for its reason: `-ExecCmds` is compiled out of Shipping, and the packaged
 * gates must be able to boot the reference workload. One screenshot at t+8 s for the
 * visual evidence; the t+8 beat also guarantees several 2 s stat bumps and killfeed
 * beats have moved the boards by shot time.
 */
static FDelegateHandle GRefHudLaunchFlagHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddLambda(
	[](UWorld* /*World*/)
	{
		if (!FParse::Param(FCommandLine::Get(), TEXT("VaCuusRefHud")))
		{
			return;
		}
		FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(GRefHudLaunchFlagHandle);
		Toggle(GRefHudVfsPath);
		ScheduleAfter(8.0f,
			[]
			{
				UE_LOG(LogVaCuus, Display, TEXT("VaCuus reference HUD (-VaCuusRefHud): requesting the gate screenshot"));
				FScreenshotRequest::RequestScreenshot(/*bInShowUI=*/true);
			});
	});

/**
 * THE MEASUREMENT IGNITIONS (M6 Task 5, spec §2(i)) — two flags in the -VaCuusM5Demo
 * pattern (below) for the same reason it exists: `-ExecCmds` is compiled out of
 * Shipping, and the passport's own venue for the RAM row IS cooked Shipping, so the
 * measurement doors must be plugin-parsed launch flags.
 *
 *   -VaCuusPerfLog   sets the vacuus.M1HUD.PerfLog cvar to 1 (cvars survive in
 *                    Shipping; only the console UI and -ExecCmds do not), so the
 *                    5-second window lines print from a packaged Shipping soak.
 *   -VaCuusMemProbe  logs FPlatformMemory::GetStats() every 5 s from map load on —
 *                    the A/B two-run delta's sampling machinery: two identical boots
 *                    (UI on vs off) print the same schedule, and the row is the
 *                    UsedPhysical delta at matched quiesced checkpoints. UsedPhysical
 *                    is the spec-named figure; Peak and virtual ride along because a
 *                    delta that disagrees with its own peak is how a transient gets
 *                    caught. Deliberately INDEPENDENT of GState/any view: the B run
 *                    (UI off) has neither.
 *
 * One handler for both flags, self-removing on the first map load like its pattern.
 */
static FDelegateHandle GPerfFlagsLaunchHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddLambda(
	[](UWorld* /*World*/)
	{
		FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(GPerfFlagsLaunchHandle);

		if (FParse::Param(FCommandLine::Get(), TEXT("VaCuusPerfLog")))
		{
			if (IConsoleVariable* PerfLogVar = IConsoleManager::Get().FindConsoleVariable(TEXT("vacuus.M1HUD.PerfLog")))
			{
				PerfLogVar->Set(1, ECVF_SetByCode);
				UE_LOG(LogVaCuus, Display, TEXT("VaCuus perf log (-VaCuusPerfLog): vacuus.M1HUD.PerfLog=1"));
			}
		}

		if (FParse::Param(FCommandLine::Get(), TEXT("VaCuusMemProbe")))
		{
			const double ProbeStartSeconds = FPlatformTime::Seconds();
			UE_LOG(LogVaCuus, Display, TEXT("VaCuus MemProbe (-VaCuusMemProbe): sampling FPlatformMemory::GetStats() every 5 s"));
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
				[ProbeStartSeconds](float)
				{
					const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
					UE_LOG(LogVaCuus, Display,
						TEXT("VaCuus MemProbe t=%.0fs: UsedPhysical=%llu (%.2f MB) PeakUsedPhysical=%llu (%.2f MB) ")
						TEXT("UsedVirtual=%llu (%.2f MB)"),
						FPlatformTime::Seconds() - ProbeStartSeconds, Stats.UsedPhysical,
						double(Stats.UsedPhysical) / (1024.0 * 1024.0), Stats.PeakUsedPhysical,
						double(Stats.PeakUsedPhysical) / (1024.0 * 1024.0), Stats.UsedVirtual,
						double(Stats.UsedVirtual) / (1024.0 * 1024.0));
					return true; // repeat for the life of the run
				}),
				5.0f);
		}
	});

/**
 * THE SHIPPING IGNITION (plan 9.3b, found at the packaged gate): `-ExecCmds` is
 * COMPILED OUT of Shipping builds — UnrealEngine.cpp:2543 wraps the :2552
 * QueueDeferredCommands in `#if !(UE_BUILD_SHIPPING) || ENABLE_PGO_PROFILE` — so a
 * packaged Shipping binary cannot launch any demo from the command line, and the
 * Shipping acceptance gate would be unrunnable. `-VaCuusM5Demo` is the explicit
 * opt-in that survives: plain FParse over FCommandLine (unfiltered in Shipping
 * unless a project opts into the arg allow-list), armed on the first map load so
 * the viewport and player controller exist, self-removing so map changes cannot
 * re-toggle. It also requests ONE screenshot at t+8 s — FScreenshotRequest is
 * Shipping-clean — so the gate has visual evidence where AutoShot's cvar cannot
 * be set. Works in every configuration; in Dev builds -ExecCmds stays the tool.
 */
static FDelegateHandle GM5DemoLaunchFlagHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddLambda(
	[](UWorld* /*World*/)
	{
		if (!FParse::Param(FCommandLine::Get(), TEXT("VaCuusM5Demo")))
		{
			return;
		}
		FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(GM5DemoLaunchFlagHandle);
		Toggle(GM5HudVfsPath);
		ScheduleAfter(8.0f,
			[]
			{
				UE_LOG(LogVaCuus, Display, TEXT("VaCuus M5 demo (-VaCuusM5Demo): requesting the gate screenshot"));
				FScreenshotRequest::RequestScreenshot(/*bInShowUI=*/true);
			});
	});

static FAutoConsoleCommand GM5GlassCommand(
	TEXT("vacuus.M5Glass"),
	TEXT("Toggle the M5 glass demo (DevUI/m5_glass.rml): a static document with a rounded and a square ")
	TEXT("backdrop-filter:blur(16px) panel plus a blur-free control panel, over a slowly panning camera — the ")
	TEXT("document idles while the blurred scene behind the panels keeps moving (composite-time glass, spec §2(a)). ")
	TEXT("Shares every vacuus.M1HUD.* sub-command."),
	FConsoleCommandDelegate::CreateLambda([] { Toggle(GM5GlassVfsPath); }));

/** Second screenshot beat for headless glass runs: AutoShot covers the first, this schedules the rest. */
static void M5GlassShot(const TArray<FString>& Args)
{
	const float DelaySeconds = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 0.0f;
	ScheduleAfter(DelaySeconds,
		[]
		{
			UE_LOG(LogVaCuus, Log, TEXT("vacuus.M5Glass.Shot: requesting a UI screenshot"));
			FScreenshotRequest::RequestScreenshot(/*bInShowUI=*/true);
		});
}

static FAutoConsoleCommand GM5GlassShotCommand(
	TEXT("vacuus.M5Glass.Shot"),
	TEXT("Take a UI-inclusive screenshot after [delaySeconds]. The second beat of the idle-freeze observation: ")
	TEXT("with the camera panning and the document idle, two beats photograph different backdrops through the ")
	TEXT("same glass."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&M5GlassShot));

static FAutoConsoleCommand GM4FreezeCommand(
	TEXT("vacuus.M4Demo.Freeze"),
	TEXT("Freeze [1] or thaw [0] the M4 demo's two clocks, optionally after [delaySeconds]: the C++ driver keeps ")
	TEXT("calling UpdateModel with a byte-identical struct (the M3 precedent) and vacuus.onFreeze(bool) pauses the ")
	TEXT("JS beats through CallJs. Frozen, nothing publishes on either path."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&FreezeM4DemoModel));

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

		// The style registry's two render-module seams (M5 Task 5b), installed
		// register-before-boot (the FVaCuusEngine::SetRenderInterface shape): the
		// TryGetShaders pre-warm walk and the builtin-key collision check both need
		// types this module owns (FVaCuusMaterialVS/PS; the builtin table) that VaCuus
		// must not know. VaCuus loads before this module (module dependency), so the
		// registry exists; nothing can have registered yet at PostConfigInit.
		FVaCuusStyleRegistry::InstallRenderHooks(
			&VaCuusMaterialDraw::PreWarmProxy_RenderThread,
			[](const FString& Key) { return VaCuusBuiltinShaders::FindMode(Key) != INDEX_NONE; });
	}

	virtual void ShutdownModule() override
	{
		// Engine shutdown with the HUD still on: drop the widget and our own
		// references. The view itself is normally already gone (the game instance's
		// subsystem deinitializes before modules unload), and the UI thread was
		// stopped by VaCuus::ShutdownModule() ahead of this one — which also ran
		// FVaCuusStyleRegistry::Shutdown_GameThread(), clearing the hooks installed
		// above before this module's code can unload. (The spike's static RHI buffer
		// refs retired with the spike; the production material path holds none.)
		VaCuusM1HUD::TearDown();
	}
	//~ End IModuleInterface
};

IMPLEMENT_MODULE(FVaCuusRenderModule, VaCuusRender)
