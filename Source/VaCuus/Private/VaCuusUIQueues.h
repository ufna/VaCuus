// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Containers/SpscQueue.h"

/** What a queued command asks the UI thread to do. */
enum class EVaCuusCommandKind : uint8
{
	/** Payload is a path for the RmlUi file interface (relative to <Project>/Content/DevUI). */
	LoadDocumentFile,

	/** Payload is RML source text, loaded from memory. */
	LoadDocumentMemory,

	/** Closes the current document; the context stays up. */
	CloseDocument,

	/** Carries only ViewSize; see the note below on coalescing. */
	Resize,

	/** In-band graceful stop: close the document, then leave the frame loop. */
	Shutdown
};

/**
 * One unit of game-thread -> UI-thread work.
 *
 * ViewSize is honoured on EVERY kind, not just Resize: a non-zero value is
 * applied before the command itself runs, so a document loads straight into the
 * right layout size instead of being laid out twice.
 */
struct FVaCuusUICommand
{
	EVaCuusCommandKind Kind = EVaCuusCommandKind::Resize;

	/** Document path or RML source, per Kind. Empty for the rest. */
	FString Payload;

	/** View size in pixels; ZeroValue means "leave the current size alone". */
	FIntPoint ViewSize = FIntPoint::ZeroValue;
};

/**
 * Command transport. TSpscQueue (not TQueue/TCircularQueue: both carry
 * "planned for deprecation in favor of TSpscQueue" warnings in 5.8, and
 * TCircularQueue memzeroes its storage so it cannot hold an FString).
 *
 * SPSC means exactly ONE producer thread. That producer is the game thread --
 * the console command, the widget's Tick and (Task 4) the subsystem all run
 * there. Anything else pushing commands needs TMpscQueue instead.
 */
using FVaCuusCommandQueue = TSpscQueue<FVaCuusUICommand>;

/**
 * Everything the game thread pushes into the UI thread, in one place so
 * FVaCuusUIThread can hold it opaquely and keep these payload types Private.
 * Task 6 adds the input-event queue here.
 *
 * Resize coalescing falls out of the drain rather than the queue: each drained
 * command's ViewSize simply overwrites the view size, and pushing an unchanged
 * size to the document host is a no-op, so a burst of resizes costs one relayout.
 */
struct FVaCuusUIQueues
{
	FVaCuusCommandQueue Commands;
};
