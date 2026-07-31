// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusJsRuntime.h"
#include "VaCuusScriptHost.h"

#include "Containers/SortedMap.h"
#include "Templates/UniquePtr.h"

class FVaCuusJsViewContext;

/**
 * The IVaCuusScriptHost implementation (M4): the UI thread's factory-built
 * gateway to quickjs. Created in FVaCuusUIThread::Init() via the factory
 * FVaCuusJsModule::StartupModule registers, destroyed at the top of Exit() --
 * see the interface for the full timing contract.
 *
 * TASK 3 SHAPE: the host owns the process's FVaCuusJsRuntime (lazy, Task 2)
 * plus one FVaCuusJsViewContext per view that has run a script (lazy again:
 * OnViewAdded only REGISTERS the id; the context materializes at the first
 * ExecuteScript). PumpFrame is live -- rAF, timers, the bounded job drain, in
 * spec 3.5's order. Documents and the ExecuteScript command plumbing are M4
 * Task 6; until then ExecuteScript's callers are tests (and Task 6 will call
 * exactly this method from the command drain).
 */
class FVaCuusJsScriptHost final : public IVaCuusScriptHost
{
public:
	/**
	 * Construction knobs, FVaCuusJsRuntime::FParams' pattern one level up:
	 * defaults resolve to cvars, read once at construction; tests pass direct
	 * values instead of mutating global cvar state.
	 */
	struct FParams
	{
		/** Handed to the runtime when it is lazily created. */
		FVaCuusJsRuntime::FParams RuntimeParams;

		/**
		 * Jobs executed per pump, ACROSS all views (the job queue is
		 * runtime-wide); -1 resolves to vacuus.Js.MaxJobsPerPump. 0 removes the
		 * bound entirely -- THE DELIBERATELY-BROKEN STATE the job-livelock test's
		 * red half runs: with it, a self-requeuing microtask drains forever.
		 */
		int32 MaxJobsPerPump = -1;

		/**
		 * TEST ONLY -- the timer-livelock test's restore-the-bug half: true
		 * relaxes the frame-start cutoff from strictly-before to at-or-before
		 * (the demo's due-test, hud-demo VacuusJs.cpp:547), under which a 0 ms
		 * self-rearming timer keeps one pass alive forever. Production never
		 * sets this.
		 */
		bool bTestRelaxTimerCutoff = false;

		/**
		 * TEST ONLY -- what makes the two red states OBSERVABLE without hanging
		 * the suite (the same job Task 2's escape interrupt handler did for the
		 * watchdog test): > 0 hard-stops a single timer pass / job drain after
		 * that many fires / jobs. The red tests assert the count was reached
		 * exactly and work was STILL pending -- non-termination observed,
		 * bounded, no hang. Production never sets these.
		 */
		int32 TestTimerPassHardStop = 0;
		int32 TestJobDrainHardStop = 0;
	};

	/** Cvar-configured; the production path (the factory calls this). */
	FVaCuusJsScriptHost();
	explicit FVaCuusJsScriptHost(const FParams& InParams);
	virtual ~FVaCuusJsScriptHost() override;

	//~ Begin IVaCuusScriptHost
	virtual void OnViewAdded(uint32 ViewId) override;
	virtual void OnViewRemoved(uint32 ViewId) override;
	virtual void OnDocumentReady(uint32 ViewId, Rml::ElementDocument* Document) override;
	virtual void OnDocumentClosing(uint32 ViewId) override;
	virtual void PumpFrame(double NowSeconds) override;
	virtual void CollectGarbage(const TCHAR* Reason) override;
	virtual void ExecuteScript(uint32 ViewId, const FString& Source, const FString& SourceName) override;
	virtual void OnInlineFrameEntry() override;
	virtual void Shutdown() override;
	//~ End IVaCuusScriptHost

	//~ Test observability. This header is module-private, so these leak nothing
	//~ past the seam; the counters the pump tests assert on live on the runtime.
	FVaCuusJsRuntime* GetRuntime() const { return Runtime.Get(); }
	FVaCuusJsViewContext* FindViewContext(uint32 ViewId) const;

private:
	/** Creates the runtime on first need (spec 2(e)); no-op once it exists. */
	void EnsureRuntime();

	/**
	 * Phase 3 of one view's pump segment: the runtime-wide job drain, bounded by
	 * Budget (-1 = unbounded). Returns jobs executed; bOutCapHit reports a stop
	 * with work still pending. See the implementation for why the WHOLE drain
	 * runs under ONE entry guard.
	 */
	int32 DrainJobs(FVaCuusJsViewContext& View, int32 Budget, bool& bOutCapHit);

	const FParams Params;

	/** Params.MaxJobsPerPump with -1 resolved to the cvar, once, at construction. */
	const int32 MaxJobsPerPump;

	/**
	 * The most recent pump's timestamp -- the deadline base handed to a context
	 * created between pumps (FVaCuusJsViewContext::PumpNowSeconds documents the
	 * semantics). Updated every PumpFrame, runtime or not, so the first script
	 * of a long-idle process still gets a current base.
	 */
	double LastPumpNowSeconds = 0.0;

	/**
	 * The process's one runtime (spec 2(e)), lazy: null until the first script
	 * needs it, so a JS-enabled build that never runs a script pays one virtual
	 * call per frame phase and allocates nothing.
	 */
	TUniquePtr<FVaCuusJsRuntime> Runtime;

	/**
	 * Every REGISTERED view, keyed by id; the value is null until the view's
	 * first script materializes its context. TSortedMap so the pump iterates in
	 * view-id order -- deterministic cross-view scheduling for free.
	 */
	TSortedMap<uint32, TUniquePtr<FVaCuusJsViewContext>> ViewContexts;
};
