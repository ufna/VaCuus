// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusJs.h"

#include "VaCuusQuickJs.h"

#include <cstring>

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsVendoringSmokeTest, "VaCuus.Js.VendoringSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
JSValue Eval(JSContext* Ctx, const char* Source)
{
	return JS_Eval(Ctx, Source, std::strlen(Source), "<VaCuusJsSmoke>", JS_EVAL_TYPE_GLOBAL);
}

/**
 * Evals Source and renders the outcome as a string: the value's string
 * conversion on success, "exception: <message>" if the eval (or the
 * conversion) threw. Never leaves a pending exception behind.
 */
FString EvalToString(JSContext* Ctx, const char* Source, bool& bOutIsException)
{
	JSValue Value = Eval(Ctx, Source);
	bOutIsException = JS_IsException(Value);
	if (bOutIsException)
	{
		JS_FreeValue(Ctx, Value);
		Value = JS_GetException(Ctx);
	}

	FString Result;
	if (const char* Utf8 = JS_ToCString(Ctx, Value))
	{
		Result = FString(UTF8_TO_TCHAR(Utf8));
		JS_FreeCString(Ctx, Utf8);
	}
	else
	{
		// JS_ToCString itself threw (it returns NULL and sets the exception);
		// clear it so the next eval starts clean.
		JS_FreeValue(Ctx, JS_GetException(Ctx));
		Result = TEXT("<unstringifiable>");
	}
	JS_FreeValue(Ctx, Value);

	if (bOutIsException)
	{
		Result = FString::Printf(TEXT("exception: %s"), *Result);
	}
	return Result;
}
}	 // namespace

/**
 * Pure-library vendoring smoke: runtime + context created, used and destroyed
 * on the automation (game) thread, no UI thread involved. That is legal by
 * quickjs's own contract: the core has no thread-identity code at all; the one
 * thread-sensitive datum is the stack anchor, captured on the creating thread
 * inside JS_NewRuntime2 (quickjs.c:2019) -- create and use on the same thread
 * and it is simply correct. The UI-thread discipline starts with the real
 * runtime in M4 Task 2.
 */
bool FVaCuusJsVendoringSmokeTest::RunTest(const FString& Parameters)
{
	JSRuntime* Runtime = JS_NewRuntime();
	if (!TestNotNull(TEXT("JS_NewRuntime"), Runtime))
	{
		return false;
	}

	JSContext* Ctx = JS_NewContext(Runtime);
	if (!TestNotNull(TEXT("JS_NewContext"), Ctx))
	{
		JS_FreeRuntime(Runtime);
		return false;
	}

	// The canonical liveness probe: parse, execute, produce a number.
	{
		JSValue Value = Eval(Ctx, "1+1");
		TestFalse(TEXT("eval(1+1) does not throw"), JS_IsException(Value));
		int32_t Sum = 0;
		TestEqual(TEXT("JS_ToInt32 on the result succeeds"), JS_ToInt32(Ctx, &Sum, Value), 0);
		TestEqual(TEXT("1+1 evaluates to 2"), Sum, 2);
		JS_FreeValue(Ctx, Value);
	}

	// The vendored snapshot is the pinned tag: QJS_VERSION_* (quickjs.h:1415-1418)
	// stringified by JS_GetVersion (quickjs.c:98-100). Drift here means the tree
	// no longer matches VENDORED_TAG.txt.
	TestEqual(TEXT("JS_GetVersion is the pinned 0.15.1"), FString(UTF8_TO_TCHAR(JS_GetVersion())), FString(TEXT("0.15.1")));

	// queueMicrotask is a CORE builtin, not ours: js_global_queueMicrotask
	// (quickjs.c:40200) is installed on the global object via js_global_funcs
	// (quickjs.c:55939). The M4 facade must NOT shadow it -- host jobs ride
	// JS_EnqueueJob beside it (spec 2(a)). This assertion guards against a
	// future facade task replacing it by accident.
	{
		bool bIsException = false;
		const FString TypeOf = EvalToString(Ctx, "typeof queueMicrotask", bIsException);
		TestFalse(TEXT("typeof queueMicrotask does not throw"), bIsException);
		TestEqual(TEXT("queueMicrotask is a core-provided function"), TypeOf, FString(TEXT("function")));
	}

	// E3 probe (research note quickjs-ng-0151.md): JS_AddIntrinsicBigInt is not
	// in JS_NewContext's intrinsic list (quickjs.c:2540-2551), so whether the
	// default context speaks BigInt is unsettled from source reading alone.
	// RECORD the answer, assert nothing -- the Tier-1 context recipe decision
	// (Task 2) reads this log line.
	{
		bool bIsException = false;
		const FString BigIntProbe = EvalToString(Ctx, "typeof 1n", bIsException);
		const FString Report = FString::Printf(TEXT("E3 BigInt probe: eval(\"typeof 1n\") in a plain JS_NewContext -> %s"), *BigIntProbe);
		AddInfo(Report);
		UE_LOG(LogVaCuusJS, Display, TEXT("%s"), *Report);
	}

	JS_FreeContext(Ctx);
	JS_FreeRuntime(Runtime);
	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
