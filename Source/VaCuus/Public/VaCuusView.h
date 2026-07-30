// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "UObject/Object.h"

#include "VaCuusInteractiveSnapshot.h"

#include "Templates/SharedPointer.h"

#include "VaCuusView.generated.h"

class FGenericWindow;
class FVaCuusImeHandler;
class FVaCuusUIThread;
class ITextInputMethodContext;
class ITextInputMethodSystem;
class UVaCuusSubsystem;
struct FVaCuusInputEvent;
struct FVaCuusViewStatus;

/**
 * Everything the platform IME needs to know about WHERE a view is drawn, as plain data.
 *
 * WHY IT IS A STRUCT AND NOT AN FGeometry: `ITextInputMethodContext` wants Slate ABSOLUTE
 * (desktop) pixels, RmlUi gives view pixels, and the transform between them belongs to the
 * Slate host -- which is in another module. Passing the two rectangles instead of the
 * geometry keeps VaCuus free of a Slate dependency (every type below is ApplicationCore or
 * Core) and keeps the mapping in one line the host can be held to:
 *
 *     Absolute = AbsolutePosition + ViewPixel * (AbsoluteSize / ViewPixelSize)
 *
 * which is exact for the axis-aligned translate+scale geometry a viewport overlay or a UMG
 * slot ever has, and degenerates safely (no caret) when ViewPixelSize is zero.
 *
 * D17 -- WORLD-SPACE SURFACES MUST NOT SUPPLY ONE. There is no valid Slate-absolute mapping
 * for a UI drawn on a mesh in the world: the "screen position" of a caret on a rotating
 * quad is a projection that changes with the camera, and an IME candidate window anchored to
 * it would chase the player's head. A UVaCuusWorldComponent-style host therefore leaves
 * TextInputMethodMethodSystem null (or simply never calls UVaCuusView::UpdateIme), which
 * degrades to exactly the Linux path -- OnKeyChar -> ProcessTextInput, no composition.
 * M2 ships no world component, so this is a documented constraint rather than code.
 */
struct FVaCuusImeSurface
{
	/**
	 * The platform's text-input method system, or NULL when the platform has none.
	 *
	 * Supplied by the host (FSlateApplication::GetTextInputMethodSystem()) rather than looked
	 * up here, because that accessor is in the Slate module. Null is not an error: it is the
	 * normal state on Linux -- FLinuxApplication never overrides
	 * GenericApplication::GetTextInputMethodSystem() -- and on any world-space host (D17).
	 * Everything downstream then no-ops and typing degrades to the OnKeyChar path.
	 */
	ITextInputMethodSystem* TextInputMethodSystem = nullptr;

	/** Native window the surface is drawn in; what ITextInputMethodContext::GetWindow answers. */
	TSharedPtr<FGenericWindow> NativeWindow;

	/** The host widget's rect in Slate ABSOLUTE (desktop) pixels. */
	FVector2D AbsolutePosition = FVector2D::ZeroVector;
	FVector2D AbsoluteSize = FVector2D::ZeroVector;

	/** The view's own pixel size -- the space RmlUi's coordinates and the snapshot rects use. */
	FIntPoint ViewPixelSize = FIntPoint::ZeroValue;

	/** True while the host holds Slate keyboard focus; false deactivates the context. */
	bool bHostHasFocus = false;
};

/**
 * Broadcast on the GAME thread for the NEWEST completed document load.
 *
 * Latest completion wins. The UI thread can finish several queued loads inside one
 * drain, and the status carries one (serial, result) pair, so the game thread's
 * next poll sees only the last of them: intermediate completions are coalesced away
 * and a superseded load is never reported. That is the honest semantic, and the
 * right one for the only thing the result is used for -- "is the document that is
 * up now the one I last asked for". Callers that need to know their own request was
 * overtaken can compare UVaCuusView::GetLastRequestedLoadSerial() against
 * GetLastCompletedLoadSerial(); coalescing is logged (Verbose) as it happens.
 */
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnVaCuusViewLoadCompleted, UVaCuusView* /*View*/, bool /*bSuccess*/);

/**
 * Handle to one UI view: one Rml::Context living on the process-wide UI thread.
 *
 * Owns nothing thread-affine. Every method here is a non-blocking enqueue onto
 * the UI thread's command queue, stamped with this view's id so the UI thread
 * routes it to the right context; the answers come back through a shared
 * FVaCuusViewStatus that UVaCuusSubsystem::Tick polls (see PollStatus()).
 *
 * Created and destroyed exclusively through UVaCuusSubsystem, which is also this
 * object's outer and which invalidates it (Invalidate()) when the view is gone --
 * after that every call here is a logged no-op, so a widget or console command
 * holding on to the handle cannot reach a dead context.
 *
 * Game thread only.
 */
