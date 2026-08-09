// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusJsScriptHost.h"

#include "VaCuusContentPaths.h"
#include "VaCuusJs.h"
#include "VaCuusJsEventListener.h"
#include "VaCuusJsModules.h"
#include "VaCuusJsRuntime.h"
#include "VaCuusJsScriptSource.h"
#include "VaCuusJsViewContext.h"
#include "VaCuusScriptDocument.h"

#include "HAL/IConsoleManager.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Factory.h>

static TAutoConsoleVariable<int32> CVarVaCuusJsMaxJobsPerPump(
	TEXT("vacuus.Js.MaxJobsPerPump"),
	10000,
	TEXT("Upper bound on quickjs jobs (promise reactions, queueMicrotask) executed per UI-frame pump, across all ")
	TEXT("views (default 10000). At the cap the drain stops with one Error naming the view; the remaining jobs run ")
	TEXT("next frame. This is the deterministic half of the microtask-livelock defense (spec 3.5) -- the watchdog is ")
	TEXT("the other -- so 0, which removes the bound entirely, is a state to debug with, never to ship. Read once ")
	TEXT("per script-host creation."));

#if !UE_BUILD_SHIPPING
static TAutoConsoleVariable<int32> CVarVaCuusJsOverlay(
	TEXT("vacuus.Js.Overlay"),
	1,
	TEXT("The dev error overlay (default 1): surfaced JS errors appear as a translucent list in the top-right of the ")
	TEXT("view's document, newest first, retracting entries whose rejections get handled later. 0 hides it; errors ")
	TEXT("are still logged and counted either way. Read at each overlay refresh. Non-shipping builds only."));
#endif

namespace VaCuusJsOverlayInternal
{
/** Stored entries per view; oldest drop past this. Display shows fewer (GOverlayDisplayLines) after coalescing. */
constexpr int32 GMaxStoredErrorsPerView = 32;

#if !UE_BUILD_SHIPPING
/**
 * The overlay element's reserved id -- how the refresh refinds it across frames without
 * holding a pointer. RESERVED means taken from the document's namespace: a user element
 * carrying id="vacuus-js-overlay" is what GetElementById finds first, so the refresh
 * would restyle and rewrite THAT element. Nothing detects the collision; the id is
 * hereby claimed, like `on*` attributes are by the instancer.
 */
static const char* GOverlayElementId = "vacuus-js-overlay";

/** Distinct (source, message) lines the overlay shows, newest-first (spec 3.8's "last N"). */
constexpr int32 GOverlayDisplayLines = 5;

/**
 * Text -> RML-safe text: the overlay body is built with SetInnerRML, and an
 * error message is arbitrary script-authored text -- unescaped, a message
 * containing markup would be PARSED into the overlay's subtree.
 */
FString EscapeRmlText(const FString& In)
{
	FString Out = In;
	Out.ReplaceInline(TEXT("&"), TEXT("&amp;"), ESearchCase::CaseSensitive);
	Out.ReplaceInline(TEXT("<"), TEXT("&lt;"), ESearchCase::CaseSensitive);
	Out.ReplaceInline(TEXT(">"), TEXT("&gt;"), ESearchCase::CaseSensitive);
	Out.ReplaceInline(TEXT("\""), TEXT("&quot;"), ESearchCase::CaseSensitive);
	// One line per entry is the display contract; a multi-line message (or one
	// that smuggled a CR) flattens rather than breaking the layout.
	Out.ReplaceInline(TEXT("\r"), TEXT(" "), ESearchCase::CaseSensitive);
	Out.ReplaceInline(TEXT("\n"), TEXT(" "), ESearchCase::CaseSensitive);
	return Out;
}
#endif
}	 // namespace VaCuusJsOverlayInternal

FVaCuusJsScriptHost::FVaCuusJsScriptHost()
	: FVaCuusJsScriptHost(FParams())
{
}

void FVaCuusJsRmlPlugin::OnInitialise()
{
	// The document-instancer registration point -- see the class comment for why
	// this hook and not the host constructor, and FVaCuusScriptDocumentInstancer
	// (VaCuusScriptDocument.h, in VaCuusRml for the vtable-locality argument
	// there) for why the instancer is process-immortal rather than host-owned.
	FVaCuusScriptDocumentInstancer::RegisterWithFactory();
}

void FVaCuusJsRmlPlugin::OnElementDestroy(Rml::Element* Element)
{
	Host.OnRmlElementDestroy(Element);
}

Rml::EventListener* FVaCuusJsEventListenerInstancer::InstanceEventListener(
	const Rml::String& Value, Rml::Element* /*Element*/)
{
	// The element is deliberately unused: at instancing time it may not even
	// have an owner document yet (attributes are applied during element
	// construction, mid-parse), and the listener re-derives the route at first
	// fire from the event's own current element -- see the class comment in
	// VaCuusJsScriptHost.h for why resolution must be lazy.
	return FVaCuusJsEventListener::CreateForAttribute(Host, Value);
}

