// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusCommandBuffer.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusRecordingRenderInterface.h"
#include "VaCuusStyleSet.h"
#include "VaCuusTestDocumentHost.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include <RmlUi/Core.h>

#include <atomic>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * M5 Task 5b: the material-decorator PRODUCTION tier — what the spike test grew into
 * once the console registry retired. Two tests:
 *
 *  - Material: the recorder path end to end through a real document — `decorator:
 *    shader(<stylekey>)` resolves through the installed snapshot into a Kind=Material
 *    desc + DrawShader; an unknown key refuses listing BOTH tables; and the per-view
 *    forced-republish term with its engine-rate clamp, observed in both directions.
 *  - StyleSet: the registry — snapshot versions monotonic, published snapshots immutable
 *    (by replacement), the named refusals, and unregistration's deferred release under
 *    two LIVE views on the real UI thread (the M3b two-view pattern), snapshot crossing
 *    by the command queue.
 *
 * WHAT THESE HONESTLY OBSERVE under -nullrhi: everything up to and including the
 * recorded buffer and the registry's bookkeeping. Pixels — the material actually
 * evaluating over text — remain the visual protocol's business (vacuus.M5MatSpike +
 * AutoShot; the spike's screenshots in docs/research/proofs/m5-t5-material-spike/).
 * The replay half (proxy resolve, the walk, the PSO) cannot run here at all: the replay
 * pass's first act is three global-shader binds the -nullrhi shader map does not carry
 * (the checkf lesson recorded at the top of the spike test, kept in this comment so
 * nobody re-learns it).
 */
namespace VaCuusMaterialTest
{
static const FIntPoint GViewSize(800, 600);
static const TCHAR* GTranslucentPath = TEXT("/VaCuus/Spike/M_VaCuusSpike_Translucent.M_VaCuusSpike_Translucent");
static const TCHAR* GOpaquePath = TEXT("/VaCuus/Spike/M_VaCuusSpike_Opaque.M_VaCuusSpike_Opaque");
static const TCHAR* GWrongDomainPath = TEXT("/VaCuus/Spike/M_VaCuusSpike_WrongDomain.M_VaCuusSpike_WrongDomain");

/** A transient style set over committed spike fixtures; null when a fixture fails to load. */
static UVaCuusStyleSet* MakeStyleSet(const TCHAR* Key, const TCHAR* MaterialPath)
{
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, MaterialPath);
	if (!Material)
	{
		return nullptr;
	}
	UVaCuusStyleSet* StyleSet = NewObject<UVaCuusStyleSet>(GetTransientPackage());
	StyleSet->Materials.Add(Key, Material);
	return StyleSet;
}

