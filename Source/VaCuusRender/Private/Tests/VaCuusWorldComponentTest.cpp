// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusCommandBuffer.h"
#include "VaCuusSubsystem.h"
#include "VaCuusUIThread.h"
#include "VaCuusView.h"
#include "VaCuusWorldComponent.h"
#include "VaCuusWorldSink.h"
#include "VaCuusWorldSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformProcess.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/App.h"
#include "RenderingThread.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusWorldComponentTest
{
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
 * A textless document (no font dependency in a headless suite) with one colored div.
 * The explicit display:block on BOTH elements is load-bearing, not style: RmlUi has
 * no default display for arbitrary tags, so without it nothing lays out, nothing
 * records, and every frame hashes empty-equal -- the idle gate then withholds the
 * very publishes these tests count (observed: the first version of this document
 * omitted display and the heal-probe publish never came). The proven shape copied
 * here is VaCuusCloseDocumentTest.cpp:52-55's "geometry to lose" document.
 */
static const TCHAR* GTinyDocument =
	TEXT("<rml><head><style>body{display:block;width:100%;height:100%;}")
	TEXT("div{display:block;position:absolute;left:4px;top:4px;width:40px;height:16px;background-color:#d04030;}")
	TEXT("</style></head><body><div/></body></rml>");

/**
 * The shared scaffold: a standalone game instance (real EWorldType::Game world, so
 * IsGameWorld() and both subsystems are the production ones -- the
 * VaCuus.UMG.Widget precedent, argued at length there) plus the UI thread.
 */
struct FWorldFixture
{
	TStrongObjectPtr<UGameInstance> GameInstance;

	UWorld* GetWorld() const { return GameInstance ? GameInstance->GetWorld() : nullptr; }

	bool Init(FAutomationTestBase& Test)
	{
		GameInstance = TStrongObjectPtr<UGameInstance>(NewObject<UGameInstance>(GEngine));
		GameInstance->InitializeStandalone();
		return Test.TestNotNull(TEXT("The standalone game instance has a world"), GetWorld());
	}

	~FWorldFixture()
	{
		if (GameInstance)
		{
			UWorld* World = GameInstance->GetWorld();
			GameInstance->Shutdown();
			if (World)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(/*bInformEngineOfWorld=*/false);
			}
		}
	}
};

/** Every test's entry gate; returns false with the skip already logged. */
static bool SuiteViable(FAutomationTestBase& Test)
{
	if (!FPlatformProcess::SupportsMultithreading())
	{
		Test.AddInfo(TEXT("Skipped: no multithreading support, so there is no worker thread to drive"));
		return false;
	}
	if (!FSlateApplication::IsInitialized())
	{
		Test.AddInfo(TEXT("Skipped: no FSlateApplication, so a game instance cannot be initialized"));
		return false;
	}
	return GEngine != nullptr;
}
}	 // namespace VaCuusWorldComponentTest

