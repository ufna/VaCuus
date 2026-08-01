// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusStats.h"

#include "VaCuusDefines.h"

#include "HAL/IConsoleManager.h"
#include "Misc/ScopeLock.h"

DEFINE_STAT(STAT_VaCuusDrainCommands);
DEFINE_STAT(STAT_VaCuusDrainInput);
DEFINE_STAT(STAT_VaCuusDataApply);
DEFINE_STAT(STAT_VaCuusJsPump);
DEFINE_STAT(STAT_VaCuusUpdate);
DEFINE_STAT(STAT_VaCuusRecord);
DEFINE_STAT(STAT_VaCuusJsGC);
DEFINE_STAT(STAT_VaCuusReplay);
DEFINE_STAT(STAT_VaCuusGlass);
DEFINE_STAT(STAT_VaCuusComposite);
DEFINE_STAT(STAT_VaCuusWorldCopy);
DEFINE_STAT(STAT_VaCuusGameTick);
DEFINE_STAT(STAT_VaCuusSlateTick);
DEFINE_STAT(STAT_VaCuusInput);
DEFINE_STAT(STAT_VaCuusModelSample);
DEFINE_STAT(STAT_VaCuusDrawCalls);
DEFINE_STAT(STAT_VaCuusCommands);

static TAutoConsoleVariable<int32> CVarVaCuusPerfLog(
	TEXT("vacuus.M1HUD.PerfLog"),
	0,
	TEXT("1 = print every VaCuus scope's timings (avg/p50/p99/max, window + cumulative) to the log every 5 seconds: the UI thread's DrainCommands/DrainInput/DataApply/JsPump/Update/Record/JsGC, the render thread's Replay/Glass/Composite/WorldCopy, and the game thread's GameTick/SlateTick/Input/ModelSample."));

namespace VaCuusPerfLogPrivate
{
static constexpr double WindowSeconds = 5.0;

// POSITIONAL: indexed by EScope, so this list must stay in the enum's order, not just at
// its length. Padded to a common width so a window's ten lines read as a column.
static const TCHAR* GScopeNames[FVaCuusPerfLog::Num] = {
	TEXT("DrainCommands (UI)"),
	TEXT("DrainInput    (UI)"),
	TEXT("DataApply     (UI)"),
	TEXT("JsPump        (UI)"),
	TEXT("Update        (UI)"),
	TEXT("Record        (UI)"),
	TEXT("JsGC          (UI)"),
	TEXT("Replay        (RT)"),
	TEXT("Glass         (RT)"),
	TEXT("Composite     (RT)"),
	TEXT("WorldCopy     (RT)"),
	TEXT("GameTick      (GT)"),
	TEXT("SlateTick     (GT)"),
	TEXT("Input         (GT)"),
	TEXT("ModelSample   (GT)"),
};
static_assert(UE_ARRAY_COUNT(GScopeNames) == FVaCuusPerfLog::Num,
	"Every EScope needs a name here, or the log prints past the end of the array");

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

	/** JS collections actually run (the JsGC scope samples every declined check too). */
	uint64 WindowJsGCs = 0;
	uint64 TotalJsGCs = 0;

	/** Worst per-collection pause in the window; the scope's p99 dilutes it with declines. */
	double WindowJsGCMaxPauseMs = 0.0;

	/** JSMemoryUsage.malloc_size at the most recent collection; 0 until one has run. */
	uint64 LastJsHeapBytes = 0;

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

/**
 * The ALWAYS-ON last-sample store behind vacuus.stats() (M4 Task 9, spec 3.11), outside
 * FState and its lock on purpose: the RAII timers call AddSample whether or not the
 * PerfLog cvar is on, so a plain per-slot store here is one double write on a path that
 * already did a virtual call and two clock reads -- while routing it through the locked
 * state would put a mutex on every scope exit in every configuration.
 *
 * THREADING, stated because it is the whole safety argument: each slot has exactly ONE
 * writing thread (a scope runs where its phase runs), and vacuus.stats() reads only the
 * UI-thread-written slots (Update, Record) plus the JsPump interval -- from the UI
 * thread, under JS execution. Same-thread read-after-write, no ordering needed; the
 * game/render-thread slots are written here for symmetry and read by nobody yet.
 */
static double GLastSampleMs[FVaCuusPerfLog::Num] = {};

/** Seconds between the last two JsPump samples -- the UI frame interval as the pump sees it, and vacuus.stats()'s fps source. */
static double GLastJsPumpIntervalSeconds = 0.0;
static double GLastJsPumpSampleSeconds = 0.0;
} // namespace VaCuusPerfLogPrivate

