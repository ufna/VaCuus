// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusCommandBuffer.h"
#include "VaCuusRecordingRenderInterface.h"

#include "HAL/IConsoleManager.h"
#include "Misc/ScopeExit.h"
#include "Templates/Function.h"

#include <RmlUi/Core/Types.h>
#include <RmlUi/Core/Vertex.h>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The idle short-circuit (M2 Task 12): a recorded frame that draws exactly what the
 * render thread already has is NOT published.
 *
 * Driven straight against FVaCuusRecordingRenderInterface, with no Rml::Context in
 * sight, because every property here is about the gate's decision and none of them is
 * about RmlUi. Two consequences worth naming:
 *  - the frames are byte-exact by construction, where a real document's are only
 *    "unchanged as far as anyone can tell";
 *  - "an unchanged frame is not published" needs an observable to be a test at all.
 *    GetNumFramesPublished()/GetNumFramesSkipped() are it: the screen looks identical
 *    whether the gate works or not.
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
	// another sizeof(FVaCuusCommandHashImage) == 112 bytes to the stream, so the hash moves
	// whether or not the header carries a count, and the assertion passes either way.
	//
	// The header's count field is therefore pinned by NOTHING here, and cannot be until the
	// image gains a variable-length field -- which is exactly the reason the count is in the
	// header at all (see VaCuusHashFrameContent). Said out loud rather than left as a label
	// that overstates its evidence.
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

#endif	// WITH_DEV_AUTOMATION_TESTS