UCLASS(BlueprintType)
class VACUUS_API UVaCuusView : public UObject
{
	GENERATED_BODY()

public:
	/** Newest completed load only; see the delegate's own comment for the coalescing rule. */
	FOnVaCuusViewLoadCompleted OnLoadCompleted;

	/**
	 * Called by UVaCuusSubsystem::CreateView() right after the AddView command is
	 * queued. InInitialViewSize is the size that command already carried, so the
	 * first widget Tick does not re-send it.
	 */
	void InitializeView(UVaCuusSubsystem* InSubsystem, uint32 InViewId,
		const TSharedRef<FVaCuusViewStatus>& InStatus, FIntPoint InInitialViewSize);

	/** Called by the subsystem once the view has been removed from the UI thread. */
	void Invalidate();

	/**
	 * Loads a document through the RmlUi file interface (paths resolve against the
	 * ordered DevUI roots -- see VaCuusContentPaths.h). Asynchronous: OnLoadCompleted
	 * reports the outcome.
	 */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	void LoadDocument(const FString& VfsPath);

	/**
	 * The VFS path of the document this view was last asked to load from FILE, or empty
	 * when there is none (never loaded, loaded from memory, or closed).
	 *
	 * IT IS THE RELOAD KEY, which is the only reason it is remembered at all: live
	 * reload has to re-issue a load without knowing who asked for the first one, and
	 * neither the widget (whose own copy is guarded, see UVaCuusWidget) nor the UI thread
	 * (which keeps an Rml::ElementDocument, not a path) can answer for it.
	 *
	 * DELIBERATELY EMPTY AFTER LoadDocumentFromMemory(): an inline document has no file
	 * to have changed, so reloading it would only destroy and rebuild identical content
	 * -- and would silently undo a fallback (vacuus.M1HUD's inline document is exactly
	 * that case) by re-loading the file that failed.
	 */
	const FString& GetDocumentPath() const { return DocumentPath; }

	/**
	 * Re-issues the last file load, forcing RmlUi to re-read the document (controller
	 * decision D21's editor live reload, and half of what vacuus.ReloadUI does).
	 *
	 * THE OTHER HALF IS NOT HERE, and the split is deliberate: an RML re-read alone shows
	 * stale CSS, because Rml::Factory keys parsed stylesheets on their file name and hands
	 * the cached one back. Dropping that cache is a PROCESS-WIDE act that must also happen
	 * when no view is live at all (an .rcss edited between PIE sessions), so it belongs to
	 * the dispatcher -- FVaCuusUIThread::EnqueueClearAssetCaches(), which
	 * FVaCuusLiveReload::ReloadAllLiveViews() enqueues once before fanning out over the
	 * views. Calling this directly, without that clear, re-reads the RML only.
	 *
	 * Returns false when there is nothing to reload (invalid view, or no file document).
	 */
	bool ReloadDocument();

	/** Same, from RML source text. */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	void LoadDocumentFromMemory(const FString& RmlSource);

	/** Closes the current document; the view (and its context) stays alive. */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	void Close();

	/**
	 * Shows or hides the document. A hidden view still records (empty) frames, which
	 * is what actually clears it from the screen.
	 */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	void SetVisible(bool bVisible);

	/** True while this handle still refers to a live view. */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	bool IsViewValid() const;

	/** Queues a re-layout at this pixel size. Only sent when the size actually changed. */
	void Resize(FIntPoint ViewSize);

	/**
	 * Queues one input event for this view's context. Never blocks, never fails: a
	 * dead view drops it.
	 *
	 * The single choke point for input on the game thread, which is also why the
	 * frame-ordering diagnostic below lives here rather than in the widget.
	 */
	void SendInput(const FVaCuusInputEvent& Event);

	uint32 GetViewId() const { return ViewId; }

	/**
	 * How many input events this handle has queued for the UI thread.
	 *
	 * This is the ONLY observable that can answer "was that event forwarded at all",
	 * and one decision needs it: controller decision D12's pass-through key set is
	 * defined as keys the widget neither consumes nor enqueues, and an event that was
	 * dropped on the game thread leaves no other trace anywhere. Counted here rather
	 * than in the widget because this is the single choke point every event goes
	 * through, widget or not.
	 *
	 * Counts events ACCEPTED for the queue: an event dropped because the view is already
	 * invalid does not increment it.
	 */
	uint64 GetNumInputEventsQueued() const { return NumInputEventsQueued; }

	/** Frames this view has published to the render thread. Useful for headless waits. */
	uint64 GetFramesPublished() const;

