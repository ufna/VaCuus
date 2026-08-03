// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusModelLayout.h"
#include "VaCuusModelShadow.h"

#include "Containers/BitArray.h"
#include "Containers/TripleBuffer.h"
#include "Templates/Function.h"

#include <atomic>

/**
 * One published set of field values: what the game thread hands the UI thread.
 *
 * THE APPLIER IS DRIVEN BY DirtyFields, NEVER BY Values ALONE (spec 4). Only the fields
 * whose bit is set are current; every other field in Values is whatever an EARLIER publish
 * left in this slot, because TTripleBuffer recycles its three buffers and nothing ever
 * clears them. That is not a defect to work around -- it is what makes the publish cheap --
 * but it does mean a reader that walks the struct instead of the bits reads stale values
 * with no diagnostic.
 */
struct FVaCuusModelUpdate
{
	/**
	 * Strictly increasing per channel, stamped on every publish. 0 means "nothing has ever
	 * been published into this slot".
	 *
	 * ITS OWN COUNTER, NOT FVaCuusViewStatus::Generation -- there is no such member, and the
	 * Generation that does exist belongs to FVaCuusInteractiveSnapshot, which is published by
	 * the OTHER thread at a different rate (spec 3.5). It is needed for the same reason the
	 * snapshot's is: SwapReadBuffers is a no-op when nothing new was published
	 * (TripleBuffer.h:149-154) and hands the same buffer back, so freshness cannot be
	 * inferred from the read alone.
	 */
	uint64 Generation = 0;

	/** One bit per FVaCuusModelLayout field index; set means "Values holds this field's current value". */
	TBitArray<> DirtyFields;

	/** A real instance of the model type. Its non-dirty fields are meaningless -- see above. */
	FVaCuusModelShadow Values;
};

