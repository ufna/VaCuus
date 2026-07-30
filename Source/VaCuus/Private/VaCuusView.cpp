// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusView.h"

#include "VaCuusDefines.h"
#include "VaCuusInputEvent.h"
#include "VaCuusSubsystem.h"
#include "VaCuusTextInput.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "CoreGlobals.h"

void UVaCuusView::InitializeView(UVaCuusSubsystem* InSubsystem, uint32 InViewId,
	const TSharedRef<FVaCuusViewStatus>& InStatus, FIntPoint InInitialViewSize)
{
	check(IsInGameThread());

	OwningSubsystem = InSubsystem;
	ViewId = InViewId;
	Status = InStatus;
	LastViewSize = InInitialViewSize;
	bRegistered = true;
}

void UVaCuusView::Invalidate()
{
	check(IsInGameThread());

	// Controller decision D18: the platform text-input system holds our context by
	// TSharedRef, so an invalidated view must let go of it HERE rather than whenever the
	// UObject happens to be collected -- otherwise the OS keeps composing into a view that
	// no longer exists.
	DetachIme();

	// The status object stays: the UI thread's host holds its own reference, and
	// dropping ours would only make a late PollStatus() crash instead of no-op.
	bRegistered = false;
}

void UVaCuusView::BeginDestroy()
{
	// The safety net. Every real teardown path (SVaCuusWidget::DetachView, the subsystem's
	// Invalidate) has already detached by now; this is what keeps a view nobody retired from
	// leaving a registered context behind.
	DetachIme();

	Super::BeginDestroy();
}

void UVaCuusView::UpdateIme(const FVaCuusImeSurface& Surface)
{
	check(IsInGameThread());

	if (!ImeHandler.IsValid())
	{
		if (Surface.TextInputMethodSystem == nullptr)
		{
			// No platform IME (Linux, or a world-space host -- D17). Nothing to build, and
			// building it anyway would allocate a handler that can never register anything. The
			// one-shot "this platform has none" log lives in the handler, so create it once
			// even here -- see below.
			//
			// Created regardless, deliberately: the handler is where the degradation is logged,
			// and it is a few dozen bytes. Everything it does afterwards is a no-op.
		}

		ImeHandler = FVaCuusImeHandler::Create(*this);
	}

	ImeHandler->UpdateSurface(Surface);
}

void UVaCuusView::NotifyImeTextInputClicked()
{
	check(IsInGameThread());

	if (ImeHandler.IsValid())
	{
		// The generation the click was answered from: the handler will not let a snapshot this
		// old, or older, cancel the activation it cannot know about yet.
		ImeHandler->NotifyTextInputClicked(CachedSnapshot.Generation);
	}
}

void UVaCuusView::DetachIme()
{
	check(IsInGameThread());

	if (ImeHandler.IsValid())
	{
		// Shutdown() first, then drop: the platform must be told before the last reference
		// goes, or UnregisterContext never happens.
		ImeHandler->Shutdown();
		ImeHandler.Reset();
	}
}

UVaCuusView::FImeStatus UVaCuusView::GetImeStatus() const
{
	FImeStatus ImeStatus;
	if (!ImeHandler.IsValid())
	{
		return ImeStatus;
	}

	ImeStatus.bHandlerBuilt = true;
	ImeStatus.bPlatformImeAbsent = ImeHandler->IsPlatformImeAbsent();
	ImeStatus.bRegistered = ImeHandler->IsRegistered();
	ImeStatus.bContextActive = ImeHandler->IsContextActive();
	return ImeStatus;
}

ITextInputMethodContext* UVaCuusView::GetImeContextForTesting() const
{
	return ImeHandler.IsValid() ? ImeHandler->GetContextForTesting() : nullptr;
}

FVaCuusUIThread* UVaCuusView::GetUIThread() const
{
	// Never cached: the thread belongs to FVaCuusModule, and going through the
	// subsystem every time is what keeps this handle from holding a raw pointer
	// across module or world teardown.
	UVaCuusSubsystem* Subsystem = OwningSubsystem.Get();
	return (bRegistered && Subsystem) ? Subsystem->GetUIThread() : nullptr;
}

