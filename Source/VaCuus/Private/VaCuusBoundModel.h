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

namespace Rml
{
class Context;
}

class UScriptStruct;

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
	 * Any thread that may touch an already-linked UScriptStruct; in practice the game thread,
	 * from UVaCuusView::BindModel.
	 */
	FVaCuusBoundModel(FName InModelName, const UScriptStruct* InStruct);

	//~ Neither copyable nor movable: FVaCuusModelChannel is neither (it holds a reference to
	//~ Layout, which is a member of this object), and RmlUi holds a raw pointer to UIShadow's
	//~ base. Owners hold this by TSharedPtr.
	FVaCuusBoundModel(const FVaCuusBoundModel&) = delete;
	FVaCuusBoundModel& operator=(const FVaCuusBoundModel&) = delete;

	/** False when the struct did not resolve or a shadow could not be allocated. */
	bool IsValid() const;

	FName GetModelName() const { return ModelName; }
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

	//~ ------------------------------------------------------------------ UI thread

	/**
	 * Creates this model on Context and binds every top-level variable to the UI shadow's
	 * base. UI thread only, and once only.
	 *
	 * MUST HAPPEN BEFORE THE DOCUMENT LOADS, and that is RmlUi's requirement rather than this
	 * class's: `data-model` is read exactly once, in Element::SetParent
	 * (Element.cpp:2202-2219), when the document's body is parented into the context. A name
	 * that does not resolve there logs Log::LT_ERROR -- which is compiled out in every
	 * configuration this plugin builds (spec 8) -- and leaves the whole subtree with no model,
	 * i.e. an inert document and no diagnostic. UVaCuusView::BindModel warns when it can see
	 * that a load has already been requested.
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
	 * `vacuus.DumpModel`'s UI half: whether the bind reached a context, the UI shadow's values,
	 * the published dirty set that last reached it, and the applied generation. UI thread; see
	 * DumpGameSide() for why this is a separate call on a separate thread.
	 */
	void DumpUISide(uint32 ViewId);

private:
	/** The body of the apply, factored out only so the ConsumeUpdate lambda stays one line. */
	void ApplyUpdate(const FVaCuusModelUpdate& Update);

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
};
