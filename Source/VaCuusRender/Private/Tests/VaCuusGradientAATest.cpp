// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusCommandBuffer.h"
#include "VaCuusDefines.h"
#include "VaCuusReplayRenderer.h"
#include "VaCuusUIShaders.h"

#include "GlobalRenderResources.h"
#include "PipelineStateCache.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RHIResourceUtils.h"
#include "RHIStaticStates.h"
#include "RenderingThread.h"
#include "ShaderParameterStruct.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * VaCuus-sw1: the gradient decorator's screen-space antialiasing.
 *
 * WHAT MAKES THIS TESTABLE AT ALL. The defect was "hard colour breaks staircase", which
 * sounds like a screenshot-only property — and the repo's own lesson is that
 * "cannot be tested" is usually "needs the right observable". The observable here is
 * plain: draw a gradient whose break is on a KNOWN DIAGONAL and count the pixels that
 * come out strictly between the two stop colours. A step function produces exactly zero
 * of them at any resolution; a screen-space ramp produces one or two, wherever the edge
 * is and however large the element is drawn. No image comparison, no golden file.
 *
 * The four tests are the four cases the fix has to get right at once:
 *   DiagonalHardStopAA    — the reported defect: a coincident stop pair on a 45-degree
 *                           line is a staircase before, a one-pixel ramp after.
 *   SmoothGradientUnchanged — the fix's central claim is that widening a pair already
 *                           wider than a pixel is EXACTLY a no-op. Asserted against the
 *                           analytic smoothstep on a deliberately small (16px) span,
 *                           where a widening that failed to be a no-op would show up at
 *                           ~4/255 — well outside the unorm rounding this tolerates.
 *   ConicSeamClean        — the trap. A conic's T wraps 1 -> 0 along one radial line;
 *                           deriving the pixel width from fwidth(T) spikes there and
 *                           paints a band down it. Sampled either side of that exact
 *                           line, on a conic that closes (first colour == last), where
 *                           the correct answer is "indistinguishable from its neighbours".
 *   RepeatingWrapAA       — a repeating gradient has TWO kinds of break per period, the
 *                           interior stop pair and the period edge itself, and a fix that
 *                           smooths only the first leaves a hatch half-staircased.
 *
 * VENUE. All four draw and read back, so all four report themselves skipped under
 * -nullrhi and pass vacuously — the same contract as VaCuus.Render.Composite.LinearOutputGPU
 * (VaCuusCompositeGammaTest.cpp:150-158). The real-RHI run is where they carry evidence.
 */
