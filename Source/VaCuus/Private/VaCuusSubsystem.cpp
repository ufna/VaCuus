// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusSubsystem.h"

#include "VaCuus.h"
#include "VaCuusBoundModel.h"
#include "VaCuusBundle.h"
#include "VaCuusBundleMount.h"
#include "VaCuusDefines.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusFontRegistry.h"
#include "VaCuusStats.h"
#include "VaCuusStyleSet.h"
#include "VaCuusTranslation.h"
#include "VaCuusUIThread.h"
#include "VaCuusView.h"
#include "VaCuusViewStatus.h"
#include "VaCuusWriteRouter.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/StringTableRegistry.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProperties.h"
#include "HAL/PlatformTime.h"

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

	// THE MOUNT PREDICATE (spec M6 2(d)): cooked builds mount the config-listed
	// bundle; the editor and uncooked -game mount nothing (loose-first, live reload
	// intact -- `vacuus.Bundle.Enable 1` is the PIE pack-on-demand door). Compile-time
	// per target: the editor binary answers false here even during PIE, the staged
	// monolithic game answers true. Idempotent across game instances -- the table is
	// process-wide and MountBundle no-ops on an already-mounted asset. The explicit
	// `vacuus.Bundle.Enable 0` (-dpcvars) suppresses this for loose A/B runs.
	if (FPlatformProperties::RequiresCookedData())
	{
		static const auto* BundleEnableCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("vacuus.Bundle.Enable"));
		if (BundleEnableCVar == nullptr || BundleEnableCVar->GetInt() != 0)
		{
			const FString ConfiguredPath = VaCuusBundleConfig::GetConfiguredBundleAssetPath();
			if (ConfiguredPath.IsEmpty())
			{
				UE_LOG(LogVaCuus, Log,
					TEXT("No [VaCuus] BundleAssetPath configured; UI files serve from the loose staged roots ")
					TEXT("(none are staged in Shipping -- configure a bundle for Shipping builds)"));
			}
			else if (UVaCuusBundle* Bundle = LoadObject<UVaCuusBundle>(nullptr, *ConfiguredPath))
			{
				FVaCuusBundleMountTable::MountBundle(Bundle);
			}
			else
			{
				// The cook-inclusion rule made loud (spec M6 2(d)): a config-soft-path-only
				// bundle is INVISIBLE to the cooker -- nothing references it, so nothing
				// cooked it, and this LoadObject is the first place that can notice.
				UE_LOG(LogVaCuus, Error,
					TEXT("[VaCuus] BundleAssetPath '%s' resolves to NO asset in this cooked build. The cooker only cooks ")
					TEXT("referenced or listed packages: add the bundle's directory to DirectoriesToAlwaysCook (or ")
					TEXT("hard-reference the asset) in the project settings"),
					*ConfiguredPath);
			}
		}
		else
		{
			UE_LOG(LogVaCuus, Log, TEXT("vacuus.Bundle.Enable 0: the cooked auto-mount is suppressed; loose roots serve"));
		}
	}

	// The BP-facing forward of the process-wide signal. Subscribed per game instance so
	// multi-PIE gets one broadcast per subsystem, which is what a Blueprint bound in one
	// client expects; the registry itself fires once.
	TranslationChangedHandle = FVaCuusTranslationRegistry::OnTableChanged().AddWeakLambda(this,
		[this](const FString& Tag, uint64 Version)
		{
			OnTranslationTableChanged.Broadcast(Tag, static_cast<int64>(Version));
		});

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

	// The registry delegate is process-wide and outlives this subsystem; AddWeakLambda
	// already makes a leaked binding inert, but removing it keeps the invocation list from
	// growing by one dead entry per PIE session.
	FVaCuusTranslationRegistry::OnTableChanged().Remove(TranslationChangedHandle);
	TranslationChangedHandle.Reset();

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

	// The write router's game-side drain (M4 Task 9): routed writes and vacuus.emit
	// events become OnModelWrite / OnJsEvent broadcasts here, inside the GameTick scope
	// where the game-thread budget is measured. The queue is process-wide (items route
	// by ViewId, across subsystems), so in multi-PIE whichever subsystem ticks first
	// drains everything and the rest find it empty -- one pass per frame either way.
	FVaCuusWriteRouter::DrainGameThread();

	// Unregistered style-set roots whose render fence completed drop here (M5 Task 5b).
	// Process-wide like the write router's drain: whichever subsystem ticks first pays
	// the (empty-check) branch for everyone.
	FVaCuusStyleRegistry::TickDeferredReleases_GameThread();

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

