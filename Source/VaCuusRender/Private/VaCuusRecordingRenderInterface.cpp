// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusRecordingRenderInterface.h"

#include "VaCuusDefines.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTLS.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Dictionary.h> // Rml::Get over the CompileFilter parameter dictionary
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
static_assert(sizeof(Rml::CompiledFilterHandle) == sizeof(FVaCuusFilterHandle), "Rml filter handle must be 64-bit");
static_assert(sizeof(Rml::LayerHandle) == sizeof(FVaCuusLayerHandle), "Rml layer handle must be 64-bit");

// The two mirrored enums are recorded by static_cast, so their numeric values must track
// RmlUi's (RenderInterface.h:10-18). Pinned value by value: a reordered or extended Rml
// enum fails here instead of silently recording the wrong operation.
static_assert(uint8(Rml::BlendMode::Blend) == uint8(EVaCuusBlendMode::Blend) &&
		uint8(Rml::BlendMode::Replace) == uint8(EVaCuusBlendMode::Replace),
	"EVaCuusBlendMode must mirror Rml::BlendMode value for value");
static_assert(uint8(Rml::ClipMaskOperation::Set) == uint8(EVaCuusClipMaskOp::Set) &&
		uint8(Rml::ClipMaskOperation::SetInverse) == uint8(EVaCuusClipMaskOp::SetInverse) &&
		uint8(Rml::ClipMaskOperation::Intersect) == uint8(EVaCuusClipMaskOp::Intersect),
	"EVaCuusClipMaskOp must mirror Rml::ClipMaskOperation value for value");

/**
 * THE KILL SWITCH for the idle short-circuit, and the reason it is worth its handful of
 * lines: the gate's failure mode is a frozen UI with no error, no ensure and no log line,
 * and it is the first thing anyone will suspect when a UI stops updating. Without this the
 * only way to test that suspicion is to rebuild the plugin with the condition commented out.
 *
 * Read on the UI thread with GetValueOnAnyThread(), following FVaCuusPerfLog::IsEnabled()
 * (VaCuusStats.cpp:104-107). Flipping it at runtime is safe in both directions: turning it
 * off just publishes every recorded frame from then on, and turning it back on compares
 * against the hash of whatever was published last, which is by definition what the render
 * target holds.
 */
