// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

/*
 * The DOM facade over Rml::Element (M4 Task 4, spec 3.9): identity-cached
 * wrappers, ElementPtr ownership for detached nodes, and a surface whose every
 * method opens with the dead-check -- a handle whose element got removed simply
 * goes dead: methods return null/false/undefined, never throw (the hud demo's
 * founding rule, hud-demo VacuusJs.cpp:1-3). Listeners and events are M4
 * Task 5, deliberately absent here.
 *
 * Split out of VaCuusJsViewContext.cpp for size only -- everything in this file
 * is FVaCuusJsViewContext (plus the shared finalizer), running on whatever
 * thread owns the host: the VaCuus UI thread in production, the automation
 * thread in library tests. RmlUi-touching calls only ever arrive here with a
 * live Rml world on this same thread (the facade tests boot the real UI thread
 * for exactly that reason).
 */

#include "VaCuusJsViewContext.h"

#include "VaCuusJs.h"
#include "VaCuusJsDomHandle.h"
#include "VaCuusJsRuntime.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/Variant.h>

#include <cstring>

namespace VaCuusJsDomInternal
{
//~ Magic values for the thunk families (int16_t in JSCFunctionListEntry, so
//~ plain enums, not enum classes).
enum EStringProp : int32
{
	PropId = 0,
	PropTagName,
	PropInnerRML
};

enum EQueryOp : int32
{
	QuerySelector = 0,
	QuerySelectorAll,
	Closest
};

enum EAttributeOp : int32
{
	AttrGet = 0,
	AttrSet,
	AttrRemove
};

enum EInsertOp : int32
{
	InsertAppend = 0,
	InsertBeforeRef
};

enum EClassListOp : int32
{
	ClassAdd = 0,
	ClassRemove,
	ClassToggle,
	ClassContains
};

enum EStyleOp : int32
{
	StyleGet = 0,
	StyleSet,
	StyleRemove
};

/**
 * Coerces Value to UTF-8 into Out. False = the coercion itself threw (a
 * toString that throws); the exception is pending and the caller must
 * propagate JS_EXCEPTION -- that throw is the SCRIPT's, not a facade throw,
 * so it does not violate the dead-handle rule.
 */
bool ToRmlString(JSContext* Ctx, JSValueConst Value, Rml::String& Out)
{
	size_t Len = 0;
	const char* Utf8 = JS_ToCStringLen(Ctx, &Len, Value);
	if (Utf8 == nullptr)
	{
		return false;
	}
	Out.assign(Utf8, Len);
	JS_FreeCString(Ctx, Utf8);
	return true;
}

JSValue NewRmlString(JSContext* Ctx, const Rml::String& Value)
{
	return JS_NewStringLen(Ctx, Value.c_str(), Value.size());
}

/**
 * The style proxy (spec 3.9), built in JS because arbitrary property names
 * need real get/set/delete traps, and a Proxy is that machinery for free --
 * the C alternative is a JSClassExoticMethods table for one object shape.
 * Compiled ONCE per context into FVaCuusJsViewContext::StyleFactory; called
 * per style access with three bound C functions (StyleOpThunk). Contract:
 *   - property GET  -> Element::GetProperty(name)->ToString(), copied at once;
 *   - property SET  -> Element::SetProperty(name, value); the trap answers
 *     true either way, so assignment NEVER throws -- setProperty(name, value)
 *     is the spelling that returns the parse-success bool;
 *   - delete / removeProperty -> Element::RemoveProperty(name).
 * Non-string keys (symbols) read undefined and write nowhere.
 */
constexpr const char* GStyleFactorySource =
	"(function(get, set, del) {"
	"    return new Proxy({}, {"
	"        get: function(t, key) {"
	"            if (key === 'setProperty') return set;"
	"            if (key === 'removeProperty') return del;"
	"            return typeof key === 'string' ? get(key) : undefined;"
	"        },"
	"        set: function(t, key, value) {"
	"            if (typeof key === 'string') set(key, value);"
	"            return true;"
	"        },"
	"        deleteProperty: function(t, key) {"
	"            if (typeof key === 'string') del(key);"
	"            return true;"
	"        }"
	"    });"
	"})";
}	 // namespace VaCuusJsDomInternal

void VaCuusJsDomFinalizer(JSRuntime* /*Rt*/, JSValueConst Value)
{
	JSClassID ClassId = 0;
	FVaCuusJsElementHandle* Handle = static_cast<FVaCuusJsElementHandle*>(JS_GetAnyOpaque(Value, &ClassId));
	if (Handle == nullptr)
	{
		return;
	}

	// Cache first, release second, and the order is load-bearing: releasing an
	// owned DETACHED subtree below fires OnElementDestroy for every element in
	// it, and the destroy hook probes the caches -- this wrapper's own entry
	// must already be gone so the hook never touches a value mid-finalization.
	// bInCache is false when the element or the whole context died first
	// (VaCuusJsDomHandle.h has the handshake).
	if (Handle->bInCache && Handle->OwnerContext != nullptr)
	{
		Handle->OwnerContext->RemoveCacheEntry(Handle->RawKey);
		Handle->bInCache = false;
	}

	// A detached element the wrapper still owned dies here, through
	// Element::Release -> instancer (Element.cpp:2168-2174) -- alive because
	// every context/runtime death precedes Rml::Shutdown (spec 2(g)). And the
	// handle's ObserverPtr release mutates RmlUi's un-mutexed global pool
	// (ObserverPtr.cpp:12-24), which is why finalizers run only on the owning
	// thread: the controlled GC point, context teardown, or a facade thunk's
	// refcount-zero -- never anywhere else.
	Handle->Owned.reset();
	delete Handle;
}

