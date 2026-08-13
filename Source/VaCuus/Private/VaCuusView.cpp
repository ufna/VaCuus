// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusView.h"

#include "VaCuusBoundModel.h"
#include "VaCuusDefines.h"
#include "VaCuusInputEvent.h"
#include "VaCuusStats.h"
#include "VaCuusSubsystem.h"
#include "VaCuusTextInput.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"
#include "VaCuusWriteRouter.h"

#include "CoreGlobals.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

void UVaCuusView::InitializeView(UVaCuusSubsystem* InSubsystem, uint32 InViewId,
	const TSharedRef<FVaCuusViewStatus>& InStatus, FIntPoint InInitialViewSize)
{
	check(IsInGameThread());

	OwningSubsystem = InSubsystem;
	ViewId = InViewId;
	Status = InStatus;
	LastViewSize = InInitialViewSize;
	bRegistered = true;

	// The write router's game-side dispatch map (M4 Task 9): what lets the queue drain
	// find this handle by the ViewId every routed item carries. Weak on that side, so
	// this registration keeps nothing alive.
	FVaCuusWriteRouter::RegisterGameView(ViewId, this);
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

	// RETIREMENT RELEASES THE GAME SIDE OF EVERY BOUND MODEL (M6 review). From here this
	// view is unreachable to NotifyStructPreRecompile's walk (it iterates the subsystem's
	// Views array), so an entry kept in this map would be a model no recompile can ever
	// condemn -- destroyed at GC time through an FProperty chain a later recompile may
	// have freed, the exact dangle the refusal exists to prevent. Dropping the references
	// is safe on the UI side's ownership: the UI thread holds its own TSharedRef per model
	// (FVaCuusUIThread::Models after the BindModel drain, the queued command before it),
	// released in RemoveView() only AFTER the host's Shutdown() destroyed the context that
	// pointed into the shadows -- so the last reference dies in that ordered drain and the
	// buffers are destroyed through still-live property chains. (With no UI thread left to
	// drain anything, the release here is already the destruction -- equally inside a
	// window where the chains are alive.) UpdateModel() answers a retired view at Verbose
	// through its registration gate, which runs BEFORE the map lookup for this reason.
	Models.Empty();

	// After this, a routed item still in flight for this view drops at Verbose in the
	// drain -- the same fate as input for a dead view, one rule for both directions.
	FVaCuusWriteRouter::UnregisterGameView(ViewId);
}

void UVaCuusView::BeginDestroy()
{
	// The safety net. Every real teardown path (SVaCuusWidget::DetachView, the subsystem's
	// Invalidate) has already detached by now; this is what keeps a view nobody retired from
	// leaving a registered context behind.
	DetachIme();

	// Same safety net for the router's dispatch map: without it, a never-invalidated
	// view leaves a stale (null-reading, but permanent) entry behind. Idempotent, and
	// guarded because CDO construction/destruction may run off the game thread while a
	// real view's BeginDestroy is GC's, i.e. game-thread.
	if (ViewId != 0 && IsInGameThread())
	{
		FVaCuusWriteRouter::UnregisterGameView(ViewId);
	}

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

	DocumentPath = VfsPath;

	const uint64 Serial = NextLoadSerial++;
	Status->LoadRequestSerial.store(Serial, std::memory_order_relaxed);
	UIThread->EnqueueLoadDocumentFile(ViewId, VfsPath, Serial, LastViewSize);
}

