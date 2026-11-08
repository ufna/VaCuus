// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusCommandBuffer.h"
#include "VaCuusEngine.h"
#include "VaCuusMaterialBoxDecorator.h"
#include "VaCuusRecordingRenderInterface.h"
#include "VaCuus.h"
#include "VaCuusStyleSet.h"
#include "VaCuusTestDocumentHost.h"
#include "VaCuusTestLogCapture.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include <RmlUi/Core.h>

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusMaterialBoxTest
{
using VaCuusMaterialBox::FParams;

static FParams MakeParams(float Top, float Right, float Bottom, float Left, bool bBorder = false)
{
	FParams Params;
	Params.Key = "test";
	Params.Margin[0] = Top;
	Params.Margin[1] = Right;
	Params.Margin[2] = Bottom;
	Params.Margin[3] = Left;
	Params.bBorder = bBorder;
	return Params;
}

static Rml::Mesh Build(const FParams& Params, Rml::Vector2f Offset, Rml::Vector2f Size, float W, float H)
{
	Rml::Mesh Mesh;
	VaCuusMaterialBox::BuildMesh(Params, Offset, Size, W, H, Rml::ColourbPremultiplied(255, 255), Mesh);
	return Mesh;
}

/** Column i, row j of the 4x4 grid. */
static const Rml::Vertex& At(const Rml::Mesh& Mesh, int32 i, int32 j)
{
	return Mesh.vertices[j * 4 + i];
}
} // namespace VaCuusMaterialBoxTest

