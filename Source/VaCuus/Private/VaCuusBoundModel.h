// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusModelChannel.h"
#include "VaCuusModelLayout.h"
#include "VaCuusModelSampler.h"
#include "VaCuusModelShadow.h"

// PRIVATE HEADER, and that is what makes the RmlUi includes legal here: VaCuus depends on
// VaCuusRml privately (VaCuus.Build.cs:27-33), so an RmlUi type may appear in this module's
// Private/ tree and nowhere else -- the same rule VaCuusDataVariable.h, VaCuusInputMap.h and
// VaCuusSystemInterface.h already follow.
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Types.h>

#include <atomic>

namespace Rml
{
class Context;
}

class UScriptStruct;

/**
 * Where one model is in the Blueprint-struct-recompile teardown (VaCuus-akj.16, spec M6
 * 2(j)). One atomic on the model, because the decision it carries -- "may the UI-side
 * buffers still be destroyed through the type, or must they be abandoned" -- is raced
 * between the game thread's fence timeout and the UI thread's drop command, and losing
 * that race the quiet way is heap corruption.
 *
 * Transitions (values are compared/exchanged as uint8 in the atomic):
 *
 *   Live -> DropQueued        game thread, CondemnForStructRecompile (inside PreChange).
 *   DropQueued -> Dropping    UI thread, the drop command, via CAS -- claims the right to
 *                             run DestroyStruct while the game thread provably waits.
 *   Dropping -> TornDown      UI thread, teardown finished.
 *   DropQueued -> AbandonRequired
 *                             game thread, fence timeout, via CAS -- the compile is about
 *                             to free the old chain, so nobody may DestroyStruct anymore.
 *   AbandonRequired -> TornDown
 *                             whoever tears down late (the drop command when it finally
 *                             drains, or ~FVaCuusBoundModel if it never does): Abandon().
 *
 * The two CASes on DropQueued are the whole safety argument: exactly one side wins, and a
 * game thread that LOSES (finds Dropping) spins until TornDown before returning from
 * PreChange -- returning earlier would let the compile delete the chain under a running
 * DestroyStruct.
 */
enum class EVaCuusModelDropState : uint8
{
	Live = 0,
	DropQueued,
	Dropping,
	TornDown,
	AbandonRequired,
};

/**
 * ONE BOUND MODEL -- the whole M3a pipeline for a single (view, model name, USTRUCT) triple,
 * and the one object both threads hold a reference to.
 *
 *
 * WHY THE FIVE PIECES LIVE IN ONE OBJECT. They are not independent: FVaCuusModelSampler and
 * FVaCuusModelChannel each hold a `const FVaCuusModelLayout&` that must outlive them, the
 * channel's bit indices are indices into that layout's field array, and the UI shadow must be
 * an instance of that layout's struct because RmlUi retains a raw void* into it and never
 * revalidates (spec 2(b)). Splitting them across two owners would put that lifetime chain
 * across a thread boundary. Declaration order below is therefore load-bearing.
 *
 *
 * WHO OWNS WHAT, AND WHEN.
 *
 *   Layout    -- immutable after construction; readable from both threads.
 *   Sampler   -- GAME thread. Holds the previous-value shadow the differ compares against.
 *   Channel   -- the seam. Single-producer (game thread) / single-consumer (whichever thread
 *                runs the UI frame), which is what TTripleBuffer is thread-safe for.
 *   UIShadow  -- UI thread, and RmlUi holds a raw pointer to its base for the life of the
 *                context.
 *   ModelHandle / TopLevelNamesUtf8 -- UI thread, valid only between BindToContext() and the
 *                context's destruction.
 *
 * The object is held by TSharedPtr from BOTH sides (UVaCuusView's map and FVaCuusUIThread's
 * per-view model list) rather than owned by one and borrowed by the other. That is what makes
 * teardown ordering a non-question: whichever side lets go last destroys the shadow, and by
 * then the context is already gone -- FVaCuusUIThread drops its reference in RemoveView(),
 * AFTER the host's Shutdown() has removed the Rml::Context that was pointing into it.
 *
 *
 * THE UI SHADOW HAS EXACTLY ONE WRITER, AND IT IS ApplyUpdate() (spec 4 / I3). Both other
 * doors are closed rather than documented:
 *
 *  - RmlUi's own write path -- data-value, data-checked and any
 *    `data-event-click="Health = 50"` assignment all reach VariableDefinition::Set -- is
 *    REFUSED by FVaCuusScalarDefinition::Set, which returns false and is counted by
 *    VaCuusData::GetNumRefusedSets(). Both RmlUi call sites skip their own DirtyVariable when
 *    Set returns false (DataControllerDefault.cpp:57-59, DataExpression.cpp:1185-1197).
 *  - VaCuus code cannot reach the buffer: UIShadow is private and there is NO non-const
 *    accessor for it anywhere in this class. A second writer cannot be added by accident,
 *    only by adding a member function -- which is the point.
 *
 * That matters because a write nobody published is invisible: the game-side differ compares
 * the live struct against its OWN shadow, sees no change, marks no bit, and the two shadows
 * disagree forever with a correct-looking screen and no log line.
 */
