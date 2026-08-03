// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusCommandBuffer.h"
#include "VaCuusEngine.h"
#include "VaCuusRecordingRenderInterface.h"
#include "VaCuusRmlDocumentHost.h"
#include "VaCuusSlateElement.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "Templates/Function.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Types.h>
#include <RmlUi/Core/Vertex.h>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The idle short-circuit (M2 Task 12): a recorded frame that draws exactly what the
 * render thread already has is NOT published.
 *
 * The first six are driven straight against FVaCuusRecordingRenderInterface, with no
 * Rml::Context in sight, because every property they assert is about the gate's decision and
 * none of them is about RmlUi. Two consequences worth naming:
 *  - the frames are byte-exact by construction, where a real document's are only
 *    "unchanged as far as anyone can tell";
 *  - "an unchanged frame is not published" needs an observable to be a test at all.
 *    GetNumFramesPublished()/GetNumFramesSkipped() are it: the screen looks identical
 *    whether the gate works or not.
 *
 * THE THREE EXCEPTIONS AT THE BOTTOM OF THE FILE ARE DELIBERATE, and each is here because a
 * synthesised frame cannot reach what it is about:
 *  - HoverRecolour drives a real Rml::Context, because the gate's completeness rests on
 *    RmlUi's own behaviour: vertex colours are not in the frame hash, so a colour-only
 *    restyle publishes solely because RmlUi has no way to change a compiled geometry's
 *    colours except by compiling new geometry. See the argument next to
 *    VaCuusHashFrameContent().
 *  - AsyncDecodeWake drives a real PNG through a real decode task, because the wake it
 *    asserts arrives from DrainCompletedDecodes and nothing synthetic ever visits that
 *    drain (VaCuus-akj.6.42(c)).
 *  - LiveReload drives the real UI thread and the real FVaCuusRmlDocumentHost, because a
 *    reload is a sequence of host decisions and the failure it guards -- an edit that never
 *    reaches the screen -- is only visible from the game thread's counters
 *    (VaCuus-akj.6.42(b)).
 *
 * VaCuus.Threading.MultiView carries the matching integration check -- the gate firing
 * on a real static RmlUi document driven by the real UI thread.
 */
namespace VaCuusIdleGateTest
{
/** Any valid geometry payload; nothing here depends on its contents. */
struct FTriangle
{
	Rml::Vertex Vertices[3] = {};
	int Indices[3] = {0, 1, 2};

	FTriangle()
	{
		Vertices[0].position = Rml::Vector2f(10.f, 20.f);
		Vertices[1].position = Rml::Vector2f(30.f, 40.f);
		Vertices[2].position = Rml::Vector2f(50.f, 60.f);
	}

	Rml::Span<const Rml::Vertex> VertexSpan() const { return Rml::Span<const Rml::Vertex>(Vertices, 3); }
	Rml::Span<const int> IndexSpan() const { return Rml::Span<const int>(Indices, 3); }
};

/** 2x2 RGBA payload for GenerateTexture (the font-atlas path; synchronous). */
struct FTexel2x2
{
	Rml::byte Pixels[2 * 2 * 4] = {};

	Rml::Span<const Rml::byte> Span() const { return Rml::Span<const Rml::byte>(Pixels, sizeof(Pixels)); }
	static Rml::Vector2i Dimensions() { return Rml::Vector2i(2, 2); }
};

static const FIntPoint GViewSize(800, 600);

/** The async-decode probe's real size; the placeholder LoadTexture records first is 1x1. */
static const FIntPoint GProbeSize(4, 2);

/** VFS path of the live-reload fixture; see Content/DevUI/Tests/idle_gate_reload.rml. */
static const TCHAR* GReloadFixturePath = TEXT("Tests/idle_gate_reload.rml");

/**
 * Writes an RGBA8 PNG for the async-decode probe. PNG because it is one of the formats
 * LoadTexture whitelists.
 *
 * DELIBERATELY NOT the shared pattern probe VaCuus.Render.Recorder.LoadTexture uses: that one
 * exists so a test can PREDICT the decoded bytes, and this file asserts nothing about bytes --
 * only that a payload of the right size arrived, and only because arrival is what wakes the
 * gate. Sharing it would couple this test to a byte contract it does not make.
 */
static bool SaveIdleGateProbePng(const FString& Path, FIntPoint Size)
{
	TArray<uint8> Pixels;
	Pixels.SetNumUninitialized(Size.X * Size.Y * 4);
	for (int32 Index = 0; Index < Pixels.Num(); ++Index)
	{
		Pixels[Index] = (Index % 4 == 3) ? 255 : uint8(Index * 7 + 3);
	}

	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>("ImageWrapper");
	const TSharedPtr<IImageWrapper> Encoder = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
	if (!Encoder.IsValid() || !Encoder->SetRaw(Pixels.GetData(), Pixels.Num(), Size.X, Size.Y, ERGBFormat::RGBA, 8))
	{
		return false;
	}

	return FFileHelper::SaveArrayToFile(Encoder->GetCompressed(), *Path);
}

/**
 * Runs exactly NumFrames UI frames, one trigger at a time. Triggering N times does NOT give N
 * frames: the wake event is an auto-reset binary latch, so triggers landing while a frame is in
 * flight coalesce. VaCuusCloseDocumentTest.cpp's helper, restated because that file is another
 * test's translation unit.
 */
static bool RunUIFrames(FVaCuusUIThread& UIThread, int32 NumFrames)
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
}	 // namespace VaCuusIdleGateTest

