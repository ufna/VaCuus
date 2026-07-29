// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusCommandBuffer.h"

#include <RmlUi/Core/RenderInterface.h>

/**
 * Rml::RenderInterface implementation that records the frame into an
 * FVaCuusCommandBuffer instead of touching any RHI. Thread-agnostic
 * single-writer contract: any one thread may drive the recorder
 * (BeginFrame() / Context::Render() / EndFrameAndPublish()), but calls must
 * never race. While a frame is open, the recorder is pinned to the thread
 * that called BeginFrame() — enforced with an ensure. Published buffers may
 * be consumed on any thread.
 *
 * Resource calls (Compile/Generate/Load/Release*) are legal outside a
 * Begin/End pair: RmlUi releases geometry and textures during document
 * teardown and Rml::Shutdown(). Such traffic accumulates in the pending
 * buffer and rides along with the next published frame. Draw-state calls
 * (RenderGeometry, scissor, transform) outside a frame are a caller bug:
 * ensureMsgf + drop.
 */
class VACUUSRENDER_API FVaCuusRecordingRenderInterface : public Rml::RenderInterface
{
public:
	virtual ~FVaCuusRecordingRenderInterface();

	//~ Begin Rml::RenderInterface
	virtual Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> Vertices, Rml::Span<const int> Indices) override;
	virtual void RenderGeometry(Rml::CompiledGeometryHandle Handle, Rml::Vector2f Translation, Rml::TextureHandle Texture) override;
	virtual void ReleaseGeometry(Rml::CompiledGeometryHandle Handle) override;
	virtual Rml::TextureHandle LoadTexture(Rml::Vector2i& OutDimensions, const Rml::String& Source) override;
	virtual Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> SourceData, Rml::Vector2i Dimensions) override;
	virtual void ReleaseTexture(Rml::TextureHandle Handle) override;
	virtual void EnableScissorRegion(bool bEnable) override;
	virtual void SetScissorRegion(Rml::Rectanglei Region) override;
	virtual void SetTransform(const Rml::Matrix4f* Transform) override;
	//~ End Rml::RenderInterface

	/** Opens a frame: subsequent draw-state calls are recorded into the pending buffer. */
	void BeginFrame(FIntPoint ViewSize);

	/**
	 * Closes the frame and hands out the pending buffer with a strictly
	 * increasing Generation. The next frame starts from a fresh buffer,
	 * though out-of-frame resource traffic arriving before the next
	 * BeginFrame() may pre-populate it (see class comment).
	 */
	TUniquePtr<FVaCuusCommandBuffer> EndFrameAndPublish();

private:
	/** Lazily creates the pending buffer; see class comment for out-of-frame semantics. */
	FVaCuusCommandBuffer& GetPending();

	/** Ensures the caller is the frame-owning thread while a frame is open. */
	void CheckOwnerThread() const;

	uint64 NextGeometryHandle = 1;
	uint64 NextTextureHandle = 1;
	uint64 Generation = 0;
	bool bInFrame = false;

	/** Thread that called BeginFrame(); only meaningful while bInFrame. */
	uint32 OwnerThreadId = 0;

	TUniquePtr<FVaCuusCommandBuffer> Pending;
};
