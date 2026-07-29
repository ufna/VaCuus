// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusUIThread.h"

#include "VaCuusDefines.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"

namespace
{
/**
 * OS id of the live UI thread, or 0 when there is none. Mirrors the engine's own
 * GGameThreadId / GRenderThreadId so IsInUIThread() needs no instance and costs a
 * relaxed load. Deliberately a single global: VaCuus runs at most one UI thread.
 */
std::atomic<uint32> GVaCuusUIThreadId{0};
}	 // namespace

FVaCuusUIThread::FVaCuusUIThread() = default;

FVaCuusUIThread::~FVaCuusUIThread()
{
	Stop();

	// The platform destructor does Kill(true), i.e. Stop() then join, so Run() and
	// Exit() have both finished by the time this returns. Destroying the thread
	// before any member dies is what keeps WakeEvent alive for its last waiter.
	delete Thread;
	Thread = nullptr;
}

bool FVaCuusUIThread::Start()
{
	if (Thread != nullptr)
	{
		UE_LOG(LogVaCuus, Warning, TEXT("UI thread is already started"));
		return true;
	}

	// Name stays within 15 characters: Linux truncates the OS thread name there.
	// Stack size 0 == platform default; BelowNormal keeps us off the game and
	// render threads' backs.
	Thread = FRunnableThread::Create(this, TEXT("VaCuusUI"), 0, TPri_BelowNormal);

	if (Thread == nullptr)
	{
		// FRunnableThread::Create() returns nullptr *without logging anything* when
		// FPlatformProcess::SupportsMultithreading() is false and the runnable has no
		// FSingleThreadRunnable -- commandlets, -nothreading, some server configs.
		// Say so loudly; the caller is expected to run UI frames inline instead.
		UE_LOG(LogVaCuus, Warning,
			TEXT("Failed to create the VaCuus UI thread (SupportsMultithreading=%s); the caller must run UI frames inline"),
			FPlatformProcess::SupportsMultithreading() ? TEXT("true") : TEXT("false"));
		return false;
	}

	// Create() blocks until Init() has returned, so GetThreadId() is already valid.
	UE_LOG(LogVaCuus, Log, TEXT("UI thread started (id %u)"), GetThreadId());
	return true;
}

void FVaCuusUIThread::Stop()
{
	// Called from the owner's thread, including from inside Kill(true). Both halves
	// are idempotent, so calling this twice -- or after the worker already left --
	// is harmless: the store repeats and the latch is simply left set.
	bStopRequested.store(true, std::memory_order_release);

	// Load-bearing: without this the worker stays parked in Wait(MAX_uint32) and the
	// join inside Kill(true) never returns.
	WakeEvent->Trigger();
}

void FVaCuusUIThread::Trigger()
{
	// The event is auto-reset, so it is a binary latch: N triggers arriving while a
	// frame is in flight wake the worker exactly once.
	WakeEvent->Trigger();
}

bool FVaCuusUIThread::IsRunning() const
{
	// Stop() asks the worker to leave; it does not join. Reporting "not running" from
	// the moment the request lands is the only answer that does not race the worker's
	// own teardown, and it is what callers actually want to gate work on.
	return Thread != nullptr && !bStopRequested.load(std::memory_order_acquire);
}

uint32 FVaCuusUIThread::GetThreadId() const
{
	return ThreadId.load(std::memory_order_acquire);
}

uint64 FVaCuusUIThread::GetFrameCount() const
{
	return FrameCount.load(std::memory_order_acquire);
}

bool FVaCuusUIThread::WaitForFrameCount(uint64 Target, double TimeoutSeconds)
{
	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;

	while (GetFrameCount() < Target)
	{
		if (FPlatformTime::Seconds() >= Deadline)
		{
			return false;
		}

		FPlatformProcess::Sleep(0.001f);
	}

	return true;
}

bool FVaCuusUIThread::IsInUIThread()
{
	const uint32 UIThreadId = GVaCuusUIThreadId.load(std::memory_order_relaxed);
	return UIThreadId != 0 && FPlatformTLS::GetCurrentThreadId() == UIThreadId;
}

bool FVaCuusUIThread::Init()
{
	// Runs on the worker thread. FRunnableThread::Create() waits for this to return,
	// so everything published here is visible to the caller once Start() succeeds.
	const uint32 CurrentThreadId = FPlatformTLS::GetCurrentThreadId();
	ThreadId.store(CurrentThreadId, std::memory_order_release);
	GVaCuusUIThreadId.store(CurrentThreadId, std::memory_order_release);

	// UI-thread-affine subsystem boot (RmlUi, script runtime) belongs here.
	return true;
}

uint32 FVaCuusUIThread::Run()
{
	while (!bStopRequested.load(std::memory_order_acquire))
	{
		WakeEvent->Wait();

		// Stop() triggers the event to break this wait; do not run a frame for it.
		if (bStopRequested.load(std::memory_order_acquire))
		{
			break;
		}

		RunFrame();
		FrameCount.fetch_add(1, std::memory_order_release);
	}

	return 0;
}

void FVaCuusUIThread::Exit()
{
	// Also runs on the worker thread, immediately after Run() returns -- the
	// FRunnable::Exit() header comment claiming "the aggregating thread" is wrong.
	// This is the teardown hook for anything Init() booted; the destructor is not,
	// because it runs on the owner's thread.
	GVaCuusUIThreadId.store(0, std::memory_order_release);
	ThreadId.store(0, std::memory_order_release);
}

void FVaCuusUIThread::RunFrame()
{
	// Everything the frame body will touch is UI-thread-affine; assert it up front so
	// the first accidental cross-thread call is caught here rather than inside RmlUi.
	check(IsInUIThread());
}
