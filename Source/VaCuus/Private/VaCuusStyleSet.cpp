// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusStyleSet.h"

#include "VaCuus.h"
#include "VaCuusDefines.h"
#include "VaCuusCoreCompat.h"
#include "VaCuusUIThread.h"

#include "MaterialDomain.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialRenderProxy.h"
#include "RenderCommandFence.h"
#include "RenderingThread.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
/** One registered material: the root, its identity, and where it came from. */
struct FVaCuusStyleEntry
{
	FString Key;
	TStrongObjectPtr<UMaterialInterface> Material;
	uint64 StableId = 0;

	/** Which asset registered it — the unregistration unit. Weak: the roots are per entry. */
	TWeakObjectPtr<UVaCuusStyleSet> Owner;
};

/**
 * Unregistered roots parked behind their fence (the ReleasedTextures discipline, see
 * UnregisterStyleSet in the header). One batch per unregistration, not one fence per
 * material: the mirror is replaced wholesale, so one fence covers the batch.
 */
struct FVaCuusPendingRelease
{
	TArray<TStrongObjectPtr<UMaterialInterface>> Roots;
	FRenderCommandFence Fence;
};

// ---------------------------------------------------------------------------------------
// GAME-THREAD state (registration rate).
// ---------------------------------------------------------------------------------------
TArray<FVaCuusStyleEntry> GEntries;
TArray<FVaCuusPendingRelease> GPendingReleases;
uint64 GNextStableId = 1;
uint64 GVersion = 0;
TSharedPtr<const FVaCuusStyleSnapshot> GCurrentSnapshot;
FVaCuusMaterialPreWarmHook GPreWarmHook = nullptr;
FVaCuusStyleReservedKeyHook GReservedKeyHook = nullptr;

// ---------------------------------------------------------------------------------------
// UI-FRAME-THREAD state: the installed snapshot. Single-owner by the same contract as
// the recorder itself (thread-agnostic single writer, VaCuusRecordingRenderInterface.h)
// — in production the UI thread installs it from the drain and reads it in
// CompileShader; in a recorder unit test the test thread does both. The two owners can
// never coexist: a test that drives recorders must own RmlUi, which the UI thread holds
// for as long as it runs (the FVaCuusEngine owner-thread contract).
// ---------------------------------------------------------------------------------------
TSharedPtr<const FVaCuusStyleSnapshot> GInstalledSnapshot;

// ---------------------------------------------------------------------------------------
// RENDER-THREAD state: the proxy mirror, replaced wholesale by each registration change.
// ---------------------------------------------------------------------------------------
TMap<uint64, const FMaterialRenderProxy*> GProxyMirror_RenderThread;

/**
 * Rebuild + publish everything a registry change moves: the game-side snapshot, the
 * render-thread mirror (replacement, via one render command), and the UI-thread
 * snapshot (via the command queue when a UI thread is running; PublishToUIThread covers
 * the thread that starts later).
 */
void RepublishAll()
{
	check(IsInGameThread());

	++GVersion;

	TSharedRef<FVaCuusStyleSnapshot> Snapshot = MakeShared<FVaCuusStyleSnapshot>();
	Snapshot->Version = GVersion;
	Snapshot->KeyToId.Reserve(GEntries.Num());

	TArray<TPair<uint64, const FMaterialRenderProxy*>> Mirror;
	Mirror.Reserve(GEntries.Num());

	for (const FVaCuusStyleEntry& Entry : GEntries)
	{
		Snapshot->KeyToId.Add(Entry.Key, Entry.StableId);

		// GetRenderProxy is game-thread API; the pointer stays valid while the entry's
		// TStrongObjectPtr roots the material — the proxy dies via a render command the
		// game thread enqueues at UObject destruction, which is necessarily AFTER the
		// mirror replacement below in render-command order (both producers are the game
		// thread). The deferred-release fence makes the same argument checkable.
		Mirror.Emplace(Entry.StableId, Entry.Material->GetRenderProxy());
	}

	GCurrentSnapshot = Snapshot;

	ENQUEUE_RENDER_COMMAND(VaCuusStyleMirror)(
		[Mirror = MoveTemp(Mirror), PreWarm = GPreWarmHook](FRHICommandListImmediate& RHICmdList)
		{
			GProxyMirror_RenderThread.Reset();
			for (const TPair<uint64, const FMaterialRenderProxy*>& Pair : Mirror)
			{
				GProxyMirror_RenderThread.Add(Pair.Key, Pair.Value);

				// THE ASYNC-COMPILE PRE-WARM (Task 5b.2): the spike observed the whole
				// proxy chain pair-less at frame 2 while shader maps were still coming
				// up. Running the TryGetShaders walk here — registration rate, before
				// any document can draw — moves the uniform-expression-cache build and
				// the first shader-map query off the first draw. Idempotent, so
				// re-walking survivors on a republish costs a map lookup each.
				if (PreWarm)
				{
					PreWarm(RHICmdList, Pair.Value);
				}
			}
		});

	// The queue crossing (never a lock): only when a UI thread is up; a thread started
	// later gets the same snapshot from PublishToUIThread in GetOrStartUIThread.
	if (FVaCuusModule* Module = FVaCuusModule::GetPtr())
	{
		if (FVaCuusUIThread* UIThread = Module->GetUIThread())
		{
			UIThread->EnqueueSetStyleSnapshot(GCurrentSnapshot);
		}
	}
}