FVaCuusJsElementHandle* FVaCuusJsViewContext::GetHandle(JSValueConst Value) const
{
	// JS_GetOpaque answers null on a class-id mismatch (quickjs.h:1045), so a
	// foreign `this` -- a script .call()ing a facade method on some other object
	// -- degrades to the dead-handle path instead of a bad cast.
	void* Opaque = JS_GetOpaque(Value, Runtime.GetElementClassId());
	if (Opaque == nullptr)
	{
		Opaque = JS_GetOpaque(Value, Runtime.GetDocumentClassId());
	}
	return static_cast<FVaCuusJsElementHandle*>(Opaque);
}

Rml::Element* FVaCuusJsViewContext::GetLiveElement(JSValueConst Value) const
{
	const FVaCuusJsElementHandle* Handle = GetHandle(Value);
	return Handle != nullptr ? Handle->Observer.get() : nullptr;
}

JSValue FVaCuusJsViewContext::WrapElement(Rml::Element* Element)
{
	if (Element == nullptr || Ctx == nullptr)
	{
		return JS_NULL;
	}

	// THE IDENTITY RULE: same element, same wrapper, `===` across lookups. The
	// cached value is borrowed; the dup is the caller's ref.
	if (const JSValue* Found = WrapperCache.Find(Element))
	{
		return JS_DupValue(Ctx, *Found);
	}

	// A document gets the richer prototype; everything else the element one.
	// One cache for both, so `body.parentNode === document` holds -- the parent
	// of a top-level element IS the ElementDocument in RmlUi.
	//
	// The discriminator is the owner-document SELF-reference: ElementDocument's
	// constructor force-sets owner_document = this (ElementDocument.cpp:135) and
	// Element::SetOwnerDocument refuses to change it for a document ever after
	// ("If this element is a document, then never change owner_document, except
	// if forced", Element.cpp:2136-2141) -- the only force-clear is
	// ~ElementDocument (ElementDocument.cpp:144), long past any possible wrap.
	// DELIBERATELY NOT rmlui_dynamic_cast: under RMLUI_CUSTOM_RTTI the class
	// identifier is the address of a function-local static inside an inline
	// member (Traits.h:67-91), and UBT's Linux modular builds compile with
	// -fvisibility-ms-compat / -fvisibility-inlines-hidden
	// (LinuxToolChain.cs:439, :450), under which THIS module instantiates its
	// own copy of that static -- the cast answers null for any object whose
	// vtable lives in VaCuusRml.so. Observed directly: with the cast, every
	// document wrapped as a plain element and document.getElementById did not
	// exist. (The same hazard hangs over every cross-module rmlui_dynamic_cast;
	// flagged to the milestone controller, out of this task's scope.)
	const bool bIsDocument = Element->GetOwnerDocument() == Element;
	JSValue Wrapper = JS_NewObjectClass(Ctx, bIsDocument ? Runtime.GetDocumentClassId() : Runtime.GetElementClassId());
	if (JS_IsException(Wrapper))
	{
		// Heap at the cap. Propagated, not swallowed: the entry boundary's
		// ReportException is what recognizes the OOM InternalError and arms the
		// fallback collection (spec 2(b)).
		return Wrapper;
	}

	FVaCuusJsElementHandle* Handle = new FVaCuusJsElementHandle();
	Handle->Observer = Element->GetObserverPtr();
	Handle->OwnerContext = this;
	Handle->RawKey = Element;
	Handle->bInCache = true;
	JS_SetOpaque(Wrapper, Handle);

	WrapperCache.Add(Element, Wrapper);
	return Wrapper;
}

void FVaCuusJsViewContext::RemoveCacheEntry(Rml::Element* RawKey)
{
	// Values are borrowed (WrapperCache's comment): removal frees nothing.
	WrapperCache.Remove(RawKey);
}

void FVaCuusJsViewContext::OnRmlElementDestroyed(Rml::Element* Element)
{
	JSValue Value;
	if (!WrapperCache.RemoveAndCopyValue(Element, Value))
	{
		return;
	}

	// The half of the handshake the finalizer needs: without this, a wrapper
	// finalized AFTER its element died would erase whatever entry now lives at
	// the recycled address. The wrapper itself needs no further marking -- its
	// ObserverPtr still reads alive DURING this hook (~EnableObserverPtr is a
	// base subobject destructor, so it runs after the ~Element body that fired
	// the hook -- Element.cpp:95-116, ObserverPtr.h:124-131), which is exactly
	// why this map keys on the raw pointer; the observer goes dead by itself
	// the moment the destructor chain completes, and no JS can run in between.
	JSClassID ClassId = 0;
	if (FVaCuusJsElementHandle* Handle = static_cast<FVaCuusJsElementHandle*>(JS_GetAnyOpaque(Value, &ClassId)))
	{
		Handle->bInCache = false;
	}
}

