// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusJsScriptHost.h"

#include "VaCuusJs.h"
#include "VaCuusJsRuntime.h"
#include "VaCuusJsViewContext.h"

#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> CVarVaCuusJsMaxJobsPerPump(
	TEXT("vacuus.Js.MaxJobsPerPump"),
	10000,
	TEXT("Upper bound on quickjs jobs (promise reactions, queueMicrotask) executed per UI-frame pump, across all ")
	TEXT("views (default 10000). At the cap the drain stops with one Error naming the view; the remaining jobs run ")
	TEXT("next frame. This is the deterministic half of the microtask-livelock defense (spec 3.5) -- the watchdog is ")
	TEXT("the other -- so 0, which removes the bound entirely, is a state to debug with, never to ship. Read once ")
	TEXT("per script-host creation."));

FVaCuusJsScriptHost::FVaCuusJsScriptHost()
	: FVaCuusJsScriptHost(FParams())
{
}

FVaCuusJsScriptHost::FVaCuusJsScriptHost(const FParams& InParams)
	: Params(InParams)
	, MaxJobsPerPump(
		  InParams.MaxJobsPerPump >= 0 ? InParams.MaxJobsPerPump : CVarVaCuusJsMaxJobsPerPump.GetValueOnAnyThread())
{
}

FVaCuusJsScriptHost::~FVaCuusJsScriptHost()
{
	// Normally already torn down: the UI thread pairs every creation with a
	// Shutdown() call in Exit(). Destroying live state here is still correct --
	// same thread, same order -- just unplanned.
	Shutdown();
}

void FVaCuusJsScriptHost::OnViewAdded(uint32 ViewId)
{
	// Registration only: the JSContext is created at the first script (spec 3.4,
	// context on demand), so a view that never runs JS costs one map entry. The
	// caller (AddView) has already refused duplicate ids.
	ViewContexts.Add(ViewId, nullptr);
}

void FVaCuusJsScriptHost::OnViewRemoved(uint32 ViewId)
{
	// CONTEXTS DIE HERE, AND THE PUMP ITERATES LIVE CONTEXTS ONLY -- that pair of
	// rules is the entire pump-vs-retired-views story. OnViewRemoved runs from
	// DrainCommands, before the same frame's PumpFrame (VaCuusUIThread.cpp
	// RemoveView), so a removed view's callbacks are gone before the pump could
	// reach them; and on the in-band shutdown path -- which closes every DOCUMENT
	// but removes no view (VaCuusUIThread.cpp's Shutdown command handling) -- the
	// contexts stay registered and the tail frame legally pumps them: their
	// `document` is null in Task 3 anyway, and Task 6's OnDocumentClosing will
	// neuter it on that path before any callback can see a dead tree.
	//
	// Destruction frees every callback JSValue against a still-live Rml tree (the
	// caller's ordering guarantee) -- irrelevant while nothing here touches Rml,
	// load-bearing from Task 4's element wrappers on.
	ViewContexts.Remove(ViewId);
}

void FVaCuusJsScriptHost::OnDocumentReady(uint32 /*ViewId*/, Rml::ElementDocument* /*Document*/)
{
	// (documents: M4 Task 6) The host-ordered recycle-and-run point (spec 2(f)).
}

void FVaCuusJsScriptHost::OnDocumentClosing(uint32 /*ViewId*/)
{
	// (documents: M4 Task 6) Unload JS dispatch, at Close() time.
}

