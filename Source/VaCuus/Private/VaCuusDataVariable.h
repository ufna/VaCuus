// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusModelLayout.h"

#include "UObject/StrongObjectPtr.h"

// PRIVATE HEADER, and that is what makes the RmlUi include legal here: VaCuus depends on
// VaCuusRml privately (VaCuus.Build.cs:27-33), so an RmlUi type may appear in this
// module's Private/ tree and nowhere else -- the same rule VaCuusSystemInterface.h and
// VaCuusInputMap.h already follow, and the same reason
// FVaCuusRecordingRenderInterface.h moved out of Public/ in M1.
#include <RmlUi/Core/DataVariable.h>

namespace Rml
{
class DataModelConstructor;
}

/**
 * The RmlUi adapter: UE reflection driving RmlUi's data binding with no per-type codegen.
 *
 *
 * THE PATH THIS USES, AND WHY IT EXISTS AT ALL.
 *
 * RmlUi's documented binding surface is templated -- Bind<T>, RegisterStruct<T>,
 * RegisterMember -- and keyed on Family<T>::Id(), a per-type counter
 * (Traits.h / Traits.cpp). A type described only at runtime cannot reach any of it.
 *
 * But that layer is a convenience over a fully runtime-dispatched core, and there is a
 * public, non-template door into the core. Two lines decide it:
 *
 *   DataModelHandle.h:65   bool BindCustomDataVariable(const String& name, DataVariable data_variable)
 *   DataVariable.h:23      DataVariable(VariableDefinition* definition, void* ptr)
 *
 * `VariableDefinition` (DataVariable.h:46-64) is a plain polymorphic class, NOT a
 * template, whose Child() resolves a field BY STRING at run time; DataModel::BindVariable
 * validates only the name and the non-nullness of the definition
 * (DataModel.cpp:117-140). Family<T> and the whole DataTypeRegister are bypassed -- which
 * is also precisely why this design is PIE-safe, since Family<T>::Id()'s non-atomic
 * counter is the one process-wide global on the binding path and nothing here touches it.
 *
 *
 * THE FOUR DEFINITION CLASSES, AND HOW A DOTTED ADDRESS RESOLVES.
 *
 * A model is bound one TOP-LEVEL NAME at a time, each as a separate DataVariable, because
 * that is the only granularity DirtyVariable accepts (DataModel.cpp:325-331). Every one of
 * them is bound with the SAME void*: the shadow buffer's base.
 *
 *   {{Health}}        -> variables["Health"] = {FVaCuusPropertyDefinition, ShadowBase}
 *                          Get -> ContainerPtrToValuePtr(ShadowBase) -> scalar Get
 *
 *   {{Origin.X}}      -> variables["Origin"] = {FVaCuusStructDefinition, ShadowBase}
 *                          Child("X") -> {FVaCuusPropertyDefinition, ShadowBase + offsetof(Origin)}
 *                          Get        -> ContainerPtrToValuePtr(that) -> scalar Get
 *
 *   {{Rows[2].Killer}} -> variables["Rows"] = {FVaCuusPropertyDefinition, ShadowBase}
 *                          Child({2})      -> {row type's root struct def, GetRawPtr(2)}
 *                                             computed by FVaCuusArrayDefinition per call
 *                          Child("Killer") -> the ROW type's own property definition
 *                          Get             -> ContainerPtrToValuePtr(GetRawPtr(2)) -> scalar Get
 *
 * THE INVARIANT THAT MAKES THAT WORK, AND IT IS THE ONE TO PROTECT: an
 * FVaCuusStructDefinition is ALWAYS handed THE BASE OF THE DEFINITION SET'S OWN TYPE,
 * never a shifted pointer, and a leaf's offset is applied exactly once, at the moment
 * Child() hands out the property definition. For a model that base is the shadow buffer;
 * for a row type serving as an array element it is FScriptArrayHelper::GetRawPtr(i), which
 * IS an instance base of exactly that type (spec 2(d)) -- the same invariant with one more
 * producer, not a second invariant. It is expressible because
 * FVaCuusModelField::ContainerOffset is absolute from its own type's base, and it is
 * enforced by shape: AddLeaf() is the only member function that takes an offset at all,
 * and AddNested() has no parameter to get it wrong with.
 *
 * Both idioms are RmlUi's own -- StructDefinition::Child hands the parent's ptr down
 * unchanged (DataVariable.cpp:76-95) while ArrayDefinition::Child computes a new one
 * (DataVariable.h:143-163) -- so neither is a deviation.
 *
 *
 * WHY DERIVE FROM BasePointerDefinition RATHER THAN WRITE Get/Set BY HAND. Verified in
 * DataVariable.cpp:134-169: the base constructor takes its variable TYPE from the
 * underlying definition (:135), all four of Get/Set/Size/Child null-check `ptr` and then
 * forward through DereferencePointer (:138-164), and ReflectMemberNames forwards as-is
 * (:166-169). So one 3-line override gets correct behaviour for a scalar leaf and would
 * get it for a struct member too. RmlUi's own MemberObjectDefinition is exactly this
 * (DataVariable.h:196-208).
 *
 * StructDefinition and ArrayDefinition cannot be reused the same way: both are `final`
 * (DataVariable.h:119, :134).
 */

