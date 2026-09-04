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
 * VENUE. Draws and reads back, so it reports itself skipped under -nullrhi and passes
 * vacuously — the GradientAA contract (VaCuusGradientAATest.cpp). The real-RHI run is where
 * it carries evidence.
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

/** Emissive = TexCoord[3] / 256 on a transient MD_UI Opaque material, compiled synchronously. */
static UMaterial* MakeProbeMaterial()
{
	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
	Material->MaterialDomain = EMaterialDomain::MD_UI;
	Material->BlendMode = BLEND_Opaque;

	UMaterialExpressionTextureCoordinate* Slot3 = NewObject<UMaterialExpressionTextureCoordinate>(Material);
	Slot3->Material = Material;
	Slot3->CoordinateIndex = 3;

	UMaterialExpressionMultiply* Scale = NewObject<UMaterialExpressionMultiply>(Material);
	Scale->Material = Material;
	Scale->A.Connect(0, Slot3);
	Scale->ConstB = 1.0f / float(GBox.X);

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
 * One material draw into a GBox-sized PF_B8G8R8A8 target through the production entry
 * point, read back. The geometry is the decorator's own: one quad over the paint box with
 * UV 0..1 (DecoratorShader.cpp:45-47). The desc is what the recorder mints for
 * `decorator: shader(slot-probe)` — Kind Material, the snapshot's stable id, Dimensions the
 * paint box (VaCuusRecordingRenderInterface.cpp, CompileShader's Material branch).
 */
static bool RenderProbe(uint64 StableId, TArray<FColor>& OutPixels, bool& bOutDrawn, bool& bOutRead)
{
	bool bDrawn = false;
	bool bRead = false;
	OutPixels.Reset();

	FVaCuusShaderDesc Desc;
	Desc.Kind = EVaCuusShaderKind::Material;
	Desc.MaterialId = StableId;
	Desc.BuiltinKey = GKey;
	Desc.Dimensions = FVector2f(float(GBox.X), float(GBox.Y));

	ENQUEUE_RENDER_COMMAND(VaCuusMaterialSlotProbe)
	([Desc, &OutPixels, &bDrawn, &bRead](FRHICommandListImmediate& RHICmdList)
		{
			const FIntPoint Extent = GBox;
			const FRHITextureCreateDesc RTDesc = FRHITextureCreateDesc::Create2D(TEXT("VaCuusMaterialSlotRT"), Extent, PF_B8G8R8A8)
													  .SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource)
													  .SetClearValue(FClearValueBinding::Black)
													  .SetInitialState(ERHIAccess::SRVMask);
			FTextureRHIRef Target = RHICmdList.CreateTexture(RTDesc);

			const FColor White(255, 255, 255, 255);
			TArray<FVaCuusVertex> Vertices;
			Vertices.Add({FVector2f(0.0f, 0.0f), White, FVector2f(0.0f, 0.0f)});
			Vertices.Add({FVector2f(float(Extent.X), 0.0f), White, FVector2f(1.0f, 0.0f)});
			Vertices.Add({FVector2f(float(Extent.X), float(Extent.Y)), White, FVector2f(1.0f, 1.0f)});
			Vertices.Add({FVector2f(0.0f, float(Extent.Y)), White, FVector2f(0.0f, 1.0f)});
			const TArray<int32> Indices = {0, 1, 2, 0, 2, 3};

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
	const bool bProbed = RenderProbe(*StableId, Pixels, bDrawn, bRead);
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

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
