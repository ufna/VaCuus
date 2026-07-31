// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusWriteRouter.h"

#include "VaCuusBoundModel.h"
#include "VaCuusDefines.h"
#include "VaCuusGameBridge.h"
#include "VaCuusUIThread.h"
#include "VaCuusView.h"

#include "Algo/AllOf.h"
#include "Containers/SpscQueue.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/StrProperty.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include "UObject/AnsiStrProperty.h"
#include "UObject/Utf8StrProperty.h"

namespace
{
/** One queued crossing: a routed model write, or a `vacuus.emit`. */
struct FVaCuusRoutedItem
{
	enum class EKind : uint8
	{
		ModelWrite,
		JsEvent,
	};

	EKind Kind = EKind::ModelWrite;
	uint32 ViewId = 0;

	/** The model name for a write; the event name for an emit. */
	FName Name;

	/** ModelWrite only: the wire path ("bPaused", "Killfeed[0].Killer"), model name NOT included -- it rides in Name. */
	FString Path;

	/** ModelWrite only. */
	FVaCuusJsValue Value;

	/** JsEvent only. */
	TArray<FVaCuusJsKeyValue> Payload;
};

/**
 * UI-THREAD STATE, plain statics behind asserted entries -- the GNumRefusedSets shape
 * (VaCuusDataVariable.cpp), and the same justification: written and read only from the
 * one thread allowed to call into RmlUi.
 */
bool GRouterRegistered = false;

/** One registered production-bound model. The raw pointer is safe: entries are removed in RemoveView / Exit strictly BEFORE the owning TSharedRefs drop (VaCuusUIThread.cpp). */
struct FRegisteredModel
{
	uint32 ViewId = 0;
	FName ModelName;
	FVaCuusBoundModel* Model = nullptr;
};

TArray<FRegisteredModel> GRegisteredModels;

/** Routed writes awaiting their revert-dirty; flushed by ApplyModelUpdates. AddUnique-deduped -- DirtyVariable twice would only re-evaluate twice. */
TArray<TPair<FVaCuusBoundModel*, FString>> GPendingReverts;

/** The read surface's one-Warning-per-(model, path) latch. Keys are "view|model|path". */
TSet<FString> GReadMissLogged;

/**
 * THE CROSSING. TSpscQueue for the same reasons the command queue gives
 * (VaCuusUIQueues.h); the single producer is whichever thread runs UI frames -- the UI
 * thread, or the game thread in inline mode, sequential and never concurrent, which is
 * the FVaCuusModelChannel consumer-thread argument in the producer seat. The consumer is
 * the game thread. TSpscQueue is unbounded by construction, so the bound is enforced
 * beside it: the producer refuses at QueueCapacity, the consumer decrements per dequeue.
 */
TSpscQueue<FVaCuusRoutedItem> GQueue;
std::atomic<int32> GQueueCount{0};

/** Latched at the first drop of a burst, re-armed by the next successful enqueue -- one line per stall, not one per item. */
bool GDropLogged = false;

std::atomic<uint64> GNumRoutedWrites{0};
std::atomic<uint64> GNumSwallowedEchoes{0};
std::atomic<uint64> GNumDroppedItems{0};

/**
 * GAME-THREAD STATE. The ViewId -> view map the drain dispatches through, and the
 * zero-handler latch. TWeakObjectPtr, not TObjectPtr: this map must never keep a view
 * alive -- ownership is UVaCuusSubsystem's -- and a stale entry must read null, not
 * dangle.
 */
TMap<uint32, TWeakObjectPtr<UVaCuusView>> GGameViews;
TSet<TPair<FName, FString>> GZeroHandlerWarned;

/** "FPlayerHud.Origin.X" -> "Origin.X". Every DiagnosticPath starts with the type name (built that way in FVaCuusModelDefinitions). */
FString TailAfterTypeRoot(const FString& DiagnosticPath)
{
	int32 DotIndex = INDEX_NONE;
	return DiagnosticPath.FindChar(TEXT('.'), DotIndex) ? DiagnosticPath.Mid(DotIndex + 1) : DiagnosticPath;
}

/** "Origin.X" -> "Origin": the only granularity DirtyVariable accepts (DataModel.cpp:325-331). */
FString FirstSegment(const FString& Path)
{
	int32 DotIndex = INDEX_NONE;
	return Path.FindChar(TEXT('.'), DotIndex) ? Path.Left(DotIndex) : Path;
}

/**
 * The write's Variant, coerced to the wire's three kinds (FVaCuusJsValue's comment
 * carries why three is the whole set). Anything exotic degrades to RmlUi's own string
 * form -- Variant::GetInto routes through TypeConverter, the exact text a document
 * would render -- rather than being dropped.
 */
FVaCuusJsValue CoerceVariant(const Rml::Variant& Variant)
{
	switch (Variant.GetType())
	{
		case Rml::Variant::BOOL:
			return FVaCuusJsValue::MakeBool(Variant.Get<bool>());

		case Rml::Variant::BYTE:
		case Rml::Variant::INT:
		case Rml::Variant::INT64:
		case Rml::Variant::UINT:
		case Rml::Variant::UINT64:
		case Rml::Variant::FLOAT:
		case Rml::Variant::DOUBLE:
			return FVaCuusJsValue::MakeNumber(Variant.Get<double>());

		default:
			return FVaCuusJsValue::MakeString(FString(UTF8_TO_TCHAR(Variant.Get<Rml::String>().c_str())));
	}
}

/** Everything a span hit knows. */
struct FAttribution
{
	const FRegisteredModel* Entry = nullptr;
	FString WirePath;
	FString TopLevelName;
};

/**
 * The class comment's span walk: attribution is storage ownership. Live allocations are
 * disjoint, so the first hit is the only possible one; misses on other models cost a
 * pointer compare per model plus one per non-empty array field.
 */
bool AttributeWrite(const void* InValuePtr, const FString& DiagnosticPath, FAttribution& Out)
{
	const uint8* Ptr = static_cast<const uint8*>(InValuePtr);

	for (const FRegisteredModel& Entry : GRegisteredModels)
	{
		const FVaCuusModelShadow& Shadow = Entry.Model->GetUIShadow();
		const uint8* Base = static_cast<const uint8*>(Shadow.GetData());
		if (Base == nullptr)
		{
			continue;
		}

		// The inline span: every top-level and flattened-nested leaf, bitfield storage
		// bytes included (the value pointer of `uint8 b : 1` addresses the shared storage
		// integer inside the struct, so it lands here like any other leaf's).
		if (Ptr >= Base && Ptr < Base + Shadow.GetStruct()->GetStructureSize())
		{
			Out.Entry = &Entry;
			Out.WirePath = TailAfterTypeRoot(DiagnosticPath);
			Out.TopLevelName = FirstSegment(Out.WirePath);
			return true;
		}

		// The element blocks: a row alias's write resolves to GetRawPtr(i) inside an
		// array's heap allocation -- outside the span above, unreachable by any
		// shadow-base test, and exactly the write the M3b I3 fixture makes. The helper
		// is constructed per call over the value pointer and nothing is stored (the
		// FVaCuusModelArrayDesc rule, spec 2(c)).
		for (const FVaCuusModelField& Field : Entry.Model->GetLayout().GetFields())
		{
			if (Field.Kind != EVaCuusFieldKind::Array)
			{
				continue;
			}

			const void* ArrayValuePtr = Field.Property->ContainerPtrToValuePtr<void>(Field.ContainerPtr(Base));
			FScriptArrayHelper Helper(Field.ArrayDesc->ArrayProperty, ArrayValuePtr);
			const int32 Num = Helper.Num();
			if (Num == 0)
			{
				continue;
			}

			// Stride is exactly the inner's element size -- tail padding baked in for
			// struct elements (FVaCuusModelArrayDesc::Inner's comment, PropertyStruct.cpp:114).
			const int32 Stride = Field.ArrayDesc->Inner->GetElementSize();
			const uint8* Start = Helper.GetRawPtr(0);
			if (Ptr < Start || Ptr >= Start + static_cast<size_t>(Num) * Stride)
			{
				continue;
			}

			const int32 Index = static_cast<int32>((Ptr - Start) / Stride);

			// Two leaf shapes arrive here, told apart by the definition's own path: a
			// SCALAR element definition is per array field and spells "FHud.Numbers[]"
			// (the index is the span's to supply), while a struct-row leaf belongs to the
			// ROW type's shared set and spells "FKillRow.Killer" -- a row-local tail the
			// wire path appends after the field and index.
			const FString Tail = TailAfterTypeRoot(DiagnosticPath);
			Out.Entry = &Entry;
			Out.WirePath = FString::Printf(TEXT("%s[%d]"), *Field.WireName, Index);
			if (!Tail.EndsWith(TEXT("[]")))
			{
				Out.WirePath += TEXT(".") + Tail;
			}
			Out.TopLevelName = Entry.Model->GetLayout().GetTopLevelNames()[Field.TopLevelNameIndex];
			return true;
		}
	}

	return false;
}

/** The producer half of the bound. UI-frame thread only (the queue comment). */
bool EnqueueItem(FVaCuusRoutedItem&& Item)
{
	if (GQueueCount.load(std::memory_order_relaxed) >= FVaCuusWriteRouter::QueueCapacity)
	{
		GNumDroppedItems.fetch_add(1, std::memory_order_relaxed);
		if (!GDropLogged)
		{
			GDropLogged = true;
			UE_LOG(LogVaCuus, Warning,
				TEXT("VaCuus router: the game-thread queue is full (%d items) and view %u's %s was dropped. The game thread ")
				TEXT("has stopped draining -- is UVaCuusSubsystem ticking? Reported once per stall; see GetNumDroppedItems()"),
				FVaCuusWriteRouter::QueueCapacity, Item.ViewId,
				Item.Kind == FVaCuusRoutedItem::EKind::ModelWrite ? TEXT("model write") : TEXT("event"));
		}
		return false;
	}

	GDropLogged = false;
	GQueue.Enqueue(MoveTemp(Item));
	GQueueCount.fetch_add(1, std::memory_order_release);
	return true;
}

/** The read surface's per-miss Warning, once per (model, path) -- the definition-latch rule, keyed because this state is not per definition. */
void WarnReadMissOnce(uint32 ViewId, FName ModelName, const FString& Path, const TCHAR* Reason)
{
	const FString Key = FString::Printf(TEXT("%u|%s|%s"), ViewId, *ModelName.ToString(), *Path);
	bool bAlreadyLogged = false;
	GReadMissLogged.Add(Key, &bAlreadyLogged);
	if (!bAlreadyLogged)
	{
		UE_LOG(LogVaCuus, Warning,
			TEXT("VaCuus read surface: view %u, model '%s', path '%s': %s; it reads as null. Reported once per (model, path)"),
			ViewId, *ModelName.ToString(), *Path, Reason);
	}
}

/**
 * One leaf, typed: the same FProperty accessor per kind as FVaCuusScalarDefinition::Get
 * (whose comments carry each choice's argument -- mask-aware bools, authored enum
 * names, display-string FText, soft-only object paths), producing the tagged value
 * instead of an Rml::Variant, because this runs for a JS caller and must not build
 * RmlUi types. No `default`: -Wswitch turns a new kind into a compile error here, the
 * house shape.
 */
bool ReadScalarValue(EVaCuusFieldKind Kind, const FProperty* Property, const void* ValuePtr, FVaCuusJsValue& OutValue)
{
	switch (Kind)
	{
		case EVaCuusFieldKind::Bool:
			OutValue = FVaCuusJsValue::MakeBool(CastFieldChecked<FBoolProperty>(Property)->GetPropertyValue(ValuePtr));
			return true;

		case EVaCuusFieldKind::SignedInt:
			// A double, because that is the only number JS has; magnitudes past 2^53
			// round, exactly as they would crossing into any JS engine.
			OutValue = FVaCuusJsValue::MakeNumber(
				static_cast<double>(CastFieldChecked<FNumericProperty>(Property)->GetSignedIntPropertyValue(ValuePtr)));
			return true;

		case EVaCuusFieldKind::UnsignedInt:
			OutValue = FVaCuusJsValue::MakeNumber(
				static_cast<double>(CastFieldChecked<FNumericProperty>(Property)->GetUnsignedIntPropertyValue(ValuePtr)));
			return true;

		case EVaCuusFieldKind::FloatingPoint:
			OutValue =
				FVaCuusJsValue::MakeNumber(CastFieldChecked<FNumericProperty>(Property)->GetFloatingPointPropertyValue(ValuePtr));
			return true;

		case EVaCuusFieldKind::String:
			OutValue = FVaCuusJsValue::MakeString(CastFieldChecked<FStrProperty>(Property)->GetPropertyValue(ValuePtr));
			return true;

		case EVaCuusFieldKind::Utf8String:
			OutValue = FVaCuusJsValue::MakeString(FString(CastFieldChecked<FUtf8StrProperty>(Property)->GetPropertyValue(ValuePtr)));
			return true;

		case EVaCuusFieldKind::AnsiString:
			OutValue = FVaCuusJsValue::MakeString(FString(CastFieldChecked<FAnsiStrProperty>(Property)->GetPropertyValue(ValuePtr)));
			return true;

		case EVaCuusFieldKind::Name:
			OutValue = FVaCuusJsValue::MakeString(CastFieldChecked<FNameProperty>(Property)->GetPropertyValue(ValuePtr).ToString());
			return true;

		case EVaCuusFieldKind::Text:
			OutValue = FVaCuusJsValue::MakeString(CastFieldChecked<FTextProperty>(Property)->GetPropertyValue(ValuePtr).ToString());
			return true;

		case EVaCuusFieldKind::Enum:
		{
			// The FEnumProperty / TEnumAsByte duality, the adapter's shape (FVaCuusScalarDefinition::GetEnumName).
			const UEnum* Enum = nullptr;
			int64 Value = 0;
			if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
			{
				Enum = EnumProperty->GetEnum();
				Value = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
			}
			else
			{
				const FNumericProperty* Numeric = CastFieldChecked<FNumericProperty>(Property);
				Enum = Numeric->GetIntPropertyEnum();
				Value = static_cast<int64>(Numeric->GetUnsignedIntPropertyValue(ValuePtr));
			}

			FString Name;
			if (Enum == nullptr || !Enum->FindAuthoredNameStringByValue(Name, Value))
			{
				return false;
			}
			OutValue = FVaCuusJsValue::MakeString(MoveTemp(Name));
			return true;
		}

		case EVaCuusFieldKind::ObjectPath:
			// Soft only, threading not taste -- the layout refuses weak references, and
			// ToString() is path arithmetic with no GUObjectArray read (SoftObjectPtr.h:96-105).
			if (const FSoftObjectProperty* SoftProperty = CastField<FSoftObjectProperty>(Property))
			{
				OutValue = FVaCuusJsValue::MakeString(SoftProperty->GetPropertyValue(ValuePtr).ToString());
				return true;
			}
			return false;

		case EVaCuusFieldKind::Array:
			// Unreachable by shape: ReadModelValue dispatches Array fields to the
			// size/index branches before any leaf read.
			checkNoEntry();
			return false;
	}

	checkNoEntry();
	return false;
}
}	 // namespace