bool UVaCuusView::ReloadDocument()
{
	check(IsInGameThread());

	if (DocumentPath.IsEmpty())
	{
		return false;
	}

	FVaCuusUIThread* UIThread = GetUIThread();
	if (!UIThread)
	{
		// Ordinary during teardown, and the caller (a watcher flush) is fanning out over
		// whatever it found a moment ago -- so this is not worth a Warning.
		UE_LOG(LogVaCuus, Verbose, TEXT("ReloadDocument() on an invalid view is ignored"));
		return false;
	}

	const uint64 Serial = NextLoadSerial++;
	Status->LoadRequestSerial.store(Serial, std::memory_order_relaxed);

	// No cache clear here: it is not this view's business (the caches are process-global)
	// and it must happen even when there is no view at all. The dispatcher that decided to
	// reload enqueues one FVaCuusUIThread::EnqueueClearAssetCaches() before fanning out,
	// and FIFO ordering on a single-producer queue puts it ahead of this load.
	UIThread->EnqueueLoadDocumentFile(ViewId, DocumentPath, Serial, LastViewSize);

	UE_LOG(LogVaCuus, Log, TEXT("View %u: reload of '%s' queued (load serial %llu)"), ViewId, *DocumentPath, Serial);
	return true;
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

	// Cleared, not kept: DocumentPath describes what is SHOWING, and an inline document
	// has no file behind it (see GetDocumentPath()). A view that fell back to an inline
	// document is re-armed by its owner through
	// UVaCuusSubsystem::OnDocumentsReloadRequested, not by a stale path left here.
	DocumentPath.Reset();

	const uint64 Serial = NextLoadSerial++;
	Status->LoadRequestSerial.store(Serial, std::memory_order_relaxed);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, RmlSource, Serial, LastViewSize);
}

void UVaCuusView::ExecuteScript(const FString& Source)
{
	check(IsInGameThread());

	FVaCuusUIThread* UIThread = GetUIThread();
	if (!UIThread)
	{
		UE_LOG(LogVaCuus, Warning, TEXT("ExecuteScript() on an invalid view is ignored"));
		return;
	}

	// The source name is deliberately deterministic -- view id, no serial -- so
	// a log reader can attribute an error to the API surface it came through,
	// and a test can match the refusal lines exactly.
	UIThread->EnqueueExecuteScript(ViewId, Source, FString::Printf(TEXT("<ExecuteScript view %u>"), ViewId));
}

void UVaCuusView::ReleaseTexture(const FString& VfsPath)
{
	check(IsInGameThread());

	if (VfsPath.IsEmpty())
	{
		// REFUSED RATHER THAN FORWARDED, because an empty payload is how the command spells
		// "all of them" on the wire -- so a caller that passed an empty variable by accident
		// would silently flush the whole view instead of one image.
		UE_LOG(LogVaCuus, Warning,
			TEXT("View %u: ReleaseTexture('') ignored; call ReleaseAllTextures() to mean that."), ViewId);
		return;
	}

	if (FVaCuusUIThread* UIThread = GetUIThread())
	{
		UIThread->EnqueueReleaseTextures(ViewId, VfsPath);
	}
}

void UVaCuusView::ReleaseAllTextures()
{
	check(IsInGameThread());

	if (FVaCuusUIThread* UIThread = GetUIThread())
	{
		UIThread->EnqueueReleaseTextures(ViewId, FString());
	}
}

void UVaCuusView::CallJs(const FString& FunctionPath, const TArray<FVaCuusJsValue>& Args)
{
	check(IsInGameThread());

	FVaCuusUIThread* UIThread = GetUIThread();
	if (!UIThread)
	{
		UE_LOG(LogVaCuus, Warning, TEXT("CallJs('%s') on an invalid view is ignored"), *FunctionPath);
		return;
	}

	UIThread->EnqueueCallScriptFunction(ViewId, FunctionPath, Args);
}

