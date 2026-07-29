// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include <atomic>

/** Outcome of a document load, as reported back by the UI thread. */
enum class EVaCuusLoadResult : uint8
{
	/** No load has completed yet. */
	None,
	Succeeded,
	Failed
};

/**
 * The one piece of state a view shares between the game thread and the UI
 * thread: "what happened to the work I asked for".
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
	 * Frames this view has recorded and published. Per-view rather than the UI
	 * thread's frame counter: a headless screenshot wants to know that *this*
	 * document made it to the render thread, not that the loop spun.
	 */
	std::atomic<uint64> FramesPublished{0};
};