/**
 * The grid against FSlateElementBatcher::AddBoxElements (ElementBatcher.cpp:821-874): cuts at
 * ImageSize * Margin px with UV = margin, crossing cuts meet in the middle with their UVs kept,
 * zero margin is one 0..1 quad, border drops the centre cell.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMaterialBoxGeometryTest, "VaCuus.Render.MaterialBox.Geometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusMaterialBoxGeometryTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMaterialBoxTest;

	// Wide box, 24 px image, margin 0.5: 12 px corners, UV 0 / 0.5 / 0.5 / 1, offset applied.
	{
		const Rml::Mesh Mesh = Build(MakeParams(0.5f, 0.5f, 0.5f, 0.5f), {10.f, 20.f}, {400.f, 100.f}, 24.f, 24.f);
		TestEqual(TEXT("wide: 16 vertices"), int32(Mesh.vertices.size()), 16);
		TestEqual(TEXT("wide: 54 indices"), int32(Mesh.indices.size()), 54);
		TestEqual(TEXT("wide: x0 is the paint box's left edge"), At(Mesh, 0, 0).position.x, 10.f);
		TestEqual(TEXT("wide: x1 = offset + W*mL"), At(Mesh, 1, 0).position.x, 22.f);
		TestEqual(TEXT("wide: x2 = offset + size - W*mR"), At(Mesh, 2, 0).position.x, 398.f);
		TestEqual(TEXT("wide: x3 is the right edge"), At(Mesh, 3, 0).position.x, 410.f);
		TestEqual(TEXT("wide: y1"), At(Mesh, 0, 1).position.y, 32.f);
		TestEqual(TEXT("wide: y2"), At(Mesh, 0, 2).position.y, 108.f);
		TestEqual(TEXT("wide: u at the left cut is the margin"), At(Mesh, 1, 0).tex_coord.x, 0.5f);
		TestEqual(TEXT("wide: u at the right cut is 1 - margin"), At(Mesh, 2, 0).tex_coord.x, 0.5f);
		TestEqual(TEXT("wide: u at the right edge"), At(Mesh, 3, 0).tex_coord.x, 1.f);
	}

	// Box smaller than two corners: cuts meet in the middle, UVs unchanged (Slate squishes).
	{
		const Rml::Mesh Mesh = Build(MakeParams(0.5f, 0.5f, 0.5f, 0.5f), {0.f, 0.f}, {16.f, 16.f}, 24.f, 24.f);
		TestEqual(TEXT("small: x1 squished to the middle"), At(Mesh, 1, 0).position.x, 8.f);
		TestEqual(TEXT("small: x2 squished to the middle"), At(Mesh, 2, 0).position.x, 8.f);
		TestEqual(TEXT("small: u keeps the margin"), At(Mesh, 1, 0).tex_coord.x, 0.5f);
	}

	// Asymmetric margin, CSS order (top right bottom left), two image dimensions.
	{
		const Rml::Mesh Mesh = Build(MakeParams(0.25f, 0.5f, 0.125f, 0.25f), {0.f, 0.f}, {200.f, 100.f}, 32.f, 64.f);
		TestEqual(TEXT("asym: left cut = W*mL"), At(Mesh, 1, 0).position.x, 8.f);
		TestEqual(TEXT("asym: right cut = size - W*mR"), At(Mesh, 2, 0).position.x, 184.f);
		TestEqual(TEXT("asym: top cut = H*mT"), At(Mesh, 0, 1).position.y, 16.f);
		TestEqual(TEXT("asym: bottom cut = size - H*mB"), At(Mesh, 0, 2).position.y, 92.f);
		TestEqual(TEXT("asym: v at the bottom cut"), At(Mesh, 0, 2).tex_coord.y, 0.875f);
	}

	// Fractional offset: inner cuts land on whole pixels, the outer edges stay as laid out.
	{
		const Rml::Mesh Mesh = Build(MakeParams(0.5f, 0.5f, 0.5f, 0.5f), {0.4f, 0.f}, {100.3f, 50.f}, 21.f, 21.f);
		TestEqual(TEXT("fraction: outer left edge unrounded"), At(Mesh, 0, 0).position.x, 0.4f);
		TestEqual(TEXT("fraction: inner cut rounded"), At(Mesh, 1, 0).position.x, 11.f);
		TestEqual(TEXT("fraction: outer right edge unrounded"), At(Mesh, 3, 0).position.x, 100.7f);
	}

	// Zero margin: DrawAs=Image, one 0..1 quad.
	{
		const Rml::Mesh Mesh = Build(MakeParams(0.f, 0.f, 0.f, 0.f), {5.f, 5.f}, {50.f, 20.f}, 24.f, 24.f);
		TestEqual(TEXT("image: 4 vertices"), int32(Mesh.vertices.size()), 4);
		TestEqual(TEXT("image: 6 indices"), int32(Mesh.indices.size()), 6);
	}

	// Border: the centre cell is left out.
	{
		const Rml::Mesh Mesh = Build(MakeParams(0.5f, 0.5f, 0.5f, 0.5f, true), {0.f, 0.f}, {100.f, 100.f}, 24.f, 24.f);
		TestEqual(TEXT("border: 48 indices"), int32(Mesh.indices.size()), 48);
		bool bCentreUsed = false;
		for (size_t Index = 0; Index + 2 < Mesh.indices.size(); Index += 3)
		{
			const int32 A = Mesh.indices[Index], B = Mesh.indices[Index + 1], C = Mesh.indices[Index + 2];
			// The centre cell is the only one whose triangles touch only vertices 5, 6, 9, 10.
			auto Inner = [](int32 V) { return V == 5 || V == 6 || V == 9 || V == 10; };
			bCentreUsed |= Inner(A) && Inner(B) && Inner(C);
		}
		TestFalse(TEXT("border: no triangle covers the centre"), bCentreUsed);
	}

	{
		const Rml::Mesh Mesh = Build(MakeParams(0.25f, 0.f, 0.25f, 0.f), {0.4f, 0.4f}, {100.2f, 50.2f}, 24.f, 24.f);
		TestEqual(TEXT("zero left margin stays at the fractional outer edge"), At(Mesh, 1, 0).position.x, At(Mesh, 0, 0).position.x);
		TestEqual(TEXT("zero right margin stays at the fractional outer edge"), At(Mesh, 2, 0).position.x, At(Mesh, 3, 0).position.x);
	}
	{
		AddExpectedMessagePlain(TEXT("border exceeds 4096 edge tiles"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
		const Rml::Mesh Mesh = Build(MakeParams(0.25f, 0.25f, 0.25f, 0.25f, true), {0.f, 0.f}, {256.f, 128.f}, 0.1f, 0.1f);
		TestTrue(TEXT("excessive border tiles leave no partial mesh"), Mesh.vertices.empty() && Mesh.indices.empty());
	}

	return true;
}

namespace VaCuusMaterialBoxDocTest
{
static const FIntPoint GViewSize(400, 300);
static const TCHAR* GMaterialPath = TEXT("/VaCuus/Spike/M_VaCuusSpike_Translucent.M_VaCuusSpike_Translucent");

/** What one recorded frame of a one-element document carried. */
struct FRecorded
{
	TArray<FVaCuusShaderDesc> Shaders;
	TArray<FVaCuusGeometryData> Geometry;
};

