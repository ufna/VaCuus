// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusJsValue.h"

// PRIVATE HEADER, which is what makes the RmlUi include legal: VaCuus depends on
// VaCuusRml privately (VaCuus.Build.cs), so an RmlUi type may appear in this module's
// Private/ tree and nowhere else -- the VaCuusDataVariable.h rule.
#include <RmlUi/Core/Variant.h>

#include <atomic>

class FVaCuusBoundModel;
class UVaCuusView;

/**
 * THE WRITE ROUTER (M4 Task 9, spec 3.10): M3 I3's promissory note, discharged.
 *
 * M3 closed RmlUi's write path -- FVaCuusScalarDefinition::Set refuses every document
 * write, because a write into the UI shadow would leave the game-side differ comparing
 * against a state the UI never had (the GetNumRefusedSets() argument). This class is the
 * legal channel that replaces the scribble: a refused write is ATTRIBUTED to the bound
 * model whose storage it aimed at, marshalled into a bounded queue, and drained on the
 * game thread into UVaCuusView::OnModelWrite -- where the game decides. The shadow is
 * never written; Set still returns false; I3 stands.
 *
 *
 * ATTRIBUTION IS STORAGE OWNERSHIP, AND THAT IS THE WHOLE DESIGN. The definitions are
 * per-TYPE stateless statics shared by every model of that type across every view
 * (FVaCuusDefinitionRegistry), so nothing on the definition can say WHICH model a Set is
 * for -- and the spec's sketch of a definition->model back-pointer cannot exist without
 * breaking that statelessness. What a Set does carry is the VALUE POINTER, and a value
 * pointer names its owner exactly: every live allocation is disjoint, so a pointer into
 * a registered model's shadow buffer -- or into one of its arrays' element blocks -- can
 * belong to no other model. So the router keeps a registry of (ViewId, name, model),
 * maintained at the BindModel drain and view removal (both UI thread), and resolves a
 * write by span test: the shadow's inline span first, then each Array field's element
 * block through a per-call FScriptArrayHelper (the M3b rule: no stage stores an element
 * address). A handful of registered models and a walk that runs only on the WRITE path
 * -- user-interaction rate -- so the evaluation hot path is untouched.
 *
 * What this buys over the spec's sketch, concretely: array-ELEMENT writes attribute too.
 * A `data-event-click="kill.Killer = 'x'"` row alias resolves to a pointer inside the
 * Killfeed element block -- outside the shadow's inline span, unreachable by any
 * shadow-base equality test -- and the span walk still names model, index and leaf.
 *
 *
 * THE REVERT-DIRTY, spec 3.10's load-bearing half. RmlUi's default actions mutate the
 * CONTROL before the controller asks anybody: a checkbox toggles its `checked` attribute
 * inside ProcessDefaultAction (InputTypeCheckbox.cpp:43-46) and only then dispatches the
 * change event whose controller calls Set -- and a false Set skips DirtyVariable at both
 * call sites (DataControllerDefault.cpp:57-59, DataExpression.cpp:1190-1194), so nothing
 * ever re-evaluates the control and it stays visually toggled against an unchanged model
 * FOREVER (v1's recorded divergence, spec 12.6). So every routed write also queues its
 * top-level name here, and FVaCuusUIThread::ApplyModelUpdates flushes the queue --
 * DirtyVariable on the model handle, next apply, before that frame's Context::Update --
 * re-running the views from the authoritative shadow. The control snaps back, unless the
 * game heard OnModelWrite and actually changed the value, in which case the ordinary
 * publish/apply path carries the new truth through the same DirtyVariable.
 *
 *
 * THE ECHO RULE, learned from a red test rather than designed up front: an attributed
 * write whose value ALREADY EQUALS the shadow's is swallowed -- no route, no revert, a
 * counter and a Verbose line. It exists because RmlUi's checkbox dispatches `change`
 * for PROGRAMMATIC attribute writes exactly as for clicks (OnAttributeChange,
 * InputTypeCheckbox.cpp:22-37), and the data-checked VIEW writes that attribute from
 * the model on every evaluation (DataViewDefault.cpp:139-148) -- so the revert-dirty's
 * own re-run, and every GAME-DRIVEN apply that moves the field, re-fires the controller
 * and would route the model's own value straight back to the game: the observed red was
 * a third OnModelWrite (`bPaused = false`) delivered for a frame in which the only
 * actor was the revert. Equality is judged in RmlUi's own string projection
 * (Variant::GetInto<String>, the exact text a document renders), which makes a
 * checkbox's bool and a text input's string echo alike. THE COST, stated: a USER write
 * that requests the current value -- toggling a checkbox off before the game accepted
 * the on -- is indistinguishable from the binding's echo and is swallowed too; the
 * control still converges to the shadow, and the game merely never hears a no-op ask.
 *
 * REGISTRATION AND THE M3 GUARANTEE. The router as a whole is registered by
 * FVaCuusUIThread::Init() and retracted in Exit() -- a core-owned flag, same lifetime as
 * the definition registry. With it down (an M3 configuration: no M4 wiring ran), Set is
 * M3 byte-for-byte plus one false branch. With it UP, only writes that ATTRIBUTE are
 * routed: a model bound outside the production BindModel drain -- every M3a/M3b test
 * fixture binds its shadow directly through BindModelVariables -- is not in the registry,
 * its writes miss every span, and the refusal Warning fires with M3's wording verbatim.
 * The M3a/M3b suites passing untouched, expected refusal messages included, is that
 * proof running in this very binary.
 *
 *
 * THREADING. Three groups of state, each single-threaded, none shared:
 *  - UI thread: the model registry, the pending reverts, the read-surface latches, the
 *    routed counter's writer. Same plain-static shape as GNumRefusedSets, and the same
 *    enforcement: every entry asserts or refuses off-thread callers.
 *  - The queue: TSpscQueue, produced by whichever thread runs UI frames (the UI thread;
 *    the game thread in inline mode -- sequential, never concurrent, the
 *    FVaCuusModelChannel consumer argument one seat over), consumed by the game thread.
 *    The bound rides an atomic count beside it.
 *  - Game thread: the ViewId -> UVaCuusView map (registered by InitializeView, dropped
 *    by Invalidate) and the zero-handler warning latch.
 */
