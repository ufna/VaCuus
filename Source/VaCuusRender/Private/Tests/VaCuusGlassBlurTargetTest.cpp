// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusDefines.h"
#include "VaCuusUIShaders.h"

#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusGlassBlurTargetGPU
{
/** The source every case samples: 640x352, a multiple of 16 on both axes so the chain's halvings are exact. */
static const FIntPoint SourceSize(640, 352);

struct FCaseResult
{
	bool bRead = false;
	int32 Divisor = 0;
	float Mean = 0.0f;
	float Min = 0.0f;
	float Max = 0.0f;
};

/**
 * Uploads Pixels (one byte per pixel, replicated to RGB, opaque) as the scene, runs the
 * production AddBlurTargetPasses for a uniform view sigma, and reads the target back.
 * PF_FloatRGBA targets so the readback is not re-quantized to 8 bits on the way out.
 */
static FCaseResult RunCase(FRHICommandListImmediate& RHICmdList, const TArray<uint8>& Pixels, float ViewSigma)
{
	FCaseResult Result;

	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(Pixels.Num() * 4);
	for (int32 Index = 0; Index < Pixels.Num(); ++Index)
	{
		Bytes[Index * 4 + 0] = Pixels[Index];
		Bytes[Index * 4 + 1] = Pixels[Index];
		Bytes[Index * 4 + 2] = Pixels[Index];
		Bytes[Index * 4 + 3] = 255;
	}
	const FRHITextureCreateDesc SourceDesc = FRHITextureCreateDesc::Create2D(TEXT("VaCuusGlassBlurTargetSource"), SourceSize, PF_B8G8R8A8)
												 .SetFlags(ETextureCreateFlags::ShaderResource)
												 .SetInitialState(ERHIAccess::SRVMask);
	FTextureRHIRef SourceRHI = RHICmdList.CreateTexture(SourceDesc);
	RHICmdList.UpdateTexture2D(SourceRHI, 0, FUpdateTextureRegion2D(0, 0, 0, 0, uint32(SourceSize.X), uint32(SourceSize.Y)),
		uint32(SourceSize.X) * 4u, Bytes.GetData());

	const VaCuusGlass::FBlurPlan Plan = VaCuusGlass::MakeBlurPlan(SourceSize, FVector2f(ViewSigma, ViewSigma));
	Result.Divisor = Plan.Divisor;

	FRDGBuilder GraphBuilder(RHICmdList);
	FRDGTextureRef Source = RegisterExternalTexture(GraphBuilder, SourceRHI, TEXT("VaCuusGlassBlurTargetSource"));
	const FRDGTextureDesc TargetDesc = FRDGTextureDesc::Create2D(Plan.TargetSize, PF_FloatRGBA, FClearValueBinding::Black,
		TexCreate_RenderTargetable | TexCreate_ShaderResource);
	FRDGTextureRef TargetA = GraphBuilder.CreateTexture(TargetDesc, TEXT("VaCuusGlassBlurTargetA"));
	FRDGTextureRef TargetB = GraphBuilder.CreateTexture(TargetDesc, TEXT("VaCuusGlassBlurTargetB"));

	// Production's pair is persistent and loaded, never cleared; a transient has no prior
	// contents to load, so give it some. Every texel is overwritten by the passes anyway.
	AddClearRenderTargetPass(GraphBuilder, TargetA, FLinearColor::Black);
	AddClearRenderTargetPass(GraphBuilder, TargetB, FLinearColor::Black);

	VaCuusGlass::AddBlurTargetPasses(GraphBuilder, GetGlobalShaderMap(GMaxRHIFeatureLevel), Source,
		FIntRect(FIntPoint::ZeroValue, SourceSize), Plan, TargetA, TargetB);

	FRHIGPUTextureReadback Readback(TEXT("VaCuusGlassBlurTargetReadback"));
	AddEnqueueCopyPass(GraphBuilder, &Readback, TargetA);
	GraphBuilder.Execute();

	RHICmdList.SubmitAndBlockUntilGPUIdle();
	if (Readback.IsReady())
	{
		int32 RowPitchInPixels = 0;
		if (const void* Data = Readback.Lock(RowPitchInPixels))
		{
			const FFloat16Color* Pixels16 = static_cast<const FFloat16Color*>(Data);
			double Sum = 0.0;
			Result.Min = TNumericLimits<float>::Max();
			Result.Max = TNumericLimits<float>::Lowest();
			for (int32 Y = 0; Y < Plan.TargetSize.Y; ++Y)
			{
				for (int32 X = 0; X < Plan.TargetSize.X; ++X)
				{
					const float Value = Pixels16[Y * RowPitchInPixels + X].G.GetFloat();
					Sum += Value;
					Result.Min = FMath::Min(Result.Min, Value);
					Result.Max = FMath::Max(Result.Max, Value);
				}
			}
			Result.Mean = float(Sum / double(Plan.TargetSize.X * Plan.TargetSize.Y));
			Result.bRead = true;
			Readback.Unlock();
		}
	}
	return Result;
}
} // namespace VaCuusGlassBlurTargetGPU

