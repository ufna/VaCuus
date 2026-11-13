// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusCommandBuffer.h"
#include "VaCuusDefines.h"
#include "VaCuusRecordingRenderInterface.h"
#include "VaCuusSlateElement.h"

#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"

#include <RmlUi/Core/Dictionary.h>
#include <RmlUi/Core/Variant.h>

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusGlassOpacityGPU
{
static const FIntPoint ViewSize(64, 64);
static constexpr float Opacities[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 0.0f, 0.5f};
static constexpr int32 NumCases = UE_ARRAY_COUNT(Opacities);

struct FFrameResult
{
	bool bRead = false;
	float Pixel = 0.0f;
};

struct FCaseResult
{
	FFrameResult Published;
	FFrameResult Idle;
	uint64 GlassFrames = 0;
};

/**
 * The recorder sequence is covered through real RmlUi documents by BackdropOpacity and
 * DistillOpacity. Drive that sequence here directly (ElementEffects.cpp:277-305) so this
 * test isolates the production GPU path, including the GlassOpacity binding and blend.
 */
static TUniquePtr<FVaCuusCommandBuffer> RecordFrame(FVaCuusRecordingRenderInterface& Recorder,
	Rml::CompiledFilterHandle Blur, Rml::CompiledFilterHandle& PreviousOpacity, int32 CaseIndex)
{
	Recorder.BeginFrame(ViewSize);
	if (PreviousOpacity)
	{
		Recorder.ReleaseFilter(PreviousOpacity);
		PreviousOpacity = 0;
	}

	const float Opacity = Opacities[CaseIndex];
	if (Opacity > 0.0f && Opacity < 1.0f)
	{
		PreviousOpacity = Recorder.CompileFilter("backdrop-opacity", Rml::Dictionary{{"value", Rml::Variant(Opacity)}});
	}
	if (Opacity > 0.0f)
	{
		const Rml::LayerHandle Layer = Recorder.PushLayer();
		Recorder.CompositeLayers(Rml::LayerHandle(0), Layer, Rml::BlendMode::Blend,
			Rml::Span<const Rml::CompiledFilterHandle>(&Blur, 1));
		const Rml::Span<const Rml::CompiledFilterHandle> OpacityFilters = PreviousOpacity
			? Rml::Span<const Rml::CompiledFilterHandle>(&PreviousOpacity, 1)
			: Rml::Span<const Rml::CompiledFilterHandle>();
		Recorder.CompositeLayers(Layer, Rml::LayerHandle(0), Rml::BlendMode::Blend, OpacityFilters);
		Recorder.PopLayer();
	}

	if (CaseIndex == NumCases - 1)
	{
		// Retirement follows the last buffer's commands. Its distilled entry must still
		// serve the idle draw, which has no new resource payload or UI publication.
		Recorder.ReleaseFilter(PreviousOpacity);
		Recorder.ReleaseFilter(Blur);
		PreviousOpacity = 0;
	}
	return Recorder.EndFrameAndPublish();
}

/** Inverting the scene on an idle frame makes a stale scene/blur distinguishable in the readback. */
static FFrameResult DrawAndRead(FRHICommandListImmediate& RHICmdList, FVaCuusSlateElement& Element, bool bInvertScene)
{
	FFrameResult Result;
	TArray<FColor> Checker;
	Checker.SetNumUninitialized(ViewSize.X * ViewSize.Y);
	for (int32 Y = 0; Y < ViewSize.Y; ++Y)
	{
		for (int32 X = 0; X < ViewSize.X; ++X)
		{
			const bool bWhite = ((X + Y) % 2 == 0) != bInvertScene;
			Checker[Y * ViewSize.X + X] = bWhite ? FColor::White : FColor::Black;
		}
	}

	const FRHITextureCreateDesc SceneDesc = FRHITextureCreateDesc::Create2D(TEXT("VaCuusGlassOpacityScene"), ViewSize, PF_B8G8R8A8)
		.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource)
		.SetInitialState(ERHIAccess::SRVMask);
	FTextureRHIRef Scene = RHICmdList.CreateTexture(SceneDesc);
	RHICmdList.UpdateTexture2D(Scene, 0,
		FUpdateTextureRegion2D(0, 0, 0, 0, uint32(ViewSize.X), uint32(ViewSize.Y)),
		uint32(ViewSize.X * sizeof(FColor)), reinterpret_cast<const uint8*>(Checker.GetData()));

	FRDGBuilder GraphBuilder(RHICmdList);
	FRDGTextureRef Output = RegisterExternalTexture(GraphBuilder, Scene, TEXT("VaCuusGlassOpacityScene"));
	FVaCuusDrawPassInputs Inputs;
	Inputs.OutputTexture = Output;
	Inputs.SceneViewRect = FIntRect(FIntPoint::ZeroValue, ViewSize);
	Inputs.ElementsOffset = FVector2f::ZeroVector;
	Inputs.ElementsMatrix = FMatrix44f::Identity;
	Element.Draw_RenderThread(GraphBuilder, Inputs);

	FRHIGPUTextureReadback Readback(TEXT("VaCuusGlassOpacityReadback"));
	AddEnqueueCopyPass(GraphBuilder, &Readback, Output);
	GraphBuilder.Execute();
	RHICmdList.SubmitAndBlockUntilGPUIdle();
	if (Readback.IsReady())
	{
		int32 RowPitchInPixels = 0;
		if (const void* Data = Readback.Lock(RowPitchInPixels))
		{
			const FColor* Pixels = static_cast<const FColor*>(Data);
			Result.Pixel = Pixels[(ViewSize.Y / 2) * RowPitchInPixels + ViewSize.X / 2].G / 255.0f;
			Result.bRead = true;
			Readback.Unlock();
		}
	}
	return Result;
}
} // namespace VaCuusGlassOpacityGPU

