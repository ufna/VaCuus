// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusDefines.h"
#include "VaCuusProbeImages.h"
#include "VaCuusRecordingRenderInterface.h"

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

#endif	  // WITH_DEV_AUTOMATION_TESTS
