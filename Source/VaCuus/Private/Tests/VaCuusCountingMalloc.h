// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusDefines.h"

#include "HAL/MemoryBase.h"
#include "HAL/PlatformAtomics.h"
#include "HAL/PlatformTLS.h"

#include <atomic>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A COUNTING FMalloc PROXY, for spec 9's two allocation rows and nothing else: without it,
 * "0 container reallocations; ~0 element allocations" is a claim with no observable -- the
 * exact rot the project's conventions forbid.
 *
 * THE SHAPE IS FMallocPoisonProxy'S (MallocPoisonProxy.h:24-186): hold the inner FMalloc,
 * forward EVERY virtual, add one side effect. The side effect here is a relaxed counter
 * bump on the entry points that hand out memory -- Malloc, MallocZeroed, TryMalloc,
 * TryMallocZeroed into NumMallocs; Realloc, TryRealloc into NumReallocs -- because those
 * are the events the spec's row is about. The Try/Zeroed variants are forwarded rather
 * than left to FMalloc's defaults for the same reason they are counted here: the base
 * implementations route through this->Malloc (MemoryBase.cpp:95-127), which would count
 * them once but hide the inner allocator's own optimized paths.
 *
 * INSTALLATION IS THE ENGINE'S OWN RUNTIME-CHAINING SHAPE: FMemory::EnablePoisonTests
 * installs a proxy over the live allocator with an InterlockedCompareExchangePointer on
 * &UE::Private::GMalloc (UnrealMemory.cpp:273-303), mid-run, with every thread allocating.
 * That works because FMemory::Malloc is FORCENOINLINE CORE_API (UnrealMemory.h:211-216) --
 * one compiled body in Core that re-reads UE::Private::GMalloc on every call
 * (FMemory.inl:28-36 via Memory.h's FMEMORY_INLINE_GMalloc, which is UE::Private::GMalloc
 * whenever PLATFORM_USES_FIXED_GMalloc_CLASS is 0, and Platform.h:487-488 defaults it to 0;
 * a fixed-GMalloc platform bypasses the pointer entirely, which is why the engine refuses
 * its proxies there, UnrealMemory.cpp:280-284). So a swapped pointer takes effect at the
 * next FMemory call, no module recompiles anything, and no caller holds a stale allocator.
 *
 * WHY THE INSTANCE IS PROCESS-LIFETIME (function-static, never destroyed). The engine's
 * proxies are installed forever; this one is UNinstalled, and the uninstall is the one
 * moment the poison proxy never faces: a thread that loaded the proxy pointer just before
 * the restoring CAS may still be INSIDE a forwarding call just after it. That is harmless
 * for a live object -- the call forwards to the same inner allocator either way -- and a
 * use-after-free for a scoped one. Static storage closes the hole by shape.
 *
 * THE COUNT IS SCOPED TO THE PARTICIPATING THREADS, AND IT USED NOT TO BE. Every thread in
 * the process allocates through this proxy while a window is open -- that is unavoidable,
 * GMalloc is one pointer -- but only the threads a window NAMES contribute to its counters.
 * Everyone else forwards and is not counted.
 *
 * IT USED TO COUNT EVERYTHING, and the protocol that was supposed to absorb that (repeat
 * the window, take the minimum) does not hold on a venue where OTHER threads are busy
 * continuously rather than occasionally. Measured, full suite, real RHI (-RenderOffscreen,
 * Vulkan) 2026-08-06, both failures on the first run and both filed as defects of the
 * instrument rather than of the code under test (VaCuus-gq6, VaCuus-z36):
 *
 *   VaCuus.Model.Sampler.ArrayAlloc  "a warm republish allocates ~nothing (min 11 <= 4)"
 *   VaCuus.RefHud.MemProxy           "canary: RemoveContext freed through GMalloc
 *                                     (frees=11, bytes=+5264)"
 *
 * The second is the clearer tell: that window frees an RmlUi context and nothing else, so
 * its byte delta is negative BY CONSTRUCTION -- unless the render thread, the RHI thread and
 * the driver are allocating into the same ledger, which on a real RHI they are. The observed
 * value swung -632, -5008, -88936 and +5264 across runs of the same window. A minimum over
 * eight windows cannot filter a contributor that is present in all eight.
 *
 * SCOPING IS BY THREAD ID AND NOT BY A thread_local FLAG, because a window may legitimately
 * measure work on a thread it does not run on: the RefHud boot window enqueues onto the UI
 * thread and pumps it, so the allocations it exists to count happen there. A flag can only
 * be set on the thread that sets it; an id list can name the UI thread from the test thread.
 *
 * The min-of-windows protocol stays in the callers, and its reason is now the narrower one it
 * can actually deliver: it absorbs a straggler on a PARTICIPATING thread -- a lazily built TLS
 * cache on the first window, a background flush the UI thread performs once -- not the steady
 * traffic of the whole process, which is now excluded by shape.
 */