FVaCuusJsScriptHost::FVaCuusJsScriptHost(const FParams& InParams)
	: Params(InParams)
	, MaxJobsPerPump(
		  InParams.MaxJobsPerPump >= 0 ? InParams.MaxJobsPerPump : CVarVaCuusJsMaxJobsPerPump.GetValueOnAnyThread())
{
	// At host creation, per the spec 2(g) cache design -- not at first context:
	// the hook must already be listening when the first element a context could
	// ever wrap comes to exist. See FVaCuusJsRmlPlugin for the registration
	// legality notes (pre-Initialise registration, the un-mutexed registry).
	RmlPlugin = MakeUnique<FVaCuusJsRmlPlugin>(*this);
	Rml::RegisterPlugin(RmlPlugin.Get());

	// The on*-attribute instancer, same timing argument: attributes are
	// instanced the moment a document parses (Element.cpp:1724-1749), which can
	// precede any script or context -- the hook must be listening from the
	// host's first breath. The Factory slot is a plain static pointer
	// (Factory.cpp:145), legal to set before Rml::Initialise and cleared by
	// Factory::Shutdown itself (Factory.cpp:274), so ours must re-register per
	// host, which ctor-to-Shutdown does.
	ListenerInstancer = MakeUnique<FVaCuusJsEventListenerInstancer>(*this);
	Rml::Factory::RegisterEventListenerInstancer(ListenerInstancer.Get());
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
	// UNLOAD FIRST, while everything is still alive. The caller (RemoveView)
	// invokes this BEFORE the document host's Shutdown(), so the view's document
	// is still open -- but that Shutdown()'s own OnDocumentClosing arrives too
	// late for JS, because the context dies right here. This is the one close
	// path where the host must dispatch the unload itself, and only when the
	// view actually has a current document (the DocumentViews probe): a
	// document-less view's onUnload is not a close of anything.
	if (FVaCuusJsViewContext* View = FindViewContext(ViewId))
	{
		for (const TPair<Rml::ElementDocument*, uint32>& Pair : DocumentViews)
		{
			if (Pair.Value == ViewId)
			{
				View->DispatchUnload();
				break;
			}
		}
	}

	// CONTEXTS DIE HERE, AND THE PUMP ITERATES LIVE CONTEXTS ONLY -- that pair of
	// rules is the entire pump-vs-retired-views story. OnViewRemoved runs from
	// DrainCommands, before the same frame's PumpFrame (VaCuusUIThread.cpp
	// RemoveView), so a removed view's callbacks are gone before the pump could
	// reach them; and on the in-band shutdown path -- which closes every DOCUMENT
	// but removes no view (VaCuusUIThread.cpp's Shutdown command handling) -- the
	// contexts stay registered and the tail frame legally pumps them: their
	// documents closed through OnDocumentClosing (unload dispatched, routing map
	// purged), leaving `document` a dead wrapper -- and dead handles answer the
	// dead-handle shape whether the tree is merely closed or already freed by
	// that frame's own Update, so no callback can reach freed memory.
	//
	// Destruction frees every callback JSValue against a still-live Rml tree (the
	// caller's ordering guarantee) -- load-bearing since Task 4: the context's
	// finalizer sweep releases wrapper ObserverPtrs and owned ElementPtrs, which
	// need the Rml pools and instancers alive.
	//
	// MOVED OUT BEFORE DYING (the ViewContexts teardown rule): a finalized
	// wrapper that still owned a detached subtree fires OnElementDestroy, whose
	// handler iterates ViewContexts -- the dying entry must already be gone, and
	// the map must not be mid-Remove. The dying context needs no probes anyway:
	// its destructor neuters its own cache first.
	TUniquePtr<FVaCuusJsViewContext> Dying;
	ViewContexts.RemoveAndCopyValue(ViewId, Dying);
	Dying.Reset();

	// The on*-routing map must not keep steering this view's documents at a
	// context that no longer exists (or, worse, at a recycled ViewId later).
	for (auto It = DocumentViews.CreateIterator(); It; ++It)
	{
		if (It.Value() == ViewId)
		{
			It.RemoveCurrent();
		}
	}

	// The error record dies with the VIEW, not with any document or context
	// (FJsViewErrors' comment): this is the one removal point short of Shutdown.
	// Also what keeps a recycled ViewId from inheriting a dead view's errors.
	ViewErrors.Remove(ViewId);
}

