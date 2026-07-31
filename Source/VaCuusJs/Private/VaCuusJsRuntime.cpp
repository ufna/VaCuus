// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusJsRuntime.h"

#include "VaCuusJs.h"
#include "VaCuusJsDomHandle.h"
#include "VaCuusStats.h"

#include "HAL/IConsoleManager.h"
#include "HAL/LowLevelMemTracker.h"
#include "HAL/PlatformTime.h"
#include "HAL/UnrealMemory.h"

static TAutoConsoleVariable<int32> CVarVaCuusJsMemoryLimitMB(
	TEXT("vacuus.Js.MemoryLimitMB"),
	16,
	TEXT("JS heap cap in MB (default 16), enforced by quickjs itself: past it, allocations fail and the script gets an ")
		TEXT("InternalError 'out of memory'. Read once per runtime creation."));

/**
 * NOT 0 IN SHIPPING, deliberately: the watchdog doubles as the microtask-livelock
 * backstop (spec 3.3 -- v1 turned the only backstop off exactly where nobody could
 * attach a debugger), so shipping keeps a generous non-zero deadline. 50 ms is ~3
 * frames of a 60 Hz game: far past any legitimate script, far short of a visible hang.
 */
#if UE_BUILD_SHIPPING
static constexpr int32 GVaCuusJsWatchdogDefaultMs = 50;
#else
static constexpr int32 GVaCuusJsWatchdogDefaultMs = 250;
#endif

static TAutoConsoleVariable<int32> CVarVaCuusJsWatchdogMs(
	TEXT("vacuus.Js.WatchdogMs"),
	GVaCuusJsWatchdogDefaultMs,
	TEXT("Watchdog deadline per host->JS entry, in ms (default 250 in development builds, 50 in Shipping). A script ")
		TEXT("running past it is interrupted with an uncatchable error. 0 disables the watchdog entirely -- never ship ")
		TEXT("that: it is also the microtask-livelock backstop. Read once per runtime creation."));

static TAutoConsoleVariable<int32> CVarVaCuusJsGCStepKB(
	TEXT("vacuus.Js.GCStepKB"),
	512,
	TEXT("The frame GC point collects once live JS bytes have grown this much since the last collection (default 512). ")
		TEXT("Smaller steps mean more, shorter pauses. Read once per runtime creation."));

/**
 * The LLM attribution for the JS heap (spec 3.3). The CUSTOM-TAG path, because it is
 * cheap, not heavyweight: LLM_DEFINE_TAG is one file-scope object plus a name
 * registration (LowLevelMemTracker.h:437), LLM_SCOPE_BYTAG one stack push/pop
 * (:384), and the entire family compiles to NOTHING when LLM is off (:949-967) --
 * which it is unless the process runs with -llm. So `-llm -llmtagsets` shows the JS
 * heap as its own "VaCuusJS" row instead of folding it into ELLMTag::UI's.
 */
LLM_DEFINE_TAG(VaCuusJS);

FVaCuusJsRuntime::FVaCuusJsRuntime()
{
	Construct(FParams());
}

FVaCuusJsRuntime::FVaCuusJsRuntime(const FParams& InParams)
{
	Construct(InParams);
}