	/**
	 * Serial of the newest load this view has ASKED for, and of the newest one the UI
	 * thread has FINISHED. Equal means the view is showing the answer to the last
	 * request; a completed serial above your own request's means yours was
	 * superseded and its result was never reported (see the delegate comment).
	 * Serials start at 1; 0 means "none yet".
	 */
	uint64 GetLastRequestedLoadSerial() const;
	uint64 GetLastCompletedLoadSerial() const;

	/** True while a queued load has not been answered yet. */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	bool IsLoadPending() const;

	/**
	 * Where this view is interactive, as of the UI thread's newest published frame.
	 *
	 * THE CONTRACT SLATE NEEDS: this is a plain value type owned by this handle, so
	 * `Snapshot.Contains(P)` is a synchronous, lock-free, never-failing answer to
	 * "does the UI want this pointer event" -- which is what lets SVaCuusWidget
	 * return Handled/Unhandled without ever asking the UI thread anything.
	 *
	 * LIFETIME: the reference is to this object's own cache and stays valid until the
	 * NEXT PollStatus(), i.e. for the rest of the game frame. That is the guarantee
	 * that matters -- ONE STABLE SNAPSHOT PER DISPATCH BATCH, AT MOST ONE FRAME OLD --
	 * because it is what keeps the answer from changing between a mouse-down and the
	 * mouse-up that releases its capture. Reading the triple buffer per event would
	 * not.
	 *
	 * THE ACTUAL ORDERING, measured rather than assumed (bead VaCuus-akj.6.13): input
	 * is dispatched BEFORE this cache is refreshed, so a handler sees the snapshot
	 * polled in the PREVIOUS game frame. On Linux, SDL events are processed
	 * synchronously inside FPlatformApplicationMisc::PumpMessages
	 * (LaunchEngineLoop.cpp:5784) -- FLinuxApplication::AddPendingEvent only defers
	 * when GPumpingMessagesOutsideOfMainLoop is set, and that flag is set on Windows
	 * only (WindowsPlatformMisc.cpp:4439) -- while PollStatus() runs from
	 * UVaCuusSubsystem::Tick inside GEngine->Tick (LaunchEngineLoop.cpp:5859), 75
	 * lines later. So the ordering is: dispatch input (frame N) -> refresh cache
	 * (frame N) -> dispatch input (frame N+1). One frame of staleness, and the
	 * per-frame stability above is unaffected.
	 *
	 * Before the first published frame this is the default snapshot: Generation 0,
	 * no rects, nothing interactive.
	 */
	const FVaCuusInteractiveSnapshot& GetSnapshot() const { return CachedSnapshot; }

	//~ IME (Task 9). The Slate host's whole interface to the platform text-input system:
	//~ everything RmlUi-shaped and everything ITextInputMethodContext-shaped stays behind
	//~ these three calls. Game thread only.

	/**
	 * Tells the IME where this view is drawn and whether the host has keyboard focus (see
	 * FVaCuusImeSurface). Called once per game frame by the host; cheap when nothing moved,
	 * and a complete no-op on a platform with no IME system.
	 *
	 * The FIRST call is what creates the handler, so a view nobody hosts in Slate never
	 * builds an IME context at all -- which is the right answer for a headless view and for a
	 * world-space one (D17).
	 */
	void UpdateIme(const FVaCuusImeSurface& Surface);

	/**
	 * Controller decision D14a: a press landed on a rect flagged
	 * EVaCuusRectFlags::TextInput, so activate the platform IME context on THIS click
	 * instead of waiting for the snapshot that will confirm it a frame later. Without this
	 * the player's first composition is lost -- the same bug D11 fixed for Slate focus.
	 */
	void NotifyImeTextInputClicked();

	/**
	 * Deactivates, unregisters and forgets the IME context NOW (controller decision D18).
	 *
	 * Called from every teardown site rather than left to destruction: the platform system
	 * holds the context by TSharedRef, so a context that outlives its Slate widget keeps the
	 * OS pointing at a window that is gone.
	 */
	void DetachIme();

	/**
	 * What the IME bridge is doing right now.
	 *
	 * ONE STRUCT RATHER THAN FOUR ACCESSORS, and it is deliberately on the VIEW: the handler and
	 * the context are both declared in a PRIVATE header (VaCuusTextInput.h), so a caller in
	 * another module -- VaCuus.Input.TextEntry lives in VaCuusRender, next to the widget whose
	 * OnKeyChar it drives -- cannot name their types at all. The view is already the facade for
	 * everything else about a view; being the facade for this too is what keeps the IME
	 * implementation private without making it untestable.
	 */
	struct FImeStatus
	{
		/** False until the first UpdateIme(): a view nobody hosts in Slate has no bridge. */
		bool bHandlerBuilt = false;

		/** True where the platform offers no ITextInputMethodSystem -- the Linux path (D16). */
		bool bPlatformImeAbsent = true;

