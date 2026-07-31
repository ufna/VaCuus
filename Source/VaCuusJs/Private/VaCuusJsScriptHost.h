// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusJsRuntime.h"
#include "VaCuusScriptHost.h"

#include "Containers/SortedMap.h"
#include "Templates/UniquePtr.h"

#include <RmlUi/Core/EventListenerInstancer.h>
#include <RmlUi/Core/Plugin.h>

namespace Rml
{
class Element;
class ElementDocument;
}
class FVaCuusJsEventListener;
class FVaCuusJsScriptHost;
class FVaCuusJsViewContext;
class FVaCuusScriptDocument;

/**
 * The host's ear on RmlUi's process-global plugin bus (M4 Task 4) -- two hooks:
 *
 * OnElementDestroy, fired from inside ~Element (Element.cpp:99) for EVERY
 * element the process destroys. That is what keeps the identity cache honest:
 * the host probes each live view context's cache by raw pointer and erases the
 * dying element's entry (spec 2(g)); misses cost one TMap lookup.
 *
 * OnInitialise (M4 Task 6), the document-instancer registration point:
 * Factory's instancer map exists only between Rml::Initialise and
 * Rml::Shutdown, so the constructor cannot register unconditionally -- the
 * library-level tests build a host with RmlUi down. This hook is exact on both
 * orderings (FVaCuusScriptDocumentInstancer's class comment): immediate from
 * RegisterPlugin when RmlUi is already up -- the production order -- and from
 * NotifyInitialise, after Factory::Initialise installed the default, when a
 * registered host sees a later boot. EVT_BASIC is claimed for this hook;
 * NotifyInitialise walks the basic list only (PluginRegistry.cpp:63-69).
 * The instancer and the document class themselves live in VaCuusRml, NOT here,
 * and that location is enforced by an observed crash -- the cross-module
 * custom-RTTI argument in VaCuusScriptDocument.h.
 *
 * NOTE the spec-2(f) NON-hook: OnDocumentLoad is deliberately absent. It fires
 * inside Context::LoadDocument (Context.cpp:299) while a replace's OLD context
 * is still current -- executing scripts there is v1's recorded bug (spec 12.1).
 * Script execution is host-ordered through IVaCuusScriptHost::OnDocumentReady.
 *
 * Registered in the host's constructor, unregistered in Shutdown() -- both on
 * the thread that owns the host, and registration is legal before
 * Rml::Initialise (RegisterPlugin forwards OnInitialise only when RmlUi is
 * already up, Core.cpp:353-359). The registry itself is un-mutexed; safe here
 * because a host is only ever created/destroyed while nothing else drives
 * RmlUi (production: the UI thread's own Init/Exit; tests: the exclusivity
 * contract every VaCuusJs test already runs under).
 */
class FVaCuusJsRmlPlugin final : public Rml::Plugin
{
public:
	explicit FVaCuusJsRmlPlugin(FVaCuusJsScriptHost& InHost)
		: Host(InHost)
	{
	}

	virtual int GetEventClasses() override { return EVT_BASIC | EVT_ELEMENT; }
	virtual void OnInitialise() override;
	virtual void OnElementDestroy(Rml::Element* Element) override;

private:
	FVaCuusJsScriptHost& Host;
};

/**
 * The on*-attribute hook (M4 Task 5, spec 3.9): RmlUi's Factory holds exactly
 * one process-global EventListenerInstancer (Factory.cpp:145), consulted every
 * time an element sees an `on*`/`on*capture` attribute (OnAttributeChange,
 * Element.cpp:1724-1749, through Factory::InstanceEventListener,
 * Factory.cpp:552-559). Registered by the host at creation
 * (RegisterEventListenerInstancer, Factory.cpp:547-550), cleared in Shutdown().
 *
 * The instancer cannot know which VIEW an attribute belongs to -- for parsed
 * markup it fires mid-document-load, before any bind -- so it returns a LAZY
 * listener that resolves element -> owner document -> the host's document->view
 * map at first fire (FVaCuusJsEventListener's class comment has the full
 * protocol). The RmlUi lifetime contract is the listener's to honor: "kept
 * alive until the call to OnDetach, and then cleaned up by the user"
 * (EventListenerInstancer.h:25-27) -- the self-deleting OnDetach is exactly
 * that cleanup.
 */
class FVaCuusJsEventListenerInstancer final : public Rml::EventListenerInstancer
{
public:
	explicit FVaCuusJsEventListenerInstancer(FVaCuusJsScriptHost& InHost)
		: Host(InHost)
	{
	}

	virtual Rml::EventListener* InstanceEventListener(const Rml::String& Value, Rml::Element* Element) override;

private:
	FVaCuusJsScriptHost& Host;
};

