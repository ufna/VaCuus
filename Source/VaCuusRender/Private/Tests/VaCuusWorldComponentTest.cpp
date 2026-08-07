// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusCommandBuffer.h"
#include "VaCuusDefines.h"
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
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/App.h"
#include "Misc/ScopeExit.h"
#include "RHI.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "TextureResource.h"
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
 * Bead VaCuus-9b3: the world sink starts the ASYNC upload too, and it does it from its
 * own entry point rather than from a caller that remembered to.
 *
 * WHAT THIS TEST CAN AND CANNOT SEE, said plainly because the split is deliberate. The
 * async path's pixels and its RHI-thread ordering are already gated byte-for-byte by
 * VaCuus.Render.Upload.AsyncPayload, which drives the SAME FVaCuusReplayRenderer both
 * sinks own -- re-reading them here would test the replayer twice and the sink zero
 * times. What was untested until this bead is the sink's own decision, and its only
 * observable is the route counters (both routes leave the same pixels in the same map,
 * which is why FVaCuusReplayRenderer::GetNumAsyncTextureUploads exists at all): one
 * buffer, two payloads, one on each side of the shipped threshold, arriving through
 * SetPendingBuffer_RenderThread must split 1 async / 1 sync.
 *
 * A bare sink, no component and no view -- the render command IS the contract, exactly
 * as VaCuus.World.SinkDiscipline argues.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusWorldAsyncUploadTest, "VaCuus.World.AsyncUpload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusWorldAsyncUploadTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusWorldComponentTest;
	if (!SuiteViable(*this))
	{
		return true;
	}

	IConsoleVariable* ThresholdVar = IConsoleManager::Get().FindConsoleVariable(TEXT("vacuus.AsyncTextureUploadBytes"));
	if (!TestNotNull(TEXT("vacuus.AsyncTextureUploadBytes exists"), ThresholdVar))
	{
		return false;
	}
	const int32 SavedThreshold = ThresholdVar->GetInt();
	ON_SCOPE_EXIT
	{
		ThresholdVar->Set(SavedThreshold, ECVF_SetByCode);
	};

	// The SHIPPED default, and payload sizes chosen against it rather than against a
	// threshold invented for the test: 1024x1024 RGBA is exactly 4 MB (so it is at the
	// bound, which the >= comparison sends async), 8x8 is 256 bytes.
	ThresholdVar->Set(4 * 1024 * 1024, ECVF_SetByCode);
	const FVaCuusTextureHandle BigHandle = 21;
	const FVaCuusTextureHandle SmallHandle = 22;

	TUniquePtr<FVaCuusCommandBuffer> Buffer = MakeUnique<FVaCuusCommandBuffer>();
	Buffer->Generation = 1;
	Buffer->ViewSize = FIntPoint(64, 64);
	FVaCuusTextureData& Big = Buffer->NewTextures.Add(BigHandle);
	Big.Size = FIntPoint(1024, 1024);
	Big.RGBA.SetNumZeroed(int64(Big.Size.X) * int64(Big.Size.Y) * 4);
	FVaCuusTextureData& Small = Buffer->NewTextures.Add(SmallHandle);
	Small.Size = FIntPoint(8, 8);
	Small.RGBA.SetNumZeroed(int64(Small.Size.X) * int64(Small.Size.Y) * 4);

	const TSharedRef<FVaCuusWorldSink> Sink = MakeShared<FVaCuusWorldSink>();
	ENQUEUE_RENDER_COMMAND(VaCuusWorldAsyncUpload)(
		[Sink, &Buffer](FRHICommandListImmediate& RHICmdList)
		{
			Sink->SetPendingBuffer_RenderThread(RHICmdList, MoveTemp(Buffer));
			Sink->ReleaseResources_RenderThread();
		});
	FlushRenderingCommands();

	TestEqual(TEXT("The buffer arrived"), int64(Sink->GetNumArrivals()), int64(1));
	TestEqual(TEXT("The 4 MB payload took the async path"),
		int64(Sink->GetReplayerForTest().GetNumAsyncTextureUploads()), int64(1));
	TestEqual(TEXT("The 256-byte payload took the inline path"),
		int64(Sink->GetReplayerForTest().GetNumSyncTextureUploads()), int64(1));

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