class FProperty;
class FVaCuusModelShadow;
class UScriptStruct;

/**
 * One bound leaf's value: reads through the FProperty accessor its kind names, and
 * REFUSES every write.
 *
 * ONE CLASS WITH AN EXHAUSTIVE SWITCH RATHER THAN ONE CLASS PER EVaCuusFieldKind, and
 * this is a deliberate deviation from the plan's wording. The eleven bodies are one to
 * three lines each and share their entire surface -- the same FProperty member, the same
 * construction, and the same refusing Set. What the switch buys that eleven classes do
 * not: it has no `default`, so -Wswitch turns a NEW field kind into a compile error right
 * here, whereas a missing subclass would only be a runtime checkNoEntry. The codebase
 * already uses that shape for LexToString(EVaCuusFieldKind).
 */
class FVaCuusScalarDefinition final : public Rml::VariableDefinition
{
public:
	FVaCuusScalarDefinition(const FProperty* InProperty, EVaCuusFieldKind InKind, FString InDiagnosticPath);

	/** InValuePtr is the VALUE pointer -- BasePointerDefinition has already offset it. */
	virtual bool Get(void* InValuePtr, Rml::Variant& OutVariant) override;

	/**
	 * ALWAYS false, and that is spec 4's third invariant rather than a limitation of this
	 * class. See VaCuusData::GetNumRefusedSets() for the whole argument.
	 */
	virtual bool Set(void* InValuePtr, const Rml::Variant& Variant) override;

	/**
	 * `data-for` over a leaf. RmlUi's base Size() already answers this correctly -- warn,
	 * return 0, iterate no rows (DataVariable.cpp:40-44, reached with no Type() check from
	 * DataViewFor::Update, DataViewDefault.cpp:498-503) -- but its warning names nothing.
	 * Same contract, named: one latched Warning carrying DiagnosticPath, then 0 (spec 3.5).
	 */
	virtual int Size(void* InValuePtr) override;

private:
	/** Split out only because each needs two shapes of property and a diagnostic of its own. */
	bool GetEnumName(const void* InValuePtr, Rml::Variant& OutVariant);
	bool GetObjectPath(const void* InValuePtr, Rml::Variant& OutVariant);

	const FProperty* Property = nullptr;
	EVaCuusFieldKind Kind = EVaCuusFieldKind::Bool;

	/** "FPlayerHud.Origin.X" -- built once, so a log line can name the field. */
	FString DiagnosticPath;

	/** Log-throttle only. UI-thread state, like everything else on this path. */
	bool bRefusalLogged = false;
	bool bUnreadableLogged = false;
	bool bSizeMissLogged = false;
};

/**
 * One property's addressing step: turns a CONTAINER pointer into a VALUE pointer and
 * forwards everything else to the underlying definition.
 *
 * This is the only place in VaCuus that calls ContainerPtrToValuePtr on the RmlUi path,
 * which is the point -- FProperty offers five identically-behaved offset accessors, one of
 * them literally named GetOffset_ReplaceWith_ContainerPtrToValuePtr (UnrealType.h:466),
 * and the blessed one is the only form that stays correct for a bitfield (whose value
 * pointer is the storage integer, not the bit).
 */
class FVaCuusPropertyDefinition final : public Rml::BasePointerDefinition
{
public:
	FVaCuusPropertyDefinition(const FProperty* InProperty, Rml::VariableDefinition* InUnderlying);

protected:
	virtual void* DereferencePointer(void* InContainerPtr) override;

private:
	const FProperty* Property = nullptr;
};

/**
 * One level of a model: resolves a member BY STRING, which is the whole reason a
 * runtime-described type can be bound at all.
 *
 * Members live in a flat array and are found by linear scan. That is not laziness: RmlUi's
 * own StructDefinition uses SmallOrderedMap, i.e. a sorted vector (Config.h:87), for the
 * same size class, and a TMap<FString,...> would cost an FString construction per lookup
 * to convert RmlUi's std::string address segment -- once per address segment per
 * expression per dirty view, which is the hot path this milestone budgets.
 */