/** Records one frame of a real context through the recorder, decorator-test-style. */
static TUniquePtr<FVaCuusCommandBuffer> RecordContextFrame(FVaCuusRecordingRenderInterface& Recorder, Rml::Context* Context)
{
	Recorder.BeginFrame(GViewSize);
	Context->Update();
	Context->Render();
	return Recorder.EndFrameAndPublish();
}
} // namespace VaCuusMaterialTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMaterialDecoratorTest, "VaCuus.Render.Decorator.Material",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusMaterialDecoratorTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMaterialTest;

	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	IConsoleVariable* Master = IConsoleManager::Get().FindConsoleVariable(TEXT("vacuus.MaterialDecorators"));
	IConsoleVariable* Remedy = IConsoleManager::Get().FindConsoleVariable(TEXT("vacuus.MaterialForcedRepublish"));
	if (!TestNotNull(TEXT("vacuus.MaterialDecorators exists"), Master) ||
		!TestNotNull(TEXT("vacuus.MaterialForcedRepublish exists"), Remedy))
	{
		return false;
	}
	const int32 SavedMaster = Master->GetInt();
	const int32 SavedRemedy = Remedy->GetInt();
	TestEqual(TEXT("vacuus.MaterialDecorators ships default 1 (Task 5b flipped it)"), SavedMaster, 1);
	Master->Set(1, ECVF_SetByCode);
	Remedy->Set(1, ECVF_SetByCode);

	// The production registration path, on the game thread (which is where automation
	// runs): register, then install the minted snapshot on THIS thread — the thread
	// that will drive the recorder, exactly as the UI thread does in production via the
	// queue (the queue crossing itself is the StyleSet test's business).
	UVaCuusStyleSet* StyleSet = MakeStyleSet(TEXT("test-mat"), GTranslucentPath);
	if (!TestNotNull(TEXT("committed MD_UI spike material loads"), StyleSet))
	{
		return false;
	}
	TStrongObjectPtr<UVaCuusStyleSet> StyleSetRoot(StyleSet);
	TestEqual(TEXT("the one entry registers"), FVaCuusStyleRegistry::RegisterStyleSet(StyleSet), 1);
	const TSharedPtr<const FVaCuusStyleSnapshot> Snapshot = FVaCuusStyleRegistry::GetSnapshot_GameThread();
	FVaCuusStyleRegistry::InstallSnapshot(Snapshot);

	ON_SCOPE_EXIT
	{
		FVaCuusStyleRegistry::UnregisterStyleSet(StyleSetRoot.Get());
		FlushRenderingCommands();
		FVaCuusStyleRegistry::TickDeferredReleases_GameThread();
		Master->Set(SavedMaster, ECVF_SetByCode);
		Remedy->Set(SavedRemedy, ECVF_SetByCode);
	};

	const uint64* ExpectedId = Snapshot->KeyToId.Find(TEXT("test-mat"));
	if (!TestNotNull(TEXT("the snapshot carries the key"), ExpectedId))
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
	const Rml::String ContextName("vacuus_material_test");
	Rml::Context* Context = Rml::CreateContext(ContextName, Rml::Vector2i(GViewSize.X, GViewSize.Y), &Recorder);
	if (!TestNotNull(TEXT("Context"), Context))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Rml::RemoveContext(ContextName);
	};

	// The unknown-key refusal must now list BOTH tables — builtins AND the registered
	// style keys — which is the Task 5b half the UnknownBuiltinKey test does not cover.
	AddExpectedMessagePlain(TEXT("registered style keys: test-mat"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("Could not generate decorator element data"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);

	// No text — no font dependency (the decorator tests' rule). #mat resolves; #bad
	// refuses; #plain is the untouched control.
	static const TCHAR* Source =
		TEXT("<rml><head><style>")
		TEXT("body{display:block;width:100%;height:100%;}")
		TEXT("div{display:block;position:absolute;width:200px;height:100px;}")
		TEXT("#mat{left:40px;top:40px;decorator:shader(test-mat);}")
		TEXT("#bad{left:40px;top:180px;decorator:shader(no-such-style-key);}")
		TEXT("#plain{left:40px;top:320px;background-color:#204080;}")
		TEXT("</style></head><body><div id=\"mat\"/><div id=\"bad\"/><div id=\"plain\"/></body></rml>");

	Rml::ElementDocument* Document =
		Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(Source)), "vacuus://material_test.rml");
	if (!TestNotNull(TEXT("Document"), Document))
	{
		return false;
	}
	Document->Show();

	// Frame 1: a view's first frame always publishes. The desc is the production shape.
	const TUniquePtr<FVaCuusCommandBuffer> First = RecordContextFrame(Recorder, Context);
	if (!TestNotNull(TEXT("The first frame publishes"), First.Get()))
	{
		return false;
	}
	TestEqual(TEXT("Exactly one shader compiled — the material; the refused key minted nothing"),
		First->NewShaders.Num(), 1);
	FVaCuusShaderHandle MaterialHandle = 0;
	for (const TPair<FVaCuusShaderHandle, FVaCuusShaderDesc>& Pair : First->NewShaders)
	{
		MaterialHandle = Pair.Key;
		TestEqual(TEXT("Kind is Material"), int32(Pair.Value.Kind), int32(EVaCuusShaderKind::Material));
		TestEqual(TEXT("MaterialId is the snapshot's stable id"), Pair.Value.MaterialId, *ExpectedId);
		TestEqual(TEXT("The key rides along for diagnostics"), Pair.Value.BuiltinKey, FString(TEXT("test-mat")));
		TestTrue(TEXT("Dimensions are the paint box (200x100)"),
			Pair.Value.Dimensions.Equals(FVector2f(200.f, 100.f), 0.5f));
	}
	int32 NumDrawShaders = 0;
	for (const FVaCuusCommand& Command : First->Commands)
	{
		if (Command.Type == EVaCuusCommandType::DrawShader)
		{
			++NumDrawShaders;
			TestEqual(TEXT("The DrawShader names the material handle"), Command.Shader, MaterialHandle);
		}
	}
	TestEqual(TEXT("Exactly one DrawShader"), NumDrawShaders, 1);

	// THE CLAMP, observed from its suppressing side first: frame 2 records inside the
	// SAME engine frame as frame 1's publish (this whole test runs inside one), so the
	// forced-republish term is clamped and the unchanged frame is withheld — at most
	// one material republish per GFrameCounter tick.
	TestNull(TEXT("Unchanged frame in the same engine frame: withheld (the clamp)"),
		RecordContextFrame(Recorder, Context).Get());

	// THE REMEDY: advance the engine frame and the same unchanged content publishes —
	// the per-view forced republish, driven by the recorder's live material table.
	//
	// ++GFrameCounter is a FORWARD-only nudge of the engine's frame stamp (CoreGlobals;
	// the engine loop increments it once per tick) — consumers compare it for equality
	// or monotonicity, so skipping ahead is indistinguishable from a frame having
	// passed, while restoring it backwards is what could confuse a latch. Never
	// restored, for exactly that reason.
	++GFrameCounter;
	TestNotNull(TEXT("Next engine frame: the unchanged frame publishes (the freeze remedy)"),
		RecordContextFrame(Recorder, Context).Get());

	// THE KILL-SWITCH: remedy off, next engine frame — withheld again. This is also the
	// freeze itself made observable: no publish means the replay pass never re-evaluates
	// the material (spec §2(f)).
	Remedy->Set(0, ECVF_SetByCode);
	++GFrameCounter;
	TestNull(TEXT("Remedy off: withheld — the freeze the remedy exists for"),
		RecordContextFrame(Recorder, Context).Get());
	Remedy->Set(1, ECVF_SetByCode);

	// THE FLAG'S OTHER EDGE: closing the document releases the compiled shader
	// (decorator data dies with the element), the live table empties, and the view can
	// idle again — remedy on, engine frame advanced, and still withheld.
	Document->Close();
	const TUniquePtr<FVaCuusCommandBuffer> Closing = RecordContextFrame(Recorder, Context);
	if (TestNotNull(TEXT("The closing frame publishes (release traffic + content change)"), Closing.Get()))
	{
		TestTrue(TEXT("...and carries the material shader's release"), Closing->ReleasedShaders.Contains(MaterialHandle));
	}
	++GFrameCounter;
	TestNull(TEXT("No live material: the idle gate is back in charge"),
		RecordContextFrame(Recorder, Context).Get());

	return true;
}

