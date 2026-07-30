// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusStats.h"

#include "VaCuusDefines.h"

#include "HAL/IConsoleManager.h"
#include "Misc/ScopeLock.h"

DEFINE_STAT(STAT_VaCuusUpdate);
DEFINE_STAT(STAT_VaCuusRecord);
DEFINE_STAT(STAT_VaCuusReplay);
DEFINE_STAT(STAT_VaCuusComposite);
DEFINE_STAT(STAT_VaCuusDrawCalls);
DEFINE_STAT(STAT_VaCuusCommands);

static TAutoConsoleVariable<int32> CVarVaCuusPerfLog(
	TEXT("vacuus.M1HUD.PerfLog"),
	0,
	TEXT("1 = print VaCuus Update/Record/Replay/Composite timings (avg/p50/p99/max, window + cumulative) to the log every 5 seconds."));

namespace VaCuusPerfLogPrivate
{
static constexpr double WindowSeconds = 5.0;

static const TCHAR* GScopeNames[FVaCuusPerfLog::Num] = {
	TEXT("Update    (UI)"),
	TEXT("Record    (UI)"),
	TEXT("Replay    (RT)"),
	TEXT("Composite (RT)"),
};

struct FState
{
	FCriticalSection Lock;

	/** Samples in ms since the last window print / since enable. */
	TArray<double> Window[FVaCuusPerfLog::Num];
	TArray<double> Cumulative[FVaCuusPerfLog::Num];

	uint64 WindowDraws = 0;
	uint64 WindowReplays = 0;
	uint64 TotalDraws = 0;
	uint64 TotalReplays = 0;

	uint64 WindowFrames = 0;
	uint64 TotalFrames = 0;

	/** UI-thread frames recorded, split by whether the idle gate let them publish. */
	uint64 WindowUIPublished = 0;
	uint64 WindowUISkipped = 0;
	uint64 TotalUIPublished = 0;
	uint64 TotalUISkipped = 0;

	double WindowStartSeconds = 0.0;
	double EnableSeconds = 0.0;
	bool bEnabled = false;
};

static FState& GetState()
{
	static FState State;
	return State;
}

struct FSummary
{
	double Avg = 0.0, P50 = 0.0, P99 = 0.0, Max = 0.0;
	int32 Count = 0;
};

/** Nearest-rank percentiles over a sorted copy; fine at soak sample counts. */
static FSummary Summarize(const TArray<double>& Samples)
{
	FSummary Out;
	Out.Count = Samples.Num();
	if (Out.Count == 0)
	{
		return Out;
	}

	TArray<double> Sorted = Samples;
	Sorted.Sort();

	double Sum = 0.0;
	for (const double Value : Sorted)
	{
		Sum += Value;
	}
	Out.Avg = Sum / double(Out.Count);
	Out.P50 = Sorted[FMath::Clamp(FMath::CeilToInt32(0.50 * Out.Count) - 1, 0, Out.Count - 1)];
	Out.P99 = Sorted[FMath::Clamp(FMath::CeilToInt32(0.99 * Out.Count) - 1, 0, Out.Count - 1)];
	Out.Max = Sorted.Last();
	return Out;
}

static void LogScopeLine(const TCHAR* Bucket, int32 Scope, const FSummary& Summary)
{
	UE_LOG(LogVaCuus, Log, TEXT("PerfLog %s %s avg=%.3f p50=%.3f p99=%.3f max=%.3f ms (%d)"),
		Bucket, GScopeNames[Scope], Summary.Avg, Summary.P50, Summary.P99, Summary.Max, Summary.Count);
}
} // namespace VaCuusPerfLogPrivate

bool FVaCuusPerfLog::IsEnabled()
{
	return CVarVaCuusPerfLog.GetValueOnAnyThread() != 0;
}

void FVaCuusPerfLog::AddSample(EScope Scope, double Milliseconds)
{
	if (!IsEnabled())
	{
		return;
	}

	using namespace VaCuusPerfLogPrivate;
	FState& State = GetState();
	FScopeLock ScopeLock(&State.Lock);
	if (State.bEnabled)
	{
		State.Window[Scope].Add(Milliseconds);
	}
}

void FVaCuusPerfLog::AddDraws(int32 NumDraws)
{
	if (!IsEnabled())
	{
		return;
	}

	using namespace VaCuusPerfLogPrivate;
	FState& State = GetState();
	FScopeLock ScopeLock(&State.Lock);
	if (State.bEnabled)
	{
		State.WindowDraws += uint64(NumDraws);
		++State.WindowReplays;
	}
}

