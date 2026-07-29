// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusViewStatus.h"

#include "Containers/SpscQueue.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

class IVaCuusDocumentHost;

/** What a queued command asks the UI thread to do. */
enum class EVaCuusCommandKind : uint8
{
	/**
	 * Registers a new view: boots the handed-over document host under ViewId and
	 * adds it to the UI thread's view map. Carries the host and the shared status.
	 */
	AddView,

	/** Retires the view: closes its document, drops its context, keeps the host retired (see below). */
	RemoveView,

	/** Payload is a path for the RmlUi file interface (relative to <Project>/Content/DevUI). */
	LoadDocumentFile,

	/** Payload is RML source text, loaded from memory. */
	LoadDocumentMemory,

	/** Closes the view's document; the context stays up. */
	CloseDocument,

	/** Carries only ViewSize; see the note below on coalescing. */
	Resize,

	/** Shows or hides the view's document per bVisible. */
	SetVisible,

	/** In-band graceful stop: close every document, then leave the frame loop. */
	Shutdown
};

/**
 * One unit of game-thread -> UI-thread work.
 *
 * ROUTING: every command except Shutdown carries the ViewId it applies to, and
 * the UI thread looks that up in its id -> document host map. That is what lets
 * one process-wide UI thread serve N views (N game instances in PIE included)
 * while each view keeps its own Rml::Context.
 *
 * ViewSize is honoured on EVERY kind, not just Resize: a non-zero value is
 * applied to the routed view before the command itself runs, so a document loads
 * straight into the right layout size instead of being laid out twice.
 *
 * Move-only (it can carry a document host), which TSpscQueue's in-place
 * variadic Enqueue and TOptional-returning Dequeue both handle.
 */
struct FVaCuusUICommand
{
	EVaCuusCommandKind Kind = EVaCuusCommandKind::Resize;

	/** View this command applies to; 0 (and Shutdown) means "the whole thread". */
	uint32 ViewId = 0;

	/** Document path or RML source, per Kind. Empty for the rest. */
	FString Payload;

	/** View size in pixels; ZeroValue means "leave the current size alone". */
	FIntPoint ViewSize = FIntPoint::ZeroValue;

	/** SetVisible only. */
	bool bVisible = true;

	/** Load kinds only: echoed into the status when the load completes. */
	uint64 LoadSerial = 0;

	/** AddView only: the view's document host, handed over to the UI thread. */
	TUniquePtr<IVaCuusDocumentHost> Host;

	/** AddView only: the status object the host reports load results through. */
	TSharedPtr<FVaCuusViewStatus> Status;
};

/**
 * Command transport. TSpscQueue (not TQueue/TCircularQueue: both carry
 * "planned for deprecation in favor of TSpscQueue" warnings in 5.8, and
 * TCircularQueue memzeroes its storage so it cannot hold an FString).
 *
 * SPSC means exactly ONE producer thread. That producer is the game thread --
 * the subsystem, the views, the widget's Tick and the console command all run
 * there. Anything else pushing commands needs TMpscQueue instead.
 */
using FVaCuusCommandQueue = TSpscQueue<FVaCuusUICommand>;

/**
 * Everything the game thread pushes into the UI thread, in one place so
 * FVaCuusUIThread can hold it opaquely and keep these payload types Private.
 * Task 6 adds the input-event queue here.
 *
 * Resize coalescing falls out of the drain rather than the queue: each drained
 * command's ViewSize simply overwrites the routed view's size, and pushing an
 * unchanged size to the document host is a no-op, so a burst of resizes costs
 * one relayout.
 */
struct FVaCuusUIQueues
{
	FVaCuusCommandQueue Commands;
};
