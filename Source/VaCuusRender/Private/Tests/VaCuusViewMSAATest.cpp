// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusCommandBuffer.h"
#include "VaCuusDefines.h"
#include "VaCuusReplayRenderer.h"

#include "HAL/IConsoleManager.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * VaCuus-3tg: `vacuus.ViewSampleCount`, the per-view render target's opt-in MSAA.
 *
 * WHAT MAKES THIS TESTABLE, which is the same trick VaCuus.Render.Gradient.DiagonalHardStopAA
 * used for the sibling bead: the defect is "a curve's outline is a staircase", and a staircase
 * is exactly "no pixel is partially covered". So draw a TESSELLATED DISC -- a triangle fan with
 * a radius-dependent point count, which is literally what RmlUi builds a `border-radius` corner
 * from (GeometryBackgroundBorder.cpp:139) -- over a transparent clear, and count the pixels that
 * come back neither fully covered nor fully clear. A single-sampled target produces exactly zero
 * of them at any radius; a multisampled one produces one ring's worth. No golden image, no
 * eyeballing.
 *
 * THE DRAW IS THE PRODUCTION ONE. These tests build an FVaCuusCommandBuffer and hand it to
 * FVaCuusReplayRenderer::Replay, so what is measured is the shipped replay path -- its render
 * pass, its store action, its blend state, its RT -- and not a replica of it that could drift.
 *
 * VENUE. The two drawing tests read back, so they report themselves skipped under -nullrhi and
 * pass vacuously there (the contract VaCuusGradientAATest.cpp:201-210 established); the real-RHI
 * run is where they carry evidence. SampleCountResolve needs no GPU at all and runs everywhere,
 * which is the point of it: the value it guards against is a CRASH, not a wrong pixel.
 */
namespace VaCuusViewMSAATest
{
/** Reports the skip once, in the words the gradient and composite GPU tests use. */
static bool SkippedUnderNullRHI(const TCHAR* TestName)
{
	if (!GUsingNullRHI)
	{
		return false;
	}
	UE_LOG(LogVaCuus, Display, TEXT("%s: SKIPPED under NullRHI (no draw, no readback)"), TestName);
	return true;
}

static constexpr int32 ViewExtent = 128;
static constexpr float DiscRadius = 48.0f;

/**
 * A triangle fan approximating a disc, in the shape RmlUi's own corner tessellation produces:
 * a centre vertex plus NumSegments points on the circumference, wound into NumSegments
 * triangles. Opaque white, which is premultiplied-white, so a fully covered pixel reads
 * (255,255,255,255) and an uncovered one keeps the transparent clear.
 *
 * The segment count is deliberately HIGH (128 for a 48px radius, against RmlUi's ~15 at that
 * size): a coarse fan has long straight edges and those alias in their own right, which would
 * let the test pass for the wrong reason. At this density every edge is under 2.4px long, so
 * what the count below measures is the CIRCLE's boundary and not a polygon's.
 */
static FVaCuusGeometryData MakeDiscFan(int32 NumSegments)
{
	FVaCuusGeometryData Data;

	const FVector2f Center(float(ViewExtent) * 0.5f, float(ViewExtent) * 0.5f);
	const FColor White(255, 255, 255, 255);

	Data.Vertices.Add({Center, White, FVector2f::ZeroVector});
	for (int32 Index = 0; Index < NumSegments; ++Index)
	{
		const float Angle = 2.0f * PI * float(Index) / float(NumSegments);
		Data.Vertices.Add(
			{Center + FVector2f(FMath::Cos(Angle), FMath::Sin(Angle)) * DiscRadius, White, FVector2f::ZeroVector});
	}
	for (int32 Index = 0; Index < NumSegments; ++Index)
	{
		Data.Indices.Add(0);
		Data.Indices.Add(1 + Index);
		Data.Indices.Add(1 + ((Index + 1) % NumSegments));
	}

	return Data;
}

/** One buffer, one disc, nothing else — generation 1, so a fresh replayer accepts it. */
static FVaCuusCommandBuffer MakeDiscBuffer()
{
	FVaCuusCommandBuffer Buffer;
	Buffer.Generation = 1;
	Buffer.ViewSize = FIntPoint(ViewExtent, ViewExtent);

	constexpr FVaCuusGeometryHandle Handle = 1;
	Buffer.NewGeometry.Add(Handle, MakeDiscFan(/*NumSegments=*/128));

	FVaCuusCommand Draw;
	Draw.Type = EVaCuusCommandType::DrawGeometry;
	Draw.Geometry = Handle;
	Buffer.Commands.Add(Draw);

	return Buffer;
}

/**
 * Replays the disc at a given `vacuus.ViewSampleCount` and reads the per-view RT back.
 *
 * The cvar is set on the GAME thread and read on the render thread, which is safe without a
 * flush here BECAUSE the cvar is not ECVF_RenderThreadSafe: OnCVarChange propagates the value
 * straight into the render-thread shadow for anything that is not (ConsoleManager.cpp:487-496).
 * The same reason `vacuus.AsyncTextureUploadBytes` can be flipped from a test.
 *
 * OutSampleCount is what the replayer GRANTED, which need not be what was asked: the platform
 * maximum applies, so a box with no 8x support must not fail an 8x assertion.
 */
static bool RenderDisc(int32 RequestedSampleCount, TArray<FColor>& OutPixels, int32& OutSampleCount)
{
	OutPixels.Reset();
	OutSampleCount = 1;

	IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("vacuus.ViewSampleCount"));
	if (CVar == nullptr)
	{
		return false;
	}
	const int32 Saved = CVar->GetInt();
	CVar->Set(RequestedSampleCount, ECVF_SetByCode);

