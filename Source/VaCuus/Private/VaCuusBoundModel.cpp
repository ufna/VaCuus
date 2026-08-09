// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusBoundModel.h"

#include "VaCuusDataVariable.h"
#include "VaCuusDefines.h"
#include "VaCuusTranslationVariable.h"
#include "VaCuusUIThread.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "UObject/Class.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>

namespace VaCuusBoundModelPrivate
{
/** UE string -> RmlUi's std::string. RmlUi's Strings are UTF-8 throughout. */
static Rml::String ToRmlString(const FString& Value)
{
	return Rml::String(TCHAR_TO_UTF8(*Value));
}
}	 // namespace VaCuusBoundModelPrivate

FVaCuusBoundModel::FVaCuusBoundModel(const FString& InModelName, const UScriptStruct* InStruct)
	: ModelNameStr(InModelName)
	// The FName is DERIVED, never the source: it exists for the case-insensitive lookups
	// (write-router registry, DumpModel matching). Deriving it may alias an unrelated
	// registration's casing (see the constructor comment in the header), which is exactly
	// why nothing below is allowed to stringify it.
	, ModelName(*InModelName)
	, Layout(InStruct)
	, Sampler(Layout)
	, Channel(Layout)
	, UIShadow(InStruct)
{
	// Nothing else: the channel's constructor is where invariant I1 lives (it is born fully
	// dirty), and both shadows are already initialised instances of InStruct.
}

FVaCuusBoundModel::~FVaCuusBoundModel()
{
	// THE SAFETY NET FOR THE DROP THAT NEVER RAN (akj.16). DropQueued or AbandonRequired at
	// destruction means the UI-side teardown command was refused by a stopping queue or
	// discarded behind a shutdown -- it cannot still be pending, because a pending command
	// holds a TSharedPtr to this object and this destructor could not be running. The struct
	// may have been recompiled since (that is what condemned the model), so the member
	// destructors' DestroyStruct path is the corruption this whole state machine exists to
	// prevent: abandon the buffers first, and the destructors below find them empty.
	//
	// No CAS needed: with both owners' references gone there is no other thread left to race.
	const EVaCuusModelDropState State = GetDropState();
	if (State == EVaCuusModelDropState::DropQueued || State == EVaCuusModelDropState::AbandonRequired)
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("VaCuus model '%s': destroyed with its recompile drop still undelivered; the UI-side buffers are freed ")
			TEXT("WITHOUT destructors and their contents leak (>= %llu bytes)"),
			*ModelNameStr, EstimateAbandonedBytes());

		UIShadow.Abandon();
		Channel.TeardownSlotsForRecompile(/*bStructChainAlive=*/false);
		// The game-side shadow was already Reset() by CondemnForStructRecompile, inside the
		// window where that was still legal.
	}
}

bool FVaCuusBoundModel::IsValid() const
{
	return Layout.IsValid() && Sampler.IsValid() && Channel.IsValid() && UIShadow.IsValid();
}

int32 FVaCuusBoundModel::Sample(const UScriptStruct* LiveType, const void* LiveData)
{
	// Asserted again inside FVaCuusModelSampler::Sample; asserted here too because this is the
	// entry point a future caller reaches first.
	check(IsInGameThread());

	if (bDeadFromRecompile)
	{
		// THE REFUSED SAMPLE (akj.16): the layout's offsets and the sampler's shadow died
		// with the recompile, so reading LiveData through them is exactly the corruption the
		// refusal exists to prevent. Once, not per frame -- this is called at frame rate.
		if (!bLoggedRefusedSample)
		{
			bLoggedRefusedSample = true;
			UE_LOG(LogVaCuus, Warning,
				TEXT("VaCuus model '%s': Sample refused -- the model was torn down when '%s' was recompiled. ")
				TEXT("Re-bind the model (BindModel replaces a dead entry) and reload the document to recover"),
				*ModelNameStr, Layout.GetStruct() != nullptr ? *Layout.GetStruct()->GetName() : TEXT("none"));
		}
		return 0;
	}

	return Sampler.Sample(LiveType, LiveData, Channel);
}