/**
 * The IVaCuusScriptHost implementation (M4): the UI thread's factory-built
 * gateway to quickjs. Created in FVaCuusUIThread::Init() via the factory
 * FVaCuusJsModule::StartupModule registers, destroyed at the top of Exit() --
 * see the interface for the full timing contract.
 *
 * SHAPE (Tasks 3-6): the host owns the process's FVaCuusJsRuntime (lazy, Task
 * 2) plus one FVaCuusJsViewContext per view that has run a script (lazy again:
 * OnViewAdded only REGISTERS the id; the context materializes at the first
 * script). PumpFrame is live -- rAF, timers, the bounded job drain, in spec
 * 3.5's order. Since Task 6 the document seam is wired end to end: the
 * document hosts call OnDocumentClosing at Close() time and OnDocumentReady
 * after old-close and Show() (through FVaCuusUIThread::GetActiveScriptHost),
 * and the command drain routes the ExecuteScript kind into ExecuteScript here.
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

	/**
	 * The plugin's callback: probes every materialized view context's wrapper
	 * cache for the dying element (spec 2(g)). Runs on the thread that drives
	 * RmlUi -- the same thread that owns this host, possibly below a facade
	 * thunk (innerRML, remove) or a wrapper finalizer that triggered the
	 * destruction; it only reads the context map, so both are safe.
	 */
	void OnRmlElementDestroy(Rml::Element* Element);

	/**
	 * TEST-ONLY (M4 Task 4): points ViewId's `document` global at a real
	 * Rml::ElementDocument, materializing the context if needed -- the facade
	 * tests' stand-in for the production wiring, which is OnDocumentReady with
	 * the spec 2(f) recycle ordering this entry does NOT perform (the Task 4/5
	 * suites depend on their globals surviving a bind). Module-private header,
	 * so nothing ships past the seam.
	 */
	void BindDocumentForTest(uint32 ViewId, Rml::ElementDocument* Document);

	//~ Test observability. This header is module-private, so these leak nothing
	//~ past the seam; the counters the pump tests assert on live on the runtime.
	FVaCuusJsRuntime* GetRuntime() const { return Runtime.Get(); }
	FVaCuusJsViewContext* FindViewContext(uint32 ViewId) const;

	/**
	 * THE on*-ROUTING LOOKUP (M4 Task 5): document -> bound view's context, or
	 * null. Backed by a TMap<Rml::ElementDocument*, ViewId> written wherever a
	 * document becomes a view's current one -- OnDocumentReady in production
	 * (which needs this exact map again for its own routing), and
	 * BindDocumentForTest. Purge points: rebind (both writers drop the view's
	 * old entries first), OnDocumentClosing (wired from the document hosts'
	 * CloseDocument since Task 6), OnViewRemoved, and Shutdown. The by-value
	 * probe rule stands regardless: keys are raw pointers, never dereferenced,
	 * so even a missed purge would mis-route rather than crash.
	 *
	 * Null is also the answer for a mapped view whose context never
	 * materialized: the map carries the VIEW binding, the context stays lazy
	 * (spec 3.4), so an on* attribute in a document with no <script> and no
	 * ExecuteScript resolves to nothing and stays inert -- the Task 5 warning
	 * path, unchanged.
	 */
	FVaCuusJsViewContext* FindViewContextForDocument(Rml::ElementDocument* Document) const;

	//~ The unresolved on*-attribute listeners' bookkeeping (the instancer and
	//~ FVaCuusJsEventListener maintain it; Shutdown() neuters the leftovers so
	//~ a never-fired onclick shell cannot outlive the host it would resolve
	//~ through).
	void AddUnresolvedAttributeListener(FVaCuusJsEventListener* Listener);
	void RemoveUnresolvedAttributeListener(FVaCuusJsEventListener* Listener);

private:
	/** Creates the runtime on first need (spec 2(e)); no-op once it exists. */
	void EnsureRuntime();

	/**
	 * The context-materialization ExecuteScript, OnDocumentReady and
	 * BindDocumentForTest share: runtime first (process-wide, lazy), then the
	 * view's context -- and, when DocumentViews says the view already has a
	 * current document, the `document` bind, so an ExecuteScript that
	 * materializes AFTER an unscripted load still sees the DOM (spec 3.4's
	 * lazy-context rule would otherwise leave `document` null on exactly that
	 * path). Null when the view is unknown or JS_NewContext failed (which
	 * already logged why).
	 */
	FVaCuusJsViewContext* EnsureViewContext(uint32 ViewId);

	/**
	 * OnDocumentReady's tail: runs Document's captured scripts, in document
	 * order, each under its own guarded Eval. External scripts resolve their src
	 * through the DevUI roots at run time (VaCuusContentPaths) and read through
	 * the pak-transparent IPlatformFile path (VaCuusJsScriptSource -- Task 7's
	 * module loader reuses it); a miss is ONE Error naming document and path,
	 * and later scripts still run. A watchdog trip inside one script skips
	 * every script after it with one Error naming how many (spec 3.3) --
	 * detected by the runtime's trip counter moving across the Eval, the only
	 * observable a consumed uncatchable leaves behind.
	 */
	void RunCapturedScripts(FVaCuusJsViewContext& View, const FVaCuusScriptDocument& Document);

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
	 *
	 * TEARDOWN RULE: a context is always MOVED OUT of this map before it is
	 * destroyed (OnViewRemoved, Shutdown) -- its dying wrappers can fire
	 * OnElementDestroy, whose handler iterates this very map, and that
	 * iteration must never walk a container mid-mutation.
	 */
	TSortedMap<uint32, TUniquePtr<FVaCuusJsViewContext>> ViewContexts;

	/** The OnElementDestroy ear (see FVaCuusJsRmlPlugin); registered ctor-to-Shutdown. */
	TUniquePtr<FVaCuusJsRmlPlugin> RmlPlugin;

	/** The on*-attribute hook (see FVaCuusJsEventListenerInstancer); registered ctor-to-Shutdown. */
	TUniquePtr<FVaCuusJsEventListenerInstancer> ListenerInstancer;

	/** See FindViewContextForDocument. */
	TMap<Rml::ElementDocument*, uint32> DocumentViews;

	/**
	 * Attribute listeners instanced but not yet resolved to a view (never
	 * fired, or fired on an unbound document). They hold NO JS state -- only a
	 * pointer back to this host -- so view/context teardown ignores them; the
	 * two exits are resolution (moves them into a context's set) and OnDetach
	 * (self-delete), with Shutdown() neutering whatever remains.
	 */
	TSet<FVaCuusJsEventListener*> UnresolvedAttributeListeners;
};