	bool bRead = false;
	ENQUEUE_RENDER_COMMAND(VaCuusViewMSAADraw)
	([&OutPixels, &OutSampleCount, &bRead](FRHICommandListImmediate& RHICmdList)
		{
			// A replayer per call, so every run starts at generation 1 with no RT to inherit —
			// the same isolation a fresh recorder/replayer pair gets in production.
			FVaCuusReplayRenderer Replayer;
			const FVaCuusCommandBuffer Buffer = MakeDiscBuffer();
			Replayer.Replay(RHICmdList, Buffer);

			OutSampleCount = Replayer.GetReplaySampleCount();

			FRHITexture* Target = Replayer.GetOutputRT();
			if (Target != nullptr)
			{
				RHICmdList.Transition(FRHITransitionInfo(Target, ERHIAccess::SRVMask, ERHIAccess::CopySrc));

				FRHIGPUTextureReadback Readback(TEXT("VaCuusViewMSAAReadback"));
				Readback.EnqueueCopy(RHICmdList, Target);
				RHICmdList.SubmitAndBlockUntilGPUIdle();

				int32 RowPitchInPixels = 0;
				if (Readback.IsReady())
				{
					if (const void* Data = Readback.Lock(RowPitchInPixels))
					{
						// The staging row pitch is the RHI's, not the extent — copy row by row.
						const FColor* Rows = static_cast<const FColor*>(Data);
						OutPixels.SetNumUninitialized(ViewExtent * ViewExtent);
						for (int32 Y = 0; Y < ViewExtent; ++Y)
						{
							FMemory::Memcpy(&OutPixels[Y * ViewExtent], Rows + int64(Y) * RowPitchInPixels,
								ViewExtent * sizeof(FColor));
						}
						Readback.Unlock();
						bRead = true;
					}
				}
			}

			Replayer.ReleaseResources();
		});
	FlushRenderingCommands();

	CVar->Set(Saved, ECVF_SetByCode);
	return bRead;
}

/**
 * Pixels that are neither clear nor fully covered — the entire observable.
 *
 * ALPHA, not colour: the fill is premultiplied white over a transparent clear, so a half-covered
 * pixel is (128,128,128,128) and a clear one is (0,0,0,0). Alpha is the coverage channel by
 * construction of the M1 contract, and testing it rather than R keeps the predicate honest if
 * anyone ever changes the fill colour. The 16/239 margins are the gradient test's, chosen the
 * same way: wide enough that unorm rounding on a fully-covered or fully-clear pixel cannot land
 * inside them.
 */
static int32 CountPartiallyCovered(const TArray<FColor>& Pixels)
{
	int32 Count = 0;
	for (const FColor& Pixel : Pixels)
	{
		if (Pixel.A > 16 && Pixel.A < 239)
		{
			++Count;
		}
	}
	return Count;
}
} // namespace VaCuusViewMSAATest

