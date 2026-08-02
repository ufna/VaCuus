// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusInteractiveSnapshot.h"

#include "Containers/TripleBuffer.h"

#include <atomic>

/** Outcome of a document load, as reported back by the UI thread. */
enum class EVaCuusLoadResult : uint8
{
	/** No load has completed yet. */
	None,
	Succeeded,
	Failed
};

/** Whether this view's document host ever came up on the UI thread. See FVaCuusViewStatus::BootState. */
enum class EVaCuusViewBootState : uint8
{
	/** The AddView command has not been drained yet. */
	Pending = 0,

	/** The host initialized and the view is registered on the UI thread. */
	Booted,

	/** The host's Initialize() failed (or the AddView carried no host); this view will never produce anything. */
	Failed
};

/**
 * The state a view shares between the game thread and the UI thread: "what
 * happened to the work I asked for", plus "where is this view interactive".
 *
 * WHY IT EXISTS: document loading is asynchronous now (the UI thread owns the
 * context, so it owns Context::LoadDocument too), but callers still need the
 * answer -- vacuus.M1HUD has to fall back to its inline document when the VFS
 * path fails to parse, and live reload (Task 10) will want the same signal.
 *
 * HOW IT REPORTS: the game thread stamps a strictly increasing serial into the
 * load command; the UI thread writes Result and *then* publishes
 * CompletedSerial with release ordering, so a reader that sees a new serial is
 * guaranteed to see the matching result. UVaCuusView polls this from
 * UVaCuusSubsystem::Tick and turns it into a game-thread delegate broadcast --
 * no locks, no marshalling, and no callback ever runs on the UI thread.
 *
 * ONE SLOT, ON PURPOSE: this holds the NEWEST result, not a queue of them, so
 * several loads finishing inside one UI-thread drain coalesce into a single
 * reported completion and superseded loads are never reported. The two serials
 * are what make that observable rather than invisible -- a caller compares its
 * request against CompletedSerial (UVaCuusView::IsLoadPending() and the two
 * GetLast*LoadSerial() accessors), and the coalescing itself is logged.
 *
 * WHY THE SNAPSHOT LIVES HERE TOO (controller decision D7): the interactive-region
 * snapshot is per-VIEW state, not per-thread. One UI thread serves N views, each
 * with its own context, its own layout and therefore its own interactive
 * geometry -- and each view's game-thread side (UVaCuusView, and the widget behind
 * it) only ever wants its own. This object is already the one channel both sides
 * of a view share, so the snapshot rides it rather than growing a second registry
 * keyed by view id on FVaCuusUIThread.
 *
 * Held by a thread-safe TSharedRef so the UI thread's copy stays valid even if
 * the UObject view is garbage-collected first.
 */
struct FVaCuusViewStatus
{
	/**
	 * Serial of the newest load the game thread has asked for. Written and read on
	 * the game thread only (atomic for uniformity, not for cross-thread ordering);
	 * paired with LoadCompletedSerial it answers "has my request been answered, or
	 * did a later one overtake it".
	 */
	std::atomic<uint64> LoadRequestSerial{0};

	/** Serial of the newest load the UI thread has finished. Published last (release). */
	std::atomic<uint64> LoadCompletedSerial{0};

	/** Result of the load identified by LoadCompletedSerial. Written before it. */
	std::atomic<uint8> LoadResult{static_cast<uint8>(EVaCuusLoadResult::None)};

	/**
	 * Did this view's host actually boot? (M6 sweep, bead VaCuus-akj.13.)
	 *
	 * WHY IT EXISTS: CreateView() constructs and returns the game-thread handle BEFORE the
	 * UI thread drains the AddView command (VaCuusSubsystem.cpp -- the enqueue-then-register
	 * order is the design, not a bug: nothing RmlUi-affine may run on the game thread). When
	 * the host's Initialize() then fails, the UI thread used to log one Error and drop the
	 * host -- with no channel back, so the handle stayed "valid" FOREVER: IsViewValid() true,
	 * every LoadDocument enqueued and dropped at Verbose for the unknown view, IsLoadPending()
	 * stuck true, OnLoadCompleted never firing. This field is that missing channel.
	 *
	 * PROTOCOL, same as FramesRecorded above: the UI thread stores with release ordering
	 * (Failed in AddView's failure branches, Booted right after the host registers), the game
	 * thread loads with acquire from UVaCuusView::PollStatus(), which on the FIRST Failed it
	 * observes logs one Error naming the view, invalidates the handle and broadcasts
	 * OnLoadCompleted(this, false) so load-waiters unblock. UVaCuusView::IsLoadPending()
	 * answers false once that admission has run (gated on the view's game-side latch, so the
	 * flip lands at the poll like every other observable there) -- the serial comparison
	 * alone would say "pending" forever, since no completion will ever be published for a
	 * dead view. Values are EVaCuusViewBootState.
	 */
	std::atomic<uint8> BootState{static_cast<uint8>(EVaCuusViewBootState::Pending)};

	/**
	 * Frames this view has RECORDED. Per-view rather than the UI thread's frame
	 * counter: a headless screenshot wants to know that *this* document was laid out
	 * and drawn, not that the loop spun.
	 *
	 * RECORDED, NOT PUBLISHED, and the distinction is real since M2 Task 12: the idle
	 * gate withholds the publish of a frame that draws what the render thread already
	 * has, so a static document records forever and publishes a handful of times. This
	 * counter deliberately follows the recording, because "has this view produced its
	 * content yet" is what every waiter here actually asks -- a screenshot after N
	 * recorded frames is correct whether or not frames 3..N were published, since a
	 * withheld frame means the pixels were already on the render thread. Counting
	 * publishes instead would stall a waiter on a static UI forever.
	 *
	 * The publish count is the sibling below.
	 */
	std::atomic<uint64> FramesRecorded{0};