class FVaCuusStructDefinition final : public Rml::VariableDefinition
{
public:
	explicit FVaCuusStructDefinition(FString InDiagnosticPath);

	/**
	 * A leaf. ContainerOffset is absolute from the base of the definition set's own type
	 * (FVaCuusModelField::ContainerOffset) -- the model shadow's base, or an element's
	 * GetRawPtr(i), whichever kind of instance this set describes.
	 */
	void AddLeaf(const FString& Segment, FVaCuusPropertyDefinition* Definition, int32 ContainerOffsetFromTypeBase);

	/** A nested struct. It is handed the type's base unchanged, so there is no offset to pass. */
	void AddNested(const FString& Segment, FVaCuusStructDefinition* Definition);

	/** InBase is the base of this definition set's own type -- see the file comment's invariant. */
	virtual Rml::DataVariable Child(void* InBase, const Rml::DataAddressEntry& Address) override;

	/** `data-for` over a struct: the scalar definition's contract, struct-flavoured latch. */
	virtual int Size(void* InBase) override;

	virtual Rml::StringList ReflectMemberNames() override;

	/** Exact segment match; null when absent. Diagnostics and tests. */
	const FVaCuusStructDefinition* FindNested(const FString& Segment) const;

private:
	struct FMember
	{
		/** UTF-8, so a lookup compares against RmlUi's address segment with no conversion. */
		Rml::String Segment;

		/** Owned by FVaCuusModelDefinitions, which outlives every model bound from it. */
		Rml::VariableDefinition* Definition = nullptr;

		/** Zero for a nested struct: it receives the type's base unchanged. */
		int32 ContainerOffsetFromTypeBase = 0;

		/** Only nested members are re-enterable by FindNested. */
		bool bNested = false;
	};

	const FMember* Find(const Rml::String& Segment) const;

	TArray<FMember> Members;
	FString DiagnosticPath;

	/**
	 * Log throttles, and they are not cosmetic -- see Child()'s comment.
	 *
	 * BOTH BRANCHES BELOW ARE ON THE PER-FRAME EXPRESSION PATH. An address that misses is
	 * re-resolved every time its ROOT variable is dirtied -- so a typo under a struct the game
	 * writes each frame costs one log line per UI frame, at UI frame rate, for the life of the
	 * process, and the document is exactly as wrong on the ten-thousandth as on the first. Same
	 * shape and same reason as FVaCuusScalarDefinition's two latches: UI-thread state, one
	 * report per definition, and the definition is process-wide per model TYPE.
	 *
	 * THE COST OF THE SHAPE, STATED: a second, DIFFERENT bad member under the same struct level
	 * is silent. The one line that does get printed lists every member the level has, so both
	 * typos are usually diagnosable from it; `vacuus.DumpModel` prints the layout when they are
	 * not.
	 */
	bool bIndexedMissLogged = false;
	bool bMemberMissLogged = false;
	bool bSizeMissLogged = false;
};

/**
 * One TArray field: `data-for` rows, `{{Arr.size}}` and indexed access, every answer
 * computed from the INCOMING value pointer at call time.
 *
 * STATELESS IS LOAD-BEARING, NOT STYLE. This class holds the FArrayProperty*, a borrowed
 * element definition and a diagnostic name -- never an element pointer and never a cached
 * Num(). The registry shares one definition set per model TYPE (see
 * FVaCuusDefinitionRegistry below), so two views over two instances arrive HERE with two
 * different void*s, interleaved however their contexts update; and RmlUi re-walks
 * root -> Child(...) from the bind-time pointer on every evaluation, storing nothing
 * (DataModel::GetVariable, DataModel.cpp:275-302), so a value cached across calls would be
 * another instance's value. Spec 2(c)/4: no stage stores an element address --
 * FScriptArrayHelper is constructed per call over the value pointer, and a GetRawPtr()
 * result lives for exactly one expression. VaCuus.Model.ArrayStateless is the test that
 * fails if either property breaks.
 *
 * Get and Set stay the base-class failures (DataVariable.cpp:30-39): a value read on the
 * array itself is a document bug RmlUi already reports, and a write into an ELEMENT
 * arrives at the element definition's Set, which refuses (I3 -- the scalar definition's
 * comment carries the argument).
 */
class FVaCuusArrayDefinition final : public Rml::VariableDefinition
{
public:
	FVaCuusArrayDefinition(const FArrayProperty* InArrayProperty, Rml::VariableDefinition* InElementDefinition, FString InDiagnosticPath);

