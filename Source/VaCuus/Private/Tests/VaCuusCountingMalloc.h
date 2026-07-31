// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusDefines.h"

#include "HAL/MemoryBase.h"
#include "HAL/PlatformAtomics.h"

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
 * THE COUNT IS PROCESS-WIDE, NOT CALLER-SCOPED. Every thread's allocations land in the
 * same counters, so a window is meaningful only when the measuring thread is the only one
 * doing relevant work -- and even then a logger or timer thread can donate a count. The
 * callers below absorb that by protocol, not by assertion: repeat the window, take the
 * MINIMUM -- deterministic allocations made by the measured code appear in every window,
 * intermittent donations do not -- and assert a small bound on that minimum rather than a
 * literal zero of the whole process.
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

	//~ Relaxed is enough: windows are read on the installing thread after the restoring
	//~ CAS (a full barrier), and the min-of-windows protocol absorbs a straggler count
	//~ from a thread still inside a forwarding call.
	std::atomic<uint64> NumMallocs{0};
	std::atomic<uint64> NumReallocs{0};
	std::atomic<uint64> NumFrees{0};

	void ResetCounts()
	{
		NumMallocs.store(0, std::memory_order_relaxed);
		NumReallocs.store(0, std::memory_order_relaxed);
		NumFrees.store(0, std::memory_order_relaxed);
	}

	virtual void* Malloc(SIZE_T Size, uint32 Alignment) override
	{
		NumMallocs.fetch_add(1, std::memory_order_relaxed);
		return Inner->Malloc(Size, Alignment);
	}

	virtual void* TryMalloc(SIZE_T Size, uint32 Alignment) override
	{
		NumMallocs.fetch_add(1, std::memory_order_relaxed);
		return Inner->TryMalloc(Size, Alignment);
	}

	virtual void* MallocZeroed(SIZE_T Size, uint32 Alignment) override
	{
		NumMallocs.fetch_add(1, std::memory_order_relaxed);
		return Inner->MallocZeroed(Size, Alignment);
	}

	virtual void* TryMallocZeroed(SIZE_T Size, uint32 Alignment) override
	{
		NumMallocs.fetch_add(1, std::memory_order_relaxed);
		return Inner->TryMallocZeroed(Size, Alignment);
	}

	virtual void* Realloc(void* Ptr, SIZE_T NewSize, uint32 Alignment) override
	{
		NumReallocs.fetch_add(1, std::memory_order_relaxed);
		return Inner->Realloc(Ptr, NewSize, Alignment);
	}

	virtual void* TryRealloc(void* Ptr, SIZE_T NewSize, uint32 Alignment) override
	{
		NumReallocs.fetch_add(1, std::memory_order_relaxed);
		return Inner->TryRealloc(Ptr, NewSize, Alignment);
	}

	virtual void Free(void* Ptr) override
	{
		NumFrees.fetch_add(1, std::memory_order_relaxed);
		Inner->Free(Ptr);
	}

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

	uint64 Total() const { return Mallocs + Reallocs; }
};

inline FVaCuusCountingMallocProxy& GetProxy()
{
	// See the class comment for why this must never be destroyed while a thread could
	// still be inside a forwarding call -- i.e. ever.
	static FVaCuusCountingMallocProxy Proxy;
	return Proxy;
}

/** @return true when the proxy is installed and counting; false means the window could not open. */
inline bool Begin()
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

	FMalloc* Live = UE::Private::GMalloc;
	Proxy.Inner = Live;
	Proxy.ResetCounts();

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
	return Counts;
}
}	 // namespace VaCuusAllocWindow

#endif	  // WITH_DEV_AUTOMATION_TESTS
