// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusBoundModel.h"

#include "VaCuusDataVariable.h"
#include "VaCuusDefines.h"
#include "VaCuusUIThread.h"

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

FVaCuusBoundModel::FVaCuusBoundModel(FName InModelName, const UScriptStruct* InStruct)
	: ModelName(InModelName)
	, Layout(InStruct)
	, Sampler(Layout)
	, Channel(Layout)
	, UIShadow(InStruct)
{
	// Nothing else: the channel's constructor is where invariant I1 lives (it is born fully
	// dirty), and both shadows are already initialised instances of InStruct.
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

	return Sampler.Sample(LiveType, LiveData, Channel);
}

bool FVaCuusBoundModel::PublishPending()
{
	check(IsInGameThread());

	return Channel.Publish(Sampler.GetShadow());
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
			*ModelName.ToString());
		return false;
	}

	if (!IsValid())
	{
		UE_LOG(LogVaCuus, Error, TEXT("VaCuus model '%s' could not be built (type '%s'); nothing is bound"),
			*ModelName.ToString(), Layout.GetStruct() != nullptr ? *Layout.GetStruct()->GetName() : TEXT("none"));
		return false;
	}

	const FString NameString = ModelName.ToString();

	Rml::DataModelConstructor Constructor = Context.CreateDataModel(VaCuusBoundModelPrivate::ToRmlString(NameString));
	if (!Constructor)
	{
		// The one way this fails: the name is already taken in THIS context. RmlUi logs
		// Log::LT_ERROR and hands back a default-constructed (falsy) constructor
		// (Context.cpp:1066-1075) -- and its logging is compiled out in every configuration
		// this plugin builds (spec 8), so this line is the only one anybody will see.
		//
		// The existing model KEEPS ITS OLD SHADOW POINTER, which is why this must not be
		// tolerated as "close enough": the document would carry on reading a buffer this
		// object is not the one filling.
		UE_LOG(LogVaCuus, Error,
			TEXT("VaCuus model '%s': this view's context already has a data model of that name; nothing is bound, and the ")
			TEXT("existing model still points at the OTHER model's shadow"),
			*NameString);
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
		*NameString, NumBound, Layout.GetTopLevelNames().Num(), Layout.GetFields().Num(),
		Layout.GetStruct() != nullptr ? *Layout.GetStruct()->GetName() : TEXT("none"));

	return true;
}

void FVaCuusBoundModel::ApplyPendingUpdate()
{
	check(FVaCuusUIThread::IsInUIThread());

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

void FVaCuusBoundModel::ApplyUpdate(const FVaCuusModelUpdate& Update)
{
	// Both are instances of the layout's type, so the layout's offsets address the same field
	// in each. Checked rather than assumed: this is the one place two independently allocated
	// buffers meet a shared offset table, and a mismatch reads whatever sits at those bytes.
	checkf(Update.Values.GetStruct() == UIShadow.GetStruct(),
		TEXT("VaCuus model '%s': the published slot is an instance of a different type from the UI shadow"),
		*ModelName.ToString());
	checkf(Update.DirtyFields.Num() == Layout.GetFields().Num(),
		TEXT("VaCuus model '%s': the published dirty set has %d bits for a layout with %d fields"), *ModelName.ToString(),
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
