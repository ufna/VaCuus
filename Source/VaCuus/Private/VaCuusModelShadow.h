// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"

class UScriptStruct;

/**
 * One model's shadow copy: a REAL UScriptStruct instance in heap memory that this
 * object owns, initialised with InitializeStruct and torn down with DestroyStruct.
 *
 * Two exist per bound model -- the game-side previous-value shadow the differ compares
 * against, and the UI-side shadow RmlUi holds a raw void* into. Both are this class; the
 * difference is only which thread owns them.
 *
 *
 * WHY A REAL INSTANCE AND NOT A PACKED BUFFER OF ITS OWN DESIGN (spec 3.1).
 *
 * Every read on both sides goes through an FProperty accessor, and every one of those
 * accessors resolves an address as `(uint8*)Container + Offset_Internal` --
 * FProperty::ContainerVoidPtrToValuePtrInternal, UnrealType.h:733-745, which is what
 * ContainerPtrToValuePtr<T>(void*) calls (:805-809). Offset_Internal is the offset in the
 * REAL struct. So if this buffer is a real instance, ContainerPtrToValuePtr works
 * directly against it, and so do CopySingleValue, GetPropertyValue_InContainer and every
 * other accessor -- the RmlUi adapter needs one class that does
 * `Property->ContainerPtrToValuePtr<void>(base)` and nothing else.
 *
 * With a packing of our own, every field kind would need hand-written marshalling on both
 * sides plus its own container types, and the layout entry would carry two offsets that
 * are the same number only by coincidence. Spec 13.2 records that incoherence as one of
 * the six errors v1 of the design shipped with.
 *
 * The cost of the real instance is that it is a full-fidelity copy: an FString field
 * really is an FString and really allocates. That is the price of the accessors. For
 * SCALAR fields it is paid once per model, not per frame -- every later sync assigns
 * through the same members. An ARRAY field is assignment-shaped per republish instead:
 * each sync rewrites its elements in place (FVaCuusModelArrayDesc::SyncCopy),
 * allocation-free only where the existing element and container capacity absorb the
 * content -- the grow-only reuse rule (ReallocForCopy, Array.h:710-751, `NewMax >
 * PrevMax`) and its shrink caveat live on SyncCopy (VaCuusModelLayout.cpp:27-52), and
 * spec 9's counting allocator is what holds "capacity absorbs it" to account.
 *
 *
 * AND THE CONSEQUENCE THAT DECIDES WHAT MAY LIVE IN IT: THIS BUFFER IS INVISIBLE TO GC.
 *
 * The collector finds struct instances by walking references it already knows about --
 * a UObject's properties, an FGCObject's AddReferencedObjects, an FReferenceCollector
 * handed a struct explicitly. A struct's own AddStructReferencedObjects hook
 * (Class.h:1148-1153, dispatched through ICppStructOps::HasAddStructReferencedObjects,
 * :2055-2058) is only ever called by something that already reached the instance. Nothing
 * reaches THIS one: it is FMemory::Malloc'd memory owned by a plain C++ object on a
 * non-game thread, referenced by no UObject and by no FGCObject.
 *
 * So a hard UObject* copied in here would be a strong-looking reference the collector
 * never sees: the object is collected, the pointer dangles, and the next read is a
 * use-after-free with no log line and no assert anywhere. That is why FObjectProperty is
 * refused by FVaCuusModelLayout, and it is a strictly harder argument than the data-race
 * one -- a race can at worst read a torn value, this reads freed memory.
 *
 * The strong reference below is to the TYPE, not to anything inside the instance. It
 * keeps the UScriptStruct (and therefore the FProperty chain the layout resolved) alive
 * for a UUserDefinedStruct, which is an ordinary collectable object; it says nothing
 * about the values.
 *
 *
 * THREADING. Construction and destruction are safe on any thread for a type that is
 * already loaded and linked. READS AND WRITES ARE NOT SYNCHRONISED AT ALL: each shadow
 * belongs to exactly one thread, and the pairing of the two shadows with the game and UI
 * threads is what the channel exists to keep honest.
 */
class FVaCuusModelShadow
{
public:
	/** An empty shadow: no type, no buffer. */
	FVaCuusModelShadow() = default;

	/**
	 * Allocates and initialises one instance of InStruct. A null type yields an empty
	 * shadow and one Error; nothing else can fail.
	 */
	explicit FVaCuusModelShadow(const UScriptStruct* InStruct);

	~FVaCuusModelShadow();

	/**
	 * NON-COPYABLE BY SHAPE, not by convention. A copy would have to duplicate a live
	 * UScriptStruct instance, and the only reason to want one is to compare two shadows --
	 * which is the differ's job and is done field by field, never buffer by buffer
	 * (a memcmp of two struct instances is wrong for every kind that owns memory, and
	 * wrong for adjacent bitfields even when it is not).
	 */
	FVaCuusModelShadow(const FVaCuusModelShadow&) = delete;
	FVaCuusModelShadow& operator=(const FVaCuusModelShadow&) = delete;

	/** Movable, so a shadow can live in a TArray/TMap of models without an extra indirection. */
	FVaCuusModelShadow(FVaCuusModelShadow&& Other);
	FVaCuusModelShadow& operator=(FVaCuusModelShadow&& Other);

	/** True when a buffer is allocated and initialised. */
	bool IsValid() const { return Data != nullptr; }

	/** The type this buffer is an instance of, or null. */
	const UScriptStruct* GetStruct() const { return Struct.Get(); }

	/**
	 * The struct base pointer: what FVaCuusModelField::ContainerPtr() takes, and what is
	 * handed to RmlUi as the bound DataVariable's void*. Null on an empty shadow.
	 */
	void* GetData() { return Data; }
	const void* GetData() const { return Data; }

	/** Destroys the instance and frees the buffer, leaving an empty shadow. */
	void Reset();

private:
	/**
	 * Strong for the same reason FVaCuusModelLayout's is: a native UScriptStruct is
	 * RF_MarkAsNative and never collected, but a UUserDefinedStruct is an ordinary
	 * collectable object -- and DestroyStruct in the destructor needs the type to still
	 * be there.
	 */
	TStrongObjectPtr<const UScriptStruct> Struct;

	/** Malloc'd, aligned to the type's MinAlignment. Owned. */
	uint8* Data = nullptr;
};
