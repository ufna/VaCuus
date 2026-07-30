// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Templates/SharedPointer.h"

struct FVaCuusViewStatus;

namespace Rml
{
class Context;
}

/**
 * The UI thread's view of "the thing that owns ONE view's RmlUi context and
 * document". One host instance per UVaCuusView; the process-wide UI thread owns
 * the id -> host map and routes commands into it.
 *
 * WHY AN INTERFACE: the concrete host needs the recording render interface and
 * the Slate element, which live in VaCuusRender, and VaCuusRender already
 * depends on VaCuus -- a direct member would close the module cycle. So VaCuus
 * declares the contract and VaCuusRender implements it
 * (FVaCuusRmlDocumentHost). The seam is deliberate: it is also where a
 * non-RmlUi or headless host can plug in (the multi-view automation test does
 * exactly that).
 *
 * THREAD AFFINITY: everything from Initialize() onwards runs ON THE VaCuus UI
 * THREAD and nowhere else -- the UI thread boots the host when it drains the
 * AddView command and shuts it down on RemoveView or in Exit(), so RmlUi is only
 * ever touched from that one thread. Implementations assert this with
 * check(FVaCuusUIThread::IsInUIThread()). Construction is the single exception:
 * the owner builds the host on its own thread and hands it over with AddView.
 *
 * LIBRARY LIFETIME: the host does NOT boot or shut down RmlUi -- the UI thread
 * does that once for the process (FVaCuusEngine is a process-wide singleton
 * because RmlUi's interfaces, its `initialised` flag and its context registry
 * are process-global statics). A host only creates and destroys its own context.
 */
class IVaCuusDocumentHost
{
public:
	virtual ~IVaCuusDocumentHost() = default;

	/**
	 * Boots this view's RmlUi state (render interface, context) under InViewId.
	 * RmlUi is already initialized when this is called. InStatus is how load
	 * results and published-frame counts get back to the game thread.
	 *
	 * Must fully roll back and return false on failure: the UI thread drops a
	 * host whose Initialize() failed without calling Shutdown().
	 */
	virtual bool Initialize(uint32 InViewId, const TSharedRef<FVaCuusViewStatus>& InStatus) = 0;

	/**
	 * Idempotent teardown of everything Initialize() built, EXCEPT anything
	 * RmlUi's global state still points at -- for the RmlUi host that is the
	 * per-view render interface, because Rml::Shutdown() destroys the
	 * RenderManager it keyed on that pointer and releases resources through it.
	 * The UI thread therefore keeps a shut-down host alive (retired) and only
	 * destroys it after RmlUi itself is down.
	 */
	virtual void Shutdown() = 0;

	/** Sets the layout/record size in pixels. Idempotent; an unchanged size costs nothing. */
	virtual void SetViewSize(FIntPoint ViewSize) = 0;

	/** Replaces the current document with one loaded through the file interface. */
	virtual void LoadDocumentFromFile(const FString& VfsPath, uint64 LoadSerial) = 0;

	/** Replaces the current document with one parsed from RML source text. */
	virtual void LoadDocumentFromMemory(const FString& RmlSource, uint64 LoadSerial) = 0;

	/**
	 * Closes the current document, if any. The host stays initialized.
	 *
	 * A HOST THAT OWNS A RENDER TARGET OWES ONE MORE FRAME AFTER THIS. Since the M2 Task
	 * 12 idle gate the render target is the only copy of an idle UI's pixels -- withheld
	 * frames are never resent, and the Slate element composites the RT whether or not a
	 * buffer arrived -- so a host that let HasView() go false here would leave the closed
	 * document composited forever. FVaCuusRmlDocumentHost keeps HasView() true for exactly
	 * one post-close frame; the empty frame it records is what clears the RT, and its
	 * Update() is also what makes RmlUi actually free the document. A host with no render
	 * target of its own (the multi-view test's probe) owes nothing.
	 */
	virtual void CloseDocument() = 0;

	/**
	 * Shows or hides the current document.
	 *
	 * A hidden view keeps RECORDING frames, which is what clears it off the screen --
	 * skipping the frame instead would leave the last visible content in the view's RT,
	 * because nothing would ever emit the empty frame that clears it. It does NOT keep
	 * publishing them: the first hidden frame records an empty command list and publishes
	 * it, and every hidden frame after that hashes equal and is withheld by the idle gate.
	 * That is safe precisely because the RT was already cleared by the frame before. (This
	 * sentence used to claim the empty frames keep publishing; that has been false since M2
	 * Task 12 -- see FVaCuusRmlDocumentHost::SetVisible.)
	 */
	virtual void SetVisible(bool bVisible) = 0;

	/**
	 * True when a frame must be produced this tick: normally "a document is loaded and the
	 * view size is valid", plus the one post-close frame CloseDocument() owes.
	 */
	virtual bool HasView() const = 0;

	/**
	 * This view's RmlUi context, or null before Initialize() / after Shutdown().
	 *
	 * WHY THE HOST HANDS ITS CONTEXT OUT: input dispatch lives on the UI thread, in
	 * VaCuus, next to the FKey/modifier/button/wheel translation it needs
	 * (VaCuusInputMap) -- so the whole RmlUi input vocabulary stays in one module and
	 * SVaCuusWidget never includes an RmlUi header. The alternative, a
	 * ProcessInput(...) method here, would push that translation into whichever
	 * module implements the host. It is the mirror image of the snapshot, which the
	 * host already builds by handing its context to VaCuus code
	 * (BuildVaCuusInteractiveSnapshot).
	 *
	 * UI THREAD ONLY, and the pointer must not be stored: it dies with Shutdown().
	 */
	virtual Rml::Context* GetContext() const = 0;

	/**
	 * Records one frame (update layout, record draw commands) and publishes it.
	 * Only called while HasView() is true.
	 */
	virtual void RecordAndPublishFrame() = 0;
};
