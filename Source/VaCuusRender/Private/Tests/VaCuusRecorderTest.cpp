// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusCommandBuffer.h"
#include "VaCuusEngine.h"
#include "VaCuusRecordingRenderInterface.h"

#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Types.h>
#include <RmlUi/Core/Vertex.h>

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
/** Triangle fixture with one fully specified spot vertex ([0]). */
struct FVaCuusRecorderTriangle
{
	Rml::Vertex Vertices[3] = {};
	int Indices[3] = {0, 1, 2};

	FVaCuusRecorderTriangle()
	{
		Vertices[0].position = Rml::Vector2f(10.f, 20.f);
		Vertices[0].colour = Rml::ColourbPremultiplied(255, 128, 64, 255);
		Vertices[0].tex_coord = Rml::Vector2f(0.25f, 0.75f);
		Vertices[1].position = Rml::Vector2f(30.f, 40.f);
		Vertices[2].position = Rml::Vector2f(50.f, 60.f);
	}

	Rml::Span<const Rml::Vertex> VertexSpan() const { return Rml::Span<const Rml::Vertex>(Vertices, 3); }
	Rml::Span<const int> IndexSpan() const { return Rml::Span<const int>(Indices, 3); }
};

/**
 * Writes an RGBA8 PNG of distinct byte values with Alpha stamped into every
 * texel's A byte, and hands back the exact pre-premultiply pixels so the caller
 * can predict what LoadTexture must produce. PNG is lossless and stores
 * straight (non-premultiplied) alpha, so the decoded bytes are these bytes.
 */
bool SaveVaCuusProbePng(const FString& Path, FIntPoint Size, uint8 Alpha, TArray<uint8>& OutPixels)
{
	OutPixels.SetNumUninitialized(Size.X * Size.Y * 4);
	for (int32 Index = 0; Index < OutPixels.Num(); ++Index)
	{
		OutPixels[Index] = (Index % 4 == 3) ? Alpha : uint8(Index * 7 + 3);
	}

	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>("ImageWrapper");
	const TSharedPtr<IImageWrapper> Encoder = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
	if (!Encoder.IsValid() ||
		!Encoder->SetRaw(OutPixels.GetData(), OutPixels.Num(), Size.X, Size.Y, ERGBFormat::RGBA, 8))
	{
		return false;
	}

	return FFileHelper::SaveArrayToFile(Encoder->GetCompressed(), *Path);
}

/**
 * Writes an opaque baseline JPEG. Lossy, so the caller cannot predict the decoded colour
 * bytes — the point is the ALPHA byte, which the JPEG path has to supply itself.
 */
bool SaveVaCuusProbeJpeg(const FString& Path, FIntPoint Size)
{
	TArray<uint8> Pixels;
	Pixels.SetNumUninitialized(Size.X * Size.Y * 4);
	for (int32 Index = 0; Index < Pixels.Num(); ++Index)
	{
		Pixels[Index] = (Index % 4 == 3) ? 255 : uint8(Index * 7 + 3);
	}

	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>("ImageWrapper");
	const TSharedPtr<IImageWrapper> Encoder = ImageWrapperModule.CreateImageWrapper(EImageFormat::JPEG);
	if (!Encoder.IsValid() ||
		!Encoder->SetRaw(Pixels.GetData(), Pixels.Num(), Size.X, Size.Y, ERGBFormat::RGBA, 8))
	{
		return false;
	}

	return FFileHelper::SaveArrayToFile(Encoder->GetCompressed(90), *Path);
}

/**
 * Writes a small opaque image in a format LoadTexture must REFUSE, using the engine's own
 * encoder so the bytes are exactly what DetectImageFormat will sniff them as. No pixels
 * come back: nothing downstream is supposed to decode these.
 *
 * BGRA8 because that is the only raw format these two encoders accept — BMP asserts
 * check(Format == ERGBFormat::BGRA || Gray) and check(BitDepth == 8)
 * (BmpImageWrapper.cpp:575-576), TGA asserts check(Image.Format == ERawImageFormat::BGRA8)
 * (TgaImageWrapper.cpp:40). Both then write 24-bit output because the alpha is all 255
 * (BmpImageWrapper.cpp:597-609, TgaImageWrapper.cpp:44-45), which is fine: the magic
 * bytes are what matters ('BM' at ImageWrapperModule.cpp:175-178; TGA is recognised from
 * ColorMapType/ImageTypeCode alone, TgaImageWrapper.cpp:152-156).
 */
bool SaveVaCuusUnsupportedProbe(const FString& Path, EImageFormat ImageFormat, FIntPoint Size)
{
	TArray<uint8> Pixels;
	Pixels.SetNumUninitialized(Size.X * Size.Y * 4);
	for (int32 Index = 0; Index < Pixels.Num(); ++Index)
	{
		Pixels[Index] = (Index % 4 == 3) ? 255 : uint8(Index * 7 + 3);
	}

	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>("ImageWrapper");
	const TSharedPtr<IImageWrapper> Encoder = ImageWrapperModule.CreateImageWrapper(ImageFormat);
	if (!Encoder.IsValid() ||
		!Encoder->SetRaw(Pixels.GetData(), Pixels.Num(), Size.X, Size.Y, ERGBFormat::BGRA, 8))
	{
		return false;
	}

	const TArray64<uint8> Compressed = Encoder->GetCompressed();
	return Compressed.Num() > 0 && FFileHelper::SaveArrayToFile(Compressed, *Path);
}