namespace VaCuusGradientAATest
{
/**
 * One gradient draw into a Size x Size PF_B8G8R8A8 target, read back.
 *
 * PF_B8G8R8A8 and no TexCreate_SRGB, exactly like the replayer's per-view RT
 * (MakeUITextureDesc's sibling in VaCuusReplayRenderer.cpp) — so a byte read back here is
 * the byte the production path would have stored, not a requantized cousin.
 *
 * The draw is the production one: FVaCuusUIVS, GVaCuusVertexDeclaration,
 * MakePixelToClipMatrix, one quad whose UV is the paint box in LOCAL PIXELS, which is
 * what the decorator records (DecoratorGradient.cpp:268-270). The only deviation from
 * ReplayCommands is the blend state: opaque overwrite instead of premultiplied-over, so
 * the readback is the pixel shader's verbatim output rather than its output composited
 * over a clear.
 */
static bool RenderGradient(int32 Size, const FVaCuusGradientPS::FParameters& InParameters, TArray<FColor>& OutPixels)
{
	bool bRead = false;
	OutPixels.Reset();

	ENQUEUE_RENDER_COMMAND(VaCuusGradientAADraw)
	([Size, InParameters, &OutPixels, &bRead](FRHICommandListImmediate& RHICmdList)
		{
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			TOptionalShaderMapRef<FVaCuusUIVS> VertexShader(ShaderMap);
			TOptionalShaderMapRef<FVaCuusGradientPS> PixelShader(ShaderMap);
			if (!VertexShader.IsValid() || !PixelShader.IsValid())
			{
				return;
			}

			const FIntPoint Extent(Size, Size);
			const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("VaCuusGradientAART"), Extent, PF_B8G8R8A8)
													.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource)
													.SetClearValue(FClearValueBinding::Black)
													.SetInitialState(ERHIAccess::SRVMask);
			FTextureRHIRef Target = RHICmdList.CreateTexture(Desc);

			// One quad covering the whole target, position == UV == local pixels. The
			// vertex colour is opaque white (premultiplied), so OutColor is Fill alone
			// (VaCuusGradient.usf's MainGradientPS ends in Color * Fill).
			const FColor White(255, 255, 255, 255);
			TArray<FVaCuusVertex> Vertices;
			Vertices.Add({FVector2f(0.0f, 0.0f), White, FVector2f(0.0f, 0.0f)});
			Vertices.Add({FVector2f(float(Size), 0.0f), White, FVector2f(float(Size), 0.0f)});
			Vertices.Add({FVector2f(float(Size), float(Size)), White, FVector2f(float(Size), float(Size))});
			Vertices.Add({FVector2f(0.0f, float(Size)), White, FVector2f(0.0f, float(Size))});
			const TArray<int32> Indices = {0, 1, 2, 0, 2, 3};

			FBufferRHIRef VB = UE::RHIResourceUtils::CreateVertexBufferFromArray<FVaCuusVertex>(
				RHICmdList, TEXT("VaCuusGradientAAVB"), EBufferUsageFlags::Static, MakeConstArrayView(Vertices));
			FBufferRHIRef IB = UE::RHIResourceUtils::CreateIndexBufferFromArray<int32>(
				RHICmdList, TEXT("VaCuusGradientAAIB"), EBufferUsageFlags::Static, MakeConstArrayView(Indices));

			RHICmdList.Transition(FRHITransitionInfo(Target, ERHIAccess::SRVMask, ERHIAccess::RTV));
			FRHIRenderPassInfo RPInfo(Target, ERenderTargetActions::Clear_Store);
			RHICmdList.BeginRenderPass(RPInfo, TEXT("VaCuusGradientAA"));
			{
				RHICmdList.SetViewport(0.0f, 0.0f, 0.0f, float(Size), float(Size), 1.0f);

				FGraphicsPipelineStateInitializer PSOInit;
				RHICmdList.ApplyCachedRenderTargets(PSOInit);
				PSOInit.BlendState = TStaticBlendState<>::GetRHI(); // opaque overwrite — see above
				PSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
				PSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
				PSOInit.BoundShaderState.VertexDeclarationRHI = GVaCuusVertexDeclaration.VertexDeclarationRHI;
				PSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
				PSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
				PSOInit.PrimitiveType = PT_TriangleList;
				SetGraphicsPipelineState(RHICmdList, PSOInit, 0);

				FVaCuusUIShaderParameters VSParameters;
				VSParameters.Projection = VaCuusReplay::MakePixelToClipMatrix(Extent);
				VSParameters.UITexture = GWhiteTexture->TextureRHI;
				VSParameters.UISampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
				VSParameters.bUseTexture = 0;
				{
					FRHIBatchedShaderParameters& Batched = RHICmdList.GetScratchShaderParameters();
					SetShaderParameters(Batched, VertexShader, VSParameters);
					RHICmdList.SetBatchedShaderParameters(VertexShader.GetVertexShader(), Batched);
				}
				{
					FRHIBatchedShaderParameters& Batched = RHICmdList.GetScratchShaderParameters();
					SetShaderParameters(Batched, PixelShader, InParameters);
					RHICmdList.SetBatchedShaderParameters(PixelShader.GetPixelShader(), Batched);
				}

				RHICmdList.SetStreamSource(0, VB, 0);
				RHICmdList.DrawIndexedPrimitive(IB, 0, 0, 4, 0, 2, 1);
			}
			RHICmdList.EndRenderPass();
			RHICmdList.Transition(FRHITransitionInfo(Target, ERHIAccess::RTV, ERHIAccess::CopySrc));

			FRHIGPUTextureReadback Readback(TEXT("VaCuusGradientAAReadback"));
			Readback.EnqueueCopy(RHICmdList, Target);
			RHICmdList.SubmitAndBlockUntilGPUIdle();
			if (!Readback.IsReady())
			{
				return;
			}

			int32 RowPitchInPixels = 0;
			if (const void* Data = Readback.Lock(RowPitchInPixels))
			{
				// The staging row pitch is the RHI's, not Size — copy row by row.
				const FColor* Rows = static_cast<const FColor*>(Data);
				OutPixels.SetNumUninitialized(Size * Size);
				for (int32 Y = 0; Y < Size; ++Y)
				{
					FMemory::Memcpy(&OutPixels[Y * Size], Rows + int64(Y) * RowPitchInPixels, Size * sizeof(FColor));
				}
				Readback.Unlock();
				bRead = true;
			}
		});
	FlushRenderingCommands();

	return bRead;
}

