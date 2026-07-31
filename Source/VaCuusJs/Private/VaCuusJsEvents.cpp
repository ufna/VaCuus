// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

/*
 * Events over the DOM facade (M4 Task 5, spec 3.9 / 2(g)): the JS-backed
 * listener with its three tolerated death orders, the per-dispatch event
 * object, add/remove/dispatchEvent, and the lazy half of the on*-attribute
 * path (the instancer itself lives with the host, VaCuusJsScriptHost.h).
 *
 * Split out of VaCuusJsDom.cpp for size only, same rules: everything here runs
 * on the thread that owns the host and drives RmlUi -- dispatch arrives from
 * inside the context's input processors (ProcessMouseButtonDown, ProcessKeyDown,
 * Update) or from a facade thunk, both of which already sit on that thread.
 */

#include "VaCuusJs.h"
#include "VaCuusJsDomHandle.h"
#include "VaCuusJsEventListener.h"
#include "VaCuusJsRuntime.h"
#include "VaCuusJsScriptHost.h"
#include "VaCuusJsViewContext.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Variant.h>

namespace VaCuusJsDomInternal
{
//~ Defined in VaCuusJsDom.cpp; a named namespace gives them external linkage,
//~ so this sibling TU of the same module declares rather than duplicates.
bool ToRmlString(JSContext* Ctx, JSValueConst Value, Rml::String& Out);
JSValue NewRmlString(JSContext* Ctx, const Rml::String& Value);
}	 // namespace VaCuusJsDomInternal

namespace VaCuusJsEventInternal
{
enum EEventOp : int32
{
	EventStopPropagation = 0,
	EventStopImmediate,
	EventPreventDefault
};

/**
 * Variant -> JS for event parameters. The convertible set is deliberately
 * small -- bool, the numeric kinds, string; everything else RmlUi puts in a
 * Dictionary (vectors, colours, the drag_element void*) reads as absent.
 * Documented Tier 1 surface: `ev.params` carries what a script can use.
 */
JSValue VariantToJs(JSContext* Ctx, const Rml::Variant& Value)
{
	using namespace VaCuusJsDomInternal;

	switch (Value.GetType())
	{
		case Rml::Variant::BOOL:
			return JS_NewBool(Ctx, Value.Get<bool>());

		case Rml::Variant::BYTE:
		case Rml::Variant::INT:
		case Rml::Variant::INT64:
		case Rml::Variant::UINT:
		case Rml::Variant::UINT64:
		case Rml::Variant::FLOAT:
		case Rml::Variant::DOUBLE:
			// One JS number type; Get<double> runs RmlUi's own type converter.
			return JS_NewFloat64(Ctx, Value.Get<double>());

		case Rml::Variant::STRING:
			return NewRmlString(Ctx, Value.GetReference<Rml::String>());

		default:
			return JS_UNDEFINED;
	}
}

/**
 * JS -> Variant for dispatchEvent's parameter object: string/number/bool only
 * (the documented conversion); false = skip this property. Numbers land as
 * DOUBLE Variants -- fine for custom payloads, and RmlUi's own readers convert.
 */
bool JsToVariant(JSContext* Ctx, JSValueConst Value, Rml::Variant& Out)
{
	using namespace VaCuusJsDomInternal;

	if (JS_IsBool(Value))
	{
		Out = JS_ToBool(Ctx, Value) > 0;
		return true;
	}
	if (JS_IsNumber(Value))
	{
		double Number = 0.0;
		JS_ToFloat64(Ctx, &Number, Value);
		Out = Number;
		return true;
	}
	if (JS_IsString(Value))
	{
		Rml::String Text;
		if (!ToRmlString(Ctx, Value, Text))
		{
			// A plain string cannot fail to stringify; treat the impossible as skip.
			JS_FreeValue(Ctx, JS_GetException(Ctx));
			return false;
		}
		Out = Text;
		return true;
	}
	return false;
}
}	 // namespace VaCuusJsEventInternal

// ---------------------------------------------------------------------------
// The event object
// ---------------------------------------------------------------------------