		/** True once the context has been handed to the platform system. Never true on Linux. */
		bool bRegistered = false;

		/** True while the platform reports OUR context as the active one. */
		bool bContextActive = false;
	};

	FImeStatus GetImeStatus() const;

	/**
	 * The platform-facing context, so a test can drive the 14 ITextInputMethodContext virtuals
	 * the way the OS would. Null before the first UpdateIme().
	 *
	 * NON-NULL EVEN WITHOUT A PLATFORM IME, which is the point: answering those virtuals needs
	 * only the shadow state and the surface, so the whole game-thread half of the IME is built
	 * and observable on a platform that will never call it (D16).
	 */
	ITextInputMethodContext* GetImeContextForTesting() const;

	/**
	 * Once per game frame, from UVaCuusSubsystem::Tick: refreshes the snapshot cache
	 * and turns the UI thread's newest load result into an OnLoadCompleted
	 * broadcast. Both halves exist to keep everything the UI thread publishes on the
	 * game thread by the time anyone reads it.
	 */
	void PollStatus();

	//~ Begin UObject
	/** Last-chance IME teardown; the explicit DetachIme() sites are the intended ones (D18). */
	virtual void BeginDestroy() override;
	//~ End UObject

private:
	/** Copies the newest published snapshot into CachedSnapshot, if there is a newer one. */
	void RefreshSnapshot();

	/**
	 * Logs the one line of evidence for the ordering claim on GetSnapshot(): what the
	 * frame's first input event saw versus what this frame's poll produced. Once per
	 * session, Verbose -- it answers a design question, not a per-frame one.
	 */
	void LogFrameOrderingOnce();

	/** The UI thread through the subsystem, or null once invalidated / after module shutdown. */
	FVaCuusUIThread* GetUIThread() const;

	/** The owner; also this object's outer. Weak so module/world teardown order cannot bite. */
	UPROPERTY(Transient)
	TWeakObjectPtr<UVaCuusSubsystem> OwningSubsystem;

	/** Shared with this view's document host on the UI thread. */
	TSharedPtr<FVaCuusViewStatus> Status;

	/**
	 * The platform IME bridge, created by the first UpdateIme() and destroyed by DetachIme().
	 *
	 * TSharedPtr because ITextInputMethodSystem::RegisterContext takes the CONTEXT by
	 * TSharedRef and the handler is what owns that context -- the platform can outlive our
	 * intent to keep it, which is precisely why Shutdown() is called explicitly.
	 */
	TSharedPtr<FVaCuusImeHandler> ImeHandler;

	/**
	 * Game-thread-owned copy of the newest snapshot the UI thread published.
	 *
	 * A COPY, not a reference into the triple buffer, for two reasons: the buffer's
	 * read reference dies at the next swap (TripleBuffer.h's own warning), and a
	 * per-frame snapshot must not change under Slate's feet mid-frame. Refreshed
	 * only when Generation moved, so an idle UI costs one integer comparison per
	 * frame and the TArray keeps its allocation.
	 */
	FVaCuusInteractiveSnapshot CachedSnapshot;

	/** GFrameCounter when CachedSnapshot last took a NEW generation. */
	uint64 SnapshotCachedOnFrame = 0;

	//~ Frame-ordering evidence for bead VaCuus-akj.6.13, logged once per session
	//~ (Verbose) the first time input and a poll happen in the same frame. Recording
	//~ what the FIRST event of a frame saw, because that is the one whose staleness
	//~ the ordering claim is about.

	/** GFrameCounter of the frame whose first input event is recorded below. */
	uint64 InputObservedFrame = 0;

	/** Snapshot generation that first event answered from, and when it was cached. */
	uint64 InputObservedGeneration = 0;
	uint64 InputObservedCachedOnFrame = 0;

	/** Set by SendInput(), consumed by the next PollStatus() in the same frame. */
	bool bInputOrderingLogPending = false;
	bool bLoggedFrameOrdering = false;

	/** Process-unique; allocated by the UI thread and stamped into every command. */
	uint32 ViewId = 0;

	/** Cleared by Invalidate(); gates every enqueue. */
	bool bRegistered = false;

	/** Backs GetNumInputEventsQueued(); game thread only, like everything else here. */
	uint64 NumInputEventsQueued = 0;

	/** Backs GetDocumentPath(); see there for why it is only ever a FILE path. */
	FString DocumentPath;

	/** Next load's serial. Strictly increasing, so a stale completion cannot be mistaken for a new one. */
	uint64 NextLoadSerial = 1;

	/** Newest completion already broadcast. */
	uint64 LastBroadcastLoadSerial = 0;

	/** Last size pushed to the UI thread; resize commands are only sent on change. */
	FIntPoint LastViewSize = FIntPoint::ZeroValue;
};
