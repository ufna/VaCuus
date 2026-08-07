// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusCommandBuffer.h"
#include "VaCuusEngine.h"
#include "VaCuusFrameSink.h"
#include "VaCuusRmlDocumentHost.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "HAL/PlatformProcess.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * BEADS VaCuus-u0q AND VaCuus-iuv — THE TWO PROPERTIES THAT NEED LAYER CAPTURE MUST FAIL
 * LOUDLY, ONCE, AND WITHOUT DAMAGE.
 *
 * `box-shadow` and `mask-image` are the only two callers in the whole tree of the only two
 * Rml::RenderInterface virtuals VaCuus does not implement — SaveLayerAsTexture
 * (GeometryBoxShadow.cpp:235) and SaveLayerAsMaskImage (ElementEffects.cpp:306). Leaving both at
 * their optional zero-returning defaults (RenderInterface.cpp:37-45) is legal by RmlUi's contract
 * and was silent, and for box-shadow it was also destructive in two separate ways:
 *
 *   1. THE WHITE RECTANGLE. ElementBackgroundBorder::GenerateGeometry erases the element's normal
 *      background and border when box-shadow is present and renders BoxShadowRenderable::geometry
 *      instead — a premultiplied WHITE quad covering the border box extended by 1.5*blur+spread
 *      (BoxShadowCache.cpp:56-59) — which with a null texture handle draws as pure vertex colour.
 *      The element's own background and border are replaced by an opaque white box.
 *
 *   2. THE FRAME-RATE CHURN. The texture callback returned true even though it produced no
 *      handle, so CallbackTextureDatabase::EnsureLoaded saw neither a handle nor load_failed and
 *      re-ran the ENTIRE callback every frame (TextureDatabase.cpp:46-60) — a layer push, a blur
 *      compiled and released, geometry made and released. That is permanent resource traffic,
 *      which permanently trips FVaCuusCommandBuffer::HasResourceTraffic() and forces a publish
 *      every frame on a document that never changes.
 *
 * Both are fixed by VaCuus patch #3 to the vendored RmlUi (Source/ThirdParty/RmlUi/
 * VENDORED_TAG.txt); this test is that patch's re-vendoring check, and the refusal's.
 *
 * WHAT MAKES THIS A TEST AND NOT A CLAIM, measured rather than asserted — and the first line of
 * it corrects the obvious guess about which hunk does what:
 *
 *   - Revert BOTH RmlUi hunks (the true pre-fix state) and section 2 never even reaches its
 *     hundred frames: the view fails to settle, reporting 201 published in 201 recorded. Every
 *     frame published, which is the bead's headline symptom.
 *   - Revert ONLY the ElementBackgroundBorder.cpp hunk and section 3 reports 12 opaque-white
 *     vertices (three shadowed elements, four corners each) and 8 of the element's own background
 *     colour instead of 20. That is the white rectangle, counted.
 *   - Revert ONLY the GeometryBoxShadow.cpp hunk and THIS TEST STILL PASSES, which is worth
 *     stating because it is not what one would guess. Once GenerateGeometry stops committing to
 *     the shadow path, nothing on an IDLE document ever asks for the shadow texture again, so
 *     there is no per-frame callback left to re-run whether or not it latched load_failed. What
 *     that hunk is actually load-bearing for is the RESTYLE case, and that is a separate test
 *     below (VaCuus.Render.LayerCapture.RestyleChurn) rather than a claim made here.
 *   - Remove either latch in FVaCuusRecordingRenderInterface and section 4's Warnings count
 *     climbs with the Calls count instead of staying at 1.
 *
 * THE MASK-IMAGE HALF IS A WARNING ONLY, deliberately: masking correctly needs the same layer
 * capture, so there is nothing to make harmless short of implementing it (bead VaCuus-iuv notes
 * the dependency). What section 4 pins is that the refusal is counted every frame and logged
 * once — which is the property most at risk, since RenderEffects reaches it on every frame of
 * every masked element.
 */
