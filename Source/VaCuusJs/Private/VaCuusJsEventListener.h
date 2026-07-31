// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "quickjs.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Types.h>

#include <atomic>

namespace Rml
{
class Element;
class Event;
}
class FVaCuusJsScriptHost;
class FVaCuusJsViewContext;

/**
 * The listener registry's key (M4 Task 5): one entry per addEventListener
 * registration, keyed exactly the way DOM defines registration identity --
 * (element, event type, callback identity, capture flag) -- so that
 * removeEventListener with the same triple finds it and a different capture
 * flag does not (Element.h:471-482 keeps the phases as separate listener
 * lists, and so does this map).
 *
 * FnPtr is the callback's OBJECT IDENTITY: JS_VALUE_GET_PTR of the function
 * value (quickjs.h:220/246/323 -- every value representation stores a heap
 * object's pointer there). Two JSValues naming the same function object carry
 * the same pointer, which is precisely SameValue for objects; the pointer is
 * stable and unique among live registrations because the registration itself
 * holds a dup of the function, pinning the object for the entry's lifetime.
 *
 * Element is the RAW pointer on purpose (the same reasoning as WrapperCache):
 * entries leave the map inside OnDetach, which on every element-death path
 * runs before the allocator could recycle the address -- directly from
 * ~Element (Element.cpp:99 -> :112) or from the unload sweep's DetachAllEvents
 * (Context.cpp:1565-1566) -- and on context death the neuter walk empties the
 * map wholesale. No stale key survives its element.
 */
struct FVaCuusJsListenerKey
{
	Rml::Element* Element = nullptr;
	FString Type;
	void* FnPtr = nullptr;
	bool bCapture = false;

	bool operator==(const FVaCuusJsListenerKey& Other) const
	{
		return Element == Other.Element && FnPtr == Other.FnPtr && bCapture == Other.bCapture && Type == Other.Type;
	}

	friend uint32 GetTypeHash(const FVaCuusJsListenerKey& Key)
	{
		return HashCombine(HashCombine(GetTypeHash(Key.Element), GetTypeHash(Key.FnPtr)),
			HashCombine(GetTypeHash(Key.Type), uint32(Key.bCapture ? 1 : 0)));
	}
};

/**
 * One JS-backed RmlUi event listener (M4 Task 5, spec 2(g)): one C++ object per
 * registration, in one of two modes --
 *
 *   FUNCTION mode (addEventListener): born with a dup'd JS callback and a
 *   registry entry in the owning view context; created only through
 *   the addEventListener thunk.
 *
 *   ATTRIBUTE mode (onclick="..." via the global EventListenerInstancer): born
 *   with only the snippet STRING and a host pointer, because at instancing time
 *   there is no view to compile against -- on* attributes are instanced from
 *   OnAttributeChange (Element.cpp:1724-1749), which for parsed markup runs
 *   mid-document-load, BEFORE the document is bound to any JS context (the
 *   host's document->view map is written at bind time). Resolution and
 *   compilation are therefore LAZY, once, at first fire; an element whose
 *   document never binds stays a no-op with one Warning, and a snippet that
 *   fails to compile stays a no-op after one Error naming document, element and
 *   attribute.
 *
 * LIFETIME -- the three death orders, all tolerated:
 *  (1) direct element destruction: ~Element fires OnElementDestroy, then
 *      destroys its meta, whose EventDispatcher destructor calls OnDetach for
 *      every remaining entry (Element.cpp:99 -> :112; EventDispatcher.cpp:33-38)
 *      -> release the JS ref, leave the registry, delete this;
 *  (2) document unload: ReleaseUnloadedDocuments detaches the WHOLE tree first,
 *      then destroys it (Context.cpp:1565-1567; DetachAllEvents recursion,
 *      EventDispatcher.cpp:64-73) -> same OnDetach path, before any destroy;
 *  (3) the context dies FIRST (view removed / shutdown, the old tree still
 *      attached): the context's neuter walk frees the JS ref and NEUTERS the
 *      shell -- this object stays allocated, owning nothing -- and RmlUi's
 *      later detach (the deferred tree teardown, or RemoveContext inside host
 *      shutdown) finds the neutered shell and self-deletes WITHOUT touching the
 *      dead context or the dead runtime. That last property is why a neutered
 *      OnDetach touches literally nothing but `delete this`.
 *
 * THE MID-DISPATCH HAZARD (hud-demo-patterns.md section 3, the experiment the
 * demo never ran): a handler that removes its own element triggers path (1)
 * SYNCHRONOUSLY -- RemoveChild's discarded ElementPtr destroys the element at
 * statement end inside JS_Call, and the destructor chain calls THIS listener's
 * OnDetach while its own ProcessEvent is still on the stack. RmlUi itself
 * survives (dispatch holds ObserverPtrs to element and listener and re-checks
 * both before each submit, EventDispatcher.cpp:81-99, :163-166); the object
 * must too: OnDetach releases refs immediately (ProcessEvent dup'd the
 * callback first, so the call in flight keeps its function) but DEFERS the
 * `delete this` until the outermost ProcessEvent frame exits. DispatchDepth is
 * a DEPTH, not a flag, because a handler can dispatchEvent() into a nested
 * dispatch that collects this same listener again.
 *
 * THREADING: every method runs on the thread that drives RmlUi and owns the JS
 * runtime -- dispatch happens inside Context::Update / ProcessMouse* /
 * ProcessKey* on the UI thread, teardown on the same thread by the existing
 * contracts. No cross-thread member exists except the shell counter below.
 */
