// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.
#include "Misc/AutomationTest.h"
#include "VaCuusBundle.h"
#include "VaCuusBundleMount.h"
#include "VaCuusEngine.h"
#include "VaCuusContentPaths.h"
#include "Misc/FileHelper.h"
#include "VaCuusRecordingRenderInterface.h"
#include "Misc/Base64.h"
#include "Misc/ScopeExit.h"
#include <RmlUi/Core.h>

#if WITH_DEV_AUTOMATION_TESTS
namespace VaCuusVectorTest
{
static const char* GAnimation = R"({"v":"5.7.4","fr":30,"ip":0,"op":60,"w":16,"h":16,"nm":"opacity probe","ddd":0,"assets":[],"layers":[{"ddd":0,"ind":1,"ty":1,"nm":"red","sr":1,"ks":{"o":{"a":1,"k":[{"t":0,"s":[100],"e":[0],"i":{"x":[1],"y":[1]},"o":{"x":[0],"y":[0]}},{"t":30,"s":[0]}]},"r":{"a":0,"k":0},"p":{"a":0,"k":[0,0,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},"sw":16,"sh":16,"sc":"#ff0000","ip":0,"op":60,"st":0,"bm":0}]})";
static const char* GImageAnimation = R"({"v":"5.7.4","fr":30,"ip":0,"op":60,"w":16,"h":16,"ddd":0,"assets":[{"id":"image","w":4,"h":4,"u":"","p":"pixel.png","e":0}],"layers":[{"ddd":0,"ind":1,"ty":2,"refId":"image","sr":1,"ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"p":{"a":0,"k":[0,0,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[400,400,100]}},"ip":0,"op":60,"st":0,"bm":0}]})";
static const char* GImageSvg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16"><image href="pixel.png" width="16" height="16"/></svg>)";

struct FBundle
{
	VaCuusBundleFormat::FCookedIndex Index;
	TArray64<uint8> Payload;
	void Add(const TCHAR* Path, const TArray<uint8>& Data)
	{
		const int64 Offset = Align(Payload.Num(), VaCuusBundleFormat::EntryAlignment);
		Payload.AddZeroed(Offset - Payload.Num());
		Payload.Append(Data.GetData(), Data.Num());
		Index.Entries.Add({VaCuusBundleFormat::NormalizePath(Path), Offset, Data.Num()});
		Index.PayloadSize = Payload.Num();
	}
	void Text(const TCHAR* Path, const char* Data)
	{
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Data), FCStringAnsi::Strlen(Data));
		Add(Path, Bytes);
	}
	bool Mount()
	{
		return FVaCuusBundleMountTable::MountTransient(TEXT("VectorTest"), TEXT("in-memory vector fixtures"),
			MoveTemp(Index), MoveTemp(Payload));
	}
};

class FClock : public Rml::SystemInterface
{
public:
	Rml::SystemInterface* Original = nullptr;
	double Time = 10.0;
	double GetElapsedTime() override { return Time; }
	void JoinPath(Rml::String& Out, const Rml::String& Base, const Rml::String& Path) override { Original->JoinPath(Out, Base, Path); }
	bool LogMessage(Rml::Log::Type Type, const Rml::String& Message) override { return Original->LogMessage(Type, Message); }
};

struct FFixture
{
	FVaCuusRecordingRenderInterface Recorder;
	FClock Clock;
	Rml::Context* Context = nullptr;
	bool bInitialized = false;
	TSet<FVaCuusTextureHandle> Alive;
	bool Init()
	{
		FVaCuusEngine::Get().SetRenderInterface(&Recorder);
		bInitialized = FVaCuusEngine::Get().Initialize();
		if (!bInitialized) return false;
		Clock.Original = Rml::GetSystemInterface();
		Rml::SetSystemInterface(&Clock);
		Context = Rml::CreateContext("vacuus_vector_test", {800, 600});
		return Context != nullptr;
	}
	~FFixture()
	{
		if (Context) Rml::RemoveContext("vacuus_vector_test");
		if (bInitialized)
		{
			Rml::SetSystemInterface(Clock.Original);
			FVaCuusEngine::Get().Shutdown();
		}
		FVaCuusEngine::Get().SetRenderInterface(nullptr);
	}
	TUniquePtr<FVaCuusCommandBuffer> Frame()
	{
		Recorder.BeginFrame({800, 600});
		if (Context) { Context->Update(); Context->Render(); }
		auto Buffer = Recorder.EndFrameAndPublish();
		if (Buffer)
		{
			for (const auto& Pair : Buffer->NewTextures) Alive.Add(Pair.Key);
			for (auto Handle : Buffer->ReleasedTextures) Alive.Remove(Handle);
		}
		return Buffer;
	}
	Rml::ElementDocument* Load(const char* Body)
	{
		const Rml::String Source = Rml::String("<rml><head><style>body{display:block;margin:0;}svg,lottie,div{display:block;width:16px;height:16px;}</style></head><body>") + Body + "</body></rml>";
		auto* Document = Context->LoadDocumentFromMemory(Source, "tests/vector/index.rml");
		if (Document) Document->Show();
		return Document;
	}
	void Close()
	{
		Rml::RemoveContext("vacuus_vector_test");
		Context = nullptr;
		Frame();
	}
};