/**
 * The glass blur target, GPU half: the production downsample chain and blur passes
 * (VaCuusGlass::AddBlurTargetPasses, the same call AddGlassPasses makes) run on a real RHI
 * over a known scene and read back. Two properties, each of which the pure-math
 * KernelKeepsItsLight cannot see:
 *
 *  - A UNIFORM scene under a 200px sigma (divisor 8) comes back at its own level. If the
 *    blur were handed a half-resolution sigma (100 texels against the kernel's 41.7), it
 *    would truncate and come back at ~0.62 of it (0.79 per pass).
 *  - A FINE GRID -- 1px white lines every 16px on black, mean 31/256 -- under a 400px sigma
 *    (divisor 16) comes back at exactly that mean, everywhere. The chain's 2x2 averages
 *    see every line; a single 1/16 bilinear pass samples between pixels 7 and 8 of each
 *    16px cell, never touches a line, and returns black.
 *
 * Needs a real RHI; under NullRHI it self-skips and passes vacuously, like
 * VaCuus.Render.Composite.LinearOutputGPU.
 *
 * Restore-the-bug (2026-09-23, Linux, Vulkan): green reads 0.4990 against 0.5020 and 0.1204
 * against 0.1211. The chain collapsed to one pass (StepDivisor starting at Plan.Divisor)
 * fails the grid only, at exactly 0.0000; the blur handed the half-resolution sigma
 * (SigmaTexels * Divisor / 2) fails both, the uniform at 0.3164 (0.63 of its level). Restored,
 * green.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassBlurTargetGPUTest, "VaCuus.Render.Glass.BlurTargetGPU",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassBlurTargetGPUTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusGlassBlurTargetGPU;

	if (GUsingNullRHI)
	{
		UE_LOG(LogVaCuus, Display, TEXT("VaCuus.Render.Glass.BlurTargetGPU: SKIPPED under NullRHI (no draw, no readback)"));
		return true;
	}

	const int32 PixelCount = SourceSize.X * SourceSize.Y;

	TArray<uint8> Uniform;
	Uniform.Init(128, PixelCount);

	TArray<uint8> Grid;
	Grid.SetNumZeroed(PixelCount);
	for (int32 Y = 0; Y < SourceSize.Y; ++Y)
	{
		for (int32 X = 0; X < SourceSize.X; ++X)
		{
			if (X % 16 == 0 || Y % 16 == 0)
			{
				Grid[Y * SourceSize.X + X] = 255;
			}
		}
	}

	FCaseResult UniformResult, GridResult;
	ENQUEUE_RENDER_COMMAND(VaCuusGlassBlurTargetGPUCases)
	([&](FRHICommandListImmediate& RHICmdList)
		{
			UniformResult = RunCase(RHICmdList, Uniform, 200.0f);
			GridResult = RunCase(RHICmdList, Grid, 400.0f);
		});
	FlushRenderingCommands();

	if (!TestTrue(TEXT("Both readbacks completed"), UniformResult.bRead && GridResult.bRead))
	{
		return false;
	}

	// 3-sigma truncation keeps ~0.997 per pass, ~0.994 after both; FP16 adds ~0.001. 1.5%
	// clears both with margin and is an order below what a truncated half-res kernel loses.
	const auto TestLevel = [this](const TCHAR* What, const FCaseResult& Result, float Expected, float Tolerance)
	{
		const FString Measured = FString::Printf(TEXT("%s (divisor %d): mean %.4f, min %.4f, max %.4f, expected %.4f +/- %.1f%%"),
			What, Result.Divisor, Result.Mean, Result.Min, Result.Max, Expected, Tolerance * 100.0f);
		AddInfo(Measured); // the numbers in the log on a pass too, so a green run is still evidence
		TestTrue(Measured,
			FMath::Abs(Result.Min - Expected) <= Expected * Tolerance && FMath::Abs(Result.Max - Expected) <= Expected * Tolerance);
	};

	TestEqual(TEXT("A 200px sigma plans divisor 8"), UniformResult.Divisor, 8);
	TestLevel(TEXT("A uniform scene keeps its level under a 200px blur"), UniformResult, 128.0f / 255.0f, 0.015f);

	TestEqual(TEXT("A 400px sigma plans divisor 16"), GridResult.Divisor, 16);
	TestLevel(TEXT("A 1px grid every 16px averages to its mean under a 400px blur"), GridResult, 31.0f / 256.0f, 0.02f);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
