// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusCommandBuffer.h"
#include "VaCuusEngine.h"
#include "VaCuusRecordingRenderInterface.h"

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusRecorderIntegrationTest, "VaCuus.Render.Recorder.Integration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusRecorderIntegrationTest::RunTest(const FString& Parameters)
{
	FVaCuusRecordingRenderInterface Recorder;

	FVaCuusEngine& Engine = FVaCuusEngine::Get();
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
