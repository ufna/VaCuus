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

namespace
{
/**
 * Written once from the game thread (module startup), read from the UI thread and
 * from decode workers. Atomic rather than a plain pointer because the read side
 * is genuinely another thread; the module itself is immortal once loaded, so the
 * pointer never has to be un-cached.
 */
std::atomic<IImageWrapperModule*> GVaCuusImageWrapperModule{nullptr};
} // namespace

IImageWrapperModule* FVaCuusRecordingRenderInterface::CacheImageWrapperModule()
{
	check(IsInGameThread());

	IImageWrapperModule* Module = GVaCuusImageWrapperModule.load(std::memory_order_acquire);
	if (Module != nullptr)
	{
		return Module;
	}

	// LoadModulePtr, not LoadModuleChecked: a missing ImageWrapper must degrade to
	// "images do not load" with one log line, not take the editor down. VaCuusRender
	// declares ImageWrapper as a dependency, so this only fails if something is
	// badly wrong with the install.
	Module = FModuleManager::LoadModulePtr<IImageWrapperModule>("ImageWrapper");
	if (Module == nullptr)
	{
		UE_LOG(LogVaCuus, Error, TEXT("ImageWrapper module unavailable — LoadTexture will refuse every image source"));
		return nullptr;
	}

	GVaCuusImageWrapperModule.store(Module, std::memory_order_release);
	return Module;
}

IImageWrapperModule* FVaCuusRecordingRenderInterface::GetImageWrapperModule()
{
	IImageWrapperModule* Module = GVaCuusImageWrapperModule.load(std::memory_order_acquire);
	if (Module == nullptr && IsInGameThread())
	{
		// Late fill-in: VaCuusRender starts at PostConfigInit, early enough that a
		// first-call-wins cache is worth a game-thread retry. Off the game thread
		// there is nothing to retry — see CacheImageWrapperModule.
		Module = CacheImageWrapperModule();
	}
	return Module;
}