void VaCuusJsEventFinalizer(JSRuntime* /*Rt*/, JSValueConst Value)
{
	// Nothing but the heap struct: an event wrapper owns no RmlUi state (the
	// raw Event* was nulled by the ProcessEvent that built it, and was never
	// owned), so finalization is safe at any point the GC picks, including a
	// stashed event object dying frames later.
	JSClassID ClassId = 0;
	if (FVaCuusJsEventHandle* Handle = static_cast<FVaCuusJsEventHandle*>(JS_GetAnyOpaque(Value, &ClassId)))
	{
		delete Handle;
	}
}

void FVaCuusJsViewContext::InstallEventPrototype()
{
	using namespace VaCuusJsEventInternal;

	static const JSCFunctionListEntry GEventProtoEntries[] = {
		JS_CFUNC_MAGIC_DEF("stopPropagation", 0, FVaCuusJsViewContext::EventOpThunk, EventStopPropagation),
		JS_CFUNC_MAGIC_DEF("stopImmediatePropagation", 0, FVaCuusJsViewContext::EventOpThunk, EventStopImmediate),
		JS_CFUNC_MAGIC_DEF("preventDefault", 0, FVaCuusJsViewContext::EventOpThunk, EventPreventDefault),
	};

	JSValue EventProto = JS_NewObject(Ctx);
	JS_SetPropertyFunctionList(Ctx, EventProto, GEventProtoEntries, UE_ARRAY_COUNT(GEventProtoEntries));
	JS_SetClassProto(Ctx, Runtime.GetEventClassId(), EventProto);
}

JSValue FVaCuusJsViewContext::BuildEventObject(Rml::Event& Event)
{
	using namespace VaCuusJsDomInternal;
	using namespace VaCuusJsEventInternal;

	JSValue Object = JS_NewObjectClass(Ctx, Runtime.GetEventClassId());
	if (JS_IsException(Object))
	{
		return Object;	  // heap at the cap; the caller reports
	}
	JS_SetOpaque(Object, new FVaCuusJsEventHandle{&Event});

	// target/currentTarget go through the identity cache, which is what makes
	// `ev.target === document.getElementById(...)` hold for free.
	JSValue Target = WrapElement(Event.GetTargetElement());
	if (JS_IsException(Target))
	{
		JS_FreeValue(Ctx, Object);
		return Target;
	}
	JSValue Current = WrapElement(Event.GetCurrentElement());
	if (JS_IsException(Current))
	{
		JS_FreeValue(Ctx, Target);
		JS_FreeValue(Ctx, Object);
		return Current;
	}

	JS_SetPropertyStr(Ctx, Object, "type", NewRmlString(Ctx, Event.GetType()));
	JS_SetPropertyStr(Ctx, Object, "target", Target);		   // takes ownership
	JS_SetPropertyStr(Ctx, Object, "currentTarget", Current);  // takes ownership

	// eventPhase uses DOM's numbering (CAPTURING_PHASE/AT_TARGET/BUBBLING_PHASE
	// = 1/2/3), NOT RmlUi's bitmask (EventPhase {None, Capture=1, Target=2,
	// Bubble=4}, Event.h:15) -- a script porting DOM code compares against the
	// numbers it knows. Documented mapping: Bubble's 4 becomes 3.
	int32 Phase = 0;
	switch (Event.GetPhase())
	{
		case Rml::EventPhase::Capture:
			Phase = 1;
			break;
		case Rml::EventPhase::Target:
			Phase = 2;
			break;
		case Rml::EventPhase::Bubble:
			Phase = 3;
			break;
		default:
			break;
	}
	JS_SetPropertyStr(Ctx, Object, "eventPhase", JS_NewInt32(Ctx, Phase));

	// `ev.params`: EVERY convertible RmlUi parameter under its RmlUi name --
	// the generic surface dispatchEvent round-trips through.
	const Rml::Dictionary& Parameters = Event.GetParameters();
	JSValue Params = JS_NewObject(Ctx);
	for (const auto& Pair : Parameters)
	{
		JSValue Converted = VariantToJs(Ctx, Pair.second);
		if (!JS_IsUndefined(Converted))
		{
			JS_SetPropertyStr(Ctx, Params, Pair.first.c_str(), Converted);
		}
	}
	JS_SetPropertyStr(Ctx, Object, "params", Params);

	// THE DOM-ISH ALIASES, set only when the parameter exists on this event.
	// The mapping, verbatim from RmlUi's parameter generators:
	//   mouse_x/mouse_y/button   -> mouseX/mouseY/button   (Context.cpp:1535-1542;
	//     `button` only on button events, same rule as RmlUi's :1540-1541)
	//   key_identifier           -> keyIdentifier           (Context.cpp:1530-1533;
	//     a NUMERIC Rml::Input::KeyIdentifier, not DOM's key string -- RmlUi has
	//     no character-level key names, the text arrives via textinput)
	//   text                     -> text                    (Context.cpp:573)
	//   ctrl_key/shift_key/alt_key/meta_key -> ctrlKey/shiftKey/altKey/metaKey
	//     (Context.cpp:1544-1550 stores them as int 0/1; the aliases convert to
	//     real JS booleans, `params` keeps the raw numbers)
	struct FAlias
	{
		const char* RmlName;
		const char* JsName;
		bool bAsBool;
	};
	static const FAlias GAliases[] = {
		{"mouse_x", "mouseX", false},
		{"mouse_y", "mouseY", false},
		{"button", "button", false},
		{"key_identifier", "keyIdentifier", false},
		{"text", "text", false},
		{"ctrl_key", "ctrlKey", true},
		{"shift_key", "shiftKey", true},
		{"alt_key", "altKey", true},
		{"meta_key", "metaKey", true},
	};
	for (const FAlias& Alias : GAliases)
	{
		const auto It = Parameters.find(Alias.RmlName);
		if (It == Parameters.end())
		{
			continue;
		}
		JS_SetPropertyStr(Ctx, Object, Alias.JsName,
			Alias.bAsBool ? JS_NewBool(Ctx, It->second.Get<int>() != 0) : VariantToJs(Ctx, It->second));
	}

	return Object;
}

