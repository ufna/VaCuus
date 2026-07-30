// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "HAL/Event.h"
#include "HAL/PlatformTLS.h"
#include "HAL/Runnable.h"
#include "Templates/SharedPointer.h"

#include <atomic>

class FRunnableThread;
class FVaCuusBoundModel;
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

	void EnqueueLoadDocumentFile(uint32 ViewId, const FString& VfsPath, uint64 LoadSerial,
		FIntPoint ViewSize = FIntPoint::ZeroValue);
	void EnqueueLoadDocumentFromMemory(uint32 ViewId, const FString& RmlSource, uint64 LoadSerial, FIntPoint ViewSize = FIntPoint::ZeroValue);
	void EnqueueCloseDocument(uint32 ViewId);
	void EnqueueResize(uint32 ViewId, FIntPoint ViewSize);
	void EnqueueSetVisible(uint32 ViewId, bool bVisible);
	void EnqueueShutdown();

	/**
	 * Creates Model's data model on the view's Rml::Context and binds its variables to the
	 * model's UI-side shadow (M3a). The model is kept until the view is removed, and its
	 * pending updates are applied once per UI frame.
	 *
	 * ENQUEUE IT BEFORE THE VIEW'S FIRST LoadDocument*. RmlUi resolves `data-model` once, in
	 * Element::SetParent, so a model created afterwards attaches to nothing -- see
	 * EVaCuusCommandKind::BindModel and UVaCuusView::BindModel, which warns when it can see a
	 * load has already been asked for.
	 */
	void EnqueueBindModel(uint32 ViewId, const TSharedRef<FVaCuusBoundModel>& Model);

	/**
	 * Asks the UI thread to print its half of `vacuus.DumpModel` (spec 8) for ModelName on this
	 * view, or for every model of the view when ModelName is None.
	 *
	 * IT IS A COMMAND AND NOT AN ACCESSOR BECAUSE OF WHAT IT READS. The UI-side shadow is a
	 * UScriptStruct instance the UI thread writes with no synchronisation -- an FString field in
	 * it is freed and reallocated by every apply that touches it -- so a game-thread getter that
	 * returned its values would be a use-after-free rather than a stale read. The answer
	 * therefore arrives in the log a UI frame later, on the thread that owns the buffer.
	 */
	void EnqueueDumpModel(uint32 ViewId, FName ModelName);

	/**
	 * Drops RmlUi's parsed stylesheet and template caches on the UI thread. Live
	 * reload's actual mechanism (controller decision D21).
	 *
	 * ITS OWN COMMAND, NOT A FLAG ON A LOAD, and that is the correctness point rather
	 * than tidiness. Rml::Factory keys parsed stylesheets and templates on their FILE
	 * NAME in process-global statics that outlive a PIE session (UVaCuusSubsystem's
	 * Deinitialize deliberately leaves this thread running; only
	 * FVaCuusModule::ShutdownModule stops it). A clear that rides on a per-view load
	 * command is therefore lost in exactly the case that needs it most -- an .rcss edited
	 * while no view is live enqueues no load, clears nothing, and the next PIE session
	 * re-parses the RML from disk while taking the STALE stylesheet from the cache.
	 * Being view-less, this command is applied before the drain's per-view lookup, so it
	 * lands whether or not anything is showing.
	 *
	 * One clear then serves every load queued behind it: the queue is FIFO from a single
	 * producer, so ReloadAllLiveViews() enqueues one of these and then fans out.
	 *
	 * NOT part of an ordinary load, on purpose: the caches are what make a second view of
	 * the same document cheap, and clearing on every load would re-parse every stylesheet
	 * each time a view swaps documents.
	 */
	void EnqueueClearAssetCaches();

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

	/**
	 * Data models bound across every view. Safe from any thread.
	 *
	 * The bind's only observable, and it is needed for the same reason
	 * GetNumAssetCacheClears() is: a bind that never happened produces a model whose values go
	 * nowhere and a document that reads empty, with nothing to ask about it. The UI thread
	 * cannot report it to the game thread, because a BindModel command carries no serial; and
	 * RmlUi cannot report it either -- NOT because its logging is compiled out (it is not; see
	 * FVaCuusSystemInterface::LogMessage) but because a bind that never happened makes no RmlUi
	 * call, so there is nothing for the library to refuse. This counter is the only way to
	 * distinguish "bound and idle" from "never bound".
	 */
	int32 GetNumBoundModels() const;

	/**
	 * How many ClearAssetCaches commands this thread has applied. Safe from any thread.
	 *
	 * The ONLY observable the clear has: RmlUi exposes no way to ask whether its
	 * stylesheet/template caches are populated (Factory has Clear* and nothing else,
	 * ThirdParty/RmlUi/Include/RmlUi/Core/Factory.h:133-135). Without this counter the
	 * two properties live reload depends on -- a clear happens even when zero views
	 * reload, and a fan-out over N views clears ONCE rather than N times -- would be
	 * untestable, which is how the "flag on the load command" bug survived review.
	 */
	uint64 GetNumAssetCacheClears() const;

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

	/**
	 * Applies every bound model's newest published update -- copy each dirty field into the
	 * model's UI shadow, dirty its top-level name -- and echoes the applied generation back.
	 * UI thread; the frame's DataApply phase.
	 */
	void ApplyModelUpdates();

	/** AddView/RemoveView handlers, split out because both are more than a line. */
	void AddView(FVaCuusUICommand& Command);
	void RemoveView(uint32 ViewId);

	/** BindModel handler: creates the model on the view's context and keeps it. UI thread. */
	void BindModel(uint32 ViewId, IVaCuusDocumentHost& Host, const TSharedPtr<FVaCuusBoundModel>& Model);

	/** DumpModel handler: prints the UI-side half of every matching model, or says there is none. UI thread. */
	void DumpModel(uint32 ViewId, FName ModelName);

	/** ClearAssetCaches handler: drops RmlUi's global stylesheet/template caches. UI thread. */
	void ClearAssetCaches();

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

	/**
	 * Data models bound per view (M3a), in bind order. UI thread only.
	 *
	 * KEYED ON THE VIEW rather than held by the host, and that is the same seam input dispatch
	 * already uses (IVaCuusDocumentHost::GetContext): the whole RmlUi data-binding vocabulary
	 * -- the definitions, the registry, DataModelConstructor -- lives in VaCuus, next to the
	 * UE reflection it is built from, and a BindModel(...) method on the host interface would
	 * push all of it into whichever module implements the host (VaCuusRender today).
	 *
	 * A model's entry is dropped in RemoveView(), AFTER the host's Shutdown() has destroyed
	 * the context: RmlUi retains a raw void* into each model's UI shadow and revalidates it
	 * never, so the buffer must outlive the context that points at it. The game thread holds
	 * its own reference to the same object, so this drop is a refcount decrement rather than a
	 * destruction in the common case.
	 */
	TMap<uint32, TArray<TSharedRef<FVaCuusBoundModel>>> Models;

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

	/** Backs GetNumBoundModels(); mirrors the flattened size of Models. */
	std::atomic<int32> NumBoundModels{0};

	/** Applied ClearAssetCaches commands; see GetNumAssetCacheClears(). */
	std::atomic<uint64> NumAssetCacheClears{0};

	/** Process-unique view ids; shared by every subsystem that uses this thread. */
	std::atomic<uint32> NextViewId{1};
};
