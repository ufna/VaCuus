// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * The UI thread's view of "the thing that owns an RmlUi document".
 *
 * WHY AN INTERFACE: the concrete host needs the recording render interface and
 * the Slate element, which live in VaCuusRender, and VaCuusRender already
 * depends on VaCuus -- a direct member would close the module cycle. So VaCuus
 * declares the contract and VaCuusRender implements it
 * (FVaCuusRmlDocumentHost). The seam is deliberate: it is also where a
 * non-RmlUi or headless host can plug in.
 *
 * THREAD AFFINITY: everything below Initialize() runs ON THE VaCuus UI THREAD
 * and nowhere else -- FVaCuusUIThread::Init() boots the host and Exit()
 * shuts it down and destroys it, so RmlUi is only ever touched from that one
 * thread. Implementations assert this with
 * check(FVaCuusUIThread::IsInUIThread()). Construction is the single
 * exception: the owner builds the host on its own thread and hands it over
 * before Start().
 */
class IVaCuusDocumentHost
{
public:
	virtual ~IVaCuusDocumentHost() = default;

	/**
	 * Boots whatever the host needs (RmlUi, the render interface, the context).
	 * Must fully roll back and return false on failure: the UI thread's Exit()
	 * -- and therefore Shutdown() -- does NOT run when Init() fails.
	 */
	virtual bool Initialize() = 0;

	/** Idempotent teardown of everything Initialize() built. */
	virtual void Shutdown() = 0;

	/** Sets the layout/record size in pixels. Idempotent; an unchanged size costs nothing. */
	virtual void SetViewSize(FIntPoint ViewSize) = 0;

	/** Replaces the current document with one loaded through the file interface. */
	virtual void LoadDocumentFromFile(const FString& VfsPath) = 0;

	/** Replaces the current document with one parsed from RML source text. */
	virtual void LoadDocumentFromMemory(const FString& RmlSource) = 0;

	/** Closes the current document, if any. The host stays initialized. */
	virtual void CloseDocument() = 0;

	/** True when a frame can be produced: a document is loaded and the view size is valid. */
	virtual bool HasView() const = 0;

	/**
	 * Records one frame (update layout, record draw commands) and publishes it.
	 * Only called while HasView() is true.
	 */
	virtual void RecordAndPublishFrame() = 0;
};
