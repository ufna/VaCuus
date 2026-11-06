// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusStyleSet.h"
#include "VaCuusTextureRegistry.h"

#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "UObject/GarbageCollection.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Exercise the module's cleanup delegate as well as both registries: directly calling
 * their flush helpers cannot catch a missing binding. World-owned resources also expose
 * the actual retention chain, unlike the shared material asset in StyleSetTeardownFlush.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusRegistryWorldCleanupTest, "VaCuus.Render.Registry.WorldCleanupFlush",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusRegistryWorldCleanupTest::RunTest(const FString& Parameters)
{
	UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/VaCuus/Spike/M_VaCuusSpike_Translucent.M_VaCuusSpike_Translucent"));
	if (!TestNotNull(TEXT("the committed MD_UI material loads"), Parent))
	{
		return false;
	}
	TStrongObjectPtr<UMaterialInterface> ParentRoot(Parent);

	FlushRenderingCommands();
	FVaCuusStyleRegistry::TickDeferredReleases_GameThread();
	FVaCuusTextureRegistry::TickDeferredReleases_GameThread();

	// Repeat in one process: the module's binding must survive the first world's cleanup.
	for (int32 Cycle = 0; Cycle < 2; ++Cycle)
	{
		UWorld::InitializationValues Init;
		Init.AllowAudioPlayback(false).CreatePhysicsScene(false).CreateNavigation(false)
			.CreateAISystem(false).ShouldSimulatePhysics(false);
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr,
			/*bAddToRoot=*/true, ERHIFeatureLevel::Num, &Init);
		if (!TestNotNull(TEXT("a real initialized world exists"), World))
		{
			return false;
		}
		TStrongObjectPtr<UVaCuusStyleSet> StyleSet(NewObject<UVaCuusStyleSet>(World));
		const FString TextureKey = TEXT("registry-world-cleanup-texture");
		ON_SCOPE_EXIT
		{
			FVaCuusStyleRegistry::UnregisterStyleSet(StyleSet.Get());
			FVaCuusTextureRegistry::UnregisterTexture(TextureKey);
			if (World)
			{
				World->DestroyWorld(false);
			}
			// Keep a failed mutation from contaminating later tests.
			FlushRenderingCommands();
			FVaCuusStyleRegistry::TickDeferredReleases_GameThread();
			FVaCuusTextureRegistry::TickDeferredReleases_GameThread();
		};

		UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Parent, World);
		UTextureRenderTarget2D* Texture = NewObject<UTextureRenderTarget2D>(World);
		Texture->RenderTargetFormat = RTF_RGBA8;
		Texture->InitAutoFormat(4, 4);
		Texture->UpdateResourceImmediate(true);
		StyleSet->Materials.Add(TEXT("registry-world-cleanup-material"), Material);
		TWeakObjectPtr<UWorld> WeakWorld(World);
		TWeakObjectPtr<UMaterialInstanceDynamic> WeakMaterial(Material);
		TWeakObjectPtr<UTextureRenderTarget2D> WeakTexture(Texture);

		TestEqual(TEXT("the world-owned material registers"),
			FVaCuusStyleRegistry::RegisterStyleSet(StyleSet.Get()), 1);
		TestTrue(TEXT("the world-owned texture registers"),
			FVaCuusTextureRegistry::RegisterTexture(TextureKey, Texture));
		FVaCuusStyleRegistry::UnregisterStyleSet(StyleSet.Get());
		FVaCuusTextureRegistry::UnregisterTexture(TextureKey);
		StyleSet.Reset();
		TestEqual(TEXT("the material root is parked before cleanup"),
			FVaCuusStyleRegistry::GetNumPendingReleases_GameThread(), 1);
		TestEqual(TEXT("the texture root is parked before cleanup"),
			FVaCuusTextureRegistry::GetNumPendingReleases_GameThread(), 1);

		// DestroyWorld calls CleanupWorld (World.cpp:2776), whose OnWorldCleanup broadcast
		// precedes the PIE GC/stale-reference check (PlayLevel.cpp:912, 502-554).
		// No registry helper, subsystem tick, or rendering flush may substitute for that hook.
		World->DestroyWorld(false);
		World = nullptr;
		TestEqual(TEXT("world cleanup drained the material root without a tick"),
			FVaCuusStyleRegistry::GetNumPendingReleases_GameThread(), 0);
		TestEqual(TEXT("world cleanup drained the texture root without a tick"),
			FVaCuusTextureRegistry::GetNumPendingReleases_GameThread(), 0);

		CollectGarbage(RF_NoFlags);
		TestFalse(TEXT("the world-owned material can be collected"), WeakMaterial.IsValid());
		TestFalse(TEXT("the world-owned texture can be collected"), WeakTexture.IsValid());
		TestFalse(TEXT("no registry root keeps the destroyed world alive"), WeakWorld.IsValid());
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
