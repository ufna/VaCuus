// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusRecordingRenderInterface.h"

#include "VaCuusDefines.h"

#include "HAL/PlatformTLS.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/FileInterface.h>

#include <cstddef>
#include <type_traits>

// FVaCuusVertex mirrors Rml::Vertex bit-for-bit so the vertex stream can be
// memcpy'd. Rml::Vertex = { Vector2f position; ColourbPremultiplied colour
// (4 bytes, RGBA order); Vector2f tex_coord }.
static_assert(sizeof(Rml::Vertex) == sizeof(FVaCuusVertex), "FVaCuusVertex must match Rml::Vertex layout");
static_assert(offsetof(Rml::Vertex, position) == offsetof(FVaCuusVertex, Position), "Position offset mismatch");
static_assert(offsetof(Rml::Vertex, colour) == offsetof(FVaCuusVertex, Color), "Color offset mismatch");
static_assert(offsetof(Rml::Vertex, tex_coord) == offsetof(FVaCuusVertex, UV), "UV offset mismatch");

// Index stream is memcpy'd as well.
static_assert(sizeof(int) == sizeof(int32), "Rml index type must be 32-bit");

// Recorder handles round-trip through Rml handles (uintptr_t) unchanged.
static_assert(sizeof(Rml::CompiledGeometryHandle) == sizeof(FVaCuusGeometryHandle), "Rml geometry handle must be 64-bit");
static_assert(sizeof(Rml::TextureHandle) == sizeof(FVaCuusTextureHandle), "Rml texture handle must be 64-bit");

FVaCuusRecordingRenderInterface::~FVaCuusRecordingRenderInterface()
{
	// Tripwire for the drop-on-teardown assumption: an unpublished pending
	// buffer here is expected only for the M1 pattern where RmlUi Release*
	// traffic lands after the last published frame and recorder + replayer
	// are torn down together. Logged (not ensured) because that legitimate
	// path would otherwise trip on every shutdown.
	if (Pending &&
		(Pending->NewGeometry.Num() > 0 || Pending->NewTextures.Num() > 0 ||
			Pending->ReleasedGeometry.Num() > 0 || Pending->ReleasedTextures.Num() > 0))
	{
		UE_LOG(LogVaCuus, Log,
			TEXT("Recorder destroyed with unpublished resource traffic (new: %d geometry, %d textures; released: %d geometry, %d textures) — dropped"),
			Pending->NewGeometry.Num(), Pending->NewTextures.Num(),
			Pending->ReleasedGeometry.Num(), Pending->ReleasedTextures.Num());
	}
}

Rml::CompiledGeometryHandle FVaCuusRecordingRenderInterface::CompileGeometry(Rml::Span<const Rml::Vertex> Vertices, Rml::Span<const int> Indices)
{
	CheckOwnerThread();
	check(Vertices.size() <= size_t(MAX_int32));
	check(Indices.size() <= size_t(MAX_int32));

	const FVaCuusGeometryHandle Handle = NextGeometryHandle++;
	ensureMsgf(Handle != 0, TEXT("Geometry handle counter wrapped to the invalid sentinel"));

	FVaCuusGeometryData& Data = GetPending().NewGeometry.Add(Handle);
	Data.Vertices.SetNumUninitialized(int32(Vertices.size()));
	if (Vertices.size() > 0)
	{
		FMemory::Memcpy(Data.Vertices.GetData(), Vertices.data(), Vertices.size() * sizeof(Rml::Vertex));
	}
	Data.Indices.SetNumUninitialized(int32(Indices.size()));
	if (Indices.size() > 0)
	{
		FMemory::Memcpy(Data.Indices.GetData(), Indices.data(), Indices.size() * sizeof(int32));
	}

	return Rml::CompiledGeometryHandle(Handle);
}

void FVaCuusRecordingRenderInterface::RenderGeometry(Rml::CompiledGeometryHandle Handle, Rml::Vector2f Translation, Rml::TextureHandle Texture)
{
	CheckOwnerThread();
	if (!ensureMsgf(bInFrame, TEXT("RenderGeometry() outside BeginFrame/EndFrameAndPublish; call dropped")))
	{
		return;
	}

	FVaCuusCommand& Command = GetPending().Commands.AddDefaulted_GetRef();
	Command.Type = EVaCuusCommandType::DrawGeometry;
	Command.Geometry = FVaCuusGeometryHandle(Handle);
	Command.Texture = FVaCuusTextureHandle(Texture);
	Command.Translation = FVector2f(Translation.x, Translation.y);
}