static bool HasPixel(const FVaCuusCommandBuffer* Buffer, uint8 R, uint8 G, uint8 B, uint8 A, int32 Tolerance = 0)
{
	if (!Buffer) return false;
	for (const auto& Pair : Buffer->NewTextures)
	{
		const auto& Bytes = Pair.Value.RGBA;
		for (int32 I = 0; I + 3 < Bytes.Num(); I += 4)
			if (FMath::Abs(int32(Bytes[I]) - R) <= Tolerance && FMath::Abs(int32(Bytes[I+1]) - G) <= Tolerance && FMath::Abs(int32(Bytes[I+2]) - B) <= Tolerance && FMath::Abs(int32(Bytes[I+3]) - A) <= Tolerance) return true;
	}
	for (const auto& Pair : Buffer->NewTextures)
	{
		const auto& D = Pair.Value;
		const int32 Center = (D.Size.Y / 2 * D.Size.X + D.Size.X / 2) * 4;
		if (D.RGBA.IsValidIndex(Center + 3))
			UE_LOG(LogTemp, Display, TEXT("Vector texture %dx%d center RGBA=(%u,%u,%u,%u), wanted (%u,%u,%u,%u)"),
				D.Size.X, D.Size.Y, D.RGBA[Center], D.RGBA[Center+1], D.RGBA[Center+2], D.RGBA[Center+3], R, G, B, A);
	}
	return false;
}