FVaCuusRecordingRenderInterface::~FVaCuusRecordingRenderInterface()
{
	// Abandon before anything else: in-flight decodes hold their own reference to
	// the sink, so this cannot dangle, and DELIBERATELY does not wait — a view
	// going away must not block the UI thread on a half-finished JPEG. A decode
	// that completes after this point either sees the flag and drops its payload,
	// or enqueues into a sink nobody will drain; the last task's reference then
	// frees the sink and the queue destroys what is left in it.
	DecodeSink->bAbandoned.store(true, std::memory_order_release);
	if (InFlightTextures.Num() > 0)
	{
		UE_LOG(LogVaCuus, Log, TEXT("Recorder destroyed with %d image decode(s) in flight — abandoned"), InFlightTextures.Num());
	}

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

	// RmlUi needs the real dimensions to lay the element out on THIS frame, so the
	// size is probed synchronously and only the decode goes to a worker. The handle
	// is minted here and immediately given a 1x1 transparent placeholder, so it
	// resolves in the replayer from the moment it exists (closes VaCuus-akj.6.2).

	// The file bytes are read HERE, on the frame-owning thread: FVaCuusFileInterface
	// (and Rml::GetFileInterface() itself) are not documented thread-safe, and the
	// probe below needs the bytes anyway. The worker gets the bytes, not the path.
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

	IImageWrapperModule* ImageWrapperModule = GetImageWrapperModule();
	if (ImageWrapperModule == nullptr)
	{
		// Already logged once by CacheImageWrapperModule; do not spam per image.
		return Rml::TextureHandle(0);
	}

	const EImageFormat Format = ImageWrapperModule->DetectImageFormat(FileData.data(), FileData.size());
	if (Format == EImageFormat::Invalid)
	{
		UE_LOG(LogVaCuus, Warning, TEXT("LoadTexture: unrecognized image format in '%hs'"), Source.c_str());
		return Rml::TextureHandle(0);
	}

	// Step 11.1, the synchronous dimension probe. Two costs that are NOT free and
	// are not avoidable through IImageWrapper's API:
	//  - SetCompressed memcpy's the whole file (ImageWrapperBase.cpp:104-118, whose
	//    own comment calls the copy "usually unnecessary"), so this is O(filesize),
	//    not O(header).
	//  - 1/2/4-bit paletted/grey PNGs DECODE inside SetCompressed
	//    (PngImageWrapper.cpp:309-329 calls UncompressPNGData), so for that one
	//    class of image the decode still happens on this thread — and then again on
	//    the worker. Rare in UI art, and there is no header-only query to use
	//    instead (IImageWrapperModule offers none).
	TSharedPtr<IImageWrapper> Probe = ImageWrapperModule->CreateImageWrapper(Format);
	if (!Probe.IsValid() || !Probe->SetCompressed(FileData.data(), FileData.size()))
	{
		UE_LOG(LogVaCuus, Warning, TEXT("LoadTexture: failed to parse '%hs'"), Source.c_str());
		return Rml::TextureHandle(0);
	}

	const FIntPoint Size(Probe->GetWidth(), Probe->GetHeight());

	// Dropped as soon as the size is out. The JPEG wrapper retains a libjpeg-turbo
	// decompressor from SetCompressed all the way to Uncompress
	// (JpegImageWrapper.cpp:362), plus its own copy of the compressed file; keeping
	// one alive per pending image for the whole in-flight window is pure overhead.
	// IImageWrapper exposes no Reset(), so the destructor does it
	// (JpegImageWrapper.cpp:91-94 -> Reset -> tjDestroy).
	Probe.Reset();

	if (Size.X <= 0 || Size.Y <= 0)
	{
		UE_LOG(LogVaCuus, Warning, TEXT("LoadTexture: '%hs' reports a degenerate size %dx%d"), Source.c_str(), Size.X, Size.Y);
		return Rml::TextureHandle(0);
	}

	const FVaCuusTextureHandle Handle = NextTextureHandle++;
	ensureMsgf(Handle != 0, TEXT("Texture handle counter wrapped to the invalid sentinel"));

	// Step 11.3, the placeholder. (0,0,0,0) is a valid PREMULTIPLIED transparent
	// texel, so it satisfies FVaCuusTextureData's contract and the replayer's
	// One/InverseSourceAlpha blend leaves the destination untouched. Swapping in the
	// real payload later is a plain re-Add on the replayer's handle map — no
	// FRHITextureReference, which costs an RHIThreadFence per swap, cannot back an
	// SRV, and is on Epic's removal list.
	FVaCuusTextureData& Placeholder = GetPending().NewTextures.Add(Handle);
	Placeholder.Size = FIntPoint(1, 1);
	Placeholder.RGBA.AddZeroed(4);

	InFlightTextures.Add(Handle);

	// The bytes ride into the task as the Rml::String they were read into — that is
	// a std::string byte buffer (Config.h:108), not an RmlUi object, and moving it
	// spares the UI thread another O(filesize) copy. No RmlUi API is touched on the
	// worker. The sink is captured by value, so the task keeps it alive on its own.
	DecodeTasks.Add(UE::Tasks::Launch(UE_SOURCE_LOCATION,
		[Sink = DecodeSink, Module = ImageWrapperModule, Handle, Format, ProbedSize = Size,
			SourcePath = FString(UTF8_TO_TCHAR(Source.c_str())), Bytes = MoveTemp(FileData)]() mutable
		{
			if (Sink->bAbandoned.load(std::memory_order_acquire))
			{
				return; // The recorder is gone; the decode has no destination.
			}

			FVaCuusTextureDecode Result;
			Result.Handle = Handle;
			Result.Source = MoveTemp(SourcePath);

			const TSharedPtr<IImageWrapper> ImageWrapper = Module->CreateImageWrapper(Format);
			TArray<uint8> RawRGBA;
			if (ImageWrapper.IsValid() && ImageWrapper->SetCompressed(Bytes.data(), Bytes.size()) &&
				ImageWrapper->GetRaw(ERGBFormat::RGBA, 8, RawRGBA))
			{
				// Alpha is premultiplied here at the decode boundary so ALL recorded
				// texture payloads share one contract: RGBA memory order,
				// premultiplied alpha. GenerateTexture (fonts) already arrives
				// premultiplied per the Rml ColourbPremultiplied contract, and the
				// replayer blends with One/InverseSourceAlpha, which assumes
				// premultiplied sources.
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

				// The probe already handed ProbedSize to RmlUi, which laid the element
				// out around it; a payload of another size would silently stretch. Two
				// SetCompressed calls over the same bytes cannot legitimately disagree,
				// so a mismatch is a decoder bug and takes the failure path below.
				const FIntPoint DecodedSize(ImageWrapper->GetWidth(), ImageWrapper->GetHeight());
				if (DecodedSize == ProbedSize && RawRGBA.Num() == ProbedSize.X * ProbedSize.Y * 4)
				{
					Result.Data.Size = DecodedSize;
					Result.Data.RGBA = MoveTemp(RawRGBA);
				}
			}
			// else: Result.Data keeps its zero Size, which the drain reads as failure.

			// Enqueued unconditionally, even if the recorder was abandoned between the
			// check above and here: the sink is refcounted, so the write is always
			// safe, and TMpscQueue destroys whatever is still queued when the last
			// reference dies.
			Sink->Completed.Enqueue(MoveTemp(Result));
		}));

	OutDimensions = Rml::Vector2i(Size.X, Size.Y);
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
	const FVaCuusTextureHandle Local = FVaCuusTextureHandle(Handle);

	// RELEASE-BEFORE-ARRIVAL. The decode cannot be recalled, but forgetting the
	// handle here makes DrainCompletedDecodes drop its payload. Without this, a late
	// payload would land in a LATER buffer's NewTextures and the replayer would
	// create an RHI texture whose only ReleasedTextures entry it already consumed —
	// a leak that lives until the whole replayer is torn down.
	InFlightTextures.Remove(Local);

	GetPending().ReleasedTextures.Add(Local);
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

	// Top of the frame, before any RmlUi call: a payload installed here is part of
	// this frame's resource delta, so it publishes with this frame.
	DrainCompletedDecodes();
}