/**
 * The game thread -> UI thread channel for ONE bound model: latest-wins, allocation-free in
 * the steady state, and built so that a value can never arrive older than the bit that
 * announces it.
 *
 * "ALLOCATION-FREE" IS SCOPED (spec 3.4). For SCALAR fields it holds as written: bit sets,
 * slots and their shadow buffers are allocated up front and a publish only assigns through
 * them. An ARRAY field's publish is per-element ASSIGNMENT into the slot's existing elements
 * (FVaCuusModelArrayDesc::SyncCopy, reached through the same CopyValue call as every other
 * kind), which allocates only where content outgrew capacity or Num grew: an FString
 * element's assignment reallocates iff the quantized reserve of the source length exceeds
 * the destination's capacity, else the buffer is reused outright (ReallocForCopy,
 * Array.h:710-751 -- `NewMax > PrevMax` on both branches), and the container block grows the
 * same way under Resize, with the SHRINK caveat SyncCopy's comment carries
 * (VaCuusModelLayout.cpp:45-52: a large trim may move the block). So a same-shape republish
 * is assignment-shaped, not allocation-shaped -- a claim spec 9 checks with a counting
 * allocator rather than takes from this comment.
 *
 *
 * WHY THE OBVIOUS DESIGN IS WRONG (spec 3.5, and the blocking finding of the design review).
 *
 * The obvious design is "OR the dirty bits into the unpublished slot, and write a field's
 * value when it changes". It regresses values, and the reason is what TTripleBuffer recycles:
 * SwapWriteBuffers swaps the write index with the TEMP index (TripleBuffer.h:182-191, helper
 * at :255-259), so after two publishes with no consume in between the producer is handed back
 * the buffer it published FIRST -- still carrying that publish's bits and that publish's
 * values.
 *
 *     Health 100 -> 90 published into slot A; not consumed.
 *     Health  90 -> 80 published into slot B; consumed. The UI applies 80.
 *     Next frame the producer gets slot A back. It still has bit(Health) set and the value
 *     90. Health did not change this frame, so nothing overwrites it. The publish goes out
 *     with bit(Health) and 90, the UI applies 90 over 80, and holds it until Health next
 *     changes.
 *
 * The applier was faithful to the bits. The bit was true. Only the value was stale, and no
 * log line anywhere says so.
 *
 *
 * THE PROTOCOL THAT REPLACES IT.
 *
 * The invariant to hold is: EVERY PUBLISHED SLOT IS SELF-SUFFICIENT. It carries the complete
 * set of fields the UI has not yet confirmed applying, each with its value as of that
 * publish. It has to be self-sufficient because the consumer is allowed to skip publishes --
 * latest-wins is the point of the container -- so a slot that only describes "what changed
 * this frame" describes a delta against a state no reader is guaranteed to have reached.
 *
 * That is three pieces of game-thread state and one atomic coming back:
 *
 *  - Pending  -- fields the differ has marked since the last publish.
 *  - Unacked  -- fields published but not yet confirmed applied. A publish writes
 *                Pending | Unacked, so a field keeps being republished, WITH ITS CURRENT
 *                VALUE, until the UI says it got it.
 *  - AppliedGeneration -- the UI thread's echo, release/acquire, exactly the shape of the
 *                document loader's load-serial (VaCuusRmlDocumentHost.cpp:263 stores,
 *                VaCuusView.cpp:747 and :864 load). Unacked clears only when the echo is >= the
 *                generation of the LAST publish, never an earlier one: a publish that
 *                superseded it may carry values the older applied generation never saw.
 *
 * A field re-dirtied after its publish lands in Pending, and clearing Unacked cannot swallow
 * it -- which is the whole reason the two sets are separate rather than one set cleared on
 * echo.
 *
 * COST OF REPUBLISHING. In the steady state the echo arrives within a UI frame or two and
 * Unacked is empty or tiny. When the UI thread stalls, this republishes the same handful of
 * fields per frame instead of accumulating a backlog -- bounded by the number of distinct
 * changed fields, never by how long the stall lasted. For an Array field "a field" is
 * O(elements): every republish rewrites the whole array into the slot (SyncCopy, per
 * element), so a stalled UI costs element assignments per outstanding array per game frame.
 *
 *
 * WHAT IS DELIBERATELY ABSENT.
 *
 * There is no pointer to FVaCuusUIThread here, and that is the enforcement of "enqueue, never
 * Trigger()": a publish cannot wake the UI thread because it has nothing to wake it with. The
 * one per-frame pulse is UVaCuusSubsystem::Tick (VaCuusSubsystem.cpp:106, and the Trigger() is
 * at :167) and waking per model update would turn a changing HUD into one UI frame per changed
 * field.
 *
 * There is also no Write() call -- TTripleBuffer::Write takes `const BufferType` BY VALUE
 * (TripleBuffer.h:199), which for a payload owning a UScriptStruct instance is not merely
 * slow but impossible. Everything is built in place through GetWriteBuffer(), the same rule
 * the interactive snapshot already follows (VaCuusViewStatus.h:116-124).
 *
 *
 * THREADING. Single-producer/single-consumer, which is what TTripleBuffer states it is
 * thread-safe for (TripleBuffer.h:32). The producer is the game thread, asserted. The
 * consumer is whichever thread runs the UI frame -- the VaCuus UI thread normally, the game
 * thread in inline mode -- and is asserted to be ONE thread rather than to be a named one,
 * because that is the property the container actually requires and it holds in a test that
 * never boots a UI thread. Nothing here calls RmlUi.
 *
 * Neither copyable nor movable, because TTripleBuffer is neither (TripleBuffer.h:265-271
 * hides both). Owners hold it by pointer.
 */