class FVaCuusWriteRouter
{
public:
	/**
	 * The queue bound (spec 3.10: "the input-ring bound + drop diagnostic pattern",
	 * arch spec 4). Sized for bursts, not backlog: the queue exists to cross one frame
	 * boundary -- UVaCuusSubsystem::Tick drains it every game frame -- so its steady
	 * depth is one frame's interactions plus one frame's emits. 256 is two orders of
	 * magnitude above that; hitting it means the game thread stopped draining, and the
	 * honest response to an unbounded stall is a named drop, not unbounded memory.
	 */
	static constexpr int32 QueueCapacity = 256;

	//~ ------------------------------------------------------------------ UI thread

	/** Init()'s registration; with it down every Set stays M3-verbatim. */
	static void RegisterRouter();

	/** Exit()'s retraction: drops the registry, the reverts and the latches. The queue is the game thread's to finish draining. */
	static void UnregisterRouter();

	/** True between the two calls above. UI-thread state, like the registry it gates. */
	static bool IsRouterRegistered();

	/** BindModel-drain registration: from here on, writes into Model's storage attribute to (ViewId, name). */
	static void RegisterModel(uint32 ViewId, FName ModelName, const TSharedRef<FVaCuusBoundModel>& Model);

	/** RemoveView's drop: the view's models leave the registry (and any pending reverts die with them). */
	static void UnregisterViewModels(uint32 ViewId);