/**
 * The registry half, including the two-view unregistration run. Version arithmetic is
 * asserted as DELTAS from whatever the process counter already reads — the registry is
 * process-wide and deliberately never resets (regression is the checkf'd bug).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusStyleSetTest, "VaCuus.Render.Decorator.StyleSet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace VaCuusMaterialTest
{
/**
 * A minimal headless document host (the M3b two-view probe's shape, reduced to what
 * this test reads): real context, real recorder — so CompileShader runs the REAL
 * production resolution against the snapshot the UI thread installed from the queue —
 * and one counter of Material descs seen in published buffers.
 */
struct FMaterialProbe
{
	std::atomic<int32> Frames{0};
	std::atomic<int32> MaterialCompiles{0};
	std::atomic<bool> bShutdown{false};
};

class FMaterialProbeHost final : public FVaCuusTestDocumentHost
{
public:
	explicit FMaterialProbeHost(const TSharedRef<FMaterialProbe>& InProbe)
		: FVaCuusTestDocumentHost(TEXT("vacuus_material_probe"), "vacuus://material_probe.rml", Rml::FocusFlag::Auto)
		, Probe(InProbe)
	{
	}

	/** Retained by Shutdown(), the production host's rule: Rml::Shutdown() releases font
	 *  textures through it. */
	virtual Rml::RenderInterface* CreateRenderInterface() override
	{
		Recorder = MakeUnique<FVaCuusRecordingRenderInterface>();
		return Recorder.Get();
	}