namespace VaCuusLayerCaptureTest
{
static const FIntPoint GViewSize(400, 300);

/**
 * The shadowed elements' background colour, and the one the LAST frame's draws are searched for.
 * Opaque, so RmlUi's ToPremultiplied leaves the three channels alone and the bytes below are
 * exactly what MeshUtilities::GenerateBackgroundBorder writes.
 */
static const uint8 GBackR = 0x3C;
static const uint8 GBackG = 0x78;
static const uint8 GBackB = 0x96;

/**
 * THREE DIFFERENT SHADOWS, not one, and it is the latch that needs them: BoxShadowCache keys on
 * resolved geometry (BoxShadowCache.cpp:37-63), so three distinct blur/spread combinations are
 * three cache entries and therefore three separate SaveLayerAsTexture refusals. With one shadow
 * "logged once" and "called once" would be indistinguishable.
 *
 * NO TEXT ANYWHERE, and no overflow or transform on any element. Both are load-bearing for
 * section 3, which asserts that nothing the final frame draws is opaque white: glyph geometry and
 * the clip-mask geometry GetClipGeometry builds (ElementBackgroundBorder.cpp:71, plain white by
 * construction) would both put white vertices into a frame that is otherwise free of them.
 *
 * The masked element carries a gradient decorator as its mask so that the refusal has real
 * artwork behind it rather than an empty decorator list.
 */
static const TCHAR* GDocument = TEXT(R"(<rml>
<head><style>
body { display: block; width: 100%; height: 100%; }
div { display: block; width: 120px; height: 40px; margin: 8px; background-color: #3C7896; border: 2px #204058; }
#s1 { box-shadow: 4px 4px 8px #000000FF; }
#s2 { box-shadow: 6px 2px 12px #000000FF; }
#s3 { box-shadow: 2px 6px 16px 3px #000000FF; }
#masked { mask-image: linear-gradient(to right, #FFFFFF, #FFFFFF00); }
</style></head>
<body>
	<div id="s1"/>
	<div id="s2"/>
	<div id="s3"/>
	<div id="masked"/>
	<div id="plain"/>
</body>
</rml>)");

/**
 * ONE published buffer, kept whole so the test can look inside it — the same shape
 * VaCuusUnsizedDrainTest uses, and for the same reason: the production sinks consume a buffer
 * into RHI state on arrival, which under -nullrhi would leave nothing to assert against.
 */
class FCaptureSink final : public IVaCuusFrameSink
{
public:
	virtual void SetPendingBuffer_RenderThread(FRHICommandListImmediate&, TUniquePtr<FVaCuusCommandBuffer> InBuffer) override
	{
		check(IsInRenderingThread());
		Buffers.Add(MoveTemp(InBuffer));
	}

	virtual void ReleaseResources_RenderThread() override
	{
		check(IsInRenderingThread());
		Buffers.Empty();
	}

	/** Written on the render thread, read on the game thread AFTER FlushRenderingCommands(). */
	TArray<TUniquePtr<FVaCuusCommandBuffer>> Buffers;
};

/** One UI frame at a time; the wake event coalesces, so N triggers are not N frames. */
static bool RunFrames(FVaCuusUIThread& UIThread, int32 NumFrames)
{
	for (int32 Index = 0; Index < NumFrames; ++Index)
	{
		const uint64 Before = UIThread.GetFrameCount();
		UIThread.Trigger();
		if (!UIThread.WaitForFrameCount(Before + 1, 5.0))
		{
			return false;
		}
	}

	return true;
}

/**
 * BYTE ORDER, NOT CHANNEL NAMES. The recorder memcpy's Rml::Vertex wholesale
 * (VaCuusRecordingRenderInterface.cpp's CompileGeometry, guarded by the offsetof static_asserts
 * at the top of that file), and Rml::ColourbPremultiplied stores red first. FColor's own member
 * order is platform-dependent (BGRA on little-endian UE), so naming .R here would read the blue
 * byte. Comparing the four bytes in memory order is what the memcpy contract actually promises.
 */
static bool ColorBytesEqual(const FColor& Value, uint8 R, uint8 G, uint8 B, uint8 A)
{
	const uint8* Bytes = reinterpret_cast<const uint8*>(&Value);
	return Bytes[0] == R && Bytes[1] == G && Bytes[2] == B && Bytes[3] == A;
}
}	 // namespace VaCuusLayerCaptureTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusLayerCaptureRefusalTest, "VaCuus.Render.LayerCapture.Refused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusLayerCaptureRefusalTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusLayerCaptureTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no UI thread to drive"));
		return true;
	}

	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	FVaCuusModule& Module = FVaCuusModule::Get();
	FVaCuusUIThread* UIThread = Module.GetOrStartUIThread();
	if (!TestNotNull(TEXT("UI thread"), UIThread))
	{
		return false;
	}

	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	const TSharedRef<FCaptureSink> Sink = MakeShared<FCaptureSink>();
	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();

	TUniquePtr<FVaCuusRmlDocumentHost> OwnedHost = MakeUnique<FVaCuusRmlDocumentHost>(Sink);

	// Kept across the hand-over so the refusal tally can be read at all — the same window and the
	// same argument as VaCuusUnsizedDrainTest's: the UI thread owns the host from AddView until
	// RemoveView or Exit(), neither of which happens before the StopUIThread above, and every
	// counter is atomic and read behind a WaitForFrameCount.
	FVaCuusRmlDocumentHost* Host = OwnedHost.Get();

	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), GViewSize, Status);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GDocument, /*LoadSerial=*/1);

	// ---- 1. Settle. ----
	//
	// The load, the first layout and the one-and-only run of each box-shadow texture callback all
	// genuinely publish. The defect cannot show itself here: a broken build settles too, it just
	// never stops.
	int32 SettleFrames = 0;
	uint64 LastPublished = 0;
	int32 StableFrames = 0;
	while (SettleFrames < 200 && StableFrames < 10)
	{
		if (!TestTrue(TEXT("frames ran"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		++SettleFrames;

		const uint64 Published = Status->FramesPublished.load(std::memory_order_acquire);
		StableFrames = (Published == LastPublished) ? StableFrames + 1 : 0;
		LastPublished = Published;
	}

	if (!TestTrue(TEXT("the view reached a steady state (a churning box-shadow never does)"), StableFrames >= 10))
	{
		AddError(FString::Printf(TEXT("still publishing after %d frames (%llu published, %llu recorded)"), SettleFrames,
			Status->FramesPublished.load(std::memory_order_acquire), Status->FramesRecorded.load(std::memory_order_acquire)));
		return false;
	}

	const uint64 PublishedBefore = Status->FramesPublished.load(std::memory_order_acquire);
	const uint64 RecordedBefore = Status->FramesRecorded.load(std::memory_order_acquire);
	AddInfo(FString::Printf(TEXT("settled after %d frames: %llu published, %llu recorded"), SettleFrames, PublishedBefore,
		RecordedBefore));

	// ---- 2. A HUNDRED IDLE FRAMES. ----
	//
	// Nothing on this document moves, so a correct build publishes none of them. Before the
	// GeometryBoxShadow.cpp hunk every one of these frames re-ran three box-shadow callbacks,
	// each pushing a layer and compiling and releasing a blur filter and geometry — resource
	// traffic, which is one of the idle gate's two wake conditions.
	if (!TestTrue(TEXT("a hundred idle frames ran"), RunFrames(*UIThread, 100)))
	{
		return false;
	}

	const uint64 RecordedAfter = Status->FramesRecorded.load(std::memory_order_acquire);
	const uint64 PublishedAfter = Status->FramesPublished.load(std::memory_order_acquire);

	AddInfo(FString::Printf(TEXT("100 idle frames: %llu published, %llu recorded"), PublishedAfter - PublishedBefore,
		RecordedAfter - RecordedBefore));

	TestTrue(TEXT("the view really did record those hundred frames"), RecordedAfter >= RecordedBefore + 100);
	TestEqual(TEXT("and an unsupported box-shadow published NOT ONE of them (u0q)"), int32(PublishedAfter - PublishedBefore), 0);

	// ---- 3. WHAT THE LAST PUBLISHED FRAME ACTUALLY DRAWS. ----
	//
	// The buffers reach the sink through ENQUEUE_RENDER_COMMAND, so the flush is the happens-before
	// edge for reading them here.
	FlushRenderingCommands();

	if (!TestTrue(TEXT("the sink captured at least one published buffer"), Sink->Buffers.Num() > 0))
	{
		return false;
	}

	// Geometry handles are created in whichever buffer first carried them and drawn by any later
	// one — NewGeometry is a delta, Commands is the whole frame — so resolving a draw means
	// accumulating every buffer's creations first.
	TMap<FVaCuusGeometryHandle, const FVaCuusGeometryData*> AllGeometry;
	for (const TUniquePtr<FVaCuusCommandBuffer>& Buffer : Sink->Buffers)
	{
		for (const TPair<FVaCuusGeometryHandle, FVaCuusGeometryData>& Pair : Buffer->NewGeometry)
		{
			AllGeometry.Add(Pair.Key, &Pair.Value);
		}
	}

	// THE LAST buffer, not all of them, and that is the point: the white quads the box-shadow
	// callback compiles for its own clip masks are real geometry and DO appear in NewGeometry
	// during settle. What must not happen is that the finished frame DRAWS one.
	const FVaCuusCommandBuffer& LastBuffer = *Sink->Buffers.Last();

	int32 BackgroundVertices = 0;
	int32 OpaqueWhiteVertices = 0;
	int32 ResolvedDraws = 0;
	for (const FVaCuusCommand& Command : LastBuffer.Commands)
	{
		if (Command.Type != EVaCuusCommandType::DrawGeometry)
		{
			continue;
		}

		const FVaCuusGeometryData* const* Found = AllGeometry.Find(Command.Geometry);
		if (Found == nullptr)
		{
			continue;
		}
		++ResolvedDraws;

		for (const FVaCuusVertex& Vertex : (*Found)->Vertices)
		{
			if (ColorBytesEqual(Vertex.Color, GBackR, GBackG, GBackB, 0xFF))
			{
				++BackgroundVertices;
			}
			else if (ColorBytesEqual(Vertex.Color, 0xFF, 0xFF, 0xFF, 0xFF))
			{
				++OpaqueWhiteVertices;
			}
		}
	}

	AddInfo(FString::Printf(TEXT("last published frame: %d resolved geometry draws, %d vertices in the element's own "
								 "background colour, %d opaque-white vertices"),
		ResolvedDraws, BackgroundVertices, OpaqueWhiteVertices));

	TestTrue(TEXT("the last frame draws geometry at all"), ResolvedDraws > 0);
	TestTrue(TEXT("a shadowed element renders its OWN background and border colour (u0q)"), BackgroundVertices > 0);
	TestEqual(TEXT("and draws no opaque-white quad -- the box-shadow substitute this bead is named for"),
		OpaqueWhiteVertices, 0);

	// ---- 4. LOUD ONCE, NOT LOUD PER ELEMENT. ----
	//
	// The two halves of the latch, and neither number means anything without the other: Calls
	// proves the refusal path really ran (a test that stopped reaching it would report a perfect
	// "logged once"), Warnings proves it spoke only the first time.
	const FVaCuusUnsupportedTally Tally = Host->GetUnsupportedTally();

	AddInfo(FString::Printf(TEXT("refusals: SaveLayerAsTexture %u calls / %u warnings, SaveLayerAsMaskImage %u calls / %u warnings"),
		Tally.SaveLayerAsTextureCalls, Tally.SaveLayerAsTextureWarnings, Tally.SaveLayerAsMaskImageCalls,
		Tally.SaveLayerAsMaskImageWarnings));

	// Three distinct box-shadow styles are three cache entries and therefore three refusals; the
	// assertion is >= rather than == because a restyle during settle may legitimately add more.
	TestTrue(TEXT("all three distinct box-shadows reached the refusal"), Tally.SaveLayerAsTextureCalls >= 3);
	TestEqual(TEXT("and between them they logged exactly one line (u0q latch)"), int32(Tally.SaveLayerAsTextureWarnings), 1);

	// The mask refusal is reached once per masked element PER FRAME, so a hundred idle frames is
	// a hundred-odd calls — which is exactly why its latch is the one that matters most.
	TestTrue(TEXT("the mask-image refusal was reached on every frame, not once"), Tally.SaveLayerAsMaskImageCalls > 50);
	TestEqual(TEXT("and logged exactly one line across all of them (iuv latch)"), int32(Tally.SaveLayerAsMaskImageWarnings), 1);

	UIThread->EnqueueRemoveView(ViewId);
	RunFrames(*UIThread, 1);

	return true;
}

/**
 * THE OTHER HALF OF VaCuus PATCH #3, and the only thing that isolates it: a shadowed element that
 * RESTYLES must not re-run its box-shadow texture callback.
 *
 * The sibling test above cannot see this. Its document is idle, so once GenerateGeometry declines
 * the shadow path nothing asks for the shadow texture again and the callback would not re-run
 * even with load_failed left unset — measured, by reverting that one hunk and watching the whole
 * test still pass. A restyle is what brings the question back: every dirty of the background
 * re-runs GenerateGeometry, which probes the texture, which reaches
 * CallbackTextureDatabase::EnsureLoaded (TextureDatabase.cpp:46-60) — and EnsureLoaded re-runs the
 * ENTIRE callback (a layer push, a blur compiled and released, geometry made and released) unless
 * a previous run reported failure. Upstream never reports it: the callback returns true whether or
 * not it produced a handle. That is the hunk this test exists for.
 *
 * WHY `image-color` IS THE PROPERTY ANIMATED, and it is the only reason this test is small.
 * Element::OnPropertyChange dirties the background for exactly five things — a border-radius
 * change, background-color, opacity, image-color and box-shadow (Element.cpp:1914-1921) — and
 * BoxShadowGeometryInfo's cache key holds every one of them EXCEPT image-color
 * (GeometryBoxShadow.cpp:81-91). So animating image-color dirties the background on every frame
 * while the shadow resolves to the SAME cache entry throughout, which is precisely the
 * one-entry-many-dirties shape the latch is about. Animating any of the other four would mint a
 * fresh cache entry per frame and the count would climb either way, proving nothing.
 *
 * The observable is the refusal COUNT, not published frames: a restyling document publishes every
 * frame regardless, because ElementBackgroundBorder::GenerateGeometry releases and re-makes its
 * mesh with no equality check (:131-137) — the same fact VaCuus patch #1 rests on. Revert the
 * GeometryBoxShadow.cpp hunk and the assertion below reports one call per dirtied frame instead
 * of one in total.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusLayerCaptureRestyleChurnTest, "VaCuus.Render.LayerCapture.RestyleChurn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusLayerCaptureRestyleChurnTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusLayerCaptureTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no UI thread to drive"));
		return true;
	}

	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	FVaCuusModule& Module = FVaCuusModule::Get();
	FVaCuusUIThread* UIThread = Module.GetOrStartUIThread();
	if (!TestNotNull(TEXT("UI thread"), UIThread))
	{
		return false;
	}

	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	// ONE shadowed element, so the expected call count is exactly one and any re-run is visible as
	// a number greater than one rather than as a ratio.
	static const TCHAR* RestyleDocument = TEXT(R"(<rml>
<head><style>
body { display: block; width: 100%; height: 100%; }
div { display: block; width: 120px; height: 40px; background-color: #3C7896; border: 2px #204058; }
#s1 { box-shadow: 4px 4px 8px #000000FF; animation: 2s infinite linear tint; }
@keyframes tint { from { image-color: #FFFFFFFF; } to { image-color: #FFFFFF00; } }
</style></head>
<body><div id="s1"/></body>
</rml>)");

	const TSharedRef<FCaptureSink> Sink = MakeShared<FCaptureSink>();
	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();

	TUniquePtr<FVaCuusRmlDocumentHost> OwnedHost = MakeUnique<FVaCuusRmlDocumentHost>(Sink);
	FVaCuusRmlDocumentHost* Host = OwnedHost.Get();

	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), GViewSize, Status);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, RestyleDocument, /*LoadSerial=*/1);

	if (!TestTrue(TEXT("the document loaded and laid out"), RunFrames(*UIThread, 10)))
	{
		return false;
	}

	const uint32 CallsAfterLoad = Host->GetUnsupportedTally().SaveLayerAsTextureCalls;

	if (!TestTrue(TEXT("sixty animated frames ran"), RunFrames(*UIThread, 60)))
	{
		return false;
	}

	const FVaCuusUnsupportedTally Tally = Host->GetUnsupportedTally();
	const uint64 Published = Status->FramesPublished.load(std::memory_order_acquire);

	AddInfo(FString::Printf(TEXT("after load: %u SaveLayerAsTexture calls; after 60 animated frames: %u calls, "
								 "%u warnings, %llu frames published"),
		CallsAfterLoad, Tally.SaveLayerAsTextureCalls, Tally.SaveLayerAsTextureWarnings, Published));

	// CONTROL, and the count below is worthless without it: an animation that never ran would
	// keep the call count flat for the most boring possible reason. A restyling document
	// republishes every frame (GenerateGeometry re-makes its mesh unconditionally), so a healthy
	// publish count is the proof that the element really was dirtied over and over.
	TestTrue(TEXT("the animation really did restyle the element every frame"), Published > 30);

	TestTrue(TEXT("the shadow was refused at least once, so the path under test really ran"), Tally.SaveLayerAsTextureCalls >= 1);
	TestEqual(TEXT("and NOT ONCE MORE across sixty restyles -- the callback latched load_failed (u0q)"),
		int32(Tally.SaveLayerAsTextureCalls), int32(CallsAfterLoad));
	TestEqual(TEXT("still exactly one log line"), int32(Tally.SaveLayerAsTextureWarnings), 1);

	UIThread->EnqueueRemoveView(ViewId);
	RunFrames(*UIThread, 1);

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