/** Zeroed parameter block — the replayer memzeros for the same reason (stack garbage past NumStops). */
static FVaCuusGradientPS::FParameters MakeParameters()
{
	FVaCuusGradientPS::FParameters Parameters;
	FMemory::Memzero(Parameters);
	Parameters.BuiltinDimensions = FVector2f(1.0f, 1.0f);
	return Parameters;
}

static void SetStop(FVaCuusGradientPS::FParameters& Parameters, int32 Index, float Position, const FVector4f& PremulColor)
{
	Parameters.StopColors[Index] = PremulColor;
	Parameters.StopPositions[Index / 4][Index % 4] = Position;
	Parameters.NumStops = FMath::Max(Parameters.NumStops, Index + 1);
}

static const FVector4f Black(0.0f, 0.0f, 0.0f, 1.0f);
static const FVector4f White(1.0f, 1.0f, 1.0f, 1.0f);
static const FVector4f Red(1.0f, 0.0f, 0.0f, 1.0f);
static const FVector4f Green(0.0f, 1.0f, 0.0f, 1.0f);

/** Neither of the two endpoint colours: the entire observable for a black/white edge. */
static bool IsIntermediate(const FColor& Pixel)
{
	return Pixel.R > 16 && Pixel.R < 239;
}

/** Reports the skip once, in the words the composite GPU test uses. */
static bool SkippedUnderNullRHI(const TCHAR* TestName)
{
	if (!GUsingNullRHI)
	{
		return false;
	}
	UE_LOG(LogVaCuus, Display, TEXT("%s: SKIPPED under NullRHI (no draw, no readback)"), TestName);
	return true;
}
} // namespace VaCuusGradientAATest

