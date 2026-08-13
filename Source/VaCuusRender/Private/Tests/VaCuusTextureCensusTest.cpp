// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusCommandBuffer.h"
#include "VaCuusDefines.h"
#include "VaCuusReplayRenderer.h"

#include "RHICommandList.h"
#include "RenderingThread.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Bead VaCuus-dqs.1: the resident-texture census.
 *
 * WHAT THIS PINS, and why the epic could not start without it. VaCuus-dqs opens on a question
 * about a weapon catalogue -- what do N icons cost, and when do they go away -- and the answer
 * read out of RmlUi's source is "230 KiB each, and they never go away on their own"
 * (FileTextureDatabase has no eviction at all, TextureDatabase.cpp:151-178). That was a claim
 * with NO OBSERVABLE. The command buffer carries per-frame DELTAS, which cannot answer a
 * question about a total, and the replayer's `Textures` map is render-thread-private. So this
 * test is what turns the epic's premise into something a later bead can watch change.
 *
 * THE SIZES ARE THE OWNER'S OWN, deliberately. 213x276 is the icon size the question was asked
 * about, so the number this test asserts (235,152 bytes) is the number in the epic's
 * description rather than a round figure invented here. It is also NON-SQUARE and shares no
 * factor pattern with the other two, so a transposed extent or a copy-paste of the wrong
 * texture's size cannot pass.
 *
 * AND IT ASSERTS BYTES, NOT JUST COUNT. A count alone would pass on a census that read every
 * texture's extent as 1x1 -- and the byte figure is the one a buyer budgets against, so it is
 * the one that has to be right.
 */
