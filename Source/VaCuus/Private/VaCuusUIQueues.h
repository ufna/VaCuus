// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusInputEvent.h"
#include "VaCuusViewStatus.h"

#include "Containers/SpscQueue.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

class FVaCuusBoundModel;
class IVaCuusDocumentHost;
struct FVaCuusStyleSnapshot;
struct FVaCuusTranslationSnapshot;

/** What a queued command asks the UI thread to do. */
enum class EVaCuusCommandKind : uint8
{
	/**
	 * Unset. The default, deliberately: a command that reaches the drain still
	 * carrying it is a producer that forgot to set Kind, and the drain says so
	 * loudly instead of silently performing whichever kind happened to be first in
	 * this enum.
	 */
	None,

	/**
	 * Registers a new view: boots the handed-over document host under ViewId and
	 * adds it to the UI thread's view map. Carries the host and the shared status.
	 */
	AddView,

	/** Retires the view: closes its document, drops its context, keeps the host retired (see below). */
	RemoveView,

	/**
	 * Drops RmlUi's parsed stylesheet/template caches. Carries nothing, and
	 * deliberately carries no ViewId: the caches are process-global statics, so this is
	 * a THREAD-level command and is handled before the drain's per-view routing -- which
	 * is the whole point. A cache clear that rode on a per-view load would be lost
	 * exactly when there is no live view to carry it, and RmlUi's caches outlive a PIE
	 * session (see FVaCuusUIThread::EnqueueClearAssetCaches).
	 */
	ClearAssetCaches,

	/** Payload is a path for the RmlUi file interface (relative to a DevUI root -- see VaCuusContentPaths.h). */
	LoadDocumentFile,

	/** Payload is RML source text, loaded from memory. */
	LoadDocumentMemory,

	/** Closes the view's document; the context stays up. */
	CloseDocument,

	/** Carries only ViewSize; see the note below on coalescing. */
	Resize,

	/**
	 * Creates one data model on the view's context and binds its variables to the model's
	 * UI-side shadow (M3a). Carries the shared FVaCuusBoundModel.
	 *
	 * ROUTED, AND IT MUST REACH THE CONTEXT BEFORE ANY LOAD DOES. `data-model` is read
	 * exactly once, in Element::SetParent (Element.cpp:2202-2219), so a model created after
	 * its document loaded never attaches: an inert document, plus one RmlUi LT_ERROR that
	 * does reach LogVaCuus (see FVaCuusSystemInterface::LogMessage) but describes the
	 * DOCUMENT's failed lookup rather than the ordering mistake behind it. The ordering is
	 * the producer's to get right; the queue is FIFO from a single producer, so a BindModel
	 * enqueued before a LoadDocument* is drained before it.
	 */
	BindModel,

	/**
	 * Prints the UI-thread half of `vacuus.DumpModel` for the model named in Payload, or for
	 * every model of the view when Payload is empty (spec 8).
	 *
	 * HANDLED BEFORE THE PER-VIEW HOST LOOKUP, unlike every other routed kind, and that is the
	 * point rather than an inconsistency: the models live in FVaCuusUIThread::Models keyed on
	 * the view, not in the host, and a diagnostic that answered "unknown view" at Verbose --
	 * which is what the lookup does -- would be the one command in the plugin that can fail
	 * silently. It reports what it found either way.
	 */
	DumpModel,

	/** Shows or hides the view's document per bVisible. */
	SetVisible,

	/**
	 * Evaluates Payload as JS source against the view's context (M4 Task 6),
	 * SourceName naming it in errors and backtraces.
	 *
	 * HANDLED BEFORE THE PER-VIEW HOST LOOKUP, like DumpModel and for a sibling
	 * reason: the target is the SCRIPT host, which keeps its own view registry
	 * (same membership as the document-host map -- OnViewAdded fires exactly when
	 * AddView registers) and refuses an unknown view at Error itself. Losing a
	 * script silently would repeat the BindModel lesson -- nothing downstream
	 * ever misses it -- so both failure modes are loud: the script host's
	 * unknown-view Error, and the drain's own Error when no script host exists
	 * at all (JS disabled, VaCuusJs absent).
	 *
	 * ORDERING FOR FREE: FIFO from the single producer means an ExecuteScript
	 * enqueued after a LoadDocument* runs against the loaded document -- the
	 * same argument BindModel-before-load rests on above.
	 */
	ExecuteScript,

	/**
	 * Installs the material-decorator style snapshot (M5 Task 5b). Carries StyleSnapshot
	 * and, like ClearAssetCaches, deliberately no ViewId: the snapshot is process-wide
	 * state the recorder's CompileShader reads (there is one registry per process, like
	 * the RmlUi library itself), so this is a THREAD-level command handled before the
	 * per-view routing. FIFO from the single producer is the ordering guarantee the
	 * registry documents: a snapshot enqueued before a LoadDocument* is installed before
	 * that document's decorators compile.
	 */
	SetStyleSnapshot,