void FVaCuusJsScriptHost::OnDocumentReady(uint32 ViewId, Rml::ElementDocument* Document)
{
	// THE HOST-ORDERED RECYCLE-AND-RUN POINT (spec 2(f)). The caller is the
	// document host's AdoptDocument, after the old document's close (whose
	// OnDocumentClosing already dispatched unload JS into the old context) and
	// after Show() -- never Plugin::OnDocumentLoad, which fires inside
	// Context::LoadDocument (Context.cpp:299) while the OLD context is current;
	// running scripts there is v1's recorded bug (spec 12.1), restored and
	// observed by the Reload test's red half.
	TUniquePtr<FVaCuusJsViewContext>* Entry = ViewContexts.Find(ViewId);
	if (Entry == nullptr || Document == nullptr)
	{
		// Unknown view is the BindModel failure shape (a document loaded, its
		// scripts silently gone); a null document is a caller bug -- AdoptDocument
		// never adopts one.
		UE_LOG(LogVaCuusJS, Error,
			TEXT("OnDocumentReady for %s view %u dropped; its document scripts will never run"),
			Entry == nullptr ? TEXT("unknown") : TEXT("a document-less"), ViewId);
		return;
	}

	// THE RECYCLE: replace = destroy + recreate, browser-refresh semantics
	// (spec 3.4). Moved out of the entry IN PLACE, not removed -- the dying
	// wrappers fire OnElementDestroy, whose handler iterates ViewContexts, and
	// that walk must never see the map mid-mutation (the ViewContexts teardown
	// rule); a present-but-null entry is the map's ordinary "registered, not
	// materialized" state and the handler skips it. The destructor's neuter
	// walk and opaque-null land automatically (VaCuusJsViewContext.cpp).
	if (Entry->IsValid())
	{
		TUniquePtr<FVaCuusJsViewContext> Dying = MoveTemp(*Entry);
		Dying.Reset();
	}

	// The on*-routing map learns the new document -- UNCONDITIONALLY, before any
	// context question: the binding is view-level, and EnsureViewContext reads
	// it back to point `document` at the right tree whenever this view's context
	// materializes (now, for a scripted document, or later, at the first
	// ExecuteScript after an unscripted load). Old entries for the view are
	// already gone -- OnDocumentClosing purged them at close time -- but purge
	// again for the one caller that closes nothing: a first load into a fresh
	// view (a no-op walk there).
	for (auto It = DocumentViews.CreateIterator(); It; ++It)
	{
		if (It.Value() == ViewId)
		{
			It.RemoveCurrent();
		}
	}
	DocumentViews.Add(Document, ViewId);

	// The capture is only readable on documents OUR instancer built --
	// IsOurs(), never rmlui_dynamic_cast (the WrapElement comment in
	// VaCuusJsDom.cpp carries the cross-module argument; VaCuusScriptDocument.h
	// records the SIGSEGV that pinned it for documents specifically). Every
	// document loaded while a host exists is ours (the instancer's registration
	// timing), so the negative branch is a host created into a session that
	// loaded documents before it -- possible only in exotic test orderings, and
	// worth a line when it happens.
	if (!FVaCuusScriptDocumentInstancer::Get().IsOurs(Document))
	{
		UE_LOG(LogVaCuusJS, Verbose,
			TEXT("View %u's document predates the JS document instancer; no scripts to run"), ViewId);
		return;
	}

	const FVaCuusScriptDocument* JsDocument = static_cast<const FVaCuusScriptDocument*>(Document);
	if (JsDocument->GetCapturedScripts().empty())
	{
		// No <script> in <head> (a <body> script never reaches the capture --
		// RmlUi has no body script handler, XMLNodeHandlerHead.cpp:84-91,
		// :126-130 -- documented, not fought). The context stays lazy: nothing
		// to run means nothing to pay for (spec 3.4).
		return;
	}

	// Fresh context (the recycle above guarantees fresh), bound to the new
	// document by EnsureViewContext's DocumentViews read, then the captured
	// scripts in document order. Null only with the JS heap at its cap --
	// already logged, and the scripts are honestly lost with it.
	if (FVaCuusJsViewContext* View = EnsureViewContext(ViewId))
	{
		RunCapturedScripts(*View, *JsDocument);
	}
}

void FVaCuusJsScriptHost::OnDocumentClosing(uint32 ViewId)
{
	// Unload JS at Close() time (spec 3.4): the caller is the document host's
	// CloseDocument, BEFORE Document->Close(), so the callback runs against a
	// document that is still current and a tree whose deferred free has not
	// even been queued yet. No materialized context means no script ever ran
	// and nothing can have registered a callback.
	if (FVaCuusJsViewContext* View = FindViewContext(ViewId))
	{
		View->DispatchUnload();
	}

	// Map hygiene (Task 5): the closing view's document leaves the on*-routing
	// map BEFORE its address can be recycled by a replacement.
	for (auto It = DocumentViews.CreateIterator(); It; ++It)
	{
		if (It.Value() == ViewId)
		{
			It.RemoveCurrent();
		}
	}
}

void FVaCuusJsScriptHost::OnTranslationTableChanged(const FString& Tag)
{
	// EVERY LIVE CONTEXT, in view-id order (TSortedMap), because the table is process-wide and
	// every view's script is equally entitled to hear about it. Only materialized contexts are
	// dispatched to -- an entry with no context means no script has ever run there, so nothing
	// can have registered a callback (OnViewRemoved's rule, restated).
	for (TPair<uint32, TUniquePtr<FVaCuusJsViewContext>>& Pair : ViewContexts)
	{
		if (Pair.Value.IsValid())
		{
			Pair.Value->DispatchLanguageChanged(Tag);
		}
	}
}

