// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusDefines.h"
#include "VaCuusUIShaders.h"

#include "RHIGPUReadback.h"
#include "RHIStaticStates.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ScreenPass.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * M6 Task 5.1: the PF_FloatRGBA composite permutation (spec §3.2 — M5's explicit
 * assignment; arch spec :193-194). Two tests with two venues:
 *
 *  - LinearOutputSelection runs everywhere (NullRHI included): the format->permutation
 *    decision table, pinned against the engine facts each row cites.
 *  - LinearOutputGPU needs a real RHI (it draws and reads back); under NullRHI it
 *    reports itself skipped and passes vacuously, so the -nullrhi suite stays green
 *    and the real-RHI run is the one that carries evidence.
 */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusCompositeLinearSelectionTest, "VaCuus.Render.Composite.LinearOutputSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusCompositeLinearSelectionTest::RunTest(const FString& Parameters)
{
	// The two formats the Slate viewport path actually produces on desktop
	// (GetViewportPixelFormat, SlateRHIRenderer.cpp:760-781):
	TestFalse(TEXT("PF_B8G8R8A8 (the LDR default) stays display-encoded — pass-through"),
		VaCuusCompositeWantsLinearOutput(PF_B8G8R8A8));
	TestTrue(TEXT("PF_FloatRGBA (r.DefaultBackBufferPixelFormat=3, RendererSettings.cpp:37-43; scRGB HDR) is a gamma-1.0 "
				  "target (UnrealEngine.cpp:2501-2504) — LinearOutput"),
		VaCuusCompositeWantsLinearOutput(PF_FloatRGBA));

	// PF_A2B10G10R10 twice over: the 10-bit SDR default (SceneTextures.cpp display
	// encoding preserved) AND the HDR10/PQ swapchain — both display-encoded, neither a
	// float format; the PQ case is the documented out-of-scope ST2084 story.
	TestFalse(TEXT("PF_A2B10G10R10 (10-bit SDR / HDR10 PQ) is not a linear target — pass-through"),
		VaCuusCompositeWantsLinearOutput(PF_A2B10G10R10));

	// Foreign float targets a host could composite us into (IsFloatFormat, PixelFormat.h:382-399).
	TestTrue(TEXT("PF_FloatR11G11B10 is linear — LinearOutput"), VaCuusCompositeWantsLinearOutput(PF_FloatR11G11B10));
	TestTrue(TEXT("PF_A32B32G32R32F is linear — LinearOutput"), VaCuusCompositeWantsLinearOutput(PF_A32B32G32R32F));

	return true;
}

namespace VaCuusCompositeGammaGPU
{
/**
 * CPU twin of the shader's decode, formula-identical to sRGBToLinear
 * (GammaCorrectionCommon.ush:56-60): select(C > 0.04045, pow(C/1.055 + 0.0521327,
 * 2.4), C/12.92). The ush adds 0.0521327 (= 0.055/1.055) AFTER the divide — the same
 * algebraic form is reproduced here so the twin cannot drift from the shader by a
 * rounding of the constant.
 */
static float SrgbToLinearChannel(float C)
{
	return C > 0.04045f ? FMath::Pow(C * (1.0f / 1.055f) + 0.0521327f, 2.4f) : C * (1.0f / 12.92f);
}

struct FCaseResult
{
	FLinearColor Output = FLinearColor::Black;
	bool bRead = false;
};

/** One 1x1 composite draw: clear the input to InputRaw, draw with the permutation, read back. */
static FCaseResult RunCase(FRHICommandListImmediate& RHICmdList, const FLinearColor& InputRaw, bool bLinearOutput)
{
	FCaseResult Result;

	FRDGBuilder GraphBuilder(RHICmdList);

	// PF_B8G8R8A8 WITHOUT TexCreate_SRGB, like the replayer's UI RT: the clear writes
	// InputRaw's values straight into the unorm bytes and the composite's sample reads
	// them back numerically unchanged — the test controls the encoded bytes exactly.
	FRDGTextureRef Input = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(FIntPoint(1, 1), PF_B8G8R8A8, FClearValueBinding::Black,
			TexCreate_RenderTargetable | TexCreate_ShaderResource),
		TEXT("VaCuusCompositeGammaInput"));
	AddClearRenderTargetPass(GraphBuilder, Input, InputRaw);

	FRDGTextureRef Output = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(FIntPoint(1, 1), PF_FloatRGBA, FClearValueBinding::Black,
			TexCreate_RenderTargetable | TexCreate_ShaderResource),
		TEXT("VaCuusCompositeGammaOutput"));

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FScreenPassVS> VertexShader(ShaderMap);
	FVaCuusCompositePS::FPermutationDomain Permutation;
	Permutation.Set<FVaCuusCompositePS::FLinearOutput>(bLinearOutput);
	TShaderMapRef<FVaCuusCompositePS> PixelShader(ShaderMap, Permutation);

	FVaCuusCompositePS::FParameters* Parameters = GraphBuilder.AllocParameters<FVaCuusCompositePS::FParameters>();
	Parameters->CompositeTexture = Input;
	Parameters->CompositeSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Parameters->RenderTargets[0] = FRenderTargetBinding(Output, ERenderTargetLoadAction::EClear);

	// Default (opaque overwrite) blend: the property under test is the PS's decode,
	// not the production blend state, and an overwrite makes the readback the shader's
	// verbatim output.
	AddDrawScreenPass(GraphBuilder, RDG_EVENT_NAME("VaCuusCompositeGammaCase"), FScreenPassViewInfo(),
		FScreenPassTextureViewport(Output), FScreenPassTextureViewport(Input), VertexShader, PixelShader, Parameters);

	FRHIGPUTextureReadback Readback(TEXT("VaCuusCompositeGammaReadback"));
	AddEnqueueCopyPass(GraphBuilder, &Readback, Output);
	GraphBuilder.Execute();

	RHICmdList.SubmitAndBlockUntilGPUIdle();
	if (Readback.IsReady())
	{
		int32 RowPitchInPixels = 0;
		if (const void* Data = Readback.Lock(RowPitchInPixels))
		{
			const FFloat16Color* Pixel = static_cast<const FFloat16Color*>(Data);
			Result.Output = FLinearColor(Pixel->R.GetFloat(), Pixel->G.GetFloat(), Pixel->B.GetFloat(), Pixel->A.GetFloat());
			Result.bRead = true;
			Readback.Unlock();
		}
	}
	return Result;
}
} // namespace VaCuusCompositeGammaGPU