/**
 * The component's whole lifecycle in a minimal real game world: the zero-size named
 * refusal (spec 2(h)), view creation with DrawSize up front, the render target's
 * non-sRGB contract, the MID's texture binding, copy-on-publish, the WS-COPY-COST
 * idle half (~0 copies while the idle gate withholds), and teardown leaving nothing
 * behind. This is the headless stand-in for a PIE smoke: the world is a real
 * EWorldType::Game world and the registration path is the production one; what it
 * cannot exercise is a player, a camera or a scene draw -- Task 7's PIE input test
 * covers that end.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusWorldComponentLifecycleTest, "VaCuus.World.ComponentLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusWorldComponentLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusWorldComponentTest;
	if (!SuiteViable(*this))
	{
		return true;
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

	FWorldFixture Fixture;
	if (!Fixture.Init(*this))
	{
		return false;
	}
	UWorld* World = Fixture.GetWorld();

	AActor* Actor = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Host actor"), Actor))
	{
		return false;
	}

	// 1. THE NAMED REFUSAL: degenerate DrawSize registers the component but creates
	// no view and says so exactly once, as an Error.
	{
		UVaCuusWorldComponent* Refused = NewObject<UVaCuusWorldComponent>(Actor);
		Refused->DrawSize = FIntPoint(0, 0);
		Refused->bAutoLoadDocument = false;
		AddExpectedError(TEXT("refused: degenerate DrawSize"), EAutomationExpectedErrorFlags::Contains, 1);
		Refused->RegisterComponentWithWorld(World);

		TestNull(TEXT("A refused panel has no view"), Refused->GetView());
		TestFalse(TEXT("...and no sink"), Refused->GetWorldSink().IsValid());
		if (UVaCuusWorldSubsystem* WorldSubsystem = World->GetSubsystem<UVaCuusWorldSubsystem>())
		{
			TestEqual(TEXT("...and did not join the input roster"), WorldSubsystem->GetWorldComponents().Num(), 0);
		}
		Refused->UnregisterComponent();
		Refused->DestroyComponent();
	}

	// 2. A valid panel: view up at DrawSize with no first-tick dance, RT and MID
	// wired, roster joined.
	UVaCuusWorldComponent* Component = NewObject<UVaCuusWorldComponent>(Actor);
	Component->DrawSize = FIntPoint(128, 64);
	Component->bAutoLoadDocument = false;
	Component->RegisterComponentWithWorld(World);

	UVaCuusView* View = Component->GetView();
	if (!TestNotNull(TEXT("Registering in a game world created a view"), View))
	{
		return false;
	}
	TestTrue(TEXT("The view handle is valid"), View->IsViewValid());
	if (!TestTrue(TEXT("UI frames ran"), RunFrames(*UIThread, 2)))
	{
		return false;
	}
	TestEqual(TEXT("The UI thread has exactly one view"), UIThread->GetNumViews(), 1);

	UTextureRenderTarget2D* RenderTarget = Component->GetRenderTarget();
	if (TestNotNull(TEXT("The render target exists at registration"), RenderTarget))
	{
		TestEqual(TEXT("...at DrawSize"), FIntPoint(RenderTarget->SizeX, RenderTarget->SizeY), FIntPoint(128, 64));
		// The non-sRGB half of the copy contract: the pixels are display-encoded
		// already (replay RT, no TexCreate_SRGB) and the gamma decision belongs to
		// the preset's graph, not the sampler (WS-GAMMA).
		TestFalse(TEXT("...and NOT sRGB-tagged"), RenderTarget->IsSRGB());
	}

	UMaterialInstanceDynamic* Mid = Component->GetMaterialInstance();
	if (TestNotNull(TEXT("The MID over the preset exists"), Mid))
	{
		TestEqual(TEXT("The MID's VaCuusUI parameter is the render target"),
			Cast<UTexture>(Mid->K2_GetTextureParameterValue(TEXT("VaCuusUI"))), Cast<UTexture>(RenderTarget));
	}

	if (UVaCuusWorldSubsystem* WorldSubsystem = World->GetSubsystem<UVaCuusWorldSubsystem>())
	{
		TestEqual(TEXT("The panel joined the world roster (Task 7's seam)"), WorldSubsystem->GetWorldComponents().Num(), 1);
	}

	// 3. Copy-on-publish: a loaded document publishes, the sink replays on arrival
	// and copies once per published frame.
	TSharedPtr<FVaCuusWorldSink> Sink = Component->GetWorldSink();
	if (!TestTrue(TEXT("The sink exists"), Sink.IsValid()))
	{
		return false;
	}

	View->LoadDocumentFromMemory(GTinyDocument);
	if (!TestTrue(TEXT("UI frames ran for the load"), RunFrames(*UIThread, 3)))
	{
		return false;
	}
	FlushRenderingCommands();

	const uint64 ArrivalsAfterLoad = Sink->GetNumArrivals();
	const uint64 CopiesAfterLoad = Sink->GetNumCopies();
	TestTrue(TEXT("Published buffers reached the world sink"), ArrivalsAfterLoad >= 1);

	// The copy needs the render target's RHI TEXTURE, and under the suite's -nullrhi
	// there is none to need: UTexture::UpdateResource creates no resource at all when
	// the app can never render (Texture.cpp:336-339), so the slot update delivered
	// null and the sink parked. That is the production dedicated-server behavior, not
	// a gap in the sink -- the texture-level copy/skip/heal discipline is asserted in
	// VaCuus.World.SinkDiscipline on raw RHI textures (null-RHI-viable), and THESE
	// assertions run whenever the suite runs on a real RHI.
	const bool bTextureResourcesExist = FApp::CanEverRender();
	if (bTextureResourcesExist)
	{
		TestTrue(TEXT("Each publish copied into the render target (extents match)"), CopiesAfterLoad >= 1);
		TestEqual(TEXT("No extent skips on a static size"), int64(Sink->GetNumExtentSkips()), int64(0));
	}
	else
	{
		TestEqual(TEXT("Without texture resources (null RHI) the sink parks: no copies"),
			int64(CopiesAfterLoad), int64(0));
		AddInfo(TEXT("Copy-on-publish assertions deferred to a real-RHI run (FApp::CanEverRender() is false)"));
	}

	// 4. THE IDLE HALF OF WS-COPY-COST, asserted exactly: a static document's frames
	// are withheld by the idle gate, so NOTHING arrives and NOTHING is copied --
	// not "few", zero. The arrival count is the RHI-independent half; the copy count
	// rides it one-to-one on a real RHI (step 3).
	if (!TestTrue(TEXT("Idle UI frames ran"), RunFrames(*UIThread, 20)))
	{
		return false;
	}
	FlushRenderingCommands();
	TestEqual(TEXT("An idle panel arrives no buffers"), int64(Sink->GetNumArrivals()), int64(ArrivalsAfterLoad));
	TestEqual(TEXT("...and costs zero copies"), int64(Sink->GetNumCopies()), int64(CopiesAfterLoad));

	// 5. Runtime SetDrawSize shares the named refusal; the size stays put.
	AddExpectedError(TEXT("refused SetDrawSize"), EAutomationExpectedErrorFlags::Contains, 1);
	Component->SetDrawSize(FIntPoint(0, 64));
	TestEqual(TEXT("A refused SetDrawSize changes nothing"), Component->GetCurrentDrawSize(), FIntPoint(128, 64));

	// 6. Teardown mirrors RetireView: no view survives the component.
	Component->UnregisterComponent();
	TestNull(TEXT("Unregistering forgets the view"), Component->GetView());
	if (TestTrue(TEXT("UI frames ran after the unregister"), RunFrames(*UIThread, 2)))
	{
		TestEqual(TEXT("The UI thread has no views left"), UIThread->GetNumViews(), 0);
	}
	if (UVaCuusWorldSubsystem* WorldSubsystem = World->GetSubsystem<UVaCuusWorldSubsystem>())
	{
		TestEqual(TEXT("The roster is empty again"), WorldSubsystem->GetWorldComponents().Num(), 0);
	}
	FlushRenderingCommands();

	return true;
}

/**
 * Spec 2(g)'s destination-slot discipline, driven deterministically on a bare sink
 * (no component, no view -- the render commands ARE the contract):
 *
 *   dest(64) -> publish(64)  : copy          -- the normal path
 *   publish(48)              : extent SKIP   -- the UI half of a resize landed first
 *   dest(48)                 : copy          -- the slot update REPAINTS from the
 *                                              persistent OutputRT; without this, an
 *                                              idle view's fresh destination would
 *                                              stay blank forever (the idle gate
 *                                              sends no more publishes)
 *   publish(48)              : copy          -- self-healed steady state
 *
 * Totals: 3 arrivals, 3 copies, 1 skip. Every mutation is a single render command
 * stream, so the interleaving is exact, not raced.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusWorldSinkDisciplineTest, "VaCuus.World.SinkDiscipline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusWorldSinkDisciplineTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusWorldComponentTest;
	if (!SuiteViable(*this))
	{
		return true;
	}

	const TSharedRef<FVaCuusWorldSink> Sink = MakeShared<FVaCuusWorldSink>();

	ENQUEUE_RENDER_COMMAND(VaCuusWorldSinkDiscipline)(
		[Sink](FRHICommandListImmediate& RHICmdList)
		{
			const auto MakeDest = [&RHICmdList](FIntPoint Size)
			{
				const FRHITextureCreateDesc Desc =
					FRHITextureCreateDesc::Create2D(TEXT("VaCuusWorldSinkTestDest"), Size, PF_B8G8R8A8)
						.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource)
						.SetInitialState(ERHIAccess::SRVMask);
				return RHICmdList.CreateTexture(Desc);
			};
			const auto MakeBuffer = [](uint64 Generation, FIntPoint Size)
			{
				TUniquePtr<FVaCuusCommandBuffer> Buffer = MakeUnique<FVaCuusCommandBuffer>();
				Buffer->Generation = Generation;
				Buffer->ViewSize = Size;
				return Buffer;
			};

			// Destination before any replay: harmless no-copy (no OutputRT yet).
			Sink->SetDestination_RenderThread(RHICmdList, MakeDest(FIntPoint(64, 64)));

			Sink->SetPendingBuffer_RenderThread(RHICmdList, MakeBuffer(1, FIntPoint(64, 64)));
			Sink->SetPendingBuffer_RenderThread(RHICmdList, MakeBuffer(2, FIntPoint(48, 48)));
			Sink->SetDestination_RenderThread(RHICmdList, MakeDest(FIntPoint(48, 48)));
			Sink->SetPendingBuffer_RenderThread(RHICmdList, MakeBuffer(3, FIntPoint(48, 48)));

			Sink->ReleaseResources_RenderThread();
		});
	FlushRenderingCommands();

	TestEqual(TEXT("Three buffers arrived"), int64(Sink->GetNumArrivals()), int64(3));
	TestEqual(TEXT("Three copies: publish(64), the dest(48) slot-update repaint, publish(48)"),
		int64(Sink->GetNumCopies()), int64(3));
	TestEqual(TEXT("One extent skip: publish(48) against the 64x64 destination"),
		int64(Sink->GetNumExtentSkips()), int64(1));

	return true;
}

/**
 * The resize race on the real pipeline: rapid DrawSize churn while the view keeps
 * publishing. Every interleaving of the three streams (UI-thread publishes,
 * game-thread RT re-inits, game-thread slot-update enqueues) must land on the
 * extent guard rather than a stale-pointer copy -- no crash, and after the churn
 * settles the first matching publish heals the panel (copies grow again at the
 * final size). Skip counts are reported, not asserted: how many publishes lose the
 * race is timing, the invariant is that losing it is always a SKIP.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusWorldResizeRaceTest, "VaCuus.World.ResizeRace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusWorldResizeRaceTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusWorldComponentTest;
	if (!SuiteViable(*this))
	{
		return true;
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

	FWorldFixture Fixture;
	if (!Fixture.Init(*this))
	{
		return false;
	}
	UWorld* World = Fixture.GetWorld();

	AActor* Actor = World->SpawnActor<AActor>();
	UVaCuusWorldComponent* Component = NewObject<UVaCuusWorldComponent>(Actor);
	Component->DrawSize = FIntPoint(96, 64);
	Component->bAutoLoadDocument = false;
	Component->RegisterComponentWithWorld(World);

	UVaCuusView* View = Component->GetView();
	TSharedPtr<FVaCuusWorldSink> Sink = Component->GetWorldSink();
	if (!TestNotNull(TEXT("View"), View) || !TestTrue(TEXT("Sink"), Sink.IsValid()))
	{
		return false;
	}

	// Copy counters need texture resources; under -nullrhi UTextures have none
	// (Texture.cpp:336-339 -- the lifecycle test's step 3 argues this in full), so
	// there the race still proves "no crash, arrivals flow, sizes settle" while the
	// guard/heal counters light up on a real-RHI run.
	const bool bTextureResourcesExist = FApp::CanEverRender();

	View->LoadDocumentFromMemory(GTinyDocument);
	if (!TestTrue(TEXT("UI frames ran for the load"), RunFrames(*UIThread, 3)))
	{
		return false;
	}
	FlushRenderingCommands();
	TestTrue(TEXT("The panel published before the churn"), Sink->GetNumArrivals() >= 1);
	if (bTextureResourcesExist)
	{
		TestTrue(TEXT("...and copied before the churn"), Sink->GetNumCopies() >= 1);
	}

	// The churn: each step re-inits the RT and enqueues its slot update on THIS
	// thread while the UI thread relayouts and publishes on its own clock. One UI
	// frame per step keeps publishes flowing mid-churn instead of after it.
	const FIntPoint Sizes[] = {{128, 64}, {96, 96}, {160, 120}, {120, 80}, {200, 100}, {160, 120}};
	for (const FIntPoint& Size : Sizes)
	{
		Component->SetDrawSize(Size);
		if (!TestTrue(TEXT("UI frame ran mid-churn"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
	}

	// Settle: drain the queued resizes, then prove the pipe healed by forcing one
	// more publish at the final size (a content change -- the idle gate would
	// withhold a byte-identical frame) and watching it copy.
	if (!TestTrue(TEXT("UI frames ran to settle"), RunFrames(*UIThread, 3)))
	{
		return false;
	}
	FlushRenderingCommands();

	const uint64 CopiesSettled = Sink->GetNumCopies();
	const uint64 SkipsSettled = Sink->GetNumExtentSkips();
	AddInfo(FString::Printf(TEXT("Churn totals: %llu arrival(s), %llu copie(s), %llu extent skip(s)"),
		Sink->GetNumArrivals(), CopiesSettled, SkipsSettled));

	const uint64 ArrivalsSettled = Sink->GetNumArrivals();
	View->LoadDocumentFromMemory(
		TEXT("<rml><head><style>body{display:block;width:100%;height:100%;}")
		TEXT("div{display:block;position:absolute;left:8px;top:8px;width:60px;height:24px;background-color:#3040d0;}")
		TEXT("</style></head><body><div/></body></rml>"));
	if (!TestTrue(TEXT("UI frames ran for the heal probe"), RunFrames(*UIThread, 3)))
	{
		return false;
	}
	FlushRenderingCommands();

	TestTrue(TEXT("The heal probe published"), Sink->GetNumArrivals() > ArrivalsSettled);
	if (bTextureResourcesExist)
	{
		TestTrue(TEXT("After the churn settles, a publish at the final size copies again (self-healed)"),
			Sink->GetNumCopies() > CopiesSettled);
	}
	else
	{
		AddInfo(TEXT("Extent-guard/heal counters deferred to a real-RHI run (FApp::CanEverRender() is false); ")
			TEXT("the texture-level discipline is asserted in VaCuus.World.SinkDiscipline"));
	}
	TestEqual(TEXT("...into an RT at the final DrawSize"),
		FIntPoint(Component->GetRenderTarget()->SizeX, Component->GetRenderTarget()->SizeY), FIntPoint(160, 120));

	Component->UnregisterComponent();
	if (TestTrue(TEXT("UI frames ran after the unregister"), RunFrames(*UIThread, 2)))
	{
		TestEqual(TEXT("No views survive the component"), UIThread->GetNumViews(), 0);
	}
	FlushRenderingCommands();

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
