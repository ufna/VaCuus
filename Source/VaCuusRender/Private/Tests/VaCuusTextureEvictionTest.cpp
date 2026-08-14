// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusDefines.h"
#include "VaCuusProbeImages.h"
#include "VaCuusRecordingRenderInterface.h"
#include "VaCuusRmlDocumentHost.h"	  // FVaCuusTextureUnitScale, the budget's unit conversion

#include "VaCuusEngine.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Bead VaCuus-dqs.3: what the eviction sweep is allowed to pick.
 *
 * THE POLICY IS THE WHOLE FEATURE, and it is one function -- CollectEvictableSources. The
 * release machinery under it was proved by VaCuus-dqs.2 and the census over it by dqs.1; what
 * has no other observable is the CHOICE: cold before warm, nothing that was just drawn, and
 * only as much as the budget actually needs.
 *
 * WHY THAT CHOICE IS NOT NEGOTIABLE, measured rather than reasoned (dqs.2): releasing a
 * texture the view is STILL DRAWING frees nothing, because RmlUi reloads it on next use. Run
 * the real sweep with its drawn-gate disabled against a catalogue that is on screen and you
 * get 7,864 evictions in one run, 165 per frame, with the tracked total sitting at 44.85 MiB
 * throughout -- every one of them back before the next sweep looked. A policy that picks by
 * size, or by age of load, or by anything other than "has not been drawn lately" is that.
 *
 * NO RmlUi AND NO RHI HERE. The recorder is a plain object that answers LoadTexture from the
 * file interface and records RenderGeometry calls, so the policy can be driven directly:
 * three real PNGs, some of them touched by a recorded draw, then ask what it would evict.
 */