class FVaCuusBoundModel
{
public:
	/**
	 * Builds the layout, both shadows and the channel for InStruct. Diagnostics for anything
	 * skipped go to LogVaCuus during the layout walk.
	 *
	 * THE NAME IS AN FString, NOT AN FName, AND THE TYPE IS THE FIX FOR A PACKAGED-ONLY BUG
	 * (VaCuus-akj.23). RmlUi resolves `data-model` byte-for-byte: Element::SetParent hands the
	 * attribute's string to Context::GetDataModelPtr (Element.cpp:2212), which is a find() on
	 * `UnorderedMap<String, ...> data_models` (Context.h:383) -- robin_hood::unordered_flat_map
	 * keyed on std::string (Config.h:83; the vendored build defines no
	 * RMLUI_NO_THIRDPARTY_CONTAINERS), so 'hud' and 'HUD' are different models. An FName cannot carry
	 * that identity in a packaged game: WITH_CASE_PRESERVING_NAME follows WITH_EDITORONLY_DATA
	 * (NameTypes.h:32-33), so a cooked target's FName::ToString() returns the casing of the
	 * name's FIRST registration process-wide -- and the engine registers 'HUD' (class AHUD)
	 * before any plugin code runs, so FName("hud") stringified as 'HUD', the model was created
	 * under 'HUD', and the document's `data-model="hud"` matched nothing. Editor builds (even
	 * -game) preserve per-instance casing, which is why the bug never showed there.
	 *
	 * Any thread that may touch an already-linked UScriptStruct; in practice the game thread,
	 * from UVaCuusView::BindModel.
	 */
	FVaCuusBoundModel(const FString& InModelName, const UScriptStruct* InStruct);

	/**
	 * Trivial in every normal life. Non-trivial in exactly one: a model condemned for a
	 * struct recompile whose UI-side drop never ran (the command was refused or discarded by
	 * a stopping UI thread). Then the old FProperty chain is gone and the member destructors'
	 * DestroyStruct would corrupt the heap, so the buffers are Abandon()ed here first --
	 * DropState is the proof of which case this is.
	 */
	~FVaCuusBoundModel();

	//~ Neither copyable nor movable: FVaCuusModelChannel is neither (it holds a reference to
	//~ Layout, which is a member of this object), and RmlUi holds a raw pointer to UIShadow's
	//~ base. Owners hold this by TSharedPtr.
	FVaCuusBoundModel(const FVaCuusBoundModel&) = delete;
	FVaCuusBoundModel& operator=(const FVaCuusBoundModel&) = delete;

	/** False when the struct did not resolve or a shadow could not be allocated. */
	bool IsValid() const;

	/** The case-insensitive key: write-router registry, game-side map lookups, delegates. */
	FName GetModelName() const { return ModelName; }