class FVaCuusJsEventListener final : public Rml::EventListener
{
public:
	/**
	 * FUNCTION mode. Dups InFn, notes the ref on the runtime, inserts the
	 * registry entry -- AT CONSTRUCTION, not in OnAttach: the thunk has already
	 * de-duplicated against the registry (the DOM ignore-duplicates rule), so
	 * attach can never be refused by RmlUi (AttachEvent only drops entries that
	 * are equal INCLUDING the listener pointer, EventDispatcher.cpp:40-52, and
	 * this object is new), and construction is the one place that knows the key.
	 * OnAttach is therefore a documented no-op.
	 */
	static FVaCuusJsEventListener* CreateForFunction(
		FVaCuusJsViewContext& InContext, const FVaCuusJsListenerKey& InKey, JSValueConst InFn);

	/** ATTRIBUTE mode. Stores the snippet; joins the host's unresolved set until first fire. */
	static FVaCuusJsEventListener* CreateForAttribute(FVaCuusJsScriptHost& InHost, const Rml::String& InSource);

	//~ Begin Rml::EventListener
	virtual void ProcessEvent(Rml::Event& Event) override;
	virtual void OnAttach(Rml::Element* Element) override;
	virtual void OnDetach(Rml::Element* Element) override;
	//~ End Rml::EventListener

	/**
	 * The third death order's first half (spec 2(g)), called ONLY by the owning
	 * context's destructor walk: frees the JS ref against the still-live
	 * JSContext, notes the release, forgets the context. The walk empties the
	 * registry containers itself -- this method deliberately does not unregister.
	 * The shell stays allocated for RmlUi's later OnDetach to reclaim.
	 */
	void NeuterFromContext();

	/**
	 * The unresolved-attribute variant, called by host Shutdown() for listeners
	 * that never fired: no JS ref exists yet, so this only forgets the host --
	 * closing the window where a never-fired onclick shell could outlive the
	 * host it would have resolved through.
	 */
	void NeuterFromHost();

	/**
	 * TEST OBSERVABILITY: live shells, process-wide. A static -- NOT a runtime
	 * member like the fn-ref gauge -- because shells legally outlive every
	 * runtime and context (death order (3)'s whole point), so no instance can
	 * own the count. Exact gauge: ++ in the constructor, -- in the destructor.
	 */
	static int32 GetNumLiveShells() { return NumLiveShells.load(std::memory_order_relaxed); }

private:
	FVaCuusJsEventListener();

	/** Self-deleting (OnDetach only) -- private so nothing else can. */
	virtual ~FVaCuusJsEventListener() override;

	/** The release half shared by OnDetach's live path: free the ref, note it, leave the owning container. */
	void ReleaseAndUnregister();

	/**
	 * ATTRIBUTE mode's one-shot lazy bind: element -> owner document -> the
	 * host's document->view map -> compile the snippet in that view's context.
	 * Every failure is terminal for this listener (one diagnostic, then inert).
	 */
	void ResolveAttribute(Rml::Element* Element, const Rml::String& TypeName);

	/** Set in FUNCTION mode at birth, in ATTRIBUTE mode at successful resolve; null once neutered. */
	FVaCuusJsViewContext* OwnerContext = nullptr;

	/** ATTRIBUTE mode only, until resolve or neuter: the route to the document->view map. */
	FVaCuusJsScriptHost* Host = nullptr;

	/** FUNCTION mode: the registry key (OnDetach must remove the exact entry). Unused in ATTRIBUTE mode. */
	FVaCuusJsListenerKey Key;

	/** ATTRIBUTE mode: the snippet, verbatim, until it compiles (or fails to). */
	Rml::String AttributeSource;

	/** The dup'd callback (FUNCTION) or the compiled snippet function (ATTRIBUTE). */
	JSValue Fn = JS_UNDEFINED;

	/** See the class comment's mid-dispatch hazard. */
	int32 DispatchDepth = 0;

	bool bAttributeMode = false;

	/** ATTRIBUTE mode: resolve/compile runs once; failure leaves Fn undefined and this true. */
	bool bResolveAttempted = false;

	/** OnDetach arrived mid-dispatch; the outermost ProcessEvent exit performs the delete. */
	bool bDetachPending = false;

	/** Death order (3) happened: everything JS is gone, OnDetach must only self-delete. */
	bool bNeutered = false;

	static std::atomic<int32> NumLiveShells;
};
