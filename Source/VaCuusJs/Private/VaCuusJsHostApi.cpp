// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

/*
 * The `vacuus.*` host API (M4 Task 9, spec 3.11): emit, model().get(), the view
 * getter and stats() -- the Tier 1 surface the spec's §3.11 lists, installed onto
 * the `vacuus` object at context birth by InstallGlobals. Reads come from the UI
 * shadow through VaCuusGameBridge (core owns the registry and the layouts);
 * writes do not exist here at all -- a two-way control's write is the router's
 * (spec 3.10), and vacuus.model deliberately mints no `set`.
 */

#include "VaCuusGameBridge.h"
#include "VaCuusJs.h"
#include "VaCuusJsValue.h"
#include "VaCuusJsViewContext.h"
#include "VaCuusStats.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>

namespace VaCuusJsHostApiInternal
{
/** JS string -> FString, exception-safe: false with the exception pending. */
static bool ToFString(JSContext* Ctx, JSValueConst Value, FString& Out)
{
	const char* Utf8 = JS_ToCString(Ctx, Value);
	if (Utf8 == nullptr)
	{
		return false;
	}
	Out = UTF8_TO_TCHAR(Utf8);
	JS_FreeCString(Ctx, Utf8);
	return true;
}

/**
 * JS -> tagged value: bool/number/string only, false = skip. The dispatchEvent
 * precedent verbatim (JsToVariant, VaCuusJsEvents.cpp) with the tagged wire type in
 * place of the Rml::Variant -- nested objects, arrays, functions and symbol-keyed
 * anything are SKIPPED, not stringified, because a silently flattened "[object
 * Object]" reaching Blueprint is a debugging session, not a payload.
 */
static bool ToHostValue(JSContext* Ctx, JSValueConst Value, FVaCuusJsValue& Out)
{
	if (JS_IsBool(Value))
	{
		Out = FVaCuusJsValue::MakeBool(JS_ToBool(Ctx, Value) > 0);
		return true;
	}
	if (JS_IsNumber(Value))
	{
		double Number = 0.0;
		JS_ToFloat64(Ctx, &Number, Value);
		Out = FVaCuusJsValue::MakeNumber(Number);
		return true;
	}
	if (JS_IsString(Value))
	{
		FString Text;
		if (!ToFString(Ctx, Value, Text))
		{
			// A failed read of a plain string is OOM; clear and skip rather than
			// abort the emit over one property.
			JS_FreeValue(Ctx, JS_GetException(Ctx));
			return false;
		}
		Out = FVaCuusJsValue::MakeString(MoveTemp(Text));
		return true;
	}
	return false;
}

/** Tagged value -> JS. Null is a real answer (the read surface's miss), mirroring `document === null`. */
static JSValue FromHostValue(JSContext* Ctx, const FVaCuusJsValue& Value)
{
	switch (Value.Kind)
	{
		case EVaCuusJsValueKind::Bool:
			return JS_NewBool(Ctx, Value.bBool);
		case EVaCuusJsValueKind::Number:
			return JS_NewFloat64(Ctx, Value.Number);
		case EVaCuusJsValueKind::String:
			return JS_NewString(Ctx, TCHAR_TO_UTF8(*Value.String));
		case EVaCuusJsValueKind::Null:
			break;
	}
	return JS_NULL;
}
}	 // namespace VaCuusJsHostApiInternal

void FVaCuusJsViewContext::InstallHostApi(JSValue Vacuus)
{
	// JS_SetPropertyStr takes ownership of every value handed to it (the
	// JSValue-parameter rule, quickjs.h:199-201) -- the InstallGlobals contract.
	JS_SetPropertyStr(Ctx, Vacuus, "emit", JS_NewCFunction(Ctx, &FVaCuusJsViewContext::EmitThunk, "emit", 2));
	JS_SetPropertyStr(Ctx, Vacuus, "model", JS_NewCFunction(Ctx, &FVaCuusJsViewContext::ModelThunk, "model", 1));
	JS_SetPropertyStr(Ctx, Vacuus, "stats", JS_NewCFunction(Ctx, &FVaCuusJsViewContext::StatsThunk, "stats", 0));

	// `view` is a GETTER, not a snapshot: id never changes, but width/height track the
	// context through resizes, and a property stamped at install time would lie after
	// the first one. JS_DefinePropertyGetSet frees the atom's refs it takes; the atom
	// itself is ours to free.
	JSAtom ViewAtom = JS_NewAtom(Ctx, "view");
	JS_DefinePropertyGetSet(Ctx, Vacuus, ViewAtom,
		JS_NewCFunction2(Ctx, reinterpret_cast<JSCFunction*>(&FVaCuusJsViewContext::ViewGetterThunk), "view", 0,
			JS_CFUNC_getter, 0),
		JS_UNDEFINED, JS_PROP_ENUMERABLE);
	JS_FreeAtom(Ctx, ViewAtom);
}

