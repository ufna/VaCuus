// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusCommandBuffer.h"
#include "VaCuusCoreCompat.h"
#include "VaCuusDefines.h"
#include "VaCuusMaterialDraw.h"
#include "VaCuusReplayRenderer.h"
#include "VaCuusStyleSet.h"
#include "VaCuusUIShaders.h"

#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Misc/ScopeExit.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RHIResourceUtils.h"
#include "RHIStaticStates.h"
#include "RenderingThread.h"
#include "ShaderCompiler.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

/**
 * THE SLOT-3 PROBE — the observable PR #1's contract had none of. VaCuusMaterial.usf fills
 * the material's texture-coordinate slots the way Slate does, and slot 3 ("Pixel Size" in
 * GetUserInterfaceUV) must be the paint box in pixels: a pixel-snapping material divides by
 * it. Nothing on the C++ side can see whether it is — the uniform's bind is SPF_Optional by
 * necessity and SetShaderValue on an unbound parameter writes nothing (the SetParameters
 * comment in VaCuusMaterialDraw.h) — so a mistyped uniform name, a reordered table or the
 * pre-PR "every slot is InUV" all compile, and every -nullrhi test stays green through all
 * three. This test draws through the production entry point, DrawMaterial_RenderThread,
 * and reads the pixels back.
 *
 * THE FIXTURE IS BUILT IN C++, not committed as a .uasset: two expressions — TextureCoordinate
 * index 3, times 1/256 — into Emissive, on a transient MD_UI Opaque material, compiled right
 * here (PostEditChange, then a SYNCHRONOUS ForceRecompileForRendering — editor-only by nature,
 * hence WITH_EDITOR). Over a
 * 256 x 128 paint box the correct slot 3 makes the fill a FLAT (1.0, 0.5, 0.0) linear, i.e.
 * (255, 188, 0) after the shader's own sRGB encode; a UV in the slot makes it a ramp that is
 * (0..1, 0..0.5)/256 — near black everywhere and different at every sample point.
 *
 * VENUE. Both probes in this file draw and read back, so they report themselves skipped
 * under -nullrhi and pass vacuously — the GradientAA contract (VaCuusGradientAATest.cpp).
 * The real-RHI run is where they carry evidence.
 *
 * Restore-the-bug, twice (2026-09-04, Linux, Vulkan SM6, -RenderOffscreen):
 *  - Slots[3] in VaCuusMaterial.usf reverted to InUV (the pre-PR value): centre pixel
 *    (6, 6, 0, 255) against (255, 188, 0), spread R 12 G 12 — "(R)", "(G)" and "the fill is
 *    flat" all failed, the draw itself reporting drawn 1, shader misses 0.
 *  - The C++ bind mistyped (TEXT("VaCuusDimensionz")): centre (0, 0, 0, 255), spread 0 —
 *    "(R)" and "(G)" failed while "flat" held, and the draw again reported drawn 1, misses 0:
 *    the silent no-op the SetParameters comment describes, visible nowhere else.
 *  Restored both times: (255, 188, 0, 255) at all five sample points, spread 0, 7/7 in
 *  VaCuus.Render.Decorator.
 */
namespace VaCuusMaterialSlotTest
{
static const FIntPoint GBox(256, 128);
static const TCHAR* GKey = TEXT("slot-probe");

/** Emissive = TexCoord[Slot] * Scale on a transient MD_UI Opaque material, compiled synchronously. */
static UMaterial* MakeProbeMaterial(int32 Slot = 3, float ScaleBy = 1.0f / 256.0f)
{
	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
	Material->MaterialDomain = EMaterialDomain::MD_UI;
	Material->BlendMode = BLEND_Opaque;

	UMaterialExpressionTextureCoordinate* Slot3 = NewObject<UMaterialExpressionTextureCoordinate>(Material);
	Slot3->Material = Material;
	Slot3->CoordinateIndex = Slot;

	UMaterialExpressionMultiply* Scale = NewObject<UMaterialExpressionMultiply>(Material);
	Scale->Material = Material;
	Scale->A.Connect(0, Slot3);
	Scale->ConstB = ScaleBy;

	Material->GetExpressionCollection().AddExpression(Slot3);
	Material->GetExpressionCollection().AddExpression(Scale);
	Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Scale);

