// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusMaterialSpike.h"

#include "VaCuusDefines.h"
#include "VaCuusUIShaders.h"

#include "Containers/Ticker.h"
#include "Engine/Texture2D.h"
#include "GameTime.h"
#include "GlobalRenderResources.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialRenderProxy.h"
#include "PipelineStateCache.h"
#include "RHIResourceUtils.h"
#include "RHIStaticStates.h"
#include "RenderingThread.h"
#include "SceneView.h"
#include "ShaderParameterUtils.h"
#include "ShowFlags.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include <atomic>

IMPLEMENT_MATERIAL_SHADER_TYPE(, FVaCuusMaterialVS, TEXT("/Plugin/VaCuus/Private/VaCuusMaterial.usf"), TEXT("MainVS"), SF_Vertex);
IMPLEMENT_MATERIAL_SHADER_TYPE(, FVaCuusMaterialPS, TEXT("/Plugin/VaCuus/Private/VaCuusMaterial.usf"), TEXT("MainPS"), SF_Pixel);

/**
 * RENDERER_API but declared in a private engine header (SceneRendering.h:3135), so the
 * declaration is repeated here rather than included. It swaps the real noise textures
 * into the fabricated view UB — the same patch Slate applies to ITS fabricated UB
 * (SlateRHIRenderingPolicy.cpp:751-753); without it a material using the Noise node
 * samples a white dummy.
 */
RENDERER_API void UpdateNoiseTextureParameters(FViewUniformShaderParameters& ViewUniformShaderParameters);

/** THE SPIKE GATE: default 0 — nothing records, nothing draws, the gate term is false. */
static TAutoConsoleVariable<int32> CVarVaCuusMaterialDecorators(
	TEXT("vacuus.MaterialDecorators"),
	0,
	TEXT("M5 material-decorator SPIKE (spec §3.3 stage 2). 1 = registered materials (vacuus.MatSpike.Add) are drawn into ")
	TEXT("the replay RT after the recorded commands, and the forced-republish freeze remedy may engage. Default 0."));

/**
 * THE FREEZE REMEDY SWITCH, separate from the master so the freeze itself can be
 * OBSERVED (remedy off => a time-animated material freezes between publishes — the
 * source-verified spec §2(f) fact this spike must measure, not just assert).
 */
static TAutoConsoleVariable<int32> CVarVaCuusMaterialForcedRepublish(
	TEXT("vacuus.MaterialForcedRepublish"),
	1,
	TEXT("1 (default) = while material draws are live (vacuus.MaterialDecorators=1 and a material registered), every ")
	TEXT("recorded UI frame publishes, so the replay pass re-evaluates materials with fresh time/parameters. ")
	TEXT("0 = observe the freeze the remedy exists for."));