class FVaCuusCountingMallocProxy final : public FMalloc
{
public:
	/** The chained-over allocator. Set by Install() before the proxy becomes reachable. */
	FMalloc* Inner = nullptr;

	/**
	 * Latched by End() when its restoring CAS fails: a foreign proxy chained over this one
	 * mid-window, so this proxy is still INSIDE the live chain (interloper -> this -> old
	 * inner). A later Begin() would read GMalloc -- now the interloper -- into Inner and
	 * close a cycle, this -> interloper -> this, that recurses forever on the first
	 * allocation. Begin() refuses while this is set: the failure is loud (one Error log and
	 * skipped windows), never a hang. Touched only by Begin()/End(), i.e. the measuring
	 * thread.
	 */
	bool bDisabled = false;

	/**
	 * THE THREADS THIS WINDOW COUNTS. Written by Begin() BEFORE the CAS that publishes the
	 * proxy, so every thread that can reach the counters already sees the final list -- the
	 * CAS is the release, and no separate barrier is needed for these.
	 *
	 * Four slots because the shapes that exist are one (the measuring thread alone) and two
	 * (measuring thread + UI thread); the fourth is headroom for a render-thread window
	 * without a format change. A window that names more is a design smell, so it is a
	 * static_assert-able ceiling rather than a TArray -- and a TArray here would allocate
	 * inside the allocator it is instrumenting.
	 */
	static constexpr int32 MaxParticipants = 4;
	std::atomic<uint32> Participants[MaxParticipants]{};
	std::atomic<int32> NumParticipants{0};

	//~ Relaxed is enough: windows are read on the installing thread after the restoring
	//~ CAS (a full barrier), and the min-of-windows protocol absorbs a straggler count
	//~ from a participating thread still inside a forwarding call.
	std::atomic<uint64> NumMallocs{0};
	std::atomic<uint64> NumReallocs{0};
	std::atomic<uint64> NumFrees{0};

	/**
	 * THE M6 §2(i) BYTE LEDGER — the RAM row's Dev-only proxy cross-check, with the
	 * SYMMETRIC QUANTIZED formula the spec fixed after v1's review: add the inner
	 * allocator's OWN size for the block (`Inner->GetAllocationSize`) after every
	 * alloc, subtract that same quantity before every free, realloc = subtract-old +
	 * add-new. Both sides of the ledger then speak quantized bytes, so the sum is
	 * EXACTLY the window's net change in live quantized bytes. v1's defect, recorded
	 * because it is the reason for this shape (spec §9.1): adding REQUESTED sizes
	 * while subtracting QUANTIZED ones accumulates negative drift proportional to
	 * churn — every alloc/free round-trip leaks the bucket padding out of the sum.
	 *
	 * SIGNED, deliberately: a block allocated before the window and freed inside it
	 * subtracts bytes the window never added — which is CORRECT for a net-live-bytes
	 * delta (the process really did shrink) and makes negative excursions legal.
	 *
	 * SizeLookupFailures is the ledger's own validity bit: one failed
	 * GetAllocationSize means one block entered or left at unknown size and the
	 * "exactly" above is void — callers assert it stayed 0.
	 */
	std::atomic<int64> LiveQuantizedBytes{0};
	std::atomic<uint64> SizeLookupFailures{0};

	/**
	 * @return true when the calling thread is one this window counts.
	 *
	 * On the hot path of every allocation in the process while a window is open, so it is a
	 * TLS read and a walk of at most four ints -- FPlatformTLS::GetCurrentThreadId() is
	 * pthread_self() on Linux and a TEB field on Windows, neither of which allocates. That
	 * last part is not a nicety: an allocating thread-id lookup would re-enter this proxy.
	 */
	FORCEINLINE bool CountsThisThread() const
	{
		const uint32 ThisThread = FPlatformTLS::GetCurrentThreadId();
		const int32 Num = NumParticipants.load(std::memory_order_relaxed);
		for (int32 Index = 0; Index < Num; ++Index)
		{
			if (Participants[Index].load(std::memory_order_relaxed) == ThisThread)
			{
				return true;
			}
		}
		return false;
	}

