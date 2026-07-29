// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FVaCuusRecordingRenderInterface;
class FVaCuusSlateElement;

namespace Rml
{
class Context;
class ElementDocument;
}

/**
 * Game-thread owner of the M1 HUD's RmlUi side: the recording render
 * interface, the Rml context and the test document (RmlUi headers stay in the
 * cpp). Boot() installs the recorder into FVaCuusEngine — which is why it must
 * run while RmlUi is still down — DrawFrame() records one UI frame and ships
 * the buffer to the Slate element, Shutdown() tears down in spec §4 order.
 *
 * M1 scope: everything runs on the game thread; the dedicated UI thread is M2.
 */
class FVaCuusM1Harness
{
public:
	explicit FVaCuusM1Harness(const TSharedRef<FVaCuusSlateElement>& InElement);

	/** Calls Shutdown() as a safety net; normal teardown runs it explicitly. */
	~FVaCuusM1Harness();

	FVaCuusM1Harness(const FVaCuusM1Harness&) = delete;
	FVaCuusM1Harness& operator=(const FVaCuusM1Harness&) = delete;

	/**
	 * Installs the recorder as the RmlUi render interface, boots the library,
	 * creates the context and loads DocumentRml from memory. Fails (false,
	 * fully rolled back) if RmlUi is already initialized — the recorder can
	 * only be installed pre-init.
	 */
	bool Boot(FIntPoint InitialViewSize, const FString& DocumentRml);

	/**
	 * Records one UI frame at ViewSize (BeginFrame -> Update -> Render ->
	 * EndFrameAndPublish) and enqueues the published buffer to the element.
	 * Also services the vacuus.M1HUD.AutoShot debug screenshot.
	 */
	void DrawFrame(FIntPoint ViewSize);

	/**
	 * Idempotent teardown, spec §4 order: close document -> RemoveContext ->
	 * engine Shutdown (RmlUi releases resources through the recorder) -> drop
	 * the recorder. The Slate element's render-thread release is the console
	 * command's job, not the harness's.
	 */
	void Shutdown();

private:
	/** Composite element the published buffers are enqueued to (thread-safe SP). */
	TSharedPtr<FVaCuusSlateElement> Element;

	/** Owns the Rml::RenderInterface handed to FVaCuusEngine; must outlive Rml::Shutdown(). */
	TUniquePtr<FVaCuusRecordingRenderInterface> Recorder;

	/** Owned by RmlUi; freed via Rml::RemoveContext in Shutdown(). */
	Rml::Context* Context = nullptr;

	/** Owned by the context; closed in Shutdown(). */
	Rml::ElementDocument* Document = nullptr;

	/** Frames drawn since boot; drives vacuus.M1HUD.AutoShot. */
	int32 FrameCount = 0;
	bool bAutoShotDone = false;
};