namespace VaCuusMaterialSpike
{
/** One registered material, game-thread side: the root and the paint rect. */
struct FGameEntry
{
	TStrongObjectPtr<UMaterialInterface> Material;
	FIntRect Rect;
};

/** The render-thread mirror: proxy + rect + the lazily built quad. */
struct FRenderEntry
{
	const FMaterialRenderProxy* Proxy = nullptr;
	FIntRect Rect;
	FBufferRHIRef VB;
	FBufferRHIRef IB;
};

static TArray<FGameEntry> GGameEntries;					// game thread only
static TArray<FRenderEntry> GRenderEntries;				// render thread only
static std::atomic<int32> GLiveMaterialCount{0};		// written game thread, read UI thread
static std::atomic<int64> GDrawCount{0};				// render thread writes, anyone reads
static std::atomic<int64> GShaderMissCount{0};			// render thread writes, anyone reads
static bool bLoggedShaderMiss = false;					// render thread only, latch

/** Rebuild the render-side mirror from a game-thread snapshot. Registration-rate, not frame-rate. */
static void MirrorToRenderThread()
{
	TArray<TPair<const FMaterialRenderProxy*, FIntRect>> Snapshot;
	Snapshot.Reserve(GGameEntries.Num());
	for (const FGameEntry& Entry : GGameEntries)
	{
		// GetRenderProxy is game-thread API; the proxy pointer stays valid while the
		// TStrongObjectPtr in GGameEntries roots the material (proxy lifetime is tied to
		// the UMaterialInterface, released via a deferred render command on destruction).
		Snapshot.Emplace(Entry.Material->GetRenderProxy(), Entry.Rect);
	}

	ENQUEUE_RENDER_COMMAND(VaCuusMatSpikeMirror)(
		[Snapshot = MoveTemp(Snapshot)](FRHICommandListImmediate& RHICmdList)
		{
			GRenderEntries.Reset();
			for (const TPair<const FMaterialRenderProxy*, FIntRect>& Pair : Snapshot)
			{
				FRenderEntry& Entry = GRenderEntries.AddDefaulted_GetRef();
				Entry.Proxy = Pair.Key;
				Entry.Rect = Pair.Value;

				// The quad: FVaCuusVertex layout, UV 0..1 over the rect (the same
				// normalization DecoratorShader.cpp:44-46 gives recorded shader
				// geometry), white premultiplied colour.
				const FVector2f Min(float(Entry.Rect.Min.X), float(Entry.Rect.Min.Y));
				const FVector2f Max(float(Entry.Rect.Max.X), float(Entry.Rect.Max.Y));
				const FColor White(255, 255, 255, 255);
				const FVaCuusVertex Vertices[4] = {
					{Min, White, FVector2f(0.0f, 0.0f)},
					{FVector2f(Max.X, Min.Y), White, FVector2f(1.0f, 0.0f)},
					{Max, White, FVector2f(1.0f, 1.0f)},
					{FVector2f(Min.X, Max.Y), White, FVector2f(0.0f, 1.0f)}};
				const int32 Indices[6] = {0, 1, 2, 0, 2, 3};
				Entry.VB = UE::RHIResourceUtils::CreateVertexBufferFromArray<FVaCuusVertex>(
					RHICmdList, TEXT("VaCuusMatSpikeVB"), EBufferUsageFlags::Static, MakeConstArrayView(Vertices, 4));
				Entry.IB = UE::RHIResourceUtils::CreateIndexBufferFromArray<int32>(
					RHICmdList, TEXT("VaCuusMatSpikeIB"), EBufferUsageFlags::Static, MakeConstArrayView(Indices, 6));
			}
		});
}

bool Register(UMaterialInterface* InMaterial, const FIntRect& InRect)
{
	check(IsInGameThread());
	if (!InMaterial)
	{
		return false;
	}

	// The registry-validation refusal (material-decorators.md §3): only MD_UI materials
	// compile the FVaCuusMaterial* permutations; anything else would silently fall back
	// to the default UI material, which is exactly the confusing failure a named refusal
	// prevents. (Scene-texture/VT rejection is production-registry work, out of spike scope;
	// the .usf hard-disables scene textures so such a material draws defaults, not garbage.)
	const UMaterial* BaseMaterial = InMaterial->GetMaterial();
	if (!BaseMaterial || BaseMaterial->MaterialDomain != EMaterialDomain::MD_UI)
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("vacuus.MatSpike: '%s' is not a User Interface (MD_UI) domain material — refused. The material tier ")
			TEXT("compiles its shaders for MD_UI only (the Slate permutation gate, SlateMaterialShader.cpp:29-32)."),
			*InMaterial->GetPathName());
		return false;
	}

	GGameEntries.Add({TStrongObjectPtr<UMaterialInterface>(InMaterial), InRect});
	GLiveMaterialCount.store(GGameEntries.Num(), std::memory_order_relaxed);
	MirrorToRenderThread();
	UE_LOG(LogVaCuus, Log, TEXT("vacuus.MatSpike: registered '%s' at (%d,%d)-(%d,%d), %d live"),
		*InMaterial->GetPathName(), InRect.Min.X, InRect.Min.Y, InRect.Max.X, InRect.Max.Y, GGameEntries.Num());
	return true;
}

