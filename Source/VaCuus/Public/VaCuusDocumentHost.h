// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Templates/SharedPointer.h"

struct FVaCuusViewStatus;

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

	/** Closes the current document, if any. The host stays initialized. */
	virtual void CloseDocument() = 0;

	/**
	 * Shows or hides the current document. A hidden view keeps producing frames
	 * (they are simply empty), which is what clears it off the screen -- skipping
	 * the frame instead would leave the last published content in the view's RT.
	 */
	virtual void SetVisible(bool bVisible) = 0;

	/** True when a frame can be produced: a document is loaded and the view size is valid. */
	virtual bool HasView() const = 0;

	/**
	 * Records one frame (update layout, record draw commands) and publishes it.
	 * Only called while HasView() is true.
	 */
	virtual void RecordAndPublishFrame() = 0;
};