JSValue FVaCuusJsViewContext::EventOpThunk(
	JSContext* Ctx, JSValueConst This, int /*Argc*/, JSValueConst* /*Argv*/, int Magic)
{
	using namespace VaCuusJsEventInternal;

	FVaCuusJsViewContext* Self = static_cast<FVaCuusJsViewContext*>(JS_GetContextOpaque(Ctx));
	check(Self != nullptr);

	// Class-checked opaque (null on mismatch, quickjs.h:1045), then the
	// stashed-event check: past its dispatch the handle's Event was nulled, and
	// the stop methods no-op -- the dead-handle rule.
	FVaCuusJsEventHandle* Handle =
		static_cast<FVaCuusJsEventHandle*>(JS_GetOpaque(This, Self->Runtime.GetEventClassId()));
	if (Handle == nullptr || Handle->Event == nullptr)
	{
		return JS_UNDEFINED;
	}

	switch (Magic)
	{
		case EventStopPropagation:
			// No-op on a non-interruptible event, RmlUi's own rule (Event.h:54-55).
			Handle->Event->StopPropagation();
			break;

		case EventStopImmediate:
			Handle->Event->StopImmediatePropagation();
			break;

		case EventPreventDefault:
			// MAPS TO StopPropagation, and that is the closest analog RmlUi has:
			// there is no separate default-action veto -- default actions run
			// after the listener chain and are skipped once the event stopped
			// propagating (the dispatch loop breaks its default-action pass on
			// !event->IsPropagating(), EventDispatcher.cpp:173-185). A DOM-only
			// preventDefault (cancel the action, keep bubbling) cannot be
			// expressed; documented deviation.
			Handle->Event->StopPropagation();
			break;

		default:
			break;
	}
	return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Registry plumbing (the containers stay private to the context)
// ---------------------------------------------------------------------------

void FVaCuusJsViewContext::RegisterListener(const FVaCuusJsListenerKey& Key, FVaCuusJsEventListener* Listener)
{
	ListenerRegistry.Add(Key, Listener);
}

void FVaCuusJsViewContext::UnregisterListener(const FVaCuusJsListenerKey& Key)
{
	ListenerRegistry.Remove(Key);
}

void FVaCuusJsViewContext::AdoptAttributeListener(FVaCuusJsEventListener* Listener)
{
	AttributeListeners.Add(Listener);
}

void FVaCuusJsViewContext::DropAttributeListener(FVaCuusJsEventListener* Listener)
{
	AttributeListeners.Remove(Listener);
}

// ---------------------------------------------------------------------------
// FVaCuusJsEventListener
// ---------------------------------------------------------------------------

std::atomic<int32> FVaCuusJsEventListener::NumLiveShells{0};

FVaCuusJsEventListener::FVaCuusJsEventListener()
{
	NumLiveShells.fetch_add(1, std::memory_order_relaxed);
}

FVaCuusJsEventListener::~FVaCuusJsEventListener()
{
	NumLiveShells.fetch_sub(1, std::memory_order_relaxed);
}

FVaCuusJsEventListener* FVaCuusJsEventListener::CreateForFunction(
	FVaCuusJsViewContext& InContext, const FVaCuusJsListenerKey& InKey, JSValueConst InFn)
{
	FVaCuusJsEventListener* Listener = new FVaCuusJsEventListener();
	Listener->OwnerContext = &InContext;
	Listener->Key = InKey;
	Listener->Fn = JS_DupValue(InContext.GetContext(), InFn);
	InContext.GetRuntime().NoteListenerRefAcquired();
	InContext.RegisterListener(InKey, Listener);
	return Listener;
}

FVaCuusJsEventListener* FVaCuusJsEventListener::CreateForAttribute(
	FVaCuusJsScriptHost& InHost, const Rml::String& InSource)
{
	FVaCuusJsEventListener* Listener = new FVaCuusJsEventListener();
	Listener->bAttributeMode = true;
	Listener->Host = &InHost;
	Listener->AttributeSource = InSource;
	InHost.AddUnresolvedAttributeListener(Listener);
	return Listener;
}

void FVaCuusJsEventListener::OnAttach(Rml::Element* /*Element*/)
{
	// Deliberately empty. FUNCTION mode registered at construction (the creator
	// documents why); ATTRIBUTE mode registers nowhere until it resolves. Attach
	// itself cannot fail or duplicate: AttachEvent drops only entries equal
	// INCLUDING the listener pointer (EventDispatcher.cpp:40-52), and every
	// listener object here is attached exactly once.
}

void FVaCuusJsEventListener::OnDetach(Rml::Element* /*Element*/)
{
	// Fires on every death order exactly once per registration: DetachEvent
	// (removeEventListener / an on* attribute rewrite, EventDispatcher.cpp:54-62),
	// ~EventDispatcher on direct element destruction (Element.cpp:99 -> :112;
	// EventDispatcher.cpp:33-38), or the unload sweep's DetachAllEvents
	// (Context.cpp:1565-1566; EventDispatcher.cpp:64-73) -- after which the
	// destructor's own loop iterates an emptied list and cannot re-fire.
	if (!bNeutered)
	{
		ReleaseAndUnregister();
	}

	if (DispatchDepth > 0)
	{
		// THE SELF-REMOVAL DEFERRAL (the class comment's hazard): this call sits
		// below our own ProcessEvent -- a handler removed its element (or
		// removeEventListener'ed itself) mid-dispatch. The JS ref is already
		// released above (ProcessEvent holds its own dup); only the delete
		// waits for the outermost dispatch frame to exit.
		bDetachPending = true;
		return;
	}

	// A neutered shell reclaims itself touching nothing else -- on the shutdown
	// path both its context and the runtime are gone by now (spec 2(g)).
	delete this;
}

void FVaCuusJsEventListener::ReleaseAndUnregister()
{
	if (OwnerContext != nullptr)
	{
		if (!JS_IsUndefined(Fn))
		{
			JS_FreeValue(OwnerContext->GetContext(), Fn);
			Fn = JS_UNDEFINED;
			OwnerContext->GetRuntime().NoteListenerRefReleased();
		}
		if (bAttributeMode)
		{
			OwnerContext->DropAttributeListener(this);
		}
		else
		{
			OwnerContext->UnregisterListener(Key);
		}
		OwnerContext = nullptr;
	}
	else if (Host != nullptr)
	{
		// Unresolved attribute listener: no JS ref ever existed; just leave the
		// host's bookkeeping set.
		Host->RemoveUnresolvedAttributeListener(this);
		Host = nullptr;
	}
}

void FVaCuusJsEventListener::NeuterFromContext()
{
	// The context destructor's walk (death order (3)). The JSContext is still
	// alive HERE -- this runs before JS_FreeContext -- which is the last moment
	// this ref can legally be freed. The walk empties the containers itself;
	// nothing to unregister.
	// RESTORE-THE-BUG record (plan 5.2): with this release pair deleted --
	// refs freed in OnDetach only, the walk forgetting them -- orders (1)/(2)
	// stay green and order (3) leaks both function objects: the death-orders
	// test's gauge line reads "refs=2, shells=0 (baseline 0)" and the rig
	// teardown dies in the runtime destructor's live-byte checkf ("JS runtime
	// destroyed with 64904 bytes still live"). Both halves observed 2026-07-31.
	if (OwnerContext != nullptr && !JS_IsUndefined(Fn))
	{
		JS_FreeValue(OwnerContext->GetContext(), Fn);
		OwnerContext->GetRuntime().NoteListenerRefReleased();
	}
	Fn = JS_UNDEFINED;
	OwnerContext = nullptr;
	Host = nullptr;
	bNeutered = true;
}

void FVaCuusJsEventListener::NeuterFromHost()
{
	// Host shutdown, listener never fired: no ref, no context -- only the
	// dangling-host window to close. RmlUi's tree teardown reclaims the shell.
	checkf(JS_IsUndefined(Fn), TEXT("an unresolved attribute listener cannot hold a JS ref"));
	Host = nullptr;
	bNeutered = true;
}

void FVaCuusJsEventListener::ResolveAttribute(Rml::Element* Element, const Rml::String& TypeName)
{
	bResolveAttempted = true;
	if (Host == nullptr || Element == nullptr)
	{
		return;
	}

	// element -> owner document -> the host's bind-time document->view map.
	// GetOwnerDocument, never rmlui_dynamic_cast (the Task 4 finding: the
	// custom-RTTI statics duplicate across modules on Linux modular builds).
	Rml::ElementDocument* Document = Element->GetOwnerDocument();
	FVaCuusJsViewContext* Context = Document != nullptr ? Host->FindViewContextForDocument(Document) : nullptr;
	if (Context == nullptr || !Context->IsValid())
	{
		// ONE Warning, then inert -- the documented no-op for an on* attribute
		// on a document no JS view bound. The listener stays in the host's
		// unresolved set so Shutdown() can still close the dangling-host window.
		// GetAddress(false, false): tag#id.classes only -- the default appends
		// every ancestor as " < parent" (Element.cpp:316-319), noise when the
		// document URL is named anyway.
		UE_LOG(LogVaCuusJS, Warning,
			TEXT("on%s attribute on '%s' fired, but its document is not bound to any JS view; the snippet stays inert"),
			UTF8_TO_TCHAR(TypeName.c_str()), UTF8_TO_TCHAR(Element->GetAddress(false, false).c_str()));
		return;
	}

	// Adopt into the view FIRST: from here on this listener holds state of that
	// context (the compiled function) and must be visible to its neuter walk.
	Host->RemoveUnresolvedAttributeListener(this);
	Host = nullptr;
	OwnerContext = Context;
	Context->AdoptAttributeListener(this);

	// COMPILED LAZILY, at first fire, and not by preference: at instancing time
	// (OnAttributeChange -> Factory::InstanceEventListener, Element.cpp:1724-1749)
	// a parsed document's elements do not yet belong to any bound view -- the
	// document->view map is written at bind time, after LoadDocument returns --
	// so there is no context to compile against. First fire is the earliest
	// moment the route exists. The snippet becomes a FUNCTION BODY with the
	// DOM's implicit signature, function(event){...}, evaluated as a function
	// expression -- creation only, no user code runs here.
	JSContext* JsCtx = Context->GetContext();
	const Rml::String& SourceUrl = Document->GetSourceURL();
	const FString DiagnosticName = FString::Printf(TEXT("on%s attribute of '%s' in %s"),
		UTF8_TO_TCHAR(TypeName.c_str()), UTF8_TO_TCHAR(Element->GetAddress(false, false).c_str()),
		UTF8_TO_TCHAR(SourceUrl.c_str()));
	const Rml::String Wrapped = "(function(event){ " + AttributeSource + "\n})";
	const FTCHARToUTF8 NameUtf8(*DiagnosticName);

	JSValue Compiled = JS_Eval(JsCtx, Wrapped.c_str(), Wrapped.size(), NameUtf8.Get(), JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(Compiled) || !JS_IsFunction(JsCtx, Compiled))
	{
		if (JS_IsException(Compiled))
		{
			// THE NAMED ERROR PATH: exactly one Error, and the source name carries
			// attribute, element and document -- ReportException logs
			// "JS exception in '<DiagnosticName>': SyntaxError: ...". One-shot:
			// bResolveAttempted keeps every later fire silent.
			Context->GetRuntime().ReportException(JsCtx, *DiagnosticName);
		}
		else
		{
			JS_FreeValue(JsCtx, Compiled);	  // a snippet like "}) + ({" evaluating to a non-function
			UE_LOG(LogVaCuusJS, Error, TEXT("%s did not compile to a function; the handler stays inert"),
				*DiagnosticName);
		}
		return;
	}

	// The eval's return IS this listener's ref -- no extra dup.
	Fn = Compiled;
	Context->GetRuntime().NoteListenerRefAcquired();
}

void FVaCuusJsEventListener::ProcessEvent(Rml::Event& Event)
{
	// A neutered shell (order (3)) or a detach-pending one (a nested dispatch
	// collected us before a mid-dispatch removal) runs nothing.
	if (bNeutered || bDetachPending)
	{
		return;
	}

	if (bAttributeMode && !bResolveAttempted)
	{
		ResolveAttribute(Event.GetCurrentElement(), Event.GetType());
	}

	// Pin the context on the STACK: OnDetach may null OwnerContext below us
	// (self-removal), and the post-call cleanup still needs it -- the context
	// object itself cannot die mid-dispatch (teardown never runs inside
	// Context::Update / the input processors).
	FVaCuusJsViewContext* Context = OwnerContext;
	if (Context == nullptr)
	{
		return;	   // unresolved/unbound/failed attribute listener: inert
	}

	JSContext* JsCtx = Context->GetContext();
	if (!JS_IsFunction(JsCtx, Fn))
	{
		return;
	}

	// Depth, not a flag: a handler can dispatchEvent() into a nested dispatch
	// that collects this same listener again.
	++DispatchDepth;

	// Dup BEFORE the call -- the handler may remove its own element or
	// removeEventListener itself, and OnDetach then frees the stored Fn while
	// the call is in flight (the timer pass's dup-before-call rule).
	const JSValue FnLocal = JS_DupValue(JsCtx, Fn);

	FVaCuusJsRuntime& JsRuntime = Context->GetRuntime();
	const FString GuardName = FString::Printf(TEXT("%s listener"), UTF8_TO_TCHAR(Event.GetType().c_str()));

	JSValue EventObject = Context->BuildEventObject(Event);
	if (JS_IsException(EventObject))
	{
		JsRuntime.ReportException(JsCtx, *GuardName);
		EventObject = JS_UNDEFINED;
	}

	// `this` inside a handler is the currentTarget, the DOM contract.
	JSValue ThisObject = Context->WrapElement(Event.GetCurrentElement());
	if (JS_IsException(ThisObject))
	{
		JsRuntime.ReportException(JsCtx, *GuardName);
		ThisObject = JS_UNDEFINED;
	}

	JSValue Ret;
	{
		FVaCuusJsEntryGuard Guard(JsRuntime, JsCtx, *GuardName);
		JSValueConst Args[1] = {EventObject};
		Ret = JS_Call(JsCtx, FnLocal, ThisObject, 1, Args);
	}
	// A throw is consumed here and never skips a sibling: the dispatcher's loop
	// (EventDispatcher.cpp:148-171) knows nothing of it, and the next listener's
	// ProcessEvent starts with a clean context.
	if (JS_IsException(Ret))
	{
		JsRuntime.ReportException(JsCtx, *GuardName);
	}
	JS_FreeValue(JsCtx, Ret);

	// NULL THE RAW EVENT before the Rml::Event leaves scope: the EventPtr dies
	// when DispatchEvent returns (instanced at EventDispatcher.cpp:141, gone by
	// :189), and a stashed `ev` must go dead, not dangling.
	if (FVaCuusJsEventHandle* Handle =
			static_cast<FVaCuusJsEventHandle*>(JS_GetOpaque(EventObject, JsRuntime.GetEventClassId())))
	{
		Handle->Event = nullptr;
	}
	JS_FreeValue(JsCtx, EventObject);
	JS_FreeValue(JsCtx, ThisObject);
	JS_FreeValue(JsCtx, FnLocal);

	if (--DispatchDepth == 0 && bDetachPending)
	{
		// The deferred half of OnDetach's mid-dispatch case. RmlUi tolerates the
		// death mid-chain: dispatch holds ObserverPtrs to element and listener
		// and re-checks both before every submit (EventDispatcher.cpp:81-99,
		// :163-166).
		delete this;
	}
}

// ---------------------------------------------------------------------------
// Element prototype: the listener surface
// ---------------------------------------------------------------------------

JSValue FVaCuusJsViewContext::AddEventListenerThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv)
{
	using namespace VaCuusJsDomInternal;

	FVaCuusJsViewContext* Self = static_cast<FVaCuusJsViewContext*>(JS_GetContextOpaque(Ctx));
	check(Self != nullptr);

	Rml::Element* Element = Self->GetLiveElement(This);
	if (Element == nullptr || Argc < 2)
	{
		return JS_UNDEFINED;	// dead handle: silent no-op, the house rule
	}
	if (!JS_IsFunction(Ctx, Argv[1]))
	{
		// No handleEvent objects, no string handlers in Tier 1 -- the timer
		// contract's shape: a non-function is a TypeError, not a silent drop.
		return JS_ThrowTypeError(Ctx, "addEventListener: listener must be a function");
	}

	Rml::String Type;
	if (!ToRmlString(Ctx, Argv[0], Type))
	{
		return JS_EXCEPTION;
	}

	// The REAL capture flag (Element.h:471-482 keeps capture and bubble as
	// distinct phases). Truthiness of the third argument; DOM's options object
	// is not parsed -- an object here reads as capture=true. Documented.
	const bool bCapture = Argc >= 3 && JS_ToBool(Ctx, Argv[2]) > 0;

	FVaCuusJsListenerKey Key;
	Key.Element = Element;
	Key.Type = UTF8_TO_TCHAR(Type.c_str());
	Key.FnPtr = JS_VALUE_GET_PTR(Argv[1]);
	Key.bCapture = bCapture;

	// The DOM duplicate rule: a second registration of the same (type, fn,
	// capture) on the same element is ignored -- one entry, one fire.
	if (Self->ListenerRegistry.Contains(Key))
	{
		return JS_UNDEFINED;
	}

	FVaCuusJsEventListener* Listener = FVaCuusJsEventListener::CreateForFunction(*Self, Key, Argv[1]);

	// The string overload resolves the type through the PROCESS-GLOBAL event
	// registry, inserting on first sight (Element.cpp:1236-1240 ->
	// EventSpecification.cpp:135-144). Ids never unregister until Rml::Shutdown
	// and cap at 65535 (ID.h:242-249) -- a custom type one view's script names
	// occupies a slot every context in the process sees. Benign, but global.
	Element->AddEventListener(Type, Listener, bCapture);
	return JS_UNDEFINED;
}