void FVaCuusJsRuntime::Construct(const FParams& InParams)
{
	const int32 MemoryLimitMB =
		InParams.MemoryLimitMB >= 0 ? InParams.MemoryLimitMB : CVarVaCuusJsMemoryLimitMB.GetValueOnAnyThread();
	WatchdogSeconds =
		double(InParams.WatchdogMs >= 0 ? InParams.WatchdogMs : CVarVaCuusJsWatchdogMs.GetValueOnAnyThread()) / 1000.0;
	const int32 GCStepKB = InParams.GCStepKB > 0 ? InParams.GCStepKB : CVarVaCuusJsGCStepKB.GetValueOnAnyThread();
	GCStepBytes = int64(GCStepKB) * 1024;

	// The five hooks (quickjs.h:456-462); opaque = this, delivered to the first four.
	// js_malloc_usable_size gets no opaque -- pointer-only by signature.
	JSMallocFunctions MallocFunctions = {};
	MallocFunctions.js_calloc = &FVaCuusJsRuntime::HookCalloc;
	MallocFunctions.js_malloc = &FVaCuusJsRuntime::HookMalloc;
	MallocFunctions.js_free = &FVaCuusJsRuntime::HookFree;
	MallocFunctions.js_realloc = &FVaCuusJsRuntime::HookRealloc;
	MallocFunctions.js_malloc_usable_size = &FVaCuusJsRuntime::HookUsableSize;

	{
		LLM_SCOPE_BYTAG(VaCuusJS);
		Runtime = JS_NewRuntime2(&MallocFunctions, this);
	}
	checkf(Runtime != nullptr, TEXT("JS_NewRuntime2 failed -- FMemory does not fail small allocations, so this is a vendoring problem"));

	// THE ACCOUNTING PROBE. Creating the runtime allocated through the hooks above, so
	// a zero counter here means FMemory::GetAllocSize answered 0 -- an allocator that
	// cannot report block sizes. Both consumers of that answer degrade together: the
	// GC trigger never sees growth, and quickjs's own cap accounting counts only
	// MALLOC_OVERHEAD per block (quickjs.c:1654). No such allocator ships on the
	// platforms this project targets (MallocBinned2 and MallocAnsi both implement
	// GetAllocationSize); this line is how a future one announces itself.
	if (GetLiveBytes() == 0)
	{
		UE_LOG(LogVaCuusJS, Warning,
			TEXT("FMemory::GetAllocSize reports 0 on this allocator: JS heap accounting is blind, so the GC trigger and ")
			TEXT("the memory cap will not work. Wire a size-reporting allocator or revisit the malloc hooks."));
	}

	// The cap (quickjs.h:498-499, "use 0 to disable"): past it the engine's allocators
	// return NULL immediately -- no collect-and-retry (quickjs.c:1645-1647) -- and the
	// script gets InternalError "out of memory" (quickjs.c:8127-8135). The fallback
	// collection is CollectGarbage()'s OOM branch.
	JS_SetMemoryLimit(Runtime, MemoryLimitMB > 0 ? size_t(MemoryLimitMB) * 1024 * 1024 : 0);

	// A quarter of the engine's 1 MiB default (JS_DEFAULT_STACK_SIZE, quickjs.h:428-429),
	// because this is a NATIVE-stack budget (quickjs.h:504-505; the check is a stack
	// pointer against an anchor, quickjs.c:1952-1957) and production runs it inside the
	// UI thread's 2 MB stack under RmlUi frames -- see GVaCuusUIThreadStackSize's
	// comment for the arithmetic. Deep recursion dies as RangeError "Maximum call stack
	// size exceeded" (quickjs.c:8138-8141), never the guard page.
	JS_SetMaxStackSize(Runtime, 256 * 1024);

	// THE FRAME-CONTROLLED GC DESIGN'S ONE LINE (spec 2(b)): (size_t)-1 disables the
	// engine's only implicit collection trigger -- "use -1 to disable automatic GC" is
	// the implementation's own comment (quickjs.c:2110-2114), and that trigger's sole
	// call site is object allocation (js_trigger_gc, called only from
	// JS_NewObjectFromShape, quickjs.c:5748). From here on, CollectGarbage() at the
	// frame point is the only collector.
	if (!InParams.bTestLeaveEngineGCEnabled)
	{
		JS_SetGCThreshold(Runtime, (size_t)-1);
	}

	// The DOM facade's wrapper classes (M4 Task 4): ids and class defs are
	// per-RUNTIME state (JS_NewClassID takes the runtime, quickjs.h:693;
	// JS_NewClass registers on it, :696), so they live here and every view
	// context of this runtime shares them -- each context then hangs its OWN
	// prototype on the shared id via JS_SetClassProto (quickjs.h:527). One
	// finalizer for both: the opaque shape is identical (VaCuusJsDomHandle.h).
	{
		static const JSClassDef GElementClassDef = {"VaCuusElement", &VaCuusJsDomFinalizer, nullptr, nullptr, nullptr};
		static const JSClassDef GDocumentClassDef = {"VaCuusDocument", &VaCuusJsDomFinalizer, nullptr, nullptr, nullptr};
		static const JSClassDef GEventClassDef = {"VaCuusEvent", &VaCuusJsEventFinalizer, nullptr, nullptr, nullptr};
		JS_NewClassID(Runtime, &ElementClassId);
		JS_NewClassID(Runtime, &DocumentClassId);
		JS_NewClassID(Runtime, &EventClassId);
		const int ElementResult = JS_NewClass(Runtime, ElementClassId, &GElementClassDef);
		const int DocumentResult = JS_NewClass(Runtime, DocumentClassId, &GDocumentClassDef);
		const int EventResult = JS_NewClass(Runtime, EventClassId, &GEventClassDef);
		checkf(ElementResult == 0 && DocumentResult == 0 && EventResult == 0,
			TEXT("JS_NewClass failed -- FMemory does not fail small allocations, so this is a vendoring problem"));
	}

	// Polled every 10k interpreter operations (JS_INTERRUPT_COUNTER_INIT,
	// quickjs.c:479, drained in js_poll_interrupts, quickjs.c:8221-8241) -- cheap
	// enough to leave installed even while no entry is armed.
	JS_SetInterruptHandler(Runtime, &FVaCuusJsRuntime::InterruptThunk, this);

	JS_SetHostPromiseRejectionTracker(Runtime, &FVaCuusJsRuntime::RejectionThunk, this);

	BytesAtLastCollection = GetLiveBytes();

	UE_LOG(LogVaCuusJS, Log,
		TEXT("JS runtime created: cap=%d MB, stack=256 KB, watchdog=%d ms, GC step=%d KB, boot heap=%lld bytes"),
		MemoryLimitMB, int32(WatchdogSeconds * 1000.0), GCStepKB, GetLiveBytes());
}