bool FVaCuusBoundModel::PublishPending()
{
	check(IsInGameThread());

	if (bDeadFromRecompile)
	{
		// Quietly, unlike Sample's latched Warning: a publish with a dead sampler has nothing
		// to say that Sample has not already said, and this runs once per frame per model
		// from the subsystem's tick whether or not anyone called UpdateModel.
		return false;
	}

	return Channel.Publish(Sampler.GetShadow());
}

bool FVaCuusBoundModel::CondemnForStructRecompile()
{
	check(IsInGameThread());

	if (bDeadFromRecompile)
	{
		// The second broadcast of the same transaction (the engine appends dependent structs
		// to the compile set and broadcasts each -- UserDefinedStructureCompilerUtils.cpp:585-616
		// loops a growing array): already condemned, nothing more to do, and returning false
		// is what keeps it at ONE Error and ONE drop command per model per incident.
		return false;
	}

	// ORDER: the flag first, then the shadow. Everything after this line runs with Sample and
	// PublishPending already refusing, so the producer side of the channel is quiescent from
	// here on -- the property TeardownSlotsForRecompile's contract stands on.
	bDeadFromRecompile = true;

	// Inside PreChange the OLD chain is provably alive (the broadcast at
	// UserDefinedStructureCompilerUtils.cpp:599 precedes the compile at :622), so this is the
	// last moment the game-side shadow can be destroyed through the type.
	Sampler.DropShadowForStructTeardown();

	DropState.store(static_cast<uint8>(EVaCuusModelDropState::DropQueued), std::memory_order_release);
	return true;
}

bool FVaCuusBoundModel::ResolveDropTimeout()
{
	check(IsInGameThread());

	uint8 Expected = static_cast<uint8>(EVaCuusModelDropState::DropQueued);
	if (DropState.compare_exchange_strong(Expected, static_cast<uint8>(EVaCuusModelDropState::AbandonRequired),
			std::memory_order_acq_rel))
	{
		// The UI thread never got there. From the moment PreChange returns the old chain is
		// fair game for the compile, so the late drop (or the destructor) must Abandon().
		return true;
	}

	// The UI side won the CAS. If it is mid-DestroyStruct, WAIT -- unbounded, deliberately:
	// this blocks the recompile for the tail of one teardown (microseconds), while returning
	// early would let the compile free the chain that teardown is walking. A UI thread that
	// dies inside the teardown leaves the editor blocked here, which is loud and debuggable;
	// the alternative is silent heap corruption.
	double NextComplaint = FPlatformTime::Seconds() + 1.0;
	while (GetDropState() == EVaCuusModelDropState::Dropping)
	{
		if (FPlatformTime::Seconds() >= NextComplaint)
		{
			NextComplaint = FPlatformTime::Seconds() + 1.0;
			UE_LOG(LogVaCuus, Warning,
				TEXT("VaCuus model '%s': still waiting on the UI thread to finish the recompile teardown"), *ModelNameStr);
		}
		FPlatformProcess::Sleep(0.0001f);
	}

	return false;
}

uint64 FVaCuusBoundModel::EstimateAbandonedBytes() const
{
	const UScriptStruct* Struct = Layout.GetStruct();
	const uint64 Stride = Struct != nullptr ? static_cast<uint64>(Struct->GetStructureSize()) : 0;

	// One UI shadow plus however many channel slots the producer ever allocated. A lower
	// bound by construction: the heap payloads inside those instances are invisible without
	// walking the (dying) property chain.
	return Stride * (1 + static_cast<uint64>(Channel.GetNumSlotBuffersAllocated()));
}

