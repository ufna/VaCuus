// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "HAL/Event.h"
#include "HAL/PlatformTLS.h"
#include "HAL/Runnable.h"

#include <atomic>

class FRunnableThread;
class IVaCuusDocumentHost;
struct FVaCuusUICommand;
struct FVaCuusUIQueues;

/**
 * The dedicated VaCuus UI thread.
 *
 * Owns a single worker thread that sleeps on an auto-reset event and runs exactly
 * one UI frame per wakeup. Because the event is a binary latch, several Trigger()
 * calls landing while a frame is in flight coalesce into a single extra frame --
 * the thread never spins and never queues up backlog.
 *
 * Init(), Run() and Exit() all execute on the worker thread; Stop() is the only
 * FRunnable entry point invoked from the caller's thread. RmlUi and script state
 * therefore live between Init() and Exit(), never in the constructor/destructor:
 * the document host handed over by SetDocumentHost() is booted in Init(), driven
 * by RunFrame() and destroyed in Exit().
 *
 * The frame body publishes its command buffer STRAIGHT to the render thread
 * (ENQUEUE_RENDER_COMMAND has no game-thread requirement), so a UI frame never
 * touches the game thread at all.
 *
 * Not thread-safe to Start()/destroy concurrently: those belong to the owner
 * (game thread), which is also the single producer for the command queue. The
 * Enqueue* methods are for that owner's thread only; Trigger(), the getters and
 * IsInUIThread() are safe from anywhere.
 *
 * NOTE this header is Public only because Task 3's owner of the UI thread is the
 * vacuus.M1HUD console command over in VaCuusRender; once UVaCuusSubsystem
 * (Task 4, in this module) takes ownership it can move back to Private.
 */
class VACUUS_API FVaCuusUIThread final : public FRunnable
{
public:
	FVaCuusUIThread();

	/** Stops the worker and joins it before any member is destroyed. */
	virtual ~FVaCuusUIThread() override;

	FVaCuusUIThread(const FVaCuusUIThread&) = delete;
	FVaCuusUIThread& operator=(const FVaCuusUIThread&) = delete;

	/**
	 * Hands over the object that owns the documents. Must be called before
	 * Start(): the host is booted inside Init() and destroyed inside Exit(), so
	 * every call it ever sees comes from the UI thread.
	 */
	void SetDocumentHost(TUniquePtr<IVaCuusDocumentHost> InHost);

	/**
	 * Spawns the worker thread. Single-shot: this instance cannot be restarted
	 * after Stop().
	 *
	 * Returns false when the thread could not be created (in practice
	 * FPlatformProcess::SupportsMultithreading() is false -- commandlets,
	 * -nothreading) or when the document host failed to boot. Both cases leave no
	 * thread behind; callers must fall back to running UI frames inline or give up
	 * on the view.
	 */
	bool Start();

	/** Wakes the worker for one UI frame. Safe from any thread; coalescing. */
	void Trigger();

	//~ Command producers. Owner's thread only (the queue is SPSC). Dropped once a
	//~ stop has been requested -- teardown stops accepting commands before it joins.
	void EnqueueLoadDocumentFile(const FString& VfsPath, FIntPoint ViewSize);
	void EnqueueLoadDocumentFromMemory(const FString& RmlSource, FIntPoint ViewSize);
	void EnqueueCloseDocument();
	void EnqueueResize(FIntPoint ViewSize);
	void EnqueueShutdown();

	/** True while the worker thread is live and no stop has been requested. */
	bool IsRunning() const;

	/** OS id of the worker thread, or 0 while no worker is live. */
	uint32 GetThreadId() const;

	/** Number of UI frames completed since Start(). */
	uint64 GetFrameCount() const;

	/** Blocks until GetFrameCount() >= Target. Returns false on timeout. Test helper. */
	bool WaitForFrameCount(uint64 Target, double TimeoutSeconds);

	/** True when the calling thread is the VaCuus UI thread. Backs the check() wrappers. */
	static bool IsInUIThread();

	//~ Begin FRunnable interface
	/** Asks the worker to leave its loop and wakes it. Idempotent, and safe after it exited. */
	virtual void Stop() override;
	//~ End FRunnable interface

protected:
	//~ Begin FRunnable interface
	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Exit() override;
	//~ End FRunnable interface

private:
	/** Runs a single UI frame on the worker thread: drain commands, record, publish. */
	void RunFrame();

	/** Applies every queued command, in order. UI thread. */
	void DrainCommands();

	/** Pushes one command, or drops it if the worker is already stopping. Owner's thread. */
	void Enqueue(FVaCuusUICommand&& Command);

	/** Owned; deleted (which stops and joins) first of all in the destructor. */
	FRunnableThread* Thread = nullptr;

	/**
	 * Owns the documents; boots in Init(), dies in Exit(). Opaque here so RmlUi
	 * never reaches this header. Only the UI thread may dereference it once the
	 * worker is live.
	 */
	TUniquePtr<IVaCuusDocumentHost> Host;

	/** Game thread -> UI thread transport; allocated in the constructor, never null. */
	TUniquePtr<FVaCuusUIQueues> Queues;

	/** Auto-reset == coalescing latch. Non-movable, and must outlive Thread. */
	FEventRef WakeEvent{EEventMode::AutoReset};

	std::atomic<bool> bStopRequested{false};

	/**
	 * Published by Init() before it returns. FRunnableThread::Create() hands back
	 * a valid pointer even when Init() returned false (the worker then exits
	 * without ever running Run() or Exit()), so this flag is the only way Start()
	 * can tell a live thread from a stillborn one.
	 */
	std::atomic<bool> bInitSucceeded{false};

	/**
	 * True between a successful Start() and the destructor's join. Backs
	 * IsRunning() so it never reads the non-atomic Thread pointer.
	 */
	std::atomic<bool> bThreadLive{false};

	std::atomic<uint32> ThreadId{0};
	std::atomic<uint64> FrameCount{0};
};