	// PostEditChange alone does NOT compile: its recompile runs at EMaterialShaderPrecompileMode
	// ::None (Material.cpp:5418), which leaves the map to be compiled on demand — and nothing
	// demands it here until the draw, which then walks to a pair-less proxy and skips (the
	// first run of this test failed exactly so, in 15 ms). ForceRecompileForRendering is the
	// same UpdateCachedExpressionData + CacheResourceShadersForRendering (Material.cpp
	// :7289-7292) with a compile mode that blocks; FinishAllCompilation is belt and braces.
	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	Material->ForceRecompileForRendering(EMaterialShaderPrecompileMode::Synchronous);
	GShaderCompilingManager->FinishAllCompilation();
	FlushRenderingCommands();
	return Material;
}

/**
 * The second probe's fixture: NumCustomizedUVs = 3 with CustomizedUV2 = TexCoord[0] * 2 and
 * Emissive = TexCoord[2]. Under Slate's contract a material that declares customized UVs
 * reads the RAW channels at the pixel stage (SlateElementPixelShader.usf:149-161), so channel
 * 2 is whatever the vertex-stage expression put there — a 2x ramp of the paint box's UV,
 * (0..2, 0..2), rather than the (0,0) the vertex table carries in that channel. A material
 * whose customized UVs never run (the state before bead VaCuus-x3k) fills black.
 */
static UMaterial* MakeCustomizedUVMaterial()
{
	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
	Material->MaterialDomain = EMaterialDomain::MD_UI;
	Material->BlendMode = BLEND_Opaque;
	Material->NumCustomizedUVs = 3;

	UMaterialExpressionTextureCoordinate* Channel0 = NewObject<UMaterialExpressionTextureCoordinate>(Material);
	Channel0->Material = Material;
	Channel0->CoordinateIndex = 0;

	UMaterialExpressionMultiply* Doubled = NewObject<UMaterialExpressionMultiply>(Material);
	Doubled->Material = Material;
	Doubled->A.Connect(0, Channel0);
	Doubled->ConstB = 2.0f;

	UMaterialExpressionTextureCoordinate* Channel2 = NewObject<UMaterialExpressionTextureCoordinate>(Material);
	Channel2->Material = Material;
	Channel2->CoordinateIndex = 2;

	Material->GetExpressionCollection().AddExpression(Channel0);
	Material->GetExpressionCollection().AddExpression(Doubled);
	Material->GetExpressionCollection().AddExpression(Channel2);
	Material->GetEditorOnlyData()->CustomizedUVs[2].Connect(0, Doubled);
	Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Channel2);

	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	Material->ForceRecompileForRendering(EMaterialShaderPrecompileMode::Synchronous);
	GShaderCompilingManager->FinishAllCompilation();
	FlushRenderingCommands();
	return Material;
}

/**
 * One material draw into a GBox-sized PF_B8G8R8A8 target through the production entry
 * point, read back. The geometry is the decorator's own: one quad over the paint box with
 * UV 0..1 (DecoratorShader.cpp:45-47). The desc is what the recorder mints for
 * `decorator: shader(slot-probe)` — Kind Material, the snapshot's stable id, Dimensions the
 * paint box (VaCuusRecordingRenderInterface.cpp, CompileShader's Material branch).
 * VertexColour is every corner's colour (an element at opacity 1 is white); ClearValue is the target's clear.
 */
/** An own-mesh decorator's draw (material-box): its mesh, and the paint box its desc carries. */
struct FOwnMesh
{
	TArray<FVaCuusVertex> Vertices;
	TArray<int32> Indices;
	FVector2f PaintOffset;
	FVector2f Dimensions;
};