/**
 * The real pipeline minus the UI thread: RmlUi booted here (which registers material-box), a
 * recording context, the style key resolved from the installed snapshot. Each
 * Record() loads `<div id="box">` styled by Css (200 x 100 content, 6 px padding), records one
 * frame and closes the document.
 */
class FHarness
{
public:
	FHarness(FAutomationTestBase& InTest)
		: Test(InTest)
	{
	}

	bool Start()
	{
		if (!Test.TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
		{
			return false;
		}
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, GMaterialPath);
		if (!Test.TestNotNull(TEXT("committed MD_UI spike material loads"), Material))
		{
			return false;
		}
		UVaCuusStyleSet* StyleSet = NewObject<UVaCuusStyleSet>(GetTransientPackage());
		StyleSet->Materials.Add(TEXT("test-mat"), Material);
		StyleSetRoot.Reset(StyleSet);
		FVaCuusStyleRegistry::RegisterStyleSet(StyleSet);
		FVaCuusStyleRegistry::InstallSnapshot(FVaCuusStyleRegistry::GetSnapshot_GameThread());

		if (!Test.TestTrue(TEXT("RmlUi initialises"), FVaCuusEngine::Get().Initialize()))
		{
			return false;
		}
		Context = Rml::CreateContext("vacuus_material_box_test", Rml::Vector2i(GViewSize.X, GViewSize.Y), &Recorder);
		return Test.TestNotNull(TEXT("Context"), Context);
	}

	~FHarness()
	{
		if (Context)
		{
			Rml::RemoveContext("vacuus_material_box_test");
		}
		if (FVaCuusEngine::Get().IsInitialized())
		{
			FVaCuusEngine::Get().Shutdown();
		}
		if (StyleSetRoot.IsValid())
		{
			FVaCuusStyleRegistry::UnregisterStyleSet(StyleSetRoot.Get());
			FlushRenderingCommands();
			FVaCuusStyleRegistry::TickDeferredReleases_GameThread();
		}
	}

	FRecorded Record(const TCHAR* Css, float DpRatio = 1.0f)
	{
		Context->SetDensityIndependentPixelRatio(DpRatio);
		const FString Source = FString::Printf(TEXT("<rml><head><style>body{display:block;width:100%%;height:100%%;}")
			TEXT("#box{display:block;position:absolute;left:20px;top:30px;width:200px;height:100px;padding:6px;%s}")
			TEXT("</style></head><body><div id=\"box\"/></body></rml>"), Css);
		FRecorded Out;
		Rml::ElementDocument* Document = Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(*Source)), "vacuus://material_box_test.rml");
		if (!Document)
		{
			return Out;
		}
		Document->Show();
		Recorder.BeginFrame(GViewSize);
		Context->Update();
		Context->Render();
		if (const TUniquePtr<FVaCuusCommandBuffer> Buffer = Recorder.EndFrameAndPublish())
		{
			for (const TPair<FVaCuusShaderHandle, FVaCuusShaderDesc>& Pair : Buffer->NewShaders)
			{
				Out.Shaders.Add(Pair.Value);
			}
			for (const TPair<FVaCuusGeometryHandle, FVaCuusGeometryData>& Pair : Buffer->NewGeometry)
			{
				Out.Geometry.Add(Pair.Value);
			}
		}
		// Closed inside a recorded frame, so the releases land where the recorder expects them.
		++GFrameCounter;
		Recorder.BeginFrame(GViewSize);
		Document->Close();
		Context->Update();
		Context->Render();
		Recorder.EndFrameAndPublish();
		++GFrameCounter;
		return Out;
	}

	/** Vertex (i, j) of a 4x4 grid. */
	static const FVaCuusVertex& At(const FVaCuusGeometryData& Grid, int32 i, int32 j) { return Grid.Vertices[j * 4 + i]; }

