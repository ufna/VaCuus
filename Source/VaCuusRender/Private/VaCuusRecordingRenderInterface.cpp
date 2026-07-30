// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusRecordingRenderInterface.h"

#include "VaCuusDefines.h"

#include "HAL/PlatformTLS.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

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
	// A plain load, DELIBERATELY with no retry. CacheImageWrapperModule() runs from
	// FVaCuusRenderModule::StartupModule (VaCuusRender.cpp:1018) and VaCuusRender's
	// LoadingPhase is PostConfigInit (VaCuus.uplugin:31), so this value is already
	// final before any document — hence any LoadTexture — can exist, and the static is
	// never reset. A retry could therefore only ever re-run the failing path and
	// re-emit the Error above once per image, which is exactly what LoadTexture's
	// "already logged once; do not spam per image" promises it does not do.
	return GVaCuusImageWrapperModule.load(std::memory_order_acquire);
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
		// UTF8_TO_TCHAR, not %hs: Source is a UTF-8 byte string (Config.h:108 aliases
		// Rml::String to std::string) and %hs treats it as narrow-ANSI, so any
		// non-ASCII path mojibakes. Same conversion the drain already uses.
		UE_LOG(LogVaCuus, Warning, TEXT("LoadTexture: failed to read '%s'"), UTF8_TO_TCHAR(Source.c_str()));
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
		UE_LOG(LogVaCuus, Warning, TEXT("LoadTexture: unrecognized image format in '%s'"), UTF8_TO_TCHAR(Source.c_str()));
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
		UE_LOG(LogVaCuus, Warning, TEXT("LoadTexture: failed to parse '%s'"), UTF8_TO_TCHAR(Source.c_str()));
		return Rml::TextureHandle(0);
	}

	const FIntPoint Size(Probe->GetWidth(), Probe->GetHeight());

	// Dropped as soon as the size is out. The JPEG wrapper retains a libjpeg-turbo
	// decompressor from SetCompressed all the way to Uncompress
	// (JpegImageWrapper.cpp:362), plus its own copy of the compressed file; keeping
	// one alive per pending image for the whole in-flight window is pure overhead.
	// IImageWrapper exposes no Reset(), so the destructor does it
	// (JpegImageWrapper.cpp:92-95 -> Reset, JpegImageWrapper.cpp:97-111, tjDestroy at :107).
	Probe.Reset();

	if (Size.X <= 0 || Size.Y <= 0)
	{
		UE_LOG(LogVaCuus, Warning, TEXT("LoadTexture: '%s' reports a degenerate size %dx%d"),
			UTF8_TO_TCHAR(Source.c_str()), Size.X, Size.Y);
		return Rml::TextureHandle(0);
	}

	// The out-parameter is satisfied HERE, at the last point that can fail, rather than
	// after the task launch below: every remaining path returns a live handle, so an
	// out-contract a reader has to trace past a 60-line lambda to confirm becomes one
	// that is obviously satisfied on every success path.
	OutDimensions = Rml::Vector2i(Size.X, Size.Y);

	const FVaCuusTextureHandle Handle = NextTextureHandle++;
	ensureMsgf(Handle != 0, TEXT("Texture handle counter wrapped to the invalid sentinel"));

	// Step 11.3, the placeholder. (0,0,0,0) is a valid PREMULTIPLIED transparent
	// texel, so it satisfies FVaCuusTextureData's contract and the replayer's
	// One/InverseSourceAlpha blend leaves the destination untouched.
	//
	// Swapping in the real payload later is a plain re-Add on the replayer's handle
	// map, NOT an FRHITextureReference. Two verified reasons: every RHI's
	// RHIUpdateTextureReference enqueues its swap with EThreadFence::Enabled
	// (D3D12TextureReference.cpp:116, VulkanTexture.cpp:1131), so each late payload
	// would cost an RHI-thread fence; and Epic carries a standing "@todo dev-pr - This
	// should be refactored out when we eventually remove FRHITextureReference"
	// (VulkanTexture.cpp:1128, DynamicRHI.cpp:581). The class itself is NOT deprecated
	// — only the implied-immediate-command-list free functions are
	// (RHICommandList.h:5321, 5327) — and it does implement
	// GetNativeShaderResourceView() (RHITextureReference.h:21), so this is a cost and
	// direction-of-travel argument, not a "cannot be done" one.
	FVaCuusTextureData& Placeholder = GetPending().NewTextures.Add(Handle);
	Placeholder.Size = FIntPoint(1, 1);
	Placeholder.RGBA.AddZeroed(4);

	InFlightTextures.Add(Handle);

	// The bytes ride into the task as the Rml::String they were read into — that is
	// a std::string byte buffer (Config.h:108), not an RmlUi object, and moving it
	// spares the UI thread another O(filesize) copy. No RmlUi API is touched on the
	// worker. The sink is captured by value, so the task keeps it alive on its own.
	//
	// WHY THE IMAGEWRAPPER API IS SAFE TO USE FROM N ARBITRARY WORKERS, which is the
	// premise of this whole change and not just of moving the bytes:
	//  - IImageWrapperModule::CreateImageWrapper is a stateless per-call MakeShared
	//    switch on the format (ImageWrapperModule.cpp:79-158) — no shared state, no
	//    cache, nothing to serialise. Each wrapper instance therefore owns its own
	//    libpng/libjpeg-turbo state, so SetCompressed/GetRaw on distinct instances do
	//    not alias.
	//  - The wrappers are NOT unconditionally reentrant, and that is the part worth
	//    writing down. JPEG: libjpeg-turbo >= 2.0.5 is thread-safe, so
	//    JPEG_SCOPE_CRITSEC() compiles to `do {} while(0)`
	//    (JpegImageWrapper.cpp:61-76) — the FCriticalSection only exists on the legacy
	//    jpgd path. PNG: libpng gets a per-call jump buffer specifically "to allow
	//    concurrent compression\decompression on concurrent threads"
	//    (PngImageWrapper.cpp:416-417).
	//  - THE EXCEPTION IS ANDROID. There, GPNGSection is a process-global
	//    FCriticalSection (PngImageWrapper.cpp:19-22) taken by both LoadPNGHeader
	//    (:573-576) and UncompressPNGData (:373-376), so N concurrent PNG decodes
	//    queue behind each other AND behind any engine PNG work — including the
	//    synchronous probe above. Correct, just not parallel; if Android UI ever loads
	//    many PNGs at once, that lock is the thing to measure, not this task.
	//
	// PRIORITY IS EXPLICIT AND THIS IS THE PLUGIN'S ONLY UE::Tasks::Launch, so it sets
	// the house default. BackgroundHigh, not the implicit ETaskPriority::Normal that
	// Launch would default to (Tasks/Task.h:302):
	//  - Normal is a FOREGROUND priority — ForegroundCount is the very next enumerator
	//    (Async/Fundamental/Task.h:20-33) — and a foreground worker's dequeue caps at
	//    ForegroundCount (Scheduler.cpp:726), so a background task can never land on
	//    one. Choosing background makes "image decode does not compete with the frame"
	//    a scheduler guarantee instead of a hope.
	//  - The foreground pool is TINY: GNumForegroundWorkers defaults to 2
	//    (TaskGraph.cpp:131) and the background pool gets every remaining worker
	//    (TaskGraph.cpp:1284-1285). A document with 40 images at Normal would both
	//    serialise 40 multi-ms decodes behind 2 workers and evict the highest-priority
	//    engine work from them. At background it gets more parallelism, not less.
	//  - BackgroundHigh rather than BackgroundNormal because image latency here IS
	//    user-visible (an undecoded image is a transparent hole at document load), and
	//    BackgroundHigh is the tier the engine maps ordinary queued work to
	//    (QueuedThreadPoolWrapper.h:490: EQueuedWorkPriority::Normal -> BackgroundHigh)
	//    and the tier FImage's own detached work uses (ImageCore.cpp:993). Queueing
	//    behind shader-cache and asset-gatherer jobs at BackgroundNormal buys nothing.
	//  - The residual cost is a lower OS thread priority
	//    (FPlatformAffinity::GetTaskBPThreadPriority(), TaskGraph.cpp:1287), so a
	//    decode can be preempted. It cannot be starved for want of a worker: there is
	//    always at least one background worker (FMath::Max(1, ...), TaskGraph.cpp:1284).
	//
	// SourcePath costs one small FString allocation on the UI thread, kept knowingly:
	// the only alternative is handing the worker a std::string copy of the path, which
	// is the same allocation on the same thread, and this call has already done an
	// O(filesize) read plus SetCompressed's O(filesize) memcpy here.
	UE::Tasks::FTask DecodeTask = UE::Tasks::Launch(UE_SOURCE_LOCATION,
		[Sink = DecodeSink, Module = ImageWrapperModule, Handle, Format, ProbedSize = Size,
			SourcePath = FString(UTF8_TO_TCHAR(Source.c_str())), Bytes = MoveTemp(FileData)]() mutable
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(VaCuusTextureDecode);

			if (Sink->bAbandoned.load(std::memory_order_acquire))
			{
				return; // The recorder is gone; the decode has no destination.
			}

			FVaCuusTextureDecode Result;
			Result.Handle = Handle;
			Result.Source = MoveTemp(SourcePath);
			Result.Failure = EVaCuusTextureDecodeFailure::Decode;

			const TSharedPtr<IImageWrapper> ImageWrapper = Module->CreateImageWrapper(Format);
			TArray<uint8> RawRGBA;

			// GetRaw(ERGBFormat, int32, TArray<uint8>&) is the overload the engine
			// header labels "this is often broken, should only be used with InFormat ==
			// GetFormat() / DEPRECATED, use GetRaw() with 1 argument or GetRawImage()"
			// (IImageWrapper.h:318-319). The caveat is live for us: JPEG's GetFormat()
			// is BGRA (JpegImageWrapper.cpp:354) while we ask for RGBA. It is satisfied
			// anyway because ConvertTJpegPixelFormat handles ERGBFormat::RGBA
			// explicitly (JpegImageWrapper.cpp:53) and PNG's UncompressPNGData branches
			// on InFormat rather than Format (PngImageWrapper.cpp:437-471). KEPT rather
			// than migrated to GetRawImage(FImage&): that returns an FImage in
			// ERawImageFormat terms (BGRA8 for these wrappers), so it would move the
			// channel-order conversion into this plugin and is a wider change than a
			// review pass should carry. Filed, not half-done.
			if (ImageWrapper.IsValid() && ImageWrapper->SetCompressed(Bytes.data(), Bytes.size()) &&
				ImageWrapper->GetRaw(ERGBFormat::RGBA, 8, RawRGBA))
			{
				// VALIDATE BEFORE TOUCHING THE BUFFER. The probe already handed
				// ProbedSize to RmlUi, which laid the element out around it, so a
				// payload of another size would silently stretch; and nothing in
				// IImageWrapper guarantees the byte count matches the reported
				// dimensions (FImageWrapperBase::GetRaw just moves RawData out,
				// ImageWrapperBase.cpp:61-80 — no size check at all). Validating first
				// also means a rejected payload is rejected before, not after, a full
				// premultiply pass over up to hundreds of MB.
				//
				// int64 because ProbedSize is two int32s: the product is unreachable
				// past MAX_int32 (GetRaw's own ensureMsgf refuses anything that will not
				// fit a 32-bit TArray, IImageWrapper.h:324) but int32 overflow is UB and
				// the cast is free.
				const FIntPoint DecodedSize(ImageWrapper->GetWidth(), ImageWrapper->GetHeight());
				const int64 ExpectedBytes = int64(ProbedSize.X) * int64(ProbedSize.Y) * 4;
				if (DecodedSize != ProbedSize || int64(RawRGBA.Num()) != ExpectedBytes)
				{
					// Two SetCompressed passes over the same bytes disagreeing means the
					// DECODER disagreed with itself, which is a different bug report
					// from "this asset will not decode" — hence its own reason code.
					Result.Failure = EVaCuusTextureDecodeFailure::SizeMismatch;
				}
				else if (Sink->bAbandoned.load(std::memory_order_acquire))
				{
					// Second abandon check, AFTER the expensive call. The destructor's
					// flag used to be read only at task entry, so a view torn down
					// mid-decode (live reload, PIE stop) still paid for the whole
					// premultiply pass and still moved the payload into a queue nobody
					// would drain — with N images in flight, N full payloads stayed
					// resident until the LAST task exited, because the sink dies with
					// the last reference. Returning here drops RawRGBA with the frame
					// and bounds teardown at one payload per still-running task.
					return;
				}
				else
				{
					// Alpha is premultiplied here at the decode boundary so ALL recorded
					// texture payloads share one contract: RGBA memory order,
					// premultiplied alpha. GenerateTexture (fonts) already arrives
					// premultiplied per the Rml ColourbPremultiplied contract, and the
					// replayer blends with One/InverseSourceAlpha, which assumes
					// premultiplied sources.
					uint8* const Begin = RawRGBA.GetData();
					uint8* const End = Begin + RawRGBA.Num();

					// JPEG SUPPLIES NO ALPHA, so the invariant has to be imposed rather
					// than trusted. RawData is AddUninitialized
					// (JpegImageWrapper.cpp:446-447) and ConvertTJpegPixelFormat maps
					// RGBA -> TJPF_RGBA under the engine's own note that "libjpeg-turbo
					// currently does not actually read/write A - TJPF_BGRA is a synonym
					// for TJPF_BGRX" (JpegImageWrapper.cpp:47-48); turbojpeg documents
					// the alpha component as undefined on decompress. Latent, not live:
					// libjpeg-turbo's extended-RGB converter happens to store 0xFF
					// there, which is why JPEGs have always rendered. But "premultiplied
					// RGBA" is a documented invariant of FVaCuusTextureData now, and
					// branching a premultiply on an undefined byte that then feeds an
					// InverseSourceAlpha blend is not something to leave to a
					// third-party filler value. Stamp opaque; premultiply by 255 is a
					// no-op, so this replaces the pass rather than adding one.
					if (Format == EImageFormat::JPEG || Format == EImageFormat::GrayscaleJPEG)
					{
						for (uint8* Pixel = Begin; Pixel < End; Pixel += 4)
						{
							Pixel[3] = 255;
						}
					}
					else
					{
						for (uint8* Pixel = Begin; Pixel < End; Pixel += 4)
						{
							const uint32 Alpha = Pixel[3];
							if (Alpha < 255)
							{
								// +127 before the divide = round-to-nearest, removes the
								// downward bias a truncating x*a/255 would add to every
								// texel. Exactly equivalent to round(c*a/255.0): no
								// integer lies in (n+127, n+127.5], so floor((n+127)/255)
								// == floor((n+127.5)/255) for every n.
								Pixel[0] = uint8((Pixel[0] * Alpha + 127u) / 255u);
								Pixel[1] = uint8((Pixel[1] * Alpha + 127u) / 255u);
								Pixel[2] = uint8((Pixel[2] * Alpha + 127u) / 255u);
							}
						}
					}

					Result.Data.Size = DecodedSize;
					Result.Data.RGBA = MoveTemp(RawRGBA);
					Result.Failure = EVaCuusTextureDecodeFailure::None;
				}
			}
			// else: Result.Data keeps its zero Size, which the drain reads as failure.

			// Third and last abandon check, immediately before the hand-off. Enqueuing
			// after teardown would be SAFE — the sink is refcounted and TMpscQueue
			// destroys whatever is still queued when the last reference dies — but it
			// keeps a whole decoded payload alive until then for no reason, so the
			// cheap read is worth it. Losing the race is still harmless.
			if (Sink->bAbandoned.load(std::memory_order_acquire))
			{
				return;
			}

			Sink->Completed.Enqueue(MoveTemp(Result));
		},
		UE::Tasks::ETaskPriority::BackgroundHigh);

