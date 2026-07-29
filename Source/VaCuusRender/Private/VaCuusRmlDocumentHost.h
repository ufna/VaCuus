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
 * The RmlUi-backed document host: owns the recording render interface, the Rml
 * context and the current document (RmlUi headers stay in the cpp).
 *
 * THREAD AFFINITY: everything except the constructor runs on the VaCuus UI
 * thread, asserted per method. FVaCuusUIThread::Init() calls Initialize() --
 * which is why RmlUi must still be down at that point, the recorder can only be
 * installed pre-init -- and Exit() calls Shutdown() and destroys the host, so
 * every RmlUi call in the process happens on that one thread.
 *
 * RecordAndPublishFrame() publishes the recorded buffer to the Slate element
 * directly from the UI thread via ENQUEUE_RENDER_COMMAND; there is no
 * game-thread hop anywhere in the frame path.
 */
class FVaCuusRmlDocumentHost final : public IVaCuusDocumentHost
{
public:
	/** Built on the owner's thread and handed to FVaCuusUIThread::SetDocumentHost(). */
	explicit FVaCuusRmlDocumentHost(const TSharedRef<FVaCuusSlateElement>& InElement);

	/** Safety net only; normal teardown runs Shutdown() from the UI thread's Exit(). */
	virtual ~FVaCuusRmlDocumentHost() override;

	FVaCuusRmlDocumentHost(const FVaCuusRmlDocumentHost&) = delete;
	FVaCuusRmlDocumentHost& operator=(const FVaCuusRmlDocumentHost&) = delete;

	//~ Begin IVaCuusDocumentHost
	virtual bool Initialize() override;
	virtual void Shutdown() override;
	virtual void SetViewSize(FIntPoint InViewSize) override;
	virtual void LoadDocumentFromFile(const FString& VfsPath) override;
	virtual void LoadDocumentFromMemory(const FString& RmlSource) override;
	virtual void CloseDocument() override;
	virtual bool HasView() const override;
	virtual void RecordAndPublishFrame() override;
	//~ End IVaCuusDocumentHost

private:
	/** Shared tail of both load paths: adopts and shows the new document, or logs the failure. */
	void AdoptDocument(Rml::ElementDocument* NewDocument, const FString& Description);

	/** Composite element the published buffers are enqueued to (thread-safe SP). */
	TSharedPtr<FVaCuusSlateElement> Element;

	/** Owns the Rml::RenderInterface handed to FVaCuusEngine; must outlive Rml::Shutdown(). */
	TUniquePtr<FVaCuusRecordingRenderInterface> Recorder;

	/** Owned by RmlUi; freed via Rml::RemoveContext in Shutdown(). */
	Rml::Context* Context = nullptr;

	/** Owned by the context; closed in CloseDocument()/Shutdown(). */
	Rml::ElementDocument* Document = nullptr;

	/** Layout/record size in pixels; pushed into the context on change. */
	FIntPoint ViewSize = FIntPoint::ZeroValue;

	/** One-shot thread-attribution log: the evidence that RmlUi left the game thread. */
	bool bLoggedFirstFrame = false;
};