// ---------------------------------------------------------------------------------------
// UI thread
// ---------------------------------------------------------------------------------------

void FVaCuusWriteRouter::RegisterRouter()
{
	check(FVaCuusUIThread::IsInUIThread());
	GRouterRegistered = true;
}

void FVaCuusWriteRouter::UnregisterRouter()
{
	check(FVaCuusUIThread::IsInUIThread());
	GRouterRegistered = false;
	GRegisteredModels.Empty();
	GPendingReverts.Empty();
	GReadMissLogged.Empty();
	// The QUEUE is deliberately not touched: its consumer is the game thread, and a
	// UI-thread clear would be a second consumer (TSpscQueue's one forbidden shape).
	// Whatever is still queued drains normally and drops at Verbose per stale view.
}

bool FVaCuusWriteRouter::IsRouterRegistered()
{
	check(FVaCuusUIThread::IsInUIThread());
	return GRouterRegistered;
}

void FVaCuusWriteRouter::RegisterModel(uint32 ViewId, FName ModelName, const TSharedRef<FVaCuusBoundModel>& Model)
{
	check(FVaCuusUIThread::IsInUIThread());
	GRegisteredModels.Add(FRegisteredModel{ViewId, ModelName, &Model.Get()});
}

void FVaCuusWriteRouter::UnregisterViewModels(uint32 ViewId)
{
	check(FVaCuusUIThread::IsInUIThread());

	for (int32 Index = GRegisteredModels.Num() - 1; Index >= 0; --Index)
	{
		if (GRegisteredModels[Index].ViewId != ViewId)
		{
			continue;
		}

		// Its pending reverts die with it: a DirtyVariable on a context mid-removal
		// would be a call into a model whose context Shutdown() is about to destroy.
		FVaCuusBoundModel* Model = GRegisteredModels[Index].Model;
		GPendingReverts.RemoveAll([Model](const TPair<FVaCuusBoundModel*, FString>& Pair) { return Pair.Key == Model; });
		GRegisteredModels.RemoveAt(Index);
	}
}