void UVaCuusView::Close()
{
	check(IsInGameThread());

	// Nothing is up any more, so nothing is reloadable.
	DocumentPath.Reset();

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

bool UVaCuusView::BindModel(const FString& ModelName, const UScriptStruct* Type)
{
	check(IsInGameThread());

	if (Type == nullptr)
	{
		UE_LOG(LogVaCuus, Error, TEXT("View %u: BindModel('%s') needs a struct type"), ViewId, *ModelName);
		return false;
	}

	if (ModelName.IsEmpty())
	{
		// The name is what a document's `data-model` attribute is compared against
		// (Element.cpp:2203-2206), so an unnamed model is one no document can reach.
		UE_LOG(LogVaCuus, Error, TEXT("View %u: BindModel needs a name; a document addresses a model by its `data-model` attribute"),
			ViewId);
		return false;
	}

	// The KEY, not the identity: everything below that indexes does so through this FName
	// (case-insensitive), while the exact-case ModelName string travels inside the model to
	// RmlUi -- the split the header comment on Models explains.
	const FName ModelKey(*ModelName);

	FVaCuusUIThread* UIThread = GetUIThread();
	if (!UIThread)
	{
		UE_LOG(LogVaCuus, Warning, TEXT("BindModel('%s') on an invalid view is ignored"), *ModelName);
		return false;
	}

	if (const TSharedPtr<FVaCuusBoundModel>* ExistingModel = Models.Find(ModelKey))
	{
		if (!(*ExistingModel)->IsCondemned())
		{
			// Refused here rather than left to RmlUi, which would also refuse it -- one
			// Log::LT_ERROR from Context::CreateDataModel (Context.cpp:1075), and that one does
			// reach LogVaCuus (see FVaCuusSystemInterface::LogMessage). What it does NOT say is
			// the part that matters: the second model's values then go nowhere while the FIRST
			// model's shadow stays on screen, so the UI keeps looking plausible. Refusing on this
			// side also keeps the game-thread map single-valued, which nothing downstream rechecks.
			UE_LOG(LogVaCuus, Error,
				TEXT("View %u already has a model called '%s'; the second BindModel is ignored (there is no unbind in RmlUi, so a ")
				TEXT("name cannot be reused on one view; names are compared case-insensitively here)"),
				ViewId, *ModelName);
			return false;
		}

		// THE RECOVERY RE-BIND (VaCuus-akj.16): the entry is a corpse -- torn down when its
		// struct was recompiled -- and refusing to replace it would leave the name dead for
		// the life of the view. The no-unbind rule above is not violated: the recompile drop
		// already ran Context::RemoveDataModel (which erases the name, Context.cpp:1111), so
		// RmlUi will accept the re-creation. Replacing the map entry drops the last game-side
		// reference; the UI side let go in the drop, so the corpse is destroyed here, with
		// its drop state at TornDown and its buffers already empty. The caller still owes a
		// document reload: the detach RemoveDataModel did to a LOADED document is one-way.
		UE_LOG(LogVaCuus, Log,
			TEXT("View %u: model '%s' replaces the one torn down by the struct recompile; reload the document so the new model ")
			TEXT("attaches"),
			ViewId, *ModelName);
		Models.Remove(ModelKey);
	}

	if (NextLoadSerial > 1)
	{
		// THE ONLY WARNING THERE IS FOR CONTRACT 1, and it exists because RmlUi's own arrives
		// too late and from the wrong side: `data-model` is resolved in Element::SetParent
		// (Element.cpp:2202-2219) and a miss is a Log::LT_ERROR that DOES reach LogVaCuus (see
		// FVaCuusSystemInterface::LogMessage) -- but it is emitted at LOAD time, names the
		// element, and by then the mistake is unfixable. This fires at BIND time, on the game
		// thread, and says what to do about it. A model created now cannot attach to a
		// document that is already up.
		//
		// A WARNING AND NOT A REFUSAL: the model is still correct for the NEXT load, which is
		// exactly what a live reload or a view that swaps documents will do -- and refusing
		// would break that. The load serial is used rather than a bLoaded flag because it
		// counts REQUESTS, so this fires even while the first load is still in flight.
		UE_LOG(LogVaCuus, Warning,
			TEXT("View %u: BindModel('%s') after a document load was already requested. RmlUi resolves `data-model` once, when a ")
			TEXT("document is parented into the context, so this model will NOT attach to the document that is up -- only to the ")
			TEXT("next one loaded. Bind every model before the first LoadDocument."),
			ViewId, *ModelName);
	}

	const TSharedRef<FVaCuusBoundModel> Model = MakeShared<FVaCuusBoundModel>(ModelName, Type);
	if (!Model->IsValid())
	{
		// The layout walk has already logged whatever it could not resolve.
		UE_LOG(LogVaCuus, Error, TEXT("View %u: BindModel('%s') could not build a model over '%s'"), ViewId, *ModelName,
			*Type->GetName());
		return false;
	}

	if (Model->GetLayout().GetFields().Num() == 0)
	{
		// Bound anyway: `data-model` still has to resolve or the whole subtree is inert. But a
		// struct that contributed nothing is almost always a mistake (every property refused
		// by the exposure rule, or by RmlUi's name rule), and the per-property lines that say
		// which are easy to miss among a frame's logging.
		UE_LOG(LogVaCuus, Warning,
			TEXT("View %u: model '%s' over '%s' has no bindable field; the model is created so `data-model` resolves, but every ")
			TEXT("expression against it will be empty (see the per-property lines above)"),
			ViewId, *ModelName, *Type->GetName());
	}

	// The game-thread half is live from here; the UI thread creates the RmlUi model when it
	// drains the command below.
	Models.Add(ModelKey, Model);
	UIThread->EnqueueBindModel(ViewId, Model);

	UE_LOG(LogVaCuus, Log, TEXT("View %u: model '%s' over '%s' queued for binding (%d field(s), %d top-level name(s))"), ViewId,
		*ModelName, *Type->GetName(), Model->GetLayout().GetFields().Num(), Model->GetLayout().GetTopLevelNames().Num());
	return true;
}

void UVaCuusView::UpdateModel(FName ModelName, const UScriptStruct* Type, const void* Data)
{
	// The same guard every other mutator here carries, and the same reason: this reads
	// gameplay memory, which has no engine synchronisation of any kind.
	check(IsInGameThread());

	// The registration gate FIRST, because Invalidate() empties the model map: on a retired
	// view "nothing bound" is not the caller's mistake, and this runs at frame rate -- a
	// view can outlive the thing driving it by a frame during teardown, so it drops at
	// Verbose for the reason SendInput() gives. (VaCuus.Model.Api holds this line: its
	// post-DestroyView UpdateModel would push the "before anything was bound" Warning past
	// the exact count the test expects.)
	if (GetUIThread() == nullptr)
	{
		UE_LOG(LogVaCuus, Verbose, TEXT("UpdateModel('%s') on an invalid view is ignored"), *ModelName.ToString());
		return;
	}

	const TSharedPtr<FVaCuusBoundModel>* Found = Models.Find(ModelName);
	if (Found == nullptr)
	{
		UE_LOG(LogVaCuus, Warning,
			TEXT("View %u: UpdateModel('%s') before anything was bound under that name; nothing was read. Call BindModel(), and ")
			TEXT("call it before LoadDocument()."),
			ViewId, *ModelName.ToString());
		return;
	}

	FVaCuusBoundModel& Model = **Found;

	// FIRST OF THE THREE VALUE CHECKS, because it is the one that cannot be recovered from:
	// every offset the sampler is about to use came out of THIS model's layout, and applying
	// them to an instance of another type reads whatever sits at those bytes.
	if (Type != Model.GetLayout().GetStruct())
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("View %u: UpdateModel('%s') was given a '%s' but the model is bound over '%s'; nothing was read"), ViewId,
			*ModelName.ToString(), Type != nullptr ? *Type->GetName() : TEXT("none"),
			Model.GetLayout().GetStruct() != nullptr ? *Model.GetLayout().GetStruct()->GetName() : TEXT("none"));
		return;
	}

	if (Data == nullptr)
	{
		UE_LOG(LogVaCuus, Warning, TEXT("View %u: UpdateModel('%s') was given a null pointer; nothing was read"), ViewId,
			*ModelName.ToString());
		return;
	}

	// MEASURED HERE RATHER THAN INSIDE THE GameTick SCOPE, and that is a deliberate deviation
	// from spec 6's wording ("must be driven from UVaCuusSubsystem::Tick"). The sample can
	// only run where the data is: the only pointer to the live struct VaCuus ever sees is this
	// argument, and a Blueprint wildcard pin's pointer is valid for the duration of the call
	// and no longer. Deferring the diff to Tick would mean copying the whole struct into a
	// third buffer first -- roughly doubling the very cost spec 9 budgets -- so what moves to
	// Tick is the PUBLISH (see PublishModelUpdates), and the sample gets a scope of its own so
	// that "outside every measurement" does not happen either way.
	//
	// Per CALL, not per frame, like the Input scope: read it as a sum over the frame.
	VACUUS_PERF_SCOPE(ModelSample);
	Model.Sample(Type, Data);
}

