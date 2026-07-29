// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusSubsystem.h"

#include "VaCuus.h"
#include "VaCuusDefines.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusUIThread.h"
#include "VaCuusView.h"
#include "VaCuusViewStatus.h"

#include "Engine/GameInstance.h"

UVaCuusSubsystem::UVaCuusSubsystem()
	: FTickableGameObject(ETickableTickType::Never)
{
	// Never at construction on purpose: UObjects may be constructed on worker
	// threads, and FTickableGameObject's registration ensures IsInGameThread().
	// Initialize() turns ticking on.
}

void UVaCuusSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	bInitialized = true;
	SetTickableTickType(GetTickableTickType());
}

void UVaCuusSubsystem::Deinitialize()
{
	// This instance's views go away; the UI thread does not. Another PIE client's
	// subsystem may still be driving it, and the module owns its lifetime anyway.
	for (TObjectPtr<UVaCuusView>& View : Views)
	{
		if (UVaCuusView* ViewPtr = View.Get())
		{
			if (FVaCuusUIThread* UIThread = GetUIThread())
			{
				UIThread->EnqueueRemoveView(ViewPtr->GetViewId());
			}
			ViewPtr->Invalidate();
		}
	}
	Views.Empty();

	SetTickableTickType(ETickableTickType::Never);
	bInitialized = false;

	Super::Deinitialize();
}

void UVaCuusSubsystem::Tick(float DeltaTime)
{
	// Turns any load result the UI thread published into a game-thread broadcast.
	for (TObjectPtr<UVaCuusView>& View : Views)
	{
		if (UVaCuusView* ViewPtr = View.Get())
		{
			ViewPtr->PollStatus();
		}
	}

	FVaCuusUIThread* UIThread = GetUIThread();
	if (!UIThread)
	{
		return;
	}

	if (UIThread->IsInlineMode())
	{
		// No worker thread on this platform/configuration: the frame runs here, at
		// the same point in the game frame the trigger would have been sent.
		UIThread->RunFrameInline();
	}
	else
	{
		// Coalescing pulse; never blocks. Several subsystems triggering the shared
		// thread in the same frame still produce exactly one UI frame.
		UIThread->Trigger();
	}
}

TStatId UVaCuusSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UVaCuusSubsystem, STATGROUP_Tickables);
}

ETickableTickType UVaCuusSubsystem::GetTickableTickType() const
{
	return (IsTemplate() || !bInitialized) ? ETickableTickType::Never : ETickableTickType::Always;
}

bool UVaCuusSubsystem::IsTickable() const
{
	return bInitialized;
}

UWorld* UVaCuusSubsystem::GetTickableGameObjectWorld() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetWorld() : nullptr;
}

UVaCuusView* UVaCuusSubsystem::CreateView(TUniquePtr<IVaCuusDocumentHost> Host, FIntPoint InitialViewSize)
{
	check(IsInGameThread());

	if (!Host.IsValid())
	{
		UE_LOG(LogVaCuus, Error, TEXT("CreateView() needs a document host"));
		return nullptr;
	}

	// Starts the process-wide thread on the very first view, in this or any other
	// game instance.
	FVaCuusUIThread* UIThread = FVaCuusModule::Get().GetOrStartUIThread();
	if (!UIThread)
	{
		// Already logged in detail by the module.
		return nullptr;
	}

	const uint32 ViewId = UIThread->AllocateViewId();
	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();

	// Queued, not applied: the host is booted on the UI thread when this is drained,
	// so nothing RmlUi-affine happens on this thread.
	UIThread->EnqueueAddView(ViewId, MoveTemp(Host), InitialViewSize, Status);

	UVaCuusView* View = NewObject<UVaCuusView>(this);
	View->InitializeView(this, ViewId, Status, InitialViewSize);
	Views.Add(View);

	UE_LOG(LogVaCuus, Log, TEXT("Created view %u (%dx%d) for game instance '%s'"),
		ViewId, InitialViewSize.X, InitialViewSize.Y,
		GetGameInstance() ? *GetGameInstance()->GetName() : TEXT("none"));
	return View;
}

void UVaCuusSubsystem::DestroyView(UVaCuusView* View)
{
	check(IsInGameThread());

	if (View == nullptr)
	{
		return;
	}

	// The UI thread closes the document, drops the context and releases the view's
	// render resources when it drains this; the other views are untouched.
	if (FVaCuusUIThread* UIThread = GetUIThread())
	{
		UIThread->EnqueueRemoveView(View->GetViewId());
	}

	View->Invalidate();
	Views.Remove(View);
}

FVaCuusUIThread* UVaCuusSubsystem::GetUIThread() const
{
	// GetPtr(), not Get(): this also runs on teardown paths, where reloading the
	// module would be worse than answering "no thread".
	const FVaCuusModule* Module = FVaCuusModule::GetPtr();
	return Module ? Module->GetUIThread() : nullptr;
}