bool FVaCuusPerfLog::IsEnabled()
{
	return CVarVaCuusPerfLog.GetValueOnAnyThread() != 0;
}

void FVaCuusPerfLog::AddSample(EScope Scope, double Milliseconds)
{
	using namespace VaCuusPerfLogPrivate;

	// Before the enabled gate: the last-sample store serves vacuus.stats() in every
	// configuration (its comment carries the threading argument).
	GLastSampleMs[Scope] = Milliseconds;
	if (Scope == JsPump)
	{
		const double NowSeconds = FPlatformTime::Seconds();
		if (GLastJsPumpSampleSeconds > 0.0)
		{
			GLastJsPumpIntervalSeconds = NowSeconds - GLastJsPumpSampleSeconds;
		}
		GLastJsPumpSampleSeconds = NowSeconds;
	}

	if (!IsEnabled())
	{
		return;
	}

	FState& State = GetState();
	FScopeLock ScopeLock(&State.Lock);
	if (State.bEnabled)
	{
		State.Window[Scope].Add(Milliseconds);
	}
}

double FVaCuusPerfLog::GetLastSampleMs(EScope Scope)
{
	return VaCuusPerfLogPrivate::GLastSampleMs[Scope];
}

double FVaCuusPerfLog::GetLastUIFrameIntervalSeconds()
{
	return VaCuusPerfLogPrivate::GLastJsPumpIntervalSeconds;
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
		if (bPublished)
		{
			++State.WindowUIPublished;
		}
		else
		{
			++State.WindowUISkipped;
		}
	}
}

void FVaCuusPerfLog::AddJsGC(double PauseMs, uint64 HeapBytes)
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
		++State.WindowJsGCs;
		State.WindowJsGCMaxPauseMs = FMath::Max(State.WindowJsGCMaxPauseMs, PauseMs);
		State.LastJsHeapBytes = HeapBytes;
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
		State.WindowJsGCs = State.TotalJsGCs = 0;
		State.WindowJsGCMaxPauseMs = 0.0;
		State.LastJsHeapBytes = 0;
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
	State.TotalJsGCs += State.WindowJsGCs;
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

	// The idle short-circuit, in one line: UI frames recorded, and how many of them the gate
	// let through.
	//
	// DO NOT READ `skipped` AS "the Replay sample count below is missing this many". That is
	// not arithmetic; it happened to hold in the static case this was measured on and will
	// mislead whoever reads the log on a busy UI. Draw_RenderThread emits ONE Replay scope per
	// PAINT that found a buffer waiting, whatever the queue depth -- the older buffers
	// surrender only their resource deltas, through a ConsumeResources call that has no scope
	// (VaCuusSlateElement.cpp:70-84, VaCuusReplayRenderer.cpp:39-47). So a CHANGING UI at a
	// ~220 Hz UI thread and a ~60 Hz paint reads published ~220/s, Replay samples ~60/s,
	// skipped 0: that gap is COALESCING. The two numbers only track each other at the other
	// extreme, an idle UI where the paint finds no buffer at all.
	const auto IdlePercent = [](uint64 Published, uint64 Skipped)
	{
		const uint64 Total = Published + Skipped;
		return Total > 0 ? 100.0 * double(Skipped) / double(Total) : 0.0;
	};
	UE_LOG(LogVaCuus, Log,
		TEXT("PerfLog window UI frames published=%llu skipped=%llu (%.1f%% idle) | total published=%llu skipped=%llu (%.1f%% idle)"),
		State.WindowUIPublished, State.WindowUISkipped,
		IdlePercent(State.WindowUIPublished, State.WindowUISkipped),
		State.TotalUIPublished, State.TotalUISkipped,
		IdlePercent(State.TotalUIPublished, State.TotalUISkipped));

	// The M4 GC line (AddJsGC). `heap` is the AT-COLLECTION sample -- 0.0 KB with GCs=0
	// means "never sampled", not "empty heap"; the JsGC SCOPE line above still shows the
	// per-frame trigger-check cost, which is almost entirely declines.
	UE_LOG(LogVaCuus, Log,
		TEXT("PerfLog window JsGC runs=%llu maxpause=%.3f ms | total runs=%llu | heap=%.1f KB (at last collection)"),
		State.WindowJsGCs, State.WindowJsGCMaxPauseMs, State.TotalJsGCs,
		double(State.LastJsHeapBytes) / 1024.0);

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
	State.WindowJsGCs = 0;
	State.WindowJsGCMaxPauseMs = 0.0;
	State.WindowStartSeconds = NowSeconds;
}
