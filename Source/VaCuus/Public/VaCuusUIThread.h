// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "HAL/Event.h"
#include "HAL/PlatformTLS.h"
#include "HAL/Runnable.h"
#include "Templates/SharedPointer.h"

#include <atomic>

class FRunnableThread;
class FVaCuusEngine;
class IVaCuusDocumentHost;
struct FVaCuusInputEvent;
struct FVaCuusUICommand;
struct FVaCuusUIQueues;
struct FVaCuusViewStatus;

/**
 * The dedicated VaCuus UI thread -- one per PROCESS, owned by FVaCuusModule.
 *
 * WHY ONE PER PROCESS (supersedes spec §4's original "one per UVaCuusSubsystem"):
 * RmlUi keeps its system/file/render interfaces, its `initialised` flag and its
 * whole CoreData -- including the context registry -- in process-global statics
 * (ThirdParty/RmlUi/Source/Core/Core.cpp), and the library is single-thread
 * affine. Two UI threads would both have to own that one global state. Views are
 * the per-instance unit instead: Rml::CreateContext() takes a per-context render
 * interface, so N views on this one thread get N contexts, N recorders and N
 * command buffers. Multi-PIE is then N subsystems, 1 UI thread, N views.
 *
 * The thread sleeps on an auto-reset event and runs exactly one UI frame per
 * wakeup. Because the event is a binary latch, several Trigger() calls landing
 * while a frame is in flight coalesce into a single extra frame -- the thread
 * never spins and never queues up backlog.
 *
 * Init(), Run() and Exit() all execute on the worker thread; Stop() is the only
 * FRunnable entry point invoked from the caller's thread. RmlUi is therefore
 * booted in Init() and shut down in Exit(), and every document host lives
 * entirely between them.
 *
 * The frame body publishes each view's command buffer STRAIGHT to the render
 * thread (ENQUEUE_RENDER_COMMAND has no game-thread requirement) and its
 * interactive-region snapshot into the view's shared FVaCuusViewStatus, so a UI
 * frame never touches the game thread at all.
 *
 * INLINE FALLBACK: FRunnableThread::Create() returns nullptr, silently, when
 * FPlatformProcess::SupportsMultithreading() is false (commandlets,
 * -nothreading). Start() reports that; the owner then calls StartInline() and
 * drives RunFrameInline() from its tick instead. Both record the calling thread
 * as the RmlUi owner for the duration of the call, so the affinity guards keep
 * working unchanged.
 *
 * Not thread-safe to Start()/destroy concurrently: those belong to the owner
 * (game thread), which is also the single producer for the command queue. The
 * Enqueue* methods are for that owner's thread only; Trigger(), the getters and
 * IsInUIThread() are safe from anywhere.
 */
class VACUUS_API FVaCuusUIThread final : public FRunnable
{
public:
	/**
	 * InEngine is borrowed and must outlive this thread. Taken by reference rather
	 * than looked up: FVaCuusEngine::Get() goes through FModuleManager, which in 5.8
	 * refuses to hand a module out to a non-game thread (and hard-asserts while that
	 * module is being unloaded -- exactly when Exit() runs).
	 */
	explicit FVaCuusUIThread(FVaCuusEngine& InEngine);

	/** Stops the worker and joins it before any member is destroyed. */
	virtual ~FVaCuusUIThread() override;

	FVaCuusUIThread(const FVaCuusUIThread&) = delete;
	FVaCuusUIThread& operator=(const FVaCuusUIThread&) = delete;

	/**
	 * Spawns the worker thread and boots RmlUi on it. Single-shot: this instance
	 * cannot be restarted after Stop().
	 *
	 * Returns false when the thread could not be created (in practice
	 * FPlatformProcess::SupportsMultithreading() is false -- commandlets,
	 * -nothreading) or when RmlUi failed to boot (e.g. an automation test owns the
	 * library on another thread). Both cases leave no thread behind; the caller
	 * falls back to StartInline() for the first, and gives up for the second.
	 */
	bool Start();

	/**
	 * Inline fallback for platforms/configurations without real threads: runs the
	 * same boot as Start() on the CALLING thread, which then becomes the RmlUi
	 * owner and must be the one calling RunFrameInline(). Game thread only.
	 */
	bool StartInline();

	/** True when this instance runs its frames inline on the game thread. */
	bool IsInlineMode() const;

	/**
	 * One UI frame on the calling thread. Inline mode only, game thread only. For
	 * the duration of the call the game thread is recorded as the RmlUi owner, so
	 * the check(IsInUIThread()) guards downstream stay meaningful outside it.
	 */
	void RunFrameInline();

	/**
	 * The graceful half of teardown, and the path FVaCuusModule::StopUIThread()
	 * takes: queues the in-band Shutdown command and waits (bounded) for the worker
	 * to drain it, so every document is closed and anything still queued is dropped
	 * *with a log* on the UI thread, before any join happens.
	 *
	 * Returns false when the worker did not get there in time (or the queue was
	 * already closed by a hard Stop()); the caller then falls back to Stop()+join,
	 * which is exactly what the destructor does anyway. Game thread only.
	 */
	bool RequestGracefulShutdown(double TimeoutSeconds);

	/** True once a stop has been requested; the command queue is closed from then on. */
	bool IsStopping() const;

