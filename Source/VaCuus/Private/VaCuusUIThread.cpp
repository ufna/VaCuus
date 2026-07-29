// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusUIThread.h"

#include "VaCuusDefines.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusUIQueues.h"

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

/**
 * Stack size for the UI thread, chosen rather than inherited: RmlUi's layout and
 * style resolution recurse with the document tree and QuickJS lands on this
 * thread in M4, so the platform default is not obviously enough -- while the
 * default 8 MB per thread is far more than a UI tree needs. Unix clamps any
 * non-zero request up to at least 128 KB (UnixPlatformRunnableThread.cpp), so
 * this value survives as given.
 */
constexpr uint32 GVaCuusUIThreadStackSize = 512 * 1024;
}	 // namespace

FVaCuusUIThread::FVaCuusUIThread()
	: Queues(MakeUnique<FVaCuusUIQueues>())
{
}

FVaCuusUIThread::~FVaCuusUIThread()
{
	Stop();

	// Report "not live" before the join: from here on the object is going away,
	// and IsRunning() must not tempt anyone into using it.
	bThreadLive.store(false, std::memory_order_release);

	// The platform destructor does Kill(true), i.e. Stop() then join, so Run() and
	// Exit() have both finished by the time this returns -- which is what makes it
	// safe for Exit() to own the RmlUi teardown. Destroying the thread before any
	// member dies is what keeps WakeEvent alive for its last waiter.
	delete Thread;
	Thread = nullptr;

	// Normally Exit() already dropped the host on the UI thread. It is still set
	// only when the worker never ran (Start() not called, or thread creation
	// failed), in which case nothing was ever booted and the game thread may
	// destroy it safely.
}

void FVaCuusUIThread::SetDocumentHost(TUniquePtr<IVaCuusDocumentHost> InHost)
{
	checkf(Thread == nullptr, TEXT("SetDocumentHost() must be called before Start()"));
	Host = MoveTemp(InHost);
}

bool FVaCuusUIThread::Start()
{
	if (Thread != nullptr)
	{
		UE_LOG(LogVaCuus, Warning, TEXT("UI thread is already started"));
		return true;
	}

	// Name stays within 15 characters: Linux truncates the OS thread name there.
	// BelowNormal keeps us off the game and render threads' backs.
	Thread = FRunnableThread::Create(
		this, TEXT("VaCuusUI"), GVaCuusUIThreadStackSize, TPri_BelowNormal);

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

	// Create() blocks on the init sync event, which the platform thread proc
	// triggers on BOTH branches of Init() -- so bInitSucceeded is readable here,
	// and a false means the worker already exited without running Run() or
	// Exit(). Without this check Start() would report success for a dead thread.
	if (!bInitSucceeded.load(std::memory_order_acquire))
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("The VaCuus UI thread exited during Init(); no UI frames will run"));

		// Kill(true) + join on a worker that has already returned; Init() unwound
		// whatever it had built before failing, because Exit() never ran.
		delete Thread;
		Thread = nullptr;
		return false;
	}

	bThreadLive.store(true, std::memory_order_release);

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

void FVaCuusUIThread::EnqueueLoadDocumentFile(const FString& VfsPath, FIntPoint ViewSize)
{
	Enqueue(FVaCuusUICommand{EVaCuusCommandKind::LoadDocumentFile, VfsPath, ViewSize});
}

void FVaCuusUIThread::EnqueueLoadDocumentFromMemory(const FString& RmlSource, FIntPoint ViewSize)
{
	Enqueue(FVaCuusUICommand{EVaCuusCommandKind::LoadDocumentMemory, RmlSource, ViewSize});
}

void FVaCuusUIThread::EnqueueCloseDocument()
{
	Enqueue(FVaCuusUICommand{EVaCuusCommandKind::CloseDocument});
}

void FVaCuusUIThread::EnqueueResize(FIntPoint ViewSize)
{
	Enqueue(FVaCuusUICommand{EVaCuusCommandKind::Resize, FString(), ViewSize});
}

void FVaCuusUIThread::EnqueueShutdown()
{
	Enqueue(FVaCuusUICommand{EVaCuusCommandKind::Shutdown});
}

void FVaCuusUIThread::Enqueue(FVaCuusUICommand&& Command)
{
	// Spec §4 teardown order starts with "stop accepting commands": once a stop is
	// requested the queue is closed, so nothing can be pushed behind the drain that
	// the worker will never see (and, worse, that would keep a document alive past
	// the close in Exit()).
	if (bStopRequested.load(std::memory_order_acquire))
	{
		UE_LOG(LogVaCuus, Verbose, TEXT("UI command dropped: the UI thread is stopping"));
		return;
	}

	Queues->Commands.Enqueue(MoveTemp(Command));
	Trigger();
}

