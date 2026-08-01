// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusStats.h"

#include "VaCuusDefines.h"

#include "HAL/IConsoleManager.h"
#include "Math/RandomStream.h"
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

/**
 * THE [all] STORE IS BOUNDED, AND EVERY SORT RUNS OUTSIDE THE LOCK (M6 sweep, bead
 * VaCuus-akj.6.39). Before this cap, Cumulative[] grew without bound and TickLog() -- the
 * game thread -- copied and SORTED every scope's whole history INSIDE State.Lock every 5
 * seconds. The cost, in terms a reader can check: the UI thread records ~13,000 frames to
 * 1 published on a static HUD (CLAUDE.md's own number, i.e. hundreds of samples/s/scope),
 * so one soak hour is ~10^5..10^6 doubles PER SCOPE; a copy+sort of that is tens of
 * milliseconds per scope, times 15 scopes, growing linearly for as long as the soak runs
 * -- all while holding the SAME lock AddSample()/AddDraws()/AddUIFrame() take at every
 * scope exit on the UI and render threads. The 5-second print therefore periodically
 * stalled the very threads being measured, in the very runs (the M6 passport soaks) whose
 * numbers the log exists to produce.
 *
 * THE BOUND: a uniform reservoir (Algorithm R) of this many samples per scope. Count, Avg
 * and Max stay EXACT (running aggregates below); p50/p99 are exact until a scope exceeds
 * the cap and a 16,384-point uniform estimate after (at p99 that is ~164 tail samples --
 * plenty for a ms-resolution log line). Worst-case work under the lock is now the fold
 * plus bounded copies (16,384 x 8 B x 15 scopes ~= 2 MB memcpy); the sorts happen on
 * private copies after the lock is released.
 */
static constexpr int32 CumulativeSampleCap = 16384;

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

	/** Samples in ms since the last window print. Naturally bounded by the window's length. */
	TArray<double> Window[FVaCuusPerfLog::Num];

	/**
	 * Since-enable samples as a CumulativeSampleCap-bounded uniform reservoir per scope
	 * (see the cap's comment). Complete below the cap; a uniform subsample above it.
	 * Order within the array carries no meaning -- Algorithm R replaces uniformly random
	 * slots, so the reservoir stays a uniform sample whatever order it is read in.
	 */
	TArray<double> Cumulative[FVaCuusPerfLog::Num];

	/** Exact since-enable aggregates; what keeps the [all] Count/Avg/Max honest past the cap. */
	uint64 CumulativeCount[FVaCuusPerfLog::Num] = {};
	double CumulativeSum[FVaCuusPerfLog::Num] = {};
	double CumulativeMax[FVaCuusPerfLog::Num] = {};

	/** Drives the reservoir's slot choice. Deterministically seeded per capture. */
	FRandomStream ReservoirRand;

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

/**
 * Nearest-rank percentiles over a sorted copy. Every caller hands it a BOUNDED array (a
 * 5-second window, or a CumulativeSampleCap reservoir) on a private copy OUTSIDE
 * State.Lock -- both halves of that sentence are akj.6.39's fix; do not regress either.
 */
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