void FVaCuusBoundModel::TearDownUISideForRecompile()
{
	// The drop command's drain -- the real UI thread, or the game thread inside
	// RunFrameInline, both of which satisfy this.
	check(FVaCuusUIThread::IsInUIThread());

	uint8 Expected = static_cast<uint8>(EVaCuusModelDropState::DropQueued);
	const bool bStructChainAlive = DropState.compare_exchange_strong(Expected,
		static_cast<uint8>(EVaCuusModelDropState::Dropping), std::memory_order_acq_rel);

	if (!bStructChainAlive && Expected != static_cast<uint8>(EVaCuusModelDropState::AbandonRequired))
	{
		// Live (never condemned), Dropping or TornDown here is a caller bug -- the command is
		// enqueued exactly once, by the same call that condemned the model.
		UE_LOG(LogVaCuus, Error, TEXT("VaCuus model '%s': recompile teardown found drop state %u; nothing torn down"),
			*ModelNameStr, static_cast<uint32>(Expected));
		return;
	}

	// Winning the CAS proves the game thread is still parked inside PreChange (its timeout
	// CAS lost), so the OLD chain is alive for the whole teardown below and the normal
	// DestroyStruct path is safe. Losing it to AbandonRequired means the fence timed out:
	// free without destructors, leak the contents -- the timeout already logged the Error.
	bBoundToContext = false;
	ModelHandle = Rml::DataModelHandle();
	TopLevelNamesUtf8.Empty();

	if (bStructChainAlive)
	{
		UIShadow.Reset();
	}
	else
	{
		UIShadow.Abandon();
	}
	Channel.TeardownSlotsForRecompile(bStructChainAlive);

	DropState.store(static_cast<uint8>(EVaCuusModelDropState::TornDown), std::memory_order_release);
}

bool FVaCuusBoundModel::BindToContext(Rml::Context& Context)
{
	check(FVaCuusUIThread::IsInUIThread());

	if (bBoundToContext)
	{
		// There is no unbind anywhere in RmlUi, so a second bind of the same model object
		// could only produce a second DataModel holding the same shadow pointer -- with the
		// first one still live and still dirtying. Refused rather than tolerated.
		UE_LOG(LogVaCuus, Error, TEXT("VaCuus model '%s' is already bound to a context; the second bind is ignored"),
			*ModelNameStr);
		return false;
	}

	if (!IsValid())
	{
		UE_LOG(LogVaCuus, Error, TEXT("VaCuus model '%s' could not be built (type '%s'); nothing is bound"),
			*ModelNameStr, Layout.GetStruct() != nullptr ? *Layout.GetStruct()->GetName() : TEXT("none"));
		return false;
	}

	// ModelNameStr AND NEVER ModelName.ToString(): `data-model` is matched byte-for-byte
	// (Context::GetDataModelPtr is a find() on a map keyed on std::string,
	// Context.cpp:1522-1526 over Context.h:383), while an FName round-trip in a packaged game
	// (WITH_CASE_PRESERVING_NAME=0, NameTypes.h:32-33) returns whatever casing was registered
	// FIRST process-wide -- the engine's 'HUD' (class AHUD) turned FName("hud") into a model
	// called 'HUD' that no document could address (VaCuus-akj.23).
	Rml::DataModelConstructor Constructor = Context.CreateDataModel(VaCuusBoundModelPrivate::ToRmlString(ModelNameStr));
	if (!Constructor)
	{
		// The one way this fails: the name is already taken in THIS context. RmlUi logs
		// Log::LT_ERROR ("Data model name '%s' already exists.") and hands back a
		// default-constructed (falsy) constructor (Context.cpp:1066-1076). That line DOES reach
		// the log, as `LogVaCuus: Error: [Rml] ...` -- see FVaCuusSystemInterface::LogMessage --
		// but it names only the model, not the view, and says nothing about the consequence
		// below, which is the reason this line exists as well rather than instead.
		//
		// The existing model KEEPS ITS OLD SHADOW POINTER, which is why this must not be
		// tolerated as "close enough": the document would carry on reading a buffer this
		// object is not the one filling.
		UE_LOG(LogVaCuus, Error,
			TEXT("VaCuus model '%s': this view's context already has a data model of that name; nothing is bound, and the ")
			TEXT("existing model still points at the OTHER model's shadow"),
			*ModelNameStr);
		return false;
	}

	ModelHandle = Constructor.GetModelHandle();

	const int32 NumBound = VaCuusData::BindModelVariables(Constructor, Layout, UIShadow);

	// Converted once, here, because DirtyVariable takes a std::string and the apply runs per
	// dirtied field per UI frame. Sized and ordered exactly like GetTopLevelNames(), because
	// FVaCuusModelField::TopLevelNameIndex indexes both.
	TopLevelNamesUtf8.Reset(Layout.GetTopLevelNames().Num());
	for (const FString& TopLevelName : Layout.GetTopLevelNames())
	{
		TopLevelNamesUtf8.Add(VaCuusBoundModelPrivate::ToRmlString(TopLevelName));
	}

	bBoundToContext = true;

	// STILL A SUCCESS WITH ZERO VARIABLES, and deliberately: what a document needs in order
	// not to be inert is the MODEL to exist, because that is what `data-model` resolves
	// against in Element::SetParent. A struct whose every property was refused (spec 3.3's
	// name rule, or an unsupported kind) still gets an empty model, and the layout walk has
	// already said why per property.
	UE_LOG(LogVaCuus, Log, TEXT("VaCuus model '%s' bound on the UI thread: %d of %d top-level variable(s), %d field(s) of '%s'"),
		*ModelNameStr, NumBound, Layout.GetTopLevelNames().Num(), Layout.GetFields().Num(),
		Layout.GetStruct() != nullptr ? *Layout.GetStruct()->GetName() : TEXT("none"));

	return true;
}

