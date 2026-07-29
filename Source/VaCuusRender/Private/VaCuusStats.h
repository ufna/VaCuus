// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Stats/Stats.h"

DECLARE_STATS_GROUP(TEXT("VaCuus"), STATGROUP_VaCuus, STATCAT_Advanced);

// (UI) == the dedicated VaCuus UI thread; these two used to be game-thread costs
// and moved off it in M2 Task 3.
DECLARE_CYCLE_STAT_EXTERN(TEXT("VaCuus Update (UI)"), STAT_VaCuusUpdate, STATGROUP_VaCuus, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("VaCuus Record (UI)"), STAT_VaCuusRecord, STATGROUP_VaCuus, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("VaCuus Replay (RT)"), STAT_VaCuusReplay, STATGROUP_VaCuus, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("VaCuus Composite (RT)"), STAT_VaCuusComposite, STATGROUP_VaCuus, );

/** Per-frame counters (cleared by the stats system every frame). */
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("VaCuus Draw Calls"), STAT_VaCuusDrawCalls, STATGROUP_VaCuus, );
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("VaCuus Commands"), STAT_VaCuusCommands, STATGROUP_VaCuus, );

/**
 * Log-based rolling perf capture for headless soaks (Task 10): wall-clock
 * samples taken around the same scopes as the cycle stats above, printed as
 * avg/p50/p99/max every 5 seconds while `vacuus.M1HUD.PerfLog 1`.
 *
 * The cycle stats stay the interactive route (`stat vacuus`); this logger
 * exists because stat output goes to the screen, not the log, and headless
 * measurement runs read the log.
 */
class FVaCuusPerfLog
{
public:
	enum EScope : int32
	{
		Update = 0,
		Record,
		Replay,
		Composite,
		Num
	};

	/** Cheap cvar check; safe on any thread. */
	static bool IsEnabled();

	/** Record one scope timing in milliseconds. No-op while disabled. Any thread. */
	static void AddSample(EScope Scope, double Milliseconds);

	/** Record one replayed buffer's draw-call count. No-op while disabled. */
	static void AddDraws(int32 NumDraws);

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