	/**
	 * THE SEAM, called by FVaCuusScalarDefinition::Set and nowhere else. Attributes
	 * InValuePtr against the registry (the class comment's span walk); on a hit, coerces
	 * Variant to a tagged value (bool / number / string -- the wire's own three kinds,
	 * FVaCuusJsValue's comment), enqueues (ViewId, model, path, value) for the game
	 * thread, queues the revert-dirty, and logs the Verbose "routed" line that replaces
	 * M3's refusal Warning for this -- now legal -- channel.
	 *
	 * @param DiagnosticPath the definition's own path ("FPlayerHud.Origin.X", or the ROW
	 *        type's "FKillRow.Killer" for a struct-element leaf, or "FHud.Numbers[]" for
	 *        a scalar-element definition). The wire path is built from it plus the span
	 *        hit: the type-name root is replaced by what the span knows (spec 3.10) --
	 *        the model for a direct hit, "Arr[i]" for an element hit.
	 * @param GetCurrentValue reads the leaf's CURRENT shadow value (the caller's own Get
	 *        over the same value pointer) -- the echo rule's comparison source, asked
	 *        for only after attribution succeeded, so unattributed (M3-shaped) writes
	 *        never pay it and never change behavior.
	 * @return true when the write was attributed and handled here -- routed, or
	 *         swallowed as an echo -- and the caller then skips the refusal counter and
	 *         its Warning. False (unattributed) falls through to M3's refusal, verbatim.
	 */
	static bool TryRouteScalarSet(const FString& DiagnosticPath, const void* InValuePtr, const Rml::Variant& Variant,
		TFunctionRef<bool(Rml::Variant&)> GetCurrentValue);

	/**
	 * The revert-dirty flush: DirtyVariable(top-level name) on every model a routed
	 * write touched since the last flush. Called by FVaCuusUIThread::ApplyModelUpdates
	 * -- after the game updates applied, before this frame's Context::Update -- so the
	 * re-evaluation lands inside Update where spec 9 budgets it, and a control mutated
	 * by a click in THIS frame's DrainInput snaps back within the same frame.
	 */
	static void FlushPendingReverts();

	/** Routed (attributed + enqueued) writes since process start. Atomic: written UI-side, read by tests from the game thread. */
	static uint64 GetNumRoutedWrites();

	/** Attributed writes swallowed by the echo rule (the class comment). The rule's only observable, same pattern. */
	static uint64 GetNumSwallowedEchoes();

	/** Items dropped at the full queue, writes and emits alike. Same cross-thread pattern. */
	static uint64 GetNumDroppedItems();

	//~ The VaCuusGameBridge implementations (the public header carries the contracts).
	static bool EnqueueJsEvent(uint32 ViewId, FName EventName, TArray<FVaCuusJsKeyValue>&& Payload);
	static bool ReadModelValue(uint32 ViewId, FName ModelName, const FString& Path, FVaCuusJsValue& OutValue);

	//~ ------------------------------------------------------------------ Game thread

	/** UVaCuusView::InitializeView's registration -- what the drain dispatches through. */
	static void RegisterGameView(uint32 ViewId, UVaCuusView* View);

	/** Invalidate()'s drop. Idempotent; a queued item for a dropped view is discarded at Verbose. */
	static void UnregisterGameView(uint32 ViewId);

	/**
	 * Drains the queue into the registered views' OnModelWrite / OnJsEvent broadcasts.
	 * Called by UVaCuusSubsystem::Tick once per frame (several subsystems in multi-PIE
	 * all call it; the later calls find the queue empty -- the queue is process-wide
	 * because the UI thread is, and items route by ViewId exactly like UI commands do).
	 * A routed write reaching a view with NOTHING bound to OnModelWrite logs one Warning
	 * per (model, path): a two-way-bound control whose writes go nowhere is a wiring
	 * bug, and it is otherwise perfectly silent.
	 */
	static void DrainGameThread();
};