JSValue FVaCuusJsViewContext::RemoveEventListenerThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv)
{
	using namespace VaCuusJsDomInternal;

	FVaCuusJsViewContext* Self = static_cast<FVaCuusJsViewContext*>(JS_GetContextOpaque(Ctx));
	check(Self != nullptr);

	Rml::Element* Element = Self->GetLiveElement(This);
	if (Element == nullptr || Argc < 2 || !JS_IsFunction(Ctx, Argv[1]))
	{
		return JS_UNDEFINED;	// only functions ever registered; anything else cannot match
	}

	Rml::String Type;
	if (!ToRmlString(Ctx, Argv[0], Type))
	{
		return JS_EXCEPTION;
	}
	const bool bCapture = Argc >= 3 && JS_ToBool(Ctx, Argv[2]) > 0;

	FVaCuusJsListenerKey Key;
	Key.Element = Element;
	Key.Type = UTF8_TO_TCHAR(Type.c_str());
	Key.FnPtr = JS_VALUE_GET_PTR(Argv[1]);
	Key.bCapture = bCapture;

	// Exact-match or no-op -- which is precisely how a capture-flag mismatch
	// fails to remove (the pinned test): the registry key carries the flag, and
	// so does RmlUi's own entry identity (DetachEvent matches (id, listener,
	// phase), EventDispatcher.cpp:54-58).
	FVaCuusJsEventListener* Found = Self->ListenerRegistry.FindRef(Key);
	if (Found == nullptr)
	{
		return JS_UNDEFINED;
	}

	// Same phase flag on the RmlUi side (Element.h:480-482); OnDetach then
	// releases the ref, removes the registry entry, and reclaims the object --
	// or defers the reclaim if this removal happens mid-dispatch.
	Element->RemoveEventListener(Type, Found, bCapture);
	return JS_UNDEFINED;
}

