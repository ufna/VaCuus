// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Engine/DataAsset.h"
#include "Templates/SharedPointer.h"

#include "VaCuusStyleSet.generated.h"

class FMaterialRenderProxy;
class FRHICommandListImmediate;
class FVaCuusUIThread;
class UMaterialInterface;

/**
 * The M5 material-decorator style set (spec §3.3, GO tier): what `decorator:
 * shader(<key>)` resolves against once the asset is registered
 * (UVaCuusSubsystem::RegisterStyleSet / FVaCuusStyleRegistry::RegisterStyleSet).
 *
 * HARD POINTERS, NOT SOFT ONES, and that is the load contract, not a shortcut. The spec
 * requires cooked+preloaded materials with no synchronous loads on the UI thread; a
 * TSoftObjectPtr would hand every consumer a "maybe loaded" it can only resolve with a
 * sync load or an async dance, on threads where neither is legal (module/asset
 * resolution off the game thread is a known hard crash class,
 * VaCuusRecordingRenderInterface.h:270-280). A hard TObjectPtr makes the preload true by
 * construction: loading the style set loads its materials, and registration — the only
 * door into the tier — can then run entirely on objects that already exist.
 *
 * COOKING (the M6 buyer-docs note): a style set the game references from code or from a
 * cooked asset cooks like anything else. A style set referenced by NOTHING — loaded by
 * path from a console command or a config string — must be forced into the cook, e.g.
 * the host project's Config/DefaultGame.ini:
 *   [/Script/UnrealEd.ProjectPackagingSettings]
 *   +DirectoriesToAlwaysCook=(Path="/VaCuus/Spike")
 * which is exactly how the committed spike materials under Content/Spike/ (this plugin's
 * test fixtures) reach a packaged game today. Runtime-constructed UMaterials cannot
 * compile outside the editor, so every material in a shipped style set is an authored,
 * cooked asset by necessity.
 */
UCLASS(BlueprintType)
class VACUUS_API UVaCuusStyleSet : public UDataAsset
{
	GENERATED_BODY()

public:
	/**
	 * Style key -> MD_UI material. The key is what RCSS writes: `decorator:
	 * shader(hud-glow)` looks up "hud-glow" verbatim (the whole shorthand value arrives
	 * as one string, RmlUi DecoratorShader.cpp:35). Builtin shader keys win over style
	 * keys at the recorder, so registration refuses a key that shadows one.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VaCuus")
	TMap<FString, TObjectPtr<UMaterialInterface>> Materials;
};

/**
 * One immutable published table: style key -> the process-unique stable id the recorder
 * writes into FVaCuusShaderDesc::MaterialId and the render thread resolves to a
 * FMaterialRenderProxy. PUBLISH-BY-REPLACEMENT: a registry change never mutates a
 * published snapshot, it mints a whole new one with a higher Version — that is the
 * spec's immutability observable, asserted where the snapshot is installed
 * (FVaCuusStyleRegistry::InstallSnapshot).
 */
struct FVaCuusStyleSnapshot
{
	/** Strictly increasing across the process; 0 never leaves the registry. */
	uint64 Version = 0;

	/** Key -> stable id. Ids are minted once per registered entry and never recycled. */
	TMap<FString, uint64> KeyToId;
};

/** Render-thread hook VaCuusRender installs: the TryGetShaders pre-warm walk (Task 5b.2). */
using FVaCuusMaterialPreWarmHook = void (*)(FRHICommandListImmediate& RHICmdList, const FMaterialRenderProxy* Proxy);

/** Game-thread hook VaCuusRender installs: is this key a builtin `shader(<key>)` name? */
using FVaCuusStyleReservedKeyHook = bool (*)(const FString& Key);

/**
 * The process-wide material-decorator registry (M5 Task 5b) — the production shape of
 * the spike's console-driven table, with the material-decorators.md §4 thread handoff:
 *
 *  - GAME THREAD (registration rate): validate + root the materials, mint stable ids,
 *    mirror {id -> FMaterialRenderProxy*} to the render thread via
 *    ENQUEUE_RENDER_COMMAND, and publish an immutable {key -> id} snapshot to the
 *    UI thread through the existing command queue — never a lock.
 *  - UI-FRAME THREAD (per CompileShader): a pure TMap lookup in the installed snapshot.
 *  - RENDER THREAD (per DrawShader): a pure TMap lookup in the proxy mirror.
 *
 * PROCESS-WIDE like the UI thread itself (one RmlUi, one recorder contract), so
 * multi-PIE instances share one table; UVaCuusSubsystem's Register/Unregister wrappers
 * are per-instance doors to the same registry.
 *
 * All state lives in the cpp; this class is only the door.
 */
class VACUUS_API FVaCuusStyleRegistry
{
public:
	/**
	 * GAME THREAD. Roots every accepted material (TStrongObjectPtr), refuses the rest by
	 * name — wrong domain (!= MD_UI), scene-texture/GBuffer or virtual-texture sampling,
	 * a key that shadows a builtin or an already-registered key — then republishes the
	 * snapshot and the proxy mirror. Returns how many entries were accepted.
	 * Double-registering the same asset is refused whole (unregister first).
	 */
	static int32 RegisterStyleSet(UVaCuusStyleSet* StyleSet);