/**
 * The registration-time validation (the named refusals). Returns null when refused —
 * every refusal has logged its own Error naming the material and the reason.
 */
UMaterialInterface* ValidateEntry(const FString& Key, UMaterialInterface* Material)
{
	if (Key.IsEmpty())
	{
		UE_LOG(LogVaCuus, Error, TEXT("StyleSet: an entry with an empty key is refused — RCSS has no way to name it"));
		return nullptr;
	}

	if (!Material)
	{
		UE_LOG(LogVaCuus, Error, TEXT("StyleSet: key '%s' has no material — refused"), *Key);
		return nullptr;
	}

	// Builtin keys win at the recorder (CompileShader checks them first, so a shadowed
	// style key could never draw); refuse the collision here, where it can be named.
	if (GReservedKeyHook && GReservedKeyHook(Key))
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("StyleSet: key '%s' shadows a builtin shader key and is refused — builtins win at CompileShader"), *Key);
		return nullptr;
	}

	for (const FVaCuusStyleEntry& Entry : GEntries)
	{
		if (Entry.Key == Key)
		{
			UE_LOG(LogVaCuus, Error,
				TEXT("StyleSet: key '%s' is already registered (by '%s') — refused; unregister that set first"),
				*Key, *GetNameSafe(Entry.Owner.Get()));
			return nullptr;
		}
	}

	// The domain refusal (the spike's, verbatim in reason): only MD_UI materials compile
	// the FVaCuusMaterial* permutations (ShouldCompilePermutation gates on it — Slate's
	// own runtime-proven gate, SlateMaterialShader.cpp:29-32); anything else would
	// silently fall back to the default UI material.
	const UMaterial* BaseMaterial = Material->GetMaterial();
	if (!BaseMaterial || BaseMaterial->MaterialDomain != EMaterialDomain::MD_UI)
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("StyleSet: '%s' (key '%s') is not a User Interface (MD_UI) domain material — refused. The material ")
			TEXT("tier compiles its shaders for MD_UI only (the Slate permutation gate, SlateMaterialShader.cpp:29-32)."),
			*Material->GetPathName(), *Key);
		return nullptr;
	}

	// The scene-texture/VT refusal (material-decorators.md §3), on the honest game-side
	// queryable: the compiled shader map's own bits. FMaterialShaderMap::
	// NeedsSceneTextures()/NeedsGBuffer() read FMaterialCompilationOutput
	// (MaterialShared.h:1828, :1834) and GetNumVirtualTextureStacks() reads the uniform
	// expression set's VT stacks (:1867) — the same facts TextureGraph refuses draw-
	// materials on (FxMaterial_DrawMaterial.cpp:145-162). Reached through
	// GetGameThreadShaderMap() (MaterialShared.h:2778-2782, game-thread checked); the
	// render-thread twin FMaterial::NeedsSceneTextures() is explicitly not callable here
	// (check(IsInParallelRenderingThread()), MaterialShared.cpp:1050-1052).
	// The query key is engine-dependent (feature level before 5.7, shader platform after);
	// VaCuusCompat::MaterialQueryTarget is where that is written down.
	if (const FMaterialResource* Resource = Material->GetMaterialResource(VaCuusCompat::MaterialQueryTarget()))
	{
		if (const FMaterialShaderMap* ShaderMap = Resource->GetGameThreadShaderMap())
		{
			if (ShaderMap->NeedsSceneTextures() || ShaderMap->NeedsGBuffer())
			{
				UE_LOG(LogVaCuus, Error,
					TEXT("StyleSet: '%s' (key '%s') samples scene textures / the GBuffer — refused. There is no scene ")
					TEXT("behind the replay pass; the .usf compiles with SCENE_TEXTURES_DISABLED and such a material ")
					TEXT("could only ever draw defaults (material-decorators.md §3)."),
					*Material->GetPathName(), *Key);
				return nullptr;
			}
			if (ShaderMap->GetNumVirtualTextureStacks() > 0)
			{
				UE_LOG(LogVaCuus, Error,
					TEXT("StyleSet: '%s' (key '%s') samples virtual textures — refused. The replay pass runs raw RHI ")
					TEXT("with no VT feedback wired (material-decorators.md §3)."),
					*Material->GetPathName(), *Key);
				return nullptr;
			}
		}
		else
		{
			// THE DOCUMENTED LIMITATION, not a silent pass: while the shader map is still
			// async-compiling (editor only — cooked materials load their maps inline)
			// there is nothing to read the bits from. Accepted; a scene-texture material
			// slipping through here draws defaults (the .usf hard-off), never garbage.
			UE_LOG(LogVaCuus, Verbose,
				TEXT("StyleSet: '%s' (key '%s') has no game-thread shader map yet (still compiling?) — the ")
				TEXT("scene-texture/VT validation is skipped for it"),
				*Material->GetPathName(), *Key);
		}
	}

	return Material;
}
} // namespace

