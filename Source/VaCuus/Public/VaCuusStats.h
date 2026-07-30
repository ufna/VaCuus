// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Stats/Stats.h"

DECLARE_STATS_GROUP(TEXT("VaCuus"), STATGROUP_VaCuus, STATCAT_Advanced);

/*
 * WHY THIS LIVES IN THE VaCuus MODULE AND NOT NEXT TO THE RENDERER (Task 14).
 *
 * It measured only UI-thread and render-thread scopes, and both of those are driven from
 * VaCuusRender, so Private/ was the right home for it. The acceptance gate added the
 * GAME-thread side -- and the game thread's per-frame VaCuus work is split across both
 * modules: UVaCuusSubsystem::Tick (this module) polls every view's published snapshot and
 * pulses the UI thread, while SVaCuusWidget (VaCuusRender) does the per-widget work and
 * every input handler. VaCuusRender depends on VaCuus and not the other way round, so a
 * facility both must reach can only sit here. The alternative -- measuring the widget half
 * and arguing the subsystem half was small -- would have left the budget gate unmeasured
 * at exactly the seam where a future regression would land.
 *
 * Everything below is therefore VACUUS_API: the four render/UI scopes are still sampled
 * from VaCuusRender.
 */

// (UI) == the dedicated VaCuus UI thread; Update and Record used to be game-thread
// costs and moved off it in M2 Task 3.
//
// THE THREE DRAIN/APPLY SCOPES (M3a Task 0). Until M3a, FVaCuusUIThread::RunFrame() had
// no scope of its own at all, so the only measured parts of a UI frame were the two
// inside RecordAndPublishFrame(). That left the two phases that run BEFORE them
// unmeasured, and they are not small in the frames that matter: DrainCommands() performs
// a full document parse plus the first layout on a load (VaCuusUIThread.cpp:900-906 ->
// IVaCuusDocumentHost::LoadDocumentFrom*), and DrainInput() runs hit-testing, focus
// resolution and the IME surface push per event (VaCuusUIThread.cpp:927+).
//
// DataApply is declared here with no sampler yet on purpose: the M3a data apply lands at
// RunFrame()'s `(data snapshots: M3)` marker (spec 3.6), i.e. between the two phases
// above and Context::Update(). Adding its scope AFTER the apply exists would leave its
// cost folded into whichever neighbour happened to grow, which is exactly the
// unattributable result this task exists to prevent.
DECLARE_CYCLE_STAT_EXTERN(TEXT("VaCuus DrainCommands (UI)"), STAT_VaCuusDrainCommands, STATGROUP_VaCuus, VACUUS_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("VaCuus DrainInput (UI)"), STAT_VaCuusDrainInput, STATGROUP_VaCuus, VACUUS_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("VaCuus DataApply (UI)"), STAT_VaCuusDataApply, STATGROUP_VaCuus, VACUUS_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("VaCuus Update (UI)"), STAT_VaCuusUpdate, STATGROUP_VaCuus, VACUUS_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("VaCuus Record (UI)"), STAT_VaCuusRecord, STATGROUP_VaCuus, VACUUS_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("VaCuus Replay (RT)"), STAT_VaCuusReplay, STATGROUP_VaCuus, VACUUS_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("VaCuus Composite (RT)"), STAT_VaCuusComposite, STATGROUP_VaCuus, VACUUS_API);

// (GT) == the game thread, i.e. the spec's own budget line. Three scopes rather than one
// because they have three different rates and only the sum is the per-frame figure:
// GameTick and SlateTick are once per frame each, Input is once per EVENT. Folding them
// into a single scope would make avg/p50 the average of a frame's parts rather than of a
// frame, which is precisely the number the budget is stated in.
DECLARE_CYCLE_STAT_EXTERN(TEXT("VaCuus GameTick (GT)"), STAT_VaCuusGameTick, STATGROUP_VaCuus, VACUUS_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("VaCuus SlateTick (GT)"), STAT_VaCuusSlateTick, STATGROUP_VaCuus, VACUUS_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("VaCuus Input (GT)"), STAT_VaCuusInput, STATGROUP_VaCuus, VACUUS_API);

/** Per-frame counters (cleared by the stats system every frame). */
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("VaCuus Draw Calls"), STAT_VaCuusDrawCalls, STATGROUP_VaCuus, VACUUS_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("VaCuus Commands"), STAT_VaCuusCommands, STATGROUP_VaCuus, VACUUS_API);