void FVaCuusJsScriptHost::RunCapturedScripts(FVaCuusJsViewContext& View, const FVaCuusScriptDocument& Document)
{
	const Rml::Vector<FVaCuusCapturedScript>& Scripts = Document.GetCapturedScripts();
	const int32 NumScripts = static_cast<int32>(Scripts.size());
	const FString DocumentUrl(UTF8_TO_TCHAR(Document.GetSourceURL().c_str()));

	for (int32 Index = 0; Index < NumScripts; ++Index)
	{
		const FVaCuusCapturedScript& Script = Scripts[Index];

		FString Source;
		FString SourceName;
		bool bIsModule = false;
		if (Script.bIsInline)
		{
			Source = UTF8_TO_TCHAR(Script.Content.c_str());
			SourceName = FString::Printf(TEXT("%s:%d"), UTF8_TO_TCHAR(Script.SourcePath.c_str()), Script.SourceLine);
		}
		else
		{
			// THE CAPTURED src IS NOT THE RAW ATTRIBUTE: the head handler joined
			// it against the DOCUMENT's own URL -- document-relative for a
			// relative src (SystemInterface::JoinPath, SystemInterface.cpp:52-84)
			// -- and pipe-encoded every ':' (Absolutepath,
			// XMLNodeHandlerHead.cpp:14-19). Undo the encoding, then strip any
			// scheme (a memory document named "vacuus://x.rml" glues "vacuus://"
			// onto the join): the remainder resolves through the ordered DevUI
			// roots like every other VFS path. The strip is load-bearing --
			// FPaths::IsRelative treats "vacuus://x.js" as relative, so an
			// unstripped scheme would probe "<Root>/vacuus://x.js" and miss
			// (the plugin-integration note's recorded trap, section 3).
			//
			// Resolved at RUN time, read via the pak-transparent IPlatformFile
			// path (VaCuusJsScriptSource). A miss is one Error naming document
			// and path, and the LATER scripts still run -- one broken include
			// must not take the whole head down.
			FString SrcPath(UTF8_TO_TCHAR(Script.SourcePath.c_str()));
			SrcPath.ReplaceCharInline(TEXT('|'), TEXT(':'));
			if (const int32 SchemeEnd = SrcPath.Find(TEXT("://")); SchemeEnd != INDEX_NONE)
			{
				SrcPath.RightChopInline(SchemeEnd + 3);
			}
			// ReadScriptByVfsPath (M6): bundle-first, then the ordered roots -- the
			// document that referenced this script came from the same precedence, so
			// the pair cannot split across a stale loose copy and a packed one. In
			// bundle-only Shipping this is the ONLY read that can succeed at all.
			FString ServedFrom;
			if (!VaCuusJsScriptSource::ReadScriptByVfsPath(SrcPath, Source, &ServedFrom))
			{
				UE_LOG(LogVaCuusJS, Error,
					TEXT("View %u: <script src=\"%s\"> in %s did not resolve to a readable file; the script is skipped ")
					TEXT("(later scripts still run)"),
					View.GetViewId(), *SrcPath, *DocumentUrl);
				continue;
			}

			// The `.mjs` module entry (Task 7; FVaCuusCapturedScript::bIsModule
			// carries the naming-convention argument). A module is NAMED by its
			// canonical vfs slot, not by the disk path the resolve happened to
			// answer with: SrcPath is already root-relative here (un-piped,
			// scheme-stripped -- the block above), and the vfs name is what the
			// module's own relative imports resolve against and what the
			// per-context cache keys on (VaCuusJsModules.h). SourceName for the
			// classic branch stays whatever ACTUALLY answered -- the disk path or
			// the bundle pseudo-path -- because a classic script has no module
			// identity, and naming the real source is the better backtrace.
			bIsModule = Script.bIsModule;
			SourceName = bIsModule ? VaCuusJsModules::MakeModuleName(SrcPath) : ServedFrom;
		}

		// The watchdog observable: a trip's uncatchable error is consumed inside
		// Eval's sink, so the counter moving across the call is the only trace
		// left (the M2 "invariant with no observable" lesson, applied to a
		// refusal). One trip skips the REST of this document's scripts -- the
		// budget is per entry, and a head that already burned it once would burn
		// it N times, freezing the UI thread for N deadlines (spec 3.3).
		const uint64 TripsBefore = View.GetRuntime().GetNumWatchdogTrips();
		if (bIsModule)
		{
			EvalModule(View, Source, SourceName);
		}
		else
		{
			View.Eval(Source, SourceName);
		}
		if (View.GetRuntime().GetNumWatchdogTrips() > TripsBefore)
		{
			const int32 NumSkipped = NumScripts - Index - 1;
			if (NumSkipped > 0)
			{
				UE_LOG(LogVaCuusJS, Error,
					TEXT("View %u: the watchdog tripped in %s; skipping the remaining %d document script(s) of %s"),
					View.GetViewId(), *SourceName, NumSkipped, *DocumentUrl);
			}
			break;
		}
	}
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

	// LAST in the pump, before any Context::Update can run (the declaration
	// comment carries the whole re-entrancy argument): spend the dirty flags
	// this frame's errors -- and last frame's mid-Update ones -- accumulated.
	RefreshDirtyOverlays();
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

void FVaCuusJsScriptHost::EvalModule(FVaCuusJsViewContext& View, const FString& Source, const FString& ModuleName)
{
	JSContext* Ctx = View.GetContext();
	const JSValue Promise = View.EvalModuleToPromise(Source, ModuleName);
	if (!JS_IsPromise(Promise))
	{
		// Compile/resolve failed and the view already reported it (a missing
		// import lands here: the loader's ReferenceError rode the exception
		// channel out of the compile). JS_UNDEFINED needs no free, but freeing
		// is the shape that stays correct if the un-promise ever isn't.
		JS_FreeValue(Ctx, Promise);
		return;
	}

	// THE DRAIN BEFORE THE VERDICT: a TLA-free async module (await of an
	// already-settled promise) settles through ordinary jobs, so the promise
	// state is meaningless until they ran. DrainJobs is the pump's own bounded
	// drain, budgeted identically -- module-init jobs are not special, and the
	// cap plus the entry guard's watchdog remain the two livelock defenses
	// (spec 3.5) on this path too.
	bool bCapHit = false;
	DrainJobs(View, MaxJobsPerPump > 0 ? MaxJobsPerPump : -1, bCapHit);

	switch (JS_PromiseState(Ctx, Promise))
	{
		case JS_PROMISE_PENDING:
			// Still pending after a full drain = the module awaits a HOST event
			// (E1's observed signal -- the engine's has_tla is not public, and the
			// drained-and-still-pending state is the embedder-visible equivalent).
			// REFUSED, one counted Error naming the module: the pump will not keep
			// a document's readiness hostage to an unresolvable await, and the
			// module's exports never materialize (spec 3.7). bCapHit distinguishes
			// the one other way to be pending here -- a job chain the budget cut --
			// so the message never blames await for a livelock or vice versa.
			// ReportSurfacedRefusal counts AND feeds the Task 8 overlay -- this is
			// the one error with no exception and no rejection, so neither of the
			// sink's other two feeders can carry it; the detailed log stays here.
			View.GetRuntime().ReportSurfacedRefusal(View.GetContext(), *ModuleName,
				bCapHit ? TEXT("still pending after the job drain hit the cap; the module is refused")
						: TEXT("top-level await refused; the module never finishes loading"));
			if (bCapHit)
			{
				UE_LOG(LogVaCuusJS, Error,
					TEXT("View %u: module '%s' is still pending after the job drain hit vacuus.Js.MaxJobsPerPump (%d) ")
					TEXT("-- an init-time job chain is outrunning the drain; the module is refused"),
					View.GetViewId(), *ModuleName, MaxJobsPerPump);
			}
			else
			{
				UE_LOG(LogVaCuusJS, Error,
					TEXT("View %u: module '%s' is still pending after the job drain -- top-level await against a host ")
					TEXT("event is refused (spec 3.7); the module never finishes loading"),
					View.GetViewId(), *ModuleName);
			}
			break;

		case JS_PROMISE_REJECTED:
			// The throw already surfaced -- reject fired the tracker with
			// is_handled=false (quickjs.c:31571-31575 -> :54371-54375), which
			// logged the reason at Error and counted it. TWICE, in fact, for a
			// throw before the first await: the engine's sync path never handles
			// the body's async-function promise (js_execute_sync_module,
			// quickjs.c:31390-31410), so it and m->promise both reject unhandled
			// -- the ThrowRejects test pins both counts. An Error here would pile
			// a THIRD report on that; this line only pins WHICH module, at a
			// verbosity that cannot double-count anything a test asserts.
			UE_LOG(LogVaCuusJS, Verbose,
				TEXT("View %u: module '%s' rejected at init (the rejection tracker carried the reason)"),
				View.GetViewId(), *ModuleName);
			break;

		case JS_PROMISE_FULFILLED:
		default:
			break;
	}
	JS_FreeValue(Ctx, Promise);
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
	if (ViewContexts.Find(ViewId) == nullptr)
	{
		// A named refusal rather than a silent drop -- the BindModel lesson
		// (VaCuusUIThread.cpp's drain): losing a script silently is this seam's
		// quietest failure. Checked BEFORE EnsureViewContext, so a stray call
		// cannot even boot the runtime.
		UE_LOG(LogVaCuusJS, Error, TEXT("ExecuteScript('%s') for unknown view %u dropped"), *SourceName, ViewId);
		return;
	}

	if (FVaCuusJsViewContext* View = EnsureViewContext(ViewId))
	{
		View->Eval(Source, SourceName);
	}
}

void FVaCuusJsScriptHost::CallFunction(
	uint32 ViewId, const FString& FunctionPath, TArrayView<const FVaCuusJsValue> Args)
{
	if (ViewContexts.Find(ViewId) == nullptr)
	{
		// ExecuteScript's refusal verbatim, and for its reason: a dropped call to the UI is a
		// silent failure with no downstream witness.
		UE_LOG(LogVaCuusJS, Error, TEXT("CallJs('%s') for unknown view %u dropped"), *FunctionPath, ViewId);
		return;
	}

	if (FVaCuusJsViewContext* View = EnsureViewContext(ViewId))
	{
		View->CallFunction(FunctionPath, Args);
	}
}

FVaCuusJsViewContext* FVaCuusJsScriptHost::EnsureViewContext(uint32 ViewId)
{
	TUniquePtr<FVaCuusJsViewContext>* Entry = ViewContexts.Find(ViewId);
	if (Entry == nullptr)
	{
		return nullptr;
	}

	if (!Entry->IsValid())
	{
		// First need for this view: runtime first (process-wide, lazy), then the
		// context. LastPumpNowSeconds seeds the deadline base -- ExecuteScript
		// runs from DrainCommands, one phase before this frame's pump, so "the
		// previous pump's now" is at most one frame stale.
		EnsureRuntime();
		TUniquePtr<FVaCuusJsViewContext> NewContext = MakeUnique<FVaCuusJsViewContext>(
			*Runtime, ViewId, LastPumpNowSeconds, Params.bTestRelaxTimerCutoff, Params.TestTimerPassHardStop);
		if (!NewContext->IsValid())
		{
			// JS_NewContext already said why, with the view named.
			return nullptr;
		}

		// If the view already has a current document (an unscripted load bound
		// it into the routing map without materializing anything), point
		// `document` at it now -- an ExecuteScript that materializes the
		// context after such a load must see the DOM, not null (spec 3.4's
		// tested contract is "null until a document is READY", not "null until
		// a script happened to be first").
		for (const TPair<Rml::ElementDocument*, uint32>& Pair : DocumentViews)
		{
			if (Pair.Value == ViewId)
			{
				NewContext->BindDocument(Pair.Key);
				break;
			}
		}

		*Entry = MoveTemp(NewContext);
	}

	return Entry->Get();
}

void FVaCuusJsScriptHost::OnRmlElementDestroy(Rml::Element* Element)
{
	// Every materialized context gets the probe; each is one TMap lookup, and
	// views that never ran JS are null entries skipped for free. READ-ONLY over
	// the map -- the teardown rule (ViewContexts' comment) guarantees no entry
	// is mid-mutation when a destruction lands here.
	for (TPair<uint32, TUniquePtr<FVaCuusJsViewContext>>& Pair : ViewContexts)
	{
		if (Pair.Value.IsValid())
		{
			Pair.Value->OnRmlElementDestroyed(Element);
		}
	}
}

void FVaCuusJsScriptHost::BindDocumentForTest(uint32 ViewId, Rml::ElementDocument* Document)
{
	if (ViewContexts.Find(ViewId) == nullptr)
	{
		UE_LOG(LogVaCuusJS, Error, TEXT("BindDocumentForTest for unknown view %u dropped"), ViewId);
		return;
	}

	if (FVaCuusJsViewContext* View = EnsureViewContext(ViewId))
	{
		View->BindDocument(Document);

		// The bind is also what writes the on*-routing map (spec 3.9): re-binds
		// first drop the view's old entry, a null bind only drops. Task 6's
		// OnDocumentReady will do exactly this from the production path.
		for (auto It = DocumentViews.CreateIterator(); It; ++It)
		{
			if (It.Value() == ViewId)
			{
				It.RemoveCurrent();
			}
		}
		if (Document != nullptr)
		{
			DocumentViews.Add(Document, ViewId);
		}
	}
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
	// The instancer slot is cleared before anything dies: no new attribute
	// listener may be instanced against a host mid-teardown. A plain static
	// pointer write (Factory.cpp:145, :547-550), legal whatever RmlUi's state.
	if (ListenerInstancer.IsValid())
	{
		Rml::Factory::RegisterEventListenerInstancer(nullptr);
		ListenerInstancer.Reset();
	}

	// Unresolved attribute listeners hold nothing but a pointer back to THIS
	// host; neuter it away so the shells that survive into RmlUi's tree
	// teardown (their OnDetach self-deletes them there) cannot dangle into a
	// destroyed host. Resolved ones are not here -- they moved into their
	// context's set and die with it below.
	for (FVaCuusJsEventListener* Listener : UnresolvedAttributeListeners)
	{
		Listener->NeuterFromHost();
	}
	UnresolvedAttributeListeners.Empty();
	DocumentViews.Empty();

	// The plugin leaves the bus FIRST: teardown destroys elements (owned
	// detached subtrees dying with their wrappers), and those destructions need
	// no cache probes -- each dying context neuters its own cache before its
	// finalizer sweep. Unregistering is legal whatever RmlUi's state, including
	// after Rml::Shutdown already released the registry (the explicit blessing
	// in PluginRegistry.cpp:41-46) -- though in the production order this host
	// is always gone before Rml is (VaCuusUIThread.cpp Exit(), step 1a).
	if (RmlPlugin.IsValid())
	{
		Rml::UnregisterPlugin(RmlPlugin.Get());
		RmlPlugin.Reset();
	}

	// The error sink leaves before anything dies: context teardown below can
	// still surface diagnostics (a finalizer-adjacent path), and those must not
	// try to build overlay entries against a host mid-teardown. The error
	// record itself goes with it -- a host teardown is every view's end.
	if (Runtime.IsValid())
	{
		Runtime->SetErrorSink(nullptr);
	}
	ViewErrors.Empty();

	// Contexts BEFORE the runtime -- every JSValue a context holds (timer and rAF
	// callbacks, the DOM wrappers) must be freed before JS_FreeRuntime
	// (research note quickjs-ng-0151.md section 2). The runtime's destructor then
	// checks the live-byte counter back to zero, which is exactly what would
	// catch a context this sweep missed. Moved out first per the ViewContexts
	// teardown rule. Idempotent: everything is already empty on the second call.
	TSortedMap<uint32, TUniquePtr<FVaCuusJsViewContext>> Doomed = MoveTemp(ViewContexts);
	Doomed.Empty();
	Runtime.Reset();
}

void FVaCuusJsScriptHost::EnsureRuntime()
{
	if (!Runtime.IsValid())
	{
		Runtime = MakeUnique<FVaCuusJsRuntime>(Params.RuntimeParams);

		// The Task 8 error fan-out: installed the moment the runtime exists, so
		// the very first script's very first error already reaches the overlay
		// state. The host strictly outlives the runtime (it owns it), so the raw
		// back-pointer needs no detach choreography beyond Shutdown's.
		Runtime->SetErrorSink(this);
	}
}

FVaCuusJsViewContext* FVaCuusJsScriptHost::FindViewContext(uint32 ViewId) const
{
	const TUniquePtr<FVaCuusJsViewContext>* Entry = ViewContexts.Find(ViewId);
	return Entry != nullptr ? Entry->Get() : nullptr;
}

FVaCuusJsViewContext* FVaCuusJsScriptHost::FindViewContextForDocument(Rml::ElementDocument* Document) const
{
	const uint32* ViewId = DocumentViews.Find(Document);
	return ViewId != nullptr ? FindViewContext(*ViewId) : nullptr;
}

void FVaCuusJsScriptHost::OnJsError(JSContext* Ctx, EVaCuusJsErrorKind /*Kind*/, const TCHAR* Source,
	const FString& Message, const FString& Stack, void* RejectionKey)
{
	using namespace VaCuusJsOverlayInternal;

	// ATTRIBUTION (the sink contract): the context opaque IS the owning
	// FVaCuusJsViewContext -- every JSContext in this module is born in its
	// constructor (JS_SetContextOpaque there) and the destructor nulls the
	// opaque before JS_FreeContext. Null therefore means UNATTRIBUTED -- a
	// removed view's pinned job, or a context mid-teardown -- and unattributed
	// is log-only BY DESIGN: the caller already logged and counted, there is no
	// view whose overlay could honestly claim the error, and inventing one
	// (say, "the first view") would misdirect whoever reads it.
	FVaCuusJsViewContext* View = Ctx != nullptr ? static_cast<FVaCuusJsViewContext*>(JS_GetContextOpaque(Ctx)) : nullptr;
	if (View == nullptr)
	{
		return;
	}

	// The Kind is part of the seam (Task 9's router may split on it) but the
	// overlay does not: every entry displays as "source: message", and the
	// Stack -- carried for any future consumer -- is deliberately not stored
	// (FJsErrorEntry's comment; the log holds it).
	(void)Stack;

	FJsViewErrors& State = ViewErrors.FindOrAdd(View->GetViewId());
	if (State.Entries.Num() >= GMaxStoredErrorsPerView)
	{
		State.Entries.RemoveAt(0);
	}
	State.Entries.Add({Source, Message, RejectionKey});
	State.bOverlayDirty = true;
}

void FVaCuusJsScriptHost::OnJsRejectionHandled(JSContext* Ctx, void* RejectionKey)
{
	FVaCuusJsViewContext* View = Ctx != nullptr ? static_cast<FVaCuusJsViewContext*>(JS_GetContextOpaque(Ctx)) : nullptr;
	if (View == nullptr)
	{
		return;
	}
	FJsViewErrors* State = ViewErrors.Find(View->GetViewId());
	if (State == nullptr)
	{
		return;
	}

	// NEWEST match only, not RemoveAll, and the difference is honesty under
	// address reuse: the key is a freed-able promise's pointer, so a long-dead
	// entry can share the key with a NEW promise allocated at the recycled
	// address -- and only the newest holder of the key can be the promise this
	// re-fire is about (the engine retracts each promise at most once,
	// quickjs.c:55182's is_handled latch). Removing all matches would retract
	// the dead entry too, whose rejection was never handled by anyone.
	for (int32 Index = State->Entries.Num() - 1; Index >= 0; --Index)
	{
		if (State->Entries[Index].RejectionKey == RejectionKey)
		{
			State->Entries.RemoveAt(Index);
			State->bOverlayDirty = true;
			break;
		}
	}
}

void FVaCuusJsScriptHost::RefreshDirtyOverlays()
{
#if !UE_BUILD_SHIPPING
	for (TPair<uint32, FJsViewErrors>& Pair : ViewErrors)
	{
		if (Pair.Value.bOverlayDirty)
		{
			Pair.Value.bOverlayDirty = false;
			RebuildOverlay(Pair.Key, Pair.Value);
		}
	}
#endif
	// Shipping: the flags simply stay set -- one bool per erroring view, never
	// read. The entries and counters remain live (spec 3.8's counter contract).
}

#if !UE_BUILD_SHIPPING
void FVaCuusJsScriptHost::RebuildOverlay(uint32 ViewId, const FJsViewErrors& State)
{
	using namespace VaCuusJsOverlayInternal;

	// The view's CURRENT document, via the same routing map the on* dispatch
	// trusts. None -- the view closed its document, or a replace's new tree has
	// not surfaced an error yet -- means nothing to draw ON; the dirty flag is
	// already spent, and the next error (or retract) re-marks it once a
	// document exists. That is the documented replace behavior: the overlay
	// element died with the old tree, the HOST-owned entries did not, and the
	// rebuild is lazy on the next arrival.
	Rml::ElementDocument* Document = nullptr;
	for (const TPair<Rml::ElementDocument*, uint32>& Pair : DocumentViews)
	{
		if (Pair.Value == ViewId)
		{
			Document = Pair.Key;
			break;
		}
	}
	if (Document == nullptr)
	{
		return;
	}

	// Refound by reserved id every refresh, never held as a pointer: the
	// element belongs to a tree whose replace/close lifecycle this host does
	// not control, and the id lookup (Element::GetElementById, a tree walk)
	// runs only on error arrival/retract -- rare by definition.
	Rml::Element* Overlay = Document->GetElementById(GOverlayElementId);

	if (CVarVaCuusJsOverlay.GetValueOnAnyThread() == 0 || State.Entries.IsEmpty())
	{
		// Off by cvar, or the last entry retracted: the overlay must VANISH, not
		// linger empty (spec 3.8 -- an empty error box is itself a lie). The
		// returned ElementPtr is discarded, destroying the subtree at statement
		// end (Element.h:522-523); its OnElementDestroy sweep erases any cache
		// entries a curious script created by querying the overlay.
		if (Overlay != nullptr)
		{
			if (Rml::Element* Parent = Overlay->GetParentNode())
			{
				Parent->RemoveChild(Overlay);
			}
		}
		return;
	}

	if (Overlay == nullptr)
	{
		// HOST-owned UI, built with raw Rml on purpose: the error path must not
		// allocate JS (a facade createElement would mint a wrapper -- JS heap
		// traffic on the very path that reports the heap's failures). The
		// wrapper cache stays out of the picture unless a script itself queries
		// the overlay, and that case is ordinary: the plugin's OnElementDestroy
		// hook erases such entries when the overlay dies (spec 2(g)).
		Rml::ElementPtr Fresh = Rml::Factory::InstanceElement(Document, "div", "div", Rml::XMLAttributes());
		if (!Fresh)
		{
			return;
		}
		Fresh->SetId(GOverlayElementId);
		Overlay = Document->AppendChild(MoveTemp(Fresh));
		if (Overlay == nullptr)
		{
			return;
		}

		// Inline styles via SetProperty -- no RCSS file dependency, so the
		// overlay works in any document, styled or bare.
		Overlay->SetProperty("position", "absolute");
		Overlay->SetProperty("top", "8dp");
		Overlay->SetProperty("right", "8dp");
		Overlay->SetProperty("max-width", "60%");
		Overlay->SetProperty("display", "block");
		Overlay->SetProperty("background-color", "#000000C0");
		Overlay->SetProperty("color", "#FF9E9E");
		Overlay->SetProperty("font-size", "12dp");
		Overlay->SetProperty("padding", "6dp");
		Overlay->SetProperty("z-index", "1000");

		// A dev surface must never eat the game's input: without this the
		// overlay would win hit tests over whatever it happens to cover.
		Overlay->SetProperty("pointer-events", "none");

		// Font: inherit the document's family when it has one (production
		// documents style themselves); otherwise name the engine's own default
		// face (VaCuusEngine.cpp loads fonts/LatoLatin-Regular.ttf at boot) --
		// a text element with NO resolvable family is a per-layout RmlUi
		// warning (LogMissingFontFace, ElementText.cpp:81-99), which in a bare
		// document the overlay itself would otherwise cause.
		if (Document->GetProperty<Rml::String>("font-family").empty())
		{
			Overlay->SetProperty("font-family", "LatoLatin");
		}
	}

	// THE COALESCING RULE (Task 8, from the Task 7 handoff): identical
	// (source, message) pairs collapse into one line with a multiplicity
	// suffix, so the engine's sync-module double-fire -- one throw, two
	// unhandled rejections with the SAME stringified reason (the body promise
	// and m->promise both reject unhandled, js_execute_sync_module reads state
	// without attaching handlers, quickjs.c:31390-31410) -- reads as one error
	// that happened, not two different ones. Newest-first; at most
	// GOverlayDisplayLines distinct lines, with duplicates of a SHOWN line
	// still counted into its suffix and distinct lines past the cap dropped.
	struct FDisplayLine
	{
		FString Text;
		int32 Count = 0;
	};
	TArray<FDisplayLine> Lines;
	TMap<FString, int32> KeyToLine;
	for (int32 Index = State.Entries.Num() - 1; Index >= 0; --Index)
	{
		const FJsErrorEntry& Entry = State.Entries[Index];
		const FString Key = Entry.Source + TEXT("\x1f") + Entry.Message;
		if (const int32* Found = KeyToLine.Find(Key))
		{
			++Lines[*Found].Count;
			continue;
		}
		if (Lines.Num() >= GOverlayDisplayLines)
		{
			continue;
		}
		KeyToLine.Add(Key, Lines.Add({Entry.Source + TEXT(": ") + Entry.Message, 1}));
	}

	FString Markup;
	for (const FDisplayLine& Line : Lines)
	{
		FString Text = Line.Text;
		if (Line.Count > 1)
		{
			// U+00D7 MULTIPLICATION SIGN, escaped so the file stays ASCII.
			Text += FString::Printf(TEXT(" \u00D7%d"), Line.Count);
		}
		// display:block inline on each line: RmlUi's default display is inline
		// and a bare document has no stylesheet to say otherwise.
		Markup += TEXT("<div style=\"display: block;\">") + EscapeRmlText(Text) + TEXT("</div>");
	}
	Overlay->SetInnerRML(Rml::String(TCHAR_TO_UTF8(*Markup)));
}
#endif	  // !UE_BUILD_SHIPPING

void FVaCuusJsScriptHost::AddUnresolvedAttributeListener(FVaCuusJsEventListener* Listener)
{
	UnresolvedAttributeListeners.Add(Listener);
}

void FVaCuusJsScriptHost::RemoveUnresolvedAttributeListener(FVaCuusJsEventListener* Listener)
{
	UnresolvedAttributeListeners.Remove(Listener);
}
