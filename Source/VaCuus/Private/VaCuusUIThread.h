// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "HAL/Event.h"
#include "HAL/PlatformTLS.h"
#include "HAL/Runnable.h"

#include <atomic>

class FRunnableThread;

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
 * therefore live between Init() and Exit(), never in the constructor/destructor.
 *
 * Not thread-safe to Start()/destroy concurrently: those belong to the owner
 * (game thread). Trigger(), the getters and IsInUIThread() are safe from anywhere.
 */
class FVaCuusUIThread final : public FRunnable
{
public:
	FVaCuusUIThread();

	/** Stops the worker and joins it before any member is destroyed. */
	virtual ~FVaCuusUIThread() override;

	FVaCuusUIThread(const FVaCuusUIThread&) = delete;
	FVaCuusUIThread& operator=(const FVaCuusUIThread&) = delete;

	/**
	 * Spawns the worker thread. Single-shot: this instance cannot be restarted
	 * after Stop().
	 *
	 * Returns false when the thread could not be created, which in practice means
	 * FPlatformProcess::SupportsMultithreading() is false (commandlets, -nothreading).
	 * Callers must fall back to running UI frames inline on their own thread.
	 */
	bool Start();

	/** Wakes the worker for one UI frame. Safe from any thread; coalescing. */
	void Trigger();

	/** True while the worker exists and no stop has been requested. */
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
	/** Runs a single UI frame on the worker thread. A stub until the frame body lands. */
	void RunFrame();

	/** Owned; deleted (which stops and joins) first of all in the destructor. */
	FRunnableThread* Thread = nullptr;

	/** Auto-reset == coalescing latch. Non-movable, and must outlive Thread. */
	FEventRef WakeEvent{EEventMode::AutoReset};

	std::atomic<bool> bStopRequested{false};
	std::atomic<uint32> ThreadId{0};
	std::atomic<uint64> FrameCount{0};
};