bool FVaCuusWriteRouter::TryRouteScalarSet(const FString& DiagnosticPath, const void* InValuePtr, const Rml::Variant& Variant,
	TFunctionRef<bool(Rml::Variant&)> GetCurrentValue)
{
	check(FVaCuusUIThread::IsInUIThread());

	if (!GRouterRegistered || InValuePtr == nullptr)
	{
		return false;
	}

	FAttribution Attribution;
	if (!AttributeWrite(InValuePtr, DiagnosticPath, Attribution))
	{
		// Not a registered model's storage -- a direct-bound test model, or a stray.
		// The caller refuses with M3's wording verbatim, which is exactly what keeps
		// the M3a/M3b suites' expected messages true in this binary.
		return false;
	}

	// THE ECHO RULE (the class comment carries the observed red that forced it): a
	// write asking for the value the shadow already holds is the binding echoing --
	// the data-checked view rewriting the control from the model re-fires the change
	// controller (InputTypeCheckbox.cpp:22-37 dispatches for programmatic writes too)
	// on every revert AND every game-driven apply. Compared in RmlUi's own string
	// projection so a bool's echo and a text input's echo read alike; swallowed with
	// no queue entry and NO revert -- control and shadow already agree.
	Rml::Variant CurrentValue;
	if (GetCurrentValue(CurrentValue))
	{
		Rml::String IncomingText;
		Rml::String CurrentText;
		if (Variant.GetInto(IncomingText) && CurrentValue.GetInto(CurrentText) && IncomingText == CurrentText)
		{
			GNumSwallowedEchoes.fetch_add(1, std::memory_order_relaxed);
			UE_LOG(LogVaCuus, Verbose,
				TEXT("VaCuus model: swallowed an echo write to '%s' (view %u, model '%s', path '%s'): the value already ")
				TEXT("equals the shadow's"),
				*DiagnosticPath, Attribution.Entry->ViewId, *Attribution.Entry->ModelName.ToString(), *Attribution.WirePath);
			return true;
		}
	}

	GNumRoutedWrites.fetch_add(1, std::memory_order_relaxed);

	FVaCuusRoutedItem Item;
	Item.Kind = FVaCuusRoutedItem::EKind::ModelWrite;
	Item.ViewId = Attribution.Entry->ViewId;
	Item.Name = Attribution.Entry->ModelName;
	Item.Path = Attribution.WirePath;
	Item.Value = CoerceVariant(Variant);

	// The Verbose line that REPLACES the refusal Warning on this channel (spec 3.10),
	// logged before the enqueue so a full queue still leaves the attribution on record
	// (the drop then logs its own Warning right after).
	UE_LOG(LogVaCuus, Verbose, TEXT("VaCuus model: routed a document write to '%s' -> view %u, model '%s', path '%s'"),
		*DiagnosticPath, Item.ViewId, *Item.Name.ToString(), *Item.Path);

	EnqueueItem(MoveTemp(Item));

	// THE REVERT-DIRTY, queued even when the queue was full: RmlUi's default action
	// mutated the CONTROL before anybody was asked (the class comment), so the control
	// must re-run from the shadow whether or not the game heard about the write.
	// RESTORE-THE-BUG, run during Task 9: with this line disabled, the router test's
	// DOM probe found the checkbox's `checked` attribute still set one frame after the
	// click, against a byte-identical shadow -- v1's 12.6 divergence -- and the drain
	// then delivered ONE write instead of two, because the re-click's toggle-off was
	// value-equal to the shadow and swallowed as an echo.
	GPendingReverts.AddUnique(TPair<FVaCuusBoundModel*, FString>(Attribution.Entry->Model, Attribution.TopLevelName));

	// True either way: the write was attributed and handled by this channel. Falling
	// back to the refusal path on a full queue would log "one-way binding" about a
	// write that is two-way bound -- a lie with a better-sounding failure mode.
	return true;
}