int32 UVaCuusSubsystem::RegisterStyleSet(UVaCuusStyleSet* StyleSet)
{
	check(IsInGameThread());
	return FVaCuusStyleRegistry::RegisterStyleSet(StyleSet);
}

void UVaCuusSubsystem::UnregisterStyleSet(UVaCuusStyleSet* StyleSet)
{
	check(IsInGameThread());
	FVaCuusStyleRegistry::UnregisterStyleSet(StyleSet);
}

void UVaCuusSubsystem::SetTranslationTable(const TMap<FString, FString>& Table, const FString& Tag)
{
	check(IsInGameThread());
	FVaCuusTranslationRegistry::SetTable(Table, Tag);
}

int32 UVaCuusSubsystem::SetTranslationTableFromStringTable(UStringTable* StringTable, const FString& Tag)
{
	check(IsInGameThread());

	if (StringTable == nullptr)
	{
		UE_LOG(LogVaCuus, Warning, TEXT("SetTranslationTableFromStringTable(null): nothing published, the table is unchanged"));
		return 0;
	}

	const FName TableId = StringTable->GetStringTableId();
	FStringTableConstRef Source = StringTable->GetStringTable();

	// THE LOOKUP HAS TO EXIST BEFORE THE VALUES ARE WORTH READING. FText::FromStringTable
	// resolves through the process-wide registry, not through the object in hand, and a table
	// that is not registered there yields the literal placeholder "<MISSING STRING TABLE ENTRY>"
	// for EVERY key -- a full, well-shaped, completely wrong table with no error anywhere.
	// UStringTable only auto-registers when IsAsset() (StringTable.cpp:401), so an object built
	// in the transient package, or one manually unregistered, lands exactly here.
	if (!FStringTableRegistry::Get().FindStringTable(TableId).IsValid())
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("String table '%s' is not in the string-table registry, so every entry would resolve to the missing-entry ")
			TEXT("placeholder; nothing was published. Pass a saved String Table ASSET (a transient UStringTable does not ")
			TEXT("auto-register)"),
			*TableId.ToString());
		return 0;
	}

	TMap<FString, FString> Table;
	Source->EnumerateKeysAndSourceStrings(
		[&Table, TableId](const FTextKey& Key, const FString& /*SourceString*/) -> bool
		{
			// THE SOURCE STRING IS DELIBERATELY IGNORED. It is the authoring-language text;
			// what a running game needs is the entry resolved against the CURRENT culture,
			// which is exactly what FText::FromStringTable gives (Text.h:510) and what makes
			// the project's .locres the thing that decides the answer.
			Table.Add(Key.ToString(), FText::FromStringTable(TableId, Key).ToString());
			return true;
		});

	if (Table.IsEmpty())
	{
		UE_LOG(LogVaCuus, Warning,
			TEXT("String table '%s' enumerated 0 entries; publishing it would REPLACE the live table with an empty one, ")
			TEXT("so nothing was published"),
			*TableId.ToString());
		return 0;
	}

	FVaCuusTranslationRegistry::SetTable(Table, Tag);
	return Table.Num();
}

bool UVaCuusSubsystem::LoadFontFace(const FString& VfsPath, bool bFallbackFace)
{
	check(IsInGameThread());
	return FVaCuusFontRegistry::RegisterFace(VfsPath, bFallbackFace);
}

bool UVaCuusSubsystem::MountBundle(UVaCuusBundle* Bundle)
{
	check(IsInGameThread());
	return FVaCuusBundleMountTable::MountBundle(Bundle);
}