	/** Wakes the worker for one UI frame. Safe from any thread; coalescing. No-op inline. */
	void Trigger();

	/** Hands out the next process-unique view id. Safe from any thread. */
	uint32 AllocateViewId();

	//~ Command producers. Owner's thread only (the queue is SPSC). Dropped once a
	//~ stop has been requested -- teardown stops accepting commands before it joins.

	/** Registers a view: the host is booted on the UI thread under ViewId. */
	void EnqueueAddView(uint32 ViewId, TUniquePtr<IVaCuusDocumentHost> Host, FIntPoint ViewSize,
		const TSharedRef<FVaCuusViewStatus>& Status);

	/** Retires a view; the rest of the views keep running. */
	void EnqueueRemoveView(uint32 ViewId);

	void EnqueueLoadDocumentFile(uint32 ViewId, const FString& VfsPath, uint64 LoadSerial, FIntPoint ViewSize = FIntPoint::ZeroValue);
	void EnqueueLoadDocumentFromMemory(uint32 ViewId, const FString& RmlSource, uint64 LoadSerial, FIntPoint ViewSize = FIntPoint::ZeroValue);
	void EnqueueCloseDocument(uint32 ViewId);
	void EnqueueResize(uint32 ViewId, FIntPoint ViewSize);
	void EnqueueSetVisible(uint32 ViewId, bool bVisible);
	void EnqueueShutdown();

	/**
	 * Queues one input event for a view. Stamps ViewId, so the caller only fills in
	 * what the event is (see FVaCuusInputEvent's factories).
	 *
	 * Deliberately does NOT wake the UI thread: input is consumed by the next frame,
	 * which UVaCuusSubsystem::Tick already asks for once per game frame. Waking here
	 * would turn a mouse drag into one UI frame per motion event -- strictly more
	 * work for a document that is going to be laid out once this frame anyway.
	 */
	void EnqueueInput(uint32 ViewId, FVaCuusInputEvent Event);

	/** True while the worker thread is live and no stop has been requested. */
	bool IsRunning() const;

	/** OS id of the worker thread, or 0 while no worker is live. */
	uint32 GetThreadId() const;

	/** Number of UI frames completed since Start(). */
	uint64 GetFrameCount() const;

	/** Number of views currently registered. Safe from any thread. */
	int32 GetNumViews() const;

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
	/** Runs a single UI frame on the worker thread: drain commands, record every view, publish. */
	void RunFrame();

	/** Applies every queued command, in order, routing each to its view. UI thread. */
	void DrainCommands();

	/**
	 * Dispatches every queued input event into its view's context, in order.
	 * Runs after DrainCommands() and before any view is updated, so an event always
	 * reaches a context that is already the right size and has the right document.
	 */
	void DrainInput();

	/** AddView/RemoveView handlers, split out because both are more than a line. */
	void AddView(FVaCuusUICommand& Command);
	void RemoveView(uint32 ViewId);

	/** Empties the command queue without applying anything; returns how much was lost. UI thread. */
	int32 DrainAndDiscardCommands();

	/** Host for ViewId, or null. UI thread. */
	IVaCuusDocumentHost* FindHost(uint32 ViewId) const;

	/** Pushes one command, or drops it if the worker is already stopping. Owner's thread. */
	void Enqueue(FVaCuusUICommand&& Command);

	/** The RmlUi library wrapper this thread boots in Init() and tears down in Exit(). */
	FVaCuusEngine& Engine;

	/** Owned; deleted (which stops and joins) first of all in the destructor. */
	FRunnableThread* Thread = nullptr;

	/**
	 * Live views, keyed by the id every command carries. Booted when AddView is
	 * drained, destroyed in Exit(). Only the UI thread may touch the hosts.
	 */
	TMap<uint32, TUniquePtr<IVaCuusDocumentHost>> Hosts;

	/**
	 * Hosts whose view was removed. They are shut down (context gone, nothing
	 * recorded) but deliberately not destroyed: RmlUi keeps a RenderManager keyed
	 * on each host's render interface until Rml::Shutdown(), and destroys it --
	 * releasing font textures through that pointer -- only then. Dropped in
	 * Exit(), after the library is down.
	 */
	TArray<TUniquePtr<IVaCuusDocumentHost>> RetiredHosts;

	/** Game thread -> UI thread transport; allocated in the constructor, never null. */
	TUniquePtr<FVaCuusUIQueues> Queues;

	/** Auto-reset == coalescing latch. Non-movable, and must outlive Thread. */
	FEventRef WakeEvent{EEventMode::AutoReset};

	std::atomic<bool> bStopRequested{false};

	/**
	 * Set by the drain once the in-band Shutdown command has been processed. It is
	 * what RequestGracefulShutdown() waits on, and what tells the difference between
	 * "the worker closed everything itself" and "we had to hard-stop it".
	 */
	std::atomic<bool> bShutdownDrained{false};

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

	/** Set by StartInline(); makes the destructor tear down on the calling thread. */
	bool bInlineMode = false;

	std::atomic<uint32> ThreadId{0};
	std::atomic<uint64> FrameCount{0};
	std::atomic<int32> NumViews{0};

	/** Process-unique view ids; shared by every subsystem that uses this thread. */
	std::atomic<uint32> NextViewId{1};
};