FVaCuusJsRuntime::~FVaCuusJsRuntime()
{
	if (Runtime == nullptr)
	{
		return;
	}

	JS_FreeRuntime(Runtime);
	Runtime = nullptr;

	// The leak observable (spec 5). JS_FreeRuntime freed every context, ran a final GC
	// and released the JSRuntime block itself through HookFree, so every byte the hooks
	// ever added has been subtracted -- unless something leaked a JSValue, which is
	// exactly what the engine's own assert(list_empty(&rt->gc_obj_list))
	// (quickjs.c:2348) would have said if UBT's global NDEBUG had not compiled it out.
	// This check is Development-real.
	checkf(GetLiveBytes() == 0,
		TEXT("JS runtime destroyed with %lld bytes still live -- a JSValue/JSContext was leaked past JS_FreeRuntime"),
		GetLiveBytes());
}

void* FVaCuusJsRuntime::HookCalloc(void* Opaque, size_t Count, size_t Size)
{
	// Overflow and zero are the caller's problem BY CONTRACT, and the contract holds:
	// js_calloc_rt asserts non-zero and rejects count*size overflow before the hook
	// (quickjs.c:1608-1625).
	LLM_SCOPE_BYTAG(VaCuusJS);
	void* Ptr = FMemory::MallocZeroed(Count * Size);
	static_cast<FVaCuusJsRuntime*>(Opaque)->LiveBytes.fetch_add(
		int64(FMemory::GetAllocSize(Ptr)), std::memory_order_relaxed);
	return Ptr;
}

void* FVaCuusJsRuntime::HookMalloc(void* Opaque, size_t Size)
{
	// THE ACCOUNTING BASIS, once, for all four hooks: the counter moves by
	// FMemory::GetAllocSize(Ptr) -- the allocator's own answer for the live block
	// -- not by the requested byte count. GetAllocSize is UNDOCUMENTED
	// (UnrealMemory.h:213 carries no contract; QuantizeSize at :217-223 promises
	// only ">= Count", for a REQUEST, not a block), so this accounting leans on
	// nothing it does not verify: (1) SYMMETRY -- whatever a live block reports at
	// the add it reports again at the subtract, so the counter self-cancels
	// exactly and zero at runtime destruction means no block outlived it; (2) the
	// constructor's zero-probe, which is what catches an allocator that answers 0
	// instead of a doc promise nobody made. The same figure feeds quickjs through
	// HookUsableSize, so our GC trigger and the engine's cap accounting (usable +
	// MALLOC_OVERHEAD per block, quickjs.c:1654) count in one currency. Note
	// FMemory::Malloc never returns null -- UE's allocators report OOM and crash
	// -- but the JS memory cap cannot be the cause: quickjs enforces it BEFORE
	// the hook is reached (quickjs.c:1645-1647).
	LLM_SCOPE_BYTAG(VaCuusJS);
	void* Ptr = FMemory::Malloc(Size);
	static_cast<FVaCuusJsRuntime*>(Opaque)->LiveBytes.fetch_add(
		int64(FMemory::GetAllocSize(Ptr)), std::memory_order_relaxed);
	return Ptr;
}