void FVaCuusBoundModel::DirtyTranslations()
{
	check(FVaCuusUIThread::IsInUIThread());

	if (!bBoundToContext)
	{
		return;
	}

	VaCuusTranslationVariable::Dirty(ModelHandle);
}

void FVaCuusBoundModel::ApplyPendingUpdate()
{
	check(FVaCuusUIThread::IsInUIThread());

	if (GetDropState() != EVaCuusModelDropState::Live)
	{
		// Condemned for a recompile (akj.16): never consume again. The window this guards is
		// narrow and real -- a frame already past DrainCommands when the fence TIMES OUT
		// would otherwise apply a pre-condemnation publish by copying through FProperty
		// pointers the compile has just freed. (Visibility: the condemning store precedes the
		// ~100 ms fence, so any frame reaching here after the timeout is far past it.) The
		// drop command removes this model from the frame loop entirely one drain later.
		return;
	}

	if (!bBoundToContext)
	{
		// NOT CONSUMED, on purpose. Consuming would echo an applied generation back and let the
		// producer stop republishing -- for values that reached no DataModel and dirtied
		// nothing. Leaving the channel untouched costs one republish of the same handful of
		// fields per frame (its cost is bounded by the number of distinct changed fields, never
		// by how long this lasts) and leaves NumOutstandingFields() climbing, which is the
		// observable that says "this model never bound".
		return;
	}

	// The echo is folded into ConsumeUpdate -- it stores the applied generation itself, after
	// this lambda returns, because a forgotten echo is invisible: everything keeps working and
	// only the republish cost grows.
	Channel.ConsumeUpdate([this](const FVaCuusModelUpdate& Update) { ApplyUpdate(Update); });
}

void FVaCuusBoundModel::DirtyTopLevelFromShadow(const FString& TopLevelName)
{
	check(FVaCuusUIThread::IsInUIThread());

	if (!bBoundToContext)
	{
		// A routed write can only have originated from a bound model's own views, so this
		// is teardown-window territory, not a caller bug: nothing to re-run, nothing lost.
		return;
	}

	// The same call ApplyUpdate makes per dirtied field, minus the copy -- the shadow
	// already holds the authoritative value, which is the entire point of the revert.
	// Converted per call rather than through TopLevelNamesUtf8: routed writes happen at
	// user-interaction rate, and the router hands over the name it derived from the wire
	// path, not an index.
	ModelHandle.DirtyVariable(VaCuusBoundModelPrivate::ToRmlString(TopLevelName));
}