	/** The case-EXACT name RmlUi knows this model by; the only form a log line should print. */
	const FString& GetModelNameString() const { return ModelNameStr; }

	const FVaCuusModelLayout& GetLayout() const { return Layout; }

	//~ ------------------------------------------------------------------ Game thread

	/**
	 * Diffs LiveData against the game-side shadow and marks whatever moved. Returns how many
	 * fields were marked.
	 *
	 * The type is a parameter for spec 7's reason: this is where the layout's offsets meet raw
	 * gameplay memory, so "wrong type" and "wrong size" are undiagnosable exactly here and the
	 * first FString field turns an undiagnosed mismatch into a crash. FVaCuusModelSampler
	 * re-checks it; the caller checks it first only so the log line can name the MODEL.
	 */
	int32 Sample(const UScriptStruct* LiveType, const void* LiveData);

	/**
	 * Publishes the current value of every outstanding field. Returns false when nothing was
	 * outstanding, which is the idle case and costs no swap and no UI-thread work.
	 *
	 * PUBLISHES FROM THE SAMPLER'S SHADOW, NEVER FROM THE LIVE STRUCT. Publishing from the
	 * live struct compiles and appears to work, and puts a LOCALISED FText -- one whose
	 * display string resolves against process-global localization state -- into a buffer the
	 * UI thread reads (FVaCuusModelSampler's header has the full argument). There is no way to
	 * express that mistake here, because this function takes no data.
	 */
	bool PublishPending();

	/** Fields the UI has not confirmed applying. Harvests the echo, so it is not const. */
	int32 NumOutstandingFields() { return Channel.NumOutstandingFields(); }

	uint64 GetNumSamples() const { return Sampler.GetNumSamples(); }
	uint64 GetNumFieldsMarked() const { return Sampler.GetNumFieldsMarked(); }
	uint64 GetNumPublishes() const { return Channel.GetNumPublishes(); }
	uint64 GetNumFieldsPublished() const { return Channel.GetNumFieldsPublished(); }
	uint64 GetLastPublishedGeneration() const { return Channel.GetLastPublishedGeneration(); }

	/** The game-side previous-value shadow. Game thread; diagnostics and tests. */
	const FVaCuusModelShadow& GetGameShadow() const { return Sampler.GetShadow(); }

	/**
	 * `vacuus.DumpModel`'s game half: the layout, the game shadow's values, and the pending and
	 * unacknowledged sets, to LogVaCuus at Display. Game thread.
	 *
	 * TWO HALVES RATHER THAN ONE, AND THE SPLIT IS OWNERSHIP, NOT TIDINESS. The UI shadow is a
	 * UScriptStruct instance the UI thread writes with no synchronisation at all (see the class
	 * comment): its FString fields are freed and reallocated by ApplyUpdate, so reading one from
	 * here is a use-after-free, not a torn integer. So the game thread prints what it owns and
	 * enqueues the rest -- see FVaCuusUIThread::EnqueueDumpModel and DumpUISide() below.
	 *
	 * The two halves therefore appear in the log one UI frame apart, and both name the view and
	 * model so they can be read as a pair. That gap is itself the diagnostic in the case that
	 * matters most: a UI half that never appears means the model reached no context.
	 */
	void DumpGameSide(uint32 ViewId);

	//~ ------------------------------------------------------------ Recompile refusal (akj.16)

	/**
	 * The game-side half of the refusal, called inside the struct-editor PreChange window
	 * while the OLD FProperty chain is still alive (UserDefinedStructureCompilerUtils.cpp:599
	 * broadcasts before the compile at :622): sets the dead flag that turns Sample() and
	 * PublishPending() into no-ops, destroys the game-side shadow through the normal
	 * DestroyStruct path NOW, and arms the drop state for the UI-side command the caller is
	 * about to enqueue. Game thread. Idempotent refusal: false when already condemned, so a
	 * second broadcast for the same incident (the engine recompiles dependent structs in the
	 * same transaction) neither logs twice nor re-enqueues.
	 */
	bool CondemnForStructRecompile();