static bool RenderProbe(uint64 StableId, const TCHAR* Key, TArray<FColor>& OutPixels, bool& bOutDrawn, bool& bOutRead,
	const FColor& VertexColour = FColor(255, 255, 255, 255), const FClearValueBinding& ClearValue = FClearValueBinding::Black,
	const TOptional<FOwnMesh>& OwnMesh = {})
{
	bool bDrawn = false;
	bool bRead = false;
	OutPixels.Reset();

	FVaCuusShaderDesc Desc;
	Desc.Kind = EVaCuusShaderKind::Material;
	Desc.MaterialId = StableId;
	Desc.BuiltinKey = Key;
	Desc.Dimensions = FVector2f(float(GBox.X), float(GBox.Y));
	if (OwnMesh.IsSet())
	{
		Desc.Dimensions = OwnMesh->Dimensions;
		Desc.PaintOffset = OwnMesh->PaintOffset;
		Desc.bHasPaintOffset = 1;
	}

	ENQUEUE_RENDER_COMMAND(VaCuusMaterialSlotProbe)
	([Desc, VertexColour, ClearValue, OwnMesh, &OutPixels, &bDrawn, &bRead](FRHICommandListImmediate& RHICmdList)
		{
			const FIntPoint Extent = GBox;
			const FRHITextureCreateDesc RTDesc = FRHITextureCreateDesc::Create2D(TEXT("VaCuusMaterialSlotRT"), Extent, PF_B8G8R8A8)
													  .SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource)
													  .SetClearValue(ClearValue)
													  .SetInitialState(ERHIAccess::SRVMask);
			FTextureRHIRef Target = RHICmdList.CreateTexture(RTDesc);

			TArray<FVaCuusVertex> Vertices;
			Vertices.Add({FVector2f(0.0f, 0.0f), VertexColour, FVector2f(0.0f, 0.0f)});
			Vertices.Add({FVector2f(float(Extent.X), 0.0f), VertexColour, FVector2f(1.0f, 0.0f)});
			Vertices.Add({FVector2f(float(Extent.X), float(Extent.Y)), VertexColour, FVector2f(1.0f, 1.0f)});
			Vertices.Add({FVector2f(0.0f, float(Extent.Y)), VertexColour, FVector2f(0.0f, 1.0f)});
			TArray<int32> Indices = {0, 1, 2, 0, 2, 3};
			if (OwnMesh.IsSet())
			{
				Vertices = OwnMesh->Vertices;
				Indices = OwnMesh->Indices;
			}

			FBufferRHIRef VB = UE::RHIResourceUtils::CreateVertexBufferFromArray<FVaCuusVertex>(
				RHICmdList, TEXT("VaCuusMaterialSlotVB"), EBufferUsageFlags::Static, MakeConstArrayView(Vertices));
			FBufferRHIRef IB = UE::RHIResourceUtils::CreateIndexBufferFromArray<int32>(
				RHICmdList, TEXT("VaCuusMaterialSlotIB"), EBufferUsageFlags::Static, MakeConstArrayView(Indices));

			RHICmdList.Transition(FRHITransitionInfo(Target, ERHIAccess::SRVMask, ERHIAccess::RTV));
			FRHIRenderPassInfo RPInfo(Target, ERenderTargetActions::Clear_Store);
			RHICmdList.BeginRenderPass(RPInfo, TEXT("VaCuusMaterialSlot"));
			{
				RHICmdList.SetViewport(0.0f, 0.0f, 0.0f, float(Extent.X), float(Extent.Y), 1.0f);

				// The production draw, with the replayer's unmasked depth-stencil state (no
				// clip mask in this document) — the PSO, the view UB and the parameters are
				// all DrawMaterial's own.
				VaCuusMaterialDraw::FPassState PassState;
				bDrawn = VaCuusMaterialDraw::DrawMaterial_RenderThread(RHICmdList, PassState, Extent,
					VaCuusReplay::MakePixelToClipMatrix(Extent), Desc, VB, IB, Vertices.Num(), Indices.Num(),
					TStaticDepthStencilState<false, CF_Always>::GetRHI(), 0);
			}
			RHICmdList.EndRenderPass();
			RHICmdList.Transition(FRHITransitionInfo(Target, ERHIAccess::RTV, ERHIAccess::CopySrc));

			FRHIGPUTextureReadback Readback(TEXT("VaCuusMaterialSlotReadback"));
			Readback.EnqueueCopy(RHICmdList, Target);
			RHICmdList.SubmitAndBlockUntilGPUIdle();
			if (!Readback.IsReady())
			{
				return;
			}

			int32 RowPitchInPixels = 0;
			if (const void* Data = Readback.Lock(RowPitchInPixels))
			{
				const FColor* Rows = static_cast<const FColor*>(Data);
				OutPixels.SetNumUninitialized(Extent.X * Extent.Y);
				for (int32 Y = 0; Y < Extent.Y; ++Y)
				{
					FMemory::Memcpy(&OutPixels[Y * Extent.X], Rows + int64(Y) * RowPitchInPixels, Extent.X * sizeof(FColor));
				}
				Readback.Unlock();
				bRead = true;
			}
		});
	FlushRenderingCommands();

	bOutDrawn = bDrawn;
	bOutRead = bRead;
	return bDrawn && bRead;
}
} // namespace VaCuusMaterialSlotTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMaterialPixelSizeSlotTest, "VaCuus.Render.Decorator.MaterialPixelSizeSlot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusMaterialPixelSizeSlotTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMaterialSlotTest;

	if (GUsingNullRHI)
	{
		UE_LOG(LogVaCuus, Display, TEXT("VaCuus.Render.Decorator.MaterialPixelSizeSlot: SKIPPED under NullRHI (no draw, no readback)"));
		return true;
	}

	UMaterial* Probe = MakeProbeMaterial();
	TStrongObjectPtr<UMaterial> ProbeRoot(Probe);

	// The production registration path: the registry roots the material, mirrors its proxy
	// to the render thread and pre-warms the shader walk; the flush lands all of that.
	UVaCuusStyleSet* StyleSet = NewObject<UVaCuusStyleSet>(GetTransientPackage());
	StyleSet->Materials.Add(GKey, Probe);
	TStrongObjectPtr<UVaCuusStyleSet> StyleSetRoot(StyleSet);
	ON_SCOPE_EXIT
	{
		FVaCuusStyleRegistry::UnregisterStyleSet(StyleSetRoot.Get());
		FlushRenderingCommands();
		FVaCuusStyleRegistry::TickDeferredReleases_GameThread();
	};
	if (!TestEqual(TEXT("the probe material registers"), FVaCuusStyleRegistry::RegisterStyleSet(StyleSet), 1))
	{
		return false;
	}
	const uint64* StableId = FVaCuusStyleRegistry::GetSnapshot_GameThread()->KeyToId.Find(GKey);
	if (!TestNotNull(TEXT("the snapshot carries the probe's id"), StableId))
	{
		return false;
	}
	FlushRenderingCommands();

	// Diagnostics first, so a failure names its stage: is the material compiled at all, and
	// which of DrawMaterial's three exits did the draw take?
	{
		const FMaterialResource* Resource = Probe->GetMaterialResource(VaCuusCompat::MaterialQueryTarget());
		AddInfo(FString::Printf(TEXT("probe material: resource %s, compilation finished %d, game-thread shader map %s, compiling-or-error %d"),
			Resource ? TEXT("yes") : TEXT("NULL"), Resource ? int32(Resource->IsCompilationFinished()) : -1,
			(Resource && Resource->GetGameThreadShaderMap()) ? TEXT("yes") : TEXT("NULL"),
			int32(Probe->IsCompilingOrHadCompileError(VaCuusCompat::MaterialQueryTarget()))));
	}
	const int64 Draws0 = VaCuusMaterialDraw::GetDrawCount();
	const int64 Misses0 = VaCuusMaterialDraw::GetShaderMissCount();
	const int64 Unresolved0 = VaCuusMaterialDraw::GetUnresolvedCount();

	TArray<FColor> Pixels;
	bool bDrawn = false, bRead = false;
	const bool bProbed = RenderProbe(*StableId, GKey, Pixels, bDrawn, bRead);
	AddInfo(FString::Printf(TEXT("probe draw: drawn %d, read back %d; draws +%lld, shader misses +%lld, unresolved +%lld"),
		int32(bDrawn), int32(bRead), VaCuusMaterialDraw::GetDrawCount() - Draws0,
		VaCuusMaterialDraw::GetShaderMissCount() - Misses0, VaCuusMaterialDraw::GetUnresolvedCount() - Unresolved0));
	if (!TestTrue(TEXT("the material drew through DrawMaterial_RenderThread and read back"), bProbed))
	{
		return false;
	}

	// (1.0, 0.5, 0.0) linear through the shader's exact sRGB encode (GammaCorrectionCommon.ush
	// :45-54 picks the branching curve on this feature level); ToFColorSRGB is the CPU twin.
	const FColor Expected = FLinearColor(1.0f, 0.5f, 0.0f).ToFColorSRGB();
	const FIntPoint Samples[] = {
		FIntPoint(GBox.X / 2, GBox.Y / 2), FIntPoint(4, 4), FIntPoint(GBox.X - 5, 4), FIntPoint(4, GBox.Y - 5), FIntPoint(GBox.X - 5, GBox.Y - 5)};

	int32 MinR = 255, MaxR = 0, MinG = 255, MaxG = 0, MinB = 255, MaxB = 0;
	for (const FIntPoint& P : Samples)
	{
		const FColor& C = Pixels[P.Y * GBox.X + P.X];
		MinR = FMath::Min<int32>(MinR, C.R); MaxR = FMath::Max<int32>(MaxR, C.R);
		MinG = FMath::Min<int32>(MinG, C.G); MaxG = FMath::Max<int32>(MaxG, C.G);
		MinB = FMath::Min<int32>(MinB, C.B); MaxB = FMath::Max<int32>(MaxB, C.B);
		TestEqual(*FString::Printf(TEXT("opaque alpha at (%d,%d)"), P.X, P.Y), int32(C.A), 255);
	}
	const FColor& Centre = Pixels[(GBox.Y / 2) * GBox.X + GBox.X / 2];
	AddInfo(FString::Printf(TEXT("centre pixel (%d, %d, %d, %d), expected (%d, %d, %d) +/- 3; spread R %d G %d B %d"),
		Centre.R, Centre.G, Centre.B, Centre.A, Expected.R, Expected.G, Expected.B, MaxR - MinR, MaxG - MinG, MaxB - MinB));

	// The value: slot 3 = (256, 128) -> Emissive (1.0, 0.5, 0). A UV there gives ~(0.002, 0.001, 0)
	// at the centre — black.
	TestTrue(TEXT("the fill is the paint box in pixels (R)"), FMath::Abs(int32(Centre.R) - int32(Expected.R)) <= 3);
	TestTrue(TEXT("the fill is the paint box in pixels (G)"), FMath::Abs(int32(Centre.G) - int32(Expected.G)) <= 3);
	TestTrue(TEXT("the fill is the paint box in pixels (B)"), FMath::Abs(int32(Centre.B) - int32(Expected.B)) <= 3);

	// The shape: a size is constant over the box; a UV is not.
	TestTrue(TEXT("the fill is flat"), (MaxR - MinR) <= 2 && (MaxG - MinG) <= 2 && (MaxB - MinB) <= 2);

	return true;
}