void FVaCuusJsScriptHost::PumpFrame(double NowSeconds)
{
	// Unconditional, BEFORE the runtime check: this is the deadline base a
	// context created between pumps inherits, and the first context of a
	// long-idle process deserves a current one.
	LastPumpNowSeconds = NowSeconds;

	if (!Runtime.IsValid())
	{
		// No script has ever run; there is nothing to pump.
		return;
	}

	// One job budget for the WHOLE pump: the queue the drain services is
	// runtime-wide, so a per-view budget would let N views multiply the cap.
	int32 JobBudget = MaxJobsPerPump > 0 ? MaxJobsPerPump : -1;
	bool bCapReported = false;

	// TSortedMap iterates ascending keys = view-id order (deterministic
	// cross-view scheduling). Only materialized contexts pump -- see
	// OnViewRemoved for why that rule is sufficient on every teardown path.
	for (TPair<uint32, TUniquePtr<FVaCuusJsViewContext>>& Pair : ViewContexts)
	{
		if (!Pair.Value.IsValid())
		{
			continue;
		}

		// Phases 1+2 (rAF, timers) in the view's context...
		Pair.Value->PumpCallbacks(NowSeconds);

		// ...then phase 3, the job drain, INSIDE the view's segment: a promise
		// resolved by this view's rAF callback runs its .then in the same pump,
		// after this view's timers (the spec 3.5 ordering, pinned by the tests).
		bool bCapHit = false;
		const int32 NumExecuted = DrainJobs(*Pair.Value, JobBudget, bCapHit);
		if (JobBudget > 0)
		{
			JobBudget -= NumExecuted;
		}
		if (bCapHit && !bCapReported)
		{
			// ONE Error per pump, naming the view whose segment exhausted the
			// budget; later segments this pump inherit a zero budget and stay
			// silent. The remaining jobs are not lost -- the queue persists and
			// next frame's drain continues it.
			bCapReported = true;
			UE_LOG(LogVaCuusJS, Error,
				TEXT("JS job drain hit vacuus.Js.MaxJobsPerPump (%d) at view %u -- a job chain is outrunning the ")
				TEXT("pump (a microtask requeuing itself?); the remaining jobs run next frame"),
				MaxJobsPerPump, Pair.Key);
		}
	}
}

int32 FVaCuusJsScriptHost::DrainJobs(FVaCuusJsViewContext& View, int32 Budget, bool& bOutCapHit)
{
	JSRuntime* Rt = Runtime->GetRuntime();
	if (!JS_IsJobPending(Rt))
	{
		return 0;
	}

	int32 NumExecuted = 0;
	{
		// ONE GUARD FOR THE WHOLE DRAIN, NOT ONE PER JOB, and the difference is
		// whether the watchdog has teeth here: a per-job guard would re-arm the
		// deadline for every microscopic job, so a self-requeuing microtask could
		// never accumulate enough time to trip it -- the drain would be bounded by
		// the cap alone. Armed once at the drain boundary, the deadline spans the
		// chain, and past it the interrupt handler keeps answering 1, killing the
		// running job at each poll (every 10000 poll-site visits,
		// JS_INTERRUPT_COUNTER_INIT, quickjs.c:479, :8221-8241); the drain ends
		// once a kill lands before a job's requeue. That makes the watchdog spec
		// 3.5's PROBABILISTIC second defense -- a kill's offset inside a job is
		// not controlled, which is exactly why the cap above is the deterministic
		// first one and ships non-zero.
		//
		// Consequence for exceptions: they are consumed INSIDE the guard, a
		// deliberate deviation from the close-then-consume contract -- with many
		// jobs under one guard there is no per-job boundary to consume at, and
		// leaving an exception pending across JS_ExecutePendingJob calls is not an
		// option. Ordinary throws lose nothing by it; a watchdog trip's
		// uncatchable error is consumed here too, so the guard's close finds no
		// pending exception and (by its own HasException check) resets nothing --
		// nothing leaks into the next entry either way.
		FVaCuusJsEntryGuard Guard(*Runtime, View.GetContext(), TEXT("<job drain>"));

		while (JS_IsJobPending(Rt))
		{
			if (Budget >= 0 && NumExecuted >= Budget)
			{
				bOutCapHit = true;
				break;
			}
			if (Params.TestJobDrainHardStop > 0 && NumExecuted >= Params.TestJobDrainHardStop)
			{
				// TEST-ONLY bound (never armed in production): what lets the red
				// half run cap-less and watchdog-less and still return -- see
				// FParams::TestJobDrainHardStop.
				break;
			}

			// Executes exactly one job; < 0 means the job threw and ITS exception is
			// pending in *pctx -- the job's own context, which with several views
			// need not be this segment's (quickjs.c:2175-2202, contract comment at
			// :2173-2174).
			JSContext* JobCtx = nullptr;
			const int Result = JS_ExecutePendingJob(Rt, &JobCtx);
			if (Result == 0)
			{
				break;
			}

			// Counted on -1 as well: a throwing job still RAN, and the budget must
			// account for its cost. One job's throw never skips the rest.
			++NumExecuted;
			Runtime->NoteJobExecuted();
			if (Result < 0)
			{
				Runtime->ReportException(JobCtx, TEXT("<js job>"));
			}
		}
	}
	return NumExecuted;
}

