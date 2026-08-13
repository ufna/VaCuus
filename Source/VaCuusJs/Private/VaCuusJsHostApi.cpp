// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

/*
 * The `vacuus.*` host API (M4 Task 9, spec 3.11): emit, model().get(), the view
 * getter and stats() -- the Tier 1 surface the spec's §3.11 lists, installed onto
 * the `vacuus` object at context birth by InstallGlobals -- plus translate()
 * (M5 Task 8, spec §2(l)), the localization hook M4 deferred. Reads come from
 * the UI shadow through VaCuusGameBridge (core owns the registry and the
 * layouts) or, for translate, from the installed FVaCuusTranslationRegistry
 * snapshot; writes do not exist here at all -- a two-way control's write is the
 * router's (spec 3.10), and vacuus.model deliberately mints no `set`.
 */

#include "VaCuusGameBridge.h"
#include "VaCuusJs.h"
#include "VaCuusJsValue.h"
#include "VaCuusJsViewContext.h"
#include "VaCuusStats.h"
#include "VaCuusTranslation.h"

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

void FVaCuusJsViewContext::CallFunction(const FString& FunctionPath, TArrayView<const FVaCuusJsValue> Args)
{
	using namespace VaCuusJsHostApiInternal;

	if (Ctx == nullptr)
	{
		return;
	}

	TArray<FString> Segments;
	FunctionPath.ParseIntoArray(Segments, TEXT("."), /*InCullEmpty=*/true);
	if (Segments.Num() == 0)
	{
		UE_LOG(LogVaCuusJS, Warning, TEXT("CallJs on view %u: the function path is empty"), ViewId);
		return;
	}

	// WALK THE PATH, HOLDING THE OWNER. `this` inside the callee must be the object the last
	// segment lives on -- `vacuus.onFreeze` has to see `this === vacuus`, which is what the
	// interpolated call it replaces gave it for free. Owner starts as globalThis so a
	// single-segment path lands on the same `this` a bare `foo(x)` would.
	JSValue Owner = JS_GetGlobalObject(Ctx);
	JSValue Callee = JS_UNDEFINED;

	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		if (!JS_IsObject(Owner))
		{
			// A non-object mid-path: `a.b.c` where `a.b` is a number. Not an error -- see the
			// missing-function contract on the declaration -- but it must not be read as "the
			// call happened".
			UE_LOG(LogVaCuusJS, Warning, TEXT("CallJs('%s') on view %u: '%s' is not an object, nothing called"),
				*FunctionPath, ViewId, *FString::Join(TArrayView<const FString>(Segments.GetData(), Index), TEXT(".")));
			JS_FreeValue(Ctx, Owner);
			return;
		}

		const FTCHARToUTF8 SegmentUtf8(*Segments[Index]);
		Callee = JS_GetPropertyStr(Ctx, Owner, SegmentUtf8.Get());
		if (JS_IsException(Callee))
		{
			// A getter on the path threw. Consumed here, like every other JS throw crossing
			// this seam, and reported through the runtime's sink rather than swallowed.
			Runtime.ReportException(Ctx, *FunctionPath);
			JS_FreeValue(Ctx, Owner);
			return;
		}

		if (Index + 1 < Segments.Num())
		{
			JS_FreeValue(Ctx, Owner);
			Owner = Callee;
			Callee = JS_UNDEFINED;
		}
	}

	if (!JS_IsFunction(Ctx, Callee))
	{
		// THE GUARD THE OLD IDIOM HAND-WROTE, in one place instead of at every call site: a
		// document that never registered the callback, or failed to load at all, gets a named
		// Warning rather than a JS exception for a call its author could not have prevented.
		UE_LOG(LogVaCuusJS, Warning, TEXT("CallJs('%s') on view %u: not a function, nothing called"),
			*FunctionPath, ViewId);
		JS_FreeValue(Ctx, Callee);
		JS_FreeValue(Ctx, Owner);
		return;
	}

	// THE ARGUMENTS NEVER BECOME TEXT. Each is one JSValue built by the same marshaller
	// `vacuus.emit` uses in the other direction, so a string containing quotes, backslashes,
	// newlines or `);` is a string -- there is no parser downstream of here for it to escape
	// into. That is the whole of VaCuus-asv's second and larger reason.
	TArray<JSValue, TInlineAllocator<4>> JsArgs;
	JsArgs.Reserve(Args.Num());
	for (const FVaCuusJsValue& Arg : Args)
	{
		JsArgs.Add(FromHostValue(Ctx, Arg));
	}

	JSValue Ret;
	{
		// The entry-guard contract (VaCuusJsRuntime.h): wraps the JS call ONLY, and closes
		// before the exception is consumed -- Eval's shape verbatim.
		FVaCuusJsEntryGuard Guard(Runtime, Ctx, *FunctionPath);
		Ret = JS_Call(Ctx, Callee, Owner, JsArgs.Num(), JsArgs.GetData());
	}
	if (JS_IsException(Ret))
	{
		Runtime.ReportException(Ctx, *FunctionPath);
	}

	JS_FreeValue(Ctx, Ret);
	for (JSValue& Arg : JsArgs)
	{
		// JS_Call does NOT take ownership of its argv (the JSValueConst rule, quickjs.h:199-201),
		// unlike JS_SetPropertyStr a few lines below. Ours to free, all of them, call or throw.
		JS_FreeValue(Ctx, Arg);
	}
	JS_FreeValue(Ctx, Callee);
	JS_FreeValue(Ctx, Owner);
}