void FVaCuusJsViewContext::BindDocument(Rml::ElementDocument* Document)
{
	if (Ctx == nullptr)
	{
		return;
	}

	JSValue DocValue = Document != nullptr ? WrapElement(Document) : JS_NULL;
	if (JS_IsException(DocValue))
	{
		Runtime.ReportException(Ctx, TEXT("BindDocument"));
		DocValue = JS_NULL;
	}

	JSValue Global = JS_GetGlobalObject(Ctx);
	JS_SetPropertyStr(Ctx, Global, "document", DocValue);	 // takes ownership of DocValue
	JS_FreeValue(Ctx, Global);
}

void FVaCuusJsViewContext::InstallDomPrototypes()
{
	using namespace VaCuusJsDomInternal;

	// The per-CONTEXT half of the class split (the runtime holds the ids and
	// defs, VaCuusJsRuntime.cpp): prototypes live on the JSContext via
	// JS_SetClassProto (quickjs.h:527), so sibling views never share method
	// objects -- one view's script tampering with a prototype stays its own.
	static const JSCFunctionListEntry GElementProtoEntries[] = {
		JS_CFUNC_MAGIC_DEF("appendChild", 1, FVaCuusJsViewContext::InsertThunk, InsertAppend),
		JS_CFUNC_MAGIC_DEF("insertBefore", 2, FVaCuusJsViewContext::InsertThunk, InsertBeforeRef),
		JS_CFUNC_DEF("removeChild", 1, FVaCuusJsViewContext::RemoveChildThunk),
		JS_CFUNC_DEF("remove", 0, FVaCuusJsViewContext::RemoveThunk),
		JS_CFUNC_MAGIC_DEF("querySelector", 1, FVaCuusJsViewContext::QueryThunk, QuerySelector),
		JS_CFUNC_MAGIC_DEF("querySelectorAll", 1, FVaCuusJsViewContext::QueryThunk, QuerySelectorAll),
		JS_CFUNC_MAGIC_DEF("closest", 1, FVaCuusJsViewContext::QueryThunk, Closest),
		JS_CFUNC_MAGIC_DEF("getAttribute", 1, FVaCuusJsViewContext::AttributeThunk, AttrGet),
		JS_CFUNC_MAGIC_DEF("setAttribute", 2, FVaCuusJsViewContext::AttributeThunk, AttrSet),
		JS_CFUNC_MAGIC_DEF("removeAttribute", 1, FVaCuusJsViewContext::AttributeThunk, AttrRemove),
		JS_CGETSET_MAGIC_DEF("id", FVaCuusJsViewContext::StringGetterThunk, FVaCuusJsViewContext::StringSetterThunk, PropId),
		JS_CGETSET_MAGIC_DEF("tagName", FVaCuusJsViewContext::StringGetterThunk, nullptr, PropTagName),
		JS_CGETSET_MAGIC_DEF(
			"innerRML", FVaCuusJsViewContext::StringGetterThunk, FVaCuusJsViewContext::StringSetterThunk, PropInnerRML),
		JS_CGETSET_DEF("parentNode", FVaCuusJsViewContext::ParentNodeGetterThunk, nullptr),
		JS_CGETSET_DEF("children", FVaCuusJsViewContext::ChildrenGetterThunk, nullptr),
		JS_CGETSET_DEF("classList", FVaCuusJsViewContext::ClassListGetterThunk, nullptr),
		JS_CGETSET_DEF("style", FVaCuusJsViewContext::StyleGetterThunk, nullptr),
		JS_CFUNC_DEF("addEventListener", 2, FVaCuusJsViewContext::AddEventListenerThunk),
		JS_CFUNC_DEF("removeEventListener", 2, FVaCuusJsViewContext::RemoveEventListenerThunk),
		JS_CFUNC_DEF("dispatchEvent", 1, FVaCuusJsViewContext::DispatchEventThunk),
	};

	static const JSCFunctionListEntry GDocumentProtoEntries[] = {
		JS_CFUNC_DEF("createElement", 1, FVaCuusJsViewContext::DocCreateElementThunk),
		JS_CFUNC_DEF("getElementById", 1, FVaCuusJsViewContext::DocGetElementByIdThunk),
		JS_CGETSET_DEF("body", FVaCuusJsViewContext::DocBodyGetterThunk, nullptr),
	};

	JSValue ElementProto = JS_NewObject(Ctx);
	JS_SetPropertyFunctionList(Ctx, ElementProto, GElementProtoEntries, UE_ARRAY_COUNT(GElementProtoEntries));

	JSValue DocumentProto = JS_NewObject(Ctx);
	JS_SetPropertyFunctionList(Ctx, DocumentProto, GDocumentProtoEntries, UE_ARRAY_COUNT(GDocumentProtoEntries));

	// The document prototype CHAINS to the element one: an Rml::ElementDocument
	// IS an Element (Element.h:47's hierarchy), so `document` answers the whole
	// element surface -- querySelector, appendChild, children -- plus its own
	// three. JS_SetPrototype borrows proto_val (quickjs.h:977); the SetClassProto
	// pair below then takes ownership of both prototypes.
	JS_SetPrototype(Ctx, DocumentProto, ElementProto);
	JS_SetClassProto(Ctx, Runtime.GetElementClassId(), ElementProto);
	JS_SetClassProto(Ctx, Runtime.GetDocumentClassId(), DocumentProto);

	// The event class's prototype (M4 Task 5) rides the same per-context rule.
	InstallEventPrototype();

	// The style factory (GStyleFactorySource's comment). A member, not a global:
	// unclobberable from script.
	StyleFactory =
		JS_Eval(Ctx, GStyleFactorySource, std::strlen(GStyleFactorySource), "<vacuus-style-proxy>", JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(StyleFactory))
	{
		// Only reachable with the heap at the cap at context birth; the style
		// getter then answers null until the context is recycled.
		Runtime.ReportException(Ctx, TEXT("<vacuus-style-proxy>"));
		StyleFactory = JS_UNDEFINED;
	}
}