#if WITH_DEV_AUTOMATION_TESTS
	// Only WaitForTextureDecodes needs the handle. See the header.
	DecodeTasks.Add(MoveTemp(DecodeTask));
#else
	// Launched and detached: the scheduler holds its own reference to a running task,
	// so dropping the handle is legal and the array above is test-only scaffolding.
	(void)DecodeTask;
#endif

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
	// weak-pointer dance against teardown.
	//
	// WHY A LATE PAYLOAD IS GUARANTEED TO REACH THE SCREEN: every UI frame is RECORDED
	// — FVaCuusUIThread::RunFrame records for every host with HasView()
	// (VaCuusUIThread.cpp:769-775) — and this drain runs at the top of each of them, so
	// an arrival always lands in a live pending buffer. Whether that buffer is
	// PUBLISHED is a separate decision, and since Task 12 it is a real one: the idle
	// gate in EndFrameAndPublish() withholds a frame whose commands and ViewSize are
	// unchanged. A non-empty NewTextures is one of its four required wake conditions,
	// which is exactly what keeps this path working — an arrival is the one resource
	// delta that can appear in a frame where nothing else moved, and withholding that
	// publish would leave the transparent 1x1 placeholder on screen indefinitely.
	//
	// Note the ordering that makes the wake condition sufficient rather than merely
	// stated: this drain runs BEFORE Context::Update() (see BeginFrame), so a payload
	// is already sitting in the pending buffer's NewTextures by the time the gate
	// inspects it at the end of the frame.
	//
	// Downstream of the gate the payload is unconditional again: the render thread
	// draws whenever any buffer is pending, replaying the newest one in full
	// (VaCuusSlateElement.cpp:56, :83). The only other filter on the path is
	// ShouldConsume's generation-idempotence check (VaCuusReplayRenderer.cpp:94-114),
	// which stops one buffer being consumed twice and has nothing to do with whether
	// the UI changed.
	//
	// "Placeholder in frame N, payload in N+1" is the COMMON CASE, NOT AN INVARIANT.
	// LoadTexture is reachable out of frame: AdoptDocument -> Document->Show()
	// (VaCuusRmlDocumentHost.cpp:205) runs inside DrainCommands (VaCuusUIThread.cpp:762,
	// dispatched at :856), i.e. before the record loop, and RmlUi loads file textures
	// lazily from FileTextureDatabase::EnsureLoaded (TextureDatabase.cpp:118-130). So a
	// load can happen with bInFrame == false, GetPending() lazily creates the buffer
	// that this very drain then adds to, and a small fast PNG can overwrite its own
	// placeholder in the SAME buffer. Benign: the buffer then simply carries the real
	// payload and never the placeholder.
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
			// subsequent frame. Nothing leaks, but note WHY: the 1x1 is retired at
			// render-manager teardown, not on document close. RmlUi releases file
			// textures only from an explicit Rml::ReleaseTexture(source)
			// (RenderManager.cpp:252-255) or ReleaseAllTextures()
			// (RenderManager.cpp:257-261, also run from ~RenderManager at :41), both
			// reaching FileTextureDatabase (TextureDatabase.cpp:151-166, :168-178).
			// Closing a document or destroying the element does NOT release them.
			//
			// One line per failed image, so a broken asset is visible in the log
			// without spamming per frame — and the two causes are named separately,
			// because a size mismatch is the DECODER contradicting itself, which sends
			// a reader to the engine rather than to the asset.
			if (Completed->Failure == EVaCuusTextureDecodeFailure::SizeMismatch)
			{
				UE_LOG(LogVaCuus, Warning,
					TEXT("LoadTexture: decoder disagreed with its own probe for '%s'; texture %llu stays transparent"),
					*Completed->Source, Completed->Handle);
			}
			else
			{
				UE_LOG(LogVaCuus, Warning, TEXT("LoadTexture: async decode of '%s' failed; texture %llu stays transparent"),
					*Completed->Source, Completed->Handle);
			}
			continue;
		}

		GetPending().NewTextures.Add(Completed->Handle, MoveTemp(Completed->Data));
	}