void FVaCuusWriteRouter::FlushPendingReverts()
{
	check(FVaCuusUIThread::IsInUIThread());

	for (const TPair<FVaCuusBoundModel*, FString>& Pair : GPendingReverts)
	{
		Pair.Key->DirtyTopLevelFromShadow(Pair.Value);
	}
	GPendingReverts.Reset();
}

uint64 FVaCuusWriteRouter::GetNumRoutedWrites()
{
	return GNumRoutedWrites.load(std::memory_order_relaxed);
}

uint64 FVaCuusWriteRouter::GetNumSwallowedEchoes()
{
	return GNumSwallowedEchoes.load(std::memory_order_relaxed);
}

uint64 FVaCuusWriteRouter::GetNumDroppedItems()
{
	return GNumDroppedItems.load(std::memory_order_relaxed);
}

bool FVaCuusWriteRouter::EnqueueJsEvent(uint32 ViewId, FName EventName, TArray<FVaCuusJsKeyValue>&& Payload)
{
	// Refused, not asserted, off the UI thread: the VaCuusGameBridge header carries the
	// library-test argument. On-thread this is the same producer as every routed write.
	if (!FVaCuusUIThread::IsInUIThread())
	{
		UE_LOG(LogVaCuus, Warning, TEXT("vacuus.emit('%s') outside the UI thread (no UI thread is live?); dropped"),
			*EventName.ToString());
		return false;
	}

	FVaCuusRoutedItem Item;
	Item.Kind = FVaCuusRoutedItem::EKind::JsEvent;
	Item.ViewId = ViewId;
	Item.Name = EventName;
	Item.Payload = MoveTemp(Payload);
	return EnqueueItem(MoveTemp(Item));
}