/**
 * The core behaviour: first frame publishes, an identical frame does not, a changed one
 * does -- and the withheld frames neither consume a generation nor leave their commands
 * behind for the next buffer to duplicate.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusIdleGateUnchangedFrameTest, "VaCuus.Render.IdleGate.UnchangedFrame",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusIdleGateUnchangedFrameTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusIdleGateTest;

	FVaCuusRecordingRenderInterface Recorder;
	const FTriangle Triangle;

	// Frame 1. Nothing to compare against, so it publishes whatever it recorded --
	// including, deliberately, the case where a future refactor lets the "previous
	// hash" default collide with a real frame's.
	Recorder.BeginFrame(GViewSize);
	const Rml::CompiledGeometryHandle Geometry = Recorder.CompileGeometry(Triangle.VertexSpan(), Triangle.IndexSpan());
	Recorder.RenderGeometry(Geometry, Rml::Vector2f(1.f, 2.f), Rml::TextureHandle(0));
	const TUniquePtr<FVaCuusCommandBuffer> First = Recorder.EndFrameAndPublish();
	if (!TestNotNull(TEXT("The first frame of a recorder is published"), First.Get()))
	{
		return false;
	}
	TestEqual(TEXT("First publish is generation 1"), int32(First->Generation), 1);
	TestEqual(TEXT("Nothing skipped yet"), int32(Recorder.GetNumFramesSkipped()), 0);

	// Frame 2: the same single draw of geometry that already exists, so no resource
	// traffic either. This is the frame the whole task exists to withhold.
	Recorder.BeginFrame(GViewSize);
	Recorder.RenderGeometry(Geometry, Rml::Vector2f(1.f, 2.f), Rml::TextureHandle(0));
	TestNull(TEXT("An unchanged frame is not published"), Recorder.EndFrameAndPublish().Get());
	TestEqual(TEXT("One frame skipped"), int32(Recorder.GetNumFramesSkipped()), 1);
	TestEqual(TEXT("The skip consumed no generation"), int32(Recorder.GetNumFramesPublished()), 1);

	// Frame 3: same command, different translation.
	Recorder.BeginFrame(GViewSize);
	Recorder.RenderGeometry(Geometry, Rml::Vector2f(3.f, 4.f), Rml::TextureHandle(0));
	const TUniquePtr<FVaCuusCommandBuffer> Second = Recorder.EndFrameAndPublish();
	if (!TestNotNull(TEXT("A frame with changed commands is published"), Second.Get()))
	{
		return false;
	}

	// The generation assertion that pins requirement 3: three frames recorded, two
	// published, so the second publish is generation 2. Generation is documented as a
	// PUBLISH counter and FVaCuusReplayRenderer::ShouldConsume reads it as one; a
	// skipped frame burning a generation would quietly turn it into a frame counter.
	TestEqual(TEXT("Second publish is generation 2, not 3"), int32(Second->Generation), 2);
	TestEqual(TEXT("The skipped frame's commands did not survive into it"), Second->Commands.Num(), 1);

	// Two consecutive skips, then a frame with two draws. If a withheld buffer were left
	// in place instead of dropped, the pending buffer would still hold frames 4 and 5's
	// draws and this one would arrive with four commands, not two.
	for (int32 Index = 0; Index < 2; ++Index)
	{
		Recorder.BeginFrame(GViewSize);
		Recorder.RenderGeometry(Geometry, Rml::Vector2f(3.f, 4.f), Rml::TextureHandle(0));
		TestNull(TEXT("Consecutive unchanged frames are all withheld"), Recorder.EndFrameAndPublish().Get());
	}
	TestEqual(TEXT("Three frames skipped in total"), int32(Recorder.GetNumFramesSkipped()), 3);

	Recorder.BeginFrame(GViewSize);
	Recorder.RenderGeometry(Geometry, Rml::Vector2f(3.f, 4.f), Rml::TextureHandle(0));
	Recorder.RenderGeometry(Geometry, Rml::Vector2f(5.f, 6.f), Rml::TextureHandle(0));
	const TUniquePtr<FVaCuusCommandBuffer> Third = Recorder.EndFrameAndPublish();
	if (TestNotNull(TEXT("A frame after two skips is published"), Third.Get()))
	{
		TestEqual(TEXT("It carries exactly its own commands"), Third->Commands.Num(), 2);
		TestEqual(TEXT("Third publish is generation 3"), int32(Third->Generation), 3);
	}

	return true;
}

/**
 * Resource traffic forces a publish even when the drawing is byte-identical. All four
 * delta arrays are exercised separately: they are four independent ways to lose data,
 * and a gate that checked only the "new" pair would look perfectly healthy while
 * leaking every released resource.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusIdleGateResourceTrafficTest, "VaCuus.Render.IdleGate.ResourceTraffic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusIdleGateResourceTrafficTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusIdleGateTest;

	// The wake predicate on its own, before any recording. It lives on the buffer
	// (FVaCuusCommandBuffer::HasResourceTraffic) so that a future resource array cannot be
	// added without the member-count assert next to it firing; the frames below prove the
	// GATE consults it, and this proves the predicate is not blind to one of the four arrays
	// in a way those frames happen not to reach.
	{
		FVaCuusCommandBuffer Probe;
		TestFalse(TEXT("An empty buffer carries no resource traffic"), Probe.HasResourceTraffic());

		Probe.Commands.AddDefaulted();
		Probe.ViewSize = GViewSize;
		Probe.Generation = 7;
		TestFalse(TEXT("Commands, ViewSize and Generation are not resource traffic"), Probe.HasResourceTraffic());

		const auto ArmsThePredicate = [this](const TCHAR* What, TFunctionRef<void(FVaCuusCommandBuffer&)> Fill)
		{
			FVaCuusCommandBuffer Armed;
			Fill(Armed);
			TestTrue(FString::Printf(TEXT("%s alone arms the predicate"), What), Armed.HasResourceTraffic());
		};
		ArmsThePredicate(TEXT("NewGeometry"), [](FVaCuusCommandBuffer& B) { B.NewGeometry.Add(1); });
		ArmsThePredicate(TEXT("NewTextures"), [](FVaCuusCommandBuffer& B) { B.NewTextures.Add(1); });
		ArmsThePredicate(TEXT("ReleasedGeometry"), [](FVaCuusCommandBuffer& B) { B.ReleasedGeometry.Add(1); });
		ArmsThePredicate(TEXT("ReleasedTextures"), [](FVaCuusCommandBuffer& B) { B.ReleasedTextures.Add(1); });
	}

	FVaCuusRecordingRenderInterface Recorder;
	const FTriangle Triangle;
	const FTexel2x2 Texel;

	// Frame 1 establishes the steady state and the spare handles the release cases
	// retire later. Every frame below draws exactly this one geometry, so the command
	// list -- and therefore the content hash -- never changes again.
	Recorder.BeginFrame(GViewSize);
	const Rml::CompiledGeometryHandle DrawHandle = Recorder.CompileGeometry(Triangle.VertexSpan(), Triangle.IndexSpan());
	const Rml::CompiledGeometryHandle SpareGeometry = Recorder.CompileGeometry(Triangle.VertexSpan(), Triangle.IndexSpan());
	const Rml::TextureHandle SpareTexture = Recorder.GenerateTexture(Texel.Span(), FTexel2x2::Dimensions());
	Recorder.RenderGeometry(DrawHandle, Rml::Vector2f(0.f, 0.f), Rml::TextureHandle(0));
	if (!TestNotNull(TEXT("Steady-state frame published"), Recorder.EndFrameAndPublish().Get()))
	{
		return false;
	}

	// One frame with the invariant command list, plus whatever the case injects.
	const auto RecordFrame = [&Recorder, DrawHandle](TFunctionRef<void()> Inject)
	{
		Recorder.BeginFrame(GViewSize);
		Recorder.RenderGeometry(DrawHandle, Rml::Vector2f(0.f, 0.f), Rml::TextureHandle(0));
		Inject();
		return Recorder.EndFrameAndPublish();
	};
	const auto Nothing = []() {};

	// Arms the gate: proves the command list really is unchanged, so every publish
	// below is attributable to the injected traffic and nothing else.
	TestNull(TEXT("The unchanged frame is withheld before any traffic"), RecordFrame(Nothing).Get());

	// 1. NewGeometry -- geometry created but never drawn this frame. Withholding it
	// would leave the replayer without a resource a later frame draws with.
	{
		const TUniquePtr<FVaCuusCommandBuffer> Buffer = RecordFrame([&Recorder, &Triangle]()
			{ Recorder.CompileGeometry(Triangle.VertexSpan(), Triangle.IndexSpan()); });
		if (TestNotNull(TEXT("NewGeometry forces a publish"), Buffer.Get()))
		{
			TestEqual(TEXT("...carrying the new geometry"), Buffer->NewGeometry.Num(), 1);
			TestEqual(TEXT("...with an unchanged command list"), Buffer->Commands.Num(), 1);
		}
		TestNull(TEXT("The next traffic-free frame is withheld again (NewGeometry)"), RecordFrame(Nothing).Get());
	}

	// 2. NewTextures -- the case that is not hypothetical: this is also the arrival
	// channel for a finished async image decode, drained at the top of BeginFrame(). A
	// gate that ignored it would leave a loaded image transparent forever.
	//
	// GenerateTexture is the SYNCHRONOUS font-atlas producer, so what this case pins is the
	// gate's reading of the array and NOT the decode path that fills it -- the drain is
	// never entered here. VaCuus.Render.IdleGate.AsyncDecodeWake drives that end
	// (VaCuus-akj.6.42(c)); this one stays as the array-level control.
	{
		const TUniquePtr<FVaCuusCommandBuffer> Buffer = RecordFrame([&Recorder, &Texel]()
			{ Recorder.GenerateTexture(Texel.Span(), FTexel2x2::Dimensions()); });
		if (TestNotNull(TEXT("NewTextures forces a publish"), Buffer.Get()))
		{
			TestEqual(TEXT("...carrying the new texture"), Buffer->NewTextures.Num(), 1);
			TestEqual(TEXT("...with an unchanged command list"), Buffer->Commands.Num(), 1);
		}
		TestNull(TEXT("The next traffic-free frame is withheld again (NewTextures)"), RecordFrame(Nothing).Get());
	}

	// 3. ReleasedGeometry -- a release nobody publishes is an RHI resource the replayer
	// holds until the whole view is torn down.
	{
		const TUniquePtr<FVaCuusCommandBuffer> Buffer =
			RecordFrame([&Recorder, SpareGeometry]() { Recorder.ReleaseGeometry(SpareGeometry); });
		if (TestNotNull(TEXT("ReleasedGeometry forces a publish"), Buffer.Get()))
		{
			TestTrue(TEXT("...carrying the retired handle"),
				Buffer->ReleasedGeometry.Contains(FVaCuusGeometryHandle(SpareGeometry)));
			TestEqual(TEXT("...with an unchanged command list"), Buffer->Commands.Num(), 1);
		}
		TestNull(TEXT("The next traffic-free frame is withheld again (ReleasedGeometry)"), RecordFrame(Nothing).Get());
	}

	// 4. ReleasedTextures -- same leak, other resource type.
	{
		const TUniquePtr<FVaCuusCommandBuffer> Buffer =
			RecordFrame([&Recorder, SpareTexture]() { Recorder.ReleaseTexture(SpareTexture); });
		if (TestNotNull(TEXT("ReleasedTextures forces a publish"), Buffer.Get()))
		{
			TestTrue(TEXT("...carrying the retired handle"),
				Buffer->ReleasedTextures.Contains(FVaCuusTextureHandle(SpareTexture)));
			TestEqual(TEXT("...with an unchanged command list"), Buffer->Commands.Num(), 1);
		}
		TestNull(TEXT("The next traffic-free frame is withheld again (ReleasedTextures)"), RecordFrame(Nothing).Get());
	}

	// 1 steady-state + 4 traffic frames published; 5 traffic-free frames withheld.
	TestEqual(TEXT("Five publishes: the steady state and one per delta array"),
		int32(Recorder.GetNumFramesPublished()), 5);
	TestEqual(TEXT("Five withheld frames between them"), int32(Recorder.GetNumFramesSkipped()), 5);

	return true;
}

/**
 * A resize with a byte-identical command list must publish. ViewSize is a property of
 * the BUFFER, not of any command, and it is what sizes the render target the commands
 * are replayed into -- withhold it and the RT keeps its old size while the composite
 * stretches stale pixels into the new rect.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusIdleGateViewSizeTest, "VaCuus.Render.IdleGate.ViewSize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusIdleGateViewSizeTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusIdleGateTest;

	FVaCuusRecordingRenderInterface Recorder;
	const FTriangle Triangle;

	Recorder.BeginFrame(GViewSize);
	const Rml::CompiledGeometryHandle Geometry = Recorder.CompileGeometry(Triangle.VertexSpan(), Triangle.IndexSpan());
	Recorder.RenderGeometry(Geometry, Rml::Vector2f(0.f, 0.f), Rml::TextureHandle(0));
	if (!TestNotNull(TEXT("Frame at the original size published"), Recorder.EndFrameAndPublish().Get()))
	{
		return false;
	}

	// Exactly the same draw at exactly the same size: withheld, which is what makes the
	// resize below the only difference in the frame after it.
	const auto DrawAt = [&Recorder, Geometry](FIntPoint ViewSize)
	{
		Recorder.BeginFrame(ViewSize);
		Recorder.RenderGeometry(Geometry, Rml::Vector2f(0.f, 0.f), Rml::TextureHandle(0));
		return Recorder.EndFrameAndPublish();
	};

	TestNull(TEXT("Same commands at the same size are withheld"), DrawAt(GViewSize).Get());

	const FIntPoint Grown(1024, 768);
	const TUniquePtr<FVaCuusCommandBuffer> Resized = DrawAt(Grown);
	if (TestNotNull(TEXT("Same commands at a NEW size are published"), Resized.Get()))
	{
		TestTrue(TEXT("...carrying the new size"), Resized->ViewSize == Grown);
	}

	TestNull(TEXT("Then the new size settles and is withheld"), DrawAt(Grown).Get());
	TestNotNull(TEXT("Shrinking back is a change too"), DrawAt(GViewSize).Get());

	TestEqual(TEXT("Three publishes: original, grown, shrunk"), int32(Recorder.GetNumFramesPublished()), 3);
	TestEqual(TEXT("Two withheld frames"), int32(Recorder.GetNumFramesSkipped()), 2);

	return true;
}

/**
 * The gate's state is per recorder, and a recorder is created per view
 * (FVaCuusRmlDocumentHost::Initialize) -- so one view going idle cannot suppress another's
 * publish even when the two record byte-identical frames, and a fresh recorder has no hash
 * to inherit from any of them.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusIdleGatePerRecorderTest, "VaCuus.Render.IdleGate.PerRecorder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusIdleGatePerRecorderTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusIdleGateTest;

	const FTriangle Triangle;

	// Two recorders standing in for two views on the one UI thread. Both mint handles
	// from 1, so the same call sequence produces byte-identical frames in both -- which
	// is exactly the situation in which a single shared hash would misfire.
	FVaCuusRecordingRenderInterface Idle;
	FVaCuusRecordingRenderInterface Busy;

	// The first frame of a view: compile the geometry, draw it once.
	const auto FirstFrame = [&Triangle](FVaCuusRecordingRenderInterface& Recorder, Rml::CompiledGeometryHandle& OutGeometry)
	{
		Recorder.BeginFrame(GViewSize);
		OutGeometry = Recorder.CompileGeometry(Triangle.VertexSpan(), Triangle.IndexSpan());
		Recorder.RenderGeometry(OutGeometry, Rml::Vector2f(0.f, 0.f), Rml::TextureHandle(0));
		return Recorder.EndFrameAndPublish();
	};

	/** A later frame: one draw of geometry that already exists, so no resource traffic. */
	const auto DrawFrame = [](FVaCuusRecordingRenderInterface& Recorder, Rml::CompiledGeometryHandle Geometry, float X)
	{
		Recorder.BeginFrame(GViewSize);
		Recorder.RenderGeometry(Geometry, Rml::Vector2f(X, 0.f), Rml::TextureHandle(0));
		return Recorder.EndFrameAndPublish();
	};

	Rml::CompiledGeometryHandle IdleGeometry = 0;
	Rml::CompiledGeometryHandle BusyGeometry = 0;
	const TUniquePtr<FVaCuusCommandBuffer> IdleFirst = FirstFrame(Idle, IdleGeometry);
	const TUniquePtr<FVaCuusCommandBuffer> BusyFirst = FirstFrame(Busy, BusyGeometry);
	if (!TestNotNull(TEXT("Idle view's first frame published"), IdleFirst.Get()) ||
		!TestNotNull(TEXT("Busy view's first frame published"), BusyFirst.Get()))
	{
		return false;
	}
	TestTrue(TEXT("The two views recorded identical content"),
		VaCuusHashFrameContent(*IdleFirst) == VaCuusHashFrameContent(*BusyFirst));

	// Three more frames: one view repeats itself, the other moves every frame.
	for (int32 Index = 1; Index <= 3; ++Index)
	{
		TestNull(TEXT("The idle view keeps withholding"), DrawFrame(Idle, IdleGeometry, 0.f).Get());
		TestNotNull(TEXT("The busy view keeps publishing"), DrawFrame(Busy, BusyGeometry, float(Index)).Get());
	}

	TestEqual(TEXT("Idle view published once"), int32(Idle.GetNumFramesPublished()), 1);
	TestEqual(TEXT("Idle view withheld three frames"), int32(Idle.GetNumFramesSkipped()), 3);
	TestEqual(TEXT("Busy view published every frame"), int32(Busy.GetNumFramesPublished()), 4);
	TestEqual(TEXT("Busy view withheld nothing"), int32(Busy.GetNumFramesSkipped()), 0);

	// A THIRD RECORDER STANDING IN FOR A RESTARTED VIEW -- and labelled honestly, because it
	// proves less than it looks like it does. A freshly constructed recorder has Generation 0
	// and LastPublishedContentHash 0 by NSDMI, so the only thing that can decide its first
	// frame is the `Generation > 0` guard that the UnchangedFrame test above already covers;
	// this cannot fail for any reason specific to a restart. What it adds is that the guard
	// still holds when the content is one a DIFFERENT live recorder has already published --
	// i.e. that the gate's state cannot be reached across recorders even by collision.
	//
	// NOT COVERED HERE, deliberately: that a torn-down-and-recreated VIEW actually gets a new
	// recorder. That is structural (FVaCuusRmlDocumentHost::Initialize constructs one per
	// Initialize call and the host owns it), and driving it would need a real UI thread and a
	// real Rml::Context, which this file has none of on purpose. VaCuus.UMG.Widget exercises
	// the rebuild path through the production host.
	FVaCuusRecordingRenderInterface Restarted;
	Rml::CompiledGeometryHandle RestartedGeometry = 0;
	const TUniquePtr<FVaCuusCommandBuffer> RestartedFirst = FirstFrame(Restarted, RestartedGeometry);
	if (TestNotNull(TEXT("A fresh recorder publishes its first frame"), RestartedFirst.Get()))
	{
		TestTrue(TEXT("...even though its content hashes the same as another recorder's last publish"),
			VaCuusHashFrameContent(*RestartedFirst) == VaCuusHashFrameContent(*IdleFirst));
	}

	return true;
}