// ---------------------------------------------------------------------------
// Document prototype
// ---------------------------------------------------------------------------

JSValue FVaCuusJsViewContext::DocCreateElementThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv)
{
	using namespace VaCuusJsDomInternal;

	FVaCuusJsViewContext* Self = static_cast<FVaCuusJsViewContext*>(JS_GetContextOpaque(Ctx));
	check(Self != nullptr);

	if (Self->GetLiveElement(This) == nullptr || Argc < 1)
	{
		return JS_NULL;
	}

	Rml::String Tag;
	if (!ToRmlString(Ctx, Argv[0], Tag))
	{
		return JS_EXCEPTION;
	}

	// LOWERCASED, OR RCSS SILENTLY NEVER MATCHES (spec 3.9, E1): instancer names
	// are registered lowercased but looked up verbatim (Factory.cpp:297 vs
	// :302), and the only guard -- the Element ctor's lowercase assert
	// (Element.cpp:76) -- is compiled out under UBT's global NDEBUG. So
	// createElement("DIV") would instance a generic element whose tag matches
	// no `div` rule and no `div` selector, with no diagnostic anywhere. The
	// facade test's red half demonstrates exactly that element.
	Tag = Rml::StringUtilities::ToLower(Tag);

	// Document::CreateElement is literally this Factory call -- it stores no
	// document pointer (ElementDocument.cpp:427-430) -- so the facade calls the
	// Factory directly and skips the ElementDocument cast. The caller of
	// InstanceElement owns the returned ElementPtr (Factory.h:64-70): the fresh
	// node is DETACHED and the wrapper adopts it (the ownership matrix,
	// VaCuusJsDomHandle.h).
	Rml::ElementPtr Fresh = Rml::Factory::InstanceElement(nullptr, Tag, Tag, Rml::XMLAttributes());
	if (!Fresh)
	{
		return JS_NULL;
	}

	JSValue Wrapper = Self->WrapElement(Fresh.get());
	if (JS_IsException(Wrapper))
	{
		return Wrapper;	   // Fresh releases here -- nothing leaked
	}
	if (FVaCuusJsElementHandle* Handle = Self->GetHandle(Wrapper))
	{
		Handle->Owned = MoveTemp(Fresh);
	}
	return Wrapper;
}

JSValue FVaCuusJsViewContext::DocGetElementByIdThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv)
{
	using namespace VaCuusJsDomInternal;

	FVaCuusJsViewContext* Self = static_cast<FVaCuusJsViewContext*>(JS_GetContextOpaque(Ctx));
	check(Self != nullptr);

	Rml::Element* Doc = Self->GetLiveElement(This);
	if (Doc == nullptr || Argc < 1)
	{
		return JS_NULL;
	}

	Rml::String Id;
	if (!ToRmlString(Ctx, Argv[0], Id))
	{
		return JS_EXCEPTION;
	}

	// Element::GetElementById searches from the OWNER DOCUMENT's root
	// (Element.cpp:1512-1528) -- called on the document wrapper that root is the
	// document itself, giving the DOM's whole-tree semantics for free.
	return Self->WrapElement(Doc->GetElementById(Id));
}

JSValue FVaCuusJsViewContext::DocBodyGetterThunk(JSContext* Ctx, JSValueConst This)
{
	FVaCuusJsViewContext* Self = static_cast<FVaCuusJsViewContext*>(JS_GetContextOpaque(Ctx));
	check(Self != nullptr);

	Rml::Element* Doc = Self->GetLiveElement(This);
	if (Doc == nullptr)
	{
		return JS_NULL;
	}

	// In RmlUi the ElementDocument IS the body element (its tag is "body" and
	// top-level elements are its direct children), so `document.body` is the
	// SAME wrapper as `document` -- `document.body === document` holds, a
	// documented collapse of the HTML distinction that does not exist here.
	return Self->WrapElement(Doc);
}

// ---------------------------------------------------------------------------
// Element prototype: tree surgery
// ---------------------------------------------------------------------------