static void LogScopeLine(const TCHAR* Bucket, int32 Scope, const FSummary& Summary, bool bPercentilesEstimated = false)
{
	// `p50~=`/`p99~=` marks a reservoir ESTIMATE (see CumulativeSampleCap): once a scope's
	// exact Count exceeds the cap, the percentiles come from the 16,384-sample uniform
	// reservoir while Count/Avg/Max stay exact -- without the marker the [all] line prints
	// estimated and exact numbers side by side as if they were the same kind of number.
	UE_LOG(LogVaCuus, Log, TEXT("PerfLog %s %s avg=%.3f p50%s=%.3f p99%s=%.3f max=%.3f ms (%d)"),
		Bucket, GScopeNames[Scope], Summary.Avg,
		bPercentilesEstimated ? TEXT("~") : TEXT(""), Summary.P50,
		bPercentilesEstimated ? TEXT("~") : TEXT(""), Summary.P99,
		Summary.Max, Summary.Count);
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

	// EVERYTHING PRINTABLE IS COPIED OUT UNDER THE LOCK AND PRINTED AFTER IT IS RELEASED
	// (bead VaCuus-akj.6.39): the sorts inside Summarize() are the expensive half of a
	// print, and holding State.Lock through them stalls every AddSample() at scope exit on
	// the UI and render threads -- the threads whose numbers this log exists to report.
	// The locked section below is the fold plus bounded copies; see CumulativeSampleCap.
	double WindowElapsed = 0.0;
	double TotalElapsed = 0.0;
	uint64 WindowFrames = 0, WindowDraws = 0, WindowReplays = 0;
	uint64 TotalFrames = 0, TotalDraws = 0, TotalReplays = 0;
	uint64 WindowUIPublished = 0, WindowUISkipped = 0, TotalUIPublished = 0, TotalUISkipped = 0;
	uint64 WindowJsGCs = 0, TotalJsGCs = 0, LastJsHeapBytes = 0;
	double WindowJsGCMaxPauseMs = 0.0;
	TArray<double> WindowCopy[Num];
	TArray<double> CumulativeCopy[Num];
	uint64 CumulativeCount[Num] = {};
	double CumulativeSum[Num] = {};
	double CumulativeMax[Num] = {};

	{
		FScopeLock ScopeLock(&State.Lock);

		if (bWantEnabled != State.bEnabled)
		{
			// Fresh capture on every enable; drop everything on disable.
			for (int32 Scope = 0; Scope < Num; ++Scope)
			{
				State.Window[Scope].Reset();
				State.Cumulative[Scope].Reset();
				State.CumulativeCount[Scope] = 0;
				State.CumulativeSum[Scope] = 0.0;
				State.CumulativeMax[Scope] = 0.0;
			}
			State.ReservoirRand.Initialize(0x5EED);
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

		WindowElapsed = NowSeconds - State.WindowStartSeconds;
		if (WindowElapsed < WindowSeconds)
		{
			return;
		}

		// Fold the window into the cumulative store: exact aggregates always, the sample
		// itself into the bounded reservoir (Algorithm R -- sample j, 0-based, takes a
		// uniformly chosen slot in [0, j] and lands only when that slot is inside the
		// reservoir, which keeps every sample's survival probability at Cap/(j+1)).
		State.TotalFrames += State.WindowFrames;
		State.TotalDraws += State.WindowDraws;
		State.TotalReplays += State.WindowReplays;
		State.TotalUIPublished += State.WindowUIPublished;
		State.TotalUISkipped += State.WindowUISkipped;
		State.TotalJsGCs += State.WindowJsGCs;
		for (int32 Scope = 0; Scope < Num; ++Scope)
		{
			TArray<double>& Reservoir = State.Cumulative[Scope];
			for (const double Sample : State.Window[Scope])
			{
				State.CumulativeSum[Scope] += Sample;
				State.CumulativeMax[Scope] = FMath::Max(State.CumulativeMax[Scope], Sample);

				const uint64 SampleIndex = State.CumulativeCount[Scope]++;
				if (Reservoir.Num() < CumulativeSampleCap)
				{
					Reservoir.Add(Sample);
				}
				else
				{
					const uint64 Slot = uint64(State.ReservoirRand.GetUnsignedInt()) % (SampleIndex + 1);
					if (Slot < uint64(CumulativeSampleCap))
					{
						Reservoir[int32(Slot)] = Sample;
					}
				}
			}
		}

		TotalElapsed = NowSeconds - State.EnableSeconds;

		// The print's inputs, copied while consistent. Every array is bounded: the window
		// by its own 5 seconds, the reservoirs by the cap.
		WindowFrames = State.WindowFrames;
		WindowDraws = State.WindowDraws;
		WindowReplays = State.WindowReplays;
		TotalFrames = State.TotalFrames;
		TotalDraws = State.TotalDraws;
		TotalReplays = State.TotalReplays;
		WindowUIPublished = State.WindowUIPublished;
		WindowUISkipped = State.WindowUISkipped;
		TotalUIPublished = State.TotalUIPublished;
		TotalUISkipped = State.TotalUISkipped;
		WindowJsGCs = State.WindowJsGCs;
		TotalJsGCs = State.TotalJsGCs;
		WindowJsGCMaxPauseMs = State.WindowJsGCMaxPauseMs;
		LastJsHeapBytes = State.LastJsHeapBytes;
		for (int32 Scope = 0; Scope < Num; ++Scope)
		{
			WindowCopy[Scope] = State.Window[Scope];
			CumulativeCopy[Scope] = State.Cumulative[Scope];
			CumulativeCount[Scope] = State.CumulativeCount[Scope];
			CumulativeSum[Scope] = State.CumulativeSum[Scope];
			CumulativeMax[Scope] = State.CumulativeMax[Scope];
		}

		// The window resets under the same hold, so no sample can fall between the copy
		// and the reset. Reset() keeps each array's capacity -- the steady state stays
		// allocation-free on the sampling side.
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

	//~ Off the lock from here on: sorts and log lines run on the private copies above.

	UE_LOG(LogVaCuus, Log,
		TEXT("PerfLog window %.1fs frames=%llu fps=%.1f draws/frame=%.1f | total %.1fs frames=%llu fps=%.1f draws/frame=%.1f"),
		WindowElapsed, WindowFrames, double(WindowFrames) / WindowElapsed,
		WindowReplays > 0 ? double(WindowDraws) / double(WindowReplays) : 0.0,
		TotalElapsed, TotalFrames, double(TotalFrames) / TotalElapsed,
		TotalReplays > 0 ? double(TotalDraws) / double(TotalReplays) : 0.0);

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
		WindowUIPublished, WindowUISkipped,
		IdlePercent(WindowUIPublished, WindowUISkipped),
		TotalUIPublished, TotalUISkipped,
		IdlePercent(TotalUIPublished, TotalUISkipped));

	// The M4 GC line (AddJsGC). `heap` is the AT-COLLECTION sample -- 0.0 KB with GCs=0
	// means "never sampled", not "empty heap"; the JsGC SCOPE line above still shows the
	// per-frame trigger-check cost, which is almost entirely declines.
	UE_LOG(LogVaCuus, Log,
		TEXT("PerfLog window JsGC runs=%llu maxpause=%.3f ms | total runs=%llu | heap=%.1f KB (at last collection)"),
		WindowJsGCs, WindowJsGCMaxPauseMs, TotalJsGCs,
		double(LastJsHeapBytes) / 1024.0);

	for (int32 Scope = 0; Scope < Num; ++Scope)
	{
		LogScopeLine(TEXT("[win]"), Scope, Summarize(WindowCopy[Scope]));
	}
	for (int32 Scope = 0; Scope < Num; ++Scope)
	{
		// The [all] line past the reservoir's cap: p50/p99 estimated from the uniform
		// reservoir (and MARKED as such -- LogScopeLine's `~`); Count, Avg and Max
		// overridden from the EXACT running aggregates, so the three numbers a budget gate
		// would key on never degrade at all. At Count == cap the reservoir still holds
		// every sample, so the percentiles are exact up to and including it.
		FSummary All = Summarize(CumulativeCopy[Scope]);
		All.Count = int32(FMath::Min<uint64>(CumulativeCount[Scope], uint64(MAX_int32)));
		All.Avg = CumulativeCount[Scope] > 0 ? CumulativeSum[Scope] / double(CumulativeCount[Scope]) : 0.0;
		All.Max = CumulativeMax[Scope];
		LogScopeLine(TEXT("[all]"), Scope, All,
			/*bPercentilesEstimated=*/CumulativeCount[Scope] > uint64(CumulativeSampleCap));
	}
}
