// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusJsValue.h"

/**
 * The `vacuus.*` host API's door into core (M4 Task 9, spec 3.11) -- what VaCuusJs's
 * thunks call. PUBLIC because the caller is another module; UI-THREAD-ONLY because
 * everything behind it is: the write router's registry and the models' UI shadows are
 * UI-thread state (spec 3.1/4), and production JS only ever runs there. A call from any
 * other thread is refused with a Warning rather than check()ed, deliberately -- the
 * VaCuusJs library-level tests legally drive whole JS contexts on the automation thread
 * with no UI thread anywhere (the Task 2 exclusivity contract), and a script they run
 * could reach these thunks; refusing keeps that configuration a no-op instead of a crash.
 *
 * Everything here is backed by FVaCuusWriteRouter (Private/VaCuusWriteRouter.h), which
 * also owns the game-thread half -- the bounded queue UVaCuusSubsystem::Tick drains into
 * UVaCuusView::OnModelWrite / OnJsEvent.
 */
namespace VaCuusGameBridge
{
/**
 * `vacuus.emit(name, payload)`: queues one game event for ViewId -- same bounded queue
 * as the write router (spec 3.10), no model. Surfaces on the game thread as
 * UVaCuusView::OnJsEvent.
 *
 * @return false when the item was dropped: queue full (the drop diagnostic has already
 *         been logged), or not on the UI thread. The JS thunk returns this to the
 *         script, so an overflow is observable where the emit happened.
 */
VACUUS_API bool EnqueueJsEvent(uint32 ViewId, FName EventName, TArray<FVaCuusJsKeyValue>&& Payload);

/**
 * `vacuus.model(name).get(path)`: reads one value out of the view's UI-thread shadow,
 * through the layout's per-kind accessors -- the same projection the bound scalar
 * definitions ship (spec 3.11: "same objects, no rework"). Never touches RmlUi and
 * never blocks: the shadow lives on the calling (UI) thread.
 *
 * THE PATH GRAMMAR, and it is the document's own address grammar plus one spelling:
 *   "Health"              -- a top-level leaf
 *   "Origin.X"            -- a flattened nested leaf (any depth the layout admits)
 *   "Killfeed[2].Killer"  -- a struct-array element's leaf
 *   "Numbers[0]"          -- a scalar-array element
 *   "Killfeed.size"       -- the element count, mirroring RmlUi's `{{Arr.size}}`
 * An exact layout WireName match wins before the `.size` reading, so a nested member
 * that happens to be named Size (legal -- reserved words bind top-level names only,
 * VaCuusModelLayout.h) stays reachable. A BARE array name has no value to give
 * ("arrays read as length + indexed access", spec 3.11) and misses with a Warning that
 * names the `.size` spelling.
 *
 * @return true with OutValue filled (typed: bool for bools, number for every numeric,
 *         string for strings/names/text/enums/soft paths). False -- which the JS thunk
 *         hands the script as `null` -- for an unknown model, an unknown path, an
 *         out-of-bounds index, or a wrong-thread call; each miss logs one Warning per
 *         (model, path).
 */
VACUUS_API bool ReadModelValue(uint32 ViewId, FName ModelName, const FString& Path, FVaCuusJsValue& OutValue);
}	 // namespace VaCuusGameBridge