	/**
	 * Frames this view has PUBLISHED, i.e. those the idle gate let through to the render
	 * thread. Always <= FramesRecorded, and the difference IS this view's idle signal:
	 * equal means the view changed on every frame, a gap that grows while FramesRecorded
	 * also grows means the gate is firing and the render target is carrying the picture.
	 *
	 * WHY IT IS HERE RATHER THAN ONLY ON THE RECORDER. The recorder has the number
	 * (FVaCuusRecordingRenderInterface::GetNumFramesPublished), but the recorder is owned by
	 * FVaCuusRmlDocumentHost in VaCuusRender/Private with no accessor on
	 * IVaCuusDocumentHost -- so before this counter existed the only possible reader was a
	 * unit test holding a recorder directly, and per-view publish/skip was not observable at
	 * runtime AT ALL. The one runtime readout, vacuus.M1HUD.PerfLog, aggregates over a
	 * file-static singleton and prints ONE process-wide published/skipped line, so with two
	 * views -- one genuinely idle, one wedged -- nothing said which contributed the skips.
	 * FramesRecorded advances for both.
	 *
	 * Atomic and published with release ordering for the same reason as FramesRecorded: the
	 * UI thread writes it, the game thread polls it, and no lock is worth taking for a
	 * counter.
	 */
	std::atomic<uint64> FramesPublished{0};

	/**
	 * Size of the newest PUBLISHED command buffer: every recorded command, and the subset
	 * of them the replayer turns into an RHI draw. Both are 0 until this view publishes.
	 *
	 * WHY PER VIEW, when vacuus.M1HUD.PerfLog already prints `draws/frame`. That figure is a
	 * process-wide ratio of two counters the render thread accumulates over every view's
	 * replays (VaCuusStats.cpp AddDraws), so with three stacked fullscreen views it answers
	 * "how much does the stack draw" and CANNOT answer "which layer is drawing it" -- which
	 * is the only question an overdraw audit asks. These two say, per view, what that view
	 * put on the screen the last time it had anything new to say.
	 *
	 * DRAWS ARE COUNTED FROM THE RECORD, NOT FROM THE REPLAY, so the number is available on
	 * the UI thread where the buffer is built and needs no channel back from the render
	 * thread. It counts the command types the replayer has a `++NumDrawCalls` for --
	 * DrawGeometry, DrawShader (VaCuusReplayRenderer.cpp:397, 445, 542) -- so it is exact
	 * except for a material DrawGeometry the render thread declines (`bDrawn` false at
	 * :443-448), which the recorder cannot know about. Layer composites are excluded for the
	 * same reason: the replayer does not count them either.
	 *
	 * NEWEST PUBLISHED, not newest recorded: on a withheld frame the render target still
	 * carries the last published buffer, so the published one is what is on screen. A view
	 * that has gone idle keeps reporting the picture it is actually showing.
	 *
	 * Written by the UI thread with release ordering, read by the game thread -- the
	 * FramesPublished protocol exactly, and for the same reason: no lock is worth taking
	 * for a counter.
	 */
	std::atomic<int32> LastPublishedCommands{0};
	std::atomic<int32> LastPublishedDrawCalls{0};

	//~ Interactive-region snapshot: UI thread produces, game thread consumes.
	//~ See FVaCuusInteractiveSnapshot for what it means and how stale it is.

	/**
	 * UI thread: the buffer to build the next snapshot into.
	 *
	 * Handed out instead of taking a finished snapshot by value because the three
	 * buffers keep their array capacity between publishes -- building in place is
	 * what makes a steady-state UI frame allocation-free. (TTripleBuffer::Write()
	 * would not do anyway: it takes `const BufferType` BY VALUE, TripleBuffer.h:199.)
	 *
	 * Valid until the matching PublishSnapshot(); do not hold it across frames.
	 */
	FVaCuusInteractiveSnapshot& GetSnapshotWriteBuffer() { return Snapshots.GetWriteBuffer(); }

	/** UI thread: makes the buffer above the newest one the game thread can read. */
	void PublishSnapshot() { Snapshots.SwapWriteBuffers(); }

	/**
	 * Game thread: the newest published snapshot.
	 *
	 * NEVER BLOCKS AND NEVER FAILS. If the UI thread published nothing since the
	 * last call, the swap is a no-op (TripleBuffer.h:151) and this returns the same
	 * buffer again -- which is why a caller must compare Generation rather than
	 * assume freshness. Before the first publish it returns a default-constructed
	 * snapshot: Generation 0, no rects, i.e. "nothing here is interactive", which is
	 * the correct answer for a view whose document has not laid out yet.
	 *
	 * LIFETIME: the returned reference is invalidated by the NEXT call to this
	 * method (the swap moves the read buffer). Nothing else invalidates it -- the UI
	 * thread cannot pull a buffer out from under a reader. Callers that keep the
	 * snapshot past their own statement must copy it; UVaCuusView does exactly that
	 * and is the intended consumer, so game-thread code should go through
	 * UVaCuusView::GetSnapshot() and leave this to it.
	 */
	const FVaCuusInteractiveSnapshot& AcquireSnapshot() { return Snapshots.SwapAndRead(); }

private:
	/**
	 * Lock-free SPSC publish-swap: one producer (the UI thread), one consumer (the
	 * game thread). Intermediate frames are dropped when the consumer is slower than
	 * the producer, which is the desired coalescing -- the game thread wants the
	 * newest geometry, never a backlog of old geometry.
	 */
	TTripleBuffer<FVaCuusInteractiveSnapshot> Snapshots;
};