void FVaCuusJsRuntime::HookFree(void* Opaque, void* Ptr)
{
	if (Ptr == nullptr)
	{
		return;
	}

	static_cast<FVaCuusJsRuntime*>(Opaque)->LiveBytes.fetch_sub(
		int64(FMemory::GetAllocSize(Ptr)), std::memory_order_relaxed);
	FMemory::Free(Ptr);
}

void* FVaCuusJsRuntime::HookRealloc(void* Opaque, void* Ptr, size_t Size)
{
	// Never called with Size == 0 or a null Ptr: js_realloc_rt routes those to
	// js_free_rt / js_malloc_rt before the hook (quickjs.c:1676-1689).
	LLM_SCOPE_BYTAG(VaCuusJS);
	FVaCuusJsRuntime* Self = static_cast<FVaCuusJsRuntime*>(Opaque);
	const int64 OldSize = int64(FMemory::GetAllocSize(Ptr));
	void* NewPtr = FMemory::Realloc(Ptr, Size);
	Self->LiveBytes.fetch_add(int64(FMemory::GetAllocSize(NewPtr)) - OldSize, std::memory_order_relaxed);
	return NewPtr;
}

size_t FVaCuusJsRuntime::HookUsableSize(const void* Ptr)
{
	// const_cast: FMemory::GetAllocSize takes void* but only queries the allocator's
	// size metadata. quickjs reads this on live pointers only (e.g. before its own
	// realloc accounting, quickjs.c:1690-1700).
	return Ptr != nullptr ? FMemory::GetAllocSize(const_cast<void*>(Ptr)) : 0;
}

int FVaCuusJsRuntime::InterruptThunk(JSRuntime* /*Rt*/, void* Opaque)
{
	// Runs INSIDE JS execution on the owning thread, every 10k interpreter ops. A
	// non-zero return makes the engine throw the uncatchable "interrupted"
	// InternalError (JS_ThrowInterrupted, quickjs.c:8215-8219). Keeps answering 1
	// while past the deadline so nothing executed during unwinding can run for free.
	FVaCuusJsRuntime* Self = static_cast<FVaCuusJsRuntime*>(Opaque);
	if (Self->WatchdogDeadlineSeconds > 0.0 && FPlatformTime::Seconds() >= Self->WatchdogDeadlineSeconds)
	{
		if (!Self->bWatchdogTripped)
		{
			Self->bWatchdogTripped = true;
			Self->NumWatchdogTrips.fetch_add(1, std::memory_order_relaxed);
		}
		return 1;
	}
	return 0;
}

void FVaCuusJsRuntime::RejectionThunk(
	JSContext* Ctx, JSValueConst /*Promise*/, JSValueConst Reason, bool bIsHandled, void* Opaque)
{
	FVaCuusJsRuntime* Self = static_cast<FVaCuusJsRuntime*>(Opaque);

	FString ReasonText(TEXT("<unstringifiable>"));
	if (const char* Utf8 = JS_ToCString(Ctx, Reason))
	{
		ReasonText = FString(UTF8_TO_TCHAR(Utf8));
		JS_FreeCString(Ctx, Utf8);
	}
	else
	{
		// JS_ToCString failed and left ITS exception pending; clear it -- a tracker
		// callback must not hand a new exception back to the engine mid-operation.
		JS_FreeValue(Ctx, JS_GetException(Ctx));
	}

	if (!bIsHandled)
	{
		// Fired when a promise rejects with no handler attached (quickjs.c:54371-54375).
		Self->NumErrors.fetch_add(1, std::memory_order_relaxed);
		UE_LOG(LogVaCuusJS, Error, TEXT("Unhandled JS promise rejection: %s"), *ReasonText);
	}
	else
	{
		// The RETRACTION re-fire: a handler was attached to the already-rejected
		// promise later (quickjs.c:55165-55169). The M4 Task 8 overlay consumes this
		// to withdraw the entry; until then the log is the record. The error COUNTER
		// is deliberately not decremented -- it counts fired diagnostics, not
		// currently-standing ones, so tests can assert exact deltas.
		UE_LOG(LogVaCuusJS, Verbose, TEXT("A rejected JS promise was later handled (reason was: %s)"), *ReasonText);
	}
}

