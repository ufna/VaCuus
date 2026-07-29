// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "UObject/Object.h"

#include "Templates/SharedPointer.h"

#include "VaCuusView.generated.h"

class FVaCuusUIThread;
class UVaCuusSubsystem;
struct FVaCuusViewStatus;

/** Broadcast on the GAME thread once a queued document load has finished. */
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
	/** Fires once per completed load, in the order the loads were requested. */
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
	 * Turns the UI thread's load results into OnLoadCompleted broadcasts. Called
	 * once per game frame by UVaCuusSubsystem::Tick, which is what keeps every
	 * callback on the game thread.
	 */
	void PollStatus();

private:
	/** The UI thread through the subsystem, or null once invalidated / after module shutdown. */
	FVaCuusUIThread* GetUIThread() const;

	/** The owner; also this object's outer. Weak so module/world teardown order cannot bite. */
	UPROPERTY(Transient)
	TWeakObjectPtr<UVaCuusSubsystem> OwningSubsystem;

	/** Shared with this view's document host on the UI thread. */
	TSharedPtr<FVaCuusViewStatus> Status;

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