	/** True from CondemnForStructRecompile() on; the walk's skip and the tests' observable. Game thread. */
	bool IsCondemned() const { return bDeadFromRecompile; }

	/**
	 * The fence's timeout resolution, game thread, still inside PreChange. Attempts
	 * DropQueued -> AbandonRequired; on success the UI-side buffers are declared
	 * unreclaimable-through-the-type and true is returned (the caller logs the leak with
	 * EstimateAbandonedBytes()). On failure the UI side won the race: if it is mid-teardown
	 * this SPINS until TornDown -- returning from PreChange while DestroyStruct walks the old
	 * chain would hand the compile a live use of memory it is about to free -- and false
	 * (no leak) is returned.
	 */
	bool ResolveDropTimeout();

	EVaCuusModelDropState GetDropState() const
	{
		return static_cast<EVaCuusModelDropState>(DropState.load(std::memory_order_acquire));
	}

	/**
	 * Lower bound on what an Abandon() of the UI-side buffers leaks, in bytes: the struct
	 * stride times (one UI shadow + the channel slots actually allocated). The heap payloads
	 * INSIDE those instances (FString buffers, array blocks) are additional and uncountable
	 * from here -- counting them would mean walking a property chain that is being deleted.
	 * Game thread (reads the producer-side allocation count).
	 */
	uint64 EstimateAbandonedBytes() const;

	/**
	 * The UI-side half: tears down the UI shadow and the channel's three slot buffers, by
	 * DestroyStruct or Abandon() as the drop state dictates (the CAS dance above), and
	 * forgets the RmlUi handle. Runs on the UI thread from the DropModelForRecompile drain --
	 * AFTER the caller removed this model from the UI thread's registry and the write router,
	 * and AFTER Context::RemoveDataModel dropped RmlUi's raw view of the shadow. The producer
	 * is quiescent by then: the dead flag preceded the command in game-thread program order.
	 */
	void TearDownUISideForRecompile();

	//~ ------------------------------------------------------------------ UI thread

	/**
	 * Creates this model on Context and binds every top-level variable to the UI shadow's
	 * base. UI thread only, and once only.
	 *
	 * MUST HAPPEN BEFORE THE DOCUMENT LOADS, and that is RmlUi's requirement rather than this
	 * class's: `data-model` is read exactly once, in Element::SetParent
	 * (Element.cpp:2202-2219), when the document's body is parented into the context. A name
	 * that does not resolve there leaves the whole subtree with no model -- an inert document,
	 * every `{{Field}}` resolving against nothing, and no retry and no second lookup.
	 *
	 * IT IS NOT SILENT, and that is a correction to what this milestone assumed throughout.
	 * Element.cpp:2218 calls Log::Message(LT_ERROR, "Could not locate data model '%s' in element
	 * %s."), which reaches UE as `LogVaCuus: Error: [Rml] ...` --
	 * FVaCuusSystemInterface::LogMessage carries the whole argument, including what genuinely
	 * IS compiled out. So the diagnostic exists; what it does not do is name the bind that
	 * should have preceded the load, which is why UVaCuusView::BindModel warns separately when
	 * it can see that a load has already been requested.
	 */
	bool BindToContext(Rml::Context& Context);

	/** True once BindToContext() has succeeded. UI thread. */
	bool IsBoundToContext() const { return bBoundToContext; }

	/**
	 * Applies the newest published update, if there is one, and echoes its generation back.
	 * UI thread only; a no-op before BindToContext() and when nothing was published.
	 *
	 * Called from FVaCuusUIThread::ApplyModelUpdates(), at the frame's DataApply phase --
	 * after both drains and BEFORE Context::Update(), so the re-evaluation a dirtied variable
	 * causes is paid inside Update where spec 9 budgets it.
	 */
	void ApplyPendingUpdate();

	uint64 GetNumUpdatesApplied() const { return Channel.GetNumUpdatesApplied(); }
	uint64 GetAppliedGeneration() const { return Channel.GetAppliedGeneration(); }

