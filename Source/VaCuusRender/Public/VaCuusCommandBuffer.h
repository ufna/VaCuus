// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Dense 1-based ids minted by the recorder; 0 = invalid/none.
 * Handles are shared with RmlUi verbatim: the recorder returns them as
 * Rml::CompiledGeometryHandle / Rml::TextureHandle (uintptr_t, 64-bit on all
 * target platforms) and gets them back in draw and release calls, so the
 * full 64-bit range round-trips without truncation.
 */
using FVaCuusGeometryHandle = uint64;
using FVaCuusTextureHandle = uint64;

enum class EVaCuusCommandType : uint8
{
	DrawGeometry,
	SetScissor,
	DisableScissor,
	SetTransform
};

/** One recorded RmlUi render call. Fields beyond Type are per-command payload. */
struct FVaCuusCommand
{
	EVaCuusCommandType Type = EVaCuusCommandType::DrawGeometry;

	/** DrawGeometry: geometry created in this or an earlier buffer. */
	FVaCuusGeometryHandle Geometry = 0;

	/** DrawGeometry: texture to sample; 0 = untextured. */
	FVaCuusTextureHandle Texture = 0;

	/** DrawGeometry: pixel-space translation applied to the geometry. */
	FVector2f Translation = FVector2f::ZeroVector;

	/** SetScissor: clip rect in window coordinates (unaffected by SetTransform). */
	FIntRect Scissor = FIntRect(0, 0, 0, 0);

	/** SetTransform: vertex transform in UE row-vector convention (v' = v * M). */
	FMatrix44f Transform = FMatrix44f::Identity;
};

/**
 * Bit-identical mirror of Rml::Vertex (Vector2f position, ColourbPremultiplied
 * colour, Vector2f tex_coord); the recorder memcpy's the vertex stream.
 *
 * Color keeps RmlUi's in-memory byte order: R,G,B,A with premultiplied alpha.
 * On little-endian platforms FColor's named channels read those bytes as
 * B,G,R,A, so R and B appear swapped through the FColor API — the replayer
 * must upload as RGBA (e.g. PF_R8G8B8A8) or swizzle at upload time.
 */
struct FVaCuusVertex
{
	FVector2f Position;
	FColor Color;
	FVector2f UV;
};

struct FVaCuusGeometryData
{
	TArray<FVaCuusVertex> Vertices;
	TArray<int32> Indices;
};

/**
 * Raw RGBA8 pixels, RmlUi byte order, ALWAYS premultiplied alpha: generated
 * textures (fonts) per the Rml contract, loaded images premultiplied at decode
 * by the recorder. Upload as PF_R8G8B8A8 and blend One/InverseSourceAlpha.
 *
 * A 1x1 (0,0,0,0) payload is the async-load placeholder: LoadTexture returns the
 * real dimensions immediately but the decode runs on a worker, so the handle
 * carries one premultiplied-transparent texel until the payload arrives. That
 * makes an unfinished image draw INVISIBLE rather than missing, which is why the
 * placeholder is a real entry and not an absent one.
 */
struct FVaCuusTextureData
{
	FIntPoint Size = FIntPoint::ZeroValue;
	TArray<uint8> RGBA;
};

/**
 * One recorded UI frame: the command list plus the resource delta since the
 * previous buffer. Produced on the game thread by FVaCuusRecordingRenderInterface,
 * consumed by the render-thread replayer.
 *
 * A handle may appear in both NewGeometry/NewTextures and the Released* arrays
 * of the same buffer (created and released within one frame): the replayer
 * must create the resource, play the commands, then retire it.
 */
struct FVaCuusCommandBuffer
{
	/** Strictly increasing publish counter; newer buffers replace older ones. */
	uint64 Generation = 0;

	/** View size the frame was laid out for, in pixels. */
	FIntPoint ViewSize = FIntPoint::ZeroValue;

	TArray<FVaCuusCommand> Commands;

	/**
	 * Resources first seen this frame, keyed by handle.
	 *
	 * NewTextures is also the arrival channel for a finished async image decode,
	 * so ONE texture handle may appear in NewTextures of two different buffers:
	 * the placeholder in the buffer that ran LoadTexture, the real payload in a
	 * later one. The replayer's TMap<Handle, FTextureRHIRef>::Add on an existing
	 * key destroys the old value before relocating the new one in
	 * (Containers/SetUtilities.h:98, MoveByRelocate), so re-adding IS the swap:
	 * the placeholder's RHI ref is released and no handle is orphaned.
	 */
	TMap<FVaCuusGeometryHandle, FVaCuusGeometryData> NewGeometry;
	TMap<FVaCuusTextureHandle, FVaCuusTextureData> NewTextures;

	/** Handles to release AFTER this buffer retires (commands above may still use them). */
	TArray<FVaCuusGeometryHandle> ReleasedGeometry;
	TArray<FVaCuusTextureHandle> ReleasedTextures;
};