void FVaCuusRecordingRenderInterface::ReleaseGeometry(Rml::CompiledGeometryHandle Handle)
{
	CheckOwnerThread();

	// Same-frame create+release keeps both the NewGeometry entry and this
	// released handle: commands recorded before the release may still draw the
	// geometry, so the replayer creates it, plays the buffer, then retires it.
	GetPending().ReleasedGeometry.Add(FVaCuusGeometryHandle(Handle));
}

Rml::TextureHandle FVaCuusRecordingRenderInterface::LoadTexture(Rml::Vector2i& OutDimensions, const Rml::String& Source)
{
	CheckOwnerThread();

	// M1 deviation from spec §5 (async decode with placeholder texture and a
	// generation bump on arrival): decode synchronously on the game thread.
	// Good enough for the render spike; recorded as tech debt.
	Rml::FileInterface* FileInterface = Rml::GetFileInterface();
	if (FileInterface == nullptr)
	{
		return Rml::TextureHandle(0);
	}

	Rml::String FileData;
	if (!FileInterface->LoadFile(Source, FileData) || FileData.empty())
	{
		UE_LOG(LogVaCuus, Warning, TEXT("LoadTexture: failed to read '%hs'"), Source.c_str());
		return Rml::TextureHandle(0);
	}

	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>("ImageWrapper");
	const EImageFormat Format = ImageWrapperModule.DetectImageFormat(FileData.data(), FileData.size());
	if (Format == EImageFormat::Invalid)
	{
		UE_LOG(LogVaCuus, Warning, TEXT("LoadTexture: unrecognized image format in '%hs'"), Source.c_str());
		return Rml::TextureHandle(0);
	}

	const TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(Format);
	TArray<uint8> RawRGBA;
	if (!ImageWrapper.IsValid() || !ImageWrapper->SetCompressed(FileData.data(), FileData.size()) ||
		!ImageWrapper->GetRaw(ERGBFormat::RGBA, 8, RawRGBA))
	{
		UE_LOG(LogVaCuus, Warning, TEXT("LoadTexture: failed to decode '%hs'"), Source.c_str());
		return Rml::TextureHandle(0);
	}

	const FIntPoint Size(ImageWrapper->GetWidth(), ImageWrapper->GetHeight());
	OutDimensions = Rml::Vector2i(Size.X, Size.Y);

	// Alpha is premultiplied here at the decode boundary so ALL recorded
	// texture payloads share one contract: RGBA memory order, premultiplied
	// alpha. GenerateTexture (fonts) already arrives premultiplied per the Rml
	// ColourbPremultiplied contract, and the replayer blends with
	// One/InverseSourceAlpha, which assumes premultiplied sources.
	{
		uint8* Pixel = RawRGBA.GetData();
		uint8* const End = Pixel + RawRGBA.Num();
		for (; Pixel < End; Pixel += 4)
		{
			const uint32 Alpha = Pixel[3];
			if (Alpha < 255)
			{
				// +127 before the divide = round-to-nearest, removes the
				// downward bias a truncating x*a/255 would add to every texel.
				Pixel[0] = uint8((Pixel[0] * Alpha + 127u) / 255u);
				Pixel[1] = uint8((Pixel[1] * Alpha + 127u) / 255u);
				Pixel[2] = uint8((Pixel[2] * Alpha + 127u) / 255u);
			}
		}
	}

	const FVaCuusTextureHandle Handle = NextTextureHandle++;
	ensureMsgf(Handle != 0, TEXT("Texture handle counter wrapped to the invalid sentinel"));

	FVaCuusTextureData& Data = GetPending().NewTextures.Add(Handle);
	Data.Size = Size;
	Data.RGBA = MoveTemp(RawRGBA);

	return Rml::TextureHandle(Handle);
}

Rml::TextureHandle FVaCuusRecordingRenderInterface::GenerateTexture(Rml::Span<const Rml::byte> SourceData, Rml::Vector2i Dimensions)
{
	CheckOwnerThread();
	check(SourceData.size() <= size_t(MAX_int32));

	// Font glyph atlas path: RGBA8 with premultiplied alpha, per Rml contract.
	const FVaCuusTextureHandle Handle = NextTextureHandle++;
	ensureMsgf(Handle != 0, TEXT("Texture handle counter wrapped to the invalid sentinel"));

	FVaCuusTextureData& Data = GetPending().NewTextures.Add(Handle);
	Data.Size = FIntPoint(Dimensions.x, Dimensions.y);
	Data.RGBA.SetNumUninitialized(int32(SourceData.size()));
	if (SourceData.size() > 0)
	{
		FMemory::Memcpy(Data.RGBA.GetData(), SourceData.data(), SourceData.size());
	}

	return Rml::TextureHandle(Handle);
}