/**
 * The permutation's numeric observable: a known premultiplied sRGB-encoded input drawn
 * into a PF_FloatRGBA target through both permutations, read back, compared against the
 * CPU twin. Runs the real pipeline — the .usf decode, the permutation wiring, the
 * parameter struct — not a reimplementation.
 *
 * Restore-the-bug (2026-08-02): with the .usf decode deleted (permutation compiled but
 * pass-through), "decoded rgb matches sRGBToLinear" failed at 0.502 vs 0.216 while
 * pass-through cases kept passing; restored, all green. Both outcomes in the Task 5
 * report.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusCompositeLinearGPUTest, "VaCuus.Render.Composite.LinearOutputGPU",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusCompositeLinearGPUTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusCompositeGammaGPU;

	if (GUsingNullRHI)
	{
		// Vacuous pass by design: the -nullrhi suite cannot draw. The real-RHI leg of
		// the suite (the matrix run) is where this test carries evidence; saying so in
		// the log keeps the skip from reading as coverage.
		UE_LOG(LogVaCuus, Display, TEXT("VaCuus.Render.Composite.LinearOutputGPU: SKIPPED under NullRHI (no draw, no readback)"));
		return true;
	}

	// Premultiplied a=1 and a=0.5 cases. Values quantize to exact unorm bytes on the
	// clear ((128,64,32,255) and (64,32,16,128)) so the encoded input is bit-known.
	const FLinearColor OpaqueRaw(128.0f / 255.0f, 64.0f / 255.0f, 32.0f / 255.0f, 1.0f);
	const FLinearColor HalfRaw(64.0f / 255.0f, 32.0f / 255.0f, 16.0f / 255.0f, 128.0f / 255.0f);

	FCaseResult PassOpaque, PassHalf, LinearOpaque, LinearHalf;
	ENQUEUE_RENDER_COMMAND(VaCuusCompositeGammaGPUCases)
	([&](FRHICommandListImmediate& RHICmdList)
		{
			PassOpaque = RunCase(RHICmdList, OpaqueRaw, /*bLinearOutput=*/false);
			PassHalf = RunCase(RHICmdList, HalfRaw, /*bLinearOutput=*/false);
			LinearOpaque = RunCase(RHICmdList, OpaqueRaw, /*bLinearOutput=*/true);
			LinearHalf = RunCase(RHICmdList, HalfRaw, /*bLinearOutput=*/true);
		});
	FlushRenderingCommands();

	if (!TestTrue(TEXT("All four readbacks completed"),
			PassOpaque.bRead && PassHalf.bRead && LinearOpaque.bRead && LinearHalf.bRead))
	{
		return false;
	}

	// FP16 storage + unorm-8 input: 0.004 absorbs both quantizations at these
	// magnitudes with an order of margin, and would not absorb a missed decode
	// (0.502 vs 0.216 on the red channel).
	const float Tolerance = 0.004f;

	const auto TestClose = [this, Tolerance](const TCHAR* What, const FLinearColor& Actual, const FLinearColor& Expected)
	{
		TestTrue(FString::Printf(TEXT("%s: got (%.4f %.4f %.4f %.4f), expected (%.4f %.4f %.4f %.4f)"), What, Actual.R,
					 Actual.G, Actual.B, Actual.A, Expected.R, Expected.G, Expected.B, Expected.A),
			FMath::Abs(Actual.R - Expected.R) < Tolerance && FMath::Abs(Actual.G - Expected.G) < Tolerance &&
				FMath::Abs(Actual.B - Expected.B) < Tolerance && FMath::Abs(Actual.A - Expected.A) < Tolerance);
	};

	// Pass-through: the M1 contract, numerically untouched.
	TestClose(TEXT("Pass-through keeps opaque input raw"), PassOpaque.Output, OpaqueRaw);
	TestClose(TEXT("Pass-through keeps premultiplied input raw"), PassHalf.Output, HalfRaw);

	// LinearOutput: un-premultiply, decode, re-premultiply; alpha untouched.
	const auto ExpectedLinear = [](const FLinearColor& Raw)
	{
		const float A = Raw.A;
		return FLinearColor(SrgbToLinearChannel(Raw.R / A) * A, SrgbToLinearChannel(Raw.G / A) * A,
			SrgbToLinearChannel(Raw.B / A) * A, A);
	};
	TestClose(TEXT("LinearOutput decodes opaque rgb to sRGBToLinear"), LinearOpaque.Output, ExpectedLinear(OpaqueRaw));
	TestClose(TEXT("LinearOutput un-premultiplies before the decode (a=0.5)"), LinearHalf.Output, ExpectedLinear(HalfRaw));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
