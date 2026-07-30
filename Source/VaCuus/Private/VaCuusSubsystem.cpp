// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusSubsystem.h"

#include "VaCuus.h"
#include "VaCuusDefines.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusStats.h"
#include "VaCuusUIThread.h"
#include "VaCuusView.h"
#include "VaCuusViewStatus.h"

#include "Engine/Engine.h"
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
	// THE SPEC'S GAME-THREAD BUDGET, one half of it (Task 14): everything below is the
	// "snapshot read" the budget names -- PollStatus() swaps this frame's published
	// snapshot into each view's game-thread cache and copies it when the generation moved
	// -- plus the pulse that asks the UI thread for the next frame. The other half is
	// SVaCuusWidget's Tick and its input handlers, sampled under SlateTick and Input.
	//
	// Around the WHOLE body rather than only the loop: Trigger() is game-thread work this
	// design costs, and a scope that excluded it would understate the budget by exactly
	// the amount nobody thought to measure.
	VACUUS_PERF_SCOPE(GameTick);

	// Turns any load result the UI thread published into a game-thread broadcast, and hands
	// the UI thread whatever this frame's UpdateModel() calls marked.
	for (TObjectPtr<UVaCuusView>& View : Views)
	{
		if (UVaCuusView* ViewPtr = View.Get())
		{
			ViewPtr->PollStatus();

			// THE PUBLISH HALF OF THE M3a DATA PIPELINE, HERE AND NOT IN UpdateModel(). Two
			// reasons, and the first is not about measurement: several UpdateModel calls in one
			// frame -- one per actor, one per subsystem -- become ONE triple-buffer swap
			// carrying each field's latest value, rather than one swap per call. The second is
			// spec 6's: this is inside the GameTick scope above, which is where the game-thread
			// budget is measured. It costs nothing when nothing changed (no outstanding field
			// means no swap, no generation bump and therefore no UI-thread work at all), which
			// is what spec 9's "idle -> 0 published frames" row rests on.
			ViewPtr->PublishModelUpdates();
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

	if (UIThread->IsStopping())
	{
		// The queue is closed, so the AddView would be dropped and the handle would
		// never refer to anything. Say no instead of handing back a dead view.
		UE_LOG(LogVaCuus, Warning, TEXT("CreateView() refused: the UI thread is shutting down"));
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

int32 UVaCuusSubsystem::ClearAssetCachesAndReloadAllViews(const TCHAR* Reason)
{
	check(IsInGameThread());

	// FIRST, AND WITHOUT REGARD TO WHETHER ANY VIEW IS FOUND BELOW. RmlUi's parsed
	// stylesheet and template caches are process-global statics keyed on file name that
	// OUTLIVE a PIE session -- Deinitialize() deliberately leaves the UI thread running,
	// only FVaCuusModule::ShutdownModule stops it -- so an .rcss edited while nothing is
	// live must still drop them. Otherwise the next Play re-reads the RML from disk and
	// takes the previous session's stylesheet, silently, and RML edits appear to
	// live-reload while RCSS edits do not.
	//
	// One clear serves every load queued behind it: single-producer FIFO, so this drains
	// ahead of the loads the fan-out below enqueues.
	//
	// GetPtr(), not Get(): a reload can be asked for on a teardown path, where reloading
	// the module would be worse than answering "no thread, nothing cached to drop".
	const FVaCuusModule* Module = FVaCuusModule::GetPtr();
	FVaCuusUIThread* UIThread = Module ? Module->GetUIThread() : nullptr;
	if (UIThread != nullptr)
	{
		UIThread->EnqueueClearAssetCaches();
	}

	if (GEngine == nullptr)
	{
		return 0;
	}

	int32 NumReloaded = 0;
	int32 NumSubsystems = 0;

	// GetWorldContexts(), not GEditor->PlayWorld or GetPIEWorldContext(): both of those see
	// only PIE instance 0 (EditorEngine.cpp:6401-6412, and the doc comment saying so is at
	// EditorEngine.h:2599-2603), so a multi-client PIE session would get one window reloaded
	// and the others left stale. Those two are also unreachable from this Runtime module,
	// which is the point of it living here -- but a future editor-only shortcut is exactly
	// the edit this note is for. Re-resolved on every call rather than cached, because a
	// game instance and its subsystems are destroyed on EndPIE and a kept pointer would
	// dangle into the next session.
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		// NO WorldType FILTER, deliberately, and it is not an oversight left over from the
		// editor-only version this replaces. That version accepted PIE and Game and
		// justified the narrowing by saying a `-game` process could never reach the code at
		// all (VaCuusEditor is EHostType::Editor, so it is not loaded there) -- which stops
		// being true the moment the dispatch lives in a Runtime module, as it now does. The
		// subsystem lookup below IS the test: a context that owns a game instance carrying a
		// UVaCuusSubsystem owns views a reload must reach, whatever the world is called, and
		// any coarser proxy can only LOSE views -- the same silent "nothing happened" this
		// whole entry point exists to prevent.
		//
		// NO Context.World() != nullptr CHECK either, which the research note calls universal
		// engine precedent -- deliberately, so nobody "restores" it and quietly narrows this:
		// nothing here dereferences the world, and UGameInstance::GetSubsystem tolerates a
		// null game instance. A context that has a game instance but no world yet (early PIE)
		// still has views worth reloading.
		UVaCuusSubsystem* Subsystem = UGameInstance::GetSubsystem<UVaCuusSubsystem>(Context.OwningGameInstance);
		if (Subsystem == nullptr)
		{
			// Legitimate: a context can exist before or after its world during PIE
			// start/teardown, and the subsystem may simply not have been created.
			continue;
		}

		++NumSubsystems;
		NumReloaded += Subsystem->ReloadAllDocuments();
	}

	UE_LOG(LogVaCuus, Verbose, TEXT("Reload (%s): %d view(s) across %d game instance(s)%s"),
		Reason, NumReloaded, NumSubsystems,
		UIThread != nullptr ? TEXT("; RmlUi asset caches dropped") : TEXT("; no UI thread, nothing cached to drop"));
	return NumReloaded;
}

int32 UVaCuusSubsystem::ReloadAllDocuments()
{
	check(IsInGameThread());

	int32 NumReloaded = 0;
	for (const TObjectPtr<UVaCuusView>& View : Views)
	{
		if (UVaCuusView* ViewPtr = View.Get())
		{
			NumReloaded += ViewPtr->ReloadDocument() ? 1 : 0;
		}
	}

	// Then the views this loop CANNOT reach: one showing an inline fallback has no
	// DocumentPath to re-issue, and its owner is the only thing that knows which file it
	// fell back from. See OnDocumentsReloadRequested.
	OnDocumentsReloadRequested.Broadcast(NumReloaded);

	return NumReloaded;
}

FVaCuusUIThread* UVaCuusSubsystem::GetUIThread() const
{
	// GetPtr(), not Get(): this also runs on teardown paths, where reloading the
	// module would be worse than answering "no thread".
	const FVaCuusModule* Module = FVaCuusModule::GetPtr();
	return Module ? Module->GetUIThread() : nullptr;
}