void ClearAll()
{
	check(IsInGameThread());
	GGameEntries.Reset();
	GLiveMaterialCount.store(0, std::memory_order_relaxed);
	MirrorToRenderThread();
	UE_LOG(LogVaCuus, Log, TEXT("vacuus.MatSpike: cleared"));
}

void ReleaseRenderResources()
{
	// Static-storage FBufferRHIRefs must not outlive the RHI; module shutdown runs
	// before RHI teardown, so this is the last safe point to drop them.
	if (GIsThreadedRendering || IsInGameThread())
	{
		ENQUEUE_RENDER_COMMAND(VaCuusMatSpikeRelease)([](FRHICommandListImmediate&) { GRenderEntries.Empty(); });
		FlushRenderingCommands();
	}
	else
	{
		GRenderEntries.Empty();
	}
}

bool WantsForcedRepublish()
{
	return GLiveMaterialCount.load(std::memory_order_relaxed) > 0 &&
		CVarVaCuusMaterialDecorators.GetValueOnAnyThread() != 0 &&
		CVarVaCuusMaterialForcedRepublish.GetValueOnAnyThread() != 0;
}

int64 GetDrawCount()
{
	return GDrawCount.load(std::memory_order_relaxed);
}

int64 GetShaderMissCount()
{
	return GShaderMissCount.load(std::memory_order_relaxed);
}

/**
 * The synthetic view UB — Slate's own recipe for drawing UI materials outside any scene
 * (SlateRHIRenderingPolicy.cpp:706-756), reproduced: ortho FViewMatrices whose
 * "projection" is the very pixel->clip matrix the vertices use, the RT-sized view rect,
 * SetupCommonViewUniformBufferParameters over game show flags, then the two patches
 * Slate applies on top (noise textures; BufferToSceneTextureScale positive). ONE UB per
 * replay pass with material draws, exactly as the research note prices it.
 */
static TUniformBufferRef<FViewUniformShaderParameters> CreateSpikeViewUniformBuffer(FIntPoint RTSize, const FMatrix44f& Projection)
{
	static const FEngineShowFlags DefaultShowFlags(ESFIM_Game);
	const FIntRect ViewRect(0, 0, RTSize.X, RTSize.Y);

	FViewMatrices::FMinimalInitializer Initializer;
	Initializer.ProjectionMatrix = FMatrix(Projection);
	Initializer.ConstrainedViewRect = ViewRect;
	const FViewMatrices ViewMatrices(Initializer);

	const FSetupViewUniformParametersInputs SetupInputs = {
		.EngineShowFlags = &DefaultShowFlags,
		.UnscaledViewRect = ViewRect,
		// Real app time so Time-driven materials ANIMATE across publishes — the freeze
		// remedy is what makes the publishes happen; this is what makes them differ.
		.Time = FGameTime::GetTimeSinceAppStart(),
	};

	FViewUniformShaderParameters Parameters;
	SetupCommonViewUniformBufferParameters(Parameters, RTSize, 1, ViewRect, ViewMatrices, ViewMatrices, SetupInputs);

	// Slate's own two post-patches (SlateRHIRenderingPolicy.cpp:744-753). VT feedback is
	// NOT wired (VT-sampling UI materials are declared unsupported, material-decorators.md
	// §3); the empty UAV keeps the UB's resource table valid where the RHI validates it.
	Parameters.BufferToSceneTextureScale = FVector2f(1.0f, 1.0f);
	UpdateNoiseTextureParameters(Parameters);
	if (GEmptyStructuredBufferWithUAV && GEmptyStructuredBufferWithUAV->UnorderedAccessViewRHI)
	{
		Parameters.VTFeedbackBuffer = GEmptyStructuredBufferWithUAV->UnorderedAccessViewRHI;
	}

	return TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(Parameters, UniformBuffer_SingleFrame);
}