bool UVaCuusSubsystem::UnmountBundle(UVaCuusBundle* Bundle)
{
	check(IsInGameThread());
	if (Bundle == nullptr)
	{
		return false;
	}
	return FVaCuusBundleMountTable::UnmountBundle(Bundle->GetPathName());
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

int32 UVaCuusSubsystem::DumpModels(uint32 ViewId, FName ModelName)
{
	check(IsInGameThread());

	if (GEngine == nullptr)
	{
		UE_LOG(LogVaCuus, Display, TEXT("DumpModel: no engine, so there are no views"));
		return 0;
	}

	int32 NumViews = 0;
	int32 NumDumped = 0;

	// GetWorldContexts(), for the reason ClearAssetCachesAndReloadAllViews() gives at length:
	// the editor's PIE accessors see instance 0 only, and this is a Runtime module anyway.
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UVaCuusSubsystem* Subsystem = UGameInstance::GetSubsystem<UVaCuusSubsystem>(Context.OwningGameInstance);
		if (Subsystem == nullptr)
		{
			continue;
		}

		for (const TObjectPtr<UVaCuusView>& View : Subsystem->Views)
		{
			UVaCuusView* ViewPtr = View.Get();
			if (ViewPtr == nullptr || (ViewId != 0 && ViewPtr->GetViewId() != ViewId))
			{
				continue;
			}

			++NumViews;
			NumDumped += ViewPtr->DumpModel(ModelName);
		}
	}

	if (NumViews == 0)
	{
		UE_LOG(LogVaCuus, Display,
			TEXT("DumpModel: no live view matches %s. Run it with no arguments to dump every model of every view"),
			ViewId != 0 ? *FString::Printf(TEXT("view %u"), ViewId) : TEXT("the search (there is no live view at all)"));
	}

	return NumDumped;
}

FVaCuusUIThread* UVaCuusSubsystem::GetUIThread() const
{
	// GetPtr(), not Get(): this also runs on teardown paths, where reloading the
	// module would be worse than answering "no thread".
	const FVaCuusModule* Module = FVaCuusModule::GetPtr();
	return Module ? Module->GetUIThread() : nullptr;
}

int32 UVaCuusSubsystem::NotifyStructPreRecompile(const UScriptStruct* ChangedStruct)
{
	check(IsInGameThread());

	if (ChangedStruct == nullptr)
	{
		return 0;
	}

	const FVaCuusModule* Module = FVaCuusModule::GetPtr();
	FVaCuusUIThread* UIThread = Module != nullptr ? Module->GetUIThread() : nullptr;
	const bool bCanEnqueue = UIThread != nullptr && !UIThread->IsStopping();

	// The stale mark goes out FIRST and UNCONDITIONALLY (matched models or none): a type can
	// sit in the definition registry purely as another model's array-element type, and the
	// FIFO from this single producer is what puts the mark ahead of any recovery re-bind.
	if (bCanEnqueue)
	{
		UIThread->EnqueueMarkDefinitionsStale(ChangedStruct, ChangedStruct->GetName());
	}

	// The DumpModels walk, verbatim and for its reasons: GetWorldContexts() because the
	// editor's PIE accessors see instance 0 only, no WorldType filter because the subsystem
	// lookup is the test, re-resolved per call because instances die on EndPIE.
	TArray<TPair<uint32, TSharedRef<FVaCuusBoundModel>>> Condemned;
	if (GEngine != nullptr)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UVaCuusSubsystem* Subsystem = UGameInstance::GetSubsystem<UVaCuusSubsystem>(Context.OwningGameInstance);
			if (Subsystem == nullptr)
			{
				continue;
			}

			for (const TObjectPtr<UVaCuusView>& View : Subsystem->Views)
			{
				if (UVaCuusView* ViewPtr = View.Get())
				{
					ViewPtr->RefuseModelsForStructRecompile(ChangedStruct, Condemned);
				}
			}
		}
	}

	if (Condemned.IsEmpty())
	{
		return 0;
	}

	// THE FENCED TEARDOWN (spec M6 2(j)). Every condemned model's UI-side drop is queued,
	// then this thread -- parked inside PreChange, old chain alive -- waits for the drain, so
	// the drops run the NORMAL DestroyStruct path. Abandon() is only the timeout fallback.
	// With no thread to enqueue on (module down, or a queue closed for shutdown) the drops
	// cannot be delivered at all and the resolution below abandons every model immediately --
	// still loud, still measured.
	if (bCanEnqueue)
	{
		for (const TPair<uint32, TSharedRef<FVaCuusBoundModel>>& Pair : Condemned)
		{
			UIThread->EnqueueDropModelForRecompile(Pair.Key, Pair.Value);
		}
	}

	const double FenceStart = FPlatformTime::Seconds();
	const double Deadline = FenceStart + 0.1;

	auto AllTornDown = [&Condemned]()
	{
		for (const TPair<uint32, TSharedRef<FVaCuusBoundModel>>& Pair : Condemned)
		{
			if (Pair.Value->GetDropState() != EVaCuusModelDropState::TornDown)
			{
				return false;
			}
		}
		return true;
	};

	if (bCanEnqueue && UIThread->IsInlineMode())
	{
		// No worker: the frame that drains the drops is ours to run, and "fence" degenerates
		// to a synchronous call. RunFrameInline() is game-thread-only, which we are.
		UIThread->RunFrameInline();
	}
	else if (bCanEnqueue)
	{
		// Trigger-and-wait per round rather than one wait: a frame already in flight past
		// DrainCommands when the drops were queued completes WITHOUT them, so the first
		// WaitForFrameCount can succeed with nothing drained -- the loop then asks for one
		// more frame, and the drop state (not the frame count) is what ends it.
		while (!AllTornDown() && FPlatformTime::Seconds() < Deadline)
		{
			const uint64 Target = UIThread->GetFrameCount() + 1;
			UIThread->Trigger();
			UIThread->WaitForFrameCount(Target, Deadline - FPlatformTime::Seconds());
		}
	}

	// Resolution. For every model the drain did not reach, ResolveDropTimeout flips it to
	// abandon-on-arrival (or waits out a teardown caught mid-flight -- see its contract);
	// true means the UI-side buffers are now unreclaimable through the type and their
	// contents leak, which is logged PER MODEL with the estimate, as the spec's 2(j) asks.
	int32 NumAbandoned = 0;
	for (const TPair<uint32, TSharedRef<FVaCuusBoundModel>>& Pair : Condemned)
	{
		const TSharedRef<FVaCuusBoundModel>& Model = Pair.Value;
		if (Model->GetDropState() != EVaCuusModelDropState::TornDown && Model->ResolveDropTimeout())
		{
			++NumAbandoned;
			UE_LOG(LogVaCuus, Error,
				TEXT("VaCuus model '%s': the UI thread did not drain the recompile drop within %.0f ms; its UI-side buffers ")
				TEXT("will be freed WITHOUT destructors and their contents leak (>= %llu bytes, struct '%s')"),
				*Model->GetModelNameString(), (Deadline - FenceStart) * 1000.0, Model->EstimateAbandonedBytes(),
				*ChangedStruct->GetName());
		}
	}

	UE_LOG(LogVaCuus, Log,
		TEXT("VaCuus struct recompile ('%s'): %d model(s) refused; UI-side teardown %s in %.1f ms"),
		*ChangedStruct->GetName(), Condemned.Num(),
		NumAbandoned == 0 ? TEXT("completed inside the fence window") : TEXT("TIMED OUT for some models (see the Errors above)"),
		(FPlatformTime::Seconds() - FenceStart) * 1000.0);

	return Condemned.Num();
}

