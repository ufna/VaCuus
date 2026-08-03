// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusModelChannel.h"

#include "VaCuusDefines.h"

#include "HAL/PlatformTLS.h"
#include "UObject/Class.h"

FVaCuusModelChannel::FVaCuusModelChannel(const FVaCuusModelLayout& InLayout)
	: Layout(InLayout)
{
	const int32 NumFields = Layout.GetFields().Num();

	// TRUE, and the header says why: this is invariant I1. The first publish therefore
	// carries every field whatever the differ concludes about it.
	Pending.Init(true, NumFields);
	Unacked.Init(false, NumFields);
	PublishSet.Init(false, NumFields);
}

void FVaCuusModelChannel::MarkFieldDirty(int32 FieldIndex)
{
	// The differ runs on the game thread because instance data has no engine
	// synchronisation at all (spec 6), and Pending is plain non-atomic state it shares with
	// Publish().
	check(IsInGameThread());
	check(Pending.IsValidIndex(FieldIndex));

	Pending[FieldIndex] = true;
}

void FVaCuusModelChannel::MarkEveryFieldDirty()
{
	check(IsInGameThread());

	// SetRange rather than Init: same result, and it cannot resize -- so a caller can never
	// silently change the bit count out from under Unacked and PublishSet.
	Pending.SetRange(0, Pending.Num(), true);
}

int32 FVaCuusModelChannel::NumOutstandingFields()
{
	check(IsInGameThread());

	ReapAcknowledgement();

	int32 Num = 0;
	for (int32 Index = 0; Index < Pending.Num(); ++Index)
	{
		Num += (Pending[Index] || Unacked[Index]) ? 1 : 0;
	}

	return Num;
}

void FVaCuusModelChannel::ReapAcknowledgement()
{
	check(IsInGameThread());

	if (!Unacked.Contains(true))
	{
		return;
	}

	// ACQUIRE, pairing with the consumer's release store after its apply. Without it the
	// producer could see the new generation while the shadow writes that preceded it are
	// still invisible -- and it would then stop republishing fields the UI has not actually
	// got. Same pairing as the load-serial (VaCuusView.cpp:584 and :662 load,
	// VaCuusRmlDocumentHost.cpp:263 stores).
	const uint64 Applied = AppliedGeneration.load(std::memory_order_acquire);

	// >= THE LAST PUBLISH, NOT >= THE GENERATION A BIT WAS FIRST SENT AT. Every publish
	// rewrites every outstanding field, so an echo older than the newest publish confirms an
	// older set of VALUES; clearing on it would drop a field whose value moved in between,
	// and the newest slot -- which the consumer may well be the one to read next -- would no
	// longer carry it. That is the same lost update this channel exists to prevent, arriving
	// by the other door.
	if (Applied >= LastPublishedGeneration)
	{
		Unacked.SetRange(0, Unacked.Num(), false);
	}
}