/**
 * The bGenerateMips contract at the resource level, both ways, on one component
 * (post-M6, the minification-strobe fix):
 *
 *   ON  (the default): the RT reports the full chain -- NumMips = FloorLog2(max
 *        side) + 1, computed at resource creation (TextureRenderTarget2D.cpp:96-103)
 *        -- filtered trilinearly, and every copy is followed by exactly one mip
 *        generation (the counter rides NumCopies one-to-one).
 *   OFF (SetGenerateMips(false)): exactly 1 mip, the filter back at the class
 *        default, and the generation counter FROZEN while copies keep flowing --
 *        the off path's zero-cost observable, not a comment.
 *   ON again: the slot-update repaint alone (no new publish -- the idle gate is
 *        withholding) rebuilds the chain, because a fresh >1-mip destination on an
 *        idle view would otherwise minify against garbage far mips forever.
 *
 * THE SAMPLER IS THE INVARIANT, NOT THE UPROPERTY: what the material draws with is
 * the resource's SamplerStateRHI, bound by identity into the MID's
 * uniform-expression cache at fill time (MaterialUniformExpressions.cpp:1714) and
 * re-carried there only by UpdateRenderTarget's RecacheUniformExpressions -- a
 * Filter UPROPERTY that never reached a new sampler object would pass a
 * property-level test while the GPU kept filtering with the old one. So each toggle
 * asserts the resource-level sampler IDENTITY moved. Identity is exact here, not
 * flaky: FTexture::GetOrCreateSamplerState dedupes by full initializer into an
 * immortal global cache (RenderResource.cpp:449-467), so equal filters mean the
 * SAME pointer (ON twice returns the identical object) and different filters mean
 * different pointers. On a platform whose RenderTarget LOD group already filtered
 * trilinearly the ON/OFF identity check would fail loudly -- flagging that the
 * toggle's filtering half is a no-op there -- rather than pass vacuously.
 *
 * Under -nullrhi UTextures create no resource at all (Texture.cpp:336-339, the
 * lifecycle test's step-3 argument), so there the test pins the property plumbing
 * (bAutoGenerateMips/Filter reach the RT object) and the counters' zeros; the
 * NumMips/counter-motion/sampler halves run wherever the suite has a real RHI.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusWorldMipChainTest, "VaCuus.World.MipChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusWorldMipChainTest::RunTest(const FString& Parameters)
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
	TestTrue(TEXT("bGenerateMips defaults ON"), Component->bGenerateMips);
	Component->DrawSize = FIntPoint(128, 64);
	Component->bAutoLoadDocument = false;
	Component->RegisterComponentWithWorld(World);

	UVaCuusView* View = Component->GetView();
	TSharedPtr<FVaCuusWorldSink> Sink = Component->GetWorldSink();
	UTextureRenderTarget2D* RenderTarget = Component->GetRenderTarget();
	if (!TestNotNull(TEXT("View"), View) || !TestTrue(TEXT("Sink"), Sink.IsValid()) ||
		!TestNotNull(TEXT("Render target"), RenderTarget))
	{
		return false;
	}

	const bool bTextureResourcesExist = FApp::CanEverRender();
	// TextureRenderTarget2D.cpp:96-103's exact formula; 128x64 -> 8. NPOT sides are
	// fine: only the max side sets the count and the generator clamps each level to
	// max(size >> level, 1) (GenerateMips.cpp:158-160).
	const int32 ExpectedNumMips = int32(FMath::FloorLog2(128u)) + 1;

	// The bound-sampler probe (see the test comment: the sampler is the invariant).
	// Safe to read from the game thread after FlushRenderingCommands: the field is
	// written once by InitRHI and the flush orders that write before this read.
	const auto BoundSampler = [&]() -> FRHISamplerState*
	{
		FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
		return Resource ? Resource->SamplerStateRHI.GetReference() : nullptr;
	};

	// 1. ON at registration.
	TestTrue(TEXT("ON: the RT object asks for mips"), RenderTarget->bAutoGenerateMips);
	TestEqual(TEXT("ON: trilinear filtering, so minification blends between the mips"),
		RenderTarget->Filter, TEnumAsByte<TextureFilter>(TF_Trilinear));
	FRHISamplerState* SamplerOn = nullptr;
	if (bTextureResourcesExist)
	{
		FlushRenderingCommands();
		TestEqual(TEXT("ON: the RT resource carries the full chain"), RenderTarget->GetNumMips(), ExpectedNumMips);
		FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
		FRHITexture* TextureRHI = Resource ? Resource->GetRenderTargetTexture() : nullptr;
		if (TestNotNull(TEXT("ON: the RHI texture exists"), TextureRHI))
		{
			TestEqual(TEXT("ON: ...and was created with the chain"), int32(TextureRHI->GetDesc().NumMips), ExpectedNumMips);
		}
		SamplerOn = BoundSampler();
		TestNotNull(TEXT("ON: the resource carries a sampler"), SamplerOn);
	}

	// 2. Publish once; on a real RHI the generation counter must ride the copies 1:1.
	View->LoadDocumentFromMemory(GTinyDocument);
	if (!TestTrue(TEXT("UI frames ran for the load"), RunFrames(*UIThread, 3)))
	{
		return false;
	}
	FlushRenderingCommands();
	if (bTextureResourcesExist)
	{
		TestTrue(TEXT("ON: publishes copied"), Sink->GetNumCopies() >= 1);
		TestEqual(TEXT("ON: one generation per copy, no more, no fewer"),
			int64(Sink->GetNumMipGenerations()), int64(Sink->GetNumCopies()));
	}
	else
	{
		TestEqual(TEXT("Without texture resources (null RHI) the sink parks: no generations either"),
			int64(Sink->GetNumMipGenerations()), int64(0));
	}

	// 3. OFF at runtime: one mip, default filter, and the counter freezes while the
	// slot-update repaint still copies.
	const uint64 GenerationsBeforeOff = Sink->GetNumMipGenerations();
	const uint64 CopiesBeforeOff = Sink->GetNumCopies();
	Component->SetGenerateMips(false);
	FlushRenderingCommands();
	TestFalse(TEXT("OFF: the RT object no longer asks for mips"), RenderTarget->bAutoGenerateMips);
	TestEqual(TEXT("OFF: the filter is the class default again (LOD-group behavior, untouched)"),
		RenderTarget->Filter, TEnumAsByte<TextureFilter>(TF_Default));
	FRHISamplerState* SamplerOff = nullptr;
	if (bTextureResourcesExist)
	{
		TestEqual(TEXT("OFF: exactly 1 mip"), RenderTarget->GetNumMips(), 1);
		TestTrue(TEXT("OFF: the re-init's slot-update repaint copied"), Sink->GetNumCopies() > CopiesBeforeOff);
		TestEqual(TEXT("OFF: ...and generated NOTHING -- the off path's zero new cost"),
			int64(Sink->GetNumMipGenerations()), int64(GenerationsBeforeOff));
		SamplerOff = BoundSampler();
		TestTrue(TEXT("OFF: the BOUND sampler moved with the toggle (a new object -- trilinear left the initializer)"),
			SamplerOff != nullptr && SamplerOff != SamplerOn);
	}

	// 4. ON again while idle: the repaint alone rebuilds the chain.
	const uint64 GenerationsBeforeOn = Sink->GetNumMipGenerations();
	Component->SetGenerateMips(true);
	FlushRenderingCommands();
	if (bTextureResourcesExist)
	{
		TestEqual(TEXT("ON again: the chain is back"), RenderTarget->GetNumMips(), ExpectedNumMips);
		TestTrue(TEXT("ON again: the repaint generated without waiting for a publish the idle gate withholds"),
			Sink->GetNumMipGenerations() > GenerationsBeforeOn);
		// The dedup cache makes this exact: same initializer, same immortal object
		// (RenderResource.cpp:449-467) -- so "back to trilinear" is pointer equality
		// with step 1's sampler, not just "different from OFF".
		TestTrue(TEXT("ON again: the bound sampler is step 1's trilinear object, identically"),
			BoundSampler() == SamplerOn);
		AddInfo(FString::Printf(TEXT("Sampler identity: on=%p off=%p on-again=%p"),
			static_cast<void*>(SamplerOn), static_cast<void*>(SamplerOff), static_cast<void*>(BoundSampler())));
	}

	Component->UnregisterComponent();
	if (TestTrue(TEXT("UI frames ran after the unregister"), RunFrames(*UIThread, 2)))
	{
		TestEqual(TEXT("No views survive the component"), UIThread->GetNumViews(), 0);
	}
	FlushRenderingCommands();

	return true;
}

namespace VaCuusWorldMipsGPU
{
/** One mip level of the panel RT, staged through a 1-mip texture (FRHIGPUTextureReadback has no mip parameter) and read back. */
struct FMipReadback
{
	TArray<FColor> Pixels;
	FIntPoint Size = FIntPoint::ZeroValue;
	bool bRead = false;