private:
	FAutomationTestBase& Test;
	TStrongObjectPtr<UVaCuusStyleSet> StyleSetRoot;
	FVaCuusRecordingRenderInterface Recorder;
	Rml::Context* Context = nullptr;
};
} // namespace VaCuusMaterialBoxDocTest

/**
 * From RCSS to the recorder: the desc material-box mints is shader()'s plus paint_offset (the
 * paint box's origin in the geometry's space), the mesh is the sliced grid at the dp ratio, the
 * vertex colour is image-color x opacity, and stock shader() still sends no paint_offset.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMaterialBoxDescTest, "VaCuus.Render.MaterialBox.Desc",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusMaterialBoxDescTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMaterialBoxDocTest;

	FHarness Harness(*this);
	if (!Harness.Start())
	{
		return false;
	}

	// content-box: the paint box starts inside the 6 px padding.
	{
		const FRecorded R = Harness.Record(TEXT("decorator: material-box(test-mat, 24dp, 0.5) content-box;"));
		if (TestEqual(TEXT("box: one shader"), R.Shaders.Num(), 1) && TestEqual(TEXT("box: one geometry"), R.Geometry.Num(), 1))
		{
			const FVaCuusShaderDesc& Desc = R.Shaders[0];
			TestEqual(TEXT("box: Kind is Material"), int32(Desc.Kind), int32(EVaCuusShaderKind::Material));
			TestEqual(TEXT("box: paint_offset sent"), int32(Desc.bHasPaintOffset), 1);
			TestTrue(TEXT("box: PaintOffset is the content box's origin (6, 6)"), Desc.PaintOffset.Equals(FVector2f(6.f, 6.f), 0.01f));
			TestTrue(TEXT("box: Dimensions are the content box (200 x 100)"), Desc.Dimensions.Equals(FVector2f(200.f, 100.f), 0.01f));

			const FVaCuusGeometryData& Grid = R.Geometry[0];
			if (TestEqual(TEXT("box: 16 vertices"), Grid.Vertices.Num(), 16))
			{
				TestEqual(TEXT("box: 54 indices"), Grid.Indices.Num(), 54);
				TestTrue(TEXT("box: first inner cut at offset + 12"), FHarness::At(Grid, 1, 1).Position.Equals(FVector2f(18.f, 18.f), 0.01f));
				TestTrue(TEXT("box: its UV is the margin"), FHarness::At(Grid, 1, 1).UV.Equals(FVector2f(0.5f, 0.5f), 0.001f));
				TestEqual(TEXT("box: default tint is white at opacity 1"), FHarness::At(Grid, 0, 0).Color, FColor(255, 255, 255, 255));
			}
		}
	}

	// dp ratio 1.5: the 12dp corner is 18 px.
	{
		const FRecorded R = Harness.Record(TEXT("decorator: material-box(test-mat, 24dp, 0.5) content-box;"), 1.5f);
		if (R.Geometry.Num() == 1 && R.Geometry[0].Vertices.Num() == 16)
		{
			TestTrue(TEXT("dp 1.5: inner cut at offset + 18"), FMath::IsNearlyEqual(FHarness::At(R.Geometry[0], 1, 0).Position.X, 6.f + 18.f, 0.01f));
		}
		else
		{
			AddError(TEXT("dp 1.5: expected one 16-vertex grid"));
		}
	}

	// Tint: image-color x opacity, premultiplied.
	{
		const FRecorded R = Harness.Record(TEXT("decorator: material-box(test-mat, 24dp, 0.5); image-color: #ff0000; opacity: 0.5;"));
		if (R.Geometry.Num() == 1 && R.Geometry[0].Vertices.Num() > 0)
		{
			const FColor C = R.Geometry[0].Vertices[0].Color;
			// The recorder memcpys Rml's r,g,b,a bytes into FColor's memory order (B,G,R,A):
			// the static_asserts at the top of VaCuusRecordingRenderInterface.cpp. So red is .B.
			AddInfo(FString::Printf(TEXT("tint bytes r,g,b,a: (%d, %d, %d, %d)"), C.B, C.G, C.R, C.A));
			TestTrue(TEXT("tint: red at half opacity, premultiplied"), FMath::Abs(int32(C.B) - 128) <= 1 && C.G == 0 && C.R == 0 && FMath::Abs(int32(C.A) - 128) <= 1);
		}
		else
		{
			AddError(TEXT("tint: expected geometry"));
		}
	}

	// Stock shader() on the same key: no paint_offset, slot 4 stays tex_coord.
	{
		const FRecorded R = Harness.Record(TEXT("decorator: shader(test-mat);"));
		if (TestEqual(TEXT("shader(): one shader"), R.Shaders.Num(), 1))
		{
			TestEqual(TEXT("shader(): no paint_offset"), int32(R.Shaders[0].bHasPaintOffset), 0);
		}
	}

	return true;
}

/**
 * The RCSS contract: every accepted form of the shorthand, and every refusal is a warning on one
 * element, never a crash.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMaterialBoxParseTest, "VaCuus.Render.MaterialBox.Parse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusMaterialBoxParseTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMaterialBoxDocTest;

	FHarness Harness(*this);
	if (!Harness.Start())
	{
		return false;
	}

	auto Grid = [this](const FRecorded& R, const TCHAR* Case) -> const FVaCuusGeometryData*
	{
		if (R.Geometry.Num() != 1 || R.Geometry[0].Vertices.Num() != 16)
		{
			AddError(FString::Printf(TEXT("%s: expected one 16-vertex grid, got %d geometries"), Case, R.Geometry.Num()));
			return nullptr;
		}
		return &R.Geometry[0];
	};

	// Two image dimensions, two margins (CSS: vertical, horizontal). Paint area: padding-box,
	// the decorator default, so the box starts at (0, 0) and is 212 x 112.
	const FRecorded TwoValues = Harness.Record(TEXT("decorator: material-box(test-mat, 32px 16px, 0.25 0.5);"));
	if (const FVaCuusGeometryData* G = Grid(TwoValues, TEXT("two values")))
	{
		TestTrue(TEXT("two values: left cut = 32 * 0.5"), FMath::IsNearlyEqual(FHarness::At(*G, 1, 0).Position.X, 16.f, 0.01f));
		TestTrue(TEXT("two values: top cut = 16 * 0.25"), FMath::IsNearlyEqual(FHarness::At(*G, 0, 1).Position.Y, 4.f, 0.01f));
		TestTrue(TEXT("two values: u at the left cut = 0.5"), FMath::IsNearlyEqual(FHarness::At(*G, 1, 0).UV.X, 0.5f, 0.001f));
		TestTrue(TEXT("two values: v at the top cut = 0.25"), FMath::IsNearlyEqual(FHarness::At(*G, 0, 1).UV.Y, 0.25f, 0.001f));
	}

	// Four margins, CSS order: top right bottom left.
	const FRecorded FourValues = Harness.Record(TEXT("decorator: material-box(test-mat, 40px, 0.1 0.2 0.3 0.4);"));
	if (const FVaCuusGeometryData* G = Grid(FourValues, TEXT("four values")))
	{
		TestTrue(TEXT("four values: v at the top cut = top"), FMath::IsNearlyEqual(FHarness::At(*G, 0, 1).UV.Y, 0.1f, 0.001f));
		TestTrue(TEXT("four values: u at the right cut = 1 - right"), FMath::IsNearlyEqual(FHarness::At(*G, 2, 0).UV.X, 0.8f, 0.001f));
		TestTrue(TEXT("four values: v at the bottom cut = 1 - bottom"), FMath::IsNearlyEqual(FHarness::At(*G, 0, 2).UV.Y, 0.7f, 0.001f));
		TestTrue(TEXT("four values: u at the left cut = left"), FMath::IsNearlyEqual(FHarness::At(*G, 1, 0).UV.X, 0.4f, 0.001f));
	}

	// border: the centre cell is left out.
	{
		const FRecorded R = Harness.Record(TEXT("decorator: material-box(test-mat, 24dp, 0.5, border);"));
		if (R.Geometry.Num() == 1)
		{
			TestEqual(TEXT("border: 48 indices"), R.Geometry[0].Indices.Num(), 48);
		}
		else
		{
			AddError(TEXT("border: expected one geometry"));
		}
	}

	// Margin out of range: clamped with a warning, still drawn.
	AddExpectedMessagePlain(TEXT("material-box(test-mat): a margin is outside 0..1, clamped"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TestEqual(TEXT("clamped margin still draws"), Harness.Record(TEXT("decorator: material-box(test-mat, 24dp, 1.5);")).Geometry.Num(), 1);

	// Refusals: one element without a decorator, a warning, no crash.
	AddExpectedMessagePlain(TEXT("material-box: needs a style key and a positive image-size"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	TestEqual(TEXT("zero image-size draws nothing"), Harness.Record(TEXT("decorator: material-box(test-mat, 0px, 0.5);")).Geometry.Num(), 0);

	AddExpectedMessagePlain(TEXT("Syntax error parsing property declaration 'decorator: material-box(test-mat)"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	TestEqual(TEXT("a missing argument draws nothing"), Harness.Record(TEXT("decorator: material-box(test-mat);")).Geometry.Num(), 0);

	// A length where the margin's fraction goes: the "number" parser refuses it, so the whole
	// declaration is dropped (what `vacuus lint` rule material-box-args warns about).
	AddExpectedMessagePlain(TEXT("Syntax error parsing property declaration 'decorator: material-box(test-mat, 24dp, 12px)"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TestEqual(TEXT("a px margin draws nothing"), Harness.Record(TEXT("decorator: material-box(test-mat, 24dp, 12px);")).Geometry.Num(), 0);

	// An unknown keyword in the OPTIONAL last slot is dropped by RmlUi's RecursiveCommaSeparated
	// shorthand without a warning, so the element draws as `box`. Pinned here so a change shows.
	{
		const FRecorded Unknown = Harness.Record(TEXT("decorator: material-box(test-mat, 24dp, 0.5, frame);"));
		TestTrue(TEXT("an unknown draw-as keyword falls back to box"), Unknown.Geometry.Num() == 1 && Unknown.Geometry[0].Indices.Num() == 54);
	}

	AddExpectedMessagePlain(TEXT("no-such-key"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	AddExpectedMessagePlain(TEXT("Could not generate decorator element data"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	TestEqual(TEXT("an unknown style key draws nothing"), Harness.Record(TEXT("decorator: material-box(no-such-key, 24dp, 0.5);")).Geometry.Num(), 0);

	return true;
}

/** vacuus.MaterialBox 0: the same declaration draws one 0..1 quad (DrawAs=Image), the desc unchanged. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMaterialBoxKillSwitchTest, "VaCuus.Render.MaterialBox.KillSwitch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusMaterialBoxKillSwitchTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMaterialBoxDocTest;

	IConsoleVariable* Switch = IConsoleManager::Get().FindConsoleVariable(TEXT("vacuus.MaterialBox"));
	if (!TestNotNull(TEXT("vacuus.MaterialBox exists"), Switch))
	{
		return false;
	}
	const int32 Saved = Switch->GetInt();
	TestEqual(TEXT("vacuus.MaterialBox ships default 1"), Saved, 1);
	ON_SCOPE_EXIT
	{
		Switch->Set(Saved, ECVF_SetByCode);
	};

	FHarness Harness(*this);
	if (!Harness.Start())
	{
		return false;
	}

	Switch->Set(0, ECVF_SetByCode);
	const FRecorded R = Harness.Record(TEXT("decorator: material-box(test-mat, 24dp, 0.5);"));
	if (TestEqual(TEXT("off: one geometry"), R.Geometry.Num(), 1))
	{
		TestEqual(TEXT("off: a quad, 4 vertices"), R.Geometry[0].Vertices.Num(), 4);
		TestEqual(TEXT("off: 6 indices"), R.Geometry[0].Indices.Num(), 6);
	}
	if (TestEqual(TEXT("off: one shader"), R.Shaders.Num(), 1))
	{
		TestEqual(TEXT("off: the desc still carries paint_offset"), int32(R.Shaders[0].bHasPaintOffset), 1);
	}

	Switch->Set(1, ECVF_SetByCode);
	const FRecorded On = Harness.Record(TEXT("decorator: material-box(test-mat, 24dp, 0.5);"));
	TestTrue(TEXT("on again: the grid is back"), On.Geometry.Num() == 1 && On.Geometry[0].Vertices.Num() == 16);

	return true;
}

namespace VaCuusMaterialBoxRegisteredTest
{
class FDocHost final : public FVaCuusTestDocumentHost
{
public:
	FDocHost()
		: FVaCuusTestDocumentHost(TEXT("material_box_registered"), "vacuus://material_box_registered.rml", Rml::FocusFlag::Document)
	{
	}

	virtual void SetVisible(bool) override {}

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());
		Context->Update();
		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}
};
} // namespace VaCuusMaterialBoxRegisteredTest

/**
 * End to end, across a restart: a document on the module's own UI thread parses material-box
 * on the first boot AND after StopUIThread() + a new boot. FVaCuusEngine registers it per boot
 * (the instancer's parsers belong to that RmlUi instance), so the second leg is the one a
 * once-only registration, or one that kept the old instancer, would fail.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMaterialBoxRegisteredTest, "VaCuus.Render.MaterialBox.Registered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusMaterialBoxRegisteredTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMaterialBoxRegisteredTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no UI thread to drive"));
		return true;
	}

	FVaCuusModule& Module = FVaCuusModule::Get();
	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	for (int32 Boot = 1; Boot <= 2; ++Boot)
	{
		Module.StopUIThread();
		FVaCuusTestLogCapture Log;
		FVaCuusUIThread* UIThread = Module.GetOrStartUIThread();
		if (!TestNotNull(*FString::Printf(TEXT("boot %d: UI thread"), Boot), UIThread))
		{
			return false;
		}

		const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
		const uint32 ViewId = UIThread->AllocateViewId();
		UIThread->EnqueueAddView(ViewId, MakeUnique<FDocHost>(), FIntPoint(200, 100), Status);
		UIThread->EnqueueLoadDocumentFromMemory(ViewId, TEXT("<rml><head><style>body { display: block; } div { display: block; width: 50px; height: 20px; decorator: material-box(any-key, 24dp, 0.5); }</style></head><body><div/></body></rml>"), /*LoadSerial=*/1);

		const double Deadline = FPlatformTime::Seconds() + 10.0;
		while (Status->LoadCompletedSerial.load(std::memory_order_acquire) < 1 && FPlatformTime::Seconds() < Deadline)
		{
			const uint64 Before = UIThread->GetFrameCount();
			UIThread->Trigger();
			UIThread->WaitForFrameCount(Before + 1, 2.0);
		}
		TestEqual(*FString::Printf(TEXT("boot %d: the document loaded"), Boot), Status->LoadResult.load(std::memory_order_relaxed),
			uint8(EVaCuusLoadResult::Succeeded));
		TestEqual(*FString::Printf(TEXT("boot %d: material-box is a known decorator"), Boot),
			Log.Count(TEXT("Decorator type 'material-box' not found")), 0);
		TestEqual(*FString::Printf(TEXT("boot %d: material-box parses"), Boot),
			Log.Count(TEXT("Syntax error parsing property declaration 'decorator")), 0);

		UIThread->EnqueueRemoveView(ViewId);
	}
	return true;
}