bool UVaCuusView::HasModel(FName ModelName) const
{
	check(IsInGameThread());
	return Models.Contains(ModelName);
}

int32 UVaCuusView::RefuseModelsForStructRecompile(
	const UScriptStruct* ChangedStruct, TArray<TPair<uint32, TSharedRef<FVaCuusBoundModel>>>& OutCondemned)
{
	check(IsInGameThread());

	if (ChangedStruct == nullptr)
	{
		return 0;
	}

	// The match rule, both halves (the header's contract): the model's own root, or any
	// array field's ELEMENT type. Element layouts cannot nest further arrays (the desc build
	// refuses nested containers, VaCuusModelLayout.h's ElementLayout comment), so one level
	// of descent is the whole surface. Nested-by-value structs need no clause of their own:
	// their leaves belong to the CONTAINING type's compile set, and the engine broadcasts a
	// PreChange for every dependent struct it recompiles in the same transaction
	// (UserDefinedStructureCompilerUtils.cpp:585-616 loops the growing ChangedStructs array),
	// so the containing root matches on its own broadcast.
	int32 NumCondemned = 0;
	for (const TPair<FName, TSharedPtr<FVaCuusBoundModel>>& Pair : Models)
	{
		FVaCuusBoundModel& Model = *Pair.Value;

		bool bMatches = Model.GetLayout().GetStruct() == ChangedStruct;
		if (!bMatches)
		{
			for (const FVaCuusModelField& Field : Model.GetLayout().GetFields())
			{
				if (Field.ArrayDesc != nullptr && Field.ArrayDesc->ElementLayout.IsValid() &&
					Field.ArrayDesc->ElementLayout->GetStruct() == ChangedStruct)
				{
					bMatches = true;
					break;
				}
			}
		}

		// CondemnForStructRecompile answers false for a model already condemned (the same
		// transaction's second broadcast), which is what holds this at ONE Error and ONE
		// drop command per model per incident.
		if (!bMatches || !Model.CondemnForStructRecompile())
		{
			continue;
		}

		UE_LOG(LogVaCuus, Error,
			TEXT("View %u: model '%s' is torn down -- its struct '%s' is being recompiled and every FProperty the model resolved ")
			TEXT("dies with the old layout. Sample/UpdateModel are refused from now on; re-bind the model and reload the ")
			TEXT("document to recover"),
			ViewId, *Model.GetModelNameString(), *ChangedStruct->GetName());

		OutCondemned.Emplace(ViewId, Pair.Value.ToSharedRef());
		++NumCondemned;
	}

	return NumCondemned;
}

