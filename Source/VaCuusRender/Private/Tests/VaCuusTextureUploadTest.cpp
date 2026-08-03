// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusCommandBuffer.h"
#include "VaCuusDefines.h"
#include "VaCuusReplayRenderer.h"

#include "HAL/IConsoleManager.h"
#include "Misc/ScopeExit.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Bead VaCuus-akj.6.25: the upload leaves the render thread.
 *
 * TWO TESTS WITH TWO DIFFERENT JOBS, and the split is the point. `Cost` is a MEASUREMENT and
 * asserts nothing about time -- a wall clock on a shared build box is evidence, not a gate --
 * it exists so the threshold's default has a number behind it that anyone can re-run.
 * `AsyncPayload` is the CORRECTNESS gate: that the async path routes, that its pixels are
 * byte-exact, and that the RHI-thread ordering holds, i.e. that a command recorded after the
 * async list really does see the filled image.
 *
 * Both self-skip loudly under NullRHI, the venue discipline of
 * VaCuus.Render.Composite.LinearOutputGPU: the -nullrhi suite creates no staging buffer, copies
 * nothing, and reads nothing back, so a silent pass there would read as coverage it is not.
 */
namespace VaCuusTextureUploadTest
{
/** Deterministic RGBA8 fill; no two consecutive bytes are equal, so a partial copy cannot pass by luck. */
static void FillPattern(TArray<uint8>& OutBytes, FIntPoint Size, uint32 Seed)
{
	OutBytes.SetNumUninitialized(int64(Size.X) * int64(Size.Y) * 4);
	for (int32 Index = 0; Index < OutBytes.Num(); ++Index)
	{
		OutBytes[Index] = uint8((uint32(Index) * 31u + Seed) & 0xFFu);
	}
}

struct FCost
{
	double CreateMs = 0.0;
	double UpdateMs = 0.0;
};

/** Times the two production calls separately, on the render thread, exactly as UploadNewResources issues them. */
static FCost TimeInlineUpload(FRHICommandList& RHICmdList, const TArray<uint8>& Bytes, FIntPoint Size)
{
	FCost Cost;

	const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("VaCuusUploadProbe"), Size, PF_R8G8B8A8)
										   .SetFlags(ETextureCreateFlags::ShaderResource)
										   .SetInitialState(ERHIAccess::SRVMask);

	const double T0 = FPlatformTime::Seconds();
	FTextureRHIRef Texture = RHICmdList.CreateTexture(Desc);
	const double T1 = FPlatformTime::Seconds();

	const FUpdateTextureRegion2D Region(0, 0, 0, 0, uint32(Size.X), uint32(Size.Y));
	RHICmdList.UpdateTexture2D(Texture, 0, Region, uint32(Size.X) * 4u, Bytes.GetData());
	const double T2 = FPlatformTime::Seconds();

	Cost.CreateMs = (T1 - T0) * 1000.0;
	Cost.UpdateMs = (T2 - T1) * 1000.0;
	return Cost;
}

/**
 * Copies an uploaded texture back off the GPU. Recorded on the SAME immediate list that
 * BeginAsyncTextureUploads queued the parallel list into, which is what makes this an ordering
 * test and not merely a content test: the copy is a later command in that stream, so it can only
 * see the payload if the RHI thread really did replay the async list first.
 */
static bool ReadBackRGBA(FRHICommandListImmediate& RHICmdList, FRHITexture* Texture, FIntPoint Size, TArray<uint8>& OutBytes)
{
	if (Texture == nullptr)
	{
		return false;
	}

	// The replayer's out-of-upload invariant for a UI texture is SRVMask (MakeUITextureDesc).
	RHICmdList.Transition(FRHITransitionInfo(Texture, ERHIAccess::SRVMask, ERHIAccess::CopySrc));

	FRHIGPUTextureReadback Readback(TEXT("VaCuusUploadReadback"));
	Readback.EnqueueCopy(RHICmdList, Texture, FIntVector::ZeroValue, 0, FIntVector(Size.X, Size.Y, 1));
	RHICmdList.SubmitAndBlockUntilGPUIdle();

	RHICmdList.Transition(FRHITransitionInfo(Texture, ERHIAccess::CopySrc, ERHIAccess::SRVMask));

	if (!Readback.IsReady())
	{
		return false;
	}

	int32 RowPitchInPixels = 0;
	const void* Data = Readback.Lock(RowPitchInPixels);
	if (Data == nullptr)
	{
		return false;
	}

	OutBytes.SetNumUninitialized(int64(Size.X) * int64(Size.Y) * 4);
	for (int32 Y = 0; Y < Size.Y; ++Y)
	{
		FMemory::Memcpy(OutBytes.GetData() + int64(Y) * Size.X * 4,
			static_cast<const uint8*>(Data) + int64(Y) * RowPitchInPixels * 4, int64(Size.X) * 4);
	}
	Readback.Unlock();
	return true;
}