	/** InValuePtr addresses the TArray itself -- BasePointerDefinition has already offset it. */
	virtual int Size(void* InValuePtr) override;

	/**
	 * Named "size" -> the element count as a literal; a legal index -> the element;
	 * everything else -> an empty variable and a latched, NAMED Warning.
	 */
	virtual Rml::DataVariable Child(void* InValuePtr, const Rml::DataAddressEntry& Address) override;

private:
	const FArrayProperty* ArrayProperty = nullptr;

	/**
	 * Borrowed: a scalar-element definition is owned by the SAME definitions set as this
	 * class; a struct-element one by the ELEMENT type's own set, which the registry never
	 * releases while the UI thread is up -- same lifetime rule as every definition here.
	 */
	Rml::VariableDefinition* ElementDefinition = nullptr;

	/** "FHudModel.Killfeed" -- so both Child() diagnostics can name the field. */
	FString DiagnosticPath;

	/** Latches, one per branch, same shape and reason as the struct definition's pair. */
	bool bNamedMissLogged = false;
	bool bOutOfBoundsLogged = false;
};

/**
 * Every definition one model TYPE needs, built once from its layout and owned for the
 * lifetime of the process.
 *
 * Constructed only by FVaCuusDefinitionRegistry -- which is the shape that keeps the
 * UI-thread assertion meaningful, since there is then no other way to make one.
 */
class FVaCuusModelDefinitions
{
public:
	/** What Task 7 binds: one entry per top-level name, all pointing at the shadow base. */
	struct FTopLevelVariable
	{
		FString Name;
		Rml::VariableDefinition* Definition = nullptr;
	};

	FVaCuusModelDefinitions(const FVaCuusModelDefinitions&) = delete;
	FVaCuusModelDefinitions& operator=(const FVaCuusModelDefinitions&) = delete;

	const UScriptStruct* GetStruct() const { return Struct.Get(); }

	/** In FVaCuusModelLayout::GetTopLevelNames() order. */
	TConstArrayView<FTopLevelVariable> GetTopLevelVariables() const { return TopLevelVariables; }

	/** Exact match on a top-level name; null when absent. */
	Rml::VariableDefinition* FindTopLevel(const FString& Name) const;

	/**
	 * The WHOLE type as one struct definition, resolving its top-level names against the
	 * base of an instance -- what an array-of-struct field borrows as its element
	 * definition, so that {{Rows[i].Field}} walks the row type's own definitions from
	 * GetRawPtr(i). Never null for a built set; empty of members for an empty model.
	 */
	Rml::VariableDefinition* GetRootStruct() const { return RootStruct; }

private:
	friend class FVaCuusDefinitionRegistry;

	explicit FVaCuusModelDefinitions(const FVaCuusModelLayout& Layout);

	/** Creates (or finds) the struct definition for a dotted prefix such as "Origin." . */
	FVaCuusStructDefinition* GetOrCreateStructFor(const FString& Prefix);

	/**
	 * Pins the type for the same reason FVaCuusModelLayout does, and for one more: the
	 * registry is keyed on the raw UScriptStruct*, so letting a UUserDefinedStruct be
	 * collected would leave a key that a freshly allocated struct could reuse -- and every
	 * FProperty* below belongs to it.
	 */
	TStrongObjectPtr<const UScriptStruct> Struct;

	//~ Owned definitions. Raw pointers into these are handed to RmlUi, which owns nothing
	//~ on the BindCustomDataVariable path, so the arrays must never reallocate their
	//~ ELEMENTS -- TUniquePtr elements keep the pointees stable however the array grows.
	TArray<TUniquePtr<FVaCuusScalarDefinition>> ScalarDefinitions;
	TArray<TUniquePtr<FVaCuusPropertyDefinition>> PropertyDefinitions;
	TArray<TUniquePtr<FVaCuusStructDefinition>> StructDefinitions;
	TArray<TUniquePtr<FVaCuusArrayDefinition>> ArrayDefinitions;

	/** Prefix ("Origin." , "A.B.") -> the struct definition for that level. Build-time only. */
	TMap<FString, FVaCuusStructDefinition*> StructsByPrefix;

	TArray<FTopLevelVariable> TopLevelVariables;

	/** Owned through StructDefinitions; built by the constructor's pass 3 (see GetRootStruct). */
	FVaCuusStructDefinition* RootStruct = nullptr;
};