void FVaCuusJsScriptHost::CollectGarbage(const TCHAR* Reason)
{
	if (Runtime.IsValid())
	{
		Runtime->CollectGarbage(Reason);
	}
}

void FVaCuusJsScriptHost::ExecuteScript(uint32 ViewId, const FString& Source, const FString& SourceName)
{
	TUniquePtr<FVaCuusJsViewContext>* Entry = ViewContexts.Find(ViewId);
	if (Entry == nullptr)
	{
		// A named refusal rather than a silent drop -- the BindModel lesson
		// (VaCuusUIThread.cpp's drain): losing a script silently is this seam's
		// quietest failure. Checked BEFORE EnsureRuntime, so a stray call cannot
		// even boot the runtime.
		UE_LOG(LogVaCuusJS, Error, TEXT("ExecuteScript('%s') for unknown view %u dropped"), *SourceName, ViewId);
		return;
	}

	if (!Entry->IsValid())
	{
		// First script for this view: runtime first (process-wide, lazy), then the
		// context. LastPumpNowSeconds seeds the deadline base -- ExecuteScript runs
		// from DrainCommands, one phase before this frame's pump, so "the previous
		// pump's now" is at most one frame stale.
		EnsureRuntime();
		TUniquePtr<FVaCuusJsViewContext> NewContext = MakeUnique<FVaCuusJsViewContext>(
			*Runtime, ViewId, LastPumpNowSeconds, Params.bTestRelaxTimerCutoff, Params.TestTimerPassHardStop);
		if (!NewContext->IsValid())
		{
			// JS_NewContext already said why, with the view named.
			return;
		}
		*Entry = MoveTemp(NewContext);
	}

	(*Entry)->Eval(Source, SourceName);
}

void FVaCuusJsScriptHost::OnInlineFrameEntry()
{
	if (Runtime.IsValid())
	{
		Runtime->UpdateStackTopOnThisThread();
	}
}

void FVaCuusJsScriptHost::Shutdown()
{
	// Contexts BEFORE the runtime -- every JSValue a context holds (timer and rAF
	// callbacks, later the DOM wrappers) must be freed before JS_FreeRuntime
	// (research note quickjs-ng-0151.md section 2). The runtime's destructor then
	// checks the live-byte counter back to zero, which is exactly what would
	// catch a context this sweep missed. Idempotent: both containers empty on the
	// second call.
	ViewContexts.Empty();
	Runtime.Reset();
}

void FVaCuusJsScriptHost::EnsureRuntime()
{
	if (!Runtime.IsValid())
	{
		Runtime = MakeUnique<FVaCuusJsRuntime>(Params.RuntimeParams);
	}
}

FVaCuusJsViewContext* FVaCuusJsScriptHost::FindViewContext(uint32 ViewId) const
{
	const TUniquePtr<FVaCuusJsViewContext>* Entry = ViewContexts.Find(ViewId);
	return Entry != nullptr ? Entry->Get() : nullptr;
}