/**
 * THE CUSTOMIZED-UV PROBE (bead VaCuus-x3k). Same draw, the second fixture: a material that
 * customizes channel 2 to twice its UV and reads it back. Three samples along one row must
 * be the 2x ramp — dark, mid, saturated — and must differ from each other; a vertex stage
 * that never ran the material's expressions leaves channel 2 at the vertex table's (0,0)
 * and the fill black, and the pre-x3k pixel-only table would have filled it a flat (1,1).
 *
 * Restore-the-bug (2026-09-05, Linux, Vulkan SM6, -RenderOffscreen): with MainVS copying the
 * vertex table into CustomizedUVs instead of calling GetMaterialCustomizedUVs, all three
 * samples came back (0, 0, 0, 255) — the six ramp assertions and "the ramp rises" failed, the
 * slot-3 probe beside it unaffected; restored, (138, 189, 0), (188, 189, 0), (255, 189, 0),
 * each within 0 of the CPU twin, 8/8 in VaCuus.Render.Decorator.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMaterialCustomizedUVsTest, "VaCuus.Render.Decorator.MaterialCustomizedUVs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusMaterialCustomizedUVsTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMaterialSlotTest;

	if (GUsingNullRHI)
	{
		UE_LOG(LogVaCuus, Display, TEXT("VaCuus.Render.Decorator.MaterialCustomizedUVs: SKIPPED under NullRHI (no draw, no readback)"));
		return true;
	}

	static const TCHAR* Key = TEXT("customized-probe");
	UMaterial* Probe = MakeCustomizedUVMaterial();
	TStrongObjectPtr<UMaterial> ProbeRoot(Probe);

	UVaCuusStyleSet* StyleSet = NewObject<UVaCuusStyleSet>(GetTransientPackage());
	StyleSet->Materials.Add(Key, Probe);
	TStrongObjectPtr<UVaCuusStyleSet> StyleSetRoot(StyleSet);
	ON_SCOPE_EXIT
	{
		FVaCuusStyleRegistry::UnregisterStyleSet(StyleSetRoot.Get());
		FlushRenderingCommands();
		FVaCuusStyleRegistry::TickDeferredReleases_GameThread();
	};
	if (!TestEqual(TEXT("the customized-UV material registers"), FVaCuusStyleRegistry::RegisterStyleSet(StyleSet), 1))
	{
		return false;
	}
	const uint64* StableId = FVaCuusStyleRegistry::GetSnapshot_GameThread()->KeyToId.Find(Key);
	if (!TestNotNull(TEXT("the snapshot carries the probe's id"), StableId))
	{
		return false;
	}
	FlushRenderingCommands();

	TArray<FColor> Pixels;
	bool bDrawn = false, bRead = false;
	const bool bProbed = RenderProbe(*StableId, Key, Pixels, bDrawn, bRead);
	AddInfo(FString::Printf(TEXT("probe draw: drawn %d, read back %d"), int32(bDrawn), int32(bRead)));
	if (!TestTrue(TEXT("the material drew through DrawMaterial_RenderThread and read back"), bProbed))
	{
		return false;
	}

	// Channel 2 = 2 * UV, sampled at pixel centres along row y = 32: the paint box is 256 x 128,
	// so the expected linear value is (2 * (x + 0.5) / 256, 2 * (32.5) / 128, 0), clamped by
	// the encode; the CPU twin of the shader's sRGB curve is ToFColorSRGB again.
	const int32 Row = 32;
	const int32 Columns[] = {32, 64, 128};
	FColor Seen[3];
	for (int32 i = 0; i < 3; ++i)
	{
		const int32 X = Columns[i];
		const FLinearColor Linear(FMath::Min(2.0f * (X + 0.5f) / GBox.X, 1.0f), FMath::Min(2.0f * (Row + 0.5f) / GBox.Y, 1.0f), 0.0f);
		const FColor Expected = Linear.ToFColorSRGB();
		Seen[i] = Pixels[Row * GBox.X + X];
		AddInfo(FString::Printf(TEXT("(%d,%d): seen (%d, %d, %d, %d), expected (%d, %d, %d) +/- 3"),
			X, Row, Seen[i].R, Seen[i].G, Seen[i].B, Seen[i].A, Expected.R, Expected.G, Expected.B));
		TestTrue(*FString::Printf(TEXT("channel 2 is the customized 2x ramp at x=%d (R)"), X), FMath::Abs(int32(Seen[i].R) - int32(Expected.R)) <= 3);
		TestTrue(*FString::Printf(TEXT("channel 2 is the customized 2x ramp at x=%d (G)"), X), FMath::Abs(int32(Seen[i].G) - int32(Expected.G)) <= 3);
		TestEqual(*FString::Printf(TEXT("opaque alpha at x=%d"), X), int32(Seen[i].A), 255);
	}

	// The shape: a ramp rises; a constant (flat (1,1) or flat black) does not.
	TestTrue(TEXT("the ramp rises across the row"), int32(Seen[2].R) - int32(Seen[0].R) >= 60 && int32(Seen[1].R) > int32(Seen[0].R));

	return true;
}

/**
 * THE VERTEX-COLOUR PROBE. A shader decorator's geometry carries the element's opacity as its
 * premultiplied vertex colour (DecoratorShader.cpp:42-43, ColourbPremultiplied(alpha, alpha)), and
 * Slate multiplies a material brush by its vertex colour (SlateElementPixelShader.usf:318).
 * So an element's opacity has to fade the material: the same opaque material drawn with white and
 * with premultiplied 0.5 must come back half as bright in colour and in alpha.
 *
 * The target clears to TRANSPARENT black, not the opaque black of the other probes. The draw blends
 * One/InvSrcAlpha on alpha as well, so over an opaque clear every draw would read alpha 255 whatever
 * the material wrote, and the alpha half of the check would prove nothing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMaterialVertexColourTest, "VaCuus.Render.Decorator.MaterialVertexColour",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusMaterialVertexColourTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMaterialSlotTest;

	if (GUsingNullRHI)
	{
		UE_LOG(LogVaCuus, Display, TEXT("VaCuus.Render.Decorator.MaterialVertexColour: SKIPPED under NullRHI (no draw, no readback)"));
		return true;
	}

	static const TCHAR* Key = TEXT("vertex-colour-probe");
	UMaterial* Probe = MakeProbeMaterial();
	TStrongObjectPtr<UMaterial> ProbeRoot(Probe);

	UVaCuusStyleSet* StyleSet = NewObject<UVaCuusStyleSet>(GetTransientPackage());
	StyleSet->Materials.Add(Key, Probe);
	TStrongObjectPtr<UVaCuusStyleSet> StyleSetRoot(StyleSet);
	ON_SCOPE_EXIT
	{
		FVaCuusStyleRegistry::UnregisterStyleSet(StyleSetRoot.Get());
		FlushRenderingCommands();
		FVaCuusStyleRegistry::TickDeferredReleases_GameThread();
	};
	if (!TestEqual(TEXT("the vertex-colour material registers"), FVaCuusStyleRegistry::RegisterStyleSet(StyleSet), 1))
	{
		return false;
	}
	const uint64* StableId = FVaCuusStyleRegistry::GetSnapshot_GameThread()->KeyToId.Find(Key);
	if (!TestNotNull(TEXT("the snapshot carries the probe's id"), StableId))
	{
		return false;
	}
	FlushRenderingCommands();

	const FClearValueBinding Transparent(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
	// Premultiplied 0.5 at byte precision is 128/255; the tolerance below absorbs the rounding.
	const FColor HalfVertex(128, 128, 128, 128);
	TArray<FColor> WhitePixels;
	TArray<FColor> FadedPixels;
	bool bDrawn = false, bRead = false;
	const bool bWhiteProbed = RenderProbe(*StableId, Key, WhitePixels, bDrawn, bRead, FColor(255, 255, 255, 255), Transparent);
	AddInfo(FString::Printf(TEXT("white draw: drawn %d, read back %d"), int32(bDrawn), int32(bRead)));
	if (!TestTrue(TEXT("the white draw went through DrawMaterial_RenderThread and read back"), bWhiteProbed))
	{
		return false;
	}
	const bool bFadedProbed = RenderProbe(*StableId, Key, FadedPixels, bDrawn, bRead, HalfVertex, Transparent);
	AddInfo(FString::Printf(TEXT("premultiplied 0.5 draw: drawn %d, read back %d"), int32(bDrawn), int32(bRead)));
	if (!TestTrue(TEXT("the premultiplied draw went through DrawMaterial_RenderThread and read back"), bFadedProbed))
	{
		return false;
	}

	// The white draw is the material's own fill, flat (1.0, 0.5, 0.0) linear as in the slot probe.
	// Without that, black against black would pass the halving checks below.
	const FColor Expected = FLinearColor(1.0f, 0.5f, 0.0f).ToFColorSRGB();
	const FIntPoint Samples[] = {FIntPoint(GBox.X / 2, GBox.Y / 2), FIntPoint(4, 4), FIntPoint(GBox.X - 5, GBox.Y - 5)};
	for (const FIntPoint& P : Samples)
	{
		const int32 Index = P.Y * GBox.X + P.X;
		const FColor& Whole = WhitePixels[Index];
		const FColor& Faded = FadedPixels[Index];
		AddInfo(FString::Printf(TEXT("(%d,%d): white (%d, %d, %d, %d), premultiplied 0.5 (%d, %d, %d, %d)"),
			P.X, P.Y, Whole.R, Whole.G, Whole.B, Whole.A, Faded.R, Faded.G, Faded.B, Faded.A));
		TestTrue(*FString::Printf(TEXT("white draw is the material fill at (%d,%d)"), P.X, P.Y),
			FMath::Abs(int32(Whole.R) - int32(Expected.R)) <= 3 && FMath::Abs(int32(Whole.G) - int32(Expected.G)) <= 3);
		TestEqual(*FString::Printf(TEXT("white draw is opaque at (%d,%d)"), P.X, P.Y), int32(Whole.A), 255);
		TestTrue(*FString::Printf(TEXT("premultiplied colour is half the white draw's at (%d,%d) (R)"), P.X, P.Y),
			FMath::Abs(float(Faded.R) - 0.5f * float(Whole.R)) <= 2.0f);
		TestTrue(*FString::Printf(TEXT("premultiplied colour is half the white draw's at (%d,%d) (G)"), P.X, P.Y),
			FMath::Abs(float(Faded.G) - 0.5f * float(Whole.G)) <= 2.0f);
		TestTrue(*FString::Printf(TEXT("premultiplied alpha is half the white draw's at (%d,%d)"), P.X, P.Y),
			FMath::Abs(float(Faded.A) - 0.5f * float(Whole.A)) <= 2.0f);
	}

	return true;
}

/**
 * THE SLOT-4 PROBE. An own-mesh decorator (material-box) puts the SLICED UV in tex_coord and
 * sends paint_offset, so slot 4 ("Normalized UV") must come from position, not from tex_coord.
 * Fixture: Emissive = TexCoord[4]; every vertex's UV is (0.5, 0.5); the paint box starts 256 px
 * left of the target and is 512 x 128, so slot 4 is ((x + 256) / 512, y / 128) -- a ramp from
 * 0.5 to 1 in R. tex_coord in the slot would fill a flat (0.5, 0.5). Without paint_offset the
 * same material must keep the stock contract: slot 4 = tex_coord.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMaterialNormalizedSlotTest, "VaCuus.Render.Decorator.MaterialNormalizedSlot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusMaterialNormalizedSlotTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMaterialSlotTest;

	if (GUsingNullRHI)
	{
		UE_LOG(LogVaCuus, Display, TEXT("VaCuus.Render.Decorator.MaterialNormalizedSlot: SKIPPED under NullRHI (no draw, no readback)"));
		return true;
	}

	static const TCHAR* Key = TEXT("normalized-probe");
	UMaterial* Probe = MakeProbeMaterial(4, 1.0f);
	TStrongObjectPtr<UMaterial> ProbeRoot(Probe);

	UVaCuusStyleSet* StyleSet = NewObject<UVaCuusStyleSet>(GetTransientPackage());
	StyleSet->Materials.Add(Key, Probe);
	TStrongObjectPtr<UVaCuusStyleSet> StyleSetRoot(StyleSet);
	ON_SCOPE_EXIT
	{
		FVaCuusStyleRegistry::UnregisterStyleSet(StyleSetRoot.Get());
		FlushRenderingCommands();
		FVaCuusStyleRegistry::TickDeferredReleases_GameThread();
	};
	if (!TestEqual(TEXT("the slot-4 material registers"), FVaCuusStyleRegistry::RegisterStyleSet(StyleSet), 1))
	{
		return false;
	}
	const uint64* StableId = FVaCuusStyleRegistry::GetSnapshot_GameThread()->KeyToId.Find(Key);
	if (!TestNotNull(TEXT("the snapshot carries the probe's id"), StableId))
	{
		return false;
	}
	FlushRenderingCommands();

	FOwnMesh OwnMesh;
	const FColor White(255, 255, 255, 255);
	OwnMesh.Vertices = {{FVector2f(0.0f, 0.0f), White, FVector2f(0.5f)}, {FVector2f(float(GBox.X), 0.0f), White, FVector2f(0.5f)},
		{FVector2f(float(GBox.X), float(GBox.Y)), White, FVector2f(0.5f)}, {FVector2f(0.0f, float(GBox.Y)), White, FVector2f(0.5f)}};
	OwnMesh.Indices = {0, 1, 2, 0, 2, 3};
	OwnMesh.PaintOffset = FVector2f(-256.0f, 0.0f);
	OwnMesh.Dimensions = FVector2f(512.0f, 128.0f);
	TArray<FColor> OwnPixels, StockPixels;
	bool bDrawn = false, bRead = false;
	if (!TestTrue(TEXT("the own-mesh draw went through DrawMaterial_RenderThread and read back"),
			RenderProbe(*StableId, Key, OwnPixels, bDrawn, bRead, FColor(255, 255, 255, 255), FClearValueBinding::Black, OwnMesh)))
	{
		return false;
	}
	if (!TestTrue(TEXT("the stock draw went through DrawMaterial_RenderThread and read back"),
			RenderProbe(*StableId, Key, StockPixels, bDrawn, bRead)))
	{
		return false;
	}

	const FIntPoint Samples[] = {FIntPoint(16, 16), FIntPoint(128, 64), FIntPoint(240, 112)};
	for (const FIntPoint& P : Samples)
	{
		const FColor& Own = OwnPixels[P.Y * GBox.X + P.X];
		const FColor& Stock = StockPixels[P.Y * GBox.X + P.X];
		const FColor ExpectedOwn = FLinearColor((P.X + 0.5f + 256.0f) / 512.0f, (P.Y + 0.5f) / 128.0f, 0.0f).ToFColorSRGB();
		const FColor ExpectedStock = FLinearColor((P.X + 0.5f) / 256.0f, (P.Y + 0.5f) / 128.0f, 0.0f).ToFColorSRGB();
		AddInfo(FString::Printf(TEXT("(%d,%d): own-mesh (%d, %d, %d) expected (%d, %d, %d); stock (%d, %d, %d) expected (%d, %d, %d)"),
			P.X, P.Y, Own.R, Own.G, Own.B, ExpectedOwn.R, ExpectedOwn.G, ExpectedOwn.B, Stock.R, Stock.G, Stock.B, ExpectedStock.R, ExpectedStock.G, ExpectedStock.B));
		TestTrue(*FString::Printf(TEXT("own-mesh slot 4 comes from position at (%d,%d)"), P.X, P.Y),
			FMath::Abs(int32(Own.R) - int32(ExpectedOwn.R)) <= 3 && FMath::Abs(int32(Own.G) - int32(ExpectedOwn.G)) <= 3);
		TestTrue(*FString::Printf(TEXT("stock slot 4 stays tex_coord at (%d,%d)"), P.X, P.Y),
			FMath::Abs(int32(Stock.R) - int32(ExpectedStock.R)) <= 3 && FMath::Abs(int32(Stock.G) - int32(ExpectedStock.G)) <= 3);
	}

	return true;
}


#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