bool FVaCuusModelChannel::Publish(const FVaCuusModelShadow& Source)
{
	check(IsInGameThread());

	if (!IsValid())
	{
		return false;
	}

	// TYPE-CHECKED, NOT TRUSTED. Every offset used below came from Layout, and applying them
	// to an instance of a different type reads whatever happens to be at those bytes -- an
	// FString field would then be copied from a pointer that is not one. This is the only
	// place the pairing can be checked, so it is checked here rather than documented.
	if (!Source.IsValid() || Source.GetStruct() != Layout.GetStruct())
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("VaCuus model channel: publish source is '%s' but the layout is for '%s'; nothing was published"),
			Source.GetStruct() != nullptr ? *Source.GetStruct()->GetName() : TEXT("none"),
			Layout.GetStruct() != nullptr ? *Layout.GetStruct()->GetName() : TEXT("none"));
		return false;
	}

	ReapAcknowledgement();

	// THE PUBLISH SET IS PENDING **PLUS** UNACKED, which is the correction the whole class
	// exists for: the slot about to be written may be one published earlier and never
	// cleared, so "what changed this frame" is not enough to make it self-sufficient.
	PublishSet = Pending;
	PublishSet.CombineWithBitwiseOR(Unacked, EBitwiseOperatorFlags::MaintainSize);

	if (!PublishSet.Contains(true))
	{
		// THE IDLE CASE, AND IT MUST COST NOTHING: no swap, so the consumer's read swap stays
		// a no-op and no UI-thread work follows. Spec 9's "idle -> 0 published frames" row is
		// a correctness gate wearing a performance costume, and this is where it starts.
		return false;
	}

	FVaCuusModelUpdate& Slot = Slots.GetWriteBuffer();

	if (!Slot.Values.IsValid())
	{
		// At most three times in this channel's life -- once per buffer, the first time that
		// buffer becomes the write buffer. Still lazy although the slots are now named
		// members (SlotStorage): a channel that never publishes -- a model whose document
		// never binds -- must not pay three struct instances for it.
		Slot.Values = FVaCuusModelShadow(Layout.GetStruct());
		if (!Slot.Values.IsValid())
		{
			return false;
		}

		// Counted for exactly one reader: the recompile timeout's leaked-bytes estimate,
		// which must know how many buffers exist without touching them cross-thread.
		++NumSlotBuffersAllocated;
	}

	// ASSIGNED, NEVER OR'd. A recycled slot's old bits describe values from an earlier
	// publish that this one is not rewriting; carrying them forward would announce stale
	// values as current, which is exactly the failure mode above with the roles reversed.
	Slot.DirtyFields = PublishSet;

	const TConstArrayView<FVaCuusModelField> Fields = Layout.GetFields();
	int32 NumWritten = 0;
	for (TConstSetBitIterator<> It(PublishSet); It; ++It)
	{
		// CURRENT VALUES, read now, for every bit in the set -- including bits that were set
		// several publishes ago. This is the line that makes a published slot self-sufficient.
		Fields[It.GetIndex()].CopyValue(Slot.Values.GetData(), Source.GetData());
		++NumWritten;
	}

	const uint64 Generation = ++NextGeneration;
	Slot.Generation = Generation;

	// LAST, AND NOTHING TOUCHES `Slot` AFTER IT. SwapWriteBuffers is an
	// InterlockedCompareExchange (TripleBuffer.h:182-191), i.e. a full barrier, so everything
	// written above is visible to the consumer that observes the swap -- and by the same
	// token the buffer stops being ours at that instant, so the generation is read out of the
	// local rather than back out of the slot the consumer may already be reading.
	Slots.SwapWriteBuffers();

	LastPublishedGeneration = Generation;
	Unacked = PublishSet;
	Pending.SetRange(0, Pending.Num(), false);

	++NumPublishes;
	NumFieldsPublished += NumWritten;
	return true;
}

bool FVaCuusModelChannel::ConsumeUpdate(TFunctionRef<void(const FVaCuusModelUpdate&)> Applier)
{
	// ONE consumer thread, whichever it is -- see the member's comment for why this is not
	// an IsInUIThread() check.
	const uint32 ThisThreadId = FPlatformTLS::GetCurrentThreadId();
	if (ConsumerThreadId == 0)
	{
		ConsumerThreadId = ThisThreadId;
	}
	checkf(ConsumerThreadId == ThisThreadId,
		TEXT("VaCuus model channel: consumed from thread %u after thread %u; TTripleBuffer is single-consumer"), ThisThreadId,
		ConsumerThreadId);

	const FVaCuusModelUpdate& Update = Slots.SwapAndRead();

	// GENERATION, NOT THE SWAP. SwapReadBuffers does nothing when the producer published
	// nothing (TripleBuffer.h:151-154), so SwapAndRead hands the previous buffer back and a
	// caller that trusted the call would re-apply the same update forever -- costing a
	// DirtyVariable per field per UI frame on a model that has not changed, which is the idle
	// budget gone.
	if (Update.Generation <= LastConsumedGeneration)
	{
		return false;
	}

	LastConsumedGeneration = Update.Generation;
	Applier(Update);
	NumUpdatesApplied.fetch_add(1, std::memory_order_relaxed);

	// AFTER the apply, with release: the producer reads this with acquire and stops
	// republishing on the strength of it, so it must not become visible before the writes the
	// applier made. Written here rather than left to the caller because an omitted echo is
	// silent -- everything keeps working and only the cost grows.
	AppliedGeneration.store(Update.Generation, std::memory_order_release);
	return true;
}

void FVaCuusModelChannel::TeardownSlotsForRecompile(bool bStructChainAlive)
{
	// No thread assert CAN be written here -- the whole point is that this touches state two
	// different threads normally own, under the quiescence protocol the header spells out.
	// The state machine that establishes it is FVaCuusBoundModel's drop state; this function
	// is mechanism only.
	for (FVaCuusModelUpdate& Slot : SlotStorage)
	{
		if (bStructChainAlive)
		{
			// The normal DestroyStruct path: the old FProperty chain is still alive (the
			// fence held), so every FString/array the slot's instance owns is released.
			Slot.Values.Reset();
		}
		else
		{
			// The timeout path: the chain is (or may be) gone; free the buffer only. The
			// caller has already logged the leak with its estimate.
			Slot.Values.Abandon();
		}
	}
}