int32 DrawInjected_RenderThread(FRHICommandList& RHICmdList, FIntPoint RTSize, const FMatrix44f& Projection)
{
	check(IsInRenderingThread());
	if (CVarVaCuusMaterialDecorators.GetValueOnRenderThread() == 0 || GRenderEntries.Num() == 0)
	{
		return 0;
	}

	FMaterialShaderTypes ShaderTypes;
	ShaderTypes.AddShaderType<FVaCuusMaterialVS>();
	ShaderTypes.AddShaderType<FVaCuusMaterialPS>();

	const TUniformBufferRef<FViewUniformShaderParameters> ViewUniformBuffer = CreateSpikeViewUniformBuffer(RTSize, Projection);

	// Recorded commands may leave a scissor enabled; the injected draws are not theirs.
	RHICmdList.SetScissorRect(false, 0, 0, 0, 0);

	int32 NumDraws = 0;
	FRHIPixelShader* BoundPS = nullptr;
	for (FRenderEntry& Entry : GRenderEntries)
	{
		// THE MATERIAL-SHADER WALK — Slate's, verbatim in shape
		// (SlateRHIRenderingPolicy.cpp:1157-1170): refresh the proxy's uniform
		// expression cache, ask the material for OUR shader types, and fall back down
		// the proxy chain (an MD_UI material still compiling lands on the default UI
		// material rather than drawing nothing).
		const FMaterialRenderProxy* Proxy = Entry.Proxy;
		const FMaterial* Material = nullptr;
		TShaderRef<FVaCuusMaterialVS> VertexShader;
		TShaderRef<FVaCuusMaterialPS> PixelShader;
		while (Proxy)
		{
			const FMaterial* Candidate = Proxy->UpdateUniformExpressionCacheIfNeeded(RHICmdList, GMaxRHIFeatureLevel);
			FMaterialShaders Shaders;
			if (Candidate && Candidate->TryGetShaders(ShaderTypes, nullptr, Shaders))
			{
				Material = Candidate;
				Shaders.TryGetShader(SF_Vertex, VertexShader);
				Shaders.TryGetShader(SF_Pixel, PixelShader);
				break;
			}
			Proxy = Proxy->GetFallback(GMaxRHIFeatureLevel);
		}

		if (!VertexShader.IsValid() || !PixelShader.IsValid())
		{
			// The observable for "the permutation does not exist here" — the exact
			// NO-GO fact the monolithic check probes for, so it is counted, not just logged.
			GShaderMissCount.fetch_add(1, std::memory_order_relaxed);
			if (!bLoggedShaderMiss)
			{
				bLoggedShaderMiss = true;
				UE_LOG(LogVaCuus, Error,
					TEXT("vacuus.MatSpike: no FVaCuusMaterialVS/PS pair down the whole proxy chain — the MD_UI ")
					TEXT("permutation is missing in this build/shader-map configuration"));
			}
			continue;
		}

		// The second PSOInit of the pass (the Task 4 caveat): a material draw is a full
		// pipeline, not just a PS swap — the VS differs too. Everything but the shaders
		// matches the recorded-draw pipeline: SAME vertex declaration, SAME premultiplied
		// One/InvSrcAlpha blend (the .usf maps material blend modes onto it), so PSO
		// count grows by one per distinct material shader map, cached by the engine's
		// PSO cache like any other.
		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
		GraphicsPSOInit.BlendState =
			TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI();
		GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GVaCuusVertexDeclaration.VertexDeclarationRHI;
		GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
		GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;
		if (BoundPS != PixelShader.GetPixelShader())
		{
			BoundPS = PixelShader.GetPixelShader();
			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
		}

		{
			FRHIBatchedShaderParameters& BatchedParameters = RHICmdList.GetScratchShaderParameters();
			VertexShader->SetParameters(BatchedParameters, ViewUniformBuffer, Projection);
			RHICmdList.SetBatchedShaderParameters(VertexShader.GetVertexShader(), BatchedParameters);
		}
		{
			FRHIBatchedShaderParameters& BatchedParameters = RHICmdList.GetScratchShaderParameters();
			PixelShader->SetParameters(BatchedParameters, ViewUniformBuffer, Proxy, *Material);
			RHICmdList.SetBatchedShaderParameters(PixelShader.GetPixelShader(), BatchedParameters);
		}

		RHICmdList.SetStreamSource(0, Entry.VB, 0);
		RHICmdList.DrawIndexedPrimitive(Entry.IB,
			/*BaseVertexIndex=*/0, /*FirstInstance=*/0, /*NumVertices=*/4,
			/*StartIndex=*/0, /*NumPrimitives=*/2, /*NumInstances=*/1);
		++NumDraws;
		GDrawCount.fetch_add(1, std::memory_order_relaxed);
	}

	return NumDraws;
}