JSValue FVaCuusJsViewContext::InsertThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv, int Magic)
{
	using namespace VaCuusJsDomInternal;

	FVaCuusJsViewContext* Self = static_cast<FVaCuusJsViewContext*>(JS_GetContextOpaque(Ctx));
	check(Self != nullptr);

	Rml::Element* Parent = Self->GetLiveElement(This);
	if (Parent == nullptr || Argc < 1)
	{
		return JS_NULL;
	}

	FVaCuusJsElementHandle* ChildHandle = Self->GetHandle(Argv[0]);
	Rml::Element* Child = ChildHandle != nullptr ? ChildHandle->Observer.get() : nullptr;
	if (Child == nullptr)
	{
		return JS_NULL;	   // dead or not-an-element argument: null, never throw
	}
	if (ChildHandle->OwnerContext != Self)
	{
		// A wrapper minted by another view's context: moving an element between
		// Rml contexts is not a supported operation; refused as null.
		return JS_NULL;
	}

	// CYCLE GUARD, because RmlUi has none: appending an ancestor into its own
	// descendant would first RemoveChild the ancestor -- destroying/detaching
	// the very subtree the target parent lives in -- and then build an
	// ownership cycle. DOM throws HierarchyRequestError here; the facade's
	// no-throw spelling is null.
	for (Rml::Element* Walk = Parent; Walk != nullptr; Walk = Walk->GetParentNode())
	{
		if (Walk == Child)
		{
			return JS_NULL;
		}
	}

	// RECOVER THE ElementPtr -- AppendChild/InsertBefore take it by value
	// (Element.cpp:1342, :1372), so somebody must give ownership up:
	//   - a DETACHED wrapper gives up its own (createElement / removeChild);
	//   - an ATTACHED child is taken from its current parent -- RemoveChild
	//     returns the ElementPtr (Element.h:522-523) -- the DOM's move-on-insert;
	//   - alive but detached and NOT ours (a C++-held ElementPtr) is nobody the
	//     facade may move: null.
	Rml::ElementPtr Moving;
	if (ChildHandle->Owned)
	{
		Moving = MoveTemp(ChildHandle->Owned);
	}
	else if (Rml::Element* OldParent = Child->GetParentNode())
	{
		Moving = OldParent->RemoveChild(Child);
	}
	if (!Moving)
	{
		return JS_NULL;
	}

	if (Magic == InsertBeforeRef)
	{
		// A null/dead/absent ref appends at the end -- RmlUi's own fallback
		// (Element.cpp:1375-1377, :1413-1416), which matches DOM for a null ref;
		// for a ref that is NOT a child of this element DOM would throw
		// NotFoundError, the facade appends instead. Documented deviation.
		Rml::Element* Ref = Argc >= 2 ? Self->GetLiveElement(Argv[1]) : nullptr;
		Parent->InsertBefore(MoveTemp(Moving), Ref);
	}
	else
	{
		Parent->AppendChild(MoveTemp(Moving));
	}

	// The wrapper is now ATTACHED: Owned is empty, the Observer is the whole
	// handle. Returned like DOM returns the inserted node -- the SAME wrapper,
	// identity preserved.
	return JS_DupValue(Ctx, Argv[0]);
}

JSValue FVaCuusJsViewContext::RemoveChildThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv)
{
	FVaCuusJsViewContext* Self = static_cast<FVaCuusJsViewContext*>(JS_GetContextOpaque(Ctx));
	check(Self != nullptr);

	Rml::Element* Parent = Self->GetLiveElement(This);
	if (Parent == nullptr || Argc < 1)
	{
		return JS_NULL;
	}

	FVaCuusJsElementHandle* ChildHandle = Self->GetHandle(Argv[0]);
	Rml::Element* Child = ChildHandle != nullptr ? ChildHandle->Observer.get() : nullptr;
	if (Child == nullptr || Child->GetParentNode() != Parent)
	{
		return JS_NULL;	   // dead, foreign, or simply not our child
	}

	// The returned ElementPtr is the element's LIFE: discarded it destroys the
	// subtree (Element.h:522-523). Adopting it into the child's own wrapper is
	// what makes removeChild-then-re-append work: the wrapper flips to
	// detached+owning (the matrix), stays fully alive, and keeps its identity.
	Rml::ElementPtr Detached = Parent->RemoveChild(Child);
	if (!Detached)
	{
		return JS_NULL;
	}
	ChildHandle->Owned = MoveTemp(Detached);

	return JS_DupValue(Ctx, Argv[0]);
}

