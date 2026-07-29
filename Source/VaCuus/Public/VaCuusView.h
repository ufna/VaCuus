// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "UObject/Object.h"

#include "VaCuusInteractiveSnapshot.h"

#include "Templates/SharedPointer.h"

#include "VaCuusView.generated.h"

class FVaCuusUIThread;
class UVaCuusSubsystem;
struct FVaCuusViewStatus;

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
	 * Loads a document through the RmlUi file interface (paths resolve against
	 * <Project>/Content/DevUI). Asynchronous: OnLoadCompleted reports the outcome.
	 */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	void LoadDocument(const FString& VfsPath);

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

	uint32 GetViewId() const { return ViewId; }

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
	 * LIFETIME: the reference is to this object's own cache and stays valid until
	 * the NEXT PollStatus(), i.e. for the rest of the game frame. That is
	 * deliberate: PollStatus() runs from UVaCuusSubsystem::Tick, inside the world
	 * tick, and Slate ticks and dispatches input later in the same frame
	 * (FSlateApplication::Tick runs after GEngine->Tick), so every input handler in
	 * one frame tests against ONE stable geometry. The alternative -- reading the
	 * triple buffer per event -- would let the answer change between a mouse-down
	 * and the mouse-up that releases its capture.
	 *
	 * Before the first published frame this is the default snapshot: Generation 0,
	 * no rects, nothing interactive.
	 */
	const FVaCuusInteractiveSnapshot& GetSnapshot() const { return CachedSnapshot; }

	/**
	 * Once per game frame, from UVaCuusSubsystem::Tick: refreshes the snapshot cache
	 * and turns the UI thread's newest load result into an OnLoadCompleted
	 * broadcast. Both halves exist to keep everything the UI thread publishes on the
	 * game thread by the time anyone reads it.
	 */
	void PollStatus();

private:
	/** Copies the newest published snapshot into CachedSnapshot, if there is a newer one. */
	void RefreshSnapshot();

	/** The UI thread through the subsystem, or null once invalidated / after module shutdown. */
	FVaCuusUIThread* GetUIThread() const;

	/** The owner; also this object's outer. Weak so module/world teardown order cannot bite. */
	UPROPERTY(Transient)
	TWeakObjectPtr<UVaCuusSubsystem> OwningSubsystem;

	/** Shared with this view's document host on the UI thread. */
	TSharedPtr<FVaCuusViewStatus> Status;

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

	/** Process-unique; allocated by the UI thread and stamped into every command. */
	uint32 ViewId = 0;

	/** Cleared by Invalidate(); gates every enqueue. */
	bool bRegistered = false;

	/** Next load's serial. Strictly increasing, so a stale completion cannot be mistaken for a new one. */
	uint64 NextLoadSerial = 1;

	/** Newest completion already broadcast. */
	uint64 LastBroadcastLoadSerial = 0;

	/** Last size pushed to the UI thread; resize commands are only sent on change. */
	FIntPoint LastViewSize = FIntPoint::ZeroValue;
};