#if WITH_DEV_AUTOMATION_TESTS
	DecodeTasks.RemoveAllSwap([](const UE::Tasks::FTask& Task) { return Task.IsCompleted(); }, EAllowShrinking::No);
#endif
}

#if WITH_DEV_AUTOMATION_TESTS
bool FVaCuusRecordingRenderInterface::WaitForTextureDecodes(FTimespan Timeout)
{
	return UE::Tasks::Wait(DecodeTasks, Timeout);
}
#endif

TUniquePtr<FVaCuusCommandBuffer> FVaCuusRecordingRenderInterface::EndFrameAndPublish()
{
	CheckOwnerThread();
	ensureMsgf(bInFrame, TEXT("EndFrameAndPublish() without a matching BeginFrame()"));
	bInFrame = false;

	// MoveTemp leaves Pending null, so whatever happens below the NEXT frame starts
	// from a fresh buffer. That matters for the skip path: a buffer left in place
	// would have this frame's commands appended to again next frame, and every
	// command would be replayed twice.
	TUniquePtr<FVaCuusCommandBuffer> Published = Pending ? MoveTemp(Pending) : MakeUnique<FVaCuusCommandBuffer>();

	// THE IDLE SHORT-CIRCUIT (M2 Task 12). RmlUi offers no dirty signal to ask.
	// Context::Update() and Context::Render() end in an unconditional `return true`
	// (ThirdParty/RmlUi/Source/Core/Context.cpp:219 and :241). IsLayoutDirty() is not
	// callable from here -- protected on Element (Include/RmlUi/Core/Element.h:637,
	// under the `protected:` at :593) and private on ElementDocument
	// (Include/RmlUi/Core/ElementDocument.h:146, under the `private:` at :135) -- and
	// would be useless even if it were public. It returns the layout_dirty flag
	// (ElementDocument.cpp:546-549), which Context::Update() has already cleared by the
	// time this code could look at it: Update() calls doc->UpdateLayout() for every
	// document (Context.cpp:207-214) and UpdateLayout() ends with layout_dirty = false
	// (ElementDocument.cpp:476-493). And it is the wrong question regardless -- a colour
	// change, a hover restyle or a scroll all change what is drawn without dirtying
	// layout. RenderManager carries no version counter, and GetNextUpdateDelay
	// (Include/RmlUi/Core/Context.h:294) is the lowest requested TIMER timestamp
	// (documented at :286), not a change flag. So the frame is recorded as usual and
	// compared afterwards.
	//
	// WHY DROPPING THIS BUFFER LOSES NOTHING: each buffer repaints the whole frame
	// from scratch (that is why the replayer draws only the newest queued one and
	// takes just the resource deltas from the rest, VaCuusSlateElement.cpp:79-83), so
	// a buffer whose commands and ViewSize hash equal to the last PUBLISHED one draws
	// exactly what the per-view render target already holds. The render thread then
	// re-composites that render target unconditionally: Draw_RenderThread's replay is
	// inside `if (PendingBuffers.Num() > 0)` but the composite that follows is outside
	// it and reads Replayer.GetOutputRT() directly (VaCuusSlateElement.cpp:91-140), so
	// the UI stays on screen with no buffer in flight at all.
	//
	// THE RESOURCE CONDITION IS NOT AN OPTIMISATION, IT IS CORRECTNESS: the four
	// delta arrays are the ONLY channel by which created and released resources
	// reach the replayer, and they are cleared with the buffer. Dropping a buffer
	// carrying any of them would either leave the replayer without a resource a
	// later frame draws with, or leak the RHI resource behind a release nobody
	// consumed. The case that makes this non-hypothetical is a finished async image
	// decode: BeginFrame() drains it into NewTextures BEFORE Context::Update() runs
	// (see BeginFrame above), so a frame where nothing else moved still arrives here
	// with non-empty resource traffic and MUST publish, or the image stays a
	// transparent 1x1 placeholder forever.
	const bool bHasResourceTraffic = Published->NewGeometry.Num() > 0 || Published->NewTextures.Num() > 0 ||
		Published->ReleasedGeometry.Num() > 0 || Published->ReleasedTextures.Num() > 0;
	const uint64 ContentHash = VaCuusHashFrameContent(*Published);

	// Generation > 0 == "this recorder has published at least once", so the first
	// frame of a view always publishes and never compares against an unset hash.
	if (Generation > 0 && ContentHash == LastPublishedContentHash && !bHasResourceTraffic)
	{
		// A skipped frame consumes NO generation: Generation is documented as a
		// strictly increasing PUBLISH counter and ShouldConsume treats a repeat as
		// "already consumed", so counting withheld frames in it would make the
		// number mean "frames recorded" instead. The UI thread's own frame count
		// (FVaCuusUIThread::GetFrameCount) is that other number and still advances
		// on every frame, published or not.
		++NumFramesSkipped;
		return nullptr;
	}

	LastPublishedContentHash = ContentHash;
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