/**
 * vacuus.IdleGate 0 turns the short-circuit off, which is the whole point of it existing: the
 * gate's failure mode is a frozen UI with no error, no ensure and no log line, so the first
 * thing anyone will want is to rule it out without a rebuild.
 *
 * Asserted rather than assumed because a kill switch that does not kill is worse than none --
 * it would send whoever flipped it looking somewhere else.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusIdleGateKillSwitchTest, "VaCuus.Render.IdleGate.KillSwitch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusIdleGateKillSwitchTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusIdleGateTest;

	IConsoleVariable* Gate = IConsoleManager::Get().FindConsoleVariable(TEXT("vacuus.IdleGate"));
	if (!TestNotNull(TEXT("vacuus.IdleGate exists"), Gate))
	{
		return false;
	}

	// Restored on every exit path below, including the early returns: this cvar is
	// process-wide and every other test in the suite assumes the gate is armed.
	const int32 Saved = Gate->GetInt();
	ON_SCOPE_EXIT { Gate->Set(Saved, ECVF_SetByCode); };
	TestEqual(TEXT("The gate is on by default"), Saved, 1);

	FVaCuusRecordingRenderInterface Recorder;
	const FTriangle Triangle;

	// Exactly the same frame every time, so the gate is the only thing that can decide.
	Rml::CompiledGeometryHandle Geometry = 0;
	const auto SameFrame = [&Recorder, &Triangle, &Geometry]()
	{
		Recorder.BeginFrame(GViewSize);
		if (Geometry == 0)
		{
			Geometry = Recorder.CompileGeometry(Triangle.VertexSpan(), Triangle.IndexSpan());
		}
		Recorder.RenderGeometry(Geometry, Rml::Vector2f(1.f, 2.f), Rml::TextureHandle(0));
		return Recorder.EndFrameAndPublish();
	};

	// Armed: first frame publishes (it also creates the geometry), the repeat does not.
	TestNotNull(TEXT("First frame publishes"), SameFrame().Get());
	TestNull(TEXT("With the gate on, the repeat is withheld"), SameFrame().Get());
	TestEqual(TEXT("One frame skipped so far"), int32(Recorder.GetNumFramesSkipped()), 1);

	// Disarmed: the identical frame publishes, twice over, and nothing is counted as
	// skipped -- the gate is not "firing and being overridden", it is not firing.
	Gate->Set(0, ECVF_SetByCode);
	TestNotNull(TEXT("With the gate off, an identical frame publishes"), SameFrame().Get());
	TestNotNull(TEXT("...and keeps publishing"), SameFrame().Get());
	TestEqual(TEXT("Nothing new was counted as skipped"), int32(Recorder.GetNumFramesSkipped()), 1);
	TestEqual(TEXT("Three publishes: the first frame and the two forced ones"),
		int32(Recorder.GetNumFramesPublished()), 3);

	// Re-armed mid-stream, which is the case a live toggle actually produces. The hash the
	// gate compares against is the one from the last PUBLISHED frame, and with the gate off
	// that is still being kept up to date -- so it withholds immediately rather than letting
	// one more frame through.
	Gate->Set(1, ECVF_SetByCode);
	TestNull(TEXT("Re-arming withholds the next identical frame at once"), SameFrame().Get());
	TestEqual(TEXT("...counted as the second skip"), int32(Recorder.GetNumFramesSkipped()), 2);

	return true;
}

/**
 * The hash must be deterministic for identical frames, which is why it is built field
 * by field out of FVaCuusCommandHashImage instead of hashing FVaCuusCommand's bytes.
 *
 * This test poisons the padding directly and shows both halves: the rejected
 * implementation (one HashBuffer over the struct) disagrees about two identical
 * commands, and VaCuusHashFrameContent does not.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusIdleGateHashPaddingTest, "VaCuus.Render.IdleGate.HashPadding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusIdleGateHashPaddingTest::RunTest(const FString& Parameters)
{
	// The padding this whole design exists for: Type is a uint8 at offset 0 and the
	// next member starts at 8, so bytes 1..7 of every command are never written by any
	// constructor. Checked here rather than assumed, since the numbers below index them.
	static_assert(offsetof(FVaCuusCommand, Geometry) == 8, "The padding after Type is not where this test writes");

	const auto FillCommand = [](FVaCuusCommand& Command)
	{
		Command.Type = EVaCuusCommandType::DrawGeometry;
		Command.Geometry = 7;
		Command.Texture = 9;
		Command.Translation = FVector2f(11.f, 13.f);
		Command.Scissor = FIntRect(1, 2, 3, 4);
		Command.Transform = FMatrix44f::Identity;
		Command.Transform.M[3][0] = 17.f;
	};

	FVaCuusCommandBuffer Clean;
	FVaCuusCommandBuffer Poisoned;
	Clean.ViewSize = Poisoned.ViewSize = FIntPoint(1920, 1080);
	FillCommand(Clean.Commands.AddDefaulted_GetRef());
	FillCommand(Poisoned.Commands.AddDefaulted_GetRef());

	// Written explicitly on BOTH sides rather than trusting whatever the allocator
	// happened to leave: the point is a guaranteed difference in the padding and
	// nowhere else, and "the compiler probably does not zero it" is not a guarantee.
	uint8* CleanBytes = reinterpret_cast<uint8*>(&Clean.Commands[0]);
	uint8* PoisonedBytes = reinterpret_cast<uint8*>(&Poisoned.Commands[0]);
	for (int32 Index = 1; Index < 8; ++Index)
	{
		CleanBytes[Index] = 0x00;
		PoisonedBytes[Index] = 0xAB;
	}

	// Sanity: without this the two assertions below would both be vacuous.
	TestTrue(TEXT("The two commands differ in their raw bytes"),
		FMemory::Memcmp(CleanBytes, PoisonedBytes, sizeof(FVaCuusCommand)) != 0);

	// The rejected implementation, run for real. If this ever starts passing, the
	// padding stopped being observable and the argument below has to be re-made rather
	// than assumed.
	TestTrue(TEXT("A raw HashBuffer over the struct WOULD have called these frames different"),
		FXxHash64::HashBuffer(CleanBytes, sizeof(FVaCuusCommand)).Hash !=
			FXxHash64::HashBuffer(PoisonedBytes, sizeof(FVaCuusCommand)).Hash);

	// The implementation actually used.
	TestTrue(TEXT("The field-by-field frame hash ignores padding"),
		VaCuusHashFrameContent(Clean) == VaCuusHashFrameContent(Poisoned));

	// Positive controls, so a hash function that returned a constant could not pass.
	//
	// PER SCALAR, NOT PER FIELD GROUP, which is a deliberate upgrade over the obvious
	// version of this test. Translation, Scissor and Transform are copied into
	// FVaCuusCommandHashImage element by element -- nine hand-written assignments in
	// VaCuusHashFrameContent -- so mutating one representative scalar per group only proves
	// the GROUP participates. A copy-paste slip like `Image.Scissor[1] = Command.Scissor.Min.X;`
	// would leave Scissor.Min.Y permanently unhashed, and a test that only ever changed
	// Scissor.Max.Y would still pass while the gate withheld a frame whose clip rect moved.
	// The nine assignments are correct today; these controls are what keeps them so.
	const uint64 Baseline = VaCuusHashFrameContent(Clean);

	const auto TestFieldIsHashed = [this, &Clean, Baseline](const TCHAR* Field, TFunctionRef<void(FVaCuusCommand&)> Mutate)
	{
		FVaCuusCommandBuffer Changed = Clean;
		Mutate(Changed.Commands[0]);
		TestTrue(FString::Printf(TEXT("%s is part of the frame hash"), Field),
			VaCuusHashFrameContent(Changed) != Baseline);
	};

	TestFieldIsHashed(TEXT("Type"), [](FVaCuusCommand& Command) { Command.Type = EVaCuusCommandType::SetTransform; });
	TestFieldIsHashed(TEXT("Geometry"), [](FVaCuusCommand& Command) { Command.Geometry = 8; });
	TestFieldIsHashed(TEXT("Texture"), [](FVaCuusCommand& Command) { Command.Texture = 10; });

	// Translation: both components. FillCommand sets (11, 13), so each of these really is a
	// change to the one scalar named and to nothing else.
	TestFieldIsHashed(TEXT("Translation.X"), [](FVaCuusCommand& Command) { Command.Translation.X = 12.f; });
	TestFieldIsHashed(TEXT("Translation.Y"), [](FVaCuusCommand& Command) { Command.Translation.Y = 14.f; });

	// Scissor: all four corners, in the order they are copied. FillCommand sets
	// FIntRect(1, 2, 3, 4), i.e. Min(1,2) Max(3,4).
	TestFieldIsHashed(TEXT("Scissor.Min.X"), [](FVaCuusCommand& Command) { Command.Scissor.Min.X = -1; });
	TestFieldIsHashed(TEXT("Scissor.Min.Y"), [](FVaCuusCommand& Command) { Command.Scissor.Min.Y = -2; });
	TestFieldIsHashed(TEXT("Scissor.Max.X"), [](FVaCuusCommand& Command) { Command.Scissor.Max.X = 6; });
	TestFieldIsHashed(TEXT("Scissor.Max.Y"), [](FVaCuusCommand& Command) { Command.Scissor.Max.Y = 5; });

	// Transform is a single Memcpy of all 16 cells rather than 16 assignments, so it cannot
	// have the per-element slip above -- but it CAN be given the wrong length or the wrong
	// source. Two cells in different rows, one of them (M[3][0]) a cell FillCommand wrote to
	// a non-default value, so a copy that stopped short of the last row would show up.
	TestFieldIsHashed(TEXT("Transform.M[0][0]"), [](FVaCuusCommand& Command) { Command.Transform.M[0][0] = 2.f; });
	TestFieldIsHashed(TEXT("Transform.M[3][0]"), [](FVaCuusCommand& Command) { Command.Transform.M[3][0] = 23.f; });
	TestFieldIsHashed(TEXT("Transform.M[3][1]"), [](FVaCuusCommand& Command) { Command.Transform.M[3][1] = 19.f; });

	FVaCuusCommandBuffer Resized = Clean;
	Resized.ViewSize = FIntPoint(1920, 1081);
	TestTrue(TEXT("ViewSize is part of the frame hash"), VaCuusHashFrameContent(Resized) != Baseline);

	// Appending a command changes the hash. NOT relabelled idly: this used to claim "the
	// command count is part of the frame hash", which it cannot show. A second command adds
	// another sizeof(FVaCuusCommandHashImage) == 168 bytes to the stream, so the hash moves
	// whether or not the header carries a count, and the assertion passes either way.
	//
	// The header's count field is therefore pinned by NOTHING here. The day this comment
	// used to defer to has since come -- M5's CompositeLayers filter lists made records
	// variable-length, so the count now earns its keep (see VaCuusHashFrameContent) -- but
	// the commands THIS test builds carry no filter list, so the count remains unpinned
	// here and the label above still claims only what it shows. The filter-list bytes have
	// their own controls in VaCuusGlassRecorderTest.cpp; the M5 Shader field's control
	// lives in VaCuusDecoratorTest.cpp.
	FVaCuusCommandBuffer Longer = Clean;
	FillCommand(Longer.Commands.AddDefaulted_GetRef());
	TestTrue(TEXT("Appending a command changes the frame hash"), VaCuusHashFrameContent(Longer) != Baseline);

	// Generation deliberately is NOT: it identifies the buffer, and hashing it would
	// make every frame differ from every other and the gate would never fire.
	FVaCuusCommandBuffer Renumbered = Clean;
	Renumbered.Generation = 4242;
	TestTrue(TEXT("Generation is NOT part of the frame hash"), VaCuusHashFrameContent(Renumbered) == Baseline);

	return true;
}

/**
 * THE ONE GATE TEST WITH A REAL RmlUi CONTEXT IN IT, and the only one that could notice the
 * assumption the gate's completeness actually rests on.
 *
 * Vertex colours are not in the frame hash -- they live in NewGeometry's payloads, which the
 * hash never reads. A `:hover` colour change moves nothing else: same element, same box, same
 * position, so the same commands at the same translation. It publishes anyway, because Rml's
 * render interface has no way to recolour an already-compiled geometry (RenderInterface.h:40-48)
 * and RmlUi therefore releases and re-compiles -- new handle in the command AND resource
 * traffic. The full citation trail is next to VaCuusHashFrameContent().
 *
 * The six tests above drive the recorder directly, so every one of them would keep passing
 * if RmlUi 6.x+1 started re-colouring in place, or if this recorder ever started handing an
 * existing handle back for changed content. This one would not: it asserts the end-to-end
 * property (a colour restyle reaches the render thread) rather than the mechanism.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusIdleGateHoverRecolourTest, "VaCuus.Render.IdleGate.HoverRecolour",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusIdleGateHoverRecolourTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusIdleGateTest;

	// This test boots RmlUi on the test thread and owns it for its duration, which only
	// works while nobody else does (FVaCuusEngine's owner-thread contract).
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

	// Per-context render interface, exactly as FVaCuusRmlDocumentHost::Initialize does it.
	FVaCuusRecordingRenderInterface Recorder;
	const Rml::String ContextName("vacuus_hover_gate_test");
	Rml::Context* Context = Rml::CreateContext(ContextName, Rml::Vector2i(GViewSize.X, GViewSize.Y), &Recorder);
	if (!TestNotNull(TEXT("Context"), Context))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Rml::RemoveContext(ContextName);
	};

	// One absolutely positioned box whose ONLY hover rule is a colour. No size, no offset,
	// no border, no text -- so a hover that produced a different command list would mean
	// RmlUi had changed something this test did not ask for, and the assertions below say so.
	static const TCHAR* Source =
		TEXT("<rml><head><style>")
		TEXT("body{display:block;width:100%;height:100%;}")
		TEXT("#box{display:block;position:absolute;left:20px;top:20px;width:100px;height:60px;background-color:#204080;}")
		TEXT("#box:hover{background-color:#FF8000;}")
		TEXT("</style></head><body><div id=\"box\"/></body></rml>");

	Rml::ElementDocument* Document =
		Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(Source)), "vacuus://hover_test.rml");
	if (!TestNotNull(TEXT("Document"), Document))
	{
		return false;
	}
	Document->Show();

	const auto RecordFrame = [&Recorder, Context]()
	{
		Recorder.BeginFrame(GViewSize);
		Context->Update();
		Context->Render();
		return Recorder.EndFrameAndPublish();
	};

	/** Colour of the first vertex of the first geometry this buffer both creates and draws. */
	const auto DrawnColour = [](const FVaCuusCommandBuffer& Buffer, FColor& OutColour) -> bool
	{
		for (const FVaCuusCommand& Command : Buffer.Commands)
		{
			if (Command.Type != EVaCuusCommandType::DrawGeometry)
			{
				continue;
			}
			if (const FVaCuusGeometryData* Data = Buffer.NewGeometry.Find(Command.Geometry))
			{
				if (Data->Vertices.Num() > 0)
				{
					OutColour = Data->Vertices[0].Color;
					return true;
				}
			}
		}
		return false;
	};

	const TUniquePtr<FVaCuusCommandBuffer> Plain = RecordFrame();
	if (!TestNotNull(TEXT("The unhovered document publishes its first frame"), Plain.Get()))
	{
		return false;
	}

	FColor PlainColour = FColor::Transparent;
	if (!TestTrue(TEXT("...drawing geometry it also created"), DrawnColour(*Plain, PlainColour)))
	{
		return false;
	}

	// Arms the gate. Without this the publish below would prove nothing: a recorder that
	// published everything would pass every assertion after it.
	if (!TestNull(TEXT("A second, identical frame is withheld"), RecordFrame().Get()))
	{
		return false;
	}

	// Into the box (it spans x 20..120, y 20..80). Modifiers 0; the hover chain is rebuilt
	// here and the restyle is applied by the Update() in the next frame.
	Context->ProcessMouseMove(70, 50, 0);

	const TUniquePtr<FVaCuusCommandBuffer> Hovered = RecordFrame();
	if (!TestNotNull(TEXT("A :hover colour change publishes"), Hovered.Get()))
	{
		return false;
	}

	// POSITIVE CONTROL. Without it the publish above could have come from anything -- a
	// relayout, a scrollbar, a stray release -- and the test would pass while the colour
	// change itself went unnoticed. This is the assertion that the pixels really differ.
	FColor HoverColour = FColor::Transparent;
	if (!TestTrue(TEXT("...publishing geometry it also created"), DrawnColour(*Hovered, HoverColour)))
	{
		return false;
	}
	TestTrue(TEXT("...and that geometry really is a different colour"), HoverColour != PlainColour);

	// NOTHING BUT THE COLOUR MOVED, which is what makes this a test of the gate rather than
	// of RmlUi's layout: same number of commands, and a different geometry handle behind them
	// (the re-compile). If the handle ever stopped changing, the two legs of the argument next
	// to VaCuusHashFrameContent() would both be gone and this test would be the only warning.
	TestEqual(TEXT("The hovered frame has the same number of commands"),
		Hovered->Commands.Num(), Plain->Commands.Num());
	TestTrue(TEXT("...and re-compiled its geometry rather than recolouring it in place"),
		Hovered->NewGeometry.Num() > 0 && Hovered->ReleasedGeometry.Num() > 0);

	// THE EXCLUSION THIS TEST POLICES, made concrete instead of argued. Put the hovered
	// colours into the FIRST frame's payload, behind its own unchanged handle, and the hash
	// does not move -- so a colour change that arrived without a new handle and without
	// resource traffic would be withheld, and the screen would keep the old colour for good.
	FVaCuusCommandBuffer Repainted = *Plain;
	for (TPair<FVaCuusGeometryHandle, FVaCuusGeometryData>& Pair : Repainted.NewGeometry)
	{
		for (FVaCuusVertex& Vertex : Pair.Value.Vertices)
		{
			Vertex.Color = HoverColour;
		}
	}
	TestTrue(TEXT("The frame hash alone is blind to vertex colour"),
		VaCuusHashFrameContent(Repainted) == VaCuusHashFrameContent(*Plain));

	// And it settles: the hovered document is static again, so the gate takes over.
	TestNull(TEXT("The hovered state then goes idle again"), RecordFrame().Get());

	return true;
}

