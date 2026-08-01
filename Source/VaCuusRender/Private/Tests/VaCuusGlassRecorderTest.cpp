// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusCommandBuffer.h"
#include "VaCuusEngine.h"
#include "VaCuusRecordingRenderInterface.h"

#include "Misc/ScopeExit.h"
#include "Templates/Function.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Dictionary.h>
#include <RmlUi/Core/Types.h>
#include <RmlUi/Core/Variant.h>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * M5 Task 2: the recorder's layer/filter/clip-mask vocabulary (spec §2(d)) — what
 * `backdrop-filter` records, what a non-blur filter refuses, and what the idle gate sees.
 *
 * The real-context tests drive documents through a real Rml::Context because the property
 * under test is RmlUi's call sequence (ElementEffects.cpp:247-282), which cannot be
 * synthesised without becoming a tautology. FilterListHash drives the recorder directly,
 * following the idle-gate file's stated pattern, because it isolates the HASH leg of the
 * gate: through a real context a sigma change is double-covered — the recompile is
 * resource traffic AND a new handle in the composite's filter list — so only a direct
 * drive with pre-minted handles can show what the filter-slice hashing alone contributes.
 */
namespace VaCuusGlassTest
{
static const FIntPoint GViewSize(800, 600);

/** Records one frame of a real context through the recorder, HoverRecolour-style. */
TUniquePtr<FVaCuusCommandBuffer> RecordContextFrame(FVaCuusRecordingRenderInterface& Recorder, Rml::Context* Context)
{
	Recorder.BeginFrame(GViewSize);
	Context->Update();
	Context->Render();
	return Recorder.EndFrameAndPublish();
}

/** Index of the next command of Type at or after Start, or INDEX_NONE. */
int32 FindNext(const FVaCuusCommandBuffer& Buffer, EVaCuusCommandType Type, int32 Start)
{
	for (int32 Index = Start; Index < Buffer.Commands.Num(); ++Index)
	{
		if (Buffer.Commands[Index].Type == Type)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

/** How many commands of Type the buffer carries. */
int32 CountOf(const FVaCuusCommandBuffer& Buffer, EVaCuusCommandType Type)
{
	int32 Count = 0;
	for (const FVaCuusCommand& Command : Buffer.Commands)
	{
		Count += (Command.Type == Type) ? 1 : 0;
	}
	return Count;
}
} // namespace VaCuusGlassTest

/**
 * The reference glass panel records the exact ElementEffects Enter-stage sequence the
 * Task 3 distiller will parse (ElementEffects.cpp:256-281; backdrop-glass.md §4):
 * scissor(border box + 3σ ink overflow) → PushLayer → CompositeLayers(0 → temp, [blur])
 * → scissor(border box) + clip-mask(Set, radius geometry) → CompositeLayers(temp → 0, {})
 * → PopLayer — with the sigma recorded in NewFilters exactly as FilterBlur resolved it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassBackdropSequenceTest, "VaCuus.Render.Glass.BackdropSequence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassBackdropSequenceTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusGlassTest;

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
	const Rml::String ContextName("vacuus_glass_sequence_test");
	Rml::Context* Context = Rml::CreateContext(ContextName, Rml::Vector2i(GViewSize.X, GViewSize.Y), &Recorder);
	if (!TestNotNull(TEXT("Context"), Context))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Rml::RemoveContext(ContextName);
	};

	// The M5 reference panel: rounded, root-level, no text (no font dependency). The
	// numbers below are load-bearing for the assertions: border box (40,40)-(240,160),
	// blur(12px) at dp-ratio 1.0 -> sigma 12, ink overflow 3*max(12,1) = 36px
	// (FilterBlur::ExtendInkOverflow, FilterBlur.cpp:22-27).
	static const TCHAR* Source =
		TEXT("<rml><head><style>")
		TEXT("body{display:block;width:100%;height:100%;}")
		TEXT("#panel{display:block;position:absolute;left:40px;top:40px;width:200px;height:120px;")
		TEXT("border-radius:16px;background-color:#30405080;backdrop-filter:blur(12px);}")
		TEXT("</style></head><body><div id=\"panel\"/></body></rml>");

	Rml::ElementDocument* Document =
		Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(Source)), "vacuus://glass_sequence.rml");
	if (!TestNotNull(TEXT("Document"), Document))
	{
		return false;
	}
	Document->Show();

	const TUniquePtr<FVaCuusCommandBuffer> Buffer = RecordContextFrame(Recorder, Context);
	if (!TestNotNull(TEXT("The glass frame publishes"), Buffer.Get()))
	{
		return false;
	}

	// The sigma, recorded verbatim: FilterBlur passes ResolveLength(12px) = 12.0 under
	// "sigma" with NO 0.5 factor (FilterBlur.cpp:16-20) — asserted against the source,
	// not against a guess.
	if (!TestEqual(TEXT("Exactly one filter compiled"), Buffer->NewFilters.Num(), 1))
	{
		return false;
	}
	FVaCuusFilterHandle BlurHandle = 0;
	for (const TPair<FVaCuusFilterHandle, FVaCuusFilterData>& Pair : Buffer->NewFilters)
	{
		BlurHandle = Pair.Key;
		TestEqual(TEXT("blur(12px) records sigma 12.0 (FilterBlur.cpp:16-20 — the length itself)"), Pair.Value.Sigma, 12.0f);
	}
	TestTrue(TEXT("The filter handle is non-zero"), BlurHandle != 0);
	TestTrue(TEXT("No filter released this frame"), Buffer->ReleasedFilters.Num() == 0);

	// The §4 sequence, found as an ordered subsequence (body's own background draw etc.
	// may precede and interleave-free assertions on absolute indices would be brittle).
	const int32 PushIndex = FindNext(*Buffer, EVaCuusCommandType::PushLayer, 0);
	if (!TestTrue(TEXT("PushLayer recorded"), PushIndex != INDEX_NONE))
	{
		return false;
	}
	const FVaCuusLayerHandle TempLayer = Buffer->Commands[PushIndex].SourceLayer;
	TestTrue(TEXT("The temp layer handle is non-zero (0 is the reserved base layer, RenderInterface.h:96)"), TempLayer != 0);

	// The scissor immediately preceding the push is the backdrop read region: border box
	// extended by the blur's ink overflow, 3*max(sigma,1) = 36px -> (4,4)-(276,196).
	{
		int32 ScissorIndex = INDEX_NONE;
		for (int32 Index = PushIndex - 1; Index >= 0; --Index)
		{
			if (Buffer->Commands[Index].Type == EVaCuusCommandType::SetScissor)
			{
				ScissorIndex = Index;
				break;
			}
		}
		if (TestTrue(TEXT("A scissor precedes the backdrop PushLayer"), ScissorIndex != INDEX_NONE))
		{
			TestTrue(TEXT("...covering border box + 3-sigma ink overflow (4,4)-(276,196)"),
				Buffer->Commands[ScissorIndex].Scissor == FIntRect(4, 4, 276, 196));
		}
	}

	// CompositeLayers(0 -> temp) carrying exactly the one blur handle — the variable-length
	// record read back through its offset/count slice.
	const int32 GrabIndex = FindNext(*Buffer, EVaCuusCommandType::CompositeLayers, PushIndex + 1);
	if (!TestTrue(TEXT("CompositeLayers(0 -> temp) recorded"), GrabIndex != INDEX_NONE))
	{
		return false;
	}
	{
		const FVaCuusCommand& Grab = Buffer->Commands[GrabIndex];
		TestTrue(TEXT("...source is the base layer 0"), Grab.SourceLayer == 0);
		TestTrue(TEXT("...destination is the pushed temp layer"), Grab.DestLayer == TempLayer);
		TestTrue(TEXT("...blend mode is Blend"), Grab.Blend == EVaCuusBlendMode::Blend);
		if (TestEqual(TEXT("...with exactly one filter"), Grab.FilterCount, 1))
		{
			TestTrue(TEXT("...whose slice names the compiled blur handle"),
				Buffer->CompositeFilters.IsValidIndex(Grab.FilterOffset) &&
					Buffer->CompositeFilters[Grab.FilterOffset] == BlurHandle);
		}
	}

	// The rounded clip: border-radius always goes through the clip mask
	// (ElementUtilities.cpp:157-169), applied between the two composites.
	const int32 EnableIndex = FindNext(*Buffer, EVaCuusCommandType::EnableClipMask, GrabIndex + 1);
	if (!TestTrue(TEXT("EnableClipMask recorded after the grab composite"), EnableIndex != INDEX_NONE))
	{
		return false;
	}
	TestEqual(TEXT("...enabling the mask"), int32(Buffer->Commands[EnableIndex].bClipMaskEnable), 1);

	const int32 MaskIndex = FindNext(*Buffer, EVaCuusCommandType::RenderToClipMask, EnableIndex + 1);
	if (!TestTrue(TEXT("RenderToClipMask recorded"), MaskIndex != INDEX_NONE))
	{
		return false;
	}
	{
		const FVaCuusCommand& Mask = Buffer->Commands[MaskIndex];
		TestTrue(TEXT("...operation Set (first mask in the list, ElementUtilities.cpp:165)"),
			Mask.ClipMaskOp == EVaCuusClipMaskOp::Set);
		TestTrue(TEXT("...at the panel's border-box offset (40,40)"), Mask.Translation == FVector2f(40.f, 40.f));
		const FVaCuusGeometryData* MaskGeometry = Buffer->NewGeometry.Find(Mask.Geometry);
		if (TestNotNull(TEXT("...with mask geometry resolvable in NewGeometry — where the distiller reads it"), MaskGeometry))
		{
			TestTrue(TEXT("...and the mask geometry is non-empty"),
				MaskGeometry->Vertices.Num() > 0 && MaskGeometry->Indices.Num() > 0);
		}
	}

	// CompositeLayers(temp -> 0) with an EMPTY filter list, then PopLayer.
	const int32 WriteBackIndex = FindNext(*Buffer, EVaCuusCommandType::CompositeLayers, MaskIndex + 1);
	if (!TestTrue(TEXT("CompositeLayers(temp -> 0) recorded"), WriteBackIndex != INDEX_NONE))
	{
		return false;
	}
	{
		const FVaCuusCommand& WriteBack = Buffer->Commands[WriteBackIndex];
		TestTrue(TEXT("...source is the temp layer"), WriteBack.SourceLayer == TempLayer);
		TestTrue(TEXT("...destination is the base layer 0"), WriteBack.DestLayer == 0);
		TestEqual(TEXT("...with no filters"), WriteBack.FilterCount, 0);
	}

	const int32 PopIndex = FindNext(*Buffer, EVaCuusCommandType::PopLayer, WriteBackIndex + 1);
	TestTrue(TEXT("PopLayer closes the backdrop sequence"), PopIndex != INDEX_NONE);

	// Frame end: ResetState clears the mask list, which reaches the recorder as the
	// disable edge (RenderManager::ApplyClipMask, RenderManager.cpp:156-176; called from
	// ResetState via SetClipMask at :178-190).
	{
		const int32 DisableIndex = FindNext(*Buffer, EVaCuusCommandType::EnableClipMask, MaskIndex + 1);
		bool bDisableSeen = false;
		for (int32 Index = DisableIndex; Index != INDEX_NONE && Index < Buffer->Commands.Num();
			Index = FindNext(*Buffer, EVaCuusCommandType::EnableClipMask, Index + 1))
		{
			bDisableSeen |= (Buffer->Commands[Index].bClipMaskEnable == 0);
		}
		TestTrue(TEXT("The mask's disable edge is recorded before the frame ends"), bDisableSeen);
	}

	// Exactly one backdrop element: one push, one pop, two composites. A count mismatch
	// here means the sequence above matched by luck.
	TestEqual(TEXT("One PushLayer in the frame"), CountOf(*Buffer, EVaCuusCommandType::PushLayer), 1);
	TestEqual(TEXT("One PopLayer in the frame"), CountOf(*Buffer, EVaCuusCommandType::PopLayer), 1);
	TestEqual(TEXT("Two CompositeLayers in the frame"), CountOf(*Buffer, EVaCuusCommandType::CompositeLayers), 2);

	return true;
}

/**
 * The blur-only policy observed end to end (spec §2(d)): a non-blur backdrop filter gets
 * handle 0, RmlUi's own per-element warning fires, VaCuus adds ONE latched line per TYPE
 * (two elements, one line), no filter lands in NewFilters, and the composite arrives with
 * an EMPTY filter list because AddHandleTo dropped the invalid handle
 * (CompiledFilterShader.cpp:6-12).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassNonBlurRefusalTest, "VaCuus.Render.Glass.NonBlurRefusal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassNonBlurRefusalTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusGlassTest;

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
	const Rml::String ContextName("vacuus_glass_refusal_test");
	Rml::Context* Context = Rml::CreateContext(ContextName, Rml::Vector2i(GViewSize.X, GViewSize.Y), &Recorder);
	if (!TestNotNull(TEXT("Context"), Context))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Rml::RemoveContext(ContextName);
	};

	// TWO elements with the same refused type: the VaCuus line must appear ONCE (latched
	// per type), while RmlUi's own warning names each element (ElementEffects.cpp:153-165,
	// routed through FVaCuusSystemInterface as a LogVaCuus warning).
	AddExpectedMessagePlain(TEXT("filter type 'brightness' is not supported"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("Could not compile filter on element"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 2);

	static const TCHAR* Source =
		TEXT("<rml><head><style>")
		TEXT("body{display:block;width:100%;height:100%;}")
		TEXT(".dim{display:block;position:absolute;width:100px;height:60px;background-color:#30405080;")
		TEXT("backdrop-filter:brightness(0.5);}")
		TEXT("#a{left:20px;top:20px;}")
		TEXT("#b{left:200px;top:20px;}")
		TEXT("</style></head><body><div id=\"a\" class=\"dim\"/><div id=\"b\" class=\"dim\"/></body></rml>");

	Rml::ElementDocument* Document =
		Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(Source)), "vacuus://glass_refusal.rml");
	if (!TestNotNull(TEXT("Document"), Document))
	{
		return false;
	}
	Document->Show();

	const TUniquePtr<FVaCuusCommandBuffer> Buffer = RecordContextFrame(Recorder, Context);
	if (!TestNotNull(TEXT("The frame publishes"), Buffer.Get()))
	{
		return false;
	}

	// The zero handle suppressed the filter, not the sequence: RmlUi still runs the full
	// backdrop pipeline per element (the FilterEntry exists even when its compile failed,
	// ElementEffects.cpp:109-122) — only the filter list is empty, because
	// RenderManager::CompileFilter wraps nothing for a zero handle
	// (RenderManager.cpp:274-283) and AddHandleTo skips the invalid handle.
	TestEqual(TEXT("No filter compiled"), Buffer->NewFilters.Num(), 0);
	TestEqual(TEXT("No filter released"), Buffer->ReleasedFilters.Num(), 0);
	TestEqual(TEXT("No filter slice recorded"), Buffer->CompositeFilters.Num(), 0);

	TestEqual(TEXT("Both backdrop sequences still ran (two pushes)"), CountOf(*Buffer, EVaCuusCommandType::PushLayer), 2);
	TestEqual(TEXT("Four composites (grab + write-back per element)"), CountOf(*Buffer, EVaCuusCommandType::CompositeLayers), 4);
	for (const FVaCuusCommand& Command : Buffer->Commands)
	{
		if (Command.Type == EVaCuusCommandType::CompositeLayers)
		{
			TestEqual(TEXT("Every composite arrives with an empty filter list"), Command.FilterCount, 0);
		}
	}

	return true;
}

/**
 * The sigma change, end to end: a static glass document IDLES (which is what the
 * per-frame layer-handle restart buys — see FVaCuusLayerHandle), and a 12px -> 20px class
 * swap publishes with the old filter released, a new strictly-greater handle compiled at
 * the new sigma, and the composite's slice naming the new handle. Through a real context
 * this is double-covered at the gate — the recompile is resource traffic AND a new handle
 * in the hashed slice — the same two-leg structure the header records for geometry; the
 * hash leg is isolated in FilterListHash below.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassSigmaChangeTest, "VaCuus.Render.Glass.SigmaChangePublishes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassSigmaChangeTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusGlassTest;

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
	const Rml::String ContextName("vacuus_glass_sigma_test");
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
		TEXT(".blur12{backdrop-filter:blur(12px);}")
		TEXT(".blur20{backdrop-filter:blur(20px);}")
		TEXT("</style></head><body><div id=\"panel\" class=\"blur12\"/></body></rml>");

	Rml::ElementDocument* Document =
		Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(Source)), "vacuus://glass_sigma.rml");
	if (!TestNotNull(TEXT("Document"), Document))
	{
		return false;
	}
	Document->Show();

	// SETTLE, then idle — and the settle is a FACT with a mechanism, not slack: a fresh
	// rounded element publishes exactly TWICE before going static. Frame 1's backdrop
	// Enter stage compiles and draws the clip-mask geometry; later in the SAME frame the
	// element's first-ever background generation runs with its dirty flag still set from
	// construction and releases every non-background entry — the clip geometry included
	// (ElementBackgroundBorder::Render, ElementBackgroundBorder.cpp:19-25). Frame 2's
	// backdrop finds the clip Geometry empty, regenerates it (GetClipGeometry,
	// ElementBackgroundBorder.cpp:66-73) and compiles a new handle at the mask draw —
	// one-geometry resource traffic, so frame 2 MUST publish. The blur is NOT recompiled
	// (observed: frame 2 carries no filter traffic), so the live handle is tracked
	// through whichever settle publishes carry one.
	//
	// Not new churn either: pre-M5 rounded-clipping documents took the same two publishes
	// — the clip compile/release always crossed the render interface; only the mask DRAW
	// was a silent no-op default then.
	FVaCuusFilterHandle OldHandle = 0;
	int32 SettlePublishes = 0;
	for (; SettlePublishes < 4; ++SettlePublishes)
	{
		const TUniquePtr<FVaCuusCommandBuffer> Frame = RecordContextFrame(Recorder, Context);
		if (!Frame)
		{
			break;
		}
		for (const TPair<FVaCuusFilterHandle, FVaCuusFilterData>& Pair : Frame->NewFilters)
		{
			TestTrue(TEXT("Each compiled blur's handle is strictly above the last (never recycled)"), Pair.Key > OldHandle);
			OldHandle = Pair.Key;
			TestEqual(TEXT("...at sigma 12"), Pair.Value.Sigma, 12.0f);
		}
	}
	TestTrue(TEXT("The document compiled its blur while settling"), OldHandle != 0);

	// THE IDLE ASSERTION THE LAYER-HANDLE DESIGN EXISTS FOR: once settled, an unchanged
	// glass frame is withheld. With cross-frame layer handles every frame would record a
	// fresh PushLayer handle, hash unique, and publish forever — the M2 idle economy
	// silently gone for every glass document. The settle loop above exited on a WITHHELD
	// frame, so this count being exactly the mechanism's two IS the observation.
	if (!TestEqual(TEXT("A static glass document publishes exactly twice, then idles (the clip-geometry settle)"),
			SettlePublishes, 2))
	{
		return false;
	}

	// The swap. Effects re-instance on the next render: the old compiled filter is
	// released (ElementEffects::ReleaseEffects, :169-183) and the new sigma compiles a
	// new handle (ReloadEffectsData, :153-166).
	Rml::Element* Panel = Document->GetElementById("panel");
	if (!TestNotNull(TEXT("Panel element"), Panel))
	{
		return false;
	}
	Panel->SetClassNames("blur20");

	const TUniquePtr<FVaCuusCommandBuffer> Swapped = RecordContextFrame(Recorder, Context);
	if (!TestNotNull(TEXT("A sigma-only class swap publishes"), Swapped.Get()))
	{
		return false;
	}

	if (!TestEqual(TEXT("One new blur compiled"), Swapped->NewFilters.Num(), 1))
	{
		return false;
	}
	FVaCuusFilterHandle NewHandle = 0;
	for (const TPair<FVaCuusFilterHandle, FVaCuusFilterData>& Pair : Swapped->NewFilters)
	{
		NewHandle = Pair.Key;
		TestEqual(TEXT("...at sigma 20"), Pair.Value.Sigma, 20.0f);
	}
	TestTrue(TEXT("Filter handles are strictly increasing, never recycled"), NewHandle > OldHandle);
	TestTrue(TEXT("The old filter is released in the same buffer"), Swapped->ReleasedFilters.Contains(OldHandle));

	// The grab composite (0 -> temp) now names the NEW handle in its slice.
	bool bNewHandleComposited = false;
	for (const FVaCuusCommand& Command : Swapped->Commands)
	{
		if (Command.Type == EVaCuusCommandType::CompositeLayers && Command.SourceLayer == 0 && Command.FilterCount == 1)
		{
			bNewHandleComposited = Swapped->CompositeFilters.IsValidIndex(Command.FilterOffset) &&
				Swapped->CompositeFilters[Command.FilterOffset] == NewHandle;
		}
	}
	TestTrue(TEXT("The grab composite's filter slice names the new handle"), bNewHandleComposited);

	// And it settles: the 20px document is static again.
	TestNull(TEXT("The swapped document then goes idle again"), RecordContextFrame(Recorder, Context).Get());

	return true;
}

/**
 * THE HASH LEG IN ISOLATION, plus per-scalar positive controls for every new hashed
 * field — the discipline VaCuus.Render.IdleGate.HashPadding applies to the original six,
 * extended to the M5 seven and to the variable-length slice.
 *
 * Part 1 pre-mints two blur handles so the sigma swap between frames carries ZERO
 * resource traffic — the one situation where the filter-slice bytes are the gate's only
 * way to see the change. This is the restore-the-bug vehicle for the tripwire: delete the
 * filter-slice Update in VaCuusHashFrameContent and "a sigma-only composite change
 * publishes" fails with GetNumFramesPublished stalled at 1.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassFilterListHashTest, "VaCuus.Render.Glass.FilterListHash",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassFilterListHashTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusGlassTest;

	FVaCuusRecordingRenderInterface Recorder;

	// Frame 1: mint BOTH filters up front (the only frame with resource traffic), then
	// composite with the first.
	Recorder.BeginFrame(GViewSize);
	const Rml::CompiledFilterHandle Blur12 =
		Recorder.CompileFilter("blur", Rml::Dictionary{{"sigma", Rml::Variant(12.0f)}});
	const Rml::CompiledFilterHandle Blur20 =
		Recorder.CompileFilter("blur", Rml::Dictionary{{"sigma", Rml::Variant(20.0f)}});
	TestTrue(TEXT("Two distinct non-zero blur handles"), Blur12 != 0 && Blur20 != 0 && Blur12 != Blur20);

	const auto RecordComposite = [&Recorder](Rml::CompiledFilterHandle Filter)
	{
		const Rml::LayerHandle Layer = Recorder.PushLayer();
		Recorder.CompositeLayers(Rml::LayerHandle(0), Layer, Rml::BlendMode::Blend,
			Rml::Span<const Rml::CompiledFilterHandle>(&Filter, 1));
		Recorder.PopLayer();
	};

	RecordComposite(Blur12);
	const TUniquePtr<FVaCuusCommandBuffer> First = Recorder.EndFrameAndPublish();
	if (!TestNotNull(TEXT("Frame 1 publishes (first frame + filter traffic)"), First.Get()))
	{
		return false;
	}
	TestEqual(TEXT("Both filters recorded with their sigmas"), First->NewFilters.Num(), 2);
	if (const FVaCuusFilterData* Data12 = First->NewFilters.Find(FVaCuusFilterHandle(Blur12)))
	{
		TestEqual(TEXT("Blur12 carries sigma 12"), Data12->Sigma, 12.0f);
	}
	if (const FVaCuusFilterData* Data20 = First->NewFilters.Find(FVaCuusFilterHandle(Blur20)))
	{
		TestEqual(TEXT("Blur20 carries sigma 20"), Data20->Sigma, 20.0f);
	}

	// Frame 2: identical composite, no traffic -> withheld. Also proves layer handles
	// restart per frame: PushLayer minted a fresh handle in frame 1 and mints the SAME
	// value again here, or the hash would differ and this would publish.
	Recorder.BeginFrame(GViewSize);
	RecordComposite(Blur12);
	TestNull(TEXT("An identical composite frame is withheld"), Recorder.EndFrameAndPublish().Get());
	TestEqual(TEXT("One publish so far"), int32(Recorder.GetNumFramesPublished()), 1);

	// Frame 3: the SAME shape drawing with the other pre-minted handle — a sigma-only
	// change with zero resource traffic. Only the hashed slice bytes can see it.
	Recorder.BeginFrame(GViewSize);
	RecordComposite(Blur20);
	const TUniquePtr<FVaCuusCommandBuffer> Third = Recorder.EndFrameAndPublish();
	TestNotNull(TEXT("A sigma-only composite change publishes (the filter slice is hashed)"), Third.Get());
	TestEqual(TEXT("Two publishes"), int32(Recorder.GetNumFramesPublished()), 2);

	// Frame 4: and the 20px state settles.
	Recorder.BeginFrame(GViewSize);
	RecordComposite(Blur20);
	TestNull(TEXT("The changed state then idles"), Recorder.EndFrameAndPublish().Get());

	// ---- Part 2: per-scalar controls for the new hashed fields (HashPadding's method).
	FVaCuusCommandBuffer Clean;
	Clean.ViewSize = FIntPoint(1920, 1080);
	{
		FVaCuusCommand& Command = Clean.Commands.AddDefaulted_GetRef();
		Command.Type = EVaCuusCommandType::CompositeLayers;
		Command.SourceLayer = 3;
		Command.DestLayer = 5;
		Command.FilterOffset = 0;
		Command.FilterCount = 1;
		Command.Blend = EVaCuusBlendMode::Blend;
		Command.ClipMaskOp = EVaCuusClipMaskOp::Set;
		Command.bClipMaskEnable = 0;
	}
	// Two entries: [0] inside the command's slice, [1] OUTSIDE every slice.
	Clean.CompositeFilters.Add(7);
	Clean.CompositeFilters.Add(9);

	const uint64 Baseline = VaCuusHashFrameContent(Clean);

	const auto TestFieldIsHashed = [this, &Clean, Baseline](const TCHAR* Field, TFunctionRef<void(FVaCuusCommandBuffer&)> Mutate)
	{
		FVaCuusCommandBuffer Changed = Clean;
		Mutate(Changed);
		TestTrue(FString::Printf(TEXT("%s is part of the frame hash"), Field),
			VaCuusHashFrameContent(Changed) != Baseline);
	};

	TestFieldIsHashed(TEXT("SourceLayer"), [](FVaCuusCommandBuffer& B) { B.Commands[0].SourceLayer = 4; });
	TestFieldIsHashed(TEXT("DestLayer"), [](FVaCuusCommandBuffer& B) { B.Commands[0].DestLayer = 6; });
	TestFieldIsHashed(TEXT("FilterOffset"), [](FVaCuusCommandBuffer& B) { B.Commands[0].FilterOffset = 1; });
	TestFieldIsHashed(TEXT("FilterCount"), [](FVaCuusCommandBuffer& B) { B.Commands[0].FilterCount = 2; });
	TestFieldIsHashed(TEXT("Blend"), [](FVaCuusCommandBuffer& B) { B.Commands[0].Blend = EVaCuusBlendMode::Replace; });
	TestFieldIsHashed(TEXT("ClipMaskOp"), [](FVaCuusCommandBuffer& B) { B.Commands[0].ClipMaskOp = EVaCuusClipMaskOp::Intersect; });
	TestFieldIsHashed(TEXT("bClipMaskEnable"), [](FVaCuusCommandBuffer& B) { B.Commands[0].bClipMaskEnable = 1; });

	// THE SLICE-CONTENT CONTROL — the whole point of the variable-length record: same
	// offset, same count, different handle byte. This is the exact mutation the
	// restore-the-bug removes coverage of.
	TestFieldIsHashed(TEXT("CompositeFilters slice content"), [](FVaCuusCommandBuffer& B) { B.CompositeFilters[0] = 8; });

	// And its negative: bytes OUTSIDE every command's slice are not frame content, so the
	// hash must ignore them — the hash covers what commands draw WITH, not the array's
	// storage. (Also what keeps the offset-bookkeeping honest: only slices matter.)
	{
		FVaCuusCommandBuffer Changed = Clean;
		Changed.CompositeFilters[1] = 10;
		TestTrue(TEXT("A filter entry outside every slice is NOT part of the frame hash"),
			VaCuusHashFrameContent(Changed) == Baseline);
	}

	return true;
}

/**
 * No regression: a document with no backdrop-filter and no border-radius records ZERO
 * commands of the five new kinds and no filter traffic — pre-M5 buffers, byte for byte.
 * (Non-glass documents rendering pixel-identically through the replayer's skip cases is
 * then immediate: no new-kind commands exist for the replayer to skip.)
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassPlainDocumentTest, "VaCuus.Render.Glass.PlainDocument",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassPlainDocumentTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusGlassTest;

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
	const Rml::String ContextName("vacuus_glass_plain_test");
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
		TEXT("#box{display:block;position:absolute;left:20px;top:20px;width:100px;height:60px;background-color:#204080;}")
		TEXT("</style></head><body><div id=\"box\"/></body></rml>");

	Rml::ElementDocument* Document =
		Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(Source)), "vacuus://glass_plain.rml");
	if (!TestNotNull(TEXT("Document"), Document))
	{
		return false;
	}
	Document->Show();

	const TUniquePtr<FVaCuusCommandBuffer> Buffer = RecordContextFrame(Recorder, Context);
	if (!TestNotNull(TEXT("The plain frame publishes"), Buffer.Get()))
	{
		return false;
	}

	TestTrue(TEXT("The plain frame draws something"), Buffer->Commands.Num() > 0);
	TestEqual(TEXT("No PushLayer"), CountOf(*Buffer, EVaCuusCommandType::PushLayer), 0);
	TestEqual(TEXT("No PopLayer"), CountOf(*Buffer, EVaCuusCommandType::PopLayer), 0);
	TestEqual(TEXT("No CompositeLayers"), CountOf(*Buffer, EVaCuusCommandType::CompositeLayers), 0);
	TestEqual(TEXT("No EnableClipMask"), CountOf(*Buffer, EVaCuusCommandType::EnableClipMask), 0);
	TestEqual(TEXT("No RenderToClipMask"), CountOf(*Buffer, EVaCuusCommandType::RenderToClipMask), 0);
	TestEqual(TEXT("No filters compiled"), Buffer->NewFilters.Num(), 0);
	TestEqual(TEXT("No filters released"), Buffer->ReleasedFilters.Num(), 0);
	TestEqual(TEXT("No filter slices"), Buffer->CompositeFilters.Num(), 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