namespace VaCuusTextureEvictionTest
{
static const FIntPoint ProbeSize(16, 16);
/** Each probe is 16x16 RGBA8. The budget arithmetic below is written in these units. */
static constexpr uint64 ProbeBytes = 16ull * 16ull * 4ull;	  // 1,024
}	 // namespace VaCuusTextureEvictionTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusTextureEvictionTest, "VaCuus.Render.Texture.EvictionPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusTextureEvictionTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusTextureEvictionTest;

	// LoadTexture resolves sources through Rml::GetFileInterface(), so the library must be
	// booted -- and it must be OURS to boot: an Initialize() while a live UI thread owns RmlUi
	// trips FVaCuusEngine's owner-thread checkf rather than failing politely, so the
	// precondition is asserted instead of assumed. Same contract as VaCuus.Render.Recorder.
	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}
	FVaCuusEngine& Engine = FVaCuusEngine::Get();
	if (!TestTrue(TEXT("RmlUi initialized"), Engine.Initialize()))
	{
		return false;
	}
	ON_SCOPE_EXIT { Engine.Shutdown(); };

	const FString TestDir = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("VaCuusEvictionTest")));
	IFileManager::Get().MakeDirectory(*TestDir, /*Tree=*/true);
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*TestDir, /*RequireExists=*/false, /*Tree=*/true); };

	const FString PathA = TestDir / TEXT("evict_a.png");
	const FString PathB = TestDir / TEXT("evict_b.png");
	const FString PathC = TestDir / TEXT("evict_c.png");

	TArray<uint8> PixelsA, PixelsB, PixelsC;
	if (!TestTrue(TEXT("three probe PNGs saved"),
			SaveVaCuusProbePng(PathA, ProbeSize, 255, PixelsA) && SaveVaCuusProbePng(PathB, ProbeSize, 255, PixelsB) &&
				SaveVaCuusProbePng(PathC, ProbeSize, 255, PixelsC)))
	{
		return false;
	}

	FVaCuusRecordingRenderInterface Recorder;

	// Frame 1: load all three. LoadTexture seeds each as drawn NOW -- a texture loads because
	// something is about to draw it, and a zero idle clock would make the very first sweep
	// evict what was just asked for.
	Recorder.BeginFrame(FIntPoint(64, 64));
	Rml::Vector2i Dims;
	const Rml::TextureHandle A = Recorder.LoadTexture(Dims, Rml::String(TCHAR_TO_UTF8(*PathA)));
	const Rml::TextureHandle B = Recorder.LoadTexture(Dims, Rml::String(TCHAR_TO_UTF8(*PathB)));
	const Rml::TextureHandle C = Recorder.LoadTexture(Dims, Rml::String(TCHAR_TO_UTF8(*PathC)));
	if (!TestTrue(TEXT("all three loaded"), A != 0 && B != 0 && C != 0))
	{
		return false;
	}
	const Rml::CompiledGeometryHandle Geo = Recorder.CompileGeometry({}, {});
	Recorder.EndFrameAndPublish();

	TestEqual(TEXT("all three are tracked"), Recorder.GetTrackedTextureBytes(), 3ull * ProbeBytes);

	TArray<FString> Evictable;

	// NOTHING IS COLD YET, so an impossible budget still evicts nothing. This is the assertion
	// that stops the policy from being "free whatever is biggest when short of room".
	Recorder.CollectEvictableSources(/*BudgetBytes=*/0, /*IdleFrames=*/2, Evictable);
	TestEqual(TEXT("a just-loaded texture is never evictable, whatever the budget"), Evictable.Num(), 0);

	// Frames 2..5: keep drawing A only. B and C go cold; A cannot.
	for (int32 Frame = 0; Frame < 4; ++Frame)
	{
		Recorder.BeginFrame(FIntPoint(64, 64));
		Recorder.RenderGeometry(Geo, Rml::Vector2f(0, 0), A);
		Recorder.EndFrameAndPublish();
	}

	// UNDER BUDGET MEANS NO WORK, even with two cold textures sitting there. Eviction is a
	// response to pressure, not a tidying habit -- a texture that costs nothing to keep is
	// cheaper kept than re-decoded.
	Recorder.CollectEvictableSources(/*BudgetBytes=*/10ull * ProbeBytes, /*IdleFrames=*/2, Evictable);
	TestEqual(TEXT("nothing is evicted while the budget is met"), Evictable.Num(), 0);

	// Room for two: one must go, and it must be one of the cold ones.
	Recorder.CollectEvictableSources(/*BudgetBytes=*/2ull * ProbeBytes, /*IdleFrames=*/2, Evictable);
	TestEqual(TEXT("a budget one short evicts exactly one"), Evictable.Num(), 1);
	TestTrue(TEXT("...and it is a texture that has gone undrawn, never the one being drawn"),
		Evictable.Num() == 1 && (Evictable[0] == PathB || Evictable[0] == PathC));

	// Room for one: both cold ones go, and A survives BECAUSE IT IS BEING DRAWN -- the budget
	// is still not met afterwards, which is the honest outcome. A sweep that reached the
	// number by evicting A would free nothing at all (see the header).
	Recorder.CollectEvictableSources(/*BudgetBytes=*/1ull * ProbeBytes, /*IdleFrames=*/2, Evictable);
	TestEqual(TEXT("a budget that only fits one evicts both cold textures"), Evictable.Num(), 2);
	TestFalse(TEXT("...and never the drawn one, even though that leaves the budget unmet"),
		Evictable.Contains(PathA));

	// THE IDLE THRESHOLD IS A REAL GATE AND NOT A ROUNDING: at a threshold longer than this
	// recorder has lived, nothing qualifies however hard the budget presses. This is the knob
	// whose zero setting produced the 7,864-evictions-per-run thrash in the header.
	Recorder.CollectEvictableSources(/*BudgetBytes=*/0, /*IdleFrames=*/1000, Evictable);
	TestEqual(TEXT("a threshold nothing has reached yields nothing"), Evictable.Num(), 0);

	// A RELEASED TEXTURE LEAVES THE BOOKS. Otherwise the sweep would keep nominating a source
	// RmlUi no longer has, and the tracked total -- which the budget is compared against --
	// would drift permanently high and evict the wrong things forever.
	Recorder.BeginFrame(FIntPoint(64, 64));
	Recorder.ReleaseTexture(B);
	Recorder.EndFrameAndPublish();
	TestEqual(TEXT("releasing a texture stops it being tracked"), Recorder.GetTrackedTextureBytes(), 2ull * ProbeBytes);
	Recorder.CollectEvictableSources(/*BudgetBytes=*/0, /*IdleFrames=*/2, Evictable);
	TestFalse(TEXT("...and stops it being nominated"), Evictable.Contains(PathB));

	return true;
}