/**
 * THE ASYNC DECODE'S ARRIVAL, THROUGH THE DRAIN (VaCuus-akj.6.42(c)).
 *
 * VaCuus.Render.IdleGate.ResourceTraffic already asserts that NewTextures wakes the gate, and
 * its own comment names the case that matters -- "this is also the arrival channel for a
 * finished async image decode, drained at the top of BeginFrame()". But it produces that
 * NewTextures entry with GenerateTexture(), which is the SYNCHRONOUS font-atlas path: it fills
 * the pending buffer from inside the caller's frame and never launches a decode, never queues
 * a completion, and never reaches DrainCompletedDecodes at all. So the one code path the
 * comment is about was the one path not being driven.
 *
 * The difference is not cosmetic. The decode's payload does not arrive from the frame that
 * asked for it -- it arrives from a task, into FVaCuusTextureDecodeSink::Completed, and is
 * moved into the pending buffer by DrainCompletedDecodes at the top of some LATER BeginFrame().
 * That later frame is, by construction, a frame whose drawing did not change: nothing in the
 * document moved, only a picture finished loading. It is exactly the frame the idle gate exists
 * to withhold, and withholding it leaves the loaded image transparent on screen forever with
 * every counter looking healthy.
 *
 * So this drives the real thing: a real PNG on disk, a real LoadTexture, a real wait on the
 * real task, and a command list that is byte-identical across all four frames -- the publish
 * can have come from nothing but the drain.
 *
 * RESTORE-THE-BUG is the gap itself, run and recorded: swap the LoadTexture below for the
 * GenerateTexture the old coverage used (and let the placeholder check through, since the
 * synchronous path installs the real 2x2 immediately) and this reads "Expected 'The drained
 * decode payload wakes the gate' to be not null." There is no decode to drain, so the frame is
 * withheld -- which is the demonstration that the previous shape could not have caught this.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusIdleGateAsyncDecodeWakeTest, "VaCuus.Render.IdleGate.AsyncDecodeWake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusIdleGateAsyncDecodeWakeTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusIdleGateTest;

	// LoadTexture resolves its source through Rml::GetFileInterface(), so the library must be
	// booted -- and it must be OURS to boot: Initialize() while a live UI thread owns RmlUi
	// trips FVaCuusEngine's owner-thread checkf (VaCuusEngine.cpp:250) instead of failing
	// politely. VaCuus.Render.Recorder.LoadTexture states the same precondition for the same
	// reason.
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

	const FString TestDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VaCuusIdleGateTest"));
	const FString PngPath = TestDir / TEXT("idle_gate_probe.png");
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*PngPath);
		IFileManager::Get().DeleteDirectory(*TestDir);
	};
	if (!TestTrue(TEXT("Probe PNG saved"), SaveIdleGateProbePng(PngPath, GProbeSize)))
	{
		return false;
	}

	FVaCuusRecordingRenderInterface Recorder;
	const FTriangle Triangle;

	// ---- Frames 1-2: the steady state, and the proof that the gate is armed. ----
	//
	// The load deliberately does NOT happen yet. DrainCompletedDecodes runs at the top of
	// BeginFrame(), so a frame that opens before LoadTexture is called cannot possibly be
	// carrying a decode -- which is what makes "withheld" here mean the gate, and not luck
	// about how fast a 4x2 PNG decodes.

	Recorder.BeginFrame(GViewSize);
	const Rml::CompiledGeometryHandle DrawHandle = Recorder.CompileGeometry(Triangle.VertexSpan(), Triangle.IndexSpan());
	Recorder.RenderGeometry(DrawHandle, Rml::Vector2f(0.f, 0.f), Rml::TextureHandle(0));
	if (!TestNotNull(TEXT("The steady-state frame published"), Recorder.EndFrameAndPublish().Get()))
	{
		return false;
	}

	Recorder.BeginFrame(GViewSize);
	Recorder.RenderGeometry(DrawHandle, Rml::Vector2f(0.f, 0.f), Rml::TextureHandle(0));
	if (!TestNull(TEXT("An unchanged frame is withheld: the gate is armed"), Recorder.EndFrameAndPublish().Get()))
	{
		return false;
	}

	// ---- Frame 3: the load. Publishes because of the placeholder it records. ----

	Recorder.BeginFrame(GViewSize);
	Rml::Vector2i Dimensions(0, 0);
	const Rml::TextureHandle Texture = Recorder.LoadTexture(Dimensions, Rml::String(TCHAR_TO_UTF8(*PngPath)));
	Recorder.RenderGeometry(DrawHandle, Rml::Vector2f(0.f, 0.f), Texture);
	const TUniquePtr<FVaCuusCommandBuffer> Loading = Recorder.EndFrameAndPublish();

	if (!TestTrue(TEXT("The probe loaded"), Texture != Rml::TextureHandle(0)) ||
		!TestNotNull(TEXT("The loading frame published"), Loading.Get()))
	{
		return false;
	}

	// The placeholder is what proves the decode has NOT landed yet -- if LoadTexture were
	// synchronous the payload would already be here and every assertion below would be
	// measuring the wrong thing. It is also decisive that the decode CANNOT have been drained
	// into this buffer: this frame's BeginFrame ran before LoadTexture existed.
	const FVaCuusTextureData* Placeholder = Loading->NewTextures.Find(FVaCuusTextureHandle(Texture));
	if (!TestNotNull(TEXT("...carrying a placeholder for the handle"), Placeholder))
	{
		return false;
	}
	if (!TestTrue(TEXT("...and the placeholder is 1x1, so the decode is still in flight"),
			Placeholder->Size == FIntPoint(1, 1)))
	{
		return false;
	}
	TestTrue(TEXT("...while the header probe already reports the real size"),
		Dimensions.x == GProbeSize.X && Dimensions.y == GProbeSize.Y);

	// Every frame from here on draws exactly what frame 3 drew, so the content hash never
	// moves again and any publish below is resource traffic by elimination.
	const auto RecordIdenticalFrame = [&Recorder, DrawHandle, Texture]()
	{
		Recorder.BeginFrame(GViewSize);
		Recorder.RenderGeometry(DrawHandle, Rml::Vector2f(0.f, 0.f), Texture);
		return Recorder.EndFrameAndPublish();
	};

	// ---- Frame 4: the wake. ----
	//
	// Deterministic: wait on the decode TASK, then let the next BeginFrame() drain it. The UI
	// thread must never do this (waiting on a decode is the hitch the async path removes) --
	// WaitForTextureDecodes exists for exactly this and is compiled out of shipping builds.
	if (!TestTrue(TEXT("The decode finished within the timeout"),
			Recorder.WaitForTextureDecodes(FTimespan::FromSeconds(30.0))))
	{
		return false;
	}

	const TUniquePtr<FVaCuusCommandBuffer> Woken = RecordIdenticalFrame();
	if (!TestNotNull(TEXT("The drained decode payload wakes the gate"), Woken.Get()))
	{
		return false;
	}

	// POSITIVE CONTROL. A non-null buffer alone would also be produced by a gate that had
	// simply stopped working; this says WHAT woke it, and that the payload really is the
	// decoded image rather than a second placeholder.
	const FVaCuusTextureData* Payload = Woken->NewTextures.Find(FVaCuusTextureHandle(Texture));
	if (TestNotNull(TEXT("...carrying the decoded payload under the same handle"), Payload))
	{
		TestTrue(TEXT("...at the image's real size, not the placeholder's"), Payload->Size == GProbeSize);
		TestEqual(TEXT("...with RGBA8 bytes for every texel"), Payload->RGBA.Num(), GProbeSize.X * GProbeSize.Y * 4);
	}

	// THE POINT OF THE WHOLE TEST: the drawing did not change. This is what makes the publish
	// attributable to the drain and nothing else.
	TestEqual(TEXT("...while the command list is byte-identical to the withheld frame's"), Woken->Commands.Num(), 1);

	// ---- Frame 5: back to sleep. The drain forgot the handle, so nothing is left to wake on,
	// ---- and this is also what makes frame 4's publish attributable to the arrival alone.

	TestNull(TEXT("The next frame is withheld again"), RecordIdenticalFrame().Get());

	TestEqual(TEXT("Three publishes: the steady state, the load's placeholder, the decode's arrival"),
		int32(Recorder.GetNumFramesPublished()), 3);
	TestEqual(TEXT("Two withheld frames, one on each side of the load"),
		int32(Recorder.GetNumFramesSkipped()), 2);

	return true;
}

/**
 * LIVE RELOAD, THROUGH THE GATE (VaCuus-akj.6.42(b)).
 *
 * Six VaCuus.LiveReload.* tests cover the watcher end -- what the debounce does, which files
 * are tracked, how the fan-out re-arms -- and all of them stop at the dispatch. Nothing asserted
 * what happens at the far end of that dispatch, which is where the risk actually is: a reload
 * lands on a view that has gone IDLE, and on an idle view the render target is the only copy of
 * the pixels. Withhold the reload's frame and the designer's edit is invisible -- the old
 * document stays composited, the log says the load succeeded, FramesRecorded keeps climbing,
 * and every counter looks healthy. That is the same failure shape as merge blocker B1, which
 * VaCuus.Render.Close.ClearingFrame now pins for Close() and nothing pinned for reload.
 *
 * DRIVEN THROUGH THE PRODUCTION DOOR, on the real UI thread with the real
 * FVaCuusRmlDocumentHost, observed through the same FVaCuusViewStatus counters the game thread
 * reads. The two enqueues below are, in order, exactly what an editor live reload issues:
 * UVaCuusSubsystem::ClearAssetCachesAndReloadAllViews() puts ONE
 * FVaCuusUIThread::EnqueueClearAssetCaches() in front of the fan-out (VaCuusSubsystem.cpp:307-312)
 * and then UVaCuusView::ReloadDocument() enqueues EnqueueLoadDocumentFile with a bumped load
 * serial (VaCuusView.cpp:202-211). What is NOT driven here is the file watcher itself, which is
 * what those six tests are for.
 *
 * A FILE FIXTURE, NOT AN INLINE STRING, because ReloadDocument() only opens for files: it
 * refuses a view with an empty DocumentPath, and LoadDocumentFromMemory() clears that path
 * deliberately (VaCuusView.cpp:188-191, :226-230). Reloading an inline document is not a thing
 * this system can be asked to do.
 *
 * WHY THE RELOAD MUST PUBLISH, mechanically: FVaCuusRmlDocumentHost::AdoptDocument loads the
 * new document and only then closes the old one (VaCuusRmlDocumentHost.cpp:192-207), so the
 * recorded frame carries the new document's compiled geometry AND the old one's releases. Both
 * are resource traffic, and the draws reference handles the replayer has never seen. A gate that
 * hashed only the drawing would look at two identical-looking box draws and withhold the frame.
 *
 * RESTORE-THE-BUG, run and recorded: drop the EnqueueLoadDocumentFile below (and stand the
 * completion guard down, so the publish assertion is the one that speaks) and this reads
 * "Expected 'A reload onto an idle view publishes' to be true" -- the publish counter frozen
 * exactly where the idle gate left it. From the game thread that is indistinguishable from a
 * reload whose frame was withheld, which is precisely why the counter is the right observable.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusIdleGateLiveReloadTest, "VaCuus.Render.IdleGate.LiveReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusIdleGateLiveReloadTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusIdleGateTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no UI thread to drive"));
		return true;
	}

	// The UI thread boots RmlUi itself and claims ownership of it, so nothing else may hold the
	// library when this starts.
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

	// A real Slate element, never painted: the published buffers simply queue up on it. The
	// production host requires one, and everything under test happens on the UI thread.
	const TSharedRef<FVaCuusSlateElement> Element = MakeShared<FVaCuusSlateElement>();
	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();

	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MakeUnique<FVaCuusRmlDocumentHost>(Element), GViewSize, Status);
	UIThread->EnqueueLoadDocumentFile(ViewId, GReloadFixturePath, /*LoadSerial=*/1);

	// Frame 1 drains the add and the load; frames 2-5 record the same static document and are
	// withheld, which is the state a live reload has to break out of.
	if (!TestTrue(TEXT("UI frames ran"), RunUIFrames(*UIThread, 5)))
	{
		return false;
	}

	if (!TestTrue(TEXT("The fixture loaded from the VFS"),
			Status->LoadCompletedSerial.load(std::memory_order_acquire) == 1 &&
				Status->LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded)))
	{
		return false;
	}

	const uint64 RecordedBefore = Status->FramesRecorded.load(std::memory_order_acquire);
	const uint64 PublishedBefore = Status->FramesPublished.load(std::memory_order_acquire);

	// THE PRECONDITION THAT MAKES THE REST MEAN ANYTHING, and the same one
	// VaCuus.Render.Close.ClearingFrame states: a view that republished every frame would pass
	// the reload assertion below no matter what the gate did.
	if (!TestTrue(TEXT("The idle gate is armed: fewer publishes than recorded frames"),
			PublishedBefore < RecordedBefore))
	{
		return false;
	}

	// ---- The live reload, in the order the dispatcher issues it. ----

	UIThread->EnqueueClearAssetCaches();
	UIThread->EnqueueLoadDocumentFile(ViewId, GReloadFixturePath, /*LoadSerial=*/2);

	if (!TestTrue(TEXT("UI frames ran after the reload"), RunUIFrames(*UIThread, 4)))
	{
		return false;
	}

	if (!TestTrue(TEXT("The reload completed"),
			Status->LoadCompletedSerial.load(std::memory_order_acquire) == 2 &&
				Status->LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded)))
	{
		return false;
	}

	const uint64 PublishedAfterReload = Status->FramesPublished.load(std::memory_order_acquire);

	// THE ASSERTION THE GAP WAS ABOUT. Not "how many" -- the release of the outgoing document's
	// geometry may or may not share a frame with the incoming document's compile, and pinning
	// that would be pinning RmlUi's unload scheduling rather than the gate. What must hold is
	// that a reload onto an IDLE view reaches the render thread at all.
	if (!TestTrue(TEXT("A reload onto an idle view publishes"), PublishedAfterReload > PublishedBefore))
	{
		return false;
	}

	// ---- And the other half: it wakes for the reload and goes straight back to sleep. ----
	//
	// Without this, "publishes" would also be satisfied by a reload that broke the gate
	// outright and left the view republishing its pixels forever -- the opposite bug, and the
	// one that costs a frame's upload per tick for the rest of the session.
	if (!TestTrue(TEXT("More UI frames ran"), RunUIFrames(*UIThread, 4)))
	{
		return false;
	}
	TestEqual(TEXT("...and the gate re-arms immediately afterwards"),
		Status->FramesPublished.load(std::memory_order_acquire), PublishedAfterReload);
	TestTrue(TEXT("...on a view that is still being driven"),
		Status->FramesRecorded.load(std::memory_order_acquire) > PublishedAfterReload);

	UIThread->EnqueueRemoveView(ViewId);
	RunUIFrames(*UIThread, 1);

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