class FVaCuusModelChannel
{
public:
	/**
	 * InLayout is BORROWED and must outlive this channel; the model object owns both.
	 *
	 * THE CHANNEL STARTS FULLY DIRTY, and that is spec 4's invariant I1 rather than a
	 * convenience. It makes the FIRST publish carry every field unconditionally, so the UI
	 * side's starting values are established by something the game thread published rather
	 * than by an argument that two independent UScriptStruct::InitializeStruct calls -- run on
	 * two threads at two times, on a type whose C++ constructor is free to be
	 * non-deterministic and which may be a UUserDefinedStruct rebuilt in between -- produced
	 * equal instances. Nothing on the game thread can check that argument, and if it is ever
	 * false the differ compares against a state the UI never had and the disagreement is
	 * permanent and silent.
	 *
	 * In the constructor rather than in a bFirstPublish flag so there is no way to construct a
	 * channel that starts empty.
	 */
	explicit FVaCuusModelChannel(const FVaCuusModelLayout& InLayout);

	FVaCuusModelChannel(const FVaCuusModelChannel&) = delete;
	FVaCuusModelChannel& operator=(const FVaCuusModelChannel&) = delete;

	/** False when the layout had no struct; nothing can be published through it. */
	bool IsValid() const { return Layout.IsValid(); }

	const FVaCuusModelLayout& GetLayout() const { return Layout; }

	//~ ------------------------------------------------------------------ Game thread

	/** Marks one field for the next publish. Index is into FVaCuusModelLayout::GetFields(). */
	void MarkFieldDirty(int32 FieldIndex);

	/** Marks every field. What the constructor does, and what a resync would do. */
	void MarkEveryFieldDirty();

	/**
	 * Publishes the current value of every field that is pending or unacknowledged, reading
	 * them out of Source.
	 *
	 * @param Source the game-side shadow, which must be an instance of the layout's type.
	 *        Typed rather than a void* because that is the one thing that can be checked: the
	 *        values written here are read on another thread through this layout's offsets, and
	 *        a wrong type would read arbitrary memory and crash on the first FString.
	 *        FVaCuusModelSampler::GetShadow() is the intended argument -- publishing straight
	 *        from the live gameplay struct would work by luck and would put a LOCALISED FText,
	 *        whose display string the UI thread would then have to resolve, into the slot.
	 *
	 * @return true if a slot was published. False means nothing was outstanding, which is the
	 *         idle case and must stay free: no swap, no generation bump, and therefore no
	 *         UI-thread work at all.
	 */
	bool Publish(const FVaCuusModelShadow& Source);

	/**
	 * Fields the UI has not confirmed applying: pending plus unacknowledged.
	 *
	 * NOT const, because it harvests the echo first. An observable that lagged the state it
	 * reports by one publish would say "15 fields outstanding" about a model the UI has
	 * finished applying, which is the sort of diagnostic that costs an afternoon.
	 */
	int32 NumOutstandingFields();

	/**
	 * The two game-side sets `vacuus.DumpModel` prints (spec 8), separately rather than as the
	 * union NumOutstandingFields() reports.
	 *
	 * THE DISTINCTION IS THE DIAGNOSTIC. Pending means "the differ saw this move and the UI has
	 * not been told"; Unacked means "the UI was told and has not confirmed". A model whose
	 * Pending set is empty while Unacked stays full for many frames is the signature of a bind
	 * that never reached a context -- FVaCuusBoundModel::ApplyPendingUpdate() deliberately does
	 * not consume in that case, so no echo ever comes back. Collapsed into one number those two
	 * states are indistinguishable, and they have completely different fixes.
	 *
	 * Const, and therefore NOT harvesting the echo first -- unlike NumOutstandingFields(). The
	 * dump calls that one immediately before these, so the sets it prints are already reaped.
	 * Game thread.
	 */
	const TBitArray<>& GetPendingFields() const { return Pending; }
	const TBitArray<>& GetUnackedFields() const { return Unacked; }

	/** Generation of the newest publish; 0 before the first. */
	uint64 GetLastPublishedGeneration() const { return LastPublishedGeneration; }

	/** Publishes made, and fields written across all of them. Observables for the tests and the dump command. */
	uint64 GetNumPublishes() const { return NumPublishes; }
	uint64 GetNumFieldsPublished() const { return NumFieldsPublished; }