static bool MountFixtures()
{
	FBundle Bundle;
	TArray<uint8> Font;
	if (!FFileHelper::LoadFileToArray(Font, *VaCuusContentPaths::ResolveExistingDocument(TEXT("fonts/LatoLatin-Regular.ttf")))) return false;
	Bundle.Add(TEXT("fonts/LatoLatin-Regular.ttf"), Font);
	TArray<uint8> Png;
	FBase64::Decode(TEXT("iVBORw0KGgoAAAANSUhEUgAAAAQAAAAECAYAAACp8Z5+AAAAEUlEQVR4nGNgaPjfgIJJFwAALmgf8YBkpkQAAAAASUVORK5CYII="), Png);
	Bundle.Add(TEXT("tests/vector/pixel.png"), Png);
	Bundle.Text(TEXT("tests/vector/picture.svg"), GImageSvg);
	Bundle.Text(TEXT("tests/vector/animation.json"), GAnimation);
	Bundle.Text(TEXT("tests/vector/image.json"), GImageAnimation);
	Bundle.Text(TEXT("tests/vector/invalid.json"), "{invalid json");
	Rml::String ZeroRate = GAnimation;
	ZeroRate.replace(ZeroRate.find("\"fr\":30"), 7, "\"fr\":0");
	Bundle.Text(TEXT("tests/vector/zero-rate.json"), ZeroRate.c_str());
	return Bundle.Mount();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusSvgTest, "VaCuus.Vector.SVG",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaCuusSvgTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusVectorTest;
	if (!TestFalse(TEXT("RmlUi is down"), FVaCuusEngine::Get().IsInitialized())) return false;
	if (!TestTrue(TEXT("memory-only resources mounted"), MountFixtures())) return false;
	ON_SCOPE_EXIT { FVaCuusBundleMountTable::UnmountAll(); };
	FFixture Fixture;
	if (!TestTrue(TEXT("initialized"), Fixture.Init())) return false;
	auto* Document = Fixture.Load(R"(<svg><rect width="16" height="16" fill="red" opacity="0.5"/></svg><svg id="file" src="picture.svg"/><div style="decorator:svg(picture.svg);"/>)");
	if (!TestNotNull(TEXT("document"), Document)) return false;
	auto First = Fixture.Frame();
	TestTrue(TEXT("inline SVG emits premultiplied RGBA"), HasPixel(First.Get(), 127, 0, 0, 127));
	TestTrue(TEXT("file SVG and nested PNG are served through the bundle"), HasPixel(First.Get(), 0, 64, 128, 128));
	if (First) TestEqual(TEXT("file element and decorator share one cached bitmap"), First->NewTextures.Num(), 2);
	Fixture.Frame();
	TestFalse(TEXT("static SVG settles to an idle frame"), Fixture.Frame().IsValid());
	Document->GetElementById("file")->SetProperty("width", "32px");
	auto Resized = Fixture.Frame();
	bool bResized = false;
	if (Resized) for (const auto& Pair : Resized->NewTextures) bResized |= Pair.Value.Size == FIntPoint(32, 16);
	TestTrue(TEXT("resizing rasterizes at the new dimensions"), bResized);

	Fixture.Load(R"(<svg width="64" height="32" style="width:64px;height:32px"><text x="0" y="24" font-family="LatoLatin" font-size="24" fill="#00ff00">A</text></svg>)");
	auto TextFrame = Fixture.Frame();
	bool bTextVisible = false;
	if (TextFrame) for (const auto& Pair : TextFrame->NewTextures)
		for (int32 I = 0; I + 3 < Pair.Value.RGBA.Num(); I += 4)
			bTextVisible |= Pair.Value.RGBA[I+1] > 0 && Pair.Value.RGBA[I+3] > 0;
	TestTrue(TEXT("SVG text uses the shipped VFS font without system font scanning"), bTextVisible);
	Fixture.Load(R"(<svg><image href="pixel.png" width="16" height="16"/></svg>)");
	auto InlineImage = Fixture.Frame();
	TestTrue(TEXT("inline SVG image paths use the owning RML directory"), HasPixel(InlineImage.Get(), 0, 64, 128, 128));

	// A second document retains the old file cache while a new document loads changed bytes at the same path.
	FVaCuusBundleMountTable::UnmountAll();
	FBundle Changed;
	Changed.Text(TEXT("tests/vector/picture.svg"), R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16"><rect width="16" height="16" fill="#00ff00"/></svg>)");
	if (!TestTrue(TEXT("replacement resource mounted"), Changed.Mount())) return false;
	Fixture.Load("<svg src=\"picture.svg\"/>");
	auto Reloaded = Fixture.Frame();
	TestTrue(TEXT("same path with changed bytes does not reuse a live stale SVG document"), HasPixel(Reloaded.Get(), 0, 255, 0, 255));
	Fixture.Close();
	TestEqual(TEXT("closing all documents releases every generated texture"), Fixture.Alive.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusLottieTest, "VaCuus.Vector.Lottie",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaCuusLottieTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusVectorTest;
	if (!TestFalse(TEXT("RmlUi is down"), FVaCuusEngine::Get().IsInitialized())) return false;
	if (!TestTrue(TEXT("memory-only resources mounted"), MountFixtures())) return false;
	ON_SCOPE_EXIT { FVaCuusBundleMountTable::UnmountAll(); };
	FFixture Fixture;
	if (!TestTrue(TEXT("initialized"), Fixture.Init())) return false;
	if (!TestNotNull(TEXT("document"), Fixture.Load("<lottie src=\"animation.json\"/><lottie src=\"image.json\"/>"))) return false;
	auto First = Fixture.Frame();
	TestTrue(TEXT("vector animation starts red, with correct channel order"), HasPixel(First.Get(), 255, 0, 0, 255));
	TestTrue(TEXT("external Lottie image uses the bundle, with premultiplied alpha"), HasPixel(First.Get(), 0, 64, 128, 128, 1));
	Fixture.Frame();
	TestFalse(TEXT("same animation frame generates no new resource traffic"), Fixture.Frame().IsValid());
	Fixture.Clock.Time += 0.5;
	auto Animated = Fixture.Frame();
	bool bChangedAlpha = false;
	if (Animated) for (const auto& Pair : Animated->NewTextures)
		for (int32 I = 0; I + 3 < Pair.Value.RGBA.Num(); I += 4)
			bChangedAlpha |= Pair.Value.RGBA[I] > 50 && Pair.Value.RGBA[I] < 220 && Pair.Value.RGBA[I] == Pair.Value.RGBA[I+3] && Pair.Value.RGBA[I+1] == 0;
	TestTrue(TEXT("advancing time changes the pixels and publishes the texture replacement"), bChangedAlpha);
	TestEqual(TEXT("animated replacement retires the previous resources"), Fixture.Alive.Num(), 2);
	Fixture.Close();
	TestEqual(TEXT("Lottie teardown releases generated textures"), Fixture.Alive.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusVectorLimitsTest, "VaCuus.Vector.Limits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaCuusVectorLimitsTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusVectorTest;
	if (!TestFalse(TEXT("RmlUi is down"), FVaCuusEngine::Get().IsInitialized())) return false;
	if (!TestTrue(TEXT("fixtures mounted"), MountFixtures())) return false;
	ON_SCOPE_EXIT { FVaCuusBundleMountTable::UnmountAll(); };
	FFixture Fixture;
	if (!TestTrue(TEXT("initialized"), Fixture.Init())) return false;
	Fixture.Load("<svg style=\"width:4097px\"><rect width=\"16\" height=\"16\"/></svg><lottie src=\"animation.json\" style=\"width:4097px\"/>");
	auto Oversized = Fixture.Frame();
	TestTrue(TEXT("oversized vectors allocate no bitmap"), Oversized && Oversized->NewTextures.IsEmpty());
	AddExpectedError(TEXT("Could not construct the lottie animation"), EAutomationExpectedErrorFlags::Contains, 2);
	Fixture.Load("<lottie src=\"invalid.json\"/><lottie src=\"zero-rate.json\"/>");
	auto Invalid = Fixture.Frame();
	TestTrue(TEXT("malformed JSON and zero frame rate allocate no bitmap"), !Invalid || Invalid->NewTextures.IsEmpty());
	Fixture.Close();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusLottieCostTest, "VaCuus.Vector.LottieCost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaCuusLottieCostTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusVectorTest;
	if (!TestFalse(TEXT("RmlUi is down"), FVaCuusEngine::Get().IsInitialized())) return false;
	if (!TestTrue(TEXT("fixtures mounted"), MountFixtures())) return false;
	ON_SCOPE_EXIT { FVaCuusBundleMountTable::UnmountAll(); };
	FFixture Fixture;
	if (!TestTrue(TEXT("initialized"), Fixture.Init())) return false;
	auto* Document = Fixture.Load("<lottie id=\"animation\" src=\"animation.json\"/>");
	if (!TestNotNull(TEXT("document"), Document)) return false;
	for (int32 Size : {64, 256, 512})
	{
		auto* Element = Document->GetElementById("animation");
		Element->SetProperty("width", TCHAR_TO_UTF8(*FString::Printf(TEXT("%dpx"), Size)));
		Element->SetProperty("height", TCHAR_TO_UTF8(*FString::Printf(TEXT("%dpx"), Size)));
		Fixture.Frame();
		double TotalMs = 0;
		int32 UploadedFrames = 0;
		for (int32 I = 0; I < 50; ++I)
		{
			Fixture.Clock.Time += 1.0 / 24.0;
			const double Start = FPlatformTime::Seconds();
			auto Buffer = Fixture.Frame();
			TotalMs += (FPlatformTime::Seconds() - Start) * 1000;
			UploadedFrames += Buffer && Buffer->NewTextures.Num() == 1;
		}
		TestEqual(TEXT("measurement rendered 50 changed frames, rather than timing cache hits"), UploadedFrames, 50);
		TestEqual(TEXT("only the current texture remains resident in the recorded stream"), Fixture.Alive.Num(), 1);
		AddInfo(FString::Printf(TEXT("Lottie %dx%d: mean UI update + rasterize + record %.4f ms; %.2f MiB RGBA/frame. Simple solid/opacity fixture, no GPU replay."),
			Size, Size, TotalMs / 50, double(Size * Size * 4) / (1024 * 1024)));
	}
	Fixture.Close();
	TestEqual(TEXT("cost fixture releases its final texture"), Fixture.Alive.Num(), 0);
	return true;
}
#endif