bool FVaCuusJsRuntime::CollectGarbage(const TCHAR* Reason)
{
	const bool bOomFallback = bOomFallbackPending;
	const int64 Live = GetLiveBytes();
	if (!bOomFallback && Live - BytesAtLastCollection < GCStepBytes)
	{
		return false;
	}

	const double StartSeconds = FPlatformTime::Seconds();

	// Synchronous three-phase cycle collector on this thread (quickjs.h:518); the
	// pause is proportional to the live GC-object graph. Wrapper finalizers (Task 4
	// on) run inside this call -- the frame point is what guarantees they run on the
	// UI thread (spec 3.6).
	JS_RunGC(Runtime);

	LastCollectionPauseMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	NumCollections.fetch_add(1, std::memory_order_relaxed);
	bOomFallbackPending = false;
	BytesAtLastCollection = GetLiveBytes();

	// AT COLLECTIONS ONLY: this walks the whole heap (spec 2(b) -- which is why it
	// cannot drive the trigger). malloc_size is the engine's own live accounting
	// (JSMemoryUsage, quickjs.h:578-592), the figure the 16 MB cap compares against.
	JSMemoryUsage Usage;
	JS_ComputeMemoryUsage(Runtime, &Usage);
	LastCollectionHeapBytes = uint64(Usage.malloc_size);

	if (bOomFallback)
	{
		// The spec 2(b) fallback: the engine refused an allocation at the cap WITHOUT
		// collecting, the script already got its InternalError, and this collection is
		// what makes the next frame's allocations succeed again.
		UE_LOG(LogVaCuusJS, Warning,
			TEXT("JS OOM fallback collection (%s): %.3f ms, heap %llu bytes, %lld live bytes. The script that hit the ")
			TEXT("cap already received InternalError 'out of memory'"),
			Reason, LastCollectionPauseMs, LastCollectionHeapBytes, BytesAtLastCollection);
	}
	else
	{
		UE_LOG(LogVaCuusJS, Verbose, TEXT("JS collection (%s): %.3f ms, heap %llu bytes, %lld live bytes"),
			Reason, LastCollectionPauseMs, LastCollectionHeapBytes, BytesAtLastCollection);
	}

	FVaCuusPerfLog::AddJsGC(LastCollectionPauseMs, LastCollectionHeapBytes);
	return true;
}