/**
 * A real Slate glass draw over a 1px checker must blend sharp and blurred pixels at the
 * panel's opacity. The same element receives every publish, covering filter replacement,
 * full-opacity release and removal/reappearance at zero. An inverted scene is drawn after
 * each publish without SetPendingBuffer, exercising the persistent entry on an idle frame.
 * NullRHI cannot execute this test's observable and explicitly skips it.
 *
 * Restore-the-bug (2026-11-13, Linux, Vulkan): omitting MainGlassPS's GlassOpacity
 * multiplier gives eight failed blend assertions, on both published and idle draws at
 * fractional opacity. Restored, opacity 0.5 reads 0.7490 over white and 0.2471 over black,
 * against the full blur's 0.4980. Zero-opacity cases schedule no glass passes.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassOpacityGPUTest, "VaCuus.Render.Glass.OpacityGPU",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassOpacityGPUTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusGlassOpacityGPU;
	if (GUsingNullRHI)
	{
		UE_LOG(LogVaCuus, Display, TEXT("VaCuus.Render.Glass.OpacityGPU: SKIPPED under NullRHI (no draw, no readback)"));
		return true;
	}

	FVaCuusRecordingRenderInterface Recorder;
	const Rml::CompiledFilterHandle Blur = Recorder.CompileFilter("blur", Rml::Dictionary{{"sigma", Rml::Variant(12.0f)}});
	Rml::CompiledFilterHandle PreviousOpacity = 0;
	TArray<TUniquePtr<FVaCuusCommandBuffer>> Buffers;
	for (int32 Index = 0; Index < NumCases; ++Index)
	{
		Buffers.Add(RecordFrame(Recorder, Blur, PreviousOpacity, Index));
		if (!TestNotNull(TEXT("Each opacity change publishes a frame"), Buffers.Last().Get()))
		{
			return false;
		}
	}

	FCaseResult Results[NumCases];
	ENQUEUE_RENDER_COMMAND(VaCuusGlassOpacityGPUCases)
	([&](FRHICommandListImmediate& RHICmdList)
		{
			FVaCuusSlateElement Element;
			Element.SetDestRect_RenderThread(FIntRect(FIntPoint::ZeroValue, ViewSize));
			for (int32 Index = 0; Index < NumCases; ++Index)
			{
				Element.SetPendingBuffer_RenderThread(RHICmdList, MoveTemp(Buffers[Index]));
				const uint64 Before = Element.GetNumGlassFrames_RenderThread();
				Results[Index].Published = DrawAndRead(RHICmdList, Element, false);
				Results[Index].Idle = DrawAndRead(RHICmdList, Element, true);
				Results[Index].GlassFrames = Element.GetNumGlassFrames_RenderThread() - Before;
			}
			Element.ReleaseResources_RenderThread();
		});
	FlushRenderingCommands();

	for (int32 Index = 0; Index < NumCases; ++Index)
	{
		if (!TestTrue(FString::Printf(TEXT("Case %d: both readbacks completed"), Index),
			Results[Index].Published.bRead && Results[Index].Idle.bRead))
		{
			return false;
		}
	}

	// The full-opacity readback calibrates the blur's truncation/quantization loss. It
	// must independently be near the checker's mean, so a missing draw cannot calibrate
	// every expected result back to the untouched scene.
	const float FullBlur = Results[4].Published.Pixel;
	TestTrue(TEXT("Full blur removes the checker detail"), FullBlur > 0.45f && FullBlur < 0.55f);
	for (int32 Index = 0; Index < NumCases; ++Index)
	{
		const float Opacity = Opacities[Index];
		const float ExpectedPublished = 1.0f - Opacity + Opacity * FullBlur;
		const float ExpectedIdle = Opacity * FullBlur;
		const FCaseResult& Result = Results[Index];
		AddInfo(FString::Printf(TEXT("Case %d, opacity %.2f: published %.4f (expected %.4f), idle %.4f (expected %.4f)"),
			Index, Opacity, Result.Published.Pixel, ExpectedPublished, Result.Idle.Pixel, ExpectedIdle));

		// Three unorm steps cover blur-target and output quantization. Omitting opacity
		// differs by at least 0.125 at the fractional values in this fixture.
		TestTrue(FString::Printf(TEXT("Case %d: published draw blends at panel opacity"), Index),
			FMath::Abs(Result.Published.Pixel - ExpectedPublished) < 0.012f);
		TestTrue(FString::Printf(TEXT("Case %d: idle draw keeps opacity over the changed scene"), Index),
			FMath::Abs(Result.Idle.Pixel - ExpectedIdle) < 0.012f);
		TestEqual(FString::Printf(TEXT("Case %d: only visible glass schedules passes"), Index),
			Result.GlassFrames, Opacity > 0.0f ? uint64(2) : uint64(0));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
