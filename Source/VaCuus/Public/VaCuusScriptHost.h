// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusJsValue.h"

#include "Templates/Function.h"
#include "Templates/UniquePtr.h"

namespace Rml
{
class ElementDocument;
}

/**
 * The UI thread's view of "the thing that runs this process's JavaScript" (M4).
 * At most one host instance per UI thread lifetime; the thread creates it in
 * Init() from the factory registered with FVaCuusModule::SetScriptHostFactory
 * and destroys it in Exit(), before the document hosts go down.
 *
 * WHY AN INTERFACE: the concrete host owns the quickjs runtime, and quickjs is
 * deliberately unreachable from everywhere else -- its headers are on VaCuusJs's
 * PRIVATE include path and its symbols are stripped from the module's dynamic
 * symbol table (vendored patch #1, Source/ThirdParty/quickjs-ng/VENDORED_TAG.txt).
 * So VaCuus declares the contract and VaCuusJs implements it
 * (FVaCuusJsScriptHost), exactly the IVaCuusDocumentHost seam one module over.
 * The seam is also the JS-off configuration: no factory registered (or
 * vacuus.Js.Enable 0 at thread boot) means no host, and every call site below
 * skips -- the M3 frame loop byte-identical, for free.
 *
 * THREAD AFFINITY: every method runs ON THE VaCuus UI THREAD and nowhere else --
 * the thread creates the host in Init() (after RmlUi is up and after the thread
 * id is published, so FVaCuusUIThread::IsInUIThread() already answers true) and
 * calls it only from its own frame loop and teardown. All JS -- creation, evals,
 * callbacks, finalizers, GC -- happens inside these calls (spec 4). Note the
 * caveat the affinity check inherits from inline mode: there the game thread IS
 * the UI thread for the duration of each frame, the asserts are vacuous, and
 * OnInlineFrameEntry() is the load-bearing part instead (spec 2(c)).
 */
class IVaCuusScriptHost
{
public:
	virtual ~IVaCuusScriptHost() = default;

	/** A view was registered on the UI thread. Per-view JS state is created lazily, not here. */
	virtual void OnViewAdded(uint32 ViewId) = 0;

	/**
	 * A view is being retired. Called BEFORE the document host's Shutdown(), i.e.
	 * while the view's Rml context and element tree still exist: the host frees
	 * this view's JS state against a live tree, so element wrappers and listeners
	 * can be released in an order RmlUi's own deferred detach then completes
	 * (spec 2(g)). NOT called for views that die in thread Exit() -- there
	 * Shutdown() below covers every view still registered, in one sweep.
	 */
	virtual void OnViewRemoved(uint32 ViewId) = 0;

	/**
	 * A document replace finished: the old document is closed, the new one is
	 * shown, and Document is current. THE host-ordered script-execution point
	 * (spec 2(f)): the caller is the document host's AdoptDocument, after
	 * old-close and Show() -- never Plugin::OnDocumentLoad, which fires inside
	 * Context::LoadDocument while the OLD context is still the current one.
	 * Typed as the Rml pointer rather than an opaque void* for the same reason
	 * IVaCuusDocumentHost::GetContext() is: both modules see the same forward
	 * declaration and the compiler checks the handoff. The caller reaches this
	 * through FVaCuusUIThread::GetActiveScriptHost() (M4 Task 6).
	 */
	virtual void OnDocumentReady(uint32 ViewId, Rml::ElementDocument* Document) = 0;

	/**
	 * The view's current document is about to close (document replace, explicit
	 * close, either shutdown path). Dispatch point for unload JS, called by the
	 * document host's CloseDocument BEFORE Document->Close() -- the document is
	 * still current, and the tree is freed later still, by a Context::Update().
	 * The one close path that does NOT arrive here in time is view removal:
	 * there OnViewRemoved runs first and owns the unload, because the context
	 * dies inside it (M4 Task 6).
	 */
	virtual void OnDocumentClosing(uint32 ViewId) = 0;

	/**
	 * One pump per UI frame, between the data apply and the record loop: rAF
	 * callbacks, due timers, then the bounded microtask drain (spec 3.5, built in
	 * M4 Task 3). NowSeconds is Rml::GetSystemInterface()->GetElapsedTime() --
	 * the clock RmlUi advances its own animations on (Clock.cpp:7-14,
	 * Element.cpp:2838-2849) -- sampled once by the caller so scripts and
	 * animations never disagree about what time this frame is.
	 */
	virtual void PumpFrame(double NowSeconds) = 0;

	/**
	 * The controlled GC point, called at the end of every UI frame after the
	 * record loop (spec 3.6). The host decides whether to actually collect
	 * (allocation growth since the last collection, or an OOM fallback); Reason
	 * is for the log line only.
	 */
	virtual void CollectGarbage(const TCHAR* Reason) = 0;

	/**
	 * Evaluates Source against ViewId's context, SourceName naming it in errors
	 * and backtraces. Called by DrainCommands for the ExecuteScript command kind
	 * (M4 Task 6); an unknown view is the host's own Error-level refusal.
	 */
	virtual void ExecuteScript(uint32 ViewId, const FString& Source, const FString& SourceName) = 0;

	/**
	 * Calls the function at FunctionPath (dotted, from globalThis) on ViewId's context with
	 * Args. Called by DrainCommands for the CallScriptFunction command kind (VaCuus-asv); an
	 * unknown view is the host's own Error-level refusal, exactly like ExecuteScript's.
	 *
	 * SEPARATE FROM ExecuteScript RATHER THAN SUGAR OVER IT, and that is the point: sugar
	 * would still have to build `f(<args>)` as text, which is the injection this exists to
	 * remove. Nothing on this path stringifies an argument -- see
	 * FVaCuusJsViewContext::CallFunction.
	 */
	virtual void CallFunction(
		uint32 ViewId, const FString& FunctionPath, TArrayView<const FVaCuusJsValue> Args) = 0;

	/**
	 * Called at the top of every RunFrameInline() -- inline mode only, where UI
	 * frames run deep in a game-thread callstack. The host re-anchors quickjs's
	 * native-stack overflow check here (JS_UpdateStackTop): the anchor is
	 * captured on the creating thread and consulted by every stack check, so a
	 * frame running on a different stack -- or at a different depth of the same
	 * one -- must refresh it before ANY JS runs (spec 2(c)).
	 *
	 * ITS OWN HOOK RATHER THAN PART OF PumpFrame(), deliberately: document
	 * scripts and ExecuteScript run from DrainCommands, which the frame runs
	 * BEFORE the pump -- an anchor refreshed in PumpFrame() would arrive after
	 * the frame's largest JS entry points already executed (the exact scope
	 * mistake spec 12.4 records the watchdog making in v1).
	 */
	virtual void OnInlineFrameEntry() = 0;

	/**
	 * Tears down everything: per-view JS state for every view still registered,
	 * then every context, then the runtime. Called at the top of thread Exit(),
	 * BEFORE the document hosts shut down -- so JS refs are released while the
	 * Rml trees still exist, and quickjs finalizers that free RmlUi objects run
	 * with the instancers alive (spec 5). Idempotent.
	 */
	virtual void Shutdown() = 0;
};

/**
 * Builds the process's script host. Registered before the UI thread boots
 * (FVaCuusModule::SetScriptHostFactory), invoked ON the UI thread in Init().
 */
using FVaCuusScriptHostFactory = TFunction<TUniquePtr<IVaCuusScriptHost>()>;
