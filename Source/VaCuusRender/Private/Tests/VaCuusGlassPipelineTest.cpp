// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusCommandBuffer.h"
#include "VaCuusEngine.h"
#include "VaCuusGlassDistiller.h"
#include "VaCuusRecordingRenderInterface.h"
#include "VaCuusUIShaders.h"

#include "Misc/ScopeExit.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Dictionary.h>
#include <RmlUi/Core/Types.h>
#include <RmlUi/Core/Variant.h>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * M5 Task 3: the glass distiller and its coordinate mapping (spec §2(a)) — what a
 * published buffer's backdrop signature becomes on the Slate element, that the list is
 * replaced WHOLESALE per publish, that sigma resolves across buffers and retires on
 * release, and that the PIE-shaped mapping (nonzero DestRect.Min) is exact.
 *
 * The distiller is deliberately thread-agnostic data-in/data-out (see its header), so
 * these tests drive it directly with recorder-produced buffers — through a real
 * Rml::Context where the property under test is RmlUi's call sequence, and through
 * direct recorder drives where it is the distiller's own bookkeeping (the idle-gate
 * file's stated split, applied one stage downstream).
 */
namespace VaCuusGlassPipelineTest
{
static const FIntPoint GViewSize(800, 600);

TUniquePtr<FVaCuusCommandBuffer> RecordContextFrame(FVaCuusRecordingRenderInterface& Recorder, Rml::Context* Context)
{
	Recorder.BeginFrame(GViewSize);
	Context->Update();
	Context->Render();
	return Recorder.EndFrameAndPublish();
}

/** Records one frame and, when the gate lets it publish, distills it. Returns whether it published. */
bool RecordAndDistill(FVaCuusRecordingRenderInterface& Recorder, Rml::Context* Context, FVaCuusGlassDistiller& Distiller)
{
	if (const TUniquePtr<FVaCuusCommandBuffer> Buffer = RecordContextFrame(Recorder, Context))
	{
		Distiller.Distill(*Buffer);
		return true;
	}
	return false;
}

/**
 * The axis-aligned box the entry's mask actually COVERS, in view pixels: every vertex
 * through the very matrix the glass draw hands its vertex shader, with the divide the GPU
 * would do. Measuring here rather than on the raw vertices is the point -- the vertices
 * are the clip element's own untransformed space, and what has to land in the right place
 * is the composition.
 */
FIntRect ComputeMaskBoundingBox(const FVaCuusGlassEntry& Entry, FIntPoint ViewSize)
{
	// A 1:1 mapping at the origin, so the answer stays in VIEW pixels and the expected
	// boxes below are the CSS numbers a reader can check by hand.
	const FVaCuusGlassMapping Direct = VaCuusMakeGlassMapping(
		FIntRect(0, 0, ViewSize.X, ViewSize.Y), FVector2f::ZeroVector, ViewSize, FIntRect(0, 0, ViewSize.X, ViewSize.Y), ViewSize);
	const FMatrix44f Mask = VaCuusMakeGlassMaskMatrix(Entry, Direct, ViewSize);

	FVector2f Min(TNumericLimits<float>::Max(), TNumericLimits<float>::Max());
	FVector2f Max(TNumericLimits<float>::Lowest(), TNumericLimits<float>::Lowest());
	for (const FVaCuusVertex& Vertex : Entry.MaskGeometry->Vertices)
	{
		const FVector4f Clip = Mask.TransformFVector4(FVector4f(Vertex.Position.X, Vertex.Position.Y, 0.0f, 1.0f));
		const float InvW = (Clip.W != 0.0f) ? 1.0f / Clip.W : 1.0f;
		const FVector2f Pixels(
			(Clip.X * InvW + 1.0f) * 0.5f * float(ViewSize.X), (1.0f - Clip.Y * InvW) * 0.5f * float(ViewSize.Y));
		Min.X = FMath::Min(Min.X, Pixels.X);
		Min.Y = FMath::Min(Min.Y, Pixels.Y);
		Max.X = FMath::Max(Max.X, Pixels.X);
		Max.Y = FMath::Max(Max.Y, Pixels.Y);
	}
	return FIntRect(FMath::RoundToInt(Min.X), FMath::RoundToInt(Min.Y), FMath::RoundToInt(Max.X), FMath::RoundToInt(Max.Y));
}

/** Every edge of Actual within one pixel of the same edge of Expected. */
bool BoxWithinOnePixel(const FIntRect& Actual, const FIntRect& Expected)
{
	return FMath::Abs(Actual.Min.X - Expected.Min.X) <= 1 && FMath::Abs(Actual.Min.Y - Expected.Min.Y) <= 1 &&
		FMath::Abs(Actual.Max.X - Expected.Max.X) <= 1 && FMath::Abs(Actual.Max.Y - Expected.Max.Y) <= 1;
}

/**
 * Outer fully contains Inner.
 *
 * NOT FIntRect::Contains(const FIntRect&): that overload is 5.8-only (IntRect.h:346). On
 * UE 5.6 the only Contains is the POINT one (IntRect.h:334), so the rect argument binds to
 * TIntPoint's implicit single-int constructor and the compile dies with "no viable
 * conversion from 'const FIntRect' to 'IntPointType'". This is the body of 5.8's overload,
 * copied so the assertion means the same thing on every supported engine.
 *
 * It lives here rather than in VaCuusEngineCompat.h on that header's own rule: it wraps the
 * four ranked runtime hotspots and says burying them "under fifty inert pass-throughs would
 * hide the seam's signal". This is one test file's arithmetic, not an engine seam.
 */
bool BoxContainsBox(const FIntRect& Outer, const FIntRect& Inner)
{
	return Inner.Min.X >= Outer.Min.X && Inner.Max.X <= Outer.Max.X && Inner.Min.Y >= Outer.Min.Y &&
		Inner.Max.Y <= Outer.Max.Y;
}

/**
 * THE ONE-PANEL RIG: load a document, settle it, and measure the single glass entry's mask.
 * Two tests below differ only in their CSS, and a copy of this skeleton per case is exactly
 * where a divergence between them would hide.
 */
struct FMeasuredGlassMask
{
	int32 EntryCount = 0;
	bool bMeasured = false;
	FIntRect MaskBox;
	FIntRect DrawRegion;
	bool bTransformOnEntry = false;
	int32 BoundsOnlyMasks = 0;
};

FMeasuredGlassMask MeasureGlassMask(FAutomationTestBase& Test, const char* ContextName, const TCHAR* DocumentSource)
{
	FMeasuredGlassMask Measured;

	FVaCuusEngine& Engine = FVaCuusEngine::Get();
	if (!Test.TestTrue(TEXT("Initialized"), Engine.Initialize()))
	{
		return Measured;
	}
	ON_SCOPE_EXIT
	{
		Engine.Shutdown();
	};

	FVaCuusRecordingRenderInterface Recorder;
	const Rml::String Name(ContextName);
	Rml::Context* Context = Rml::CreateContext(Name, Rml::Vector2i(GViewSize.X, GViewSize.Y), &Recorder);
	if (!Test.TestNotNull(TEXT("Context"), Context))
	{
		return Measured;
	}
	ON_SCOPE_EXIT
	{
		Rml::RemoveContext(Name);
	};

	Rml::ElementDocument* Document =
		Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(DocumentSource)), "vacuus://glass_transform.rml");
	if (!Test.TestNotNull(TEXT("Document"), Document))
	{
		return Measured;
	}
	Document->Show();

	// Settle, same as the removal test: a fresh rounded panel publishes more than once (the
	// clip-geometry settle), and every published buffer is distilled as the element would.
	FVaCuusGlassDistiller Distiller;
	for (int32 Settle = 0; Settle < 4 && RecordAndDistill(Recorder, Context, Distiller); ++Settle)
	{
	}

	Measured.EntryCount = Distiller.GetEntries().Num();
	if (Measured.EntryCount == 1 && Distiller.GetEntries()[0].MaskGeometry.IsValid())
	{
		const FVaCuusGlassEntry& Entry = Distiller.GetEntries()[0];
		Measured.bMeasured = true;
		Measured.MaskBox = ComputeMaskBoundingBox(Entry, GViewSize);
		Measured.DrawRegion = Entry.DrawRegion;
		Measured.bTransformOnEntry = (Entry.MaskTransform != FMatrix44f::Identity);
		Measured.BoundsOnlyMasks = Entry.BoundsOnlyMasks;
	}
	return Measured;
}
} // namespace VaCuusGlassPipelineTest