int32 FVaCuusStyleRegistry::RegisterStyleSet(UVaCuusStyleSet* StyleSet)
{
	check(IsInGameThread());

	if (!StyleSet)
	{
		UE_LOG(LogVaCuus, Error, TEXT("RegisterStyleSet(null) refused"));
		return 0;
	}

	for (const FVaCuusStyleEntry& Entry : GEntries)
	{
		if (Entry.Owner.Get() == StyleSet)
		{
			UE_LOG(LogVaCuus, Error,
				TEXT("RegisterStyleSet: '%s' is already registered — refused whole; unregister it first"),
				*StyleSet->GetPathName());
			return 0;
		}
	}

	int32 NumAccepted = 0;
	for (const TPair<FString, TObjectPtr<UMaterialInterface>>& Pair : StyleSet->Materials)
	{
		UMaterialInterface* Material = ValidateEntry(Pair.Key, Pair.Value.Get());
		if (!Material)
		{
			continue;
		}

		FVaCuusStyleEntry& Entry = GEntries.AddDefaulted_GetRef();
		Entry.Key = Pair.Key;
		Entry.Material = TStrongObjectPtr<UMaterialInterface>(Material);
		Entry.StableId = GNextStableId++;
		Entry.Owner = StyleSet;
		++NumAccepted;
	}

	// Republished even when everything was refused: the VERSION is the observable a
	// caller (and the test) uses to see the registration was processed at all.
	RepublishAll();

	UE_LOG(LogVaCuus, Log, TEXT("RegisterStyleSet('%s'): %d of %d entries accepted, snapshot v%llu (%d live total)"),
		*StyleSet->GetPathName(), NumAccepted, StyleSet->Materials.Num(), GVersion, GEntries.Num());
	return NumAccepted;
}

void FVaCuusStyleRegistry::UnregisterStyleSet(UVaCuusStyleSet* StyleSet)
{
	check(IsInGameThread());

	if (!StyleSet)
	{
		return;
	}

	FVaCuusPendingRelease Pending;
	for (int32 Index = GEntries.Num() - 1; Index >= 0; --Index)
	{
		// A stale Owner (the asset itself was GC'd) matches nothing; its entries stay
		// until Shutdown — by design, since nothing can name them for unregistration.
		if (GEntries[Index].Owner.Get() == StyleSet)
		{
			Pending.Roots.Add(MoveTemp(GEntries[Index].Material));
			GEntries.RemoveAt(Index);
		}
	}

	if (Pending.Roots.Num() == 0)
	{
		UE_LOG(LogVaCuus, Verbose, TEXT("UnregisterStyleSet('%s'): nothing registered under it"), *StyleSet->GetPathName());
		return;
	}

	// Replacement mirror first, then the fence: BeginFence() enqueues behind the mirror
	// command, so a completed fence proves the render thread can no longer resolve the
	// removed ids — only then may the roots drop and GC free the materials (whose proxy
	// deletion rides its own, later render command). Same AFTER-the-last-consumer shape
	// as RetireBufferResources' deferred release.
	const int32 NumRemoved = Pending.Roots.Num();
	RepublishAll();
	Pending.Fence.BeginFence();
	GPendingReleases.Add(MoveTemp(Pending));

	UE_LOG(LogVaCuus, Log,
		TEXT("UnregisterStyleSet('%s'): %d entr%s removed, snapshot v%llu (%d live; live draws naming the removed keys ")
		TEXT("now skip with a latched log)"),
		*StyleSet->GetPathName(), NumRemoved, NumRemoved == 1 ? TEXT("y") : TEXT("ies"), GVersion, GEntries.Num());
}