void FVaCuusBoundModel::ApplyUpdate(const FVaCuusModelUpdate& Update)
{
	// Both are instances of the layout's type, so the layout's offsets address the same field
	// in each. Checked rather than assumed: this is the one place two independently allocated
	// buffers meet a shared offset table, and a mismatch reads whatever sits at those bytes.
	checkf(Update.Values.GetStruct() == UIShadow.GetStruct(),
		TEXT("VaCuus model '%s': the published slot is an instance of a different type from the UI shadow"),
		*ModelNameStr);
	checkf(Update.DirtyFields.Num() == Layout.GetFields().Num(),
		TEXT("VaCuus model '%s': the published dirty set has %d bits for a layout with %d fields"), *ModelNameStr,
		Update.DirtyFields.Num(), Layout.GetFields().Num());

	const TConstArrayView<FVaCuusModelField> Fields = Layout.GetFields();
	void* ShadowBase = UIShadow.GetData();
	const void* PublishedBase = Update.Values.GetData();

	// DRIVEN BY THE BITS, AND THE COPY LIVES INSIDE THE DIRTYING LOOP (spec 4). Two properties
	// fall out of that shape and neither is expressible any other way round:
	//
	//  - a field that is not dirty is never copied, so the slot's stale non-dirty values (the
	//    triple buffer recycles its three buffers and nothing clears them) can never be read
	//    as current;
	//  - a field that IS copied is dirtied in the same iteration, so "written to the shadow
	//    but never announced to RmlUi" -- the stale-value-forever failure this milestone is
	//    built against -- cannot be introduced by editing one branch.
	for (TConstSetBitIterator<> It(Update.DirtyFields); It; ++It)
	{
		const FVaCuusModelField& Field = Fields[It.GetIndex()];

		Field.CopyValue(ShadowBase, PublishedBase);

		// TOP-LEVEL NAMES ONLY: DataModel::DirtyVariable asserts LegalVariableName and looks
		// the name up in `variables` (DataModel.cpp:325-331), so a dotted path matches nothing
		// -- and both of those guards are RMLUI_ASSERTMSG, i.e. compiled out here. A nested
		// leaf therefore dirties its ROOT's name, which is what TopLevelNameIndex is for.
		//
		// Repeats within one update are free: dirty_variables is a SmallUnorderedSet<String>
		// (DataTypes.h:35) and this is an emplace, so Origin.X and Origin.Y both dirtying
		// "Origin" costs one entry and one re-evaluation.
		ModelHandle.DirtyVariable(TopLevelNamesUtf8[Field.TopLevelNameIndex]);

		++NumFieldsApplied;
	}
}