/**
 * The reference panel end to end: the Task 2 recorded sequence (BackdropSequence's
 * numbers) distilled into exactly one entry carrying the grab scissor as SampleRegion,
 * the border-box scissor as DrawRegion, sigma verbatim, and an owned copy of the
 * clip-mask geometry at the border-box translation.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassDistillSequenceTest, "VaCuus.Render.Glass.DistillBackdropSequence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassDistillSequenceTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusGlassPipelineTest;

	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	FVaCuusEngine& Engine = FVaCuusEngine::Get();
	if (!TestTrue(TEXT("Initialized"), Engine.Initialize()))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Engine.Shutdown();
	};

	FVaCuusRecordingRenderInterface Recorder;
	const Rml::String ContextName("vacuus_glass_distill_test");
	Rml::Context* Context = Rml::CreateContext(ContextName, Rml::Vector2i(GViewSize.X, GViewSize.Y), &Recorder);
	if (!TestNotNull(TEXT("Context"), Context))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Rml::RemoveContext(ContextName);
	};

	// The Task 2 reference panel: border box (40,40)-(240,160), blur(12px) -> sigma 12,
	// ink overflow 3*max(12,1) = 36px -> grab scissor (4,4)-(276,196).
	static const TCHAR* Source =
		TEXT("<rml><head><style>")
		TEXT("body{display:block;width:100%;height:100%;}")
		TEXT("#panel{display:block;position:absolute;left:40px;top:40px;width:200px;height:120px;")
		TEXT("border-radius:16px;background-color:#30405080;backdrop-filter:blur(12px);}")
		TEXT("</style></head><body><div id=\"panel\"/></body></rml>");

	Rml::ElementDocument* Document =
		Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(Source)), "vacuus://glass_distill.rml");
	if (!TestNotNull(TEXT("Document"), Document))
	{
		return false;
	}
	Document->Show();

	FVaCuusGlassDistiller Distiller;
	if (!TestTrue(TEXT("The glass frame publishes and distills"), RecordAndDistill(Recorder, Context, Distiller)))
	{
		return false;
	}

	if (!TestEqual(TEXT("Exactly one glass entry"), Distiller.GetEntries().Num(), 1))
	{
		return false;
	}
	const FVaCuusGlassEntry& Entry = Distiller.GetEntries()[0];

	TestTrue(TEXT("SampleRegion is the grab scissor: border box + 3-sigma ink overflow (4,4)-(276,196)"),
		Entry.SampleRegion == FIntRect(4, 4, 276, 196));
	TestTrue(TEXT("DrawRegion is the border box (40,40)-(240,160)"), Entry.DrawRegion == FIntRect(40, 40, 240, 160));
	TestEqual(TEXT("Sigma is FilterBlur's resolved value verbatim"), Entry.Sigma, 12.0f);
	TestTrue(TEXT("The list stores the buffer's ViewSize (the mapping's denominator)"), Distiller.GetViewSize() == GViewSize);

	if (TestTrue(TEXT("The rounded panel carries its clip-mask geometry"), Entry.MaskGeometry.IsValid()))
	{
		TestTrue(TEXT("...a non-empty owned copy"),
			Entry.MaskGeometry->Vertices.Num() > 0 && Entry.MaskGeometry->Indices.Num() > 0);
		TestTrue(TEXT("...at the border-box translation (40,40)"), Entry.MaskTranslation == FVector2f(40.0f, 40.0f));
	}

	return true;
}

/**
 * A glass panel under a transformed ancestor — the usual game-HUD shape of a panel root
 * scaled by transform: scale() around a screen corner. Before the fix the mask carried
 * only the panel's UNSCALED border box, because Distill() never tracked SetTransform, so
 * the mask covered only the part of the scaled panel nearest the transform origin. After
 * the fix the mask vertices land at the scaled box, same as SampleRegion/DrawRegion
 * always did.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassDistillTransformedMaskTest, "VaCuus.Render.Glass.DistillTransformedMask",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassDistillTransformedMaskTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusGlassPipelineTest;

	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	FVaCuusEngine& Engine = FVaCuusEngine::Get();
	if (!TestTrue(TEXT("Initialized"), Engine.Initialize()))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Engine.Shutdown();
	};

	FVaCuusRecordingRenderInterface Recorder;
	const Rml::String ContextName("vacuus_glass_transform_test");
	Rml::Context* Context = Rml::CreateContext(ContextName, Rml::Vector2i(GViewSize.X, GViewSize.Y), &Recorder);
	if (!TestNotNull(TEXT("Context"), Context))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Rml::RemoveContext(ContextName);
	};

	// #panel is the Task 2 reference panel again: border box (40,40)-(240,160),
	// border-radius:12px, blur(12px). #parent sits at the document origin and scales it
	// 1.5x around its own top-left corner (the HUD's own transform-origin choice), so the
	// scale's centre IS the document origin and every corner just multiplies by 1.5:
	//   scaled box = (40*1.5, 40*1.5) .. ((40+200)*1.5, (40+120)*1.5) = (60,60)-(360,240)
	static const TCHAR* Source =
		TEXT("<rml><head><style>")
		TEXT("body{display:block;width:100%;height:100%;}")
		TEXT("#parent{display:block;position:absolute;left:0px;top:0px;width:400px;height:300px;}")
		TEXT("#parent.scaled{transform:scale(1.5);transform-origin:left top;}")
		TEXT("#panel{display:block;position:absolute;left:40px;top:40px;width:200px;height:120px;")
		TEXT("border-radius:12px;background-color:#30405080;backdrop-filter:blur(12px);}")
		TEXT("</style></head><body><div id=\"parent\" class=\"scaled\"><div id=\"panel\"/></div></body></rml>");

	Rml::ElementDocument* Document =
		Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(Source)), "vacuus://glass_transform.rml");
	if (!TestNotNull(TEXT("Document"), Document))
	{
		return false;
	}
	Document->Show();

	Rml::Element* Parent = Document->GetElementById("parent");
	if (!TestNotNull(TEXT("Parent element"), Parent))
	{
		return false;
	}

	// Settle, same as the removal test: a fresh rounded panel publishes more than once
	// (the clip-geometry settle), and every published buffer is distilled as the element
	// would distill it.
	FVaCuusGlassDistiller Distiller;
	for (int32 Settle = 0; Settle < 4 && RecordAndDistill(Recorder, Context, Distiller); ++Settle)
	{
	}

	if (TestEqual(TEXT("Exactly one glass entry under the transformed ancestor"), Distiller.GetEntries().Num(), 1))
	{
		const FVaCuusGlassEntry& Entry = Distiller.GetEntries()[0];
		if (TestTrue(TEXT("The transformed panel still carries its clip-mask geometry"), Entry.MaskGeometry.IsValid()))
		{
			const FIntRect ExpectedBox(60, 60, 360, 240);
			const FIntRect ActualBox = ComputeMaskBoundingBox(Entry, GViewSize);
			TestTrue(FString::Printf(TEXT("Mask vertices land at the SCALED border box %s (got %s)"), *ExpectedBox.ToString(), *ActualBox.ToString()),
				BoxWithinOnePixel(ActualBox, ExpectedBox));
			TestTrue(TEXT("DrawRegion contains the scaled mask box"), BoxContainsBox(Entry.DrawRegion, ActualBox));

			// NOT BAKED: the scale lives in the entry's matrix, and the geometry is still
			// the cross-buffer map's shared untransformed copy. That is what lets the
			// element reuse this entry's vertex buffers across publishes.
			TestTrue(TEXT("The transform rides on the entry, not on the vertices"), Entry.MaskTransform != FMatrix44f::Identity);
		}
	}

	// THE REGRESSION GUARD: the same panel with #parent's transform removed must still
	// give the UNSCALED box — the identity path (shared ref + MaskTranslation, today's
	// behaviour) stays untouched by this fix.
	Parent->SetClass("scaled", false);
	for (int32 Settle = 0; Settle < 4 && RecordAndDistill(Recorder, Context, Distiller); ++Settle)
	{
	}

	if (TestEqual(TEXT("Exactly one glass entry once the ancestor transform is gone"), Distiller.GetEntries().Num(), 1))
	{
		const FVaCuusGlassEntry& Entry = Distiller.GetEntries()[0];
		if (TestTrue(TEXT("The untransformed panel still carries its clip-mask geometry"), Entry.MaskGeometry.IsValid()))
		{
			const FIntRect ExpectedBox(40, 40, 240, 160);
			const FIntRect ActualBox = ComputeMaskBoundingBox(Entry, GViewSize);
			TestTrue(FString::Printf(TEXT("Mask vertices land at the UNSCALED border box %s (got %s)"), *ExpectedBox.ToString(), *ActualBox.ToString()),
				BoxWithinOnePixel(ActualBox, ExpectedBox));
		}
	}

	return true;
}


/**
 * ROTATION, not just scale. The fix is not scale-specific and must not be tested as if it
 * were: RmlUi pushes a clip mask for the glass element itself whenever that element is
 * transformed, because has_clipping_content is true for the forced self-clip
 * (ThirdParty/RmlUi/Source/Core/ElementUtilities.cpp:148-150). A rotated panel is where the
 * two shapes visibly part company -- the mask is the turned rectangle, DrawRegion is only
 * its axis-aligned hull.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassDistillRotatedMaskTest, "VaCuus.Render.Glass.DistillRotatedMask",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassDistillRotatedMaskTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusGlassPipelineTest;

	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	// #panel: border box (100,100)-(300,200), 200x100, turned a quarter turn about its own
	// centre (the CSS default transform-origin, 50% 50%, i.e. (200,150)). A quarter turn
	// swaps the extents about that centre: 200 wide by 100 tall becomes 100 by 200, so the
	// covered box is (150,50)-(250,250).
	static const TCHAR* Source =
		TEXT("<rml><head><style>")
		TEXT("body{display:block;width:100%;height:100%;}")
		TEXT("#panel{display:block;position:absolute;left:100px;top:100px;width:200px;height:100px;")
		TEXT("border-radius:12px;background-color:#30405080;backdrop-filter:blur(12px);")
		TEXT("transform:rotate(90deg);}")
		TEXT("</style></head><body><div id=\"panel\"/></body></rml>");

	const FMeasuredGlassMask Measured = MeasureGlassMask(*this, "vacuus_glass_rotate_test", Source);
	if (!TestEqual(TEXT("Exactly one glass entry for the rotated panel"), Measured.EntryCount, 1) ||
		!TestTrue(TEXT("The rotated panel carries its clip-mask geometry"), Measured.bMeasured))
	{
		return false;
	}

	const FIntRect ExpectedBox(150, 50, 250, 250);
	TestTrue(FString::Printf(TEXT("Mask vertices land at the ROTATED border box %s (got %s)"), *ExpectedBox.ToString(),
				 *Measured.MaskBox.ToString()),
		BoxWithinOnePixel(Measured.MaskBox, ExpectedBox));
	TestTrue(TEXT("The rotation rides on the entry's matrix"), Measured.bTransformOnEntry);
	TestTrue(TEXT("DrawRegion contains the rotated mask box"), BoxContainsBox(Measured.DrawRegion, Measured.MaskBox));

	return true;
}

/**
 * THE TRANSFORM ON THE PANEL ITSELF, not on an ancestor. Same code path, different origin
 * of the matrix: ApplyClipMask uses the clip element's accumulated transform either way, so
 * a panel that carries its own transform must land exactly like one that inherits it. Worth
 * its own case because the accumulation in Element::UpdateTransformState is what makes the
 * ancestor case work, and this one does not depend on it at all.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassDistillSelfTransformedMaskTest, "VaCuus.Render.Glass.DistillSelfTransformedMask",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassDistillSelfTransformedMaskTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusGlassPipelineTest;

	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	// The reference panel again -- border box (40,40)-(240,160) -- scaled 1.5x about its OWN
	// top-left corner, so that corner is fixed and the far one goes to
	// (40 + 200*1.5, 40 + 120*1.5) = (340,220).
	static const TCHAR* Source =
		TEXT("<rml><head><style>")
		TEXT("body{display:block;width:100%;height:100%;}")
		TEXT("#panel{display:block;position:absolute;left:40px;top:40px;width:200px;height:120px;")
		TEXT("border-radius:12px;background-color:#30405080;backdrop-filter:blur(12px);")
		TEXT("transform:scale(1.5);transform-origin:left top;}")
		TEXT("</style></head><body><div id=\"panel\"/></body></rml>");

	const FMeasuredGlassMask Measured = MeasureGlassMask(*this, "vacuus_glass_self_transform_test", Source);
	if (!TestEqual(TEXT("Exactly one glass entry for the self-transformed panel"), Measured.EntryCount, 1) ||
		!TestTrue(TEXT("The self-transformed panel carries its clip-mask geometry"), Measured.bMeasured))
	{
		return false;
	}

	const FIntRect ExpectedBox(40, 40, 340, 220);
	TestTrue(FString::Printf(TEXT("Mask vertices land at the panel's OWN scaled box %s (got %s)"), *ExpectedBox.ToString(),
				 *Measured.MaskBox.ToString()),
		BoxWithinOnePixel(Measured.MaskBox, ExpectedBox));
	TestTrue(TEXT("The panel's own transform rides on the entry's matrix"), Measured.bTransformOnEntry);
	TestTrue(TEXT("DrawRegion contains the self-scaled mask box"), BoxContainsBox(Measured.DrawRegion, Measured.MaskBox));

	return true;
}

/**
 * A TRANSFORM MUST NOT CHANGE WHAT IS SUPPORTED. An ancestor that clips its overflow reaches
 * a glass entry through the scissor, and DrawRegion carries it -- until that ancestor is
 * transformed, at which point GetClippingRegion puts NOTHING in the scissor and expresses the
 * clip only as an Intersect mask (ThirdParty/RmlUi/Source/Core/ElementUtilities.cpp:162-178).
 * The fold below is what keeps the two cases equivalent, and BoundsOnlyMasks is what says out
 * loud that the ancestor's shape was reduced to its box on the way.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassTransformedAncestorClipTest, "VaCuus.Render.Glass.TransformedAncestorClip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassTransformedAncestorClipTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusGlassPipelineTest;

	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	// #outer clips its overflow, sits at (50,50) 200x150, and is scaled 1.5x about the
	// document origin -> its clip box lands at (75,75)-(375,300). #panel inside it is 400x400,
	// far bigger than the container, so the container really does have content to clip --
	// which is what makes RmlUi push the Intersect mask at all (has_clipping_content).
	static const TCHAR* Source =
		TEXT("<rml><head><style>")
		TEXT("body{display:block;width:100%;height:100%;}")
		TEXT("#outer{display:block;position:absolute;left:50px;top:50px;width:200px;height:150px;")
		TEXT("overflow:hidden;transform:scale(1.5);transform-origin:left top;}")
		TEXT("#panel{display:block;width:400px;height:400px;")
		TEXT("border-radius:12px;background-color:#30405080;backdrop-filter:blur(12px);}")
		TEXT("</style></head><body><div id=\"outer\"><div id=\"panel\"/></div></body></rml>");

	const FMeasuredGlassMask Measured = MeasureGlassMask(*this, "vacuus_glass_ancestor_clip_test", Source);
	if (!TestEqual(TEXT("Exactly one glass entry under the clipping ancestor"), Measured.EntryCount, 1) ||
		!TestTrue(TEXT("The panel carries its clip-mask geometry"), Measured.bMeasured))
	{
		return false;
	}

	TestEqual(TEXT("The transformed ancestor's clip arrived as one bounds-only mask"), Measured.BoundsOnlyMasks, 1);

	// #outer's border box is (50,50)-(250,200); scaled 1.5 about its own top-left corner
	// that is (50,50)-(350,275), and DrawRegion is the intersection of it with the panel's
	// own box, which starts at the same corner. Without the fold DrawRegion would be the
	// PANEL's scaled box instead, (50,50)-(650,650) clipped to the view.
	const FIntRect ExpectedRegion(50, 50, 350, 275);
	TestTrue(FString::Printf(TEXT("DrawRegion is the ancestor's scaled clip box %s (got %s)"), *ExpectedRegion.ToString(),
				 *Measured.DrawRegion.ToString()),
		BoxWithinOnePixel(Measured.DrawRegion, ExpectedRegion));

	return true;
}

/**
 * THE REMOVAL TEST (spec §7, the list-replacement invariant made observable): show glass
 * -> the list has one entry; remove the panel (display:none via class swap) -> the next
 * published buffer distills to an EMPTY list; same for document unload.
 *
 * Restore-the-bug: give Distill() an early-out on glass-free buffers (return before the
 * Entries.Reset() when the buffer records no CompositeLayers) — "the list is empty after
 * removal" fails with the stale entry still in the list, observed via the list-size
 * observable. Verified by doing exactly that; both outcomes in the Task 3 report.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassRemovalTest, "VaCuus.Render.Glass.Removal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassRemovalTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusGlassPipelineTest;

	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	FVaCuusEngine& Engine = FVaCuusEngine::Get();
	if (!TestTrue(TEXT("Initialized"), Engine.Initialize()))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Engine.Shutdown();
	};

	FVaCuusRecordingRenderInterface Recorder;
	const Rml::String ContextName("vacuus_glass_removal_test");
	Rml::Context* Context = Rml::CreateContext(ContextName, Rml::Vector2i(GViewSize.X, GViewSize.Y), &Recorder);
	if (!TestNotNull(TEXT("Context"), Context))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Rml::RemoveContext(ContextName);
	};

	static const TCHAR* Source =
		TEXT("<rml><head><style>")
		TEXT("body{display:block;width:100%;height:100%;}")
		TEXT("#panel{display:block;position:absolute;left:40px;top:40px;width:200px;height:120px;")
		TEXT("border-radius:16px;background-color:#30405080;}")
		// #panel.hidden, not bare .hidden: the #panel rule's display:block is ID-specific
		// (specificity 1-0-0) and would beat a lone class selector — the panel would
		// never hide and the "removal" frame would be withheld as genuinely unchanged.
		// Caught by exactly that happening on the first run.
		TEXT(".glass{backdrop-filter:blur(12px);}")
		TEXT("#panel.hidden{display:none;}")
		TEXT("</style></head><body><div id=\"panel\" class=\"glass\"/></body></rml>");

	Rml::ElementDocument* Document =
		Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(Source)), "vacuus://glass_removal.rml");
	if (!TestNotNull(TEXT("Document"), Document))
	{
		return false;
	}
	Document->Show();

	Rml::Element* Panel = Document->GetElementById("panel");
	if (!TestNotNull(TEXT("Panel element"), Panel))
	{
		return false;
	}

	// Settle (a fresh rounded panel publishes exactly twice — the clip-geometry settle,
	// VaCuus.Render.Glass.SigmaChangePublishes) with every published buffer distilled,
	// as the element would.
	FVaCuusGlassDistiller Distiller;
	for (int32 Settle = 0; Settle < 4 && RecordAndDistill(Recorder, Context, Distiller); ++Settle)
	{
	}
	if (!TestEqual(TEXT("The shown glass panel distills to one entry"), Distiller.GetEntries().Num(), 1))
	{
		return false;
	}

	// THE REMOVAL: display:none via class swap. The frame's command list changes, so the
	// gate publishes it — and that glass-free buffer must WHOLESALE-replace the list.
	Panel->SetClassNames("glass hidden");
	if (!TestTrue(TEXT("The removal frame publishes"), RecordAndDistill(Recorder, Context, Distiller)))
	{
		return false;
	}
	TestEqual(TEXT("After display:none the glass list is EMPTY (wholesale replacement)"), Distiller.GetEntries().Num(), 0);

	// Re-show, so the unload leg below starts from a live entry again — this also
	// exercises the mask geometry being recompiled after its release (fresh handle,
	// fresh NewGeometry copy).
	Panel->SetClassNames("glass");
	for (int32 Settle = 0; Settle < 4 && RecordAndDistill(Recorder, Context, Distiller); ++Settle)
	{
	}
	if (!TestEqual(TEXT("Re-shown glass distills to one entry again"), Distiller.GetEntries().Num(), 1))
	{
		return false;
	}

	// THE UNLOAD: closing the document releases its resources; the next recorded frame
	// carries that traffic, publishes, and distills to nothing.
	Document->Close();
	if (!TestTrue(TEXT("The unload frame publishes"), RecordAndDistill(Recorder, Context, Distiller)))
	{
		return false;
	}
	TestEqual(TEXT("After document unload the glass list is EMPTY"), Distiller.GetEntries().Num(), 0);

	return true;
}

/**
 * Cross-buffer sigma resolution and retirement, plus the wholesale invariant across
 * publishes and the square (no-mask) entry shape — direct recorder drive, because the
 * property is the distiller's own bookkeeping and pre-minted handles isolate it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassCrossBufferSigmaTest, "VaCuus.Render.Glass.CrossBufferSigma",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassCrossBufferSigmaTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusGlassPipelineTest;

	FVaCuusRecordingRenderInterface Recorder;
	FVaCuusGlassDistiller Distiller;

	const auto RecordGlassComposite = [&Recorder](Rml::CompiledFilterHandle Filter)
	{
		const Rml::LayerHandle Layer = Recorder.PushLayer();
		Recorder.CompositeLayers(Rml::LayerHandle(0), Layer, Rml::BlendMode::Blend,
			Rml::Span<const Rml::CompiledFilterHandle>(&Filter, 1));
		Recorder.CompositeLayers(Layer, Rml::LayerHandle(0), Rml::BlendMode::Blend, {});
		Recorder.PopLayer();
	};

	// Frame 1: compile + composite in one buffer, no scissor -> the scissorless default.
	Recorder.BeginFrame(GViewSize);
	const Rml::CompiledFilterHandle Blur12 =
		Recorder.CompileFilter("blur", Rml::Dictionary{{"sigma", Rml::Variant(12.0f)}});
	TestTrue(TEXT("Blur compiled"), Blur12 != 0);
	RecordGlassComposite(Blur12);
	{
		const TUniquePtr<FVaCuusCommandBuffer> Buffer = Recorder.EndFrameAndPublish();
		if (!TestNotNull(TEXT("Frame 1 publishes"), Buffer.Get()))
		{
			return false;
		}
		Distiller.Distill(*Buffer);
	}
	if (!TestEqual(TEXT("Frame 1: one entry"), Distiller.GetEntries().Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("...sigma 12 (same-buffer resolution)"), Distiller.GetEntries()[0].Sigma, 12.0f);
	TestTrue(TEXT("...scissorless grab samples the full view"),
		Distiller.GetEntries()[0].SampleRegion == FIntRect(0, 0, GViewSize.X, GViewSize.Y));
	TestFalse(TEXT("...no clip mask means a square entry"), Distiller.GetEntries()[0].MaskGeometry.IsValid());

	// Frame 2: the SAME handle under a scissor — no filter traffic in this buffer, so the
	// cross-buffer map is the only place sigma can come from.
	Recorder.BeginFrame(GViewSize);
	Recorder.SetScissorRegion(Rml::Rectanglei::FromCorners({10, 10}, {200, 200}));
	RecordGlassComposite(Blur12);
	{
		const TUniquePtr<FVaCuusCommandBuffer> Buffer = Recorder.EndFrameAndPublish();
		if (!TestNotNull(TEXT("Frame 2 publishes (the scissor changed)"), Buffer.Get()))
		{
			return false;
		}
		TestEqual(TEXT("Frame 2 carries no filter traffic"), Buffer->NewFilters.Num(), 0);
		Distiller.Distill(*Buffer);
	}
	if (!TestEqual(TEXT("Frame 2: still exactly one entry (wholesale replacement, not accumulation)"),
			Distiller.GetEntries().Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("...sigma 12 resolved CROSS-BUFFER"), Distiller.GetEntries()[0].Sigma, 12.0f);
	TestTrue(TEXT("...at the new frame's scissor"), Distiller.GetEntries()[0].SampleRegion == FIntRect(10, 10, 200, 200));

	// Frame 3: release + reference in the SAME buffer — the parse must still resolve
	// (retirement is deferred past the parse, the replayer's own release rule).
	Recorder.BeginFrame(GViewSize);
	Recorder.ReleaseFilter(Blur12);
	Recorder.SetScissorRegion(Rml::Rectanglei::FromCorners({20, 20}, {180, 180}));
	RecordGlassComposite(Blur12);
	{
		const TUniquePtr<FVaCuusCommandBuffer> Buffer = Recorder.EndFrameAndPublish();
		if (!TestNotNull(TEXT("Frame 3 publishes (release traffic)"), Buffer.Get()))
		{
			return false;
		}
		Distiller.Distill(*Buffer);
	}
	TestEqual(TEXT("Frame 3: same-buffer release still resolves for this buffer's composite"),
		Distiller.GetEntries().Num(), 1);

	// Frame 4: the handle is retired; a composite that still names it resolves nothing,
	// produces no entry, and says so once.
	AddExpectedMessagePlain(TEXT("no recorded sigma"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Recorder.BeginFrame(GViewSize);
	Recorder.SetScissorRegion(Rml::Rectanglei::FromCorners({30, 30}, {160, 160}));
	RecordGlassComposite(Blur12);
	{
		const TUniquePtr<FVaCuusCommandBuffer> Buffer = Recorder.EndFrameAndPublish();
		if (!TestNotNull(TEXT("Frame 4 publishes (the scissor changed)"), Buffer.Get()))
		{
			return false;
		}
		Distiller.Distill(*Buffer);
	}
	TestEqual(TEXT("Frame 4: a retired sigma produces NO glass entry"), Distiller.GetEntries().Num(), 0);

	return true;
}

/**
 * A filterless layer round-trip (the refused-non-blur shape: the sequence runs, the
 * lists arrive empty — CompiledFilterShader.cpp:6-12) produces no glass entry.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassNoBlurNoEntryTest, "VaCuus.Render.Glass.NoBlurNoEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassNoBlurNoEntryTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusGlassPipelineTest;

	FVaCuusRecordingRenderInterface Recorder;
	FVaCuusGlassDistiller Distiller;

	Recorder.BeginFrame(GViewSize);
	const Rml::LayerHandle Layer = Recorder.PushLayer();
	Recorder.CompositeLayers(Rml::LayerHandle(0), Layer, Rml::BlendMode::Blend, {});
	Recorder.CompositeLayers(Layer, Rml::LayerHandle(0), Rml::BlendMode::Blend, {});
	Recorder.PopLayer();

	const TUniquePtr<FVaCuusCommandBuffer> Buffer = Recorder.EndFrameAndPublish();
	if (!TestNotNull(TEXT("The frame publishes"), Buffer.Get()))
	{
		return false;
	}
	Distiller.Distill(*Buffer);

	TestEqual(TEXT("A composite with no blur produces no glass entry"), Distiller.GetEntries().Num(), 0);
	return true;
}

/**
 * THE MASK DRAW MATRIX (the composition the glass draw hands the vertex shader): the
 * clip element's own transform has to apply AFTER the mask's border-box translation and
 * BEFORE the DestRect mapping, or a transformed panel's mask lands somewhere its draw
 * region is not. Pure math, no engine, no document — the same shape as the mapping test
 * below, and the same reason: this is what the element applies every engine frame.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassMaskMatrixTest, "VaCuus.Render.Glass.MaskMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassMaskMatrixTest::RunTest(const FString& Parameters)
{
	const FIntPoint OutputExtent(1920, 1080);

	// Where a mask vertex LANDS, in output pixels: run it through the matrix the shader
	// gets, do the divide the GPU would do, then undo MakePixelToClipMatrix. Undoing the
	// ortho by hand rather than reusing its inverse is deliberate — an inverse taken from
	// the same function could not catch that function being wrong.
	auto ToOutputPixels = [&OutputExtent](const FMatrix44f& Matrix, const FVector2f& MaskVertex)
	{
		const FVector4f Clip = Matrix.TransformFVector4(FVector4f(MaskVertex.X, MaskVertex.Y, 0.0f, 1.0f));
		const float InvW = (Clip.W != 0.0f) ? 1.0f / Clip.W : 1.0f;
		return FVector2f(
			(Clip.X * InvW + 1.0f) * 0.5f * float(OutputExtent.X), (1.0f - Clip.Y * InvW) * 0.5f * float(OutputExtent.Y));
	};

	// A plain 2D scale about the document origin, built by hand: the entry's transform
	// arrives from the recorder as a raw FMatrix44f, so the test should too.
	auto MakeScale = [](float Scale)
	{
		FMatrix44f M = FMatrix44f::Identity;
		M.M[0][0] = Scale;
		M.M[1][1] = Scale;
		return M;
	};

	// 1:1 at the origin, so this case isolates the mask matrix from the DestRect mapping.
	const FVaCuusGlassMapping Direct = VaCuusMakeGlassMapping(
		FIntRect(0, 0, 1920, 1080), FVector2f::ZeroVector, FIntPoint(1920, 1080), FIntRect(0, 0, 1920, 1080), OutputExtent);

	{
		// No transform: translation alone moves the vertex, exactly as before this existed.
		FVaCuusGlassEntry Entry;
		Entry.MaskTranslation = FVector2f(40.0f, 40.0f);
		const FVector2f Landed = ToOutputPixels(VaCuusMakeGlassMaskMatrix(Entry, Direct, OutputExtent), FVector2f(200.0f, 120.0f));
		TestTrue(FString::Printf(TEXT("Untransformed: vertex + translation (got %s)"), *Landed.ToString()),
			Landed.Equals(FVector2f(240.0f, 160.0f), 0.05f));
	}

	{
		// scale(1.5) about the document origin: (200+40, 120+40) * 1.5 = (360, 240). If the
		// transform were applied BEFORE the translation the answer would be (340, 220).
		FVaCuusGlassEntry Entry;
		Entry.MaskTranslation = FVector2f(40.0f, 40.0f);
		Entry.MaskTransform = MakeScale(1.5f);
		const FVector2f Landed = ToOutputPixels(VaCuusMakeGlassMaskMatrix(Entry, Direct, OutputExtent), FVector2f(200.0f, 120.0f));
		TestTrue(FString::Printf(TEXT("Scaled: the transform applies AFTER the translation (got %s)"), *Landed.ToString()),
			Landed.Equals(FVector2f(360.0f, 240.0f), 0.05f));
	}

	{
		// THE ORDER GUARD, and the reason this test is not just the one above: a PIE-shaped
		// mapping at half scale with a nonzero origin. Transform first, mapping second ->
		// (360,240)*0.5 + (100,50) = (280,170). Mapping first would give (240,160)*0.5
		// +(100,50) = (220,130), then scaled about the wrong origin — a different answer,
		// which is what makes the two orders distinguishable here and not in the case above.
		const FVaCuusGlassMapping Pie = VaCuusMakeGlassMapping(
			FIntRect(100, 50, 1060, 590), FVector2f::ZeroVector, FIntPoint(1920, 1080), FIntRect(0, 0, 1920, 1080), OutputExtent);
		FVaCuusGlassEntry Entry;
		Entry.MaskTranslation = FVector2f(40.0f, 40.0f);
		Entry.MaskTransform = MakeScale(1.5f);
		const FVector2f Landed = ToOutputPixels(VaCuusMakeGlassMaskMatrix(Entry, Pie, OutputExtent), FVector2f(200.0f, 120.0f));
		TestTrue(FString::Printf(TEXT("PIE-shaped: mask transform first, DestRect mapping second (got %s)"), *Landed.ToString()),
			Landed.Equals(FVector2f(280.0f, 170.0f), 0.05f));
	}

	return true;
}

/**
 * THE BUFFER-REUSE RULE (the reason the clip transform is carried as a matrix and not baked
 * into the vertices). Every published buffer replaces the glass list, so without a rule the
 * element re-uploads every panel's mask on every publish — including publishes nothing about
 * that panel caused. Pure data in, yes/no out, so it is a unit test and not an RHI one.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassDrawReuseTest, "VaCuus.Render.Glass.DrawBufferReuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassDrawReuseTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<const FVaCuusGeometryData> RoundedShape = MakeShared<const FVaCuusGeometryData>();
	const TSharedPtr<const FVaCuusGeometryData> OtherShape = MakeShared<const FVaCuusGeometryData>();

	{
		// THE ONE THAT PAYS FOR THE WHOLE DESIGN: a panel being animated by `transform`
		// changes its matrix and its regions every single frame, and re-uploads NOTHING,
		// because the transform never reaches the vertices.
		FVaCuusGlassEntry Moved;
		Moved.MaskGeometry = RoundedShape;
		Moved.DrawRegion = FIntRect(60, 60, 360, 240);
		Moved.MaskTranslation = FVector2f(40.0f, 40.0f);
		Moved.MaskTransform.M[0][0] = 1.5f;
		Moved.MaskTransform.M[1][1] = 1.5f;
		TestTrue(TEXT("A rounded panel keeps its buffers when only its transform and regions moved"),
			VaCuusGlassDrawMatchesEntry(RoundedShape, FIntRect(), Moved));
	}

	{
		// A relayout that recompiles the clip shape is a different payload, and must upload.
		FVaCuusGlassEntry Relaid;
		Relaid.MaskGeometry = OtherShape;
		TestFalse(TEXT("A recompiled clip shape rebuilds"), VaCuusGlassDrawMatchesEntry(RoundedShape, FIntRect(), Relaid));
	}

	{
		// border-radius dropped: the entry turns square and its draw becomes a generated
		// quad, which the old mask buffers cannot serve.
		FVaCuusGlassEntry Squared;
		Squared.DrawRegion = FIntRect(60, 60, 360, 240);
		TestFalse(TEXT("A rounded panel that turns square rebuilds"), VaCuusGlassDrawMatchesEntry(RoundedShape, FIntRect(), Squared));

		// And the same swap the other way: a square panel that gains a border-radius.
		FVaCuusGlassEntry Rounded;
		Rounded.MaskGeometry = RoundedShape;
		Rounded.DrawRegion = FIntRect(60, 60, 360, 240);
		TestFalse(TEXT("...and so does a square one that turns rounded"),
			VaCuusGlassDrawMatchesEntry(nullptr, FIntRect(60, 60, 360, 240), Rounded));
	}

	{
		// The square case keys on its rect, because that rect IS its geometry.
		FVaCuusGlassEntry Square;
		Square.DrawRegion = FIntRect(10, 10, 110, 60);
		TestTrue(TEXT("A square panel at the same rect keeps its quad"),
			VaCuusGlassDrawMatchesEntry(nullptr, FIntRect(10, 10, 110, 60), Square));
		TestFalse(TEXT("A square panel that moved rebuilds its quad"),
			VaCuusGlassDrawMatchesEntry(nullptr, FIntRect(10, 10, 120, 60), Square));
	}

	return true;
}

/**
 * THE PIE-SHAPED MAPPING (spec §2(a), §12.1): view-space data drawn at window
 * coordinates works fullscreen and breaks in PIE, where DestRect.Min != 0. Pure math on
 * the mapping the element applies every engine frame — offset = DestRect.Min +
 * ElementsOffset, scale = DestRect.Size / ViewSize on regions AND sigma, SceneViewRect
 * clamp.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassMappingTest, "VaCuus.Render.Glass.MappingPIEShaped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassMappingTest::RunTest(const FString& Parameters)
{
	// PIE-shaped: the view lives at (320,180) inside a 1920x1080 elements texture, with a
	// nonzero elements offset on top, at 1:1 scale.
	{
		const FVaCuusGlassMapping Mapping = VaCuusMakeGlassMapping(
			/*DestRect=*/FIntRect(320, 180, 1280, 720), /*ElementsOffset=*/FVector2f(8.0f, 4.0f),
			/*ViewSize=*/FIntPoint(960, 540), /*SceneViewRect=*/FIntRect(320, 180, 1280, 720),
			/*OutputExtent=*/FIntPoint(1920, 1080));

		TestTrue(TEXT("Scale is 1:1 when DestRect.Size == ViewSize"), Mapping.Scale == FVector2f(1.0f, 1.0f));
		TestTrue(TEXT("Offset is DestRect.Min + ElementsOffset"), Mapping.Offset == FVector2f(328.0f, 184.0f));
		TestTrue(TEXT("A view-space rect lands at the offset window position"),
			Mapping.MapRect(FIntRect(4, 4, 276, 196)) == FIntRect(332, 188, 604, 380));
		// (-50,-50) maps to (278,134) — outside the scene view — so the clamp bites at
		// SceneViewRect.Min (320,180), NOT at the mapping offset (328,184).
		TestTrue(TEXT("A rect poking off the view clamps to SceneViewRect (the PIE clamp)"),
			Mapping.MapRect(FIntRect(-50, -50, 100, 100)) == FIntRect(320, 180, 428, 284));
		TestTrue(TEXT("Sigma is unscaled at 1:1"), Mapping.MapSigma(12.0f) == FVector2f(12.0f, 12.0f));
	}

	// Scaled: the composite stretches a half-size view — regions AND sigma scale with it.
	{
		const FVaCuusGlassMapping Mapping = VaCuusMakeGlassMapping(
			FIntRect(0, 0, 480, 270), FVector2f::ZeroVector, FIntPoint(960, 540), FIntRect(0, 0, 480, 270),
			FIntPoint(1920, 1080));

		TestTrue(TEXT("Half-size DestRect halves the scale"), Mapping.Scale == FVector2f(0.5f, 0.5f));
		TestTrue(TEXT("Regions scale through"), Mapping.MapRect(FIntRect(40, 40, 240, 160)) == FIntRect(20, 20, 120, 80));
		TestTrue(TEXT("Sigma scales with the same factor"), Mapping.MapSigma(12.0f) == FVector2f(6.0f, 6.0f));
	}

	// Degenerate SceneViewRect: the clamp falls back to the output extent, never wider.
	{
		const FVaCuusGlassMapping Mapping = VaCuusMakeGlassMapping(
			FIntRect(0, 0, 960, 540), FVector2f::ZeroVector, FIntPoint(960, 540), FIntRect(0, 0, 0, 0),
			FIntPoint(960, 540));
		TestTrue(TEXT("An empty SceneViewRect clamps to the output extent"),
			Mapping.MapRect(FIntRect(-10, -10, 2000, 2000)) == FIntRect(0, 0, 960, 540));
	}

	return true;
}