	void ResetCounts()
	{
		NumMallocs.store(0, std::memory_order_relaxed);
		NumReallocs.store(0, std::memory_order_relaxed);
		NumFrees.store(0, std::memory_order_relaxed);
		LiveQuantizedBytes.store(0, std::memory_order_relaxed);
		SizeLookupFailures.store(0, std::memory_order_relaxed);
	}

	virtual void* Malloc(SIZE_T Size, uint32 Alignment) override
	{
		const bool bCount = CountsThisThread();
		if (bCount)
		{
			NumMallocs.fetch_add(1, std::memory_order_relaxed);
		}
		void* Result = Inner->Malloc(Size, Alignment);
		if (bCount)
		{
			AddBlockBytes(Result);
		}
		return Result;
	}

	virtual void* TryMalloc(SIZE_T Size, uint32 Alignment) override
	{
		const bool bCount = CountsThisThread();
		if (bCount)
		{
			NumMallocs.fetch_add(1, std::memory_order_relaxed);
		}
		void* Result = Inner->TryMalloc(Size, Alignment);
		if (bCount)
		{
			AddBlockBytes(Result);
		}
		return Result;
	}

	virtual void* MallocZeroed(SIZE_T Size, uint32 Alignment) override
	{
		const bool bCount = CountsThisThread();
		if (bCount)
		{
			NumMallocs.fetch_add(1, std::memory_order_relaxed);
		}
		void* Result = Inner->MallocZeroed(Size, Alignment);
		if (bCount)
		{
			AddBlockBytes(Result);
		}
		return Result;
	}

	virtual void* TryMallocZeroed(SIZE_T Size, uint32 Alignment) override
	{
		const bool bCount = CountsThisThread();
		if (bCount)
		{
			NumMallocs.fetch_add(1, std::memory_order_relaxed);
		}
		void* Result = Inner->TryMallocZeroed(Size, Alignment);
		if (bCount)
		{
			AddBlockBytes(Result);
		}
		return Result;
	}

	virtual void* Realloc(void* Ptr, SIZE_T NewSize, uint32 Alignment) override
	{
		// ONE decision per call, taken once and reused: a realloc that counted its
		// subtraction but not its addition (or the reverse) would corrupt the ledger far
		// more quietly than a foreign thread ever did.
		const bool bCount = CountsThisThread();
		if (bCount)
		{
			NumReallocs.fetch_add(1, std::memory_order_relaxed);
			SubBlockBytes(Ptr); // BEFORE the realloc invalidates the old block's size
		}
		void* Result = Inner->Realloc(Ptr, NewSize, Alignment);
		if (bCount)
		{
			if (Result == nullptr && Ptr != nullptr && NewSize > 0)
			{
				AddBlockBytes(Ptr); // a failed grow leaves the old block live: restore its bytes
			}
			else
			{
				AddBlockBytes(Result);
			}
		}
		return Result;
	}

	virtual void* TryRealloc(void* Ptr, SIZE_T NewSize, uint32 Alignment) override
	{
		const bool bCount = CountsThisThread();
		if (bCount)
		{
			NumReallocs.fetch_add(1, std::memory_order_relaxed);
			SubBlockBytes(Ptr);
		}
		void* Result = Inner->TryRealloc(Ptr, NewSize, Alignment);
		if (bCount)
		{
			if (Result == nullptr && Ptr != nullptr && NewSize > 0)
			{
				AddBlockBytes(Ptr);
			}
			else
			{
				AddBlockBytes(Result);
			}
		}
		return Result;
	}