/**
 * Log-based rolling perf capture for headless soaks (Task 10): wall-clock
 * samples taken around the same scopes as the cycle stats above, printed as
 * avg/p50/p99/max every 5 seconds while `vacuus.M1HUD.PerfLog 1`.
 *
 * The cycle stats stay the interactive route (`stat vacuus`); this logger
 * exists because stat output goes to the screen, not the log, and headless
 * measurement runs read the log.
 */
class VACUUS_API FVaCuusPerfLog
{
public:
	/**
	 * ORDERED AS ONE FRAME RUNS, and the order is load-bearing in exactly one place:
	 * VaCuusPerfLogPrivate::GScopeNames is a positional array indexed by these values,
	 * so an enumerator inserted anywhere must have its name inserted at the same
	 * position. The static_assert next to that array catches a MISSING name; only
	 * reading the two lists together catches a mis-ORDERED one.
	 */
	enum EScope : int32
	{
		// The UI thread's own frame, in FVaCuusUIThread::RunFrame() order.
		DrainCommands = 0,
		DrainInput,
		DataApply,
		Update,
		Record,

		// The render thread.
		Replay,
		Composite,

		/**
		 * The three GAME-thread scopes, added for the Task 14 budget gate ("input + snapshot
		 * read <= 0.10 ms"). Read them as a sum per frame:
		 *
		 *   GameTick  -- UVaCuusSubsystem::Tick: poll every view's published snapshot into its
		 *                game-thread cache, then pulse the UI thread. Once per frame, and the
		 *                "snapshot read" half of the budget.
		 *   SlateTick -- SVaCuusWidget::Tick: the resize check, the focus-release edge, the
		 *                analog repeat clock and the IME surface push. Once per frame per
		 *                hosted view. Deliberately EXCLUDES FVaCuusPerfLog::TickLog(), which
		 *                is the logger's own 5-second print and would otherwise be the max
		 *                sample in every window it printed.
		 *   Input     -- one sample per input EVENT, covering the whole handler: the
		 *                screen-to-view transform, the snapshot scan that produces the FReply,
		 *                and the enqueue. The "input" half of the budget.
		 */
		GameTick,
		SlateTick,
		Input,

		Num
	};

	/** Cheap cvar check; safe on any thread. */
	static bool IsEnabled();

	/** Record one scope timing in milliseconds. No-op while disabled. Any thread. */
	static void AddSample(EScope Scope, double Milliseconds);

	/** Record one replayed buffer's draw-call count. No-op while disabled. */
	static void AddDraws(int32 NumDraws);

	/**
	 * Record one RECORDED UI frame and whether it was published. No-op while
	 * disabled. UI thread.
	 *
	 * The idle short-circuit's measurable half: without it the window line shows
	 * fewer Replay samples than frames and nothing says why. UI-thread frames are
	 * counted here rather than reusing the existing `frames=` field, which counts
	 * game-thread HUD ticks and is a different rate entirely.
	 */
	static void AddUIFrame(bool bPublished);

	/**
	 * Called once per game-thread HUD frame; handles enable/disable transitions
	 * and prints the 5-second window plus cumulative stats when the window ends.
	 */
	static void TickLog();
};

/**
 * RAII wall-clock sampler feeding FVaCuusPerfLog; pairs with the cycle stat of
 * the same scope via VACUUS_PERF_SCOPE below.
 */
struct FVaCuusPerfScopeTimer
{
	explicit FVaCuusPerfScopeTimer(FVaCuusPerfLog::EScope InScope)
		: Scope(InScope)
		, StartSeconds(FPlatformTime::Seconds())
	{
	}

	~FVaCuusPerfScopeTimer()
	{
		FVaCuusPerfLog::AddSample(Scope, (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
	}

	FVaCuusPerfLog::EScope Scope;
	double StartSeconds;
};

/** One line = cycle stat scope (`stat vacuus`) + wall-clock sample (PerfLog). */
#define VACUUS_PERF_SCOPE(ScopeName)                    \
	SCOPE_CYCLE_COUNTER(STAT_VaCuus##ScopeName);        \
	FVaCuusPerfScopeTimer UE_JOIN(VaCuusPerfTimer, __LINE__)(FVaCuusPerfLog::ScopeName)