	/**
	 * GAME THREAD. Removes the set's entries and republishes. THE DEFERRED-RELEASE
	 * DISCIPLINE (ReleasedTextures' shape, VaCuusReplayRenderer::RetireBufferResources):
	 * live views may still hold DrawShader descs naming these ids, so the game-side
	 * roots are parked behind a render fence begun AFTER the mirror replacement and
	 * dropped only once the render thread has provably stopped resolving them
	 * (TickDeferredReleases_GameThread). A draw whose id no longer resolves skips with
	 * a latched log — the named refusal, not a crash.
	 */
	static void UnregisterStyleSet(UVaCuusStyleSet* StyleSet);

	/** GAME THREAD. Current version; 0 = nothing ever registered. */
	static uint64 GetVersion_GameThread();

	/** GAME THREAD. Live registered entries (all sets). */
	static int32 GetNumEntries_GameThread();

	/** GAME THREAD. Unregistered roots still held behind their render fence. */
	static int32 GetNumPendingReleases_GameThread();

	/**
	 * GAME THREAD. The current immutable snapshot (never null once Version > 0; null
	 * before the first registration). What PublishToUIThread enqueues, and what a test
	 * that drives a recorder on its own thread hands to InstallSnapshot directly.
	 */
	static TSharedPtr<const FVaCuusStyleSnapshot> GetSnapshot_GameThread();

	/** GAME THREAD, once per subsystem tick: drops unregistered roots whose fence completed. */
	static void TickDeferredReleases_GameThread();

	/**
	 * GAME THREAD. Re-enqueues the current snapshot to a (re)started UI thread — the
	 * register-before-boot half: a style set registered before the first view must be
	 * visible to the first document's CompileShader, and the queue is FIFO from this one
	 * producer, so a snapshot enqueued before AddView/Load drains before them.
	 */
	static void PublishToUIThread(FVaCuusUIThread& UIThread);

	/**
	 * THE UI-FRAME-THREAD INSTALL — the drain's handler for a SetStyleSnapshot command,
	 * and the test seam for recorders driven on the test thread. Owned by whichever
	 * single thread drives recorders (the RmlUi owner-thread contract, FVaCuusEngine);
	 * checkf's the version monotonic (non-decreasing: a re-publish of the same snapshot
	 * to a restarted UI thread is idempotent, a REGRESSION is the immutability bug).
	 */
	static void InstallSnapshot(const TSharedPtr<const FVaCuusStyleSnapshot>& Snapshot);

	/** The installed snapshot, same thread contract as InstallSnapshot. Null = none yet. */
	static TSharedPtr<const FVaCuusStyleSnapshot> GetInstalledSnapshot();

	/** RENDER THREAD. Proxy for a stable id, or null once unregistered/never known. */
	static const FMaterialRenderProxy* ResolveProxy_RenderThread(uint64 StableId);

	/**
	 * GAME THREAD, VaCuusRender's StartupModule (the FVaCuusEngine::SetRenderInterface
	 * register-before-boot shape: installed before anything can register). Both hooks
	 * live in VaCuusRender because they need types this module must not know —
	 * FVaCuusMaterialVS/PS for the walk, the builtin key table for the collision check.
	 */
	static void InstallRenderHooks(FVaCuusMaterialPreWarmHook PreWarm, FVaCuusStyleReservedKeyHook IsReservedKey);

	/**
	 * GAME THREAD, module shutdown: flushes rendering commands, clears the mirror and
	 * every root (registered and pending alike), clears the hooks. After this the
	 * registry is empty but versions keep counting — they must never regress.
	 */
	static void Shutdown_GameThread();
};