JSValue FVaCuusJsViewContext::RemoveThunk(JSContext* Ctx, JSValueConst This, int /*Argc*/, JSValueConst* /*Argv*/)
{
	FVaCuusJsViewContext* Self = static_cast<FVaCuusJsViewContext*>(JS_GetContextOpaque(Ctx));
	check(Self != nullptr);

	FVaCuusJsElementHandle* Handle = Self->GetHandle(This);
	Rml::Element* Element = Handle != nullptr ? Handle->Observer.get() : nullptr;
	if (Element == nullptr)
	{
		return JS_UNDEFINED;	// already dead: a second remove() is a silent no-op
	}

	if (Handle->Owned)
	{
		// Detached and ours: remove() == discard == destroy, now. The subtree's
		// OnElementDestroy hooks erase every cache entry before this returns.
		Handle->Owned.reset();
	}
	else if (Rml::Element* Parent = Element->GetParentNode())
	{
		// Attached: the demo's exact mechanism -- RemoveChild's returned
		// ElementPtr, discarded, destroys the element at statement end
		// (Element.h:522-523; hud-demo VacuusJs.cpp:253).
		Parent->RemoveChild(Element);
	}
	// else: alive, detached, owned by C++ somewhere -- not the facade's to
	// destroy; no-op by the same never-throw rule.

	return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Element prototype: queries and attributes
// ---------------------------------------------------------------------------

JSValue FVaCuusJsViewContext::QueryThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv, int Magic)
{
	using namespace VaCuusJsDomInternal;

	FVaCuusJsViewContext* Self = static_cast<FVaCuusJsViewContext*>(JS_GetContextOpaque(Ctx));
	check(Self != nullptr);

	Rml::Element* Element = Self->GetLiveElement(This);

	// Dead handles: null for the single-result pair, an EMPTY ARRAY for
	// querySelectorAll -- the one deliberate softening of the null rule, so a
	// `for..of` over a dead handle's query iterates zero times instead of
	// throwing on null. (An invalid selector behaves the same way: RmlUi logs
	// a warning and returns null/empty, Element.cpp:1592-1596 -- no throw.)
	if (Element == nullptr || Argc < 1)
	{
		return Magic == QuerySelectorAll ? JS_NewArray(Ctx) : JS_NULL;
	}

	Rml::String Selector;
	if (!ToRmlString(Ctx, Argv[0], Selector))
	{
		return JS_EXCEPTION;
	}

	switch (Magic)
	{
		case QuerySelector:
			// DEVIATION FROM DOM, pinned by the tests: matching starts at the
			// CHILDREN -- the element itself is never a match, because the
			// recursive matcher only ever tests child nodes
			// (QuerySelectorMatchRecursive, Element.cpp:1540-1546).
			return Self->WrapElement(Element->QuerySelector(Selector));

		case Closest:
			// DEVIATION FROM DOM, pinned by the tests: DOM's closest() starts at
			// the element itself; RmlUi's starts at the PARENT and walks up
			// (Element.cpp:1077-1090), so el.closest(matching-own-selector) is
			// null unless an ancestor also matches.
			return Self->WrapElement(Element->Closest(Selector));

		case QuerySelectorAll:
		{
			Rml::ElementList Results;
			Element->QuerySelectorAll(Results, Selector);

			JSValue Array = JS_NewArray(Ctx);
			uint32 Index = 0;
			for (Rml::Element* Match : Results)
			{
				JSValue Wrapper = Self->WrapElement(Match);
				if (JS_IsException(Wrapper))
				{
					JS_FreeValue(Ctx, Array);
					return Wrapper;
				}
				JS_DefinePropertyValueUint32(Ctx, Array, Index++, Wrapper, JS_PROP_C_W_E);	  // takes Wrapper
			}
			return Array;
		}

		default:
			return JS_NULL;
	}
}

JSValue FVaCuusJsViewContext::AttributeThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv, int Magic)
{
	using namespace VaCuusJsDomInternal;

	FVaCuusJsViewContext* Self = static_cast<FVaCuusJsViewContext*>(JS_GetContextOpaque(Ctx));
	check(Self != nullptr);

	Rml::Element* Element = Self->GetLiveElement(This);
	if (Element == nullptr || Argc < 1)
	{
		return Magic == AttrGet ? JS_NULL : JS_UNDEFINED;
	}

	Rml::String Name;
	if (!ToRmlString(Ctx, Argv[0], Name))
	{
		return JS_EXCEPTION;
	}

	switch (Magic)
	{
		case AttrGet:
		{
			// Attributes are Variants (Element.h:291); Get<String> runs the type
			// converter, so numeric attributes read back as their string form.
			// Missing attribute -> null, the DOM answer.
			const Rml::Variant* Value = Element->GetAttribute(Name);
			return Value != nullptr ? NewRmlString(Ctx, Value->Get<Rml::String>()) : JS_NULL;
		}

		case AttrSet:
		{
			if (Argc < 2)
			{
				return JS_UNDEFINED;
			}
			Rml::String Value;
			if (!ToRmlString(Ctx, Argv[1], Value))
			{
				return JS_EXCEPTION;
			}
			// Writes the map, then notifies OnAttributeChange synchronously with
			// the one changed entry (Element.inl:15-23) -- so id, class and style
			// attribute writes take their built-in effects immediately.
			Element->SetAttribute(Name, Value);
			return JS_UNDEFINED;
		}

		case AttrRemove:
			Element->RemoveAttribute(Name);
			return JS_UNDEFINED;

		default:
			return JS_UNDEFINED;
	}
}

// ---------------------------------------------------------------------------
// Element prototype: properties
// ---------------------------------------------------------------------------

JSValue FVaCuusJsViewContext::StringGetterThunk(JSContext* Ctx, JSValueConst This, int Magic)
{
	using namespace VaCuusJsDomInternal;

	FVaCuusJsViewContext* Self = static_cast<FVaCuusJsViewContext*>(JS_GetContextOpaque(Ctx));
	check(Self != nullptr);

	Rml::Element* Element = Self->GetLiveElement(This);
	if (Element == nullptr)
	{
		return JS_NULL;
	}

	switch (Magic)
	{
		case PropId:
			return NewRmlString(Ctx, Element->GetId());

		case PropTagName:
			// Verbatim, which in RmlUi means LOWERCASE -- a documented deviation
			// from DOM's uppercased tagName: the whole RCSS world is lowercase
			// (the E1 trap), and echoing that truth beats imitating HTML.
			return NewRmlString(Ctx, Element->GetTagName());

		case PropInnerRML:
			return NewRmlString(Ctx, Element->GetInnerRML());

		default:
			return JS_NULL;
	}
}