	/**
	 * Installs the localization snapshot (M5 Task 8, spec §2(l)). SetStyleSnapshot's
	 * shape verbatim: carries TranslationSnapshot, deliberately no ViewId — the table
	 * is process-wide state `vacuus.translate` and TranslateString read (one
	 * SystemInterface per process, like the recorder contract), handled before the
	 * per-view routing, and FIFO from the single producer means a table enqueued
	 * before a LoadDocument* is installed before that document's text instances.
	 */
	SetTranslationSnapshot,

	/** In-band graceful stop: close every document, then leave the frame loop. */
	Shutdown
};

/**
 * One unit of game-thread -> UI-thread work.
 *
 * ROUTING: every per-view command carries the ViewId it applies to, and the UI
 * thread looks that up in its id -> document host map. That is what lets one
 * process-wide UI thread serve N views (N game instances in PIE included) while
 * each view keeps its own Rml::Context. Shutdown and ClearAssetCaches are the
 * THREAD-level kinds: they carry no view and are applied before the lookup, so
 * they still happen when the view map is empty.
 *
 * ViewSize is honoured on every ROUTED kind, not just Resize: a non-zero value is
 * applied to the routed view before the command itself runs, so a document loads
 * straight into the right layout size instead of being laid out twice. The kinds
 * that bypass that -- AddView applies its size at boot instead, RemoveView,
 * ClearAssetCaches and Shutdown ignore it -- are handled before the routing block.
 *
 * Move-only (it can carry a document host), which TSpscQueue's in-place
 * variadic Enqueue and TOptional-returning Dequeue both handle.
 */
struct FVaCuusUICommand
{
	EVaCuusCommandKind Kind = EVaCuusCommandKind::None;

	/** View this command applies to; 0 (and Shutdown) means "the whole thread". */
	uint32 ViewId = 0;

	/** Document path, RML source or JS source, per Kind. Empty for the rest. */
	FString Payload;

	/**
	 * ExecuteScript only: the name errors and backtraces report for Payload.
	 * A second string rather than a packed encoding in Payload, because the
	 * payload is VERBATIM source -- any separator would be a byte some script
	 * legally contains.
	 */
	FString SourceName;

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

	/**
	 * BindModel only: the model both threads share.
	 *
	 * SHARED, NOT HANDED OVER (unlike Host above), because the game thread keeps sampling and
	 * publishing into it for the rest of the view's life. The UI thread takes its own
	 * reference when it drains this, and drops it in RemoveView() -- after the host's
	 * Shutdown() has destroyed the Rml::Context that holds a raw pointer into the model's
	 * shadow.
	 */
	TSharedPtr<FVaCuusBoundModel> Model;

	/**
	 * SetStyleSnapshot only: the immutable style table (publish-by-replacement — the
	 * game thread never mutates a published snapshot, it enqueues a fresh one; const in
	 * the type so the drain cannot either).
	 */
	TSharedPtr<const FVaCuusStyleSnapshot> StyleSnapshot;

	/** SetTranslationSnapshot only: the immutable translation table, same replacement rule as StyleSnapshot above. */
	TSharedPtr<const FVaCuusTranslationSnapshot> TranslationSnapshot;
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
 * Input transport. Same container and same single-producer rule as the command
 * queue: Slate dispatches input on the game thread and nowhere else.
 *
 * ONE SHARED QUEUE, NOT ONE PER VIEW (and the payload carries its ViewId exactly
 * like a command does). Three reasons:
 *
 *  - Order is preserved GLOBALLY, not just within a view. Mouse capture, hover
 *    and focus are all sequences, and with several views (multi-PIE) a per-view
 *    queue would let view B's mouse-down be applied before view A's mouse-up
 *    simply because the drain reached B's queue first.
 *  - The producer stays trivial. A per-view queue means the game thread has to
 *    find the queue for a view before every event, which is a lookup into
 *    structures the UI thread owns -- the very thing this design avoids.
 *  - It matches the command queue, so "dropped because the view is gone" and
 *    "dropped because the thread is stopping" are one rule, in one place, with
 *    one log line, rather than two.
 *
 * The cost is one branch per drained event to route it. A frame's worth of input
 * is single digits, so that is not a cost.
 *
 * NOTE that a separate queue (rather than one queue of a union type) is what makes
 * the ordering *between* commands and input deliberate instead of accidental: the
 * drain applies every command first and only then the input, so an event can never
 * be delivered to a context that is about to be resized or reloaded in the same
 * frame.
 */
using FVaCuusInputQueue = TSpscQueue<FVaCuusInputEvent>;

/**
 * Everything the game thread pushes into the UI thread, in one place so
 * FVaCuusUIThread can hold it opaquely and keep these payload types Private.
 *
 * Resize coalescing falls out of the drain rather than the queue: each drained
 * command's ViewSize simply overwrites the routed view's size, and pushing an
 * unchanged size to the document host is a no-op, so a burst of resizes costs
 * one relayout.
 */
struct FVaCuusUIQueues
{
	FVaCuusCommandQueue Commands;
	FVaCuusInputQueue Input;
};