/**
 * THE REPORTED DEFECT. A linear gradient down the 45-degree diagonal with a coincident
 * stop pair at the middle — which is how RCSS spells a hard colour break, and what every
 * segment boundary of a conic dial ring is made of.
 *
 * Restore-the-bug (2026-08-05, real RHI): with MixStopColors' widening reverted to the
 * reference's verbatim smoothstep(Edge0, max(Edge1, Edge0 + 1e-6), T), "the diagonal
 * break spans at least one intermediate pixel" failed with 0 on all three sampled rows —
 * a pure staircase, which is the defect stated as a number. Restored: 1 per row, which is
 * what the arithmetic says it should be (the ramp is 2px wide in x here and the middle
 * pixel centre is the only one that lands strictly inside it).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGradientDiagonalAATest, "VaCuus.Render.Gradient.DiagonalHardStopAA",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGradientDiagonalAATest::RunTest(const FString& Parameters_)
{
	using namespace VaCuusGradientAATest;
	if (SkippedUnderNullRHI(TEXT("VaCuus.Render.Gradient.DiagonalHardStopAA")))
	{
		return true;
	}

	constexpr int32 Size = 64;
	FVaCuusGradientPS::FParameters Parameters = MakeParameters();
	Parameters.GradientMode = 0; // linear
	Parameters.GradientP = FVector2f(0.0f, 0.0f);
	Parameters.GradientV = FVector2f(float(Size), float(Size)); // T = (x + y) / 128
	SetStop(Parameters, 0, 0.0f, Black);
	SetStop(Parameters, 1, 0.5f, Black);
	SetStop(Parameters, 2, 0.5f, White); // the hard break, on the anti-diagonal x + y = 64
	SetStop(Parameters, 3, 1.0f, White);

	TArray<FColor> Pixels;
	if (!TestTrue(TEXT("Readback completed"), RenderGradient(Size, Parameters, Pixels)))
	{
		return false;
	}

	// Three rows, so the claim is about the whole edge and not one lucky pixel.
	for (const int32 Y : {8, 32, 55})
	{
		int32 NumIntermediate = 0;
		for (int32 X = 0; X < Size; ++X)
		{
			NumIntermediate += IsIntermediate(Pixels[Y * Size + X]) ? 1 : 0;
		}
		// Logged, not just asserted: the count IS the measurement this test exists to
		// make, and a number visible in a green run is what makes a later regression
		// legible as a change rather than as a pass that turned into a fail.
		AddInfo(FString::Printf(TEXT("Row %d: %d intermediate pixels across the diagonal break"), Y, NumIntermediate));
		TestTrue(FString::Printf(TEXT("Row %d: the diagonal break spans at least one intermediate pixel (got %d)"), Y, NumIntermediate),
			NumIntermediate >= 1);
		// An unbounded widening would smear the whole row; the ramp is a pixel wide, and
		// |ddx| + |ddy| on a 45-degree edge is sqrt(2) of one, so three is generous.
		TestTrue(FString::Printf(TEXT("Row %d: the ramp stays about a pixel wide (got %d intermediate)"), Y, NumIntermediate),
			NumIntermediate <= 3);
	}

	// Far field: the widening must not have leaked into the flat ends.
	TestTrue(TEXT("Far side of the break is still pure black"), Pixels[32 * Size + 0].R < 4);
	TestTrue(TEXT("Far side of the break is still pure white"), Pixels[32 * Size + 63].R > 251);

	return true;
}

/**
 * THE NO-OP CLAIM. A two-stop gradient spanning 16 pixels — deliberately small, so one
 * pixel is 1/16 of the whole range and a widening that were NOT a no-op would move the
 * midtones by about 4/255. The expected values are the analytic smoothstep the reference
 * shader has always computed; matching them to 2/255 says the AA work left this gradient
 * bit-for-bit where it was, up to unorm rounding.
 *
 * Restore-the-bug (2026-08-05, real RHI): with MixStopColors' min/max replaced by the
 * unconditional widening smoothstep(Edge0 - HalfAA, Edge1 + HalfAA, T), 11 of the 16
 * pixels failed — pixel 3 at 36 against 31.3, pixel 5 at 73 against 69.7, pixel 15 at 252
 * against 254.3 — the whole curve rescaled by the 1/16 of a range one pixel is worth here.
 * Restored: all 16 within tolerance.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGradientSmoothUnchangedTest, "VaCuus.Render.Gradient.SmoothGradientUnchanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGradientSmoothUnchangedTest::RunTest(const FString& Parameters_)
{
	using namespace VaCuusGradientAATest;
	if (SkippedUnderNullRHI(TEXT("VaCuus.Render.Gradient.SmoothGradientUnchanged")))
	{
		return true;
	}

	constexpr int32 Size = 16;
	FVaCuusGradientPS::FParameters Parameters = MakeParameters();
	Parameters.GradientMode = 0; // linear
	Parameters.GradientP = FVector2f(0.0f, 0.0f);
	Parameters.GradientV = FVector2f(float(Size), 0.0f); // T = x / 16, left to right
	SetStop(Parameters, 0, 0.0f, Black);
	SetStop(Parameters, 1, 1.0f, White);

	TArray<FColor> Pixels;
	if (!TestTrue(TEXT("Readback completed"), RenderGradient(Size, Parameters, Pixels)))
	{
		return false;
	}

	for (int32 X = 0; X < Size; ++X)
	{
		const float T = (float(X) + 0.5f) / float(Size); // pixel centre
		const float Expected = T * T * (3.0f - 2.0f * T) * 255.0f;
		const float Actual = float(Pixels[8 * Size + X].R);
		TestTrue(FString::Printf(TEXT("Pixel %d matches the analytic smoothstep: got %.0f, expected %.1f"), X, Actual, Expected),
			FMath::Abs(Actual - Expected) <= 2.0f);
	}

	return true;
}

/**
 * THE CONIC SEAM. T = 0.5 + atan2(...)/2pi wraps 1 -> 0 along one radial line; with
 * GradientV = (1, 0) that line is straight up from the centre.
 *
 * THE CENTRE IS AT x = 33, NOT 32, AND THE ODD NUMBER IS THE WHOLE TEST. Derivatives are
 * differences across a 2x2 quad, and quads are aligned to EVEN framebuffer coordinates.
 * A seam on x = 32 falls on a quad boundary: the quad {30,31} sits wholly on one side of
 * it and {32,33} wholly on the other, so ddx(T) never sees the jump and even a naive
 * fwidth(T) reads a perfectly ordinary value. Written that way first, this test passed
 * against the bug it exists to catch. On x = 33 the seam runs THROUGH quad {32,33} and
 * the jump is in the derivative where it belongs.
 *
 * The gradient CLOSES (red -> green -> red), so the correct picture has nothing at all
 * happening there: the two columns flanking the seam must be as red as their neighbours.
 * A pixel width taken as fwidth(T) reads ~1.0 on that quad — the whole 0..1 range in one
 * pixel — widens every stop pair to half the range, and paints a desaturated band down
 * the line.
 *
 * Restore-the-bug (2026-08-05, real RHI): with the conic's AAWidth taken from fwidth(T)
 * instead of the analytic angle gradient, all six seam samples failed — (217, 38, 0) on
 * column 32 and (215, 40, 0) on column 33 against the required R>=240, G<=15. That is the
 * band, measured: a 15% wash of the next stop's green painted down the seam. Restored:
 * (255, 0, 0) on all six.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGradientConicSeamTest, "VaCuus.Render.Gradient.ConicSeamClean",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGradientConicSeamTest::RunTest(const FString& Parameters_)
{
	using namespace VaCuusGradientAATest;
	if (SkippedUnderNullRHI(TEXT("VaCuus.Render.Gradient.ConicSeamClean")))
	{
		return true;
	}

	constexpr int32 Size = 64;
	FVaCuusGradientPS::FParameters Parameters = MakeParameters();
	Parameters.GradientMode = 2; // conic
	Parameters.GradientP = FVector2f(33.0f, 32.0f); // x = 33 so the seam runs through a quad, not between two
	Parameters.GradientV = FVector2f(1.0f, 0.0f);   // from 0deg
	SetStop(Parameters, 0, 0.0f, Red);
	SetStop(Parameters, 1, 0.5f, Green);
	SetStop(Parameters, 2, 1.0f, Red); // closes the loop — the seam is invisible when correct

	TArray<FColor> Pixels;
	if (!TestTrue(TEXT("Readback completed"), RenderGradient(Size, Parameters, Pixels)))
	{
		return false;
	}

	// Rows well above the centre, so the radius is 15-25px and the true angular rate is
	// under half a degree per pixel — nothing should be blending here at all.
	for (const int32 Y : {8, 12, 16})
	{
		for (const int32 X : {32, 33})
		{
			const FColor Pixel = Pixels[Y * Size + X];
			TestTrue(FString::Printf(TEXT("Seam column %d at row %d is still red: got (%d %d %d)"), X, Y, Pixel.R, Pixel.G, Pixel.B),
				Pixel.R >= 240 && Pixel.G <= 15);
		}
	}

	// ...and the gradient still works: a quarter turn round from the seam is green-ish.
	TestTrue(TEXT("A quarter turn from the seam is green, so the conic still sweeps"), Pixels[32 * Size + 4].G > 100);

	return true;
}

/**
 * THE REPEATING WRAP. Two kinds of break per period: the coincident stop pair inside it,
 * and the period edge, where T jumps from the last stop back to the first. The second is
 * invisible to the smoothstep chain — it happens between the chain's last edge and its
 * first — so a fix that only widens stop pairs leaves alternate edges staircased.
 *
 * The gradient runs down the 45-degree diagonal at a 32-pixel period along the sampled
 * row, putting interior breaks at x = 16 and 48 and period edges at x = 0 and 32.
 *
 * Restore-the-bug (2026-08-05, real RHI): with the wrap seed and the half-pixel window
 * shift removed (Seed = StopColors[0] and the old T0 + mod(...) wrap), "period edge at
 * x=32 is antialiased" failed with 0 intermediate pixels while the interior break at
 * x=16 kept its 1 — the exact half-smooth/half-staircase picture the shift exists to
 * prevent, and the reason the two counts are asserted EQUAL and not merely nonzero.
 * Restored: 1 at both. (The mirror image also holds: with the per-pair widening reverted
 * but the seed kept, the same equality fails the other way round, 0 interior to 1 period.)
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGradientRepeatingWrapTest, "VaCuus.Render.Gradient.RepeatingWrapAA",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGradientRepeatingWrapTest::RunTest(const FString& Parameters_)
{
	using namespace VaCuusGradientAATest;
	if (SkippedUnderNullRHI(TEXT("VaCuus.Render.Gradient.RepeatingWrapAA")))
	{
		return true;
	}

	constexpr int32 Size = 64;
	FVaCuusGradientPS::FParameters Parameters = MakeParameters();
	Parameters.GradientMode = 0; // linear
	Parameters.bRepeating = 1;
	Parameters.GradientP = FVector2f(0.0f, 0.0f);
	Parameters.GradientV = FVector2f(16.0f, 16.0f); // T = (x + y) / 32
	SetStop(Parameters, 0, 0.0f, White);
	SetStop(Parameters, 1, 0.5f, White);
	SetStop(Parameters, 2, 0.5f, Black); // interior break
	SetStop(Parameters, 3, 1.0f, Black); // ...and the period edge back to White

	TArray<FColor> Pixels;
	if (!TestTrue(TEXT("Readback completed"), RenderGradient(Size, Parameters, Pixels)))
	{
		return false;
	}

	constexpr int32 Row = 32; // T = (x + 32) / 32, so a period edge at x = 32 and an interior break at x = 16
	const auto CountIntermediateNear = [&Pixels](int32 Centre)
	{
		int32 Count = 0;
		for (int32 X = Centre - 2; X <= Centre + 2; ++X)
		{
			Count += IsIntermediate(Pixels[Row * Size + X]) ? 1 : 0;
		}
		return Count;
	};

	const int32 InteriorCount = CountIntermediateNear(16);
	const int32 PeriodCount = CountIntermediateNear(32);
	AddInfo(FString::Printf(TEXT("Interior break: %d intermediate pixels; period edge: %d"), InteriorCount, PeriodCount));
	TestTrue(FString::Printf(TEXT("Interior break at x=16 is antialiased (got %d intermediate pixels)"), InteriorCount), InteriorCount >= 1);
	TestTrue(FString::Printf(TEXT("Period edge at x=32 is antialiased (got %d intermediate pixels)"), PeriodCount), PeriodCount >= 1);

	// Both kinds of edge get the same treatment: a hatch with one crisp edge and one soft
	// edge per band would be worse than the staircase it replaced.
	TestEqual(TEXT("Both kinds of break are widened by the same amount"), PeriodCount, InteriorCount);

	// The bands themselves survive: midway between two breaks the colour is still saturated.
	TestTrue(TEXT("Band centre at x=8 is still white"), Pixels[Row * Size + 8].R > 251);
	TestTrue(TEXT("Band centre at x=24 is still black"), Pixels[Row * Size + 24].R < 4);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