// Stock shader() uses the render-box outline (DecoratorShader.cpp:43, MeshUtilities.cpp:125-126).
// Keep that coverage for both sliced meshes and the documented zero-margin equivalent.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMaterialBoxRoundedCornersTest, "VaCuus.Render.MaterialBox.RoundedCorners",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusMaterialBoxRoundedCornersTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMaterialBoxDocTest;
	FHarness Harness(*this);
	if (!Harness.Start())
	{
		return false;
	}
	auto Covers = [](const FVaCuusGeometryData& Mesh, FVector2f P)
	{
		auto Cross = [](FVector2f A, FVector2f B, FVector2f C)
		{
			return (B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X);
		};
		for (int32 I = 0; I + 2 < Mesh.Indices.Num(); I += 3)
		{
			const FVector2f A = Mesh.Vertices[Mesh.Indices[I]].Position;
			const FVector2f B = Mesh.Vertices[Mesh.Indices[I + 1]].Position;
			const FVector2f C = Mesh.Vertices[Mesh.Indices[I + 2]].Position;
			if (FMath::Abs(Cross(A, B, C)) < UE_SMALL_NUMBER)
			{
				continue;
			}
			const float AB = Cross(A, B, P), BC = Cross(B, C, P), CA = Cross(C, A, P);
			if ((AB >= 0 && BC >= 0 && CA >= 0) || (AB <= 0 && BC <= 0 && CA <= 0))
			{
				return true;
			}
		}
		return false;
	};
	for (const TCHAR* Css : {
		TEXT("decorator: shader(test-mat); border-radius: 24px;"),
		TEXT("decorator: material-box(test-mat, 24px, 0); border-radius: 24px;"),
		TEXT("decorator: material-box(test-mat, 24px, 0.5); border-radius: 24px;"),
		TEXT("decorator: material-box(test-mat, 24px, 0.5); border-radius: 24px 18px 12px 6px;")})
	{
		const FRecorded R = Harness.Record(Css);
		if (!TestEqual(Css, R.Geometry.Num(), 1))
		{
			continue;
		}
		TestTrue(*FString::Printf(TEXT("interior drawn: %s"), Css), Covers(R.Geometry[0], FVector2f(50, 50)));
		for (const FVector2f Corner : {FVector2f(1, 1), FVector2f(211, 1), FVector2f(211, 111), FVector2f(1, 111)})
		{
			TestFalse(*FString::Printf(TEXT("rounded corner (%g,%g) excluded: %s"), Corner.X, Corner.Y, Css), Covers(R.Geometry[0], Corner));
		}
	}
	const FRecorded Content = Harness.Record(TEXT("decorator: material-box(test-mat, 24px, 0.5) content-box; border-radius: 24px;"));
	if (TestEqual(TEXT("rounded content-box produces one mesh"), Content.Geometry.Num(), 1))
	{
		TestFalse(TEXT("inset rounded corner excluded"), Covers(Content.Geometry[0], FVector2f(7, 7)));
		TestTrue(TEXT("inset interior drawn"), Covers(Content.Geometry[0], FVector2f(50, 50)));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
