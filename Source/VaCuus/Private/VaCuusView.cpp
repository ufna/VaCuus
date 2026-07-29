// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusView.h"

#include "VaCuusDefines.h"
#include "VaCuusSubsystem.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

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

	// The status object stays: the UI thread's host holds its own reference, and
	// dropping ours would only make a late PollStatus() crash instead of no-op.
	bRegistered = false;
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