bool FVaCuusWriteRouter::ReadModelValue(uint32 ViewId, FName ModelName, const FString& Path, FVaCuusJsValue& OutValue)
{
	if (!FVaCuusUIThread::IsInUIThread())
	{
		UE_LOG(LogVaCuus, Warning,
			TEXT("vacuus.model('%s').get('%s') outside the UI thread (no UI thread is live?); it reads as null"),
			*ModelName.ToString(), *Path);
		return false;
	}

	// The model, by (view, name), over the same registry attribution uses -- one
	// source of truth for "what JS can see", which is exactly the production-bound set.
	const FRegisteredModel* Entry = nullptr;
	for (const FRegisteredModel& Candidate : GRegisteredModels)
	{
		if (Candidate.ViewId == ViewId && Candidate.ModelName == ModelName)
		{
			Entry = &Candidate;
			break;
		}
	}
	if (Entry == nullptr)
	{
		WarnReadMissOnce(ViewId, ModelName, Path, TEXT("no model of that name is bound to this view"));
		return false;
	}

	const FVaCuusModelLayout& Layout = Entry->Model->GetLayout();
	const FVaCuusModelShadow& Shadow = Entry->Model->GetUIShadow();
	const void* Base = Shadow.GetData();
	if (Base == nullptr)
	{
		WarnReadMissOnce(ViewId, ModelName, Path, TEXT("the model has no shadow buffer"));
		return false;
	}

	int32 BracketIndex = INDEX_NONE;
	if (!Path.FindChar(TEXT('['), BracketIndex))
	{
		// EXACT WireName MATCH FIRST (the VaCuusGameBridge grammar): a nested member
		// legally named Size stays reachable; only when nothing is bound under the
		// spelling does ".size" read as the RmlUi length convention.
		if (const FVaCuusModelField* Field = Layout.FindField(Path))
		{
			if (Field->Kind == EVaCuusFieldKind::Array)
			{
				WarnReadMissOnce(ViewId, ModelName, Path,
					TEXT("a bare array has no value; read its length as '<Arr>.size' and elements as '<Arr>[i]'"));
				return false;
			}
			const void* ValuePtr = Field->Property->ContainerPtrToValuePtr<void>(Field->ContainerPtr(Base));
			if (ReadScalarValue(Field->Kind, Field->Property, ValuePtr, OutValue))
			{
				return true;
			}
			WarnReadMissOnce(ViewId, ModelName, Path, TEXT("the value could not be read (out-of-range enum?)"));
			return false;
		}

		if (Path.EndsWith(TEXT(".size")))
		{
			const FString ArrayPath = Path.LeftChop(5);
			const FVaCuusModelField* Field = Layout.FindField(ArrayPath);
			if (Field != nullptr && Field->Kind == EVaCuusFieldKind::Array)
			{
				const void* ArrayValuePtr = Field->Property->ContainerPtrToValuePtr<void>(Field->ContainerPtr(Base));
				OutValue = FVaCuusJsValue::MakeNumber(
					static_cast<double>(FScriptArrayHelper(Field->ArrayDesc->ArrayProperty, ArrayValuePtr).Num()));
				return true;
			}
		}

		WarnReadMissOnce(ViewId, ModelName, Path, TEXT("no bound field has that path"));
		return false;
	}

	// "Arr[i]" / "Arr[i].Rest": the bracket splits array path, index and row tail.
	const FString ArrayPath = Path.Left(BracketIndex);
	int32 CloseIndex = Path.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, BracketIndex);
	if (CloseIndex == INDEX_NONE)
	{
		WarnReadMissOnce(ViewId, ModelName, Path, TEXT("the index bracket never closes"));
		return false;
	}

	const FString IndexText = Path.Mid(BracketIndex + 1, CloseIndex - BracketIndex - 1);
	// Digits only, not IsNumeric(): that accepts "1.5" and "-2", and Atoi would then truncate
	// "1.5" to element 1 -- a silent wrong read where the contract promises a named miss.
	// Negative indices fall through to the same Warning instead of the out-of-bounds one.
	const bool bDigitsOnly = !IndexText.IsEmpty() &&
		Algo::AllOf(IndexText, [](TCHAR C) { return FChar::IsDigit(C); });
	if (!bDigitsOnly)
	{
		WarnReadMissOnce(ViewId, ModelName, Path, TEXT("the index is not a non-negative integer"));
		return false;
	}
	const int32 Index = FCString::Atoi(*IndexText);

	FString Rest = Path.Mid(CloseIndex + 1);
	if (Rest.StartsWith(TEXT(".")))
	{
		Rest.RightChopInline(1);
	}
	else if (!Rest.IsEmpty())
	{
		WarnReadMissOnce(ViewId, ModelName, Path, TEXT("only '.member' may follow an index"));
		return false;
	}

	const FVaCuusModelField* Field = Layout.FindField(ArrayPath);
	if (Field == nullptr || Field->Kind != EVaCuusFieldKind::Array)
	{
		WarnReadMissOnce(ViewId, ModelName, Path, TEXT("no bound array has that path"));
		return false;
	}

	const void* ArrayValuePtr = Field->Property->ContainerPtrToValuePtr<void>(Field->ContainerPtr(Base));
	FScriptArrayHelper Helper(Field->ArrayDesc->ArrayProperty, ArrayValuePtr);
	if (!Helper.IsValidIndex(Index))
	{
		WarnReadMissOnce(ViewId, ModelName, Path, TEXT("the index is out of bounds"));
		return false;
	}

	// GetRawPtr's result lives for exactly this read -- the M3b rule, spec 2(c).
	const void* ElementPtr = Helper.GetRawPtr(Index);

	if (Field->ArrayDesc->IsStructElement())
	{
		const FVaCuusModelField* ElementField = Rest.IsEmpty() ? nullptr : Field->ArrayDesc->ElementLayout->FindField(Rest);
		if (ElementField == nullptr || ElementField->Kind == EVaCuusFieldKind::Array)
		{
			WarnReadMissOnce(ViewId, ModelName, Path, TEXT("a struct row reads by member, '<Arr>[i].<Member>'"));
			return false;
		}

		// GetRawPtr(i) IS an instance base of the row type (the one addressing
		// invariant, VaCuusDataVariable.h) -- the element field addresses from it
		// exactly as a model field addresses from the shadow base.
		const void* ValuePtr = ElementField->Property->ContainerPtrToValuePtr<void>(ElementField->ContainerPtr(ElementPtr));
		if (ReadScalarValue(ElementField->Kind, ElementField->Property, ValuePtr, OutValue))
		{
			return true;
		}
		WarnReadMissOnce(ViewId, ModelName, Path, TEXT("the value could not be read (out-of-range enum?)"));
		return false;
	}

	if (!Rest.IsEmpty())
	{
		WarnReadMissOnce(ViewId, ModelName, Path, TEXT("a scalar element has no members"));
		return false;
	}

	// A scalar element's value pointer is a container pointer for nothing
	// (FVaCuusModelArrayDesc::Inner's comment): it feeds the accessor directly.
	if (ReadScalarValue(Field->ArrayDesc->ElementKind, Field->ArrayDesc->Inner, ElementPtr, OutValue))
	{
		return true;
	}
	WarnReadMissOnce(ViewId, ModelName, Path, TEXT("the value could not be read (out-of-range enum?)"));
	return false;
}