uint64 FVaCuusStyleRegistry::GetVersion_GameThread()
{
	check(IsInGameThread());
	return GVersion;
}

int32 FVaCuusStyleRegistry::GetNumEntries_GameThread()
{
	check(IsInGameThread());
	return GEntries.Num();
}

int32 FVaCuusStyleRegistry::GetNumPendingReleases_GameThread()
{
	check(IsInGameThread());
	return GPendingReleases.Num();
}

TSharedPtr<const FVaCuusStyleSnapshot> FVaCuusStyleRegistry::GetSnapshot_GameThread()
{
	check(IsInGameThread());
	return GCurrentSnapshot;
}

void FVaCuusStyleRegistry::TickDeferredReleases_GameThread()
{
	check(IsInGameThread());

	// Multi-PIE calls this once per subsystem per frame; the empty check keeps that a
	// single branch for everyone past the first.
	for (int32 Index = GPendingReleases.Num() - 1; Index >= 0; --Index)
	{
		if (GPendingReleases[Index].Fence.IsFenceComplete())
		{
			GPendingReleases.RemoveAt(Index);
		}
	}
}

void FVaCuusStyleRegistry::PublishToUIThread(FVaCuusUIThread& UIThread)
{
	check(IsInGameThread());

	if (GCurrentSnapshot.IsValid())
	{
		UIThread.EnqueueSetStyleSnapshot(GCurrentSnapshot);
	}
}

void FVaCuusStyleRegistry::InstallSnapshot(const TSharedPtr<const FVaCuusStyleSnapshot>& Snapshot)
{
	checkf(Snapshot.IsValid(), TEXT("InstallSnapshot(null) — the drain never enqueues one and neither may a test"));

	// THE IMMUTABILITY OBSERVABLE (spec §3.3): publish-by-replacement means versions only
	// ever move forward. Equality is legal — the same snapshot re-published to a
	// restarted UI thread (PublishToUIThread) — a regression means somebody mutated or
	// replayed history, which is exactly the bug this checkf exists to catch.
	checkf(!GInstalledSnapshot.IsValid() || Snapshot->Version >= GInstalledSnapshot->Version,
		TEXT("Style snapshot version regressed (%llu after %llu) — snapshots are publish-by-replacement and immutable"),
		Snapshot->Version, GInstalledSnapshot.IsValid() ? GInstalledSnapshot->Version : 0);

	GInstalledSnapshot = Snapshot;
}

TSharedPtr<const FVaCuusStyleSnapshot> FVaCuusStyleRegistry::GetInstalledSnapshot()
{
	return GInstalledSnapshot;
}

const FMaterialRenderProxy* FVaCuusStyleRegistry::ResolveProxy_RenderThread(uint64 StableId)
{
	check(IsInRenderingThread());
	const FMaterialRenderProxy* const* Found = GProxyMirror_RenderThread.Find(StableId);
	return Found ? *Found : nullptr;
}

void FVaCuusStyleRegistry::InstallRenderHooks(FVaCuusMaterialPreWarmHook PreWarm, FVaCuusStyleReservedKeyHook IsReservedKey)
{
	check(IsInGameThread());
	GPreWarmHook = PreWarm;
	GReservedKeyHook = IsReservedKey;
}

void FVaCuusStyleRegistry::Shutdown_GameThread()
{
	check(IsInGameThread());

	GEntries.Empty();
	GCurrentSnapshot.Reset();
	GInstalledSnapshot.Reset();
	GPreWarmHook = nullptr;
	GReservedKeyHook = nullptr;

	// The mirror holds raw proxy pointers whose owners are about to lose their roots:
	// clear it on the render thread and wait, so nothing can resolve a proxy after the
	// roots below drop. The mirror itself holds no RHI refs — a map of pointers.
	if (GIsThreadedRendering || IsInGameThread())
	{
		ENQUEUE_RENDER_COMMAND(VaCuusStyleMirrorShutdown)(
			[](FRHICommandListImmediate&) { GProxyMirror_RenderThread.Empty(); });
		FlushRenderingCommands();
	}
	else
	{
		GProxyMirror_RenderThread.Empty();
	}

	// The flush above completed every fence too.
	GPendingReleases.Empty();
}
