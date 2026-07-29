// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusDocumentHost.h"

class FVaCuusRecordingRenderInterface;
class FVaCuusSlateElement;

namespace Rml
{
class Context;
class ElementDocument;
}

/**
 * The RmlUi-backed document host for ONE view: owns that view's recording render
 * interface, its Rml context and its current document (RmlUi headers stay in the
 * cpp).
 *
 * ONE CONTEXT, ONE RECORDER, ONE ELEMENT: Rml::CreateContext() takes a per-context
 * render interface and RmlUi gives each distinct interface its own RenderManager,
 * so N hosts on the shared UI thread record N independent command buffers and
 * publish each to its own Slate element. That is what makes several views (e.g.
 * one per PIE client) work on a single UI thread.
 *
 * THREAD AFFINITY: everything except the constructor runs on the VaCuus UI
 * thread, asserted per method. The UI thread calls Initialize() when it drains the
 * AddView command and Shutdown() on RemoveView or in Exit(), so every RmlUi call
 * in the process happens on that one thread. The host does NOT boot or shut down
 * RmlUi itself -- the UI thread does that once for the process.
 *
 * RecordAndPublishFrame() publishes the recorded buffer to the Slate element
 * directly from the UI thread via ENQUEUE_RENDER_COMMAND; there is no
 * game-thread hop anywhere in the frame path.
 */
class FVaCuusRmlDocumentHost final : public IVaCuusDocumentHost
{
public:
	/** Built on the owner's thread and handed to UVaCuusSubsystem::CreateView(). */
	explicit FVaCuusRmlDocumentHost(const TSharedRef<FVaCuusSlateElement>& InElement);

	/** Safety net only; normal teardown runs Shutdown() from the UI thread. */
	virtual ~FVaCuusRmlDocumentHost() override;

	FVaCuusRmlDocumentHost(const FVaCuusRmlDocumentHost&) = delete;
	FVaCuusRmlDocumentHost& operator=(const FVaCuusRmlDocumentHost&) = delete;

	//~ Begin IVaCuusDocumentHost
	virtual bool Initialize(uint32 InViewId, const TSharedRef<FVaCuusViewStatus>& InStatus) override;
	virtual void Shutdown() override;
	virtual void SetViewSize(FIntPoint InViewSize) override;
	virtual void LoadDocumentFromFile(const FString& VfsPath, uint64 LoadSerial) override;
	virtual void LoadDocumentFromMemory(const FString& RmlSource, uint64 LoadSerial) override;
	virtual void CloseDocument() override;
	virtual void SetVisible(bool bVisible) override;
	virtual bool HasView() const override;
	virtual void RecordAndPublishFrame() override;
	//~ End IVaCuusDocumentHost

private:
	/** Shared tail of both load paths: adopts and shows the new document, or logs the failure. */
	void AdoptDocument(Rml::ElementDocument* NewDocument, const FString& Description, uint64 LoadSerial);

	/** Publishes the outcome of a load back to the game thread (see FVaCuusViewStatus). */
	void ReportLoadResult(uint64 LoadSerial, bool bSuccess);

	/** Composite element the published buffers are enqueued to (thread-safe SP). Dropped by Shutdown(). */
	TSharedPtr<FVaCuusSlateElement> Element;

	/**
	 * This view's Rml::RenderInterface, handed to Rml::CreateContext().
	 *
	 * DELIBERATELY OUTLIVES Shutdown(): RmlUi keys a RenderManager on this pointer
	 * and only destroys it -- releasing this view's font textures through it -- in
	 * Rml::Shutdown(). The UI thread therefore keeps a retired host alive until the
	 * library is down (FVaCuusUIThread::RetiredHosts).
	 */
	TUniquePtr<FVaCuusRecordingRenderInterface> Recorder;

	/** Shared with UVaCuusView; how load results and frame counts reach the game thread. */
	TSharedPtr<FVaCuusViewStatus> Status;

	/** Owned by RmlUi; freed via Rml::RemoveContext in Shutdown(). */
	Rml::Context* Context = nullptr;

	/** Owned by the context; closed in CloseDocument()/Shutdown(). */
	Rml::ElementDocument* Document = nullptr;

	/** Layout/record size in pixels; pushed into the context on change. */
	FIntPoint ViewSize = FIntPoint::ZeroValue;

	/** Context name, derived from the view id: RmlUi keys its context registry by name. */
	FString ContextName;

	/** Routing/diagnostics only; the UI thread's map is the real registry. */
	uint32 ViewId = 0;

	/** One-shot thread-attribution log: the evidence that RmlUi left the game thread. */
	bool bLoggedFirstFrame = false;
};