namespace VaCuusModelDiagnostics
{
/**
 * `vacuus.DumpModel [view] [model]` -- spec 8's tool, and it is registered HERE rather than
 * beside the demo toggles in VaCuusRender for one reason: everything it prints is private to
 * this module. FVaCuusBoundModel, both shadows and the channel are in VaCuus/Private, and
 * VaCuusRender depends on VaCuus rather than the other way round, so a command over there could
 * only reach them through a public API invented for it. `vacuus.ReloadUI` lives here too now
 * (below; bead VaCuus-akj.6.18) -- product-facing commands belong to the runtime module, so
 * they exist in `-game` and packaged builds.
 *
 * BOTH ARGUMENTS ARE OPTIONAL AND DEFAULT TO "EVERYTHING". A milestone whose failure mode is no
 * output at all must not require the reader to already know a view id.
 */
static void DumpModelNow(uint32 ViewId, FName ModelName)
{
	const int32 NumDumped = UVaCuusSubsystem::DumpModels(ViewId, ModelName);

	UE_LOG(LogVaCuus, Display,
		TEXT("DumpModel: %d model(s) dumped from the game thread; each one's UI-THREAD HALF FOLLOWS ON THE NEXT UI FRAME ")
		TEXT("(it is printed by the thread that owns the UI shadow, so it cannot be printed here)"),
		NumDumped);
}

static void DumpModel(const TArray<FString>& Args)
{
	// `-` RATHER THAN AN OMITTED ARGUMENT, because the delay has to be reachable without naming
	// a view or a model: the delay is exactly what an -ExecCmds run needs and exactly the run
	// that has no view id to name yet. Both wildcards spelled the same way so there is one rule.
	auto IsWildcard = [](const FString& Arg) { return Arg.IsEmpty() || Arg == TEXT("-"); };

	const uint32 ViewId = (Args.Num() > 0 && !IsWildcard(Args[0])) ? static_cast<uint32>(FCString::Atoi(*Args[0])) : 0;
	const FName ModelName = (Args.Num() > 1 && !IsWildcard(Args[1])) ? FName(*Args[1]) : NAME_None;
	const float DelaySeconds = Args.Num() > 2 ? FCString::Atof(*Args[2]) : 0.0f;

	if (DelaySeconds <= 0.0f)
	{
		DumpModelNow(ViewId, ModelName);
		return;
	}

	// WHY THE DELAY EXISTS AT ALL, and it is the same argument vacuus.M2Demo.Rects carries:
	// every `-ExecCmds` command runs on ONE early tick, before the UI thread has drained a
	// single command. A dump issued there prints a model whose game half exists and whose UI
	// half has not been created yet -- which looks exactly like the bind having failed, i.e. the
	// one diagnosis this command exists to make unambiguous.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[ViewId, ModelName](float)
		{
			DumpModelNow(ViewId, ModelName);
			return false;
		}),
		DelaySeconds);

	UE_LOG(LogVaCuus, Display, TEXT("DumpModel: scheduled at t+%.1fs"), DelaySeconds);
}