	virtual void OnShutdown() override { Probe->bShutdown.store(true, std::memory_order_release); }

	virtual void SetVisible(bool bVisible) override
	{
		check(FVaCuusUIThread::IsInUIThread());
		if (RmlDocument)
		{
			bVisible ? RmlDocument->Show() : RmlDocument->Hide();
		}
	}

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());
		Recorder->BeginFrame(ViewSize);
		Context->Update();
		Context->Render();
		if (const TUniquePtr<FVaCuusCommandBuffer> Buffer = Recorder->EndFrameAndPublish())
		{
			for (const TPair<FVaCuusShaderHandle, FVaCuusShaderDesc>& Pair : Buffer->NewShaders)
			{
				if (Pair.Value.Kind == EVaCuusShaderKind::Material)
				{
					Probe->MaterialCompiles.fetch_add(1, std::memory_order_relaxed);
				}
			}
		}
		Probe->Frames.fetch_add(1, std::memory_order_release);
		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

private:
	TSharedRef<FMaterialProbe> Probe;
	TUniquePtr<FVaCuusRecordingRenderInterface> Recorder;
};

/** One-at-a-time frame stepping — the M3b pattern (the wake event coalesces, so N triggers != N frames). */
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
		// The forced-republish clamp is per engine frame; advancing the stamp between
		// steps keeps the material views publishing the way live engine frames would.
		++GFrameCounter;
	}
	return true;
}
} // namespace VaCuusMaterialTest