	/**
	 * Slot buffers Publish() has lazily allocated (0..3). Game thread -- the producer is the
	 * only writer. Exists for one caller: the recompile timeout's leaked-bytes estimate
	 * (FVaCuusBoundModel::EstimateAbandonedBytes), which runs on the game thread and must not
	 * touch the slots themselves to count them.
	 */
	int32 GetNumSlotBuffersAllocated() const { return NumSlotBuffersAllocated; }

	//~ ------------------------------------------------------------------ Consumer thread

	/**
	 * Applies the newest published update, if there is one this consumer has not seen, and
	 * THEN echoes its generation back to the producer.
	 *
	 * THE ECHO IS NOT THE CALLER'S JOB, BY SHAPE. It is the half of the protocol whose
	 * omission is invisible: forget it and every publish keeps republishing every field
	 * forever, correctly, so nothing fails and nothing is logged -- only the cost grows. So
	 * the applier is passed in and the echo happens after it returns, on this thread, in
	 * program order, which is exactly "applied" and cannot be skipped.
	 *
	 * @return true if Applier ran.
	 */
	bool ConsumeUpdate(TFunctionRef<void(const FVaCuusModelUpdate&)> Applier);

	/**
	 * The slot this consumer read last -- the PUBLISHED dirty set spec 8's dump prints, next to
	 * the values that set announces.
	 *
	 * Read(), not SwapAndRead(): a swap here would consume a publish the real applier then never
	 * sees, so a dump would silently drop one frame's update. TripleBuffer.h:137 hands back the
	 * current read buffer and touches no index.
	 *
	 * CONSUMER THREAD ONLY, for the same reason ConsumeUpdate() is: the buffer this points at is
	 * the producer's again the moment this thread swaps. Meaningless (generation 0, no bits, no
	 * buffer) before the first successful ConsumeUpdate.
	 *
	 * Non-const because TTripleBuffer::Read() is (TripleBuffer.h:137) -- the same reason
	 * NumOutstandingFields() above is not const, and preferable to a const_cast that would hide
	 * a container contract behind a keyword.
	 */
	const FVaCuusModelUpdate& GetLastConsumedUpdate() { return Slots.Read(); }

	/** Newest generation this consumer took; 0 before the first. Consumer thread. */
	uint64 GetLastConsumedGeneration() const { return LastConsumedGeneration; }

	/** Updates this consumer has applied. Any thread. */
	uint64 GetNumUpdatesApplied() const { return NumUpdatesApplied.load(std::memory_order_relaxed); }

	/** The generation the consumer last echoed back; 0 before the first. Any thread. */
	uint64 GetAppliedGeneration() const { return AppliedGeneration.load(std::memory_order_acquire); }

	//~ ------------------------------------------------------------------ Recompile teardown

	/**
	 * Tears down all THREE slot buffers at once: Reset() (DestroyStruct) when
	 * bStructChainAlive, Abandon() (free, no destructors, contents leak) when not. The dirty
	 * bit sets are left alone -- they are plain bits and mean nothing once the buffers are
	 * empty. VaCuus-akj.16 / spec M6 2(j); FVaCuusBoundModel's drop-state machine is the only
	 * caller and says which of the two applies.
	 *
	 * LEGAL ONLY UNDER MUTUAL QUIESCENCE, WHICH THE CALLER'S PROTOCOL ESTABLISHES AND THIS
	 * CLASS CANNOT CHECK. TTripleBuffer gives the producer the write slot and the consumer the
	 * read slot; nothing may name all three unless BOTH roles are provably parked. They are,
	 * at every call site: the game-side producer set the model's dead flag BEFORE the teardown
	 * was queued (program order on the game thread; Publish is unreachable past the flag), and
	 * the caller runs either ON the consumer thread (the UI-side drop, inside DrainCommands,
	 * after the model left FVaCuusUIThread::Models -- so this frame's ApplyModelUpdates no
	 * longer reaches ConsumeUpdate for it) or after BOTH sides dropped their references
	 * (~FVaCuusBoundModel's safety net, where no other toucher can exist by definition).
	 */
	void TeardownSlotsForRecompile(bool bStructChainAlive);

private:
	/** Clears Unacked when the echo confirms the LAST publish. Game thread. */
	void ReapAcknowledgement();

