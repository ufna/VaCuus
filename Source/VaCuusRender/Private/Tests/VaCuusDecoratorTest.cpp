// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusCommandBuffer.h"
#include "VaCuusEngine.h"
#include "VaCuusRecordingRenderInterface.h"

#include "Misc/ScopeExit.h"
#include "Templates/Function.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/Dictionary.h>
#include <RmlUi/Core/Types.h>
#include <RmlUi/Core/Variant.h>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * M5 Task 4: the recorder's shader vocabulary (spec §2(e)) — what the three gradient
 * decorators and `shader(<builtin>)` record, what an unknown key refuses, and what the
 * idle gate sees.
 *
 * The real-context tests drive documents through a real Rml::Context because the
 * property under test is RmlUi's dictionary contents at the CompileShader boundary
 * (DecoratorGradient.cpp:252-259, :422-428, :619-625; DecoratorShader.cpp:35), which a
 * synthetic drive would merely restate. ShaderHash drives the recorder directly,
 * following the idle-gate file's stated pattern, because it isolates the HASH leg:
 * through a real context a gradient change is double-covered — the recompile is
 * resource traffic AND a new Shader handle in the draw command — so only a direct drive
 * with pre-minted handles can show what hashing the Shader field alone contributes.
 */
namespace VaCuusDecoratorTest
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

/** The first desc of the given kind in NewShaders, with its handle; null if absent. */
const FVaCuusShaderDesc* FindDescOfKind(const FVaCuusCommandBuffer& Buffer, EVaCuusShaderKind Kind, FVaCuusShaderHandle* OutHandle = nullptr)
{
	for (const TPair<FVaCuusShaderHandle, FVaCuusShaderDesc>& Pair : Buffer.NewShaders)
	{
		if (Pair.Value.Kind == Kind)
		{
			if (OutHandle)
			{
				*OutHandle = Pair.Key;
			}
			return &Pair.Value;
		}
	}
	return nullptr;
}

/** The DrawShader command whose handle matches, or null. */
const FVaCuusCommand* FindDrawOfShader(const FVaCuusCommandBuffer& Buffer, FVaCuusShaderHandle Shader)
{
	for (const FVaCuusCommand& Command : Buffer.Commands)
	{
		if (Command.Type == EVaCuusCommandType::DrawShader && Command.Shader == Shader)
		{
			return &Command;
		}
	}
	return nullptr;
}
} // namespace VaCuusDecoratorTest