int32 UVaCuusView::NumOutstandingModelFields(FName ModelName)
{
	check(IsInGameThread());

	const TSharedPtr<FVaCuusBoundModel>* Found = Models.Find(ModelName);
	return Found != nullptr ? (*Found)->NumOutstandingFields() : INDEX_NONE;
}

int32 UVaCuusView::DumpModel(FName ModelName)
{
	check(IsInGameThread());

	int32 NumDumped = 0;
	for (const TPair<FName, TSharedPtr<FVaCuusBoundModel>>& Pair : Models)
	{
		if (ModelName.IsNone() || Pair.Key == ModelName)
		{
			Pair.Value->DumpGameSide(ViewId);
			++NumDumped;
		}
	}

	if (NumDumped == 0)
	{
		UE_LOG(LogVaCuus, Display, TEXT("DumpModel: view %u has no model called '%s' (%d bound: %s)"), ViewId,
			*ModelName.ToString(), Models.Num(), Models.IsEmpty() ? TEXT("none") : TEXT("see vacuus.DumpModel with no arguments"));
		return 0;
	}

	// ENQUEUED EVEN WHEN THE VIEW IS NO LONGER REGISTERED -- EnqueueDumpModel drops it with a
	// log line of its own in that case (Enqueue()'s stopping gate), which is more useful than
	// this function deciding not to ask. The half that does arrive still names the view, so a
	// missing UI half is never ambiguous about which view it belonged to.
	if (FVaCuusUIThread* UIThread = GetUIThread())
	{
		UIThread->EnqueueDumpModel(ViewId, ModelName);
	}
	else
	{
		UE_LOG(LogVaCuus, Display,
			TEXT("DumpModel: view %u is no longer registered, so there is no UI-thread half to print"), ViewId);
	}

	return NumDumped;
}