void FVaCuusRecordingRenderInterface::ReleaseTexture(Rml::TextureHandle Handle)
{
	CheckOwnerThread();
	GetPending().ReleasedTextures.Add(FVaCuusTextureHandle(Handle));
}

void FVaCuusRecordingRenderInterface::EnableScissorRegion(bool bEnable)
{
	CheckOwnerThread();

	// RmlUi only calls EnableScissorRegion(true) right before handing the rect
	// to SetScissorRegion() (see RenderManager::SetScissorRegion), so the
	// enable edge carries no information of its own: SetScissor implies
	// enabled. Only the disable edge becomes a command.
	if (bEnable)
	{
		return;
	}

	if (!ensureMsgf(bInFrame, TEXT("EnableScissorRegion() outside BeginFrame/EndFrameAndPublish; call dropped")))
	{
		return;
	}

	FVaCuusCommand& Command = GetPending().Commands.AddDefaulted_GetRef();
	Command.Type = EVaCuusCommandType::DisableScissor;
}

void FVaCuusRecordingRenderInterface::SetScissorRegion(Rml::Rectanglei Region)
{
	CheckOwnerThread();
	if (!ensureMsgf(bInFrame, TEXT("SetScissorRegion() outside BeginFrame/EndFrameAndPublish; call dropped")))
	{
		return;
	}

	FVaCuusCommand& Command = GetPending().Commands.AddDefaulted_GetRef();
	Command.Type = EVaCuusCommandType::SetScissor;
	Command.Scissor = FIntRect(Region.Left(), Region.Top(), Region.Right(), Region.Bottom());
}

void FVaCuusRecordingRenderInterface::SetTransform(const Rml::Matrix4f* Transform)
{
	CheckOwnerThread();
	if (!ensureMsgf(bInFrame, TEXT("SetTransform() outside BeginFrame/EndFrameAndPublish; call dropped")))
	{
		return;
	}

	FVaCuusCommand& Command = GetPending().Commands.AddDefaulted_GetRef();
	Command.Type = EVaCuusCommandType::SetTransform;
	if (Transform != nullptr)
	{
		// Rml::Matrix4f is column-major storage with column-vector convention
		// (v' = M * v); FMatrix44f is row-vector convention (v' = v * M). The
		// raw copy lands each Rml column in a UE row — i.e. the transpose —
		// which is exactly the conversion between the two conventions.
		static_assert(std::is_same_v<Rml::Matrix4f, Rml::ColumnMajorMatrix4f>, "Recorder assumes RmlUi's default column-major matrices");
		FMemory::Memcpy(Command.Transform.M, Transform->data(), 16 * sizeof(float));
	}
	// nullptr -> identity, which is FVaCuusCommand's default Transform.
}

void FVaCuusRecordingRenderInterface::BeginFrame(FIntPoint ViewSize)
{
	ensureMsgf(!bInFrame, TEXT("BeginFrame() called twice without EndFrameAndPublish()"));

	// Keep the existing pending buffer: it may already hold resource traffic
	// recorded between frames (see class comment), which belongs to this frame.
	GetPending().ViewSize = ViewSize;
	OwnerThreadId = FPlatformTLS::GetCurrentThreadId();
	bInFrame = true;
}

TUniquePtr<FVaCuusCommandBuffer> FVaCuusRecordingRenderInterface::EndFrameAndPublish()
{
	CheckOwnerThread();
	ensureMsgf(bInFrame, TEXT("EndFrameAndPublish() without a matching BeginFrame()"));
	bInFrame = false;

	TUniquePtr<FVaCuusCommandBuffer> Published = Pending ? MoveTemp(Pending) : MakeUnique<FVaCuusCommandBuffer>();
	Published->Generation = ++Generation;
	return Published;
}

FVaCuusCommandBuffer& FVaCuusRecordingRenderInterface::GetPending()
{
	// Created lazily so out-of-frame resource calls (document teardown,
	// Rml::Shutdown) land in the buffer published with the NEXT frame. A
	// trailing buffer that never gets published is dropped with the recorder;
	// harmless in M1 where the replayer is torn down together with it.
	if (!Pending)
	{
		Pending = MakeUnique<FVaCuusCommandBuffer>();
	}
	return *Pending;
}

void FVaCuusRecordingRenderInterface::CheckOwnerThread() const
{
	// Single-writer contract: while a frame is open, every recorder call must
	// come from the thread that called BeginFrame(). Out-of-frame calls are
	// not pinned (single-writer is still assumed, just not verifiable here).
	ensureMsgf(!bInFrame || FPlatformTLS::GetCurrentThreadId() == OwnerThreadId,
		TEXT("Recorder called from thread %u while the open frame is owned by thread %u"),
		FPlatformTLS::GetCurrentThreadId(), OwnerThreadId);
}
