// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusDefines.h"

#include "DynamicRHI.h"
#include "RHIResources.h"
#include "RenderingThread.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The one figure the texture-memory epic (VaCuus-dqs) named everywhere and never measured:
 * what the RHI ACTUALLY allocates, against the logical W*H*bpp the census reports.
 *
 * WHY IT MATTERS ENOUGH TO HAVE ITS OWN TEST. VaCuus-dqs.4 measured a 200-icon catalogue at
 * 44.85 MiB loose against 46.87 MiB as one atlas and concluded the atlas costs MORE -- with
 * the caveat, repeated in the script, the commit and the bead, that row-pitch padding could
 * reverse it. 213 x 4 = 852 bytes per row; an RHI that aligns rows to 256 rounds that to 1024,
 * which is +20% on each of two hundred textures against +1.2% once on the atlas. That is the
 * difference between "the atlas is 2 MiB worse" and "the atlas is 7 MiB better", and until now
 * nobody here knew which.
 *
 * A MEASUREMENT, NOT A GATE. It asserts only that the platform answers at all and that its
 * answer is not smaller than the logical size; the numbers themselves are the platform's and
 * are printed for a reader. Asserting a specific padding would pin this project to one RHI on
 * one driver, which is precisely the thing that varies.
 *
 * SELF-SKIPS UNDER NullRHI, the venue discipline VaCuus.Render.Upload.AsyncPayload sets: the
 * null RHI answers a placeholder that would read as a result.
 */
namespace VaCuusTexturePlatformSizeTest
{
/** The catalogue's own two shapes, so the numbers line up with VaCuus-dqs.4's table. */
static const FIntPoint IconSize(213, 276);
static const FIntPoint AtlasSize(4047, 3036);
static constexpr int32 IconCount = 200;

static uint64 LogicalBytes(FIntPoint Size)
{
	return uint64(Size.X) * uint64(Size.Y) * uint64(GPixelFormats[PF_R8G8B8A8].BlockBytes);
}

/** Exactly the desc MakeUITextureDesc issues, so this measures what VaCuus really creates. */
static FRHITextureDesc UIDesc(FIntPoint Size)
{
	return FRHITextureCreateDesc::Create2D(TEXT("VaCuusSizeProbe"), Size, PF_R8G8B8A8)
		.SetFlags(ETextureCreateFlags::ShaderResource)
		.SetInitialState(ERHIAccess::SRVMask);
}
}	 // namespace VaCuusTexturePlatformSizeTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusTexturePlatformSizeTest, "VaCuus.Render.Texture.PlatformSize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusTexturePlatformSizeTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusTexturePlatformSizeTest;

	if (GUsingNullRHI)
	{
		UE_LOG(LogVaCuus, Display,
			TEXT("VaCuus.Render.Texture.PlatformSize: SKIPPED under NullRHI (no platform allocator to ask)"));
		return true;
	}

	uint64 IconPlatform = 0, AtlasPlatform = 0;
	uint32 IconAlign = 0, AtlasAlign = 0;

	// RHICalcTexturePlatformSize is the engine's own "true measure of a texture resource for
	// the current running platform RHI" (RHIResources.h:1924). It allocates nothing -- it asks
	// the platform allocator what a desc would cost -- but it goes through GDynamicRHI, so it
	// belongs on the render thread with everything else that does.
	ENQUEUE_RENDER_COMMAND(VaCuusTexturePlatformSize)
	([&](FRHICommandListImmediate&)
		{
			const FRHICalcTextureSizeResult Icon = RHICalcTexturePlatformSize(UIDesc(IconSize), 0);
			const FRHICalcTextureSizeResult Atlas = RHICalcTexturePlatformSize(UIDesc(AtlasSize), 0);
			IconPlatform = Icon.Size;
			IconAlign = Icon.Align;
			AtlasPlatform = Atlas.Size;
			AtlasAlign = Atlas.Align;
		});
	FlushRenderingCommands();

	const uint64 IconLogical = LogicalBytes(IconSize);
	const uint64 AtlasLogical = LogicalBytes(AtlasSize);

	if (!TestTrue(TEXT("the platform answered for both shapes"), IconPlatform > 0 && AtlasPlatform > 0))
	{
		return false;
	}
	// The only invariant that holds on every platform: an allocation cannot be smaller than the
	// pixels it must hold. Everything above that is the platform's business.
	TestTrue(TEXT("an icon allocates at least its pixels"), IconPlatform >= IconLogical);
	TestTrue(TEXT("the atlas allocates at least its pixels"), AtlasPlatform >= AtlasLogical);

	const double IconOverheadPct = 100.0 * (double(IconPlatform) - double(IconLogical)) / double(IconLogical);
	const double AtlasOverheadPct = 100.0 * (double(AtlasPlatform) - double(AtlasLogical)) / double(AtlasLogical);

	const uint64 LooseTotal = uint64(IconCount) * IconPlatform;
	const uint64 LooseLogicalTotal = uint64(IconCount) * IconLogical;

	UE_LOG(LogVaCuus, Display,
		TEXT("VaCuus.Render.Texture.PlatformSize (VaCuus-dqs, the unmeasured caveat):\n")
		TEXT("  one %dx%d icon : logical %llu B, platform %llu B (align %u) — %+.2f%%\n")
		TEXT("  the %dx%d atlas: logical %llu B, platform %llu B (align %u) — %+.2f%%\n")
		TEXT("  %d loose icons : logical %.2f MiB, platform %.2f MiB\n")
		TEXT("  one atlas      : logical %.2f MiB, platform %.2f MiB\n")
		TEXT("  VERDICT: the atlas is %.2f MiB %s than %d loose files, in real allocations."),
		IconSize.X, IconSize.Y, IconLogical, IconPlatform, IconAlign, IconOverheadPct,
		AtlasSize.X, AtlasSize.Y, AtlasLogical, AtlasPlatform, AtlasAlign, AtlasOverheadPct,
		IconCount, double(LooseLogicalTotal) / (1024.0 * 1024.0), double(LooseTotal) / (1024.0 * 1024.0),
		double(AtlasLogical) / (1024.0 * 1024.0), double(AtlasPlatform) / (1024.0 * 1024.0),
		FMath::Abs(double(LooseTotal) - double(AtlasPlatform)) / (1024.0 * 1024.0),
		AtlasPlatform < LooseTotal ? TEXT("CHEAPER") : TEXT("dearer"), IconCount);

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