	virtual void Free(void* Ptr) override
	{
		if (CountsThisThread())
		{
			NumFrees.fetch_add(1, std::memory_order_relaxed);
			SubBlockBytes(Ptr); // BEFORE the free retires the block
		}
		Inner->Free(Ptr);
	}

private:
	void AddBlockBytes(void* Ptr)
	{
		SIZE_T BlockSize = 0;
		if (Ptr == nullptr)
		{
			return;
		}
		if (Inner->GetAllocationSize(Ptr, BlockSize))
		{
			LiveQuantizedBytes.fetch_add(int64(BlockSize), std::memory_order_relaxed);
		}
		else
		{
			SizeLookupFailures.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void SubBlockBytes(void* Ptr)
	{
		SIZE_T BlockSize = 0;
		if (Ptr == nullptr)
		{
			return;
		}
		if (Inner->GetAllocationSize(Ptr, BlockSize))
		{
			LiveQuantizedBytes.fetch_sub(int64(BlockSize), std::memory_order_relaxed);
		}
		else
		{
			SizeLookupFailures.fetch_add(1, std::memory_order_relaxed);
		}
	}

public:

	//~ Pure forwarding from here down -- the FMallocPoisonProxy checklist PLUS the two
	//~ cached-memory-size getters that proxy skips (neither GetImmediatelyFreeableCachedMemorySize
	//~ nor GetTotalFreeCachedMemorySize appears anywhere in MallocPoisonProxy.h:24-186), so this
	//~ list is more complete than its named source, not a copy of it.

	virtual SIZE_T QuantizeSize(SIZE_T Count, uint32 Alignment) override { return Inner->QuantizeSize(Count, Alignment); }
	virtual bool GetAllocationSize(void* Original, SIZE_T& SizeOut) override { return Inner->GetAllocationSize(Original, SizeOut); }
	virtual void InitializeStatsMetadata() override { Inner->InitializeStatsMetadata(); }
	virtual void UpdateStats() override { Inner->UpdateStats(); }
	virtual void GetAllocatorStats(FGenericMemoryStats& OutStats) override { Inner->GetAllocatorStats(OutStats); }
	virtual void DumpAllocatorStats(class FOutputDevice& Ar) override { Inner->DumpAllocatorStats(Ar); }
	virtual bool IsInternallyThreadSafe() const override { return Inner->IsInternallyThreadSafe(); }
	virtual bool ValidateHeap() override { return Inner->ValidateHeap(); }
	virtual const TCHAR* GetDescriptiveName() override { return Inner->GetDescriptiveName(); }
	virtual void Trim(bool bTrimThreadCaches) override { Inner->Trim(bTrimThreadCaches); }
	virtual void SetupTLSCachesOnCurrentThread() override { Inner->SetupTLSCachesOnCurrentThread(); }
	virtual void MarkTLSCachesAsUsedOnCurrentThread() override { Inner->MarkTLSCachesAsUsedOnCurrentThread(); }
	virtual void MarkTLSCachesAsUnusedOnCurrentThread() override { Inner->MarkTLSCachesAsUnusedOnCurrentThread(); }
	virtual void ClearAndDisableTLSCachesOnCurrentThread() override { Inner->ClearAndDisableTLSCachesOnCurrentThread(); }
	virtual void OnMallocInitialized() override { Inner->OnMallocInitialized(); }
	virtual void OnPreFork() override { Inner->OnPreFork(); }
	virtual void OnPostFork() override { Inner->OnPostFork(); }
	virtual uint64 GetImmediatelyFreeableCachedMemorySize() const override { return Inner->GetImmediatelyFreeableCachedMemorySize(); }
	virtual uint64 GetTotalFreeCachedMemorySize() const override { return Inner->GetTotalFreeCachedMemorySize(); }

#if UE_ALLOW_EXEC_COMMANDS
	virtual bool Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar) override { return Inner->Exec(InWorld, Cmd, Ar); }
#endif
};

/**
 * One counted window: Begin() chains the proxy over the live allocator, End() restores it
 * and returns what the window saw. Both from the measuring thread; windows never nest.
 */
namespace VaCuusAllocWindow
{
struct FCounts
{
	uint64 Mallocs = 0;
	uint64 Reallocs = 0;
	uint64 Frees = 0;

	/** Net change in live quantized bytes across the window (the §2(i) ledger; signed by design). */
	int64 LiveQuantizedBytesDelta = 0;

	/** Nonzero voids the ledger's exactness claim — see the proxy member's comment. */
	uint64 SizeLookupFailures = 0;