/**
 * THE CLAMP, and it is a crash guard rather than a taste guard.
 *
 * FVulkanTexture's sample-count switch ends in `checkf(0, TEXT("Unsupported number of samples
 * %d"))` for anything that is not a power of two (VulkanTexture.cpp:481-506), so an unclamped
 * `vacuus.ViewSampleCount 3` would take the process down. This is the one part of the feature
 * that can be asserted with no GPU in the room, which is exactly why it is a free function.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusViewMSAAResolveTest, "VaCuus.Render.MSAA.SampleCountResolve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusViewMSAAResolveTest::RunTest(const FString& Parameters_)
{
	using VaCuusReplay::ResolveViewSampleCount;

	// The documented set passes through untouched on a platform that allows 8.
	TestEqual(TEXT("1 stays 1"), ResolveViewSampleCount(1, 8), 1);
	TestEqual(TEXT("2 stays 2"), ResolveViewSampleCount(2, 8), 2);
	TestEqual(TEXT("4 stays 4"), ResolveViewSampleCount(4, 8), 4);
	TestEqual(TEXT("8 stays 8"), ResolveViewSampleCount(8, 8), 8);

	// Anything else rounds DOWN to a power of two rather than being refused: a user who types 3
	// wants more samples than 1, and 2 is the most that can be delivered.
	TestEqual(TEXT("3 rounds down to 2"), ResolveViewSampleCount(3, 8), 2);
	TestEqual(TEXT("5 rounds down to 4"), ResolveViewSampleCount(5, 8), 4);
	TestEqual(TEXT("7 rounds down to 4"), ResolveViewSampleCount(7, 8), 4);
	TestEqual(TEXT("16 caps at 8"), ResolveViewSampleCount(16, 8), 8);
	TestEqual(TEXT("64 caps at 8"), ResolveViewSampleCount(64, 8), 8);

	// Off, and the degenerate spellings of off.
	TestEqual(TEXT("0 is off"), ResolveViewSampleCount(0, 8), 1);
	TestEqual(TEXT("-1 is off"), ResolveViewSampleCount(-1, 8), 1);
	TestEqual(TEXT("MIN_int32 is off"), ResolveViewSampleCount(MIN_int32, 8), 1);

	// The platform ceiling wins, and is itself floored to a power of two — an RHI that reports a
	// max of 3 can build 2, not 3.
	TestEqual(TEXT("platform max 4 caps 8"), ResolveViewSampleCount(8, 4), 4);
	TestEqual(TEXT("platform max 2 caps 4"), ResolveViewSampleCount(4, 2), 2);
	TestEqual(TEXT("platform max 3 grants 2"), ResolveViewSampleCount(8, 3), 2);
	TestEqual(TEXT("platform max 1 disables"), ResolveViewSampleCount(4, 1), 1);
	TestEqual(TEXT("platform max 0 disables"), ResolveViewSampleCount(4, 0), 1);

	return true;
}

/**
 * THE INVARIANT EVERY CONSUMER RESTS ON: the knob changes the sample count of the target the
 * draws land in and NOTHING about the target they resolve into.
 *
 * This is what made MSAA the cheap option rather than the expensive one on bead VaCuus-3tg, and
 * it is worth an assertion because the failure mode is silent and remote: FVaCuusWorldSink
 * SKIPS its copy whenever the RT extent and the destination extent disagree
 * (VaCuusWorldSink.cpp:71-78), so a change that let this knob move OutputRT's extent or format
 * would freeze every world panel with nothing but a Verbose log to say so.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusViewMSAAOutputRTTest, "VaCuus.Render.MSAA.OutputRTUnchanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusViewMSAAOutputRTTest::RunTest(const FString& Parameters_)
{
	using namespace VaCuusViewMSAATest;

	if (SkippedUnderNullRHI(TEXT("VaCuus.Render.MSAA.OutputRTUnchanged")))
	{
		return true;
	}

	IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("vacuus.ViewSampleCount"));
	if (!TestNotNull(TEXT("vacuus.ViewSampleCount exists"), CVar))
	{
		return false;
	}
	const int32 Saved = CVar->GetInt();

	struct FObserved
	{
		int32 ReplaySamples = 0;
		int32 OutputSamples = 0;
		FIntPoint OutputSize = FIntPoint::ZeroValue;
		EPixelFormat OutputFormat = PF_Unknown;
	};

	const auto Observe = [](int32 Requested)
	{
		IConsoleManager::Get().FindConsoleVariable(TEXT("vacuus.ViewSampleCount"))->Set(Requested, ECVF_SetByCode);

		FObserved Result;
		ENQUEUE_RENDER_COMMAND(VaCuusViewMSAAObserve)
		([&Result](FRHICommandListImmediate& RHICmdList)
			{
				FVaCuusReplayRenderer Replayer;
				const FVaCuusCommandBuffer Buffer = MakeDiscBuffer();
				Replayer.Replay(RHICmdList, Buffer);

				Result.ReplaySamples = Replayer.GetReplaySampleCount();
				if (FRHITexture* Target = Replayer.GetOutputRT())
				{
					Result.OutputSamples = int32(Target->GetDesc().NumSamples);
					Result.OutputSize = Target->GetSizeXY();
					Result.OutputFormat = Target->GetDesc().Format;
				}
				Replayer.ReleaseResources();
			});
		FlushRenderingCommands();
		return Result;
	};

	const FObserved Off = Observe(1);
	const FObserved On = Observe(4);

	CVar->Set(Saved, ECVF_SetByCode);

	TestEqual(TEXT("knob off rasterizes single-sampled"), Off.ReplaySamples, 1);
	if (!TestTrue(TEXT("knob on rasterizes multisampled (platform grants > 1)"), On.ReplaySamples > 1))
	{
		return false;
	}

	// The three facts every downstream consumer reads off GetOutputRT().
	TestEqual(TEXT("OutputRT stays single-sampled with the knob on"), On.OutputSamples, 1);
	TestEqual(TEXT("OutputRT keeps its extent"), On.OutputSize, Off.OutputSize);
	TestEqual(TEXT("OutputRT keeps its format"), int32(On.OutputFormat), int32(Off.OutputFormat));

	UE_LOG(LogVaCuus, Display,
		TEXT("VaCuus.Render.MSAA.OutputRTUnchanged: replay samples %d -> %d, OutputRT %dx%d fmt %d samples %d throughout"),
		Off.ReplaySamples, On.ReplaySamples, On.OutputSize.X, On.OutputSize.Y, int32(On.OutputFormat), On.OutputSamples);

	// TURNING THE KNOB BACK OFF MUST GIVE THE MEMORY BACK, on the same replayer rather than a
	// fresh one -- the whole cost of this feature is that allocation, so a cvar that only ever
	// grew it would be a lie. The two Observe() calls above cannot see this: each builds its own
	// replayer, which starts with nothing to release.
	//
	// ONE REPLAYER ACROSS THREE RENDER COMMANDS, with the cvar flipped from the GAME thread in
	// between: IConsoleVariable::Set on any other thread is `check(0)`
	// (ConsoleManager.cpp:498-502), so the flip cannot live inside a render command and the
	// replayer therefore cannot live on a render command's stack. Heap, deleted by the last one.
	int32 SamplesAfterOn = 0;
	int32 SamplesAfterOffAgain = 0;
	FVaCuusReplayRenderer* Shared = new FVaCuusReplayRenderer();

	CVar->Set(4, ECVF_SetByCode);
	ENQUEUE_RENDER_COMMAND(VaCuusViewMSAAToggleOn)
	([Shared, &SamplesAfterOn](FRHICommandListImmediate& RHICmdList)
		{
			const FVaCuusCommandBuffer Buffer = MakeDiscBuffer();
			Shared->Replay(RHICmdList, Buffer);
			SamplesAfterOn = Shared->GetReplaySampleCount();
		});
	FlushRenderingCommands();

	CVar->Set(1, ECVF_SetByCode);
	ENQUEUE_RENDER_COMMAND(VaCuusViewMSAAToggleOff)
	([Shared, &SamplesAfterOffAgain](FRHICommandListImmediate& RHICmdList)
		{
			// A second GENERATION, not the same buffer: ShouldConsume() refuses one it has
			// already seen, so replaying generation 1 again would return before touching a target.
			FVaCuusCommandBuffer Buffer = MakeDiscBuffer();
			Buffer.Generation = 2;
			Shared->Replay(RHICmdList, Buffer);
			SamplesAfterOffAgain = Shared->GetReplaySampleCount();

			Shared->ReleaseResources();
			delete Shared; // render-thread only, like every other method on it
		});
	FlushRenderingCommands();
	CVar->Set(Saved, ECVF_SetByCode);

	TestTrue(TEXT("the same replayer is multisampled while the knob is on"), SamplesAfterOn > 1);
	TestEqual(TEXT("turning the knob off releases the multisampled target"), SamplesAfterOffAgain, 1);

	return true;
}

/**
 * THE REPORTED DEFECT, AS A NUMBER. A 48px-radius tessellated disc — the same construction
 * RmlUi fills a `border-radius` corner with, and the thing the owner has reported three times
 * as a dial ring reading "broken-pixel".
 *
 * Single-sampled, the disc's boundary is a hard step: every pixel is either fully covered or
 * fully clear, and the count of partially covered pixels is EXACTLY zero. That zero is the bug,
 * stated as a measurement rather than an impression — and it is not an approximate zero, which
 * is what makes the assertion sharp.
 *
 * Restore-the-bug (2026-08-06, Vulkan / RTX 3090, offscreen): with `bResolve` in
 * ReplayCommands forced false -- the multisampled target still created, but the pass binding
 * OutputRT with Clear_Store, which is exactly the pre-3tg pass -- the run reported "partially
 * covered pixels 0 at 1x, 0 at 4x" and failed on
 *   Expected 'multisampled: the boundary is softened (0 partial pixels, expected > 75)' to be true.
 * Restored: 0 at 1x, 200 at 4x. Note the other two tests in this file PASSED throughout that
 * run, which is the right split -- the target was still being created and OutputRT was still
 * untouched; only the resolve was gone.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusViewMSAADiscEdgeTest, "VaCuus.Render.MSAA.TessellatedDiscEdgeAA",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusViewMSAADiscEdgeTest::RunTest(const FString& Parameters_)
{
	using namespace VaCuusViewMSAATest;

	if (SkippedUnderNullRHI(TEXT("VaCuus.Render.MSAA.TessellatedDiscEdgeAA")))
	{
		return true;
	}

	TArray<FColor> Single;
	TArray<FColor> Multi;
	int32 SingleSamples = 0;
	int32 MultiSamples = 0;

	if (!TestTrue(TEXT("single-sampled disc read back"), RenderDisc(1, Single, SingleSamples)))
	{
		return false;
	}
	if (!TestTrue(TEXT("multisampled disc read back"), RenderDisc(4, Multi, MultiSamples)))
	{
		return false;
	}
	if (!TestTrue(TEXT("the platform granted more than one sample"), MultiSamples > 1))
	{
		return false;
	}

	const int32 SinglePartial = CountPartiallyCovered(Single);
	const int32 MultiPartial = CountPartiallyCovered(Multi);

	UE_LOG(LogVaCuus, Display,
		TEXT("VaCuus.Render.MSAA.TessellatedDiscEdgeAA: partially covered pixels %d at %dx, %d at %dx (r=%.0f, %d px target)"),
		SinglePartial, SingleSamples, MultiPartial, MultiSamples, DiscRadius, ViewExtent);

	// The defect, restated: a single-sampled rasterizer produces a step function, so there is no
	// such thing as a partially covered pixel. Not "few" — none.
	TestEqual(TEXT("single-sampled: the disc boundary is a hard staircase"), SinglePartial, 0);

	// The fix. The floor is a quarter of the circumference rather than a tuned number: the
	// boundary is 2*pi*r ~= 302 px long and a coverage-sampled edge leaves most of those pixels
	// partial, so anything at or below 75 would mean the resolve is not happening.
	TestTrue(FString::Printf(TEXT("multisampled: the boundary is softened (%d partial pixels, expected > 75)"), MultiPartial),
		MultiPartial > 75);

	// ...and the disc is still a disc. Its centre must be fully covered and a corner fully clear,
	// or "lots of intermediate pixels" could be satisfied by a resolve that smeared the whole
	// image — the failure a coverage count alone cannot see.
	TestEqual(TEXT("multisampled: the disc centre is still fully covered"),
		int32(Multi[(ViewExtent / 2) * ViewExtent + (ViewExtent / 2)].A), 255);
	TestEqual(TEXT("multisampled: the corner is still fully clear"), int32(Multi[0].A), 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