/**
 * INDEPENDENT oracle for the recorder's premultiply — deliberately NOT the production
 * expression restated. Production computes the integer (c*a + 127) / 255; this computes
 * round(c*a / 255.0) in floating point.
 *
 * The two are EXACTLY equal, which is what makes this a real check rather than a
 * different-looking copy: round(n/255) == floor((n + 127.5)/255), and
 * floor((n + 127.5)/255) == floor((n + 127)/255) for every integer n because no integer
 * lies in the half-open interval (n+127, n+127.5]. The rounding mode is therefore
 * irrelevant too — a tie would need c*a == 255k + 127.5, which is not an integer, so no
 * halfway case can arise. Nor can the divide's rounding error matter: c*a <= 65025 is
 * exact in a double and the true quotient is at least 0.5/255 away from any half-integer.
 *
 * The point of the rewrite: the previous version was character-for-character identical to
 * the production line, so it could only ever fail on the loop SHAPE (stride, which
 * channels) — swap production to a truncating (c*a)/255 and it still passed. This version
 * disagrees on every value where truncation and rounding differ.
 */
TArray<uint8> PremultiplyVaCuusProbe(const TArray<uint8>& Straight)
{
	TArray<uint8> Result = Straight;
	for (int32 Index = 0; Index + 3 < Result.Num(); Index += 4)
	{
		const double A = double(Result[Index + 3]);
		for (int32 Channel = 0; Channel < 3; ++Channel)
		{
			Result[Index + Channel] = uint8(FMath::RoundToInt32(double(Result[Index + Channel]) * A / 255.0));
		}
	}
	return Result;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusRecorderCompileGeometryTest, "VaCuus.Render.Recorder.CompileGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusRecorderCompileGeometryTest::RunTest(const FString& Parameters)
{
	FVaCuusRecordingRenderInterface Recorder;
	Recorder.BeginFrame(FIntPoint(800, 600));

	const FVaCuusRecorderTriangle Triangle;
	const Rml::CompiledGeometryHandle First = Recorder.CompileGeometry(Triangle.VertexSpan(), Triangle.IndexSpan());
	const Rml::CompiledGeometryHandle Second = Recorder.CompileGeometry(Triangle.VertexSpan(), Triangle.IndexSpan());

	TestTrue(TEXT("First handle is non-zero"), First != Rml::CompiledGeometryHandle(0));
	TestTrue(TEXT("Second handle is non-zero"), Second != Rml::CompiledGeometryHandle(0));
	TestTrue(TEXT("Handles are distinct"), First != Second);

	const TUniquePtr<FVaCuusCommandBuffer> Buffer = Recorder.EndFrameAndPublish();
	if (!TestNotNull(TEXT("Published buffer"), Buffer.Get()))
	{
		return false;
	}

	TestEqual(TEXT("Both geometries land in NewGeometry"), Buffer->NewGeometry.Num(), 2);

	const FVaCuusGeometryData* Data = Buffer->NewGeometry.Find(FVaCuusGeometryHandle(First));
	if (!TestNotNull(TEXT("First geometry present"), Data))
	{
		return false;
	}

	TestEqual(TEXT("Vertex count"), Data->Vertices.Num(), 3);
	TestEqual(TEXT("Index count"), Data->Indices.Num(), 3);
	if (Data->Vertices.Num() == 3 && Data->Indices.Num() == 3)
	{
		TestTrue(TEXT("Spot vertex position"), Data->Vertices[0].Position == FVector2f(10.f, 20.f));
		TestTrue(TEXT("Spot vertex UV"), Data->Vertices[0].UV == FVector2f(0.25f, 0.75f));
		// Contract: the vertex stream is a bit-identical passthrough of Rml::Vertex.
		TestTrue(TEXT("Spot vertex bytes match source"),
			FMemory::Memcmp(&Data->Vertices[0], &Triangle.Vertices[0], sizeof(Rml::Vertex)) == 0);
		TestEqual(TEXT("Spot index"), Data->Indices[2], 2);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusRecorderReleaseGeometryTest, "VaCuus.Render.Recorder.ReleaseGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusRecorderReleaseGeometryTest::RunTest(const FString& Parameters)
{
	FVaCuusRecordingRenderInterface Recorder;
	Recorder.BeginFrame(FIntPoint(320, 240));

	const FVaCuusRecorderTriangle Triangle;
	const Rml::CompiledGeometryHandle Handle = Recorder.CompileGeometry(Triangle.VertexSpan(), Triangle.IndexSpan());
	Recorder.ReleaseGeometry(Handle);

	const TUniquePtr<FVaCuusCommandBuffer> Buffer = Recorder.EndFrameAndPublish();
	if (!TestNotNull(TEXT("Published buffer"), Buffer.Get()))
	{
		return false;
	}

	// Create+release within one frame is legal: the buffer must carry BOTH the
	// NewGeometry entry and the released handle so the replayer can create the
	// resource for this frame's commands and retire it afterwards.
	TestTrue(TEXT("NewGeometry keeps the same-frame entry"), Buffer->NewGeometry.Contains(FVaCuusGeometryHandle(Handle)));
	TestTrue(TEXT("ReleasedGeometry carries the handle"), Buffer->ReleasedGeometry.Contains(FVaCuusGeometryHandle(Handle)));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusRecorderPublishTest, "VaCuus.Render.Recorder.Publish",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusRecorderPublishTest::RunTest(const FString& Parameters)
{
	FVaCuusRecordingRenderInterface Recorder;

	// Frame 1: one compiled geometry, one draw.
	Recorder.BeginFrame(FIntPoint(640, 480));
	const FVaCuusRecorderTriangle Triangle;
	const Rml::CompiledGeometryHandle Handle = Recorder.CompileGeometry(Triangle.VertexSpan(), Triangle.IndexSpan());
	Recorder.RenderGeometry(Handle, Rml::Vector2f(5.f, 7.f), Rml::TextureHandle(0));

	const TUniquePtr<FVaCuusCommandBuffer> First = Recorder.EndFrameAndPublish();
	if (!TestNotNull(TEXT("First buffer"), First.Get()))
	{
		return false;
	}

	TestTrue(TEXT("First buffer view size"), First->ViewSize == FIntPoint(640, 480));
	TestEqual(TEXT("First buffer has the draw"), First->Commands.Num(), 1);
	if (First->Commands.Num() == 1)
	{
		const FVaCuusCommand& Command = First->Commands[0];
		TestTrue(TEXT("Draw command type"), Command.Type == EVaCuusCommandType::DrawGeometry);
		TestTrue(TEXT("Draw command geometry"), Command.Geometry == FVaCuusGeometryHandle(Handle));
		TestTrue(TEXT("Draw command is untextured"), Command.Texture == FVaCuusTextureHandle(0));
		TestTrue(TEXT("Draw command translation"), Command.Translation == FVector2f(5.f, 7.f));
	}

	// Frame 2: nothing recorded -> fresh empty buffer, strictly newer generation.
	Recorder.BeginFrame(FIntPoint(640, 480));
	const TUniquePtr<FVaCuusCommandBuffer> Second = Recorder.EndFrameAndPublish();
	if (!TestNotNull(TEXT("Second buffer"), Second.Get()))
	{
		return false;
	}

	TestTrue(TEXT("Generation increases monotonically"), Second->Generation > First->Generation);
	TestEqual(TEXT("No stale commands"), Second->Commands.Num(), 0);
	TestEqual(TEXT("No stale geometry"), Second->NewGeometry.Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusRecorderLoadTextureTest, "VaCuus.Render.Recorder.LoadTexture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusRecorderLoadTextureTest::RunTest(const FString& Parameters)
{
	// LoadTexture resolves sources through Rml::GetFileInterface(), so the
	// library must be booted (installs FVaCuusFileInterface; absolute paths
	// pass through). The null-file-interface early-out stays untested: it can
	// only occur pre-boot and would require poking RmlUi globals directly.
	FVaCuusEngine& Engine = FVaCuusEngine::Get();
	if (!TestTrue(TEXT("Initialized"), Engine.Initialize()))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Engine.Shutdown();
	};

	const FString TestDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VaCuusTest"));
	const FString PngPath = TestDir / TEXT("recorder_probe.png");
	const FString AlphaPngPath = TestDir / TEXT("recorder_alpha.png");
	const FString ReleasedPngPath = TestDir / TEXT("recorder_released.png");
	const FString FakePngPath = TestDir / TEXT("recorder_fake.png");
	const FString JpegPath = TestDir / TEXT("recorder_probe.jpg");
	const FString BmpPath = TestDir / TEXT("recorder_probe.bmp");
	const FString TgaPath = TestDir / TEXT("recorder_probe.tga");
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*PngPath);
		IFileManager::Get().Delete(*AlphaPngPath);
		IFileManager::Get().Delete(*ReleasedPngPath);
		IFileManager::Get().Delete(*FakePngPath);
		IFileManager::Get().Delete(*JpegPath);
		IFileManager::Get().Delete(*BmpPath);
		IFileManager::Get().Delete(*TgaPath);
		IFileManager::Get().DeleteDirectory(*TestDir);
	};

	// Two 4x2 probes with the same byte pattern and different alpha: A=255 keeps
	// the premultiply a no-op (byte-exact round trip), A=128 actually exercises
	// the round-to-nearest divide.
	const FIntPoint ProbeSize(4, 2);
	TArray<uint8> OpaquePixels;
	TArray<uint8> AlphaPixels;
	TArray<uint8> ReleasedPixels;
	if (!TestTrue(TEXT("Opaque probe PNG saved"), SaveVaCuusProbePng(PngPath, ProbeSize, 255, OpaquePixels)) ||
		!TestTrue(TEXT("Translucent probe PNG saved"), SaveVaCuusProbePng(AlphaPngPath, ProbeSize, 128, AlphaPixels)) ||
		!TestTrue(TEXT("Released-probe PNG saved"), SaveVaCuusProbePng(ReleasedPngPath, ProbeSize, 255, ReleasedPixels)) ||
		!TestTrue(TEXT("Probe JPEG saved"), SaveVaCuusProbeJpeg(JpegPath, FIntPoint(8, 8))))
	{
		return false;
	}

	FVaCuusRecordingRenderInterface Recorder;
	Recorder.BeginFrame(FIntPoint(64, 64));

	// Step 11.1: the dimension probe is SYNCHRONOUS. These reads happen before
	// any drain point, so no decode can have contributed to them.
	Rml::Vector2i Dimensions(0, 0);
	const Rml::TextureHandle Handle = Recorder.LoadTexture(Dimensions, Rml::String(TCHAR_TO_UTF8(*PngPath)));
	TestTrue(TEXT("LoadTexture returns non-zero handle"), Handle != Rml::TextureHandle(0));
	TestTrue(TEXT("Dimensions are probed synchronously (4x2)"), Dimensions.x == 4 && Dimensions.y == 2);

	Rml::Vector2i AlphaDimensions(0, 0);
	const Rml::TextureHandle AlphaHandle = Recorder.LoadTexture(AlphaDimensions, Rml::String(TCHAR_TO_UTF8(*AlphaPngPath)));
	TestTrue(TEXT("Translucent load returns non-zero handle"), AlphaHandle != Rml::TextureHandle(0));
	TestTrue(TEXT("Translucent dimensions are probed synchronously (4x2)"), AlphaDimensions.x == 4 && AlphaDimensions.y == 2);

	// JPEG: the only format whose decoder does not write the A byte at all. Asserted
	// below that every alpha byte comes back 255.
	Rml::Vector2i JpegDimensions(0, 0);
	const Rml::TextureHandle JpegHandle = Recorder.LoadTexture(JpegDimensions, Rml::String(TCHAR_TO_UTF8(*JpegPath)));
	TestTrue(TEXT("JPEG load returns non-zero handle"), JpegHandle != Rml::TextureHandle(0));
	TestTrue(TEXT("JPEG dimensions are probed synchronously (8x8)"), JpegDimensions.x == 8 && JpegDimensions.y == 8);

	// Release-before-arrival: retired in the very frame that started its decode,
	// so the completion below must be dropped rather than resurrecting a texture
	// the replayer has already retired. Race-free without any sleep: the drain
	// that would install the payload cannot run before the release.
	Rml::Vector2i ReleasedDimensions(0, 0);
	const Rml::TextureHandle ReleasedHandle = Recorder.LoadTexture(ReleasedDimensions, Rml::String(TCHAR_TO_UTF8(*ReleasedPngPath)));
	TestTrue(TEXT("Released-probe load returns non-zero handle"), ReleasedHandle != Rml::TextureHandle(0));
	Recorder.ReleaseTexture(ReleasedHandle);

	// Failure: missing file.
	Rml::Vector2i MissingDimensions(0, 0);
	TestTrue(TEXT("Missing file yields zero handle"),
		Recorder.LoadTexture(MissingDimensions, Rml::String(TCHAR_TO_UTF8(*(TestDir / TEXT("does_not_exist.png"))))) == Rml::TextureHandle(0));

	// Failure: extension lies, content is plain text -> format detection fails.
	if (TestTrue(TEXT("Fake PNG saved"), FFileHelper::SaveStringToFile(TEXT("this is not a png"), *FakePngPath)))
	{
		Rml::Vector2i FakeDimensions(0, 0);
		TestTrue(TEXT("Non-image content yields zero handle"),
			Recorder.LoadTexture(FakeDimensions, Rml::String(TCHAR_TO_UTF8(*FakePngPath))) == Rml::TextureHandle(0));
	}

	// Failure: a PERFECTLY VALID image in a format the decode cannot ask for as RGBA8.
	// These two are why LoadTexture whitelists formats at all, and they are BOTH here
	// because they fail in two different ways:
	//  - BMP took the whole editor down. FBmpImageWrapper::UncompressBMPData opens with
	//    check(InFormat == ERGBFormat::BGRA) (BmpImageWrapper.cpp:69), which is fatal in
	//    every DO_CHECK build and used to fire on a DECODE WORKER, so a document with one
	//    <img src="*.bmp"> killed the process. Nothing before the whitelist stopped it: the
	//    'BM' sniff succeeds (ImageWrapperModule.cpp:175-178) and FBmpImageWrapper::
	//    SetCompressed only reads the header, so the synchronous probe was happy.
	//  - TGA was the silent one, and the reason the fix is a whitelist rather than a
	//    BMP/DDS blocklist: FTgaImageWrapper::Uncompress ignores InFormat and always writes
	//    BGRA order (TgaImageWrapper.cpp:131-133). The byte count is still W*H*4, so every
	//    check on the worker passed and red and blue simply came out swapped. A test that
	//    only covered the crash would not have pinned the shape of the fix.
	if (TestTrue(TEXT("Probe BMP saved"), SaveVaCuusUnsupportedProbe(BmpPath, EImageFormat::BMP, FIntPoint(4, 2))))
	{
		Rml::Vector2i BmpDimensions(0, 0);
		TestTrue(TEXT("A valid BMP yields zero handle instead of crashing a decode worker"),
			Recorder.LoadTexture(BmpDimensions, Rml::String(TCHAR_TO_UTF8(*BmpPath))) == Rml::TextureHandle(0));
	}
	if (TestTrue(TEXT("Probe TGA saved"), SaveVaCuusUnsupportedProbe(TgaPath, EImageFormat::TGA, FIntPoint(4, 2))))
	{
		Rml::Vector2i TgaDimensions(0, 0);
		TestTrue(TEXT("A valid TGA yields zero handle instead of channel-swapped pixels"),
			Recorder.LoadTexture(TgaDimensions, Rml::String(TCHAR_TO_UTF8(*TgaPath))) == Rml::TextureHandle(0));
	}

	// A draw issued in the same frame as the load: the replayer resolves textures
	// out of its handle map, so the placeholder has to be in THIS buffer's
	// NewTextures or the replayer's unknown-handle ensure fires.
	const FVaCuusRecorderTriangle Triangle;
	const Rml::CompiledGeometryHandle Geometry = Recorder.CompileGeometry(Triangle.VertexSpan(), Triangle.IndexSpan());
	Recorder.RenderGeometry(Geometry, Rml::Vector2f(0.f, 0.f), Handle);

	const TUniquePtr<FVaCuusCommandBuffer> First = Recorder.EndFrameAndPublish();
	if (!TestNotNull(TEXT("First published buffer"), First.Get()))
	{
		return false;
	}

	TestEqual(TEXT("Four placeholders recorded, nothing for the failed loads"), First->NewTextures.Num(), 4);

	// BOTH placeholders are checked, not just the first: a load path that decoded
	// synchronously on its second call would otherwise slip through with the 4x2 payload
	// already in this buffer.
	const auto TestIsPlaceholder = [this](const TCHAR* Label, const FVaCuusTextureData* Entry)
	{
		if (!TestNotNull(Label, Entry))
		{
			return;
		}
		TestTrue(FString::Printf(TEXT("%s is 1x1"), Label), Entry->Size == FIntPoint(1, 1));
		TestEqual(FString::Printf(TEXT("%s is one texel"), Label), Entry->RGBA.Num(), 4);
		const bool bTransparent = Entry->RGBA.Num() == 4 && Entry->RGBA[0] == 0 &&
			Entry->RGBA[1] == 0 && Entry->RGBA[2] == 0 && Entry->RGBA[3] == 0;
		TestTrue(FString::Printf(TEXT("%s texel is premultiplied transparent (0,0,0,0)"), Label), bTransparent);
	};

	const FVaCuusTextureData* Placeholder = First->NewTextures.Find(FVaCuusTextureHandle(Handle));
	TestIsPlaceholder(TEXT("Opaque-probe placeholder"), Placeholder);
	TestIsPlaceholder(TEXT("Translucent-probe placeholder"),
		First->NewTextures.Find(FVaCuusTextureHandle(AlphaHandle)));

	const FVaCuusCommand* Draw = First->Commands.FindByPredicate(
		[](const FVaCuusCommand& Command) { return Command.Type == EVaCuusCommandType::DrawGeometry; });
	if (TestNotNull(TEXT("Frame recorded the draw"), Draw))
	{
		TestTrue(TEXT("Draw references the loaded handle"), Draw->Texture == FVaCuusTextureHandle(Handle));
		TestTrue(TEXT("A not-yet-ready handle still resolves in NewTextures"), First->NewTextures.Contains(Draw->Texture));
	}

	// Same-frame create+release keeps both, exactly as for geometry: the
	// commands above were recorded while the handle was still live.
	TestTrue(TEXT("Released-in-flight handle is retired by this buffer"),
		First->ReleasedTextures.Contains(FVaCuusTextureHandle(ReleasedHandle)));

	// Deterministic: wait on the decode tasks themselves, then let the next
	// BeginFrame() drain their payloads. Generous timeout, hard failure on expiry.
	if (!TestTrue(TEXT("Async decodes completed within the timeout"),
			Recorder.WaitForTextureDecodes(FTimespan::FromSeconds(30.0))))
	{
		return false;
	}

	Recorder.BeginFrame(FIntPoint(64, 64));
	const TUniquePtr<FVaCuusCommandBuffer> Second = Recorder.EndFrameAndPublish();
	if (!TestNotNull(TEXT("Second published buffer"), Second.Get()))
	{
		return false;
	}

	// "Placeholder in this buffer, payload in the next" is deterministic HERE only
	// because every load above happened inside a BeginFrame/EndFrameAndPublish pair,
	// which puts the drain that installs the payload strictly after this frame's
	// publish. It is NOT a general invariant: out-of-frame loads (RmlUi's lazy
	// FileTextureDatabase::EnsureLoaded during Document->Show()) can land the payload
	// in the same buffer as the placeholder. See DrainCompletedDecodes.
	//
	// Pinned so that a spurious extra arrival — or a payload for the released handle —
	// cannot go unnoticed: three loads survive to be decoded, the fourth was released.
	TestEqual(TEXT("Exactly the three surviving payloads arrive"), Second->NewTextures.Num(), 3);

	const FVaCuusTextureData* Opaque = Second->NewTextures.Find(FVaCuusTextureHandle(Handle));
	if (TestNotNull(TEXT("Opaque payload lands under the same handle"), Opaque))
	{
		TestTrue(TEXT("Opaque payload size is 4x2"), Opaque->Size == FIntPoint(4, 2));
		TestEqual(TEXT("Opaque payload is 32 bytes"), Opaque->RGBA.Num(), 32);
		TestTrue(TEXT("A=255 keeps premultiply a no-op, so the decode round-trips losslessly"), Opaque->RGBA == OpaquePixels);
	}

	const FVaCuusTextureData* Translucent = Second->NewTextures.Find(FVaCuusTextureHandle(AlphaHandle));
	if (TestNotNull(TEXT("Translucent payload lands under the same handle"), Translucent))
	{
		TestTrue(TEXT("Translucent payload size is 4x2"), Translucent->Size == FIntPoint(4, 2));
		TestTrue(TEXT("A<255 is premultiplied with round-to-nearest"), Translucent->RGBA == PremultiplyVaCuusProbe(AlphaPixels));

		// One value stated as ground truth by hand, so the test does not depend on ANY
		// implementation of the formula: the fixture's texel 0 red is Index*7+3 = 3 with
		// A = 128, and 3*128/255 = 1.5059, which rounds to 2 and truncates to 1. This
		// single byte is what separates round-to-nearest from the biased alternative.
		if (Translucent->RGBA.Num() == 32)
		{
			TestEqual(TEXT("Texel 0 red: c=3, A=128 -> 2 (hand-computed)"), int32(Translucent->RGBA[0]), 2);
		}
	}

	// JPEG carries no alpha channel, so A=255 is what a correct decode must produce. The
	// recorder stamps it rather than reading it, and this pins the result.
	//
	// WHAT THIS ASSERTION DOES AND DOES NOT PROVE: it does NOT prove the stamp is load-
	// bearing, and the stamp is not. Deleting it would leave this passing, because
	// turbojpeg DOCUMENTS the guarantee: TJPF_RGBA — which is what UE maps
	// ERGBFormat::RGBA to (JpegImageWrapper.cpp:53) — is TJPF_RGBX "except that when
	// decompressing, the X component is guaranteed to be equal to the maximum sample
	// value, which can be interpreted as an opaque alpha channel"
	// (ThirdParty/libjpeg-turbo/3.0.0/include/turbojpeg.h:264-269). Beware the engine's
	// own comment at the head of that same switch, which says libjpeg-turbo "does not
	// actually read/write A" (JpegImageWrapper.cpp:47-48): that is true of COMPRESS
	// (turbojpeg.h:233-234) and stale for decompress, and it has already misled one
	// reviewer of this file into calling the guarantee an implementation accident.
	//
	// What this DOES buy is coverage of the JPEG branch itself — a stamp that corrupted
	// colours, skipped texels or mis-strided would fail on the non-alpha bytes below —
	// and a tripwire if the mapping at JpegImageWrapper.cpp:53 ever moves to an X
	// variant, where the byte really would be undefined. Colours are only
	// sanity-checked, not pinned: JPEG is lossy.
	const FVaCuusTextureData* Jpeg = Second->NewTextures.Find(FVaCuusTextureHandle(JpegHandle));
	if (TestNotNull(TEXT("JPEG payload lands under the same handle"), Jpeg))
	{
		TestTrue(TEXT("JPEG payload size is 8x8"), Jpeg->Size == FIntPoint(8, 8));
		TestEqual(TEXT("JPEG payload is 256 bytes"), Jpeg->RGBA.Num(), 8 * 8 * 4);
		if (Jpeg->RGBA.Num() == 8 * 8 * 4)
		{
			int32 OpaqueTexels = 0;
			int32 NonBlackChannels = 0;
			for (int32 Index = 0; Index + 3 < Jpeg->RGBA.Num(); Index += 4)
			{
				OpaqueTexels += (Jpeg->RGBA[Index + 3] == 255) ? 1 : 0;
				NonBlackChannels += (Jpeg->RGBA[Index] != 0 || Jpeg->RGBA[Index + 1] != 0 || Jpeg->RGBA[Index + 2] != 0) ? 1 : 0;
			}
			TestEqual(TEXT("Every JPEG texel is opaque (A=255), so premultiply is a no-op"), OpaqueTexels, 64);
			TestEqual(TEXT("The JPEG actually decoded (no texel left black)"), NonBlackChannels, 64);
		}
	}

	// The leak this guards: installing this payload would create an RHI texture
	// whose ReleasedTextures entry was already consumed by the first buffer.
	TestFalse(TEXT("A handle released while in flight never receives its payload"),
		Second->NewTextures.Contains(FVaCuusTextureHandle(ReleasedHandle)));

	return true;
}

/**
 * Base that drops LogImageWrapper from the automation log capture.
 *
 * A file that parses but will not decode makes libpng and FImageWrapperBase log — at
 * Error verbosity, FROM THE DECODE WORKER. The harness attributes any thread's log to
 * whatever test is CurTest when the line is written (AutomationTest.cpp:229-230), so an
 * Error we cannot prevent would fail this test. AddExpectedError is the usual answer and
 * is the WRONG one here: it matches per test, not per thread, and it asserts an
 * occurrence count over messages whose number and wording are libpng's business, not
 * ours (PngImageWrapper.cpp:723 per libpng error, :421 after the longjmp,
 * ImageWrapperBase.cpp:68 for the GetRaw failure, PngImageWrapper.cpp:691 per short
 * read). Dropping the whole category at the capture gate
 * (AutomationTest.cpp:232-234, ShouldCaptureLogCategory at :234) is wording-, count- and
 * thread-independent, and it still leaves LogVaCuus captured, which is what the test
 * actually asserts on.
 */
class FVaCuusImageWrapperQuietTest : public FAutomationTestBase
{
public:
	FVaCuusImageWrapperQuietTest(const FString& InName, const bool bInComplexTask)
		: FAutomationTestBase(InName, bInComplexTask)
	{
	}

	virtual bool ShouldCaptureLogCategory(const FName& Category) const override
	{
		return Category != TEXT("LogImageWrapper");
	}
};

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FVaCuusRecorderDecodeFailureTest, FVaCuusImageWrapperQuietTest,
	"VaCuus.Render.Recorder.DecodeFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusRecorderDecodeFailureTest::RunTest(const FString& Parameters)
{
	// The one NEW user-visible failure mode async decode introduced: a file that reads
	// and PARSES — so LoadTexture returns a live handle and real dimensions, and RmlUi
	// lays the element out — but will not decode. Two decisions are pinned here:
	//   1. the handle KEEPS its 1x1 transparent placeholder forever; retiring it would
	//      trip the replayer's unknown-handle ensure on every subsequent frame, because
	//      RmlUi still owns the handle and keeps drawing with it;
	//   2. the failure is logged EXACTLY ONCE, not once per frame.
	FVaCuusEngine& Engine = FVaCuusEngine::Get();
	if (!TestTrue(TEXT("Initialized"), Engine.Initialize()))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Engine.Shutdown();
	};

	const FString TestDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VaCuusTest"));
	const FString TruncatedPngPath = TestDir / TEXT("recorder_truncated.png");
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*TruncatedPngPath);
		IFileManager::Get().DeleteDirectory(*TestDir);
	};

	// Fixture: a valid RGBA8 PNG cut so that IHDR survives and IDAT does not. The cut is
	// made relative to the IDAT TAG rather than at a fixed offset, because which chunks
	// libpng writes before IDAT is libpng's choice, not ours.
	//
	// WHAT +8 ACTUALLY KEEPS, since a PNG chunk is length(4), type(4), data(length),
	// CRC(4) — the length comes BEFORE the type, so the scan below finds the tag with its
	// length already behind it. Keeping IdatTagIndex + 8 therefore keeps the 4-byte tag
	// plus the first 4 bytes of the compressed DATA, and drops the rest of the data and
	// the CRC. The declared length is intact and larger than what remains, which is
	// exactly the shape that makes the read below run off the end.
	//
	// What each stage then does: DetectImageFormat passes on the 8-byte signature;
	// SetCompressed -> LoadPNGHeader passes because png_read_info only needs IHDR and the
	// IDAT header; GetRaw -> UncompressPNGData then reads past the end, which
	// user_read_compressed refuses with SetError (PngImageWrapper.cpp:689-694) — so GetRaw
	// returns false on the non-empty LastError (ImageWrapperBase.cpp:66-70) whether or not
	// libpng itself longjmps first. Failure is therefore deterministic, not
	// libpng-version-dependent.
	TArray<uint8> WholePixels;
	const FString WholePngPath = TestDir / TEXT("recorder_whole_for_truncation.png");
	if (!TestTrue(TEXT("Source PNG saved"), SaveVaCuusProbePng(WholePngPath, FIntPoint(16, 16), 255, WholePixels)))
	{
		return false;
	}
	TArray<uint8> PngBytes;
	const bool bLoaded = FFileHelper::LoadFileToArray(PngBytes, *WholePngPath);
	IFileManager::Get().Delete(*WholePngPath);
	if (!TestTrue(TEXT("Source PNG read back"), bLoaded))
	{
		return false;
	}

	int32 IdatTagIndex = INDEX_NONE;
	for (int32 Index = 0; Index + 4 <= PngBytes.Num(); ++Index)
	{
		if (PngBytes[Index] == 'I' && PngBytes[Index + 1] == 'D' && PngBytes[Index + 2] == 'A' && PngBytes[Index + 3] == 'T')
		{
			IdatTagIndex = Index;
			break;
		}
	}
	if (!TestTrue(TEXT("IDAT chunk found in the fixture"), IdatTagIndex != INDEX_NONE) ||
		!TestTrue(TEXT("IDAT is followed by more data than the truncation keeps"), PngBytes.Num() > IdatTagIndex + 8))
	{
		return false;
	}
	PngBytes.SetNum(IdatTagIndex + 8, EAllowShrinking::No);
	if (!TestTrue(TEXT("Truncated PNG saved"), FFileHelper::SaveArrayToFile(PngBytes, *TruncatedPngPath)))
	{
		return false;
	}

	// The property under test, asserted through the harness: one line, not one per frame.
	// Registered as an expectation rather than merely tolerated, so a drain that logged
	// twice (or not at all) fails the test on the count.
	AddExpectedMessagePlain(TEXT("LoadTexture: async decode of"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);

	FVaCuusRecordingRenderInterface Recorder;
	Recorder.BeginFrame(FIntPoint(64, 64));

	Rml::Vector2i Dimensions(0, 0);
	const Rml::TextureHandle Handle = Recorder.LoadTexture(Dimensions, Rml::String(TCHAR_TO_UTF8(*TruncatedPngPath)));
	if (!TestTrue(TEXT("A parseable file still yields a live handle"), Handle != Rml::TextureHandle(0)))
	{
		Recorder.EndFrameAndPublish();
		return false;
	}
	TestTrue(TEXT("The header probe still reports the real size (16x16)"), Dimensions.x == 16 && Dimensions.y == 16);

	const TUniquePtr<FVaCuusCommandBuffer> First = Recorder.EndFrameAndPublish();
	if (!TestNotNull(TEXT("First published buffer"), First.Get()))
	{
		return false;
	}

	const FVaCuusTextureData* Placeholder = First->NewTextures.Find(FVaCuusTextureHandle(Handle));
	if (TestNotNull(TEXT("Placeholder recorded for the doomed handle"), Placeholder))
	{
		TestTrue(TEXT("Placeholder is 1x1"), Placeholder->Size == FIntPoint(1, 1));
	}

	if (!TestTrue(TEXT("The failing decode finished within the timeout"),
			Recorder.WaitForTextureDecodes(FTimespan::FromSeconds(30.0))))
	{
		return false;
	}

	// Frame 2 drains the failure, and since M2 Task 12 publishes NOTHING -- which is a
	// STRONGER assertion than the two this replaces ("no payload arrived", "the handle
	// was not retired"), because both of those are resource traffic and any resource
	// traffic at all forces a publish (EndFrameAndPublish's gate). A null buffer
	// therefore proves NewTextures and ReleasedTextures are both empty; a non-null one
	// would mean the drain smuggled something through.
	//
	// It is also the proof that the short-circuit gates the PUBLISH and not the RECORD:
	// the drain ran on a frame that was never published, and the Occurrences=1
	// expectation registered above is only satisfiable if it did.
	Recorder.BeginFrame(FIntPoint(64, 64));
	const TUniquePtr<FVaCuusCommandBuffer> Second = Recorder.EndFrameAndPublish();
	TestNull(TEXT("A failed decode leaves nothing to publish"), Second.Get());
	TestEqual(TEXT("The withheld frame consumed no generation"), int32(Recorder.GetNumFramesPublished()), 1);
	TestEqual(TEXT("...and was counted as skipped"), int32(Recorder.GetNumFramesSkipped()), 1);

	// Frame 3 proves the log is not per-frame: the drain forgot the handle in frame 2, so
	// there is nothing left to report. The Occurrences=1 expectation above fails if this
	// frame produces a second line.
	Recorder.BeginFrame(FIntPoint(64, 64));
	const TUniquePtr<FVaCuusCommandBuffer> Third = Recorder.EndFrameAndPublish();
	TestNull(TEXT("Still nothing to publish a frame later"), Third.Get());
	TestEqual(TEXT("Two withheld frames, one publish"), int32(Recorder.GetNumFramesSkipped()), 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusRecorderIntegrationTest, "VaCuus.Render.Recorder.Integration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusRecorderIntegrationTest::RunTest(const FString& Parameters)
{
	FVaCuusRecordingRenderInterface Recorder;

	FVaCuusEngine& Engine = FVaCuusEngine::Get();
	if (!TestFalse(TEXT("Engine must be down before the recorder is installed"), Engine.IsInitialized()))
	{
		return false;
	}
	Engine.SetRenderInterface(&Recorder); // Must land before the first Initialize().
	if (!TestTrue(TEXT("Initialized"), Engine.Initialize()))
	{
		Engine.SetRenderInterface(nullptr);
		return false;
	}

	// Restores the engine to its pre-test state on every exit path below.
	ON_SCOPE_EXIT
	{
		Engine.Shutdown();
		Engine.SetRenderInterface(nullptr);
	};

	Rml::Context* Context = Rml::CreateContext("recorder_test", Rml::Vector2i(800, 600));
	if (!TestNotNull(TEXT("Context"), Context))
	{
		return false;
	}

	// ~20 elements of styled text; explicit font-family/display so the default
	// style sheet gap does not spam warnings.
	Rml::String DocumentSource =
		"<rml><head><style>"
		"body { font-family: LatoLatin; display: block; width: 800px; height: 600px; background-color: #202020; }"
		"div { display: block; font-size: 16px; color: #e0e0e0; margin: 2px; padding: 2px; background-color: #303030; }"
		"</style></head><body>";
	for (int32 Index = 0; Index < 20; ++Index)
	{
		DocumentSource += "<div>Row " + Rml::ToString(Index) + "</div>";
	}
	DocumentSource += "</body></rml>";

	Rml::ElementDocument* Document = Context->LoadDocumentFromMemory(DocumentSource);
	if (!TestNotNull(TEXT("Document"), Document))
	{
		Rml::RemoveContext("recorder_test");
		return false;
	}
	Document->Show();

	Recorder.BeginFrame(FIntPoint(800, 600));
	Context->Update();
	Context->Render();
	const TUniquePtr<FVaCuusCommandBuffer> Buffer = Recorder.EndFrameAndPublish();

	if (TestNotNull(TEXT("Published buffer"), Buffer.Get()))
	{
		TestTrue(TEXT("Frame recorded draw commands"), Buffer->Commands.Num() > 0);
		TestTrue(TEXT("Font atlas texture recorded"), Buffer->NewTextures.Num() >= 1);
	}

	// Full teardown with the recorder still installed: RmlUi releases geometry
	// and textures outside any frame here, which must not trip the recorder.
	Document->Close();
	Rml::RemoveContext("recorder_test");
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