void UVaCuusView::LoadDocument(const FString& VfsPath)
{
	check(IsInGameThread());

	FVaCuusUIThread* UIThread = GetUIThread();
	if (!UIThread)
	{
		UE_LOG(LogVaCuus, Warning, TEXT("LoadDocument('%s') on an invalid view is ignored"), *VfsPath);
		return;
	}

	const uint64 Serial = NextLoadSerial++;
	Status->LoadRequestSerial.store(Serial, std::memory_order_relaxed);
	UIThread->EnqueueLoadDocumentFile(ViewId, VfsPath, Serial, LastViewSize);
}

void UVaCuusView::LoadDocumentFromMemory(const FString& RmlSource)
{
	check(IsInGameThread());

	FVaCuusUIThread* UIThread = GetUIThread();
	if (!UIThread)
	{
		UE_LOG(LogVaCuus, Warning, TEXT("LoadDocumentFromMemory() on an invalid view is ignored"));
		return;
	}

	const uint64 Serial = NextLoadSerial++;
	Status->LoadRequestSerial.store(Serial, std::memory_order_relaxed);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, RmlSource, Serial, LastViewSize);
}

void UVaCuusView::Close()
{
	check(IsInGameThread());

	if (FVaCuusUIThread* UIThread = GetUIThread())
	{
		UIThread->EnqueueCloseDocument(ViewId);
	}
}

void UVaCuusView::SetVisible(bool bVisible)
{
	check(IsInGameThread());

	if (FVaCuusUIThread* UIThread = GetUIThread())
	{
		UIThread->EnqueueSetVisible(ViewId, bVisible);
	}
}

bool UVaCuusView::IsViewValid() const
{
	return GetUIThread() != nullptr;
}

void UVaCuusView::Resize(FIntPoint ViewSize)
{
	check(IsInGameThread());

	if (ViewSize == LastViewSize || ViewSize.X <= 0 || ViewSize.Y <= 0)
	{
		return;
	}

	FVaCuusUIThread* UIThread = GetUIThread();
	if (!UIThread)
	{
		return;
	}

	// Remembered even though the command may be dropped mid-teardown: the next
	// load carries this size, so a view that comes back up lays out correctly.
	LastViewSize = ViewSize;
	UIThread->EnqueueResize(ViewId, ViewSize);

	UE_LOG(LogVaCuus, Verbose, TEXT("View %u: queued resize to %dx%d"), ViewId, ViewSize.X, ViewSize.Y);
}

void UVaCuusView::SendInput(const FVaCuusInputEvent& Event)
{
	check(IsInGameThread());

	// Recorded before the enqueue, and only for the first event of each frame: this
	// is the frame-ordering evidence for bead VaCuus-akj.6.13. What matters is which
	// snapshot the handler that produced this event answered Slate from -- see
	// PollStatus() for the log line and GetSnapshot() for the conclusion.
	if (InputObservedFrame != GFrameCounter)
	{
		InputObservedFrame = GFrameCounter;
		InputObservedGeneration = CachedSnapshot.Generation;
		InputObservedCachedOnFrame = SnapshotCachedOnFrame;
		bInputOrderingLogPending = true;
	}

	FVaCuusUIThread* UIThread = GetUIThread();
	if (!UIThread)
	{
		// Ordinary: a widget can outlive its view by a frame during teardown. Verbose
		// rather than Warning because input arrives dozens of times per second and a
		// louder level would bury the log.
		UE_LOG(LogVaCuus, Verbose, TEXT("Input event on an invalid view is ignored"));
		return;
	}

	++NumInputEventsQueued;
	UIThread->EnqueueInput(ViewId, Event);
}

uint64 UVaCuusView::GetFramesPublished() const
{
	return Status.IsValid() ? Status->FramesPublished.load(std::memory_order_acquire) : 0;
}

uint64 UVaCuusView::GetLastRequestedLoadSerial() const
{
	return Status.IsValid() ? Status->LoadRequestSerial.load(std::memory_order_relaxed) : 0;
}