void FVaCuusBoundModel::DumpGameSide(uint32 ViewId)
{
	check(IsInGameThread());

	// FIRST, because it is what harvests the echo -- and the two sets printed below are read
	// straight out of the channel afterwards, so without this they would show fields the UI has
	// already confirmed. The count is printed too: it is the number UVaCuusView's own
	// NumOutstandingModelFields() reports, and a dump that disagreed with the public observable
	// would send the reader looking in the wrong place.
	const int32 NumOutstanding = NumOutstandingFields();

	const TConstArrayView<FVaCuusModelField> Fields = Layout.GetFields();
	const TConstArrayView<FString> TopLevelNames = Layout.GetTopLevelNames();

	// Display, not Log: this is the answer to a console command somebody just typed, and
	// LogVaCuus's default verbosity would otherwise decide whether they see it.
	UE_LOG(LogVaCuus, Display,
		TEXT("DumpModel: view %u model '%s' over '%s' -- %d field(s), %d top-level name(s), %s"), ViewId, *ModelNameStr,
		Layout.GetStruct() != nullptr ? *Layout.GetStruct()->GetName() : TEXT("none"), Fields.Num(), TopLevelNames.Num(),
		IsValid() ? TEXT("valid") : TEXT("INVALID (a shadow or the layout could not be built)"));

	UE_LOG(LogVaCuus, Display,
		TEXT("DumpModel:   game thread: samples=%llu fieldsMarked=%llu publishes=%llu fieldsPublished=%llu ")
		TEXT("lastPublishedGeneration=%llu appliedGenerationEcho=%llu outstanding=%d"),
		GetNumSamples(), GetNumFieldsMarked(), GetNumPublishes(), GetNumFieldsPublished(), GetLastPublishedGeneration(),
		GetAppliedGeneration(), NumOutstanding);

	const TBitArray<>& PendingFields = Channel.GetPendingFields();
	const TBitArray<>& UnackedFields = Channel.GetUnackedFields();
	const void* GameBase = GetGameShadow().GetData();

	for (int32 Index = 0; Index < Fields.Num(); ++Index)
	{
		const FVaCuusModelField& Field = Fields[Index];

		UE_LOG(LogVaCuus, Display, TEXT("DumpModel:   [%2d] %-28s %-14s top='%s' pending=%s unacked=%s game=%s"), Index,
			*Field.WireName, LexToString(Field.Kind),
			TopLevelNames.IsValidIndex(Field.TopLevelNameIndex) ? *TopLevelNames[Field.TopLevelNameIndex] : TEXT("<none>"),
			PendingFields.IsValidIndex(Index) && PendingFields[Index] ? TEXT("Y") : TEXT("."),
			UnackedFields.IsValidIndex(Index) && UnackedFields[Index] ? TEXT("Y") : TEXT("."),
			GameBase != nullptr ? *Field.DescribeValue(GameBase) : TEXT("<no shadow>"));
	}
}

void FVaCuusBoundModel::DumpUISide(uint32 ViewId)
{
	check(FVaCuusUIThread::IsInUIThread());

	// THE LINE THAT MATTERS MOST IS THE ONE THAT SAYS `boundToContext=no`. That is this
	// milestone's signature failure -- a model whose values go nowhere and a document that reads
	// empty. RmlUi does complain about its own half of it (Element.cpp:2218 logs LT_ERROR, which
	// reaches LogVaCuus through FVaCuusSystemInterface::LogMessage), but only about the DOCUMENT
	// finding no model: a bind that never reached a context at all produces no RmlUi call and so
	// no RmlUi line, and this is the only place that distinction is visible.
	UE_LOG(LogVaCuus, Display,
		TEXT("DumpModel:   UI thread (view %u model '%s'): boundToContext=%s updatesApplied=%llu fieldsApplied=%llu ")
		TEXT("appliedGeneration=%llu lastConsumedGeneration=%llu"),
		ViewId, *ModelNameStr, bBoundToContext ? TEXT("yes") : TEXT("NO -- this model reached no document"),
		GetNumUpdatesApplied(), GetNumFieldsApplied(), GetAppliedGeneration(), Channel.GetLastConsumedGeneration());

	const FVaCuusModelUpdate& Published = Channel.GetLastConsumedUpdate();
	const TConstArrayView<FVaCuusModelField> Fields = Layout.GetFields();
	const void* UIBase = UIShadow.GetData();

	UE_LOG(LogVaCuus, Display, TEXT("DumpModel:   published slot: generation=%llu, %d of %d bit(s) set"), Published.Generation,
		Published.DirtyFields.CountSetBits(), Fields.Num());

	for (int32 Index = 0; Index < Fields.Num(); ++Index)
	{
		const FVaCuusModelField& Field = Fields[Index];

		UE_LOG(LogVaCuus, Display, TEXT("DumpModel:   [%2d] %-28s published=%s ui=%s"), Index, *Field.WireName,
			Published.DirtyFields.IsValidIndex(Index) && Published.DirtyFields[Index] ? TEXT("Y") : TEXT("."),
			UIBase != nullptr ? *Field.DescribeValue(UIBase) : TEXT("<no shadow>"));
	}
}