/**
 * The four dictionaries, recorded verbatim through a real context — every expected
 * number below is derived from the RmlUi source, not from a first run:
 *
 *  - linear-gradient(90deg) on a 200x100 box: the gradient line runs horizontally
 *    through the box center — p0 (0,50), p1 (200,50), length 200 (CalculateShape,
 *    DecoratorGradient.cpp:291-322: line_vector (sin 90, -cos 90) = (1,0), the corner
 *    projections onto the horizontal line through (100,50), length = |200*1| + |100*0|).
 *    Two auto stops resolve to the 0 and 1 edges (resolve_edge_stop, :50-56).
 *  - radial-gradient(circle) on the same box: center (100,50) (default `at center`),
 *    farthest-corner default → radius = |(100,50)| = 111.8034 in BOTH components
 *    (CalculateRadialGradientShape, :486-497: circle radius = r.Magnitude()).
 *  - conic-gradient(from 45deg): angle 45deg in radians, center Round()ed (100,50)
 *    (:614-616); three stops, the middle auto evenly spaced to 0.5 (:66-99).
 *  - shader(glass-panel): the registry key verbatim plus the paint-box dimensions
 *    (DecoratorShader.cpp:35).
 *
 * Plus the draw side: one DrawShader per decorator, each handle resolving in
 * NewShaders, each geometry in NewGeometry, translation = the element's border-box
 * offset (RenderElement passes GetAbsoluteOffset(Border), DecoratorGradient.cpp:287).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusDecoratorDictionariesTest, "VaCuus.Render.Decorator.GradientDictionaries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusDecoratorDictionariesTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusDecoratorTest;

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
	const Rml::String ContextName("vacuus_decorator_dict_test");
	Rml::Context* Context = Rml::CreateContext(ContextName, Rml::Vector2i(GViewSize.X, GViewSize.Y), &Recorder);
	if (!TestNotNull(TEXT("Context"), Context))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Rml::RemoveContext(ContextName);
	};

	// Four 200x100 panels at known offsets; no text (no font dependency). Full-alpha
	// colors so the premultiplied conversion is exact in bytes.
	static const TCHAR* Source =
		TEXT("<rml><head><style>")
		TEXT("body{display:block;width:100%;height:100%;}")
		TEXT("div{display:block;position:absolute;width:200px;height:100px;}")
		TEXT("#lin{left:40px;top:40px;decorator:linear-gradient(90deg, #ff0000, #0000ff);}")
		TEXT("#rad{left:40px;top:180px;decorator:radial-gradient(circle, #00ff00 0%, #000000 100%);}")
		TEXT("#con{left:40px;top:320px;decorator:conic-gradient(from 45deg, #ff0000, #ffff00, #ff0000);}")
		TEXT("#gls{left:40px;top:460px;decorator:shader(glass-panel);}")
		TEXT("</style></head><body><div id=\"lin\"/><div id=\"rad\"/><div id=\"con\"/><div id=\"gls\"/></body></rml>");

	Rml::ElementDocument* Document =
		Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(Source)), "vacuus://decorator_dict.rml");
	if (!TestNotNull(TEXT("Document"), Document))
	{
		return false;
	}
	Document->Show();

	const TUniquePtr<FVaCuusCommandBuffer> Buffer = RecordContextFrame(Recorder, Context);
	if (!TestNotNull(TEXT("The decorator frame publishes"), Buffer.Get()))
	{
		return false;
	}

	TestEqual(TEXT("Four shaders compiled — one per decorator"), Buffer->NewShaders.Num(), 4);
	TestEqual(TEXT("Four DrawShader commands — one per decorator"), CountOf(*Buffer, EVaCuusCommandType::DrawShader), 4);
	TestEqual(TEXT("No shader released this frame"), Buffer->ReleasedShaders.Num(), 0);

	const auto TestStops = [this](const TCHAR* Which, const FVaCuusShaderDesc& Desc, std::initializer_list<FVaCuusColorStop> Expected)
	{
		if (!TestEqual(FString::Printf(TEXT("%s: stop count"), Which), Desc.Stops.Num(), int32(Expected.size())))
		{
			return;
		}
		int32 Index = 0;
		for (const FVaCuusColorStop& Stop : Expected)
		{
			TestTrue(FString::Printf(TEXT("%s: stop %d color (premultiplied float RGBA)"), Which, Index),
				Desc.Stops[Index].Color.Equals(Stop.Color, 1e-4f));
			TestTrue(FString::Printf(TEXT("%s: stop %d position"), Which, Index),
				FMath::IsNearlyEqual(Desc.Stops[Index].Position, Stop.Position, 1e-4f));
			++Index;
		}
	};

	const auto TestDraw = [this, &Buffer](const TCHAR* Which, FVaCuusShaderHandle Handle, const FVector2f& ExpectedTranslation)
	{
		const FVaCuusCommand* Draw = FindDrawOfShader(*Buffer, Handle);
		if (!TestNotNull(FString::Printf(TEXT("%s: a DrawShader references the compiled handle"), Which), Draw))
		{
			return;
		}
		TestTrue(FString::Printf(TEXT("%s: draw translation is the element's border-box offset"), Which),
			Draw->Translation.Equals(ExpectedTranslation, 0.01f));
		TestTrue(FString::Printf(TEXT("%s: draw geometry resolves in NewGeometry"), Which),
			Buffer->NewGeometry.Contains(Draw->Geometry));
		TestTrue(FString::Printf(TEXT("%s: no texture rides the shader draw"), Which), Draw->Texture == 0);
	};

	// linear-gradient(90deg, #ff0000, #0000ff).
	{
		FVaCuusShaderHandle Handle = 0;
		const FVaCuusShaderDesc* Desc = FindDescOfKind(*Buffer, EVaCuusShaderKind::LinearGradient, &Handle);
		if (TestNotNull(TEXT("linear: desc recorded"), Desc))
		{
			TestTrue(TEXT("linear: p0 = (0,50), the box's left-center"), Desc->P0.Equals(FVector2f(0.f, 50.f), 0.01f));
			TestTrue(TEXT("linear: p1 = (200,50), the box's right-center"), Desc->P1.Equals(FVector2f(200.f, 50.f), 0.01f));
			TestTrue(TEXT("linear: length = 200"), FMath::IsNearlyEqual(Desc->Length, 200.f, 0.01f));
			TestEqual(TEXT("linear: not repeating"), int32(Desc->bRepeating), 0);
			TestStops(TEXT("linear"), *Desc,
				{{FVector4f(1.f, 0.f, 0.f, 1.f), 0.f}, {FVector4f(0.f, 0.f, 1.f, 1.f), 1.f}});
			TestDraw(TEXT("linear"), Handle, FVector2f(40.f, 40.f));
		}
	}

	// radial-gradient(circle, #00ff00 0%, #000000 100%).
	{
		FVaCuusShaderHandle Handle = 0;
		const FVaCuusShaderDesc* Desc = FindDescOfKind(*Buffer, EVaCuusShaderKind::RadialGradient, &Handle);
		if (TestNotNull(TEXT("radial: desc recorded"), Desc))
		{
			TestTrue(TEXT("radial: center = (100,50)"), Desc->Center.Equals(FVector2f(100.f, 50.f), 0.01f));
			TestTrue(TEXT("radial: circle farthest-corner radius = |(100,50)| both axes"),
				Desc->Radius.Equals(FVector2f(111.8034f, 111.8034f), 0.01f));
			TestEqual(TEXT("radial: not repeating"), int32(Desc->bRepeating), 0);
			TestStops(TEXT("radial"), *Desc,
				{{FVector4f(0.f, 1.f, 0.f, 1.f), 0.f}, {FVector4f(0.f, 0.f, 0.f, 1.f), 1.f}});
			TestDraw(TEXT("radial"), Handle, FVector2f(40.f, 180.f));
		}
	}

	// conic-gradient(from 45deg, #ff0000, #ffff00, #ff0000).
	{
		FVaCuusShaderHandle Handle = 0;
		const FVaCuusShaderDesc* Desc = FindDescOfKind(*Buffer, EVaCuusShaderKind::ConicGradient, &Handle);
		if (TestNotNull(TEXT("conic: desc recorded"), Desc))
		{
			TestTrue(TEXT("conic: angle = 45deg in radians"), FMath::IsNearlyEqual(Desc->Angle, 0.7853982f, 1e-4f));
			TestTrue(TEXT("conic: center = (100,50), rounded"), Desc->Center.Equals(FVector2f(100.f, 50.f), 0.01f));
			TestStops(TEXT("conic"), *Desc,
				{{FVector4f(1.f, 0.f, 0.f, 1.f), 0.f}, {FVector4f(1.f, 1.f, 0.f, 1.f), 0.5f},
					{FVector4f(1.f, 0.f, 0.f, 1.f), 1.f}});
			TestDraw(TEXT("conic"), Handle, FVector2f(40.f, 320.f));
		}
	}

	// shader(glass-panel).
	{
		FVaCuusShaderHandle Handle = 0;
		const FVaCuusShaderDesc* Desc = FindDescOfKind(*Buffer, EVaCuusShaderKind::Builtin, &Handle);
		if (TestNotNull(TEXT("builtin: desc recorded"), Desc))
		{
			TestEqual(TEXT("builtin: the registry key arrives verbatim"), Desc->BuiltinKey, FString(TEXT("glass-panel")));
			TestTrue(TEXT("builtin: dimensions = the 200x100 paint box"), Desc->Dimensions.Equals(FVector2f(200.f, 100.f), 0.01f));
			TestEqual(TEXT("builtin: no stops"), Desc->Stops.Num(), 0);
			TestDraw(TEXT("builtin"), Handle, FVector2f(40.f, 460.f));
		}
	}

	return true;
}

/**
 * Handle lifecycle through a real context: a static gradient document SETTLES AND
 * IDLES (decorator shaders are compiled once and pooled with the element data,
 * DecoratorShader.cpp:49-53 — nothing recompiles per frame), and a class swap that
 * only changes the gradient's angle publishes with the old handle released, a
 * strictly-greater handle compiled, and the DrawShader command naming the new one.
 *
 * The swap IS the "animated gradients are resource traffic by construction" note in
 * spec §2(e)/plan Task 4: every parameter change re-runs GenerateElementData — release
 * + fresh CompileShader (ElementEffects::ReloadEffectsData, ElementEffects.cpp:133-148)
 * — so an every-frame animation would carry shader traffic every frame and never idle.
 * That is correct behavior, not a gap: the pixels change every frame too.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusDecoratorLifecycleTest, "VaCuus.Render.Decorator.HandleLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusDecoratorLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusDecoratorTest;

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
	const Rml::String ContextName("vacuus_decorator_lifecycle_test");
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
		TEXT("#panel{display:block;position:absolute;left:40px;top:40px;width:200px;height:100px;}")
		TEXT(".g90{decorator:linear-gradient(90deg, #ff0000, #0000ff);}")
		TEXT(".g270{decorator:linear-gradient(270deg, #ff0000, #0000ff);}")
		TEXT("</style></head><body><div id=\"panel\" class=\"g90\"/></body></rml>");

	Rml::ElementDocument* Document =
		Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(Source)), "vacuus://decorator_lifecycle.rml");
	if (!TestNotNull(TEXT("Document"), Document))
	{
		return false;
	}
	Document->Show();

	// Settle: cross-buffer live-handle tracking through however many publishes the
	// document needs (glass settles in two for its clip geometry; a radius-free gradient
	// panel is expected in one — asserted below, not assumed).
	TSet<FVaCuusShaderHandle> Live;
	FVaCuusShaderHandle Newest = 0;
	int32 SettlePublishes = 0;
	for (; SettlePublishes < 4; ++SettlePublishes)
	{
		const TUniquePtr<FVaCuusCommandBuffer> Frame = RecordContextFrame(Recorder, Context);
		if (!Frame)
		{
			break;
		}
		for (const TPair<FVaCuusShaderHandle, FVaCuusShaderDesc>& Pair : Frame->NewShaders)
		{
			TestTrue(TEXT("Each compiled shader's handle is strictly above the last (never recycled)"), Pair.Key > Newest);
			Newest = Pair.Key;
			Live.Add(Pair.Key);
		}
		for (const FVaCuusShaderHandle Handle : Frame->ReleasedShaders)
		{
			Live.Remove(Handle);
		}
	}
	TestEqual(TEXT("A static gradient document publishes exactly once, then idles"), SettlePublishes, 1);
	if (!TestEqual(TEXT("Exactly one live shader after the settle"), Live.Num(), 1))
	{
		return false;
	}
	const FVaCuusShaderHandle OldHandle = *Live.CreateConstIterator();

	// The angle swap: an angle-only restyle. The recorded form of "the angle changed" is
	// a fresh dictionary — new p0/p1 — under a NEW handle, with the old one released.
	Rml::Element* Panel = Document->GetElementById("panel");
	if (!TestNotNull(TEXT("Panel element"), Panel))
	{
		return false;
	}
	Panel->SetClassNames("g270");

	const TUniquePtr<FVaCuusCommandBuffer> Swapped = RecordContextFrame(Recorder, Context);
	if (!TestNotNull(TEXT("An angle-only class swap publishes"), Swapped.Get()))
	{
		return false;
	}

	if (!TestEqual(TEXT("One new shader compiled"), Swapped->NewShaders.Num(), 1))
	{
		return false;
	}
	FVaCuusShaderHandle NewHandle = 0;
	for (const TPair<FVaCuusShaderHandle, FVaCuusShaderDesc>& Pair : Swapped->NewShaders)
	{
		NewHandle = Pair.Key;
		// 270deg runs right-to-left: p0 at the right-center, p1 at the left-center.
		TestTrue(TEXT("...with the 270deg dictionary: p0 = (200,50)"), Pair.Value.P0.Equals(FVector2f(200.f, 50.f), 0.01f));
		TestTrue(TEXT("...p1 = (0,50)"), Pair.Value.P1.Equals(FVector2f(0.f, 50.f), 0.01f));
	}
	TestTrue(TEXT("Shader handles are strictly increasing, never recycled"), NewHandle > OldHandle);
	TestTrue(TEXT("The old shader is released in the same buffer"), Swapped->ReleasedShaders.Contains(OldHandle));
	TestNotNull(TEXT("The DrawShader command names the new handle"), FindDrawOfShader(*Swapped, NewHandle));
	TestNull(TEXT("No DrawShader still names the old handle"), FindDrawOfShader(*Swapped, OldHandle));

	// And it settles: the 270deg document is static again — the idle row for decorated
	// documents (the M3b gates hold; nothing here publishes without a change).
	TestNull(TEXT("The swapped document then goes idle again"), RecordContextFrame(Recorder, Context).Get());

	return true;
}

/**
 * THE HASH LEG IN ISOLATION — the Shader field's restore-the-bug vehicle (the
 * FilterListHash shape). Both shaders are pre-minted in frame 1, so the later swap
 * between them carries ZERO resource traffic: the Shader slot in
 * FVaCuusCommandHashImage is the gate's only way to see it. Delete
 * `Image.Shader = Command.Shader;` in VaCuusHashFrameContent and "a shader-only draw
 * change publishes" fails with GetNumFramesPublished stalled at 1 — observed both ways
 * for the Task 4 report.
 *
 * The real-world frame this synthesizes: two decorators both live (two elements, or a
 * hover restyle toggling between cached looks), a reflow swapping WHICH one draws where
 * — same geometry, same translation, different Shader, nothing compiled or released.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusDecoratorShaderHashTest, "VaCuus.Render.Decorator.ShaderHash",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusDecoratorShaderHashTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusDecoratorTest;

	FVaCuusRecordingRenderInterface Recorder;

	const auto MakeLinearDictionary = [](Rml::Vector2f P0, Rml::Vector2f P1)
	{
		Rml::ColorStopList Stops;
		Stops.push_back({Rml::ColourbPremultiplied(255, 0, 0, 255), Rml::NumericValue(0.f, Rml::Unit::NUMBER)});
		Stops.push_back({Rml::ColourbPremultiplied(0, 0, 255, 255), Rml::NumericValue(1.f, Rml::Unit::NUMBER)});
		return Rml::Dictionary{
			{"p0", Rml::Variant(P0)},
			{"p1", Rml::Variant(P1)},
			{"length", Rml::Variant(200.f)},
			{"repeating", Rml::Variant(false)},
			{"color_stop_list", Rml::Variant(std::move(Stops))},
		};
	};

	// Frame 1: mint the geometry and BOTH shaders up front (the only frame with resource
	// traffic), then draw with the first.
	Recorder.BeginFrame(GViewSize);

	const Rml::Vertex Vertices[4] = {};
	const int Indices[6] = {0, 1, 2, 0, 2, 3};
	const Rml::CompiledGeometryHandle Geometry =
		Recorder.CompileGeometry(Rml::Span<const Rml::Vertex>(Vertices, 4), Rml::Span<const int>(Indices, 6));

	// The two dictionaries differ only in gradient direction — the pre-minted form of
	// "the angle changed".
	const Rml::CompiledShaderHandle Shader90 =
		Recorder.CompileShader("linear-gradient", MakeLinearDictionary({0.f, 50.f}, {200.f, 50.f}));
	const Rml::CompiledShaderHandle Shader270 =
		Recorder.CompileShader("linear-gradient", MakeLinearDictionary({200.f, 50.f}, {0.f, 50.f}));
	TestTrue(TEXT("Two distinct non-zero shader handles"), Shader90 != 0 && Shader270 != 0 && Shader90 != Shader270);

	Recorder.RenderShader(Shader90, Geometry, {10.f, 20.f}, 0);
	const TUniquePtr<FVaCuusCommandBuffer> First = Recorder.EndFrameAndPublish();
	if (!TestNotNull(TEXT("Frame 1 publishes (first frame + shader traffic)"), First.Get()))
	{
		return false;
	}

	// The recorded dictionary, verbatim — the direct-drive counterpart of the
	// real-context assertions in GradientDictionaries.
	TestEqual(TEXT("Both shaders recorded"), First->NewShaders.Num(), 2);
	if (const FVaCuusShaderDesc* Desc = First->NewShaders.Find(FVaCuusShaderHandle(Shader90)))
	{
		TestTrue(TEXT("Shader90 desc: kind linear"), Desc->Kind == EVaCuusShaderKind::LinearGradient);
		TestTrue(TEXT("Shader90 desc: p0"), Desc->P0.Equals(FVector2f(0.f, 50.f), 1e-4f));
		TestTrue(TEXT("Shader90 desc: p1"), Desc->P1.Equals(FVector2f(200.f, 50.f), 1e-4f));
		TestEqual(TEXT("Shader90 desc: two stops"), Desc->Stops.Num(), 2);
	}
	else
	{
		AddError(TEXT("Shader90's desc is missing from NewShaders"));
	}

	// Frame 2: identical draw, no traffic -> withheld.
	Recorder.BeginFrame(GViewSize);
	Recorder.RenderShader(Shader90, Geometry, {10.f, 20.f}, 0);
	TestNull(TEXT("An identical shader frame is withheld"), Recorder.EndFrameAndPublish().Get());
	TestEqual(TEXT("One publish so far"), int32(Recorder.GetNumFramesPublished()), 1);

	// Frame 3: the SAME draw with the other pre-minted handle — zero resource traffic;
	// only the hashed Shader field can see it.
	Recorder.BeginFrame(GViewSize);
	Recorder.RenderShader(Shader270, Geometry, {10.f, 20.f}, 0);
	TestNotNull(TEXT("A shader-only draw change publishes (the Shader field is hashed)"), Recorder.EndFrameAndPublish().Get());
	TestEqual(TEXT("Two publishes"), int32(Recorder.GetNumFramesPublished()), 2);

	// Frame 4: and the changed state settles.
	Recorder.BeginFrame(GViewSize);
	Recorder.RenderShader(Shader270, Geometry, {10.f, 20.f}, 0);
	TestNull(TEXT("The changed state then idles"), Recorder.EndFrameAndPublish().Get());

	// Frame 5: release retires through the buffer — resource traffic, so it publishes
	// even though the draw list did not change shape.
	Recorder.BeginFrame(GViewSize);
	Recorder.RenderShader(Shader270, Geometry, {10.f, 20.f}, 0);
	Recorder.ReleaseShader(Shader90);
	const TUniquePtr<FVaCuusCommandBuffer> Released = Recorder.EndFrameAndPublish();
	if (TestNotNull(TEXT("A release-only frame publishes (resource traffic)"), Released.Get()))
	{
		TestTrue(TEXT("...carrying the released handle"), Released->ReleasedShaders.Contains(FVaCuusShaderHandle(Shader90)));
	}

	// ---- The per-scalar positive control for the new hashed field (HashPadding's method).
	FVaCuusCommandBuffer Clean;
	Clean.ViewSize = FIntPoint(1920, 1080);
	{
		FVaCuusCommand& Command = Clean.Commands.AddDefaulted_GetRef();
		Command.Type = EVaCuusCommandType::DrawShader;
		Command.Geometry = 7;
		Command.Shader = 3;
		Command.Translation = FVector2f(11.f, 13.f);
	}
	const uint64 Baseline = VaCuusHashFrameContent(Clean);

	FVaCuusCommandBuffer Changed = Clean;
	Changed.Commands[0].Shader = 4;
	TestTrue(TEXT("Shader is part of the frame hash"), VaCuusHashFrameContent(Changed) != Baseline);

	return true;
}

/**
 * The unknown-builtin refusal, end to end — THE DECISION OBSERVED (M5 plan Task 4's
 * "decide which behavior is more honest"): an unregistered `shader(<key>)` returns
 * handle 0 rather than minting an inert handle, because a zero suppresses exactly ONE
 * decorator on ONE element (Decorator.h:42-44; the per-entry guard in
 * ElementEffects.cpp:196-200) — not the declaration, not the element, not the document
 * — making it the same per-effect refusal shape as the blur-only filter policy, with
 * zero dead state recorded. Observed here: ONE latched VaCuus line naming the known
 * keys (two elements, one line), RmlUi's own per-element warning
 * (ElementEffects.cpp:150-151) twice, NO desc and NO DrawShader for the unknown key —
 * while the sibling glass-panel decorator and the plain background in the same document
 * record and draw normally.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusDecoratorUnknownKeyTest, "VaCuus.Render.Decorator.UnknownBuiltinKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusDecoratorUnknownKeyTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusDecoratorTest;

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
	const Rml::String ContextName("vacuus_decorator_unknown_test");
	Rml::Context* Context = Rml::CreateContext(ContextName, Rml::Vector2i(GViewSize.X, GViewSize.Y), &Recorder);
	if (!TestNotNull(TEXT("Context"), Context))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Rml::RemoveContext(ContextName);
	};

	// TWO elements with the same unknown key: the VaCuus line must appear ONCE (latched
	// per key), while RmlUi's own warning names each element.
	AddExpectedMessagePlain(TEXT("shader builtin key 'no-such-builtin' is not registered"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("Could not generate decorator element data"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 2);

	static const TCHAR* Source =
		TEXT("<rml><head><style>")
		TEXT("body{display:block;width:100%;height:100%;}")
		TEXT("div{display:block;position:absolute;width:100px;height:60px;}")
		TEXT("#a{left:20px;top:20px;decorator:shader(no-such-builtin);}")
		TEXT("#b{left:140px;top:20px;decorator:shader(no-such-builtin);}")
		TEXT("#c{left:260px;top:20px;decorator:shader(glass-panel);}")
		TEXT("#d{left:380px;top:20px;background-color:#204080;}")
		TEXT("</style></head><body><div id=\"a\"/><div id=\"b\"/><div id=\"c\"/><div id=\"d\"/></body></rml>");

	Rml::ElementDocument* Document =
		Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(Source)), "vacuus://decorator_unknown.rml");
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

	// The refusal recorded NOTHING for the unknown key — no desc, no draw, no dead
	// handle — and the rest of the document is untouched: the registered builtin
	// compiled and drew, the plain background drew.
	TestEqual(TEXT("Exactly one shader compiled (the registered builtin)"), Buffer->NewShaders.Num(), 1);
	FVaCuusShaderHandle GlassHandle = 0;
	if (const FVaCuusShaderDesc* Desc = FindDescOfKind(*Buffer, EVaCuusShaderKind::Builtin, &GlassHandle))
	{
		TestEqual(TEXT("...and it is glass-panel"), Desc->BuiltinKey, FString(TEXT("glass-panel")));
	}
	TestEqual(TEXT("Exactly one DrawShader (the registered builtin's)"), CountOf(*Buffer, EVaCuusCommandType::DrawShader), 1);
	TestNotNull(TEXT("...naming the compiled handle"), FindDrawOfShader(*Buffer, GlassHandle));
	TestTrue(TEXT("The document still draws its plain content"), CountOf(*Buffer, EVaCuusCommandType::DrawGeometry) > 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