	/** Borrowed; see the constructor. */
	const FVaCuusModelLayout& Layout;

	/**
	 * CALLER-OWNED SLOT STORAGE, handed to the buffer through its third constructor
	 * (TripleBuffer.h:103-108: takes `BufferType (&)[3]`, sets OwnsMemory=false). The default
	 * constructor is unusable anyway -- it does `Buffers[0] = Buffers[1] = Buffers[2] =
	 * BufferType()` (TripleBuffer.h:69-73), a COPY assignment FVaCuusModelUpdate cannot offer,
	 * since it owns a UScriptStruct instance and FVaCuusModelShadow is deliberately
	 * non-copyable -- but the previous form here was `Slots{NoInit}` (new BufferType[3],
	 * :76-79), and the switch to named storage is FOR TeardownSlotsForRecompile: the API hands
	 * out only the write slot (producer) and the read slot (consumer), so a teardown that must
	 * visit all THREE buffers has to own them by name. Same default-constructed start
	 * (generation 0, no bits, no buffer); each slot's buffer is still allocated the first time
	 * it becomes the write slot, i.e. at most three times ever.
	 *
	 * THE ONE BEHAVIOURAL DIFFERENCE, CHECKED AGAINST THE CONSUMER: this constructor starts
	 * with the Dirty flag SET (TripleBuffer.h:106: `Initial | Dirty`), so the first
	 * SwapAndRead swaps once with nothing published. Harmless by the generation gate:
	 * ConsumeUpdate reads generation 0, which is <= LastConsumedGeneration's initial 0, and
	 * applies nothing.
	 *
	 * DECLARED BEFORE Slots -- the buffer's constructor takes the array by reference.
	 */
	FVaCuusModelUpdate SlotStorage[3];
	TTripleBuffer<FVaCuusModelUpdate> Slots{SlotStorage};

	/** Marked by the differ since the last publish. Game thread only. */
	TBitArray<> Pending;

	/** Published, not yet confirmed applied. Game thread only. */
	TBitArray<> Unacked;

	/** Scratch for `Pending | Unacked`, kept as a member so a publish allocates nothing. */
	TBitArray<> PublishSet;

	/** Game thread only. */
	uint64 NextGeneration = 0;
	uint64 LastPublishedGeneration = 0;
	uint64 NumPublishes = 0;
	uint64 NumFieldsPublished = 0;

	/** Slot buffers Publish() has allocated so far (0..3). Game thread; see the getter. */
	int32 NumSlotBuffersAllocated = 0;

	/**
	 * The echo. Written by the consumer with release AFTER its apply, read by the producer
	 * with acquire -- the load-serial protocol (VaCuusRmlDocumentHost.cpp:263 stores,
	 * VaCuusView.cpp:584 and :662 load), and the release is what makes "the values are in the
	 * UI shadow" visible to the thread that decides whether to send them again.
	 */
	std::atomic<uint64> AppliedGeneration{0};

	/** Consumer only, but read from anywhere as an observable. */
	std::atomic<uint64> NumUpdatesApplied{0};

	/** Newest generation this consumer has taken. Consumer thread only. */
	uint64 LastConsumedGeneration = 0;

	/**
	 * The consumer thread, latched on the first ConsumeUpdate.
	 *
	 * NOT check(FVaCuusUIThread::IsInUIThread()), and the difference is deliberate: what
	 * TTripleBuffer requires is a single consumer THREAD (TripleBuffer.h:32), not a
	 * particular one -- and in inline mode the UI frame runs on the game thread, so the
	 * identity check would be asserting something weaker than what is needed while also
	 * making the channel untestable without booting a UI thread.
	 */
	uint32 ConsumerThreadId = 0;
};