JSValue FVaCuusJsViewContext::DispatchEventThunk(JSContext* Ctx, JSValueConst This, int Argc, JSValueConst* Argv)
{
	using namespace VaCuusJsDomInternal;
	using namespace VaCuusJsEventInternal;

	FVaCuusJsViewContext* Self = static_cast<FVaCuusJsViewContext*>(JS_GetContextOpaque(Ctx));
	check(Self != nullptr);

	Rml::Element* Element = Self->GetLiveElement(This);
	if (Element == nullptr || Argc < 1)
	{
		return JS_FALSE;	// dead handle: the bool-shaped no-op (dispatch "did not propagate")
	}

	Rml::String Type;
	if (!ToRmlString(Ctx, Argv[0], Type))
	{
		return JS_EXCEPTION;
	}

	// The parameter object's OWN ENUMERABLE STRING properties become the
	// Rml::Dictionary, values converted by JsToVariant (string/number/bool;
	// anything else -- nested objects, symbols keys, functions -- is skipped).
	Rml::Dictionary Parameters;
	if (Argc >= 2 && JS_IsObject(Argv[1]))
	{
		JSPropertyEnum* Properties = nullptr;
		uint32_t NumProperties = 0;
		if (JS_GetOwnPropertyNames(
				Ctx, &Properties, &NumProperties, Argv[1], JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
		{
			return JS_EXCEPTION;
		}
		for (uint32_t Index = 0; Index < NumProperties; ++Index)
		{
			JSValue Value = JS_GetProperty(Ctx, Argv[1], Properties[Index].atom);
			if (JS_IsException(Value))
			{
				// A throwing getter is the script's throw; propagate it.
				JS_FreePropertyEnum(Ctx, Properties, NumProperties);
				return JS_EXCEPTION;
			}
			Rml::Variant Converted;
			if (JsToVariant(Ctx, Value, Converted))
			{
				if (const char* Name = JS_AtomToCString(Ctx, Properties[Index].atom))
				{
					Parameters[Name] = Converted;
					JS_FreeCString(Ctx, Name);
				}
			}
			JS_FreeValue(Ctx, Value);
		}
		JS_FreePropertyEnum(Ctx, Properties, NumProperties);
	}

	// The two-argument overload (Element.cpp:1258-1263): an UNKNOWN type
	// auto-registers with interruptible=true, bubbles=true, no default action
	// (EventSpecification.cpp:125-133) -- taking one of the process-global id
	// slots forever (the addEventListener comment has the arithmetic). The
	// return is RmlUi's "still propagating"; with preventDefault mapped onto
	// stopPropagation this is exactly DOM's "false if canceled".
	const bool bPropagating = Element->DispatchEvent(Type, Parameters);
	return JS_NewBool(Ctx, bPropagating);
}