void UVaCuusView::DumpNodeCount()
{
	check(IsInGameThread());

	if (FVaCuusUIThread* UIThread = GetUIThread())
	{
		UIThread->EnqueueDumpNodeCount(ViewId);
	}
	else
	{
		UE_LOG(LogVaCuus, Display,
			TEXT("NodeCount: view %u is no longer registered, so there is no tree to count"), ViewId);
	}
}

void UVaCuusView::PublishModelUpdates()
{
	check(IsInGameThread());

	if (!bRegistered)
	{
		// Nothing is consuming the channel any more -- the UI thread dropped its models when
		// it drained the RemoveView -- so a publish here would only swap buffers nobody reads.
		return;
	}

	for (const TPair<FName, TSharedPtr<FVaCuusBoundModel>>& Pair : Models)
	{
		// Free when nothing is outstanding: no swap, no generation bump, no UI-thread work.
		// That is what makes a bound-but-unchanging model cost zero published frames.
		Pair.Value->PublishPending();
	}
}

bool UVaCuusView::CreateModelFromStruct(const FString& ModelName, const int32& Struct)
{
	// Never called: the UFUNCTION is CustomThunk, so Blueprint dispatches to
	// execCreateModelFromStruct below and C++ callers use BindModel() directly. A body exists
	// only because the declaration does. Same shape as the engine's own wildcard nodes
	// (UBlueprintMapLibrary::Map_Add and friends).
	checkNoEntry();
	return false;
}

void UVaCuusView::UpdateWholeModel(FName ModelName, const int32& Struct)
{
	checkNoEntry();
}

DEFINE_FUNCTION(UVaCuusView::execCreateModelFromStruct)
{
	// A STRING OFF THE STACK, NOT A NAME: an FName here would hand BindModel the name pool's
	// casing rather than the designer's in a cooked game -- BindModel's header comment has the
	// full mechanism (VaCuus-akj.23).
	P_GET_PROPERTY(FStrProperty, ModelName);

	// THE WILDCARD PIN, AND WHERE THE TYPE COMES FROM. StepCompiledIn<FStructProperty> walks
	// the next argument on the script stack and leaves MostRecentProperty describing whatever
	// struct the designer actually wired in -- which is the whole reason the Blueprint node
	// needs no type parameter and yet reaches exactly the same check C++ does. Both pointers
	// are cleared first because Step only writes them when the argument is a property
	// reference rather than a temporary.
	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FStructProperty>(nullptr);
	const FStructProperty* StructProperty = CastField<FStructProperty>(Stack.MostRecentProperty);

	P_FINISH;

	bool bResult = false;
	P_NATIVE_BEGIN;
	bResult = P_THIS->BindModel(ModelName, StructProperty != nullptr ? StructProperty->Struct : nullptr);
	P_NATIVE_END;

	*static_cast<bool*>(RESULT_PARAM) = bResult;
}