namespace VaCuusTextureCensusTest
{
/** RGBA8 payload of the right length; contents are irrelevant here, only the extent is measured. */
static void MakePayload(FVaCuusTextureData& Out, FIntPoint Size)
{
	Out.Size = Size;
	Out.RGBA.SetNumZeroed(int64(Size.X) * int64(Size.Y) * 4);
}

/** The icon from the question that opened VaCuus-dqs, and two neighbours that cannot be confused with it. */
static const FIntPoint IconSize(213, 276);
static const FIntPoint TinySize(8, 8);
static const FIntPoint WideSize(64, 32);

static constexpr uint64 IconBytes = 213ull * 276ull * 4ull;	  // 235,152
static constexpr uint64 TinyBytes = 8ull * 8ull * 4ull;		  // 256
static constexpr uint64 WideBytes = 64ull * 32ull * 4ull;	  // 8,192

static constexpr FVaCuusTextureHandle IconHandle = 101;
static constexpr FVaCuusTextureHandle TinyHandle = 102;
static constexpr FVaCuusTextureHandle WideHandle = 103;
}	 // namespace VaCuusTextureCensusTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusTextureCensusTest, "VaCuus.Render.Texture.Census",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusTextureCensusTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusTextureCensusTest;

	// HEAP-ALLOCATED AND TORN DOWN ON THE RENDER THREAD because that is the class's stated
	// affinity -- "Every method -- including the destructor -- must run on the render thread"
	// (VaCuusReplayRenderer.h). A stack instance in RunTest would destruct on the game thread.
	FVaCuusReplayRenderer* Replayer = new FVaCuusReplayRenderer();

	int32 CountEmpty = -1, CountAfterThree = -1, CountAfterRelease = -1, CountAfterTeardown = -1;
	uint64 BytesEmpty = MAX_uint64, BytesAfterThree = 0, BytesAfterRelease = 0, BytesAfterTeardown = MAX_uint64;

	// THE PUBLISHED TOTAL IS ASSERTED AS A DELTA, never as an absolute. It is process-wide, so a
	// replayer another automation test happens to be holding is legitimately part of it; what is
	// deterministic is how much THIS replayer moved it. That difference is the invariant that
	// matters -- the lock-free number JS reads has to agree with the walk the console command
	// does, or one of the two readers is lying.
	int32 PublishedBefore = 0, PublishedAfterThree = 0, PublishedAfterRelease = 0;
	uint64 PublishedBytesBefore = 0, PublishedBytesAfterThree = 0, PublishedBytesAfterRelease = 0;

	ENQUEUE_RENDER_COMMAND(VaCuusTextureCensus)
	([&](FRHICommandListImmediate& RHICmdList)
		{
			CountEmpty = Replayer->GetResidentTextureCount();
			BytesEmpty = Replayer->GetResidentTextureBytes();
			PublishedBefore = FVaCuusReplayRenderer::GetPublishedTextureCount();
			PublishedBytesBefore = FVaCuusReplayRenderer::GetPublishedTextureBytes();

			// Three textures in one buffer.
			{
				FVaCuusCommandBuffer Buffer;
				Buffer.Generation = 1;
				Buffer.ViewSize = FIntPoint(64, 64);
				MakePayload(Buffer.NewTextures.Add(IconHandle), IconSize);
				MakePayload(Buffer.NewTextures.Add(TinyHandle), TinySize);
				MakePayload(Buffer.NewTextures.Add(WideHandle), WideSize);
				Replayer->Replay(RHICmdList, Buffer);
			}
			CountAfterThree = Replayer->GetResidentTextureCount();
			BytesAfterThree = Replayer->GetResidentTextureBytes();
			PublishedAfterThree = FVaCuusReplayRenderer::GetPublishedTextureCount();
			PublishedBytesAfterThree = FVaCuusReplayRenderer::GetPublishedTextureBytes();

			// A SECOND GENERATION, not the same buffer replayed twice: ShouldConsume() refuses
			// one it has already seen, so a repeated generation would return before retiring
			// anything and this half of the test would pass for the wrong reason.
			{
				FVaCuusCommandBuffer Buffer;
				Buffer.Generation = 2;
				Buffer.ViewSize = FIntPoint(64, 64);
				Buffer.ReleasedTextures.Add(IconHandle);
				Replayer->Replay(RHICmdList, Buffer);
			}
			CountAfterRelease = Replayer->GetResidentTextureCount();
			BytesAfterRelease = Replayer->GetResidentTextureBytes();
			PublishedAfterRelease = FVaCuusReplayRenderer::GetPublishedTextureCount();
			PublishedBytesAfterRelease = FVaCuusReplayRenderer::GetPublishedTextureBytes();

			Replayer->ReleaseResources();
			CountAfterTeardown = Replayer->GetResidentTextureCount();
			BytesAfterTeardown = Replayer->GetResidentTextureBytes();

			delete Replayer;	// render-thread only, like every other method on it
		});
	FlushRenderingCommands();

	TestEqual(TEXT("a fresh replayer holds no textures"), CountEmpty, 0);
	TestEqual(TEXT("...and no bytes"), BytesEmpty, uint64(0));

	TestEqual(TEXT("three uploaded textures are three resident textures"), CountAfterThree, 3);
	TestEqual(TEXT("...costing the sum of their own extents"), BytesAfterThree, IconBytes + TinyBytes + WideBytes);

	TestEqual(TEXT("retiring one leaves two"), CountAfterRelease, 2);
	TestEqual(TEXT("...and takes exactly that texture's bytes with it"), BytesAfterRelease, TinyBytes + WideBytes);

	TestEqual(TEXT("the published total tracks the walk when textures arrive"),
		PublishedAfterThree - PublishedBefore, 3);
	TestEqual(TEXT("...in bytes too"),
		PublishedBytesAfterThree - PublishedBytesBefore, IconBytes + TinyBytes + WideBytes);
	TestEqual(TEXT("...and when one is retired"),
		PublishedAfterRelease - PublishedBefore, 2);
	TestEqual(TEXT("...in bytes too, on the way down"),
		PublishedBytesAfterRelease - PublishedBytesBefore, TinyBytes + WideBytes);

	TestEqual(TEXT("teardown releases everything"), CountAfterTeardown, 0);
	TestEqual(TEXT("...down to zero bytes"), BytesAfterTeardown, uint64(0));

	// The number the epic quotes, printed so a reader of the log can check the arithmetic that
	// the whole texture-memory argument rests on without recomputing it.
	UE_LOG(LogVaCuus, Display,
		TEXT("VaCuus.Render.Texture.Census: one %dx%d icon = %llu bytes (%.1f KiB); three resident = %llu bytes"),
		IconSize.X, IconSize.Y, IconBytes, double(IconBytes) / 1024.0, BytesAfterThree);

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