bool FVaCuusStyleSetTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMaterialTest;

	// ---- The registry's bookkeeping: versions, immutability, refusals. -----------------

	const uint64 V0 = FVaCuusStyleRegistry::GetVersion_GameThread();
	const int32 E0 = FVaCuusStyleRegistry::GetNumEntries_GameThread();

	UVaCuusStyleSet* SetA = MakeStyleSet(TEXT("styleset-test-a"), GTranslucentPath);
	UVaCuusStyleSet* SetB = MakeStyleSet(TEXT("styleset-test-b"), GOpaquePath);
	UVaCuusStyleSet* SetWrong = MakeStyleSet(TEXT("styleset-test-wrong"), GWrongDomainPath);
	UVaCuusStyleSet* SetShadow = MakeStyleSet(TEXT("glass-panel"), GTranslucentPath);
	if (!TestNotNull(TEXT("fixture A loads"), SetA) || !TestNotNull(TEXT("fixture B loads"), SetB) ||
		!TestNotNull(TEXT("wrong-domain fixture loads"), SetWrong) || !TestNotNull(TEXT("shadow fixture loads"), SetShadow))
	{
		return false;
	}
	TStrongObjectPtr<UVaCuusStyleSet> RootA(SetA), RootB(SetB), RootWrong(SetWrong), RootShadow(SetShadow);

	ON_SCOPE_EXIT
	{
		// Idempotent for whatever a mid-test failure left behind.
		FVaCuusStyleRegistry::UnregisterStyleSet(RootA.Get());
		FVaCuusStyleRegistry::UnregisterStyleSet(RootB.Get());
		FlushRenderingCommands();
		FVaCuusStyleRegistry::TickDeferredReleases_GameThread();
	};

	TestEqual(TEXT("A registers"), FVaCuusStyleRegistry::RegisterStyleSet(SetA), 1);
	const TSharedPtr<const FVaCuusStyleSnapshot> SnapA = FVaCuusStyleRegistry::GetSnapshot_GameThread();
	if (!TestTrue(TEXT("a snapshot exists"), SnapA.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("registration bumped the version by one"), SnapA->Version, V0 + 1);
	TestTrue(TEXT("A's key is in it"), SnapA->KeyToId.Contains(TEXT("styleset-test-a")));

	TestEqual(TEXT("B registers"), FVaCuusStyleRegistry::RegisterStyleSet(SetB), 1);
	const TSharedPtr<const FVaCuusStyleSnapshot> SnapB = FVaCuusStyleRegistry::GetSnapshot_GameThread();
	TestEqual(TEXT("second registration: version + 1 again"), SnapB->Version, V0 + 2);
	TestTrue(TEXT("both keys present"),
		SnapB->KeyToId.Contains(TEXT("styleset-test-a")) && SnapB->KeyToId.Contains(TEXT("styleset-test-b")));
	TestNotEqual(TEXT("stable ids are distinct"),
		SnapB->KeyToId[TEXT("styleset-test-a")], SnapB->KeyToId[TEXT("styleset-test-b")]);

	// IMMUTABILITY BY REPLACEMENT (the spec's observable, from the consumer's side): the
	// snapshot handed out for A is bit-for-bit what it was — B's registration minted a
	// NEW table instead of growing A's.
	TestEqual(TEXT("A's snapshot version did not move"), SnapA->Version, V0 + 1);
	TestEqual(TEXT("A's snapshot still has exactly its one key"), SnapA->KeyToId.Num(), 1);

	// The wrong-domain refusal, named (the spike's error text, now at registration).
	// AddExpectedError, not AddExpectedMessagePlain: these land at Error severity, and
	// this is the codebase's proven way to both match and un-fail one.
	AddExpectedError(TEXT("is not a User Interface"), EAutomationExpectedErrorFlags::Contains, 1);
	TestEqual(TEXT("a surface-domain material registers nothing"), FVaCuusStyleRegistry::RegisterStyleSet(SetWrong), 0);
	TestFalse(TEXT("...and its key is nowhere"),
		FVaCuusStyleRegistry::GetSnapshot_GameThread()->KeyToId.Contains(TEXT("styleset-test-wrong")));

	// A key that shadows a builtin is refused where it can be named — builtins win at
	// CompileShader, so accepting it would register a key that can never draw.
	AddExpectedError(TEXT("shadows a builtin shader key"), EAutomationExpectedErrorFlags::Contains, 1);
	TestEqual(TEXT("a builtin-shadowing key registers nothing"), FVaCuusStyleRegistry::RegisterStyleSet(SetShadow), 0);

	// A material with customized UVs is refused too (PR #1's boundary, bead VaCuus-x3k):
	// the replay pass has no material vertex stage to run them in. A transient UMaterial
	// is enough — the check reads the DECLARED count (Material.h:608-610), so nothing has
	// to compile, and this leg runs under -nullrhi like the two refusals above.
	UMaterial* Customized = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
	Customized->MaterialDomain = EMaterialDomain::MD_UI;
	Customized->NumCustomizedUVs = 1;
	UVaCuusStyleSet* SetCustomized = NewObject<UVaCuusStyleSet>(GetTransientPackage());
	SetCustomized->Materials.Add(TEXT("styleset-test-customized"), Customized);
	TStrongObjectPtr<UVaCuusStyleSet> RootCustomized(SetCustomized);
	AddExpectedError(TEXT("customized UV"), EAutomationExpectedErrorFlags::Contains, 1);
	TestEqual(TEXT("a customized-UV material registers nothing"), FVaCuusStyleRegistry::RegisterStyleSet(SetCustomized), 0);
	TestFalse(TEXT("...and its key is nowhere"),
		FVaCuusStyleRegistry::GetSnapshot_GameThread()->KeyToId.Contains(TEXT("styleset-test-customized")));

	// ---- The two-view unregistration run (the M3b pattern): snapshot over the queue,
	// ---- deferred release under live views, no crash. ----------------------------------

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Two-view half skipped: no multithreading support, so there is no worker thread to drive"));
		FVaCuusStyleRegistry::UnregisterStyleSet(SetA);
		FVaCuusStyleRegistry::UnregisterStyleSet(SetB);
		return true;
	}
	if (!TestFalse(TEXT("RmlUi is down before the two-view half"), FVaCuusEngine::Get().IsInitialized()))
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

	// The document both views show: A's key resolves (registered BEFORE the loads — the
	// snapshot crossed the queue when A registered, and GetOrStartUIThread re-published
	// the current one at boot ahead of everything this test enqueues).
	static const TCHAR* ProbeSource =
		TEXT("<rml><head><style>")
		TEXT("body{display:block;width:100%;height:100%;}")
		TEXT("#m{display:block;position:absolute;left:10px;top:10px;width:100px;height:60px;")
		TEXT("decorator:shader(styleset-test-a);}")
		TEXT("</style></head><body><div id=\"m\"/></body></rml>");

	const TSharedRef<FMaterialProbe> ProbeA = MakeShared<FMaterialProbe>();
	const TSharedRef<FMaterialProbe> ProbeB = MakeShared<FMaterialProbe>();
	const uint32 ViewA = UIThread->AllocateViewId();
	const uint32 ViewB = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewA, MakeUnique<FMaterialProbeHost>(ProbeA), FIntPoint(320, 240), MakeShared<FVaCuusViewStatus>());
	UIThread->EnqueueAddView(ViewB, MakeUnique<FMaterialProbeHost>(ProbeB), FIntPoint(320, 240), MakeShared<FVaCuusViewStatus>());
	UIThread->EnqueueLoadDocumentFromMemory(ViewA, ProbeSource, /*LoadSerial=*/1);
	UIThread->EnqueueLoadDocumentFromMemory(ViewB, ProbeSource, /*LoadSerial=*/1);

	if (!TestTrue(TEXT("frames run"), RunFrames(*UIThread, 3)))
	{
		return false;
	}
	TestTrue(TEXT("view A compiled the material desc through the queued snapshot"),
		ProbeA->MaterialCompiles.load(std::memory_order_relaxed) >= 1);
	TestTrue(TEXT("view B compiled the material desc through the queued snapshot"),
		ProbeB->MaterialCompiles.load(std::memory_order_relaxed) >= 1);

	// UNREGISTER UNDER LIVE VIEWS: both documents keep their compiled handles (RmlUi
	// holds a compiled shader until release) and keep recording DrawShader commands for
	// them; the registry parks A's root behind the fence. Nothing may crash, and the
	// views must keep running frames. (Flush+tick first so any stray pending release
	// from elsewhere in the process cannot blur the 0 -> 1 -> 0 arc asserted here.)
	FlushRenderingCommands();
	FVaCuusStyleRegistry::TickDeferredReleases_GameThread();
	FVaCuusStyleRegistry::UnregisterStyleSet(SetA);
	TestEqual(TEXT("the unregistered root is fence-parked, not dropped"),
		FVaCuusStyleRegistry::GetNumPendingReleases_GameThread(), 1);

	const int32 FramesABefore = ProbeA->Frames.load(std::memory_order_acquire);
	if (!TestTrue(TEXT("frames still run after the unregister"), RunFrames(*UIThread, 3)))
	{
		return false;
	}
	TestTrue(TEXT("view A kept recording"), ProbeA->Frames.load(std::memory_order_acquire) > FramesABefore);

	// One view retires, the other keeps going — the two-view pattern's point.
	UIThread->EnqueueRemoveView(ViewA);
	const int32 FramesBBefore = ProbeB->Frames.load(std::memory_order_acquire);
	if (!TestTrue(TEXT("frames still run after removing view A"), RunFrames(*UIThread, 2)))
	{
		return false;
	}
	TestTrue(TEXT("view A shut down"), ProbeA->bShutdown.load(std::memory_order_acquire));
	TestTrue(TEXT("view B kept recording"), ProbeB->Frames.load(std::memory_order_acquire) > FramesBBefore);

	// The deferred release completes once the render thread has passed the mirror
	// replacement — the fence the unregister began.
	FlushRenderingCommands();
	FVaCuusStyleRegistry::TickDeferredReleases_GameThread();
	TestEqual(TEXT("the fence completed and the root dropped"),
		FVaCuusStyleRegistry::GetNumPendingReleases_GameThread(), 0);

	FVaCuusStyleRegistry::UnregisterStyleSet(SetB);
	TestEqual(TEXT("entry count is back where it started"),
		FVaCuusStyleRegistry::GetNumEntries_GameThread(), E0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