uint64 UVaCuusView::GetLastCompletedLoadSerial() const
{
	return Status.IsValid() ? Status->LoadCompletedSerial.load(std::memory_order_acquire) : 0;
}

bool UVaCuusView::IsLoadPending() const
{
	return GetLastCompletedLoadSerial() < GetLastRequestedLoadSerial();
}

void UVaCuusView::RefreshSnapshot()
{
	check(IsInGameThread());

	// SwapReadBuffers() inside here is a no-op when the UI thread published nothing,
	// in which case the same buffer comes back and Generation has not moved -- so the
	// comparison, not the swap, is what tells a fresh frame from a repeat. The
	// reference dies at the next AcquireSnapshot(), which is why this copies.
	const FVaCuusInteractiveSnapshot& Published = Status->AcquireSnapshot();
	if (Published.Generation == CachedSnapshot.Generation)
	{
		return;
	}

	CachedSnapshot = Published;
	SnapshotCachedOnFrame = GFrameCounter;
}

void UVaCuusView::LogFrameOrderingOnce()
{
	if (!bInputOrderingLogPending)
	{
		return;
	}
	bInputOrderingLogPending = false;

	if (bLoggedFrameOrdering)
	{
		return;
	}
	bLoggedFrameOrdering = true;

	// The measurement behind the ordering claim on GetSnapshot(): both numbers are
	// from the SAME game frame -- what the frame's first input event answered from,
	// and what this frame's poll has just made available. If the first is older, input
	// ran before the poll.
	UE_LOG(LogVaCuus, Verbose,
		TEXT("View %u frame ordering: game frame %llu dispatched its first input event against snapshot generation %llu ")
		TEXT("(cached on game frame %llu); PollStatus on frame %llu now caches generation %llu. ")
		TEXT("Input therefore runs BEFORE the poll -- one frame of snapshot staleness."),
		ViewId, InputObservedFrame, InputObservedGeneration, InputObservedCachedOnFrame,
		GFrameCounter, CachedSnapshot.Generation);
}

void UVaCuusView::PollStatus()
{
	check(IsInGameThread());

	if (!Status.IsValid())
	{
		return;
	}

	// First, and unconditionally: the snapshot is refreshed every frame whether or
	// not a load completed, and the load-result path below returns early most frames.
	RefreshSnapshot();

	// Immediately after, so the log line can print both halves of the comparison.
	LogFrameOrderingOnce();

	// IME reconciliation rides the same poll, immediately after the snapshot it reads: the
	// context's shadow state must be the newest published one before any notification tells
	// the OS to come and read it (see FVaCuusImeHandler::OnSnapshotRefreshed).
	if (ImeHandler.IsValid())
	{
		ImeHandler->OnSnapshotRefreshed(CachedSnapshot);
	}

	// Acquire pairs with the host's release store, so the result read below belongs
	// to this serial and not to the load before it.
	const uint64 Completed = Status->LoadCompletedSerial.load(std::memory_order_acquire);
	if (Completed == LastBroadcastLoadSerial)
	{
		return;
	}

	// Serials are consecutive per view, so the gap is exactly how many earlier
	// results this one hid: the UI thread finished several loads inside one drain (or
	// some were dropped during teardown) and only the last (serial, result) pair
	// survives in the status. Coalescing is the contract, not a bug -- but a silent
	// one would be, so it is named here.
	if (const uint64 NumSuperseded = Completed - LastBroadcastLoadSerial - 1; NumSuperseded > 0)
	{
		UE_LOG(LogVaCuus, Verbose,
			TEXT("View %u: load %llu completed; %llu earlier load result(s) superseded (coalesced or dropped). Last requested: %llu"),
			ViewId, Completed, NumSuperseded, Status->LoadRequestSerial.load(std::memory_order_relaxed));
	}

	// Advanced even with nothing bound, so a listener added later hears about the
	// next load rather than replaying an old one.
	LastBroadcastLoadSerial = Completed;

	const bool bSuccess =
		static_cast<EVaCuusLoadResult>(Status->LoadResult.load(std::memory_order_relaxed)) == EVaCuusLoadResult::Succeeded;
	OnLoadCompleted.Broadcast(this, bSuccess);
}