JSValue FVaCuusJsViewContext::StringSetterThunk(JSContext* Ctx, JSValueConst This, JSValueConst Value, int Magic)
{
	using namespace VaCuusJsDomInternal;

	FVaCuusJsViewContext* Self = static_cast<FVaCuusJsViewContext*>(JS_GetContextOpaque(Ctx));
	check(Self != nullptr);

	Rml::Element* Element = Self->GetLiveElement(This);
	if (Element == nullptr)
	{
		return JS_UNDEFINED;	// dead: assignment silently no-ops, never throws
	}

	Rml::String Text;
	if (!ToRmlString(Ctx, Value, Text))
	{
		return JS_EXCEPTION;
	}

	switch (Magic)
	{
		case PropId:
			Element->SetId(Text);
			break;

		case PropInnerRML:
			// SetInnerRML removes every DOM child DISCARDING each RemoveChild
			// return, so the replaced children are destroyed synchronously INSIDE
			// this call (Element.cpp:1167-1177; rmlui-scripting.md section 6):
			// their wrappers read dead and their cache entries are erased -- via
			// the destroy hook -- before this setter returns. Identity is never
			// preserved across an innerRML write.
			Element->SetInnerRML(Text);
			break;

		default:
			break;
	}
	return JS_UNDEFINED;
}

JSValue FVaCuusJsViewContext::ParentNodeGetterThunk(JSContext* Ctx, JSValueConst This)
{
	FVaCuusJsViewContext* Self = static_cast<FVaCuusJsViewContext*>(JS_GetContextOpaque(Ctx));
	check(Self != nullptr);

	Rml::Element* Element = Self->GetLiveElement(This);
	if (Element == nullptr)
	{
		return JS_NULL;
	}

	// Null for a detached node AND for the document itself; for a top-level
	// element this is the ElementDocument, which wraps -- through the same
	// cache -- as the Document-class wrapper: `topLevel.parentNode === document`.
	return Self->WrapElement(Element->GetParentNode());
}

JSValue FVaCuusJsViewContext::ChildrenGetterThunk(JSContext* Ctx, JSValueConst This)
{
	FVaCuusJsViewContext* Self = static_cast<FVaCuusJsViewContext*>(JS_GetContextOpaque(Ctx));
	check(Self != nullptr);

	// A LIVE SNAPSHOT PER CALL, not DOM's live HTMLCollection: a fresh array
	// reflecting the tree as of this access; later tree mutations do not update
	// an array a script already holds -- re-read `children` instead. Dead
	// handles read an empty array (the querySelectorAll softening). Contents:
	// DOM children only (GetNumChildren excludes RmlUi's non-DOM extras like
	// scrollbars, Element.cpp:1147-1150), minus text nodes -- RmlUi text is an
	// element tagged "#text", which querySelector skips the same way
	// (Element.cpp:1547) and DOM's `children` never contains.
	JSValue Array = JS_NewArray(Ctx);
	Rml::Element* Element = Self->GetLiveElement(This);
	if (Element == nullptr)
	{
		return Array;
	}

	const int NumChildren = Element->GetNumChildren();
	uint32 Index = 0;
	for (int ChildIndex = 0; ChildIndex < NumChildren; ++ChildIndex)
	{
		Rml::Element* Child = Element->GetChild(ChildIndex);
		if (Child == nullptr || Child->GetTagName() == "#text")
		{
			continue;
		}
		JSValue Wrapper = Self->WrapElement(Child);
		if (JS_IsException(Wrapper))
		{
			JS_FreeValue(Ctx, Array);
			return Wrapper;
		}
		JS_DefinePropertyValueUint32(Ctx, Array, Index++, Wrapper, JS_PROP_C_W_E);	  // takes Wrapper
	}
	return Array;
}

JSValue FVaCuusJsViewContext::ClassListGetterThunk(JSContext* Ctx, JSValueConst This)
{
	using namespace VaCuusJsDomInternal;

	// A fresh object of four bound functions per access (`el.classList ===
	// el.classList` is false -- a documented deviation; DOMTokenList identity
	// buys nothing here). Each function carries the WRAPPER as JSCFunctionData
	// (quickjs.h:453, dup'd and freed by the function object), so the methods
	// keep working -- and keep dead-checking -- however far the object travels.
	// Built even on a dead handle: `dead.classList.add('x')` must no-op, not
	// throw on a null.
	JSValue Object = JS_NewObject(Ctx);
	if (JS_IsException(Object))
	{
		return Object;
	}

	static const struct
	{
		const char* Name;
		int32 Magic;
		int Length;
	} GOps[] = {
		{"add", ClassAdd, 1},
		{"remove", ClassRemove, 1},
		{"toggle", ClassToggle, 1},
		{"contains", ClassContains, 1},
	};

	JSValueConst Data[1] = {This};
	for (const auto& Op : GOps)
	{
		JS_SetPropertyStr(Ctx, Object, Op.Name,
			JS_NewCFunctionData2(Ctx, &FVaCuusJsViewContext::ClassListOpThunk, Op.Name, Op.Length, Op.Magic, 1, Data));
	}
	return Object;
}