void FVaCuusJsRuntime::ReportException(JSContext* Ctx, const TCHAR* SourceName)
{
	JSValue Exception = JS_GetException(Ctx);

	FString Message(TEXT("<unstringifiable>"));
	if (const char* Utf8 = JS_ToCString(Ctx, Exception))
	{
		Message = FString(UTF8_TO_TCHAR(Utf8));
		JS_FreeCString(Ctx, Utf8);
	}
	else
	{
		// JS_ToCString failure sets an exception of its own (Task 1); clear it.
		JS_FreeValue(Ctx, JS_GetException(Ctx));
	}

	// `stack` is a real string property, installed by build_backtrace
	// (quickjs.c:7766) -- present on Error instances, absent on thrown non-Errors.
	FString Stack;
	if (JS_IsError(Exception))
	{
		JSValue StackValue = JS_GetPropertyStr(Ctx, Exception, "stack");
		if (!JS_IsException(StackValue) && !JS_IsUndefined(StackValue))
		{
			if (const char* Utf8 = JS_ToCString(Ctx, StackValue))
			{
				Stack = FString(UTF8_TO_TCHAR(Utf8));
				JS_FreeCString(Ctx, Utf8);
			}
		}
		if (JS_IsException(StackValue))
		{
			JS_FreeValue(Ctx, JS_GetException(Ctx));
		}
		JS_FreeValue(Ctx, StackValue);
	}

	NumErrors.fetch_add(1, std::memory_order_relaxed);

	// The OOM recognition that arms the fallback. The engine's shape is fixed:
	// JS_ThrowOutOfMemory throws an InternalError with message exactly "out of
	// memory" (quickjs.c:8127-8136, latched by rt->in_out_of_memory so it cannot
	// even nest), which stringifies to exactly "InternalError: out of memory".
	// The predicate below is the tightest the public API allows: an error-KIND
	// check does not exist -- every native error is built with class
	// JS_CLASS_ERROR and differs only by prototype (JS_MakeError2,
	// quickjs.c:7980-7985), and the prototype table is context-private -- so it
	// is JS_IsError (the class check, quickjs.c:11604-11607) plus WHOLE-string
	// equality. A script's `throw new Error("... out of memory ...")` stringifies
	// as "Error: ...": no match (the bug a Contains() here had). Forging the
	// exact string takes renaming an Error by hand -- the InternalError
	// constructor is not installed as a global, only handed to internal builtins
	// (quickjs.c:8598-8600) -- and buys the forger nothing but one spurious
	// collection at the next frame point.
	if (JS_IsError(Exception) && Message.Equals(TEXT("InternalError: out of memory")))
	{
		NoteOutOfMemory();
	}

	UE_LOG(LogVaCuusJS, Error, TEXT("JS exception in '%s': %s%s%s"),
		SourceName, *Message,
		Stack.IsEmpty() ? TEXT("") : TEXT("\n"), *Stack);

	JS_FreeValue(Ctx, Exception);
}

void FVaCuusJsRuntime::UpdateStackTopOnThisThread()
{
	// Re-captures stack_top and recomputes stack_limit from it (quickjs.c:2778-2782
	// via 2764-2768); the anchor was first captured inside JS_NewRuntime2
	// (quickjs.c:2019) on whatever thread built this runtime.
	JS_UpdateStackTop(Runtime);
}

FVaCuusJsEntryGuard::FVaCuusJsEntryGuard(FVaCuusJsRuntime& InRuntime, JSContext* InCtx, const TCHAR* InSourceName)
	: Runtime(InRuntime)
	, Ctx(InCtx)
	, SourceName(InSourceName)
{
	// Outermost entry only (see the struct comment): nested guards must not extend
	// the deadline the first boundary armed. WatchdogSeconds == 0 is the disabled
	// (deliberately-broken, test-red) state -- nothing arms, nothing ever trips.
	if (Runtime.WatchdogEntryDepth++ == 0 && Runtime.WatchdogSeconds > 0.0)
	{
		Runtime.bWatchdogTripped = false;
		Runtime.WatchdogDeadlineSeconds = FPlatformTime::Seconds() + Runtime.WatchdogSeconds;
	}
}

FVaCuusJsEntryGuard::~FVaCuusJsEntryGuard()
{
	if (--Runtime.WatchdogEntryDepth > 0)
	{
		return;
	}

	Runtime.WatchdogDeadlineSeconds = 0.0;

	if (Runtime.bWatchdogTripped)
	{
		Runtime.bWatchdogTripped = false;

		// The trip is uncatchable by design -- script try/catch cannot swallow it
		// (JS_ThrowInterrupted sets the uncatchable bit, quickjs.c:8215-8219) -- and
		// the reset happens HERE, at the same boundary that armed the deadline, so it
		// cannot leak into the next entry. JS_ResetUncatchableError re-throws the
		// pending exception with the bit cleared (quickjs.h:829-833), which is what
		// lets the code after this scope consume it like any other failure. Guarded on
		// HasException for the corner where the caller consumed it early despite the
		// usage contract.
		UE_LOG(LogVaCuusJS, Error,
			TEXT("JS watchdog: '%s' ran past its %d ms deadline and was interrupted"),
			SourceName, int32(Runtime.WatchdogSeconds * 1000.0));

		if (JS_HasException(Ctx))
		{
			JS_ResetUncatchableError(Ctx);
		}
	}
}