void FVaCuusRecordingRenderInterface::DrainCompletedDecodes()
{
	// Arrivals go through the command buffer like every other resource, NOT straight
	// into the replayer's texture map. The replayer is reached only from the render
	// thread and only through published buffers, so an in-band arrival needs no
	// weak-pointer dance against teardown — and, decisively, a non-empty NewTextures
	// is the signal the idle short-circuit already watches for, so a texture that
	// lands while the UI is otherwise static still forces a re-replay instead of
	// leaving the placeholder on screen.
	//
	// NOTE THE ASYMMETRY, it is deliberate: the DECODE is async, the UPLOAD is not.
	// The replayer still does its UpdateTexture2D memcpy on the render thread when
	// this payload reaches it. Decode is the expensive half and the one that used to
	// hitch the UI thread; async upload is filed separately.
	while (TOptional<FVaCuusTextureDecode> Completed = DecodeSink->Completed.Dequeue())
	{
		// Zero here means ReleaseTexture() already retired the handle while the
		// decode was running. Installing the payload now would leak; drop it.
		if (InFlightTextures.Remove(Completed->Handle) == 0)
		{
			continue;
		}

		if (Completed->Data.Size.X <= 0 || Completed->Data.Size.Y <= 0)
		{
			// A file that read and parsed but would not decode. The handle KEEPS its
			// transparent placeholder rather than being retired here: RmlUi still owns
			// it and will keep drawing with it, and a handle the replayer has dropped
			// trips its "Draw references unknown texture handle" ensure on every
			// subsequent frame. Nothing leaks — RmlUi's own ReleaseTexture retires the
			// 1x1 when the source goes away. One line per failed image, so a broken
			// asset is visible in the log without spamming per frame.
			UE_LOG(LogVaCuus, Warning, TEXT("LoadTexture: async decode of '%s' failed; texture %llu stays transparent"),
				*Completed->Source, Completed->Handle);
			continue;
		}

		GetPending().NewTextures.Add(Completed->Handle, MoveTemp(Completed->Data));
	}

	DecodeTasks.RemoveAllSwap([](const UE::Tasks::FTask& Task) { return Task.IsCompleted(); }, EAllowShrinking::No);
}

bool FVaCuusRecordingRenderInterface::WaitForTextureDecodes(FTimespan Timeout)
{
	return UE::Tasks::Wait(DecodeTasks, Timeout);
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
	//
	// DELIBERATELY an ensure, where every other M2 thread-affinity guard is a hard
	// check(): those guard RmlUi itself, whose global state a wrong-thread call
	// silently corrupts, so failing fast is the only safe answer. This one guards
	// OUR buffer, and the worst case is a mis-recorded frame — recoverable, and the
	// recorder is deliberately thread-agnostic (tests drive it straight from the
	// test thread). An ensure reports the misuse once and keeps the editor session
	// alive, which is the right trade here and nowhere else.
	ensureMsgf(!bInFrame || FPlatformTLS::GetCurrentThreadId() == OwnerThreadId,
		TEXT("Recorder called from thread %u while the open frame is owned by thread %u"),
		FPlatformTLS::GetCurrentThreadId(), OwnerThreadId);
}