// ---------------------------------------------------------------------------------------
// Console surface (game thread). Spike-only; the production surface is UVaCuusStyleSet.
// ---------------------------------------------------------------------------------------

/** Args: <ObjectPath> [x y w h]. Default rect matches m5_matspike.rml's first column. */
static FIntRect ParseRect(const TArray<FString>& Args, int32 FirstIndex)
{
	FIntRect Rect(60, 130, 60 + 380, 130 + 240);
	if (Args.Num() >= FirstIndex + 4)
	{
		const int32 X = FCString::Atoi(*Args[FirstIndex]);
		const int32 Y = FCString::Atoi(*Args[FirstIndex + 1]);
		const int32 W = FCString::Atoi(*Args[FirstIndex + 2]);
		const int32 H = FCString::Atoi(*Args[FirstIndex + 3]);
		Rect = FIntRect(X, Y, X + FMath::Max(W, 1), Y + FMath::Max(H, 1));
	}
	return Rect;
}

static void CmdAdd(const TArray<FString>& Args)
{
	if (Args.Num() < 1)
	{
		UE_LOG(LogVaCuus, Error, TEXT("Usage: vacuus.MatSpike.Add <ObjectPath> [x y w h]"));
		return;
	}
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *Args[0]);
	if (!Material)
	{
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.MatSpike.Add: could not load '%s'"), *Args[0]);
		return;
	}
	Register(Material, ParseRect(Args, 1));
}

/**
 * The day-2 (b) probe: an MID whose scalar ("SpikeScalar") and texture ("SpikeTex")
 * parameters are driven EVERY GAME FRAME; the proxy must pick both up in our pass with
 * no Slate draw anywhere and no game-thread hitch. The pump self-times and logs avg/max
 * microseconds every 5 seconds — the measurement the gate decision records.
 */
static FTSTicker::FDelegateHandle GMidPumpHandle;
static double GMidPumpStart = 0.0;
static uint64 GMidPumpCycles = 0;
static uint64 GMidPumpMaxCycles = 0;
static int64 GMidPumpTicks = 0;
static double GMidPumpLastLog = 0.0;
static TStrongObjectPtr<UMaterialInstanceDynamic> GMidPumpTarget;
static TStrongObjectPtr<UTexture2D> GMidPumpTexA;
static TStrongObjectPtr<UTexture2D> GMidPumpTexB;

static void StopMidPump()
{
	if (GMidPumpHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GMidPumpHandle);
		GMidPumpHandle.Reset();
	}
	GMidPumpTarget.Reset();
	GMidPumpTexA.Reset();
	GMidPumpTexB.Reset();
}