	uint64 Total() const { return Mallocs + Reallocs; }
};

inline FVaCuusCountingMallocProxy& GetProxy()
{
	// See the class comment for why this must never be destroyed while a thread could
	// still be inside a forwarding call -- i.e. ever.
	static FVaCuusCountingMallocProxy Proxy;
	return Proxy;
}

/**
 * Open a window that counts the CALLING thread, plus any threads named in AlsoCount.
 *
 * The default -- calling thread only -- is right for every window that measures a call it
 * makes itself, which is all of them except the RefHud boot and still windows: those enqueue
 * onto the UI thread and pump it, so the allocations they exist to count happen there and
 * they pass `{UIThread->GetThreadId()}`. Naming a thread that is not actually doing the work
 * costs nothing; failing to name one that is makes the window read zero, which is the failure
 * mode to watch for when a window suddenly measures ~nothing.
 *
 * @return true when the proxy is installed and counting; false means the window could not open.
 */
inline bool Begin(TConstArrayView<uint32> AlsoCount = {})
{
	FVaCuusCountingMallocProxy& Proxy = GetProxy();

	// Refuse after a failed End() (see bDisabled: Inner would be re-based onto an allocator
	// that still chains to this proxy -- an infinite-recursion cycle), and refuse while the
	// proxy is already the head of the chain, where the self-referential Inner would be the
	// same cycle with no interloper needed.
	if (Proxy.bDisabled || UE::Private::GMalloc == &Proxy)
	{
		return false;
	}
	if (!ensureMsgf(AlsoCount.Num() + 1 <= FVaCuusCountingMallocProxy::MaxParticipants,
			TEXT("VaCuusAllocWindow: %d participants asked for, %d slots"),
			AlsoCount.Num() + 1, FVaCuusCountingMallocProxy::MaxParticipants))
	{
		return false;
	}

	FMalloc* Live = UE::Private::GMalloc;
	Proxy.Inner = Live;
	Proxy.ResetCounts();

	// BEFORE the CAS, which is what publishes both the proxy and this list. A duplicate id
	// (a caller naming its own thread) is harmless -- CountsThisThread stops at the first
	// match -- so it is not filtered.
	int32 NumParticipants = 0;
	Proxy.Participants[NumParticipants++].store(FPlatformTLS::GetCurrentThreadId(), std::memory_order_relaxed);
	for (const uint32 ThreadId : AlsoCount)
	{
		Proxy.Participants[NumParticipants++].store(ThreadId, std::memory_order_relaxed);
	}
	Proxy.NumParticipants.store(NumParticipants, std::memory_order_relaxed);

	// The CAS, not a plain store: if another thread chained its own proxy between the read
	// and here, a store would silently orphan it. One attempt is enough for a test window;
	// the caller skips the measurement rather than fighting for the pointer.
	return FPlatformAtomics::InterlockedCompareExchangePointer(
			   reinterpret_cast<void**>(&UE::Private::GMalloc), &Proxy, Live) == Live;
}

inline FCounts End()
{
	FVaCuusCountingMallocProxy& Proxy = GetProxy();

	// Restore by CAS for the same reason: succeed only if the window's proxy is still the
	// head of the chain. A failure here means someone chained over us mid-window; leaving
	// the pointer alone is then the only safe move, the caller's counts are void -- and no
	// window may ever open again, because the proxy is still inside the interloper's chain
	// (see bDisabled for the cycle a re-Begin() would close). The log fires at most once by
	// construction: the latch makes every later Begin() refuse, so no later End() runs.
	if (FPlatformAtomics::InterlockedCompareExchangePointer(
			reinterpret_cast<void**>(&UE::Private::GMalloc), Proxy.Inner, &Proxy) != &Proxy)
	{
		Proxy.bDisabled = true;
		UE_LOG(LogVaCuus, Error,
			TEXT("VaCuusAllocWindow: a foreign allocator proxy chained over the counting proxy mid-window; "
				 "this window's counts are void and counting windows are disabled for the rest of the process"));
	}

	FCounts Counts;
	Counts.Mallocs = Proxy.NumMallocs.load(std::memory_order_relaxed);
	Counts.Reallocs = Proxy.NumReallocs.load(std::memory_order_relaxed);
	Counts.Frees = Proxy.NumFrees.load(std::memory_order_relaxed);
	Counts.LiveQuantizedBytesDelta = Proxy.LiveQuantizedBytes.load(std::memory_order_relaxed);
	Counts.SizeLookupFailures = Proxy.SizeLookupFailures.load(std::memory_order_relaxed);
	return Counts;
}
}	 // namespace VaCuusAllocWindow

#endif	  // WITH_DEV_AUTOMATION_TESTS