/**
 * THE KERNEL KEEPS ITS LIGHT: the blur's weights are not renormalised, so whatever they
 * sum to scales the glass once per separable pass (squared overall). The sum is taken the way VaCuusBlur.usf:58-68
 * reads the array -- slot 0's center tap, then each (Weight, Offset) pair twice, once per
 * mirrored tap, for i < SampleCount stepping by 2 -- so a pair the fill writes and the
 * shader never reads counts for nothing here either.
 */
static float VaCuusSumBlurWeights(float Sigma)
{
	FVaCuusBlurPS::FParameters Parameters;
	const int32 SampleCount = VaCuusGlass::FillBlurWeights(&Parameters, Sigma);

	float Sum = Parameters.WeightAndOffsets[0].X + 2.0f * Parameters.WeightAndOffsets[0].Z;
	for (int32 Sample = 2; Sample < SampleCount; Sample += 2)
	{
		const FVector4f& Slot = Parameters.WeightAndOffsets[Sample / 2];
		Sum += 2.0f * (Slot.X + Slot.Z);
	}
	return Sum;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassKernelLightTest, "VaCuus.Render.Glass.KernelKeepsItsLight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassKernelLightTest::RunTest(const FString& Parameters)
{
	// The premise: at half resolution a large sigma loses light. Should the fill ever
	// renormalise, this fails first and says the divisor is no longer what fixes it.
	TestTrue(TEXT("Premise: at divisor 2 a 320px view sigma truncates to under 60% of its light"),
		VaCuusSumBlurWeights(320.0f / 2.0f) < 0.6f);

	TestEqual(TEXT("A HUD-sized sigma stays at half resolution"), VaCuusGlass::PickBlurDivisor(12.0f), 2);
	TestEqual(TEXT("So does the largest sigma half resolution holds"), VaCuusGlass::PickBlurDivisor(83.0f), 2);
	TestEqual(TEXT("A sigma past every divisor stops at MaxDivisor"),
		VaCuusGlass::PickBlurDivisor(2000.0f), VaCuusGlass::MaxDivisor);

	for (const float ViewSigma : {12.0f, 83.0f, 100.0f, 120.0f, 240.0f, 320.0f, 480.0f, 666.0f})
	{
		const int32 Divisor = VaCuusGlass::PickBlurDivisor(ViewSigma);
		const float Sum = VaCuusSumBlurWeights(ViewSigma / float(Divisor));
		TestTrue(FString::Printf(TEXT("A %.0fpx view sigma keeps its light (divisor %d, weights sum to %.4f)"),
			ViewSigma, Divisor, Sum), Sum > 0.99f && Sum < 1.01f);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