DEFINE_FUNCTION(UVaCuusView::execUpdateWholeModel)
{
	P_GET_PROPERTY(FNameProperty, ModelName);

	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FStructProperty>(nullptr);
	const FStructProperty* StructProperty = CastField<FStructProperty>(Stack.MostRecentProperty);
	const void* StructData = Stack.MostRecentPropertyAddress;

	P_FINISH;

	P_NATIVE_BEGIN;
	// The type and the data come from the ONE pin, so they cannot disagree -- and both go
	// through UpdateModel(), so an unconnected pin (null property, null address) produces the
	// same two log lines a C++ caller would get rather than a special Blueprint path.
	P_THIS->UpdateModel(ModelName, StructProperty != nullptr ? StructProperty->Struct : nullptr, StructData);
	P_NATIVE_END;
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

uint64 UVaCuusView::GetFramesRecorded() const
{
	return Status.IsValid() ? Status->FramesRecorded.load(std::memory_order_acquire) : 0;
}

uint64 UVaCuusView::GetFramesPublished() const
{
	return Status.IsValid() ? Status->FramesPublished.load(std::memory_order_acquire) : 0;
}

int32 UVaCuusView::GetLastPublishedCommandCount() const
{
	return Status.IsValid() ? Status->LastPublishedCommands.load(std::memory_order_acquire) : 0;
}

int32 UVaCuusView::GetLastPublishedDrawCallCount() const
{
	return Status.IsValid() ? Status->LastPublishedDrawCalls.load(std::memory_order_acquire) : 0;
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
	// A dead view has nothing pending (M6 review): after the boot-failure admission --
	// PollStatus's latch, which also invalidated the handle and broadcast the one
	// OnLoadCompleted(false) -- the completed serial can never advance (the UI thread
	// dropped the host, so no result will ever be published), and the serial comparison
	// alone would answer "pending" forever. Gated on the game-side LATCH rather than on
	// BootState directly, deliberately: the raw flag is stamped whenever the UI thread's
	// drain happens to run (Enqueue() triggers a frame, so that races the very next game
	// statement), while every observable on this handle changes at the POLL -- a mid-frame
	// flip here would be the one exception, and VaCuus.View.BootFailure's "pending before
	// the drain" assertion is what catches it.
	if (bBootFailureReported)
	{
		return false;
	}

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

	// A view whose host never booted admits it here (bead VaCuus-akj.13). Before the
	// snapshot refresh because there is nothing to refresh: the host was dropped on the UI
	// thread, so no snapshot, frame or load result will ever be published for this view.
	// Acquire pairs with the release store in FVaCuusUIThread::AddView's failure branches.
	if (!bBootFailureReported &&
		static_cast<EVaCuusViewBootState>(Status->BootState.load(std::memory_order_acquire)) ==
			EVaCuusViewBootState::Failed)
	{
		bBootFailureReported = true;

		// One Error naming the view; the UI thread already logged WHY it failed to boot.
		UE_LOG(LogVaCuus, Error,
			TEXT("View %u never booted: its document host failed to initialize on the UI thread (see the Error ")
			TEXT("logged there). The handle is now invalid; OnLoadCompleted fires once with bSuccess=false."),
			ViewId);

		// Invalidate BEFORE the broadcast, so a listener that reacts by querying the view
		// sees IsViewValid() == false -- the honest answer, and the one every later call
		// site (LoadDocument, SendInput, ...) already handles as a logged no-op.
		Invalidate();
		OnLoadCompleted.Broadcast(this, /*bSuccess=*/false);
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