static FAutoConsoleCommand GDumpModelCommand(
	TEXT("vacuus.DumpModel"),
	TEXT("Print a bound data model: the layout, both shadows, the pending/unacknowledged/published dirty sets and the ")
	TEXT("last applied generation. Optional [viewId] [modelName] [delaySeconds]; `-` or 0 means 'any', and the delay ")
	TEXT("is for -ExecCmds use, where everything runs before the first UI frame. The UI-thread half arrives one UI ")
	TEXT("frame after the game-thread half."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&DumpModel));

/**
 * `vacuus.ReloadUI`, MOVED here from VaCuusEditor (bead VaCuus-akj.6.18): the body was always
 * a call to the runtime static above it, but the FAutoConsoleCommand lived next to the editor
 * file watcher, so the one MANUAL reload door did not exist in `-game` or in a packaged
 * Development build -- exactly the venues that have no watcher and need the door most. Moved,
 * not copied: a second registration under the same name is the trap, and NOT the way an
 * earlier version of this comment told it. For a console COMMAND the manager does not keep
 * the first -- AddConsoleObject warns (ConsoleManager.cpp:3308) and then the ExistingCmd
 * branch REPLACES it (:3389-3396, "Replace console command with the new one and release the
 * existing one"): the name maps to the SECOND registration and the first one's
 * IConsoleCommand is Release()d out from under its still-live FAutoConsoleCommand, which
 * keeps a raw Target pointer to it (IConsoleManager.h:1532) for the UnregisterConsoleObject
 * its destructor will run. A twin in the editor module would mean module load order decides
 * which body serves the name, with the loser holding a freed pointer until its own teardown
 * -- so the editor module now registers nothing.
 *
 * Manual-only by design out here: the WATCHER stays editor-only (nothing pumps
 * DirectoryWatcher outside UEditorEngine::Tick, and it is lossless nowhere -- the Linux
 * backend drops IN_Q_OVERFLOW outright, DirectoryWatchRequestLinux.cpp:496, which is why a
 * manual escape hatch has to exist at all). This command is that escape hatch, unconditional
 * and watcher-free: both halves of a whole reload (cache drop + fan-out) through the one
 * paired door.
 */
static void ReloadUI()
{
	const int32 NumReloaded = UVaCuusSubsystem::ClearAssetCachesAndReloadAllViews(TEXT("vacuus.ReloadUI"));
	UE_LOG(LogVaCuus, Log, TEXT("vacuus.ReloadUI reloaded %d view(s)"), NumReloaded);
}

static FAutoConsoleCommand GReloadUICommand(
	TEXT("vacuus.ReloadUI"),
	TEXT("Re-load the current document of every live VaCuus view, dropping RmlUi's stylesheet/template caches first. ")
	TEXT("The manual counterpart to the editor's file watcher (which is not lossless), and the only reload door in ")
	TEXT("-game and packaged builds."),
	FConsoleCommandDelegate::CreateStatic(&ReloadUI));
}	 // namespace VaCuusModelDiagnostics