static void CmdMid(const TArray<FString>& Args)
{
	if (Args.Num() < 1)
	{
		UE_LOG(LogVaCuus, Error, TEXT("Usage: vacuus.MatSpike.MID <BaseMaterialObjectPath> [x y w h]"));
		return;
	}
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, *Args[0]);
	if (!Base)
	{
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.MatSpike.MID: could not load '%s'"), *Args[0]);
		return;
	}

	UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, GetTransientPackage());
	if (!Register(Mid, ParseRect(Args, 1)))
	{
		return;
	}

	StopMidPump();
	GMidPumpTarget = TStrongObjectPtr<UMaterialInstanceDynamic>(Mid);
	GMidPumpTexA = TStrongObjectPtr<UTexture2D>(LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture")));
	GMidPumpTexB = TStrongObjectPtr<UTexture2D>(LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture")));
	GMidPumpStart = FPlatformTime::Seconds();
	GMidPumpLastLog = GMidPumpStart;
	GMidPumpCycles = 0;
	GMidPumpMaxCycles = 0;
	GMidPumpTicks = 0;

	GMidPumpHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[](float) -> bool
		{
			UMaterialInstanceDynamic* Target = GMidPumpTarget.Get();
			if (!Target)
			{
				return false;
			}

			const double Now = FPlatformTime::Seconds();
			const double T = Now - GMidPumpStart;

			const uint64 Cycles0 = FPlatformTime::Cycles64();
			Target->SetScalarParameterValue(TEXT("SpikeScalar"), 0.15f + 0.85f * (0.5f + 0.5f * FMath::Sin(float(T) * 2.0f)));
			UTexture2D* Tex = (int64(T) & 1) ? GMidPumpTexB.Get() : GMidPumpTexA.Get();
			if (Tex)
			{
				Target->SetTextureParameterValue(TEXT("SpikeTex"), Tex);
			}
			const uint64 CyclesDelta = FPlatformTime::Cycles64() - Cycles0;

			GMidPumpCycles += CyclesDelta;
			GMidPumpMaxCycles = FMath::Max(GMidPumpMaxCycles, CyclesDelta);
			++GMidPumpTicks;
			if (Now - GMidPumpLastLog >= 5.0)
			{
				GMidPumpLastLog = Now;
				const double ToUs = FPlatformTime::GetSecondsPerCycle64() * 1e6;
				UE_LOG(LogVaCuus, Log,
					TEXT("vacuus.MatSpike.MID pump: %lld ticks, avg %.2f us, max %.2f us per game-thread frame (scalar+texture set)"),
					GMidPumpTicks, double(GMidPumpCycles) / double(FMath::Max<int64>(GMidPumpTicks, 1)) * ToUs,
					double(GMidPumpMaxCycles) * ToUs);
				GMidPumpCycles = 0;
				GMidPumpMaxCycles = 0;
				GMidPumpTicks = 0;
			}
			return true;
		}));
}

static void CmdClear(const TArray<FString>&)
{
	StopMidPump();
	ClearAll();
}

static FAutoConsoleCommand GMatSpikeAddCommand(
	TEXT("vacuus.MatSpike.Add"),
	TEXT("M5 material SPIKE: <ObjectPath> [x y w h] — load an MD_UI material and draw it into the replay RT at the ")
	TEXT("rect (view px). Requires vacuus.MaterialDecorators 1."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&CmdAdd));

static FAutoConsoleCommand GMatSpikeMidCommand(
	TEXT("vacuus.MatSpike.MID"),
	TEXT("M5 material SPIKE: <BaseMaterialObjectPath> [x y w h] — create an MID over the base, drive 'SpikeScalar' and ")
	TEXT("'SpikeTex' every game frame, and log the game-thread cost every 5s."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&CmdMid));

static FAutoConsoleCommand GMatSpikeClearCommand(
	TEXT("vacuus.MatSpike.Clear"),
	TEXT("M5 material SPIKE: unregister every material and stop the MID pump."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&CmdClear));
} // namespace VaCuusMaterialSpike