/**
 * Bead VaCuus-cyn: WHICH BYTES vacuus.TextureBudgetMB IS WRITTEN IN.
 *
 * The census prints what the platform reserves (VaCuus-aam); the recorder counts w*h*4. Until
 * this conversion existed the sweep compared one against the other, so a budget of 40 really
 * admitted ~58 MiB of icons -- a buyer reading a number off vacuus.TextureStats and typing it
 * into the cvar got a limit 46% larger than they asked for.
 *
 * THE NUMBERS ARE THE MEASURED ONES, not invented ratios: 213x276 RGBA8 reserves 344,064 B
 * against 235,152 logical on desktop Vulkan, which VaCuus.Render.Texture.PlatformSize measures
 * and asserts on directly. This test is the arithmetic that hangs off that measurement.
 *
 * NO RHI AND NO HOST HERE, deliberately. The automation suite runs under -nullrhi, where the
 * allocated figure is 0 and every conversion is an identity -- so a test that drove a real
 * sweep would exercise only the fallback and prove nothing about the fix. The conversion is a
 * plain struct precisely so the interesting case can be asserted on a build with no allocator.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusTextureBudgetUnitTest, "VaCuus.Render.Texture.BudgetUnit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusTextureBudgetUnitTest::RunTest(const FString& Parameters)
{
	// One icon, measured. The ratio is 1.463..., i.e. the +46% the bead is about.
	constexpr uint64 IconLogical = 235152;
	constexpr uint64 IconAllocated = 344064;
	constexpr uint64 MiB = 1024ull * 1024ull;

	// UNKNOWN IS IDENTITY, and it has two causes that must behave the same: nothing published
	// yet (both zero) and no allocator to ask (allocated zero, the -nullrhi case this very run
	// is). Either way the sweep falls back to comparing logical against logical, which is what
	// it did before this existed -- a fallback that quietly scaled by something would be worse
	// than no fallback.
	const FVaCuusTextureUnitScale Unpublished;
	TestEqual(TEXT("an unpublished view converts a budget unchanged"), Unpublished.AllocatedToLogical(40 * MiB), 40 * MiB);
	TestEqual(TEXT("...and a total unchanged"), Unpublished.LogicalToAllocated(58 * MiB), 58 * MiB);

	const FVaCuusTextureUnitScale NoRHI{IconLogical, 0};
	TestFalse(TEXT("a census with no allocated figure is not a known scale"), NoRHI.IsKnown());
	TestEqual(TEXT("...and converts a budget unchanged"), NoRHI.AllocatedToLogical(40 * MiB), 40 * MiB);

	// THE HEADLINE. 200 icons: 44.85 MiB logical, 65.62 MiB allocated (VaCuus-aam's figures).
	const FVaCuusTextureUnitScale Icons{200 * IconLogical, 200 * IconAllocated};
	TestTrue(TEXT("a published census with both figures is a known scale"), Icons.IsKnown());

	const uint64 BudgetInRecorderBytes = Icons.AllocatedToLogical(40 * MiB);
	TestEqual(TEXT("a 40 MiB allocated budget is ~27.3 MiB of recorder bytes"),
		double(BudgetInRecorderBytes) / double(MiB), 27.34, /*Tolerance=*/0.01);

	// RESTORING THE BUG IS THE POINT OF THIS LINE: without the conversion the sweep would let
	// the recorder's total reach 40 MiB logical, which is this much art actually resident.
	TestEqual(TEXT("...which is the 58.5 MiB the unconverted budget used to admit"),
		double(Icons.LogicalToAllocated(40 * MiB)) / double(MiB), 58.53, /*Tolerance=*/0.01);

	// The two directions are inverses, so the log line's figure and the threshold agree about
	// the same art. Exact equality would be asserting on double rounding; a byte is the
	// tolerance that means "no unit was dropped".
	const uint64 RoundTripped = Icons.LogicalToAllocated(Icons.AllocatedToLogical(40 * MiB));
	TestTrue(TEXT("the conversions round-trip to within a byte"),
		RoundTripped + 1 >= 40 * MiB && RoundTripped <= 40 * MiB + 1);

	// A NARROW ICON AND A BIG ATLAS DO NOT SHARE A RATIO -- +46% against +1.21% -- which is why
	// this is sampled per view rather than taken process-wide. Same budget, two answers.
	const FVaCuusTextureUnitScale Atlas{49146768, 49741444};	 // 4047x3036 RGBA8, +1.21%
	TestTrue(TEXT("an atlas view converts the same budget much less than an icon view"),
		Atlas.AllocatedToLogical(40 * MiB) > Icons.AllocatedToLogical(40 * MiB) + 10 * MiB);

	// OVERFLOW WOULD READ AS "EVICT EVERYTHING", so it is asserted rather than reasoned about:
	// a wrapped 64-bit product here is a budget of a few bytes against a live total.
	const FVaCuusTextureUnitScale Huge{1ull << 40, 1ull << 41};
	TestTrue(TEXT("a budget near the top of the range does not wrap to nothing"),
		Huge.AllocatedToLogical(TNumericLimits<uint64>::Max() / 4) > MiB);

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