JSValue FVaCuusJsViewContext::ClassListOpThunk(
	JSContext* Ctx, JSValueConst /*This*/, int Argc, JSValueConst* Argv, int Magic, JSValueConst* FuncData)
{
	using namespace VaCuusJsDomInternal;

	FVaCuusJsViewContext* Self = static_cast<FVaCuusJsViewContext*>(JS_GetContextOpaque(Ctx));
	check(Self != nullptr);

	Rml::Element* Element = Self->GetLiveElement(FuncData[0]);
	if (Element == nullptr || Argc < 1)
	{
		return (Magic == ClassToggle || Magic == ClassContains) ? JS_FALSE : JS_UNDEFINED;
	}

	Rml::String Name;
	if (!ToRmlString(Ctx, Argv[0], Name))
	{
		return JS_EXCEPTION;
	}

	// NEVER THROUGH THE CLASS ATTRIBUTE, in either direction. SetClass mutates
	// meta->style directly and does NOT write the `class` attribute back --
	// only SetClassNames routes through SetAttribute -- so after any SetClass
	// the attribute is STALE while IsClassSet/GetClassNames tell the truth
	// (Element.cpp:258-276). A classList built on get/setAttribute("class")
	// would read that stale string and quietly drop every SetClass-applied
	// class on its next write; the facade test observes the staleness from both
	// sides to pin this trap. One token per call (DOM's variadic add(...tokens)
	// is not implemented -- call twice).
	switch (Magic)
	{
		case ClassAdd:
			Element->SetClass(Name, true);
			return JS_UNDEFINED;

		case ClassRemove:
			Element->SetClass(Name, false);
			return JS_UNDEFINED;

		case ClassToggle:
		{
			const bool bNowSet = !Element->IsClassSet(Name);
			Element->SetClass(Name, bNowSet);
			return JS_NewBool(Ctx, bNowSet);	// DOM toggle: the NEW state
		}

		case ClassContains:
			return JS_NewBool(Ctx, Element->IsClassSet(Name));

		default:
			return JS_UNDEFINED;
	}
}

JSValue FVaCuusJsViewContext::StyleGetterThunk(JSContext* Ctx, JSValueConst This)
{
	using namespace VaCuusJsDomInternal;

	FVaCuusJsViewContext* Self = static_cast<FVaCuusJsViewContext*>(JS_GetContextOpaque(Ctx));
	check(Self != nullptr);

	if (!JS_IsFunction(Ctx, Self->StyleFactory))
	{
		return JS_NULL;	   // the factory failed to compile at context birth (heap at cap)
	}

	// Like classList: a fresh proxy per access, three bound functions carrying
	// the wrapper, dead-checks inside the C ops -- so even `dead.style.width`
	// reads null instead of throwing.
	JSValueConst Data[1] = {This};
	JSValue GetFn = JS_NewCFunctionData2(Ctx, &FVaCuusJsViewContext::StyleOpThunk, "get", 1, StyleGet, 1, Data);
	JSValue SetFn = JS_NewCFunctionData2(Ctx, &FVaCuusJsViewContext::StyleOpThunk, "setProperty", 2, StyleSet, 1, Data);
	JSValue RemoveFn =
		JS_NewCFunctionData2(Ctx, &FVaCuusJsViewContext::StyleOpThunk, "removeProperty", 1, StyleRemove, 1, Data);

	JSValueConst Args[3] = {GetFn, SetFn, RemoveFn};
	JSValue Proxy = JS_Call(Ctx, Self->StyleFactory, JS_UNDEFINED, 3, Args);

	JS_FreeValue(Ctx, GetFn);
	JS_FreeValue(Ctx, SetFn);
	JS_FreeValue(Ctx, RemoveFn);
	return Proxy;
}

JSValue FVaCuusJsViewContext::StyleOpThunk(
	JSContext* Ctx, JSValueConst /*This*/, int Argc, JSValueConst* Argv, int Magic, JSValueConst* FuncData)
{
	using namespace VaCuusJsDomInternal;

	FVaCuusJsViewContext* Self = static_cast<FVaCuusJsViewContext*>(JS_GetContextOpaque(Ctx));
	check(Self != nullptr);

	Rml::Element* Element = Self->GetLiveElement(FuncData[0]);
	if (Element == nullptr || Argc < 1)
	{
		switch (Magic)
		{
			case StyleGet:
				return JS_NULL;
			case StyleSet:
				return JS_FALSE;
			default:
				return JS_UNDEFINED;
		}
	}

	Rml::String Name;
	if (!ToRmlString(Ctx, Argv[0], Name))
	{
		return JS_EXCEPTION;
	}

	switch (Magic)
	{
		case StyleGet:
		{
			// COPIED AT ONCE, and that is the whole contract: the returned
			// Property* "is invalidated on any following call into RmlUi, please
			// copy by value" (Element.h:187-193) -- ToString() into an owned
			// string is the first and only use of the pointer. A VALID property
			// name always answers (local -> stylesheet -> inherited -> default,
			// per the same doc block); an UNKNOWN name answers nullptr -> null.
			const Rml::Property* Property = Element->GetProperty(Name);
			if (Property == nullptr)
			{
				return JS_NULL;
			}
			const Rml::String Copied = Property->ToString();
			return NewRmlString(Ctx, Copied);
		}

		case StyleSet:
		{
			if (Argc < 2)
			{
				return JS_FALSE;
			}
			Rml::String Value;
			if (!ToRmlString(Ctx, Argv[1], Value))
			{
				return JS_EXCEPTION;
			}
			// The bool is PARSE SUCCESS (Element.h:176-177); a failure also logs
			// RmlUi's own "Syntax error parsing inline property declaration"
			// warning (Element.cpp:591-598). Plain assignment through the proxy
			// discards this bool by design -- setProperty() is the spelling that
			// keeps it.
			return JS_NewBool(Ctx, Element->SetProperty(Name, Value));
		}

		case StyleRemove:
			// Longhands, `--custom`, and shorthand expansion are all
			// RemoveProperty's own business (Element.cpp:620-646).
			Element->RemoveProperty(Name);
			return JS_UNDEFINED;

		default:
			return JS_UNDEFINED;
	}
}
