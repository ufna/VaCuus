// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "quickjs.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ObserverPtr.h>
#include <RmlUi/Core/Types.h>

class FVaCuusJsViewContext;

/**
 * The class-id opaque behind every element/document wrapper (M4 spec 3.9,
 * 2(g)): one heap allocation per wrapper, attached with JS_SetOpaque
 * (quickjs.h:1044) and deleted by the shared finalizer below.
 *
 * THE OWNERSHIP MATRIX -- who owns the Rml element, per wrapper state:
 *
 *              | alive                              | dead
 *   -----------+------------------------------------+--------------------------------
 *   attached   | The PARENT owns: AppendChild /     | The tree destroyed it (remove(),
 *              | InsertBefore take the ElementPtr   | an innerRML replacement, a
 *              | by value into the parent's         | document close). Observer reads
 *              | children vector (Element.cpp:1347, | null -- ~EnableObserverPtr nulled
 *              | :1350, :1403). Owned is null; the  | the shared block
 *              | wrapper holds only the Observer.   | (ObserverPtr.h:124-131) -- and
 *              |                                    | the cache entry is ALREADY gone:
 *              |                                    | ~Element fired OnElementDestroy
 *              |                                    | first thing (Element.cpp:99).
 *   -----------+------------------------------------+--------------------------------
 *   detached   | THE WRAPPER OWNS: Owned holds the  | Reached only through the wrapper
 *              | ElementPtr -- createElement's      | itself: remove() on a detached
 *              | fresh instance, or the RemoveChild | wrapper resets Owned, destroying
 *              | return re-adopted ("discard the    | the subtree. From then on same
 *              | result to immediately destroy",    | as attached-dead.
 *              | Element.h:522-523, which is        |
 *              | exactly what this adoption         |
 *              | avoids). The finalizer frees the   |
 *              | subtree if JS drops the last ref.  |
 *
 * Transitions: createElement -> detached+alive (owns); appendChild/insertBefore
 * -> attached (Owned moved into the parent); removeChild -> detached+alive
 * (owns again, same wrapper); remove() / subtree replacement / document close
 * -> dead. The asymmetry that matters: an ATTACHED element can die at any time
 * RmlUi pleases, but a DETACHED one is unkillable from outside -- its
 * OnElementDestroy cannot fire until this wrapper releases the ElementPtr
 * (finalizer or remove()), because nobody else holds it.
 *
 * THE CACHE HANDSHAKE (bInCache): the owning context's WrapperCache maps
 * Element* -> wrapper JSValue WITHOUT a dup -- a borrowed ref, so the cache
 * alone keeps no wrapper alive. Two removers, one flag:
 *   - OnElementDestroy (the host's Rml plugin) probes each live context's cache
 *     by RAW pointer, erases the entry and clears bInCache. Raw pointer, not
 *     Observer.get(): during the hook the ObserverPtr still reads ALIVE, since
 *     ~EnableObserverPtr is a base subobject destructor and runs only after the
 *     ~Element body that fires the hook (Element.cpp:95-116; ObserverPtr.h:124-131).
 *   - The finalizer erases the entry if bInCache is still set (wrapper died
 *     first, element still alive).
 * The flag is what makes address reuse harmless: once either side cleared it,
 * a late finalizer cannot erase a NEW element's entry that happens to sit at
 * the recycled address.
 *
 * THREADING: constructing or destroying an ObserverPtr mutates RmlUi's
 * un-mutexed process-global block pool (ObserverPtr.cpp:12-24), so handles are
 * created and deleted only on the thread that owns the runtime -- in production
 * the UI thread, where the controlled GC point runs the finalizers (spec 3.6).
 */
struct FVaCuusJsElementHandle
{
	/** Null the moment the element dies -- the dead-check every method opens with. */
	Rml::ObserverPtr<Rml::Element> Observer;

	/** Set only while DETACHED: the wrapper's own ElementPtr (see the matrix). */
	Rml::ElementPtr Owned;

	/** The cache to erase from at finalization; nulled when the context dies first. */
	FVaCuusJsViewContext* OwnerContext = nullptr;

	/**
	 * The cache key, kept raw ON PURPOSE: at both erase sites the ObserverPtr is
	 * useless -- dead in the finalizer, still-alive-but-dying in the destroy hook.
	 */
	Rml::Element* RawKey = nullptr;

	/** The cache handshake -- see the class comment. */
	bool bInCache = false;
};

/**
 * The one finalizer for both wrapper classes (JSClassFinalizer, quickjs.h:659),
 * registered by FVaCuusJsRuntime, defined in VaCuusJsDom.cpp. Erases the cache
 * entry (if still owned), releases an owned ElementPtr -- which needs the
 * element's instancer alive (Element::Release, Element.cpp:2168-2174), true
 * structurally because every context/runtime death precedes Rml::Shutdown
 * (spec 2(g)) -- and deletes the handle.
 */
void VaCuusJsDomFinalizer(JSRuntime* Rt, JSValueConst Value);