/**
 * The process-wide definition store. UI THREAD ONLY, ASSERTED.
 *
 * PROCESS-WIDE RATHER THAN PER MODEL, because a definition carries only an FProperty*, a
 * field kind and a diagnostic string -- no shadow pointer, no per-instance state at all.
 * Two views showing the same USTRUCT therefore share one set of definitions and differ
 * only in the void* they were bound with. Nothing in RmlUi owns a definition on this path
 * (research 2.1's crash table: BindCustomDataVariable stores the 16-byte {definition, ptr}
 * pair by value and owns neither), so somebody has to, and this is it.
 *
 * NEVER CLEARED WHILE THE UI THREAD IS UP, deliberately, and it costs one pinned
 * UScriptStruct per distinct model type ever bound. That is the same property RmlUi's own
 * DataTypeRegister has (research 8.6: it exposes only RegisterDefinition and
 * GetDefinition), and for the same reason -- a definition handed out once may still be
 * reachable from any live model, and there is no unbind API anywhere to tell.
 *
 * IT IS CLEARED IN EXACTLY ONE PLACE, FVaCuusUIThread::Exit(), through ReleaseAll(). See
 * that function for why leaving it to static destruction is not an option.
 */
class FVaCuusDefinitionRegistry
{
public:
	/** Builds on first use for this type, then returns the cached set. Never null for a valid layout. */
	static const FVaCuusModelDefinitions* GetOrCreate(const FVaCuusModelLayout& Layout);

	/** Cached model types. The registry's only observable, and what the cache-hit test asserts. */
	static int32 Num();

	/**
	 * Drops every cached definition set. Returns how many were released. UI thread only, and
	 * called from FVaCuusUIThread::Exit() and nowhere else -- see the .cpp for the
	 * static-destruction argument and for why Exit() is the one point where this is provable.
	 */
	static int32 ReleaseAll();
};

namespace VaCuusData
{
/**
 * Binds every top-level name of Layout into Constructor, all pointing at the shadow's
 * base. Returns how many were bound. UI thread only.
 *
 * TAKES THE SHADOW RATHER THAN A void*: the pointer RmlUi retains is stored once, at bind
 * time, and never revalidated (research 2.1), so the one place where "this addresses a
 * UI-owned instance of exactly this type" can be checked is here -- and a parameter that
 * carries its own type is the only form that lets it be.
 *
 * ORDERING IS A HARD REQUIREMENT OF RmlUi, not of this function: `data-model` is read
 * exactly once, in Element::SetParent (Element.cpp:2203-2219), so every variable must
 * exist before the document loads or the address never resolves.
 */
int32 BindModelVariables(Rml::DataModelConstructor& Constructor, const FVaCuusModelLayout& Layout, FVaCuusModelShadow& Shadow);

/**
 * How many writes the shadow has refused since the process started.
 *
 * WHY A COUNTER AND NOT JUST THE LOG LINE. Spec 4/I3: RmlUi's own data-value,
 * data-checked and `data-event-click="Health = 50"` all reach VariableDefinition::Set and
 * would write the shadow DIRECTLY, with no VaCuus code involved and no game-thread
 * participation -- after which the game-side differ compares the live struct against its
 * OWN shadow, sees no change, sets no bit, and the two shadows diverge permanently. A
 * stale value on screen forever, with no log line, because the idle gate correctly
 * withholds a frame that genuinely did not change.
 *
 * Refusing is clean because BOTH RmlUi call sites skip their DirtyVariable when Set
 * returns false -- verified at DataControllerDefault.cpp:57-59 and
 * DataExpression.cpp:1185-1197, which are the only two.
 *
 * The refusal LOGS ONCE PER FIELD, so the log alone cannot be asserted twice in one
 * process; this counter is the durable observable, and an invariant with no observable
 * cannot be tested and will rot.
 *
 * One-way binding is M3's contract. Write-back is M4's surface, where it belongs on a
 * delegate marshalled to the game thread rather than on a scribble into a buffer the game
 * thread is concurrently diffing.
 */
int32 GetNumRefusedSets();

/**
 * Evaluation counters (spec 3.5): every evaluation of a bound leaf terminates in
 * FVaCuusScalarDefinition::Get, and every array-path evaluation passes the array
 * definition's Size or Child. They exist because the idle gate's third layer -- an
 * unchanged model must EVALUATE nothing, not merely publish and apply nothing -- is an
 * invariant with no other observable: RmlUi's DataViews::Update runs unconditionally
 * every frame and exposes no counter of its own, so "0 evaluations" is assertable only as
 * an exact delta of these. Same UI-thread-only, plain-static shape as GetNumRefusedSets.
 */
int32 GetNumScalarGets();
int32 GetNumArraySizes();
int32 GetNumArrayChilds();
}	 // namespace VaCuusData