void FVaCuusPerfLog::AddUIFrame(bool bPublished)
{
	if (!IsEnabled())
	{
		return;
	}

	using namespace VaCuusPerfLogPrivate;
	FState& State = GetState();
	FScopeLock ScopeLock(&State.Lock);
	if (State.bEnabled)
	{
		bPublished ? ++State.WindowUIPublished : ++State.WindowUISkipped;
	}
}

void FVaCuusPerfLog::TickLog()
{
	using namespace VaCuusPerfLogPrivate;

	FState& State = GetState();
	const bool bWantEnabled = IsEnabled();
	const double NowSeconds = FPlatformTime::Seconds();

	FScopeLock ScopeLock(&State.Lock);

	if (bWantEnabled != State.bEnabled)
	{
		// Fresh capture on every enable; drop everything on disable.
		for (int32 Scope = 0; Scope < Num; ++Scope)
		{
			State.Window[Scope].Reset();
			State.Cumulative[Scope].Reset();
		}
		State.WindowDraws = State.TotalDraws = 0;
		State.WindowReplays = State.TotalReplays = 0;
		State.WindowFrames = State.TotalFrames = 0;
		State.WindowUIPublished = State.TotalUIPublished = 0;
		State.WindowUISkipped = State.TotalUISkipped = 0;
		State.WindowStartSeconds = State.EnableSeconds = NowSeconds;
		State.bEnabled = bWantEnabled;
		if (bWantEnabled)
		{
			UE_LOG(LogVaCuus, Log, TEXT("PerfLog capture started (window %.0fs)"), WindowSeconds);
		}
		return;
	}

	if (!State.bEnabled)
	{
		return;
	}

	++State.WindowFrames;

	const double WindowElapsed = NowSeconds - State.WindowStartSeconds;
	if (WindowElapsed < WindowSeconds)
	{
		return;
	}

	// Fold the window into the cumulative store, then print both views.
	State.TotalFrames += State.WindowFrames;
	State.TotalDraws += State.WindowDraws;
	State.TotalReplays += State.WindowReplays;
	State.TotalUIPublished += State.WindowUIPublished;
	State.TotalUISkipped += State.WindowUISkipped;
	for (int32 Scope = 0; Scope < Num; ++Scope)
	{
		State.Cumulative[Scope].Append(State.Window[Scope]);
	}

	const double TotalElapsed = NowSeconds - State.EnableSeconds;
	UE_LOG(LogVaCuus, Log,
		TEXT("PerfLog window %.1fs frames=%llu fps=%.1f draws/frame=%.1f | total %.1fs frames=%llu fps=%.1f draws/frame=%.1f"),
		WindowElapsed, State.WindowFrames, double(State.WindowFrames) / WindowElapsed,
		State.WindowReplays > 0 ? double(State.WindowDraws) / double(State.WindowReplays) : 0.0,
		TotalElapsed, State.TotalFrames, double(State.TotalFrames) / TotalElapsed,
		State.TotalReplays > 0 ? double(State.TotalDraws) / double(State.TotalReplays) : 0.0);

	// The idle short-circuit, in one line: UI frames recorded, and how many of them
	// the gate let through. `skipped` is what the Replay sample count below is missing.
	UE_LOG(LogVaCuus, Log,
		TEXT("PerfLog window UI frames published=%llu skipped=%llu (%.1f%% idle) | total published=%llu skipped=%llu (%.1f%% idle)"),
		State.WindowUIPublished, State.WindowUISkipped,
		(State.WindowUIPublished + State.WindowUISkipped) > 0
			? 100.0 * double(State.WindowUISkipped) / double(State.WindowUIPublished + State.WindowUISkipped)
			: 0.0,
		State.TotalUIPublished, State.TotalUISkipped,
		(State.TotalUIPublished + State.TotalUISkipped) > 0
			? 100.0 * double(State.TotalUISkipped) / double(State.TotalUIPublished + State.TotalUISkipped)
			: 0.0);

	for (int32 Scope = 0; Scope < Num; ++Scope)
	{
		LogScopeLine(TEXT("[win]"), Scope, Summarize(State.Window[Scope]));
	}
	for (int32 Scope = 0; Scope < Num; ++Scope)
	{
		LogScopeLine(TEXT("[all]"), Scope, Summarize(State.Cumulative[Scope]));
	}

	for (int32 Scope = 0; Scope < Num; ++Scope)
	{
		State.Window[Scope].Reset();
	}
	State.WindowDraws = 0;
	State.WindowReplays = 0;
	State.WindowFrames = 0;
	State.WindowUIPublished = 0;
	State.WindowUISkipped = 0;
	State.WindowStartSeconds = NowSeconds;
}