void FVaCuusJsViewContext::InstallHostApi(JSValue Vacuus)
{
	// JS_SetPropertyStr takes ownership of every value handed to it (the
	// JSValue-parameter rule, quickjs.h:199-201) -- the InstallGlobals contract.
	JS_SetPropertyStr(Ctx, Vacuus, "emit", JS_NewCFunction(Ctx, &FVaCuusJsViewContext::EmitThunk, "emit", 2));
	JS_SetPropertyStr(Ctx, Vacuus, "model", JS_NewCFunction(Ctx, &FVaCuusJsViewContext::ModelThunk, "model", 1));
	JS_SetPropertyStr(Ctx, Vacuus, "stats", JS_NewCFunction(Ctx, &FVaCuusJsViewContext::StatsThunk, "stats", 0));
	JS_SetPropertyStr(Ctx, Vacuus, "textureStats",
		JS_NewCFunction(Ctx, &FVaCuusJsViewContext::TextureStatsThunk, "textureStats", 0));
	JS_SetPropertyStr(
		Ctx, Vacuus, "translate", JS_NewCFunction(Ctx, &FVaCuusJsViewContext::TranslateThunk, "translate", 2));

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

JSValue FVaCuusJsViewContext::TranslateThunk(JSContext* Ctx, JSValueConst /*This*/, int Argc, JSValueConst* Argv)
{
	using namespace VaCuusJsHostApiInternal;

	FVaCuusJsViewContext* Self = GetSelfOrNull(Ctx);
	if (Self == nullptr)
	{
		return JS_NULL;	   // dead context: the null-shaped no-op, the house rule
	}

	if (Argc < 1 || !JS_IsString(Argv[0]))
	{
		return JS_ThrowTypeError(Ctx, "vacuus.translate(key, params?) needs a string key");
	}

	FString Key;
	if (!ToFString(Ctx, Argv[0], Key))
	{
		return JS_EXCEPTION;
	}

	// THE SNAPSHOT LOOKUP (spec §2(l)): the game's "handler" is the table it pushed
	// (UVaCuusSubsystem::SetTranslationTable -> the drain's InstallSnapshot); this
	// call never leaves the UI thread and never blocks. Identity on a key miss with
	// a table present is quiet — the table is the contract. The one NAMED refusal
	// is no table at all: identity plus one latched Verbose per context, because a
	// script asking for localization in a game that never pushed a table is a
	// wiring gap that is otherwise perfectly silent (the string still shows).
	FString Resolved;
	if (!FVaCuusTranslationRegistry::TranslateKey(Key, Resolved))
	{
		Resolved = Key;
		if (!FVaCuusTranslationRegistry::GetInstalledSnapshot().IsValid() && !Self->bTranslateNoTableWarned)
		{
			Self->bTranslateNoTableWarned = true;
			UE_LOG(LogVaCuusJS, Verbose,
				TEXT("vacuus.translate('%s') on view %u: no translation table has been published ")
				TEXT("(UVaCuusSubsystem::SetTranslationTable); keys pass through as identity. Reported once per context"),
				*Key, Self->ViewId);
		}
	}

	// PARAMS SUBSTITUTION, documented format: every own enumerable string property
	// {name: value} of params replaces the literal token `{name}` in the resolved
	// string; values cross by the emit contract (bool/number/string; the rest are
	// skipped, not stringified). SINGLE braces on purpose: RmlUi's data-binding
	// scanner arms on `{{` only (XMLParseTools.cpp:152-155, driven per character
	// from Factory.cpp:352), so a translated string carrying an unsubstituted
	// `{name}` renders literally instead of becoming an expression. Substitution
	// runs on the identity string too — a params-bearing key works before any
	// table exists.
	//
	// Params are COLLECTED FIRST, then substituted in ONE left-to-right pass over
	// the pattern that never rescans appended text. The per-param ReplaceInline
	// loop this replaces mutated the same evolving string once per property:
	// order-dependent, and a param VALUE containing another param's token was
	// rewritten on a later iteration — translate('{killer} downed {victim}',
	// {killer: 'xX{victim}Xx', victim: 'Moth'}) produced 'xXMothXx downed Moth',
	// and killer/victim are exactly the user-data shape the killfeed feeds here.
	// Under the single pass property order is irrelevant and {n: '{n}'} is
	// trivially safe (VaCuus.Js.Translate carries the adversarial case).
	if (Argc >= 2 && JS_IsObject(Argv[1]))
	{
		JSPropertyEnum* Properties = nullptr;
		uint32_t NumProperties = 0;
		if (JS_GetOwnPropertyNames(Ctx, &Properties, &NumProperties, Argv[1], JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
		{
			return JS_EXCEPTION;
		}
		TMap<FString, FString> Params;
		Params.Reserve(NumProperties);
		for (uint32_t Index = 0; Index < NumProperties; ++Index)
		{
			JSValue Value = JS_GetProperty(Ctx, Argv[1], Properties[Index].atom);
			if (JS_IsException(Value))
			{
				JS_FreePropertyEnum(Ctx, Properties, NumProperties);
				return JS_EXCEPTION;
			}

			FVaCuusJsValue Converted;
			if (ToHostValue(Ctx, Value, Converted))
			{
				if (const char* Name = JS_AtomToCString(Ctx, Properties[Index].atom))
				{
					FString Text;
					switch (Converted.Kind)
					{
						case EVaCuusJsValueKind::Bool:
							Text = Converted.bBool ? TEXT("true") : TEXT("false");
							break;
						case EVaCuusJsValueKind::Number:
							// Integral numbers print without a decimal tail ("5", not
							// "5.000000") — the value a translator's "{count} kills"
							// expects; everything else takes %g's compact form.
							Text = FMath::IsFinite(Converted.Number) &&
										   Converted.Number == FMath::FloorToDouble(Converted.Number) &&
										   FMath::Abs(Converted.Number) < 9.0e15
									   ? FString::Printf(TEXT("%lld"), static_cast<int64>(Converted.Number))
									   : FString::Printf(TEXT("%g"), Converted.Number);
							break;
						case EVaCuusJsValueKind::String:
							Text = MoveTemp(Converted.String);
							break;
						case EVaCuusJsValueKind::Null:
							break;
					}
					Params.Add(UTF8_TO_TCHAR(Name), MoveTemp(Text));
					JS_FreeCString(Ctx, Name);
				}
			}
			JS_FreeValue(Ctx, Value);
		}
		JS_FreePropertyEnum(Ctx, Properties, NumProperties);

		// THE ONE PASS: scan for '{'; a token ends at the first '}' (an inner '{'
		// before it restarts the scan, so "{a{b}" keeps "{a" literal and still
		// substitutes {b} — the ReplaceInline behavior for that input). A known
		// name appends its value; an unknown token, or a dangling '{', appends its
		// own characters unchanged — never a rescan of what was appended.
		FString Output;
		Output.Reserve(Resolved.Len());
		const TCHAR* Chars = *Resolved;
		const int32 Len = Resolved.Len();
		int32 Pos = 0;
		while (Pos < Len)
		{
			if (Chars[Pos] != TEXT('{'))
			{
				Output.AppendChar(Chars[Pos++]);
				continue;
			}
			int32 Close = Pos + 1;
			while (Close < Len && Chars[Close] != TEXT('}') && Chars[Close] != TEXT('{'))
			{
				++Close;
			}
			if (Close >= Len || Chars[Close] == TEXT('{'))
			{
				// No token here: everything up to the inner '{' (or the end) is literal.
				Output.AppendChars(Chars + Pos, Close - Pos);
				Pos = Close;
				continue;
			}
			const FString TokenName = FString::ConstructFromPtrSize(Chars + Pos + 1, Close - Pos - 1);
			if (const FString* Substitution = Params.Find(TokenName))
			{
				Output += *Substitution;
			}
			else
			{
				Output.AppendChars(Chars + Pos, Close - Pos + 1);
			}
			Pos = Close + 1;
		}
		Resolved = MoveTemp(Output);
	}

	return JS_NewString(Ctx, TCHAR_TO_UTF8(*Resolved));
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

/**
 * `vacuus.textureStats()` — what the UI is holding in texture memory (VaCuus-dqs.1).
 *
 * ITS OWN CALL RATHER THAN TWO MORE FIELDS ON stats(). That one is per-FRAME wall clock from
 * the sampler store and is read every frame by anything drawing a debug overlay; this is a
 * MEMORY figure that only moves when a document loads or drops an image. Folding them together
 * would invite a per-frame reader to think the texture numbers were per-frame too.
 *
 * A PUBLISHED SNAPSHOT, NOT A LIVE WALK, and it could not be otherwise. The truth lives in the
 * render thread's texture maps, and the only way to read those from here is
 * FlushRenderingCommands() — which would stall the UI thread on the renderer once per call. So
 * the render thread publishes a total whenever a replayed buffer actually changed a texture set
 * (FVaCuusReplayRenderer::PublishCensus) and this reads the last one, lock-free. It can be one
 * frame stale; it cannot be wrong about what it saw.
 *
 * `bytes` IS THE LOGICAL FOOTPRINT — extent x format block bytes. The RHI may pad row pitch and
 * nothing on this side of the bridge can see that. Documented in vacuus.d.ts, because a JS
 * author reading a number called `bytes` will otherwise take it for VRAM.
 */
JSValue FVaCuusJsViewContext::TextureStatsThunk(JSContext* Ctx, JSValueConst /*This*/, int /*Argc*/, JSValueConst* /*Argv*/)
{
	FVaCuusJsViewContext* Self = GetSelfOrNull(Ctx);
	if (Self == nullptr)
	{
		return JS_NULL;	   // dead context, the same shape stats() answers with
	}

	// PROCESS-WIDE, like stats() and for the same reason: the published total sums every live
	// view, and RmlUi shares a file texture by SOURCE across every document in a view anyway, so
	// a per-view split would report precision the mechanism does not have.
	JSValue Out = JS_NewObject(Ctx);
	JS_SetPropertyStr(Ctx, Out, "count", JS_NewInt32(Ctx, FVaCuusPerfLog::GetResidentTextureCount()));
	// Float64: a byte count passes 2^31 at 2 GiB, which a texture budget reaches, and JS has no
	// integer type wider than the double this becomes anyway.
	JS_SetPropertyStr(Ctx, Out, "bytes", JS_NewFloat64(Ctx, double(FVaCuusPerfLog::GetResidentTextureBytes())));
	return Out;
}