bool FVaCuusUIThread::IsRunning() const
{
	// Both halves are atomic, so this answer is safe from any thread (the old
	// version read the plain Thread pointer). Stop() asks the worker to leave; it
	// does not join. Reporting "not running" from the moment the request lands is
	// the only answer that does not race the worker's own teardown, and it is what
	// callers actually want to gate work on.
	return bThreadLive.load(std::memory_order_acquire) && !bStopRequested.load(std::memory_order_acquire);
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

	// UI-thread-affine subsystem boot: RmlUi (and, from M4, the script runtime)
	// comes up here and nowhere else.
	if (Host.IsValid() && !Host->Initialize())
	{
		// Exit() will NOT run when Init() fails, so this is the only chance to
		// unwind. Initialize() rolls its own partial state back; all that is left is
		// to drop the host (on this thread, while it is still the UI thread) and to
		// retract the thread-id publication.
		Host.Reset();
		GVaCuusUIThreadId.store(0, std::memory_order_release);
		ThreadId.store(0, std::memory_order_release);

		bInitSucceeded.store(false, std::memory_order_release);
		return false;
	}

	// Set last, and before returning: Start() reads it as soon as Create() returns.
	bInitSucceeded.store(true, std::memory_order_release);
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
	if (Host.IsValid())
	{
		// Documents close, the context is removed and RmlUi shuts down here, on the
		// thread that created all of it. Resetting rather than just shutting down
		// also runs the host's destructor on this thread.
		Host->Shutdown();
		Host.Reset();
	}

	GVaCuusUIThreadId.store(0, std::memory_order_release);
	ThreadId.store(0, std::memory_order_release);
}

void FVaCuusUIThread::RunFrame()
{
	// Everything below is UI-thread-affine; assert it up front so the first
	// accidental cross-thread call is caught here rather than inside RmlUi.
	check(IsInUIThread());

	DrainCommands();
	// (input drain: Task 6; data snapshots: M3)

	if (!Host.IsValid() || !Host->HasView())
	{
		return;
	}

	// Records the frame and publishes the command buffer straight to the render
	// thread -- no game-thread hop. (Interactive snapshot publish: Task 5.)
	Host->RecordAndPublishFrame();
}

void FVaCuusUIThread::DrainCommands()
{
	check(IsInUIThread());

	while (TOptional<FVaCuusUICommand> Command = Queues->Commands.Dequeue())
	{
		// Applied first and for every kind: a document then loads straight into the
		// right layout size. SetViewSize() is idempotent, so a burst of resize
		// commands costs exactly one relayout -- that is the coalescing.
		if (Host.IsValid() && Command->ViewSize.X > 0 && Command->ViewSize.Y > 0)
		{
			Host->SetViewSize(Command->ViewSize);
		}

		switch (Command->Kind)
		{
			case EVaCuusCommandKind::LoadDocumentFile:
				if (Host.IsValid())
				{
					Host->LoadDocumentFromFile(Command->Payload);
				}
				break;

			case EVaCuusCommandKind::LoadDocumentMemory:
				if (Host.IsValid())
				{
					Host->LoadDocumentFromMemory(Command->Payload);
				}
				break;

			case EVaCuusCommandKind::CloseDocument:
				if (Host.IsValid())
				{
					Host->CloseDocument();
				}
				break;

			case EVaCuusCommandKind::Resize:
				// Nothing left to do: the view size was applied above.
				break;

			case EVaCuusCommandKind::Shutdown:
			{
				// In-band graceful stop: close the document now, then leave the loop
				// after this frame. The owner still joins us through the destructor,
				// and Exit() still runs the full teardown.
				if (Host.IsValid())
				{
					Host->CloseDocument();
				}
				bStopRequested.store(true, std::memory_order_release);

				// Anything queued behind a shutdown is dead by definition; drop it
				// here and say how much, so the loss is as visible as Enqueue()'s.
				int32 NumDropped = 0;
				while (Queues->Commands.Dequeue())
				{
					++NumDropped;
				}
				UE_LOG(LogVaCuus, Verbose,
					TEXT("UI thread stopping on an in-band shutdown command; dropped %d queued command(s) behind it"),
					NumDropped);
				return;
			}
		}
	}
}