JSValue FVaCuusJsViewContext::EmitThunk(JSContext* Ctx, JSValueConst /*This*/, int Argc, JSValueConst* Argv)
{
	using namespace VaCuusJsHostApiInternal;

	FVaCuusJsViewContext* Self = GetSelfOrNull(Ctx);
	if (Self == nullptr)
	{
		return JS_FALSE;	// dead context (a removed view's pinned job): the bool-shaped no-op
	}

	if (Argc < 1 || !JS_IsString(Argv[0]))
	{
		return JS_ThrowTypeError(Ctx, "vacuus.emit(name, payload?) needs a string name");
	}

	FString Name;
	if (!ToFString(Ctx, Argv[0], Name))
	{
		return JS_EXCEPTION;
	}

	// The payload's OWN ENUMERABLE STRING properties, flat -- the dispatchEvent
	// conversion contract, one seat over (DispatchEventThunk's walk): bool/number/
	// string values cross, everything else is skipped.
	TArray<FVaCuusJsKeyValue> Payload;
	if (Argc >= 2 && JS_IsObject(Argv[1]))
	{
		JSPropertyEnum* Properties = nullptr;
		uint32_t NumProperties = 0;
		if (JS_GetOwnPropertyNames(Ctx, &Properties, &NumProperties, Argv[1], JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
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

			FVaCuusJsValue Converted;
			if (ToHostValue(Ctx, Value, Converted))
			{
				if (const char* Key = JS_AtomToCString(Ctx, Properties[Index].atom))
				{
					FVaCuusJsKeyValue& Pair = Payload.AddDefaulted_GetRef();
					Pair.Key = UTF8_TO_TCHAR(Key);
					Pair.Value = MoveTemp(Converted);
					JS_FreeCString(Ctx, Key);
				}
			}
			JS_FreeValue(Ctx, Value);
		}
		JS_FreePropertyEnum(Ctx, Properties, NumProperties);
	}

	// The return is the enqueue's own truth: false means dropped (queue full -- the
	// router logged the named diagnostic -- or no UI thread), so a script CAN notice
	// backpressure where it happens instead of wondering why the game went deaf.
	const bool bEnqueued = VaCuusGameBridge::EnqueueJsEvent(Self->ViewId, FName(*Name), MoveTemp(Payload));
	return JS_NewBool(Ctx, bEnqueued);
}

JSValue FVaCuusJsViewContext::ModelThunk(JSContext* Ctx, JSValueConst /*This*/, int Argc, JSValueConst* Argv)
{
	FVaCuusJsViewContext* Self = GetSelfOrNull(Ctx);
	if (Self == nullptr)
	{
		return JS_NULL;
	}

	if (Argc < 1 || !JS_IsString(Argv[0]))
	{
		return JS_ThrowTypeError(Ctx, "vacuus.model(name) needs a string name");
	}

	// A FRESH {name, get} per call, no identity and no caching -- vacuus.model('hud')
	// !== vacuus.model('hud'), documented. The name rides get's FuncData
	// (JS_NewCFunctionData dups it, the ClassListOpThunk pattern), so the object is
	// self-contained: existence does NOT validate the name -- resolution happens per
	// get(), against a registry whose binds may land after this call. get(path) on an
	// unbound name answers null with the read surface's one Warning per (model, path).
	JSValue GetFn = JS_NewCFunctionData(Ctx, &FVaCuusJsViewContext::ModelGetThunk, 1, 0, 1, &Argv[0]);
	if (JS_IsException(GetFn))
	{
		return GetFn;
	}

	JSValue Model = JS_NewObject(Ctx);
	JS_SetPropertyStr(Ctx, Model, "name", JS_DupValue(Ctx, Argv[0]));
	JS_SetPropertyStr(Ctx, Model, "get", GetFn);
	return Model;
}

JSValue FVaCuusJsViewContext::ModelGetThunk(
	JSContext* Ctx, JSValueConst /*This*/, int Argc, JSValueConst* Argv, int /*Magic*/, JSValueConst* FuncData)
{
	using namespace VaCuusJsHostApiInternal;

	FVaCuusJsViewContext* Self = GetSelfOrNull(Ctx);
	if (Self == nullptr)
	{
		return JS_NULL;
	}

	if (Argc < 1 || !JS_IsString(Argv[0]))
	{
		return JS_ThrowTypeError(Ctx, "get(path) needs a string path");
	}

	FString ModelName;
	FString Path;
	if (!ToFString(Ctx, FuncData[0], ModelName) || !ToFString(Ctx, Argv[0], Path))
	{
		return JS_EXCEPTION;
	}

	FVaCuusJsValue Value;
	if (!VaCuusGameBridge::ReadModelValue(Self->ViewId, FName(*ModelName), Path, Value))
	{
		return JS_NULL;
	}
	return FromHostValue(Ctx, Value);
}

JSValue FVaCuusJsViewContext::ViewGetterThunk(JSContext* Ctx, JSValueConst /*This*/)
{
	FVaCuusJsViewContext* Self = GetSelfOrNull(Ctx);
	if (Self == nullptr)
	{
		return JS_NULL;
	}

	// Width/height are the CONTEXT's dimensions -- the same numbers RmlUi lays out
	// against (Context::SetDimensions from the host's SetViewSize) -- reached through
	// the member BindDocument stashes, so a clobbered `document` global cannot redirect
	// the answer. 0x0 before a document is bound: honest, feature-testable, and exactly
	// the state a sizeless-but-alive UMG view is in before its first Slate tick.
	int32 Width = 0;
	int32 Height = 0;
	if (Self->CurrentDocument != nullptr)
	{
		if (const Rml::Context* Context = Self->CurrentDocument->GetContext())
		{
			const Rml::Vector2i Dimensions = Context->GetDimensions();
			Width = Dimensions.x;
			Height = Dimensions.y;
		}
	}

	JSValue View = JS_NewObject(Ctx);
	JS_SetPropertyStr(Ctx, View, "id", JS_NewUint32(Ctx, Self->ViewId));
	JS_SetPropertyStr(Ctx, View, "width", JS_NewInt32(Ctx, Width));
	JS_SetPropertyStr(Ctx, View, "height", JS_NewInt32(Ctx, Height));
	return View;
}

JSValue FVaCuusJsViewContext::StatsThunk(JSContext* Ctx, JSValueConst /*This*/, int /*Argc*/, JSValueConst* /*Argv*/)
{
	FVaCuusJsViewContext* Self = GetSelfOrNull(Ctx);
	if (Self == nullptr)
	{
		return JS_NULL;
	}

	// THE HONEST SUBSET (spec 3.11 "the demo shape"): last-frame Update and Record
	// wall-clock from the always-on store (FVaCuusPerfLog::GetLastSampleMs -- one
	// double per scope, written by the RAII timers whether or not the PerfLog cvar is
	// on), and fps from the interval between JsPump samples -- the one per-frame scope
	// guaranteed to run whenever JS exists to ask. All three are process-wide UI-frame
	// figures, not per-view ones: the scopes wrap the frame's phases across every view
	// (RunFrame), and inventing a per-view split here would report precision the
	// samplers do not have. Zeros until the scopes have run -- a probe-host test rig
	// never samples Update/Record, and stats() there honestly says 0.
	const double UpdateMs = FVaCuusPerfLog::GetLastSampleMs(FVaCuusPerfLog::Update);
	const double RecordMs = FVaCuusPerfLog::GetLastSampleMs(FVaCuusPerfLog::Record);
	const double IntervalSeconds = FVaCuusPerfLog::GetLastUIFrameIntervalSeconds();

	JSValue Stats = JS_NewObject(Ctx);
	JS_SetPropertyStr(Ctx, Stats, "updateMs", JS_NewFloat64(Ctx, UpdateMs));
	JS_SetPropertyStr(Ctx, Stats, "renderMs", JS_NewFloat64(Ctx, RecordMs));
	JS_SetPropertyStr(Ctx, Stats, "fps", JS_NewFloat64(Ctx, IntervalSeconds > 0.0 ? 1.0 / IntervalSeconds : 0.0));
	return Stats;
}