	FColor At(int32 X, int32 Y) const { return Pixels[Y * Size.X + X]; }
};

static FMipReadback ReadMip(FRHICommandListImmediate& RHICmdList, FRHITexture* WorldRT, int32 MipIndex)
{
	FMipReadback Result;
	Result.Size = FIntPoint(FMath::Max(WorldRT->GetSizeXY().X >> MipIndex, 1), FMath::Max(WorldRT->GetSizeXY().Y >> MipIndex, 1));

	// The readback's own staging texture clones the SOURCE's desc (mip count and
	// all, RHIGPUReadback.cpp EnqueueCopy), and its copy has no mip argument -- so
	// mip N is first isolated into a 1-mip texture of mip-N extent.
	const FRHITextureCreateDesc StageDesc =
		FRHITextureCreateDesc::Create2D(TEXT("VaCuusMipStage"), Result.Size, WorldRT->GetFormat())
			.SetFlags(ETextureCreateFlags::ShaderResource)
			.SetInitialState(ERHIAccess::CopyDest);
	FTextureRHIRef Stage = RHICmdList.CreateTexture(StageDesc);

	// The panel RT sits in SRVMask between publishes (the sink's steady state).
	RHICmdList.Transition(FRHITransitionInfo(WorldRT, ERHIAccess::SRVMask, ERHIAccess::CopySrc));
	FRHICopyTextureInfo CopyInfo;
	CopyInfo.SourceMipIndex = uint32(MipIndex);
	CopyInfo.Size = FIntVector(Result.Size.X, Result.Size.Y, 1);
	RHICmdList.CopyTexture(WorldRT, Stage, CopyInfo);
	RHICmdList.Transition(FRHITransitionInfo(WorldRT, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
	RHICmdList.Transition(FRHITransitionInfo(Stage, ERHIAccess::CopyDest, ERHIAccess::CopySrc));

	FRHIGPUTextureReadback Readback(TEXT("VaCuusMipReadback"));
	Readback.EnqueueCopy(RHICmdList, Stage, FIntVector::ZeroValue, 0, FIntVector(Result.Size.X, Result.Size.Y, 1));
	RHICmdList.SubmitAndBlockUntilGPUIdle();

	if (Readback.IsReady())
	{
		int32 RowPitchInPixels = 0;
		if (const void* Data = Readback.Lock(RowPitchInPixels))
		{
			// PF_B8G8R8A8 rows; FColor is the same B,G,R,A byte order.
			Result.Pixels.SetNumUninitialized(Result.Size.X * Result.Size.Y);
			for (int32 Y = 0; Y < Result.Size.Y; ++Y)
			{
				FMemory::Memcpy(Result.Pixels.GetData() + Y * Result.Size.X,
					static_cast<const FColor*>(Data) + Y * RowPitchInPixels, Result.Size.X * sizeof(FColor));
			}
			Result.bRead = true;
			Readback.Unlock();
		}
	}
	return Result;
}
}	 // namespace VaCuusWorldMipsGPU

/**
 * GPU proof that the chain is REAL -- generated pixels in mip 1, not just a mip
 * count in a desc. The tiny document's solid #d04030 div (premultiplied a=1, so
 * bytes 208/64/48/255 exactly) is published into a 128x64 mips-on panel; mip 0 and
 * mip 1 are read back and compared at three points:
 *
 *   - mip 0 div center (24,12): the copy landed -- exact bytes.
 *   - mip 1 (12,6), whose 2x2 mip-0 source block sits entirely inside the div: a
 *     box filter of four identical texels IS that texel, so a generated mip 1
 *     equals the div color to rounding -- while an UNWRITTEN mip 1 holds whatever
 *     the allocation held (typically zero, never accidentally 208/64/48/255).
 *   - mip 1 (56,28), whose source block is entirely background: generated
 *     transparent black, exactly.
 *
 * Venue discipline copied from VaCuus.Render.Composite.LinearOutputGPU: under
 * NullRHI this self-skips loudly and passes vacuously; the real-RHI leg carries
 * the evidence.
 *
 * Restore-the-bug (2026-08-02): with the sink's GenerateDestinationMips call
 * suppressed, "mip 1 over the div equals the div color" failed at (0 0 0 0) vs
 * (208 64 48 255) while every mip 0 assertion kept passing; restored, all green.
 * Both outcomes verbatim in docs/research/proofs/worldpanel-mips/restore-the-bug.md.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusWorldMipContentGPUTest, "VaCuus.World.MipContentGPU",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusWorldMipContentGPUTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusWorldComponentTest;
	using namespace VaCuusWorldMipsGPU;
	if (!SuiteViable(*this))
	{
		return true;
	}

	if (GUsingNullRHI || !FApp::CanEverRender())
	{
		UE_LOG(LogVaCuus, Display,
			TEXT("VaCuus.World.MipContentGPU: SKIPPED under NullRHI (no RT resource, no copy, no readback)"));
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
	Component->DrawSize = FIntPoint(128, 64);
	Component->bAutoLoadDocument = false;
	Component->RegisterComponentWithWorld(World);

	UVaCuusView* View = Component->GetView();
	TSharedPtr<FVaCuusWorldSink> Sink = Component->GetWorldSink();
	if (!TestNotNull(TEXT("View"), View) || !TestTrue(TEXT("Sink"), Sink.IsValid()))
	{
		return false;
	}

	View->LoadDocumentFromMemory(GTinyDocument);
	if (!TestTrue(TEXT("UI frames ran for the load"), RunFrames(*UIThread, 3)))
	{
		return false;
	}
	FlushRenderingCommands();
	if (!TestTrue(TEXT("The panel published and copied"), Sink->GetNumCopies() >= 1))
	{
		return false;
	}
	TestEqual(TEXT("Every copy generated the chain"), int64(Sink->GetNumMipGenerations()), int64(Sink->GetNumCopies()));

	// No scene render can interleave here: RunTest owns the game thread, and only the
	// game thread's own Tick enqueues scene renders -- so the RT still holds exactly
	// what the last copy + generation left in it.
	FTextureRenderTargetResource* Resource = Component->GetRenderTarget()->GameThread_GetRenderTargetResource();
	if (!TestNotNull(TEXT("The RT resource exists"), Resource))
	{
		return false;
	}

	FMipReadback Mip0, Mip1;
	ENQUEUE_RENDER_COMMAND(VaCuusWorldMipReadback)
	([&Mip0, &Mip1, Resource](FRHICommandListImmediate& RHICmdList)
		{
			FRHITexture* WorldRT = Resource->GetRenderTargetTexture();
			if (!WorldRT || WorldRT->GetDesc().NumMips < 2)
			{
				return;
			}
			Mip0 = ReadMip(RHICmdList, WorldRT, 0);
			Mip1 = ReadMip(RHICmdList, WorldRT, 1);
		});
	FlushRenderingCommands();

	if (!TestTrue(TEXT("Both mip readbacks completed"), Mip0.bRead && Mip1.bRead))
	{
		return false;
	}

	const FColor DivColor(208, 64, 48, 255);
	const auto TestClose = [this](const TCHAR* What, const FColor& Actual, const FColor& Expected, int32 Tolerance)
	{
		TestTrue(FString::Printf(TEXT("%s: got (%d %d %d %d), expected (%d %d %d %d) +/-%d"), What, Actual.R, Actual.G,
					 Actual.B, Actual.A, Expected.R, Expected.G, Expected.B, Expected.A, Tolerance),
			FMath::Abs(int32(Actual.R) - int32(Expected.R)) <= Tolerance &&
				FMath::Abs(int32(Actual.G) - int32(Expected.G)) <= Tolerance &&
				FMath::Abs(int32(Actual.B) - int32(Expected.B)) <= Tolerance &&
				FMath::Abs(int32(Actual.A) - int32(Expected.A)) <= Tolerance);
	};

	// The div spans (4,4)-(44,20) at mip 0. Centers avoid every AA edge.
	TestClose(TEXT("mip 0 div center (the copy)"), Mip0.At(24, 12), DivColor, 2);
	TestClose(TEXT("mip 1 over the div equals the div color (the generation)"), Mip1.At(12, 6), DivColor, 6);
	TestClose(TEXT("mip 1 over the background is transparent black"), Mip1.At(56, 28), FColor(0, 0, 0, 0), 2);

	AddInfo(FString::Printf(TEXT("GPU evidence: mip0(24,12)=(%d %d %d %d), mip1(12,6)=(%d %d %d %d), mip1(56,28)=(%d %d %d %d), ")
							TEXT("%llu cop(ies), %llu generation(s)"),
		Mip0.At(24, 12).R, Mip0.At(24, 12).G, Mip0.At(24, 12).B, Mip0.At(24, 12).A, Mip1.At(12, 6).R, Mip1.At(12, 6).G,
		Mip1.At(12, 6).B, Mip1.At(12, 6).A, Mip1.At(56, 28).R, Mip1.At(56, 28).G, Mip1.At(56, 28).B, Mip1.At(56, 28).A,
		Sink->GetNumCopies(), Sink->GetNumMipGenerations()));

	Component->UnregisterComponent();
	if (TestTrue(TEXT("UI frames ran after the unregister"), RunFrames(*UIThread, 2)))
	{
		TestEqual(TEXT("No views survive the component"), UIThread->GetNumViews(), 0);
	}
	FlushRenderingCommands();

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