	/** Fields copied into the UI shadow across every apply. UI thread; the apply's observable. */
	uint64 GetNumFieldsApplied() const { return NumFieldsApplied; }

	/**
	 * The UI-side shadow, CONST. There is deliberately no mutable accessor -- see the class
	 * comment's single-writer argument. Read from the UI thread, or from a test that has
	 * synchronised with it.
	 */
	const FVaCuusModelShadow& GetUIShadow() const { return UIShadow; }

	/**
	 * The write router's revert-dirty (M4 Task 9, spec 3.10): DirtyVariable(TopLevelName)
	 * on this model's handle, so the next Context::Update re-runs the name's views FROM
	 * THE SHADOW -- which nothing has written, so a control RmlUi's default action
	 * mutated (a clicked checkbox's `checked`, InputTypeCheckbox.cpp:43-46) snaps back to
	 * the authoritative value. NOT a write and NOT an apply: the one shadow writer is
	 * still ApplyUpdate(), this only marks a name dirty exactly as an apply would.
	 *
	 * UI thread only; a no-op before BindToContext(). Called from
	 * FVaCuusWriteRouter::FlushPendingReverts, inside the frame's DataApply phase.
	 */
	void DirtyTopLevelFromShadow(const FString& TopLevelName);

	/**
	 * `vacuus.DumpModel`'s UI half: whether the bind reached a context, the UI shadow's values,
	 * the published dirty set that last reached it, and the applied generation. UI thread; see
	 * DumpGameSide() for why this is a separate call on a separate thread.
	 */
	void DumpUISide(uint32 ViewId);

private:
	/** The body of the apply, factored out only so the ConsumeUpdate lambda stays one line. */
	void ApplyUpdate(const FVaCuusModelUpdate& Update);

	/**
	 * The name EXACTLY as the caller wrote it, and the ONLY string that may reach
	 * Context::CreateDataModel -- see the constructor comment for why an FName round-trip
	 * mangles it in a packaged game. ModelName below is derived from it and is a
	 * case-insensitive KEY, never an identity.
	 */
	FString ModelNameStr;
	FName ModelName;

	//~ DECLARATION ORDER IS LOAD-BEARING: Sampler and Channel each hold a
	//~ `const FVaCuusModelLayout&` bound to Layout, so Layout must be constructed first and
	//~ destroyed last.
	FVaCuusModelLayout Layout;
	FVaCuusModelSampler Sampler;
	FVaCuusModelChannel Channel;

	/** UI thread. RmlUi holds a raw void* to its base from BindToContext() until the context dies. */
	FVaCuusModelShadow UIShadow;

	/** UI thread. Default-constructed handles are falsy; bBoundToContext is what gates its use. */
	Rml::DataModelHandle ModelHandle;

	/**
	 * FVaCuusModelLayout::GetTopLevelNames(), converted ONCE at bind time.
	 *
	 * DirtyVariable takes a `const Rml::String&` (i.e. std::string), so converting per dirtied
	 * field per frame would allocate on the hot path this milestone budgets -- and the names
	 * are fixed for the life of the layout. Indexed by FVaCuusModelField::TopLevelNameIndex.
	 */
	TArray<Rml::String> TopLevelNamesUtf8;

	uint64 NumFieldsApplied = 0;

	bool bBoundToContext = false;

	/**
	 * Set by CondemnForStructRecompile(), read by the game-thread entry points it gates
	 * (Sample, PublishPending) and by the walk's idempotence check. PLAIN bool, not the
	 * atomic below, and that is a statement: every reader and the one writer are the game
	 * thread, so making it atomic would claim a cross-thread contract it does not have.
	 */
	bool bDeadFromRecompile = false;

	/** Latches the one refused-Sample Warning per model; frame-rate calls must not spam. Game thread. */
	bool bLoggedRefusedSample = false;

	/** EVaCuusModelDropState; the enum's comment is the protocol. The ONE cross-thread member here. */
	std::atomic<uint8> DropState{static_cast<uint8>(EVaCuusModelDropState::Live)};
};