/** First differing byte index, or INDEX_NONE. */
static int32 FirstDifference(const TArray<uint8>& A, const TArray<uint8>& B)
{
	if (A.Num() != B.Num())
	{
		return 0;
	}
	for (int32 Index = 0; Index < A.Num(); ++Index)
	{
		if (A[Index] != B[Index])
		{
			return Index;
		}
	}
	return INDEX_NONE;
}
} // namespace VaCuusTextureUploadTest

/**
 * WHAT ONE IMAGE UPLOAD COSTS THE RENDER THREAD, measured rather than assumed — the evidence
 * bead VaCuus-akj.6.25 asks for before anything is built, and the number behind
 * vacuus.AsyncTextureUploadBytes' default.
 *
 * It times the two calls UploadNewResources issues, separately, because they are not remotely
 * the same size: on Vulkan the whole cost is UpdateTexture2D's staging memcpy
 * (VulkanTexture.cpp:1664-1677), and knowing that is what makes "create synchronously, upload
 * asynchronously" the right split rather than a guess.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusTextureUploadCostTest, "VaCuus.Render.Upload.Cost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusTextureUploadCostTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusTextureUploadTest;

	if (GUsingNullRHI)
	{
		UE_LOG(LogVaCuus, Display, TEXT("VaCuus.Render.Upload.Cost: SKIPPED under NullRHI (no staging buffer, no memcpy)"));
		return true;
	}

	const FIntPoint Sizes[] = {FIntPoint(256, 256), FIntPoint(512, 512), FIntPoint(1024, 1024), FIntPoint(2048, 2048),
		FIntPoint(4096, 4096), FIntPoint(6000, 6000)};

	// The AFTER arm shares one replayer across the sizes so its generations stay monotonic; see
	// the AsyncPayload test for why it is heap-allocated.
	FVaCuusReplayRenderer* Replayer = new FVaCuusReplayRenderer();
	ON_SCOPE_EXIT
	{
		ENQUEUE_RENDER_COMMAND(VaCuusUploadCostTeardown)
		([Replayer](FRHICommandListImmediate&)
			{
				Replayer->ReleaseResources();
				delete Replayer;
			});
		FlushRenderingCommands();
	};

	IConsoleVariable* ThresholdVar = IConsoleManager::Get().FindConsoleVariable(TEXT("vacuus.AsyncTextureUploadBytes"));
	if (!TestNotNull(TEXT("vacuus.AsyncTextureUploadBytes exists"), ThresholdVar))
	{
		return false;
	}
	const int32 SavedThreshold = ThresholdVar->GetInt();
	ThresholdVar->Set(1, ECVF_SetByCode); // every size takes the async route in the AFTER arm
	ON_SCOPE_EXIT
	{
		ThresholdVar->Set(SavedThreshold, ECVF_SetByCode);
	};

	uint64 Generation = 0;
	FVaCuusTextureHandle Handle = 0;

	for (const FIntPoint Size : Sizes)
	{
		TArray<uint8> Bytes;
		FillPattern(Bytes, Size, 7);

		// ONE BUFFER PER SIZE FOR THE AFTER ARM, built before the timing so the payload copy is
		// not measured: BeginAsyncTextureUploads MOVES the payload out, so it cannot be reused.
		TUniquePtr<FVaCuusCommandBuffer> AsyncBuffer = MakeUnique<FVaCuusCommandBuffer>();
		AsyncBuffer->Generation = ++Generation;
		FVaCuusTextureData& AsyncPayload = AsyncBuffer->NewTextures.Add(++Handle);
		AsyncPayload.Size = Size;
		AsyncPayload.RGBA = Bytes;

		FCost First, Second;
		double AsyncStartMs = 0.0;
		ENQUEUE_RENDER_COMMAND(VaCuusUploadCostProbe)
		([&First, &Second, &AsyncStartMs, &Bytes, Size, Replayer, &AsyncBuffer](FRHICommandListImmediate& RHICmdList)
			{
				// BEFORE, twice: the first pass pays whatever the staging manager has to allocate
				// for a size it has never seen (FStagingManager::AcquireBuffer pools by EXACT
				// size, VulkanMemory.cpp:4449-4452), the second reuses it. Production sees both.
				First = TimeInlineUpload(RHICmdList, Bytes, Size);
				Second = TimeInlineUpload(RHICmdList, Bytes, Size);

				// AFTER: everything the render thread still pays for this payload -- the texture
				// create, the parallel command list, the QueueAsyncCommandListSubmit (which
				// dispatches the immediate list, so its cost is honestly inside this number) and
				// the task launch. The memcpy is what is no longer here.
				const double T0 = FPlatformTime::Seconds();
				Replayer->BeginAsyncTextureUploads(RHICmdList, *AsyncBuffer);
				AsyncStartMs = (FPlatformTime::Seconds() - T0) * 1000.0;

				// Completes the handover so the replayer is not left holding a parked texture.
				Replayer->ConsumeResources(RHICmdList, *AsyncBuffer);
			});
		FlushRenderingCommands();

		AddInfo(FString::Printf(TEXT("%dx%d (%.1f MB) INLINE: create %.3f/%.3f ms, update %.3f/%.3f ms (cold/warm staging) ")
								TEXT("-> render thread %.3f/%.3f ms | ASYNC START: %.3f ms"),
			Size.X, Size.Y, double(Bytes.Num()) / (1024.0 * 1024.0), First.CreateMs, Second.CreateMs, First.UpdateMs,
			Second.UpdateMs, First.CreateMs + First.UpdateMs, Second.CreateMs + Second.UpdateMs, AsyncStartMs));
	}

	return true;
}

/**
 * THE CORRECTNESS GATE for the async upload path, and the byte-exact precedent it follows is
 * VaCuus.Render.Recorder.LoadTexture: a known pattern in, the same bytes out, compared texel by
 * texel rather than sampled.
 *
 * Three properties, none of which the other two imply:
 *
 *  (1) ROUTING. One payload over the threshold and one under it, in ONE buffer, must split
 *      1 async / 1 sync. Without the counters this is invisible — both paths leave the same
 *      pixels in the same map (the reason GetNumAsyncTextureUploads exists at all).
 *  (2) CONTENT AND ORDERING. The readback is recorded on the same immediate command list the
 *      parallel list was queued into, AFTER it, so it reads through the RHI thread's own
 *      ordering. A byte-exact result therefore says both "the memcpy happened on the worker"
 *      and "nothing downstream can observe the image before it did".
 *  (3) THE TWO PATHS AGREE. The same big payload is then re-run with
 *      vacuus.AsyncTextureUploadBytes = 0 — the documented kill switch — and must come back
 *      byte-identical through the inline path. That is what stops the async path silently
 *      becoming the only tested one.
 *
 * Restore-the-bug (2026-08-03): with the UpdateTexture2D call deleted from the async task's body
 * (so the parallel list carries nothing), "async payload is byte-exact" failed at byte 0 while
 * the sync payload and both counters kept passing; restored, all green. Both outcomes verbatim
 * in the bead report.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusTextureUploadAsyncTest, "VaCuus.Render.Upload.AsyncPayload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusTextureUploadAsyncTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusTextureUploadTest;

	if (GUsingNullRHI)
	{
		UE_LOG(LogVaCuus, Display,
			TEXT("VaCuus.Render.Upload.AsyncPayload: SKIPPED under NullRHI (no upload, no readback)"));
		return true;
	}

	IConsoleVariable* ThresholdVar = IConsoleManager::Get().FindConsoleVariable(TEXT("vacuus.AsyncTextureUploadBytes"));
	if (!TestNotNull(TEXT("vacuus.AsyncTextureUploadBytes exists"), ThresholdVar))
	{
		return false;
	}
	const int32 SavedThreshold = ThresholdVar->GetInt();
	ON_SCOPE_EXIT
	{
		ThresholdVar->Set(SavedThreshold, ECVF_SetByCode);
	};

	// 1024x1024 = 4 MB, at the shipped default, so the sizes under test are the shipped decision
	// rather than a threshold invented for the test. 8x8 = 256 bytes is unambiguously under it.
	const FIntPoint BigSize(1024, 1024);
	const FIntPoint SmallSize(8, 8);
	const FVaCuusTextureHandle BigHandle = 11;
	const FVaCuusTextureHandle SmallHandle = 12;

	TArray<uint8> BigPattern, SmallPattern;
	FillPattern(BigPattern, BigSize, 7);
	FillPattern(SmallPattern, SmallSize, 131);

	const auto MakeBuffer = [&](uint64 Generation, bool bIncludeSmall)
	{
		TUniquePtr<FVaCuusCommandBuffer> Buffer = MakeUnique<FVaCuusCommandBuffer>();
		Buffer->Generation = Generation;
		Buffer->ViewSize = FIntPoint(64, 64);
		FVaCuusTextureData& Big = Buffer->NewTextures.Add(BigHandle);
		Big.Size = BigSize;
		Big.RGBA = BigPattern;
		if (bIncludeSmall)
		{
			FVaCuusTextureData& Small = Buffer->NewTextures.Add(SmallHandle);
			Small.Size = SmallSize;
			Small.RGBA = SmallPattern;
		}
		return Buffer;
	};

	// HEAP-ALLOCATED AND TORN DOWN ON THE RENDER THREAD because that is the class's stated
	// affinity -- "Every method -- including the destructor -- must run on the render thread"
	// (VaCuusReplayRenderer.h:31-32). A stack instance in RunTest would destruct on the game
	// thread and quietly break the one rule the type asks for.
	FVaCuusReplayRenderer* Replayer = new FVaCuusReplayRenderer();
	ON_SCOPE_EXIT
	{
		ENQUEUE_RENDER_COMMAND(VaCuusUploadTeardown)
		([Replayer](FRHICommandListImmediate&)
			{
				Replayer->ReleaseResources();
				delete Replayer;
			});
		FlushRenderingCommands();
	};

	// ---- Pass 1: the default threshold. One buffer, two payloads, one of each route. ----
	ThresholdVar->Set(4 * 1024 * 1024, ECVF_SetByCode);
	TUniquePtr<FVaCuusCommandBuffer> Buffer = MakeBuffer(/*Generation=*/1, /*bIncludeSmall=*/true);

	TArray<uint8> BigReadback, SmallReadback;
	bool bBigRead = false;
	bool bSmallRead = false;
	ENQUEUE_RENDER_COMMAND(VaCuusUploadAsyncCase)
	([&](FRHICommandListImmediate& RHICmdList)
		{
			// Production's own two phases, in production's order: the async start at RDG
			// graph-build time (VaCuusSlateElement.cpp's step 1b), then the buffer consumed the
			// way the replay pass consumes it.
			Replayer->BeginAsyncTextureUploads(RHICmdList, *Buffer);
			Replayer->ConsumeResources(RHICmdList, *Buffer);

			bBigRead = ReadBackRGBA(RHICmdList, Replayer->GetTextureForTest(BigHandle), BigSize, BigReadback);
			bSmallRead = ReadBackRGBA(RHICmdList, Replayer->GetTextureForTest(SmallHandle), SmallSize, SmallReadback);
		});
	FlushRenderingCommands();

	TestEqual(TEXT("the 4 MB payload took the async path"), int64(Replayer->GetNumAsyncTextureUploads()), int64(1));
	TestEqual(TEXT("the 256-byte payload took the inline path"), int64(Replayer->GetNumSyncTextureUploads()), int64(1));

	if (TestTrue(TEXT("async payload read back"), bBigRead))
	{
		const int32 Diff = FirstDifference(BigReadback, BigPattern);
		TestEqual(FString::Printf(TEXT("async payload is byte-exact (first difference at %d of %d)"), Diff, BigPattern.Num()),
			Diff, INDEX_NONE);
	}
	if (TestTrue(TEXT("inline payload read back"), bSmallRead))
	{
		const int32 Diff = FirstDifference(SmallReadback, SmallPattern);
		TestEqual(FString::Printf(TEXT("inline payload is byte-exact (first difference at %d of %d)"), Diff, SmallPattern.Num()),
			Diff, INDEX_NONE);
	}

	// ---- Pass 2: the kill switch. The same big payload must survive the inline route. ----
	ThresholdVar->Set(0, ECVF_SetByCode);
	TUniquePtr<FVaCuusCommandBuffer> InlineBuffer = MakeBuffer(/*Generation=*/2, /*bIncludeSmall=*/false);

	TArray<uint8> InlineReadback;
	bool bInlineRead = false;
	ENQUEUE_RENDER_COMMAND(VaCuusUploadInlineCase)
	([&](FRHICommandListImmediate& RHICmdList)
		{
			Replayer->BeginAsyncTextureUploads(RHICmdList, *InlineBuffer);
			Replayer->ConsumeResources(RHICmdList, *InlineBuffer);
			bInlineRead = ReadBackRGBA(RHICmdList, Replayer->GetTextureForTest(BigHandle), BigSize, InlineReadback);
		});
	FlushRenderingCommands();

	TestEqual(TEXT("vacuus.AsyncTextureUploadBytes 0 sends nothing async"), int64(Replayer->GetNumAsyncTextureUploads()), int64(1));
	TestEqual(TEXT("vacuus.AsyncTextureUploadBytes 0 sends the 4 MB payload inline"),
		int64(Replayer->GetNumSyncTextureUploads()), int64(2));

	if (TestTrue(TEXT("kill-switch payload read back"), bInlineRead))
	{
		const int32 Diff = FirstDifference(InlineReadback, BigPattern);
		TestEqual(
			FString::Printf(TEXT("both routes produce identical pixels (first difference at %d of %d)"), Diff, BigPattern.Num()),
			Diff, INDEX_NONE);
	}

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