// ---------------------------------------------------------------------------------------
// Game thread
// ---------------------------------------------------------------------------------------

void FVaCuusWriteRouter::RegisterGameView(uint32 ViewId, UVaCuusView* View)
{
	check(IsInGameThread());
	GGameViews.Add(ViewId, View);
}

void FVaCuusWriteRouter::UnregisterGameView(uint32 ViewId)
{
	check(IsInGameThread());
	GGameViews.Remove(ViewId);

	// The zero-handler latch resets when the LAST view leaves -- the game-side
	// equivalent of the UI-side latches dying with the UI thread's registries, and what
	// keeps the Warning per (model, path) meaningful per session rather than
	// once-per-process-forever. Same thread as every other reader of the set.
	if (GGameViews.IsEmpty())
	{
		GZeroHandlerWarned.Empty();
	}
}

void FVaCuusWriteRouter::DrainGameThread()
{
	check(IsInGameThread());

	while (TOptional<FVaCuusRoutedItem> Item = GQueue.Dequeue())
	{
		GQueueCount.fetch_sub(1, std::memory_order_relaxed);

		UVaCuusView* View = GGameViews.FindRef(Item->ViewId).Get();
		if (View == nullptr)
		{
			// Ordinary during teardown -- the view died between enqueue and drain.
			// Verbose for the same reason input's unknown-view drop is: this runs at
			// frame rate and a louder level would bury the log exactly then.
			UE_LOG(LogVaCuus, Verbose, TEXT("A routed item for view %u arrived after the view was gone; dropped"), Item->ViewId);
			continue;
		}

		if (Item->Kind == FVaCuusRoutedItem::EKind::ModelWrite)
		{
			if (!View->OnModelWrite.IsBound())
			{
				// The named refusal spec 6 lists: a two-way-bound control whose writes
				// go nowhere is a wiring bug that is otherwise perfectly silent -- the
				// control even LOOKS right, because the revert-dirty snapped it back.
				bool bAlreadyWarned = false;
				GZeroHandlerWarned.Add(TPair<FName, FString>(Item->Name, Item->Path), &bAlreadyWarned);
				if (!bAlreadyWarned)
				{
					UE_LOG(LogVaCuus, Warning,
						TEXT("View %u routed a write to model '%s' path '%s', but nothing is bound to OnModelWrite; the write ")
						TEXT("goes nowhere and the control snaps back. Reported once per (model, path)"),
						Item->ViewId, *Item->Name.ToString(), *Item->Path);
				}
			}
			View->OnModelWrite.Broadcast(Item->Name, Item->Path, Item->Value);
		}
		else
		{
			View->OnJsEvent.Broadcast(Item->Name, Item->Payload);
		}
	}
}

// ---------------------------------------------------------------------------------------
// VaCuusGameBridge -- the public face (contracts in Public/VaCuusGameBridge.h)
// ---------------------------------------------------------------------------------------

bool VaCuusGameBridge::EnqueueJsEvent(uint32 ViewId, FName EventName, TArray<FVaCuusJsKeyValue>&& Payload)
{
	return FVaCuusWriteRouter::EnqueueJsEvent(ViewId, EventName, MoveTemp(Payload));
}

bool VaCuusGameBridge::ReadModelValue(uint32 ViewId, FName ModelName, const FString& Path, FVaCuusJsValue& OutValue)
{
	return FVaCuusWriteRouter::ReadModelValue(ViewId, ModelName, Path, OutValue);
}