static TAutoConsoleVariable<int32> CVarVaCuusIdleGate(
	TEXT("vacuus.IdleGate"),
	1,
	TEXT("1 (default) = withhold the publish of a recorded UI frame that draws what the render thread already has.\n")
		TEXT("0 = publish every recorded frame. Use this to rule the idle short-circuit out when a UI looks frozen."));

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
	//
	// The SECOND site that used to name all the delta arrays inline; it asks the buffer
	// now, for the same reason the gate does. The message below still itemises the six --
	// a seventh array would make it read "0 of everything" here, which is a cosmetic gap and
	// not a lost resource, and FVaCuusCommandBuffer::HasResourceTraffic is one grep away.
	if (Pending && Pending->HasResourceTraffic())
	{
		UE_LOG(LogVaCuus, Log,
			TEXT("Recorder destroyed with unpublished resource traffic (new: %d geometry, %d textures, %d filters; released: %d geometry, %d textures, %d filters) — dropped"),
			Pending->NewGeometry.Num(), Pending->NewTextures.Num(), Pending->NewFilters.Num(),
			Pending->ReleasedGeometry.Num(), Pending->ReleasedTextures.Num(), Pending->ReleasedFilters.Num());
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

	// THE DECODE-FORMAT WHITELIST. A WORKAROUND, NOT A FIX — the real one is at the end of
	// this comment and is deliberately not attempted here.
	//
	// DetectImageFormat can return eleven distinct formats (ImageWrapperModule.cpp:160-216)
	// and the decode below asks whichever one it got for GetRaw(ERGBFormat::RGBA, 8). That
	// is only a REQUEST: FImageWrapperBase::GetRaw forwards InFormat to Uncompress
	// unfiltered (ImageWrapperBase.cpp:61-64), and the wrappers do not agree about
	// honouring it. Two of them do not merely misbehave, they assert:
	//  - BMP always writes BGRA8 and says so with check(InFormat == ERGBFormat::BGRA)
	//    (BmpImageWrapper.cpp:69), reached from FBmpImageWrapper::Uncompress (:32-53) for
	//    anything DetectImageFormat called BMP — the guard at :42 is the same 'BM' magic the
	//    sniff already matched, and is skipped outright when bHasHeader is false.
	//  - DDS, under its own "only support decoding to own format": check(InFormat ==
	//    GetFormat()) (DdsImageWrapper.cpp:176-178).
	//  - ICO forwards to a PNG or BMP sub-wrapper (IcoImageWrapper.cpp:89-95), so a
	//    BMP-payload .ico inherits the BMP assert.
	// check() is fatal in every DO_CHECK build, Development Editor included, and this
	// decode runs on a WORKER: before this whitelist, one <img src="logo.bmp"> in any
	// document took the whole editor down from a background thread with no VaCuus frame in
	// the callstack. Nothing upstream stopped it — the sniff is the two bytes 0x42 0x4D
	// (IMAGE_MAGIC_BMP at ImageWrapperModule.cpp:32, matched at :175-178) and
	// FBmpImageWrapper::SetCompressed only reads the header, so the synchronous probe below
	// was perfectly happy.
	//
	// TGA is why this is a WHITELIST and not a BMP/DDS blocklist: FTgaImageWrapper::
	// Uncompress ignores InFormat entirely and always writes BGRA order
	// (TgaImageWrapper.cpp:131-133). The byte count is still W*H*4, so every validation on
	// the worker passes and red and blue simply come out swapped with no diagnostic. A
	// blocklist of the formats that crash would have shipped that one.
	//
	// What is left in is what has been read and confirmed to honour a (RGBA, 8) request:
	//  - PNG. UncompressPNGData sizes and orders from InFormat, not from its own Format
	//    (PngImageWrapper.cpp:450, and png_set_bgr only for BGRA at :468-470), and
	//    FPngImageWrapper::Uncompress re-decodes whenever the two differ (:360-367) — which
	//    is what makes greyscale and paletted PNGs come out RGBA8 too, not just truecolour.
	//  - JPEG. UncompressTurbo derives its channel count from InFormat
	//    (JpegImageWrapper.cpp:427-430) and ConvertTJpegPixelFormat maps RGBA to TJPF_RGBA
	//    (:53). Greyscale JPEGs are included: Channels is 4 for any RGBA request.
	//  - UEJPEG. Ignores InFormat, but a four-component UEJPEG is natively RGBA
	//    (UEJpegImageWrapper.cpp:144-146, :275-276) so it is correct by coincidence rather
	//    than by contract. Three components its own SetCompressed refuses (:147-148); the
	//    greyscale case is the one hole this whitelist does not close, so the probe below
	//    closes it.
	// The remaining four decline an 8-bit RGBA request rather than misbehave, so they were
	// never the bug — they are excluded anyway, because "declines cleanly" is a per-wrapper
	// accident and this list should say what VaCuus supports, not what happens not to hurt:
	// EXR reports "Unsupported bit depth, expected 16 or 32" and SetErrors out before it can
	// reach its own check(InBitDepth == 32) (ExrImageWrapper.cpp:447-462, :628); HDR
	// overrides GetRaw itself and rejects anything that is not (BGRE, 8) with a logged error
	// (HdrImageWrapper.cpp:205-211 — the check() pair at :164-165 is on SetRaw, the ENCODE
	// path, and is not reachable from here); TIFF gates its whole decode on
	// InFormat == Format and SetErrors otherwise (TiffImageWrapper.cpp:579, :654-657), and
	// (RGBA, 8) is not among the formats its SetCompressed can report (:582-647); ICNS
	// returns false straight out of SetCompressed off Mac (IcnsImageWrapper.cpp:67-69), so
	// its non-Mac checkf(false) at :236 is unreachable and the probe below already refused
	// it. On Mac it would in fact have worked (check accepts RGBA, :119-120).
	//
	// GrayscaleJPEG and GrayscaleUEJPEG are absent because DetectImageFormat CANNOT return
	// them — both sniffs carry the engine's own "@Todo: Should we detect grayscale vs
	// non-grayscale?" (ImageWrapperModule.cpp:169, :173).
	//
	// THE REAL FIX is GetRawImage(FImage&) / the one-argument GetRaw(), which ask each
	// wrapper for its OWN format and therefore trip none of the asserts above. That is a
	// wider change than a fix pass should carry, because it moves channel-order conversion
	// into this plugin: GetRawImage goes through GetRaw(GetFormat(), ...) and
	// GetClosestRawImageFormat (ImageWrapperBase.cpp:420-434), and GetFormat() is BGRA for
	// PNG at 8 bits (PngImageWrapper.cpp:624-627) and for non-greyscale JPEG
	// (JpegImageWrapper.cpp:354) — so migrating trades this whitelist for an R/B swap of our
	// own, in exchange for BMP/DDS/TGA/ICO support. Filed, not half-done.
	if (Format != EImageFormat::PNG && Format != EImageFormat::JPEG && Format != EImageFormat::UEJPEG)
	{
		UE_LOG(LogVaCuus, Warning,
			TEXT("LoadTexture: unsupported image format '%s' in '%s' — VaCuus decodes PNG, JPEG and UEJPEG only"),
			ImageWrapperModule->GetExtension(Format), UTF8_TO_TCHAR(Source.c_str()));
		return Rml::TextureHandle(0);
	}

	// Step 11.1, the synchronous dimension probe. Two costs that are NOT free and
	// are not avoidable through IImageWrapper's API:
	//  - SetCompressed memcpy's the whole file (ImageWrapperBase.cpp:104-122, whose
	//    own comment calls the copy "usually unnecessary" at :111), so this is
	//    O(filesize), not O(header).
	//  - 1/2/4-bit paletted/grey PNGs DECODE inside SetCompressed
	//    (PngImageWrapper.cpp:309-331 calls UncompressPNGData at :328), so for that one
	//    class of image the decode still happens on this thread — and then again on
	//    the worker. Rare in UI art, and there is no header-only query to use
	//    instead (IImageWrapperModule offers none).
	TSharedPtr<IImageWrapper> Probe = ImageWrapperModule->CreateImageWrapper(Format);
	if (!Probe.IsValid() || !Probe->SetCompressed(FileData.data(), FileData.size()))
	{
		UE_LOG(LogVaCuus, Warning, TEXT("LoadTexture: failed to parse '%s'"), UTF8_TO_TCHAR(Source.c_str()));
		return Rml::TextureHandle(0);
	}

	// THE ONE HOLE THE FORMAT WHITELIST ABOVE CANNOT CLOSE, closed here because the probe is
	// the first place that knows it. UEJPEG is whitelisted for its four-component form, but
	// FUEJpegImageWrapper::Uncompress ignores the requested ERGBFormat and allocates
	// Width*Height*NumColors from what the decoder found (UEJpegImageWrapper.cpp:275-276),
	// so a GREYSCALE UEJPEG returns W*H bytes from a (RGBA, 8) request and GetRaw still
	// returns TRUE. That used to reach the worker's byte-count check and be reported as
	// "the decoder disagreed with its own probe", which sent a reader to the engine for what
	// is really an unsupported input.
	//
	// GetFormat() is the exact test and needs no guessing: FUEJpegImageWrapper::SetCompressed
	// sets Gray for one component and RGBA for four and refuses everything else
	// (UEJpegImageWrapper.cpp:139-148). Scoped to UEJPEG deliberately — a greyscale PNG or
	// JPEG also reports Gray here and both decode correctly to RGBA8 (PNG re-decodes when
	// InFormat differs from Format, PngImageWrapper.cpp:360-367; JPEG takes its channel
	// count from InFormat, JpegImageWrapper.cpp:427-430), so testing GetFormat() alone would
	// refuse images that work.
	if (Format == EImageFormat::UEJPEG && Probe->GetFormat() != ERGBFormat::RGBA)
	{
		UE_LOG(LogVaCuus, Warning,
			TEXT("LoadTexture: unsupported single-channel UEJPEG in '%s' — VaCuus needs four-component RGBA"),
			UTF8_TO_TCHAR(Source.c_str()));
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
	// Launch would default to (Tasks/Task.h:302). ONE ARGUMENT CARRIES THE DECISION, and it
	// is a guarantee rather than a hope:
	//  - A foreground worker can NEVER dequeue a background task. Its scan is capped at
	//    ForegroundCount — MaxPriority is Count only when bPermitBackgroundWork
	//    (Scheduler.cpp:726, the loop at :728) — and Normal sits below that cap while
	//    BackgroundHigh IS that cap (BackgroundHigh == ForegroundCount,
	//    Async/Fundamental/Task.h:20-33). So a decode at BackgroundHigh cannot land on a
	//    foreground worker, and "image decode never competes with the frame for one" stops
	//    being a hope about scheduler behaviour and becomes a structural property. At Normal
	//    it would be exactly the opposite: a multi-ms decode could occupy one of the two
	//    workers the engine reserves for latency-critical work.
	//
	// THE HONEST TRADE, because the cap is ONE-DIRECTIONAL and it is easy to get backwards:
	// a BACKGROUND worker scans from priority index 0 too (Scheduler.cpp:728), so it happily
	// runs — and in fact PREFERS — High and Normal tasks. Choosing background therefore
	// COSTS parallelism rather than gaining it: at Normal these decodes would be eligible for
	// every worker, at BackgroundHigh only for the background pool. The cost is exactly
	// GNumForegroundWorkers workers (NumBackgroundWorkers = NumWorkerThreads -
	// GNumForegroundWorkers, TaskGraph.cpp:1284-1285), which is 2 on any machine the editor
	// runs on — so all-but-two instead of all, plus a lower OS thread priority
	// (FPlatformAffinity::GetTaskBPThreadPriority(), TaskGraph.cpp:1287) so a decode can be
	// preempted. Cheap, and bought for the guarantee above. Starvation is not part of the
	// price: there is always at least one background worker (FMath::Max(1, ...),
	// TaskGraph.cpp:1284) and the scheduler wakes one on every cross-pool launch precisely
	// because "foreground threads are not allowed to process them" (Scheduler.cpp:546-552).
	//
	// BackgroundHigh rather than BackgroundNormal because image latency here IS user-visible
	// (an undecoded image is a transparent hole at document load), and BackgroundHigh
	// genuinely outranks routine thread-pool work: FQueuedLowLevelThreadPool maps the
	// DEFAULT EQueuedWorkPriority::Normal to BackgroundNormal, one tier below us
	// (QueuedThreadPoolWrapper.h:488-491 indexed by EQueuedWorkPriority, QueuedThreadPool.h:
	// 13-22 — BackgroundHigh is where EQueuedWorkPriority::High lands, not Normal), and in
	// the editor GThreadPool schedules onto exactly that pool (LaunchEngineLoop.cpp:2621,
	// :2630). So shader-cache and asset-gatherer jobs queued at the default do not sit in
	// front of a decode. It is also the tier FImage's own detached work uses
	// (ImageCore.cpp:986-993).
	//
	// (WHY "2 ON ANY MACHINE" and not just "the default is 2": GNumForegroundWorkers = 2 at
	// TaskGraph.cpp:131 is only the INITIALIZER, and quoting it as the answer would be wrong.
	// Startup overwrites it with FMath::Max(FMath::DivideAndRoundUp(NumThreads, 21), 2)
	// (TaskGraph.cpp:1790) where NumThreads is NumberOfWorkerThreadsToSpawn()
	// (LaunchEngineLoop.cpp:2592), then applies a -foregroundworkers= override (:1793); the
	// scheduler additionally clamps it to 1 below four worker threads (:1279-1282). So it
	// lands on 2 for any NumThreads below 42, and the worker count is cores-1 generically
	// (GenericPlatformMisc.cpp:1984) or logical-cores-2 on Unix with hyperthreading
	// (UnixPlatformMisc.cpp:1695-1706) — 30 on the 16-core/32-thread box this was measured
	// on, hence 28 background workers there. Nowhere near 42 either way.)
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
			// (IImageWrapper.h:318-319). The caveat is live for us: JPEG's GetFormat() is
			// BGRA for anything not 4:0:0 subsampled (JpegImageWrapper.cpp:354 — the
			// greyscale arm of that ternary reports Gray) while we ask for RGBA. It is
			// satisfied anyway, for the three formats the whitelist in LoadTexture lets
			// through and ONLY for those — see that comment for the per-wrapper reading and
			// for why this is not migrated to GetRawImage(FImage&).
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
					//
					// AND THAT IS NOW TRUE OF THIS BRANCH, which it was not before. Every
					// format that survives LoadTexture's whitelist derives its byte count
					// from the REQUESTED format, so W*H*4 is arithmetic rather than luck
					// (PNG at PngImageWrapper.cpp:450, JPEG at JpegImageWrapper.cpp:427-430).
					// The one input that used to arrive here without any decoder
					// contradicting anything — a greyscale UEJPEG, W*H bytes from a wrapper
					// that ignores InFormat — is refused at the probe now, so the message
					// the drain prints for this code no longer misdirects. What is left is a
					// genuine self-contradiction: UEJPEG re-reads width, height and component
					// count from its own decoder (UEJpegImageWrapper.cpp:269-276) and could
					// in principle disagree with what SetCompressed reported.
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

					// JPEG CARRIES NO ALPHA CHANNEL, so A=255 is a fact about the format,
					// not about the buffer. The buffer is guaranteed too, and the guarantee
					// is worth citing precisely because an earlier version of this comment
					// claimed the opposite: UE maps ERGBFormat::RGBA to TJPF_RGBA
					// (JpegImageWrapper.cpp:53), and turbojpeg documents TJPF_RGBA as
					// "the same as TJPF_RGBX, except that when decompressing, the X
					// component is guaranteed to be equal to the maximum sample value,
					// which can be interpreted as an opaque alpha channel"
					// (ThirdParty/libjpeg-turbo/3.0.0/include/turbojpeg.h:264-269). The
					// "ignored when compressing and undefined when decompressing" language
					// belongs to TJPF_RGBX/BGRX/XBGR/XRGB (turbojpeg.h:233-234, and the same
					// sentence at :240-241, :247-248, :254-255) — NOT to the A variants.
					//
					// WHAT MISLEADS A READER HERE, and the reason this paragraph is long:
					// the engine's own neighbouring comment says "libjpeg-turbo currently
					// does not actually read/write A - TJPF_BGRA is a synonym for
					// TJPF_BGRX" (JpegImageWrapper.cpp:47-48). That is accurate for
					// COMPRESS, where the X/A distinction genuinely does not exist
					// (turbojpeg.h:231-234), and it is stale for DECOMPRESS, which is the
					// only direction this code uses. It has already cost one review cycle.
					//
					// The stamp is therefore BELT AND BRACES, kept because it makes the
					// invariant LOCAL: FVaCuusTextureData documents premultiplied RGBA, and
					// this loop establishes it here instead of inheriting it from a
					// third-party header that a dependency bump could change. RawData is
					// AddUninitialized (JpegImageWrapper.cpp:446-447), so "local" is not
					// nothing. Free either way: premultiplying by 255 is a no-op, so this
					// replaces the general pass rather than adding one.
					//
					// JPEG alone, no GrayscaleJPEG arm: DetectImageFormat cannot return
					// GrayscaleJPEG (ImageWrapperModule.cpp:169 carries the engine's
					// "@Todo: Should we detect grayscale vs non-grayscale?"), so that arm was
					// unreachable and read as coverage that did not exist. A 4:0:0 JPEG
					// arrives here as EImageFormat::JPEG like any other and takes this
					// branch; nothing is lost. GrayscaleUEJPEG is unreachable for the same
					// reason (:173), and UEJPEG is excluded from the stamp on purpose: its
					// fourth channel is the alpha its encoder was handed, not a filler
					// (SetCompressed reports RGBA for four components,
					// UEJpegImageWrapper.cpp:144-146; Compress feeds NumComponents straight
					// from the raw buffer, :232), so the general path below is the correct
					// one for it and must not be short-circuited to opaque.
					if (Format == EImageFormat::JPEG)
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
			// else: Result.Failure keeps the Decode it was initialised with, which is what
			// the drain reads. Note the ONE place the two assignments above must stay
			// together: Failure becomes None only after Data is filled, so "Failure == None"
			// implies a valid payload and is the single source of truth for the drain.

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

Rml::CompiledFilterHandle FVaCuusRecordingRenderInterface::CompileFilter(const Rml::String& Name, const Rml::Dictionary& Parameters)
{
	CheckOwnerThread();

	// THE BLUR-ONLY POLICY (M5 spec §2(d)). RmlUi registers ten filter instancers
	// (Factory.cpp:216-226); v1 compiles exactly one. Returning 0 is the SAFE refusal by
	// RmlUi's own contract: RenderManager::CompileFilter wraps the handle into a live
	// CompiledFilter only when it is nonzero (RenderManager.cpp:274-283), so a zero never
	// reaches ReleaseFilter (CompiledFilter::Release guards on the invalid handle,
	// CompiledFilterShader.cpp:14-21) and never lands in a composite's filter list
	// (AddHandleTo skips the invalid handle, CompiledFilterShader.cpp:6-12). RmlUi then
	// warns per element ("Could not compile filter on element",
	// ElementEffects.cpp:153-165) and renders the element with the filter dropped — the
	// same discipline as an unknown decorator key.
	if (Name != "blur")
	{
		// Latched per TYPE, not per element: RmlUi's warning already names each element;
		// this line exists to say which types would have worked, once.
		bool bAlreadyRefused = false;
		RefusedFilterTypes.Add(UTF8_TO_TCHAR(Name.c_str()), &bAlreadyRefused);
		if (!bAlreadyRefused)
		{
			UE_LOG(LogVaCuus, Warning,
				TEXT("CompileFilter: filter type '%s' is not supported — VaCuus compiles 'blur' only; the effect is dropped per element"),
				UTF8_TO_TCHAR(Name.c_str()));
		}
		return Rml::CompiledFilterHandle(0);
	}

	// FilterBlur::CompileFilter sends exactly {"sigma": resolved length in px} — the
	// blur() length itself, no 0.5 factor (FilterBlur.cpp:16-20) — so blur(12px) at
	// dp-ratio 1.0 arrives as sigma 12.0 and is recorded verbatim.
	const float Sigma = Rml::Get(Parameters, "sigma", 0.0f);

	const FVaCuusFilterHandle Handle = NextFilterHandle++;
	ensureMsgf(Handle != 0, TEXT("Filter handle counter wrapped to the invalid sentinel"));

	FVaCuusFilterData& Data = GetPending().NewFilters.Add(Handle);
	Data.Sigma = Sigma;

	return Rml::CompiledFilterHandle(Handle);
}

void FVaCuusRecordingRenderInterface::ReleaseFilter(Rml::CompiledFilterHandle Filter)
{
	CheckOwnerThread();

	// Resource call, legal out of frame: ElementEffects::ReleaseEffects destroys compiled
	// filters at document teardown (ElementEffects.cpp:169-183), which runs from
	// Document->Close() outside any Begin/End pair. Same-frame compile+release keeps both
	// entries, exactly like geometry.
	GetPending().ReleasedFilters.Add(FVaCuusFilterHandle(Filter));
}

Rml::LayerHandle FVaCuusRecordingRenderInterface::PushLayer()
{
	CheckOwnerThread();
	if (!ensureMsgf(bInFrame, TEXT("PushLayer() outside BeginFrame/EndFrameAndPublish; call dropped")))
	{
		// 0 is the reserved base layer (RenderInterface.h:96) — a harmless answer for a
		// call that cannot legitimately happen (RmlUi only renders inside Context::Render).
		return Rml::LayerHandle(0);
	}

	const FVaCuusLayerHandle Handle = NextLayerHandle++;

	FVaCuusCommand& Command = GetPending().Commands.AddDefaulted_GetRef();
	Command.Type = EVaCuusCommandType::PushLayer;
	Command.SourceLayer = Handle;

	return Rml::LayerHandle(Handle);
}

void FVaCuusRecordingRenderInterface::CompositeLayers(Rml::LayerHandle Source, Rml::LayerHandle Destination, Rml::BlendMode BlendMode,
	Rml::Span<const Rml::CompiledFilterHandle> Filters)
{
	CheckOwnerThread();
	if (!ensureMsgf(bInFrame, TEXT("CompositeLayers() outside BeginFrame/EndFrameAndPublish; call dropped")))
	{
		return;
	}
	check(Filters.size() <= size_t(MAX_int32));

	FVaCuusCommandBuffer& Buffer = GetPending();

	// The filter list is recorded verbatim into the buffer-level side array — the
	// variable-length record. Zero handles cannot arrive here: every list RmlUi builds
	// goes through CompiledFilter::AddHandleTo, which skips the invalid handle
	// (CompiledFilterShader.cpp:6-12; list assembly at ElementEffects.cpp:269-271), so a
	// refused non-blur filter reaches this composite as an ABSENCE, not a zero.
	//
	// Appending to CompositeFilters cannot invalidate the Commands ref: two different
	// arrays.
	FVaCuusCommand& Command = Buffer.Commands.AddDefaulted_GetRef();
	Command.Type = EVaCuusCommandType::CompositeLayers;
	Command.SourceLayer = FVaCuusLayerHandle(Source);
	Command.DestLayer = FVaCuusLayerHandle(Destination);
	Command.Blend = EVaCuusBlendMode(BlendMode);
	Command.FilterOffset = Buffer.CompositeFilters.Num();
	Command.FilterCount = int32(Filters.size());
	for (const Rml::CompiledFilterHandle FilterHandle : Filters)
	{
		Buffer.CompositeFilters.Add(FVaCuusFilterHandle(FilterHandle));
	}
}

void FVaCuusRecordingRenderInterface::PopLayer()
{
	CheckOwnerThread();
	if (!ensureMsgf(bInFrame, TEXT("PopLayer() outside BeginFrame/EndFrameAndPublish; call dropped")))
	{
		return;
	}

	FVaCuusCommand& Command = GetPending().Commands.AddDefaulted_GetRef();
	Command.Type = EVaCuusCommandType::PopLayer;
}

void FVaCuusRecordingRenderInterface::EnableClipMask(bool bEnable)
{
	CheckOwnerThread();
	if (!ensureMsgf(bInFrame, TEXT("EnableClipMask() outside BeginFrame/EndFrameAndPublish; call dropped")))
	{
		return;
	}

	// BOTH edges are recorded, unlike the scissor's (EnableScissorRegion above): RmlUi
	// batches mask geometry through ApplyClipMask, whose enable call is the only signal
	// that a NEW mask list replaces the old one (RenderManager.cpp:156-176) — and the
	// disable edge is what delimits the mask's scope for Task 3's glass distiller.
	FVaCuusCommand& Command = GetPending().Commands.AddDefaulted_GetRef();
	Command.Type = EVaCuusCommandType::EnableClipMask;
	Command.bClipMaskEnable = bEnable ? 1 : 0;
}

void FVaCuusRecordingRenderInterface::RenderToClipMask(Rml::ClipMaskOperation Operation, Rml::CompiledGeometryHandle Geometry, Rml::Vector2f Translation)
{
	CheckOwnerThread();
	if (!ensureMsgf(bInFrame, TEXT("RenderToClipMask() outside BeginFrame/EndFrameAndPublish; call dropped")))
	{
		return;
	}

	// The mask geometry is ordinary compiled geometry — border-radius clip shapes arrive
	// through CompileGeometry like everything else (RenderManager::GetCompiledGeometryHandle,
	// RenderManager.cpp:197-212, reached from ApplyClipMask at :169-170) — so the handle
	// resolves in NewGeometry/earlier buffers and Task 3's distiller can read the mask's
	// vertices from the same place the replayer would.
	FVaCuusCommand& Command = GetPending().Commands.AddDefaulted_GetRef();
	Command.Type = EVaCuusCommandType::RenderToClipMask;
	Command.ClipMaskOp = EVaCuusClipMaskOp(Operation);
	Command.Geometry = FVaCuusGeometryHandle(Geometry);
	Command.Translation = FVector2f(Translation.x, Translation.y);
}

void FVaCuusRecordingRenderInterface::BeginFrame(FIntPoint ViewSize)
{
	ensureMsgf(!bInFrame, TEXT("BeginFrame() called twice without EndFrameAndPublish()"));

	// Keep the existing pending buffer: it may already hold resource traffic
	// recorded between frames (see class comment), which belongs to this frame.
	GetPending().ViewSize = ViewSize;
	OwnerThreadId = FPlatformTLS::GetCurrentThreadId();
	bInFrame = true;

	// Layer handles restart every frame — see the declaration for why this is what keeps
	// a static glass document idle-gated.
	NextLayerHandle = 1;

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

		// ONE source of truth for "did this decode succeed". This used to test
		// Data.Size.X <= 0 first and read Failure only to pick a message, so the same fact
		// lived in two fields kept in sync by convention; Failure alone is sufficient and
		// unambiguous (the worker sets it to None only after filling Data, see the task).
		if (Completed->Failure != EVaCuusTextureDecodeFailure::None)
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
	// layout. RenderManager carries no version counter either.
	//
	// GetNextUpdateDelay (Include/RmlUi/Core/Context.h:294) deserves more than the
	// brush-off it used to get here -- RmlUi presents it as exactly this kind of
	// mechanism, "used by elements and the application to implement ON-DEMAND RENDERING
	// and thus drastically save CPU/GPU" (:284-286, on RequestNextUpdate, which is what
	// feeds it). It still cannot answer the question this gate asks, for three reasons
	// its own contract states (:289-293): it is FORWARD-looking (when Update() should next
	// be called, not what the last frame produced); it returns INFINITY when the answer is
	// "wait for user input", which is the steady state of an idle document and says
	// nothing about whether that document was already drawn; and it is a schedule, so
	// acting on it means skipping Update()/Render() -- a different, larger feature than
	// withholding a publish, and one that would stop input and animation from being
	// serviced on time. So the frame is recorded as usual and compared afterwards.
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
	//
	// The predicate itself lives on the BUFFER, next to the arrays it names, and not
	// here: this gate is 500 lines from those declarations and has no compile-time link
	// to them, so a fifth resource array added for shaders or filters would have been
	// invisible from this site. See FVaCuusCommandBuffer::HasResourceTraffic.
	const bool bHasResourceTraffic = Published->HasResourceTraffic();
	const uint64 ContentHash = VaCuusHashFrameContent(*Published);

	// Generation > 0 == "this recorder has published at least once", so the first
	// frame of a view always publishes and never compares against an unset hash.
	// CVarVaCuusIdleGate is the kill switch; see its declaration.
	const bool bGateEnabled = CVarVaCuusIdleGate.GetValueOnAnyThread() != 0;
	if (bGateEnabled && Generation > 0 && ContentHash == LastPublishedContentHash && !bHasResourceTraffic)
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
