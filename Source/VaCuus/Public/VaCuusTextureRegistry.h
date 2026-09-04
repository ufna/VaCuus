// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "RHIResources.h" // FTextureRHIRef, held by the render-thread mirror below

#include "VaCuusTextureRegistry.generated.h"

class FVaCuusUIThread;
class UTexture;

/**
 * How the pixel shader must treat the sampled values of an engine texture.
 *
 * THE PROBLEM THIS SOLVES. VaCuus stores sRGB-ENCODED premultiplied bytes in the view
 * render target and decodes them at composite time (VaCuusUIShaders.h:52-65). Its own
 * textures uphold that by construction: they are created PF_R8G8B8A8 with no sRGB flag
 * (VaCuusReplayRenderer.cpp:227-232), so the sampler hands the shader the bytes verbatim.
 *
 * An engine texture does not. A texture with UTexture::SRGB set is created in an _SRGB
 * format, and the sampler DECODES it — the shader receives linear values where the
 * pipeline expects encoded ones, and the image reads visibly washed out. EncodeFromLinear
 * re-applies the transfer curve so the contract holds again.
 */
UENUM(BlueprintType)
enum class EVaCuusTextureEncoding : uint8
{
	/** Derive from the texture: EncodeFromLinear when the sampler decodes, Raw otherwise. */
	Auto,

	/** The sampler already yields display-encoded values; pass them through. */
	Raw,

	/** The sampler yields linear values; encode to sRGB before blending. */
	EncodeFromLinear
};

/**
 * What the sampled alpha channel means.
 *
 * WHY Opaque IS THE DEFAULT AND NOT A CONVENIENCE. A UTextureRenderTarget2D fed by a
 * SceneCapture2D typically carries alpha = 0 across the whole image (the capture writes
 * scene colour, and nothing writes coverage). Under the pipeline's premultiplied
 * One/InverseSourceAlpha blend that is a FULLY TRANSPARENT image — i.e. the first thing
 * anyone tries would look like the feature does not work at all, with nothing in any log
 * to say why. Opaque forces alpha to 1 and makes the common case correct with no argument.
 */
UENUM(BlueprintType)
enum class EVaCuusTextureAlpha : uint8
{
	/** Derive from the texture: Opaque for a render target, Premultiplied otherwise. */
	Auto,

	/** Ignore the sampled alpha; draw the texture fully opaque. */
	Opaque,

	/** RGB is already multiplied by A — the RmlUi contract, nothing to do. */
	Premultiplied,

	/** RGB is straight colour; multiply by A before blending. */
	Straight
};

/**
 * What the UI thread knows about one registered key: enough to lay the element out and to
 * decide whether this view must keep republishing, and nothing more. The pixels, and the
 * two enums that say how to read them, live only in the render-thread mirror — a value
 * that changes on re-registration should not have to travel through a UI-thread snapshot
 * and back out to be correct.
 */
struct FVaCuusTextureBinding
{
	/** Hash of the key; what the recorder writes into the command buffer. Never 0. */
	uint64 StableId = 0;

	/** The texture's size at registration — a FIRST-LAYOUT HINT, see the registry note. */
	FIntPoint Size = FIntPoint::ZeroValue;

	/** Registered as continuously changing: every frame counts as dirty. */
	bool bLive = false;
};

/**
 * One immutable published table: key -> binding. PUBLISH-BY-REPLACEMENT, exactly as
 * FVaCuusStyleSnapshot: a registry change never mutates a published snapshot, it mints a
 * whole new one with a higher Version, and that is the immutability observable asserted
 * where the snapshot is installed.
 */
struct FVaCuusTextureSnapshot
{
	/** Strictly increasing across the process; 0 never leaves the registry. */
	uint64 Version = 0;

	TMap<FString, FVaCuusTextureBinding> KeyToBinding;
};

/** What the render thread resolves a stable id to: the pixels plus how to read them. */
struct FVaCuusExternalTextureBinding
{
	/**
	 * The registered texture's STABLE RHI REFERENCE, not its current resource. See
	 * FVaCuusTextureRegistry::RegisterTexture for why that distinction is the whole
	 * reason this works across a render-target resize.
	 */
	FTextureRHIRef Texture;

	/** Both resolved at registration; never Auto by the time they reach here. */
	EVaCuusTextureEncoding Encoding = EVaCuusTextureEncoding::Raw;
	EVaCuusTextureAlpha Alpha = EVaCuusTextureAlpha::Opaque;
};

/**
 * THE ENGINE-TEXTURE REGISTRY (spec 2026-08-22): what `<img src="unreal://<key>">`
 * resolves against, and the door through which a UTexture — a render target, a cooked
 * texture asset, a media texture — reaches a document.
 *
 * DELIBERATELY THE SAME SHAPE AS FVaCuusStyleRegistry (VaCuusStyleSet.h:80-92), down to
 * the method names, so a reader who has understood one has understood both:
 *
 *  - GAME THREAD (registration rate): validate + root the texture, take its stable RHI
 *    reference, mirror {id -> binding} to the render thread via ENQUEUE_RENDER_COMMAND,
 *    and publish an immutable {key -> binding} snapshot to the UI thread through the
 *    existing command queue — never a lock.
 *  - UI-FRAME THREAD (per LoadTexture): a pure TMap lookup in the installed snapshot.
 *  - RENDER THREAD (per draw): a pure TMap lookup in the mirror.
 *
 * WHAT IS NOT COPIED FROM THE STYLE REGISTRY, AND WHY: stable ids are HASHED FROM THE KEY
 * rather than minted by a counter. The counter version would require the UI thread to ask
 * the game thread for an id, and the UI thread must be able to name a key that has never
 * been registered — see IdForKey.
 *
 * SIZE IS A FIRST-LAYOUT HINT, NOT A LIVE VALUE. RmlUi caches a texture entry's
 * dimensions and refreshes them nowhere: FileTextureDatabase::EnsureLoaded re-enters
 * LoadTextureEntry only while the handle is still 0 (RmlUi TextureDatabase.cpp:118-130).
 * So a render target that resizes after first layout will not relayout. Size the element
 * in CSS; this field only spares the author from having to when the size is already known.
 */
class VACUUS_API FVaCuusTextureRegistry
{
public:
	/**
	 * THE STABLE ID, AND WHY IT IS A HASH.
	 *
	 * The id must be computable by the UI thread for a key the game has NEVER MENTIONED,
	 * because of a latch in RmlUi: FileTextureDatabase::LoadTextureEntry sets
	 * load_texture_failed on a zero handle (TextureDatabase.cpp:106-113) and EnsureLoaded
	 * never retries a latched entry (:118-130). A document that loads before its key is
	 * registered would therefore be dead FOREVER, with one warning at load and silence
	 * after. So LoadTexture must mint a handle for an unregistered key and let resolution
	 * happen later — which means naming the key with an id, on the UI thread, with no
	 * game-thread round trip and no lock.
	 *
	 * A hash gives that for free. The collision risk over a handful of keys is nil, and a
	 * collision is not silent: registration is the one place both keys are visible on one
	 * thread, and it refuses the newcomer by name.
	 *
	 * Never returns 0 — 0 is the command buffer's "no texture" sentinel.
	 */
	static uint64 IdForKey(const FString& Key);

	/**
	 * GAME THREAD. Registers (or re-registers) a key. Returns false on a named refusal —
	 * empty key, null texture, an id already held by a different key, or a texture whose
	 * RHI reference has never been initialised.
	 *
	 * THE RHI REFERENCE IS THE POINT, not the texture's current resource. What the mirror
	 * takes is UTexture::TextureReference.TextureReferenceRHI: an FRHITextureReference,
	 * which is itself an FRHITexture (RHITextureReference.h:7) so it binds as an ordinary
	 * SRV, and which the RHI re-points at the new resource on every resource recreation —
	 * "a FRHITextureReference to update whenever the FTexture::TextureRHI changes… It
	 * allows to prevent dereferencing the UAsset pointers when updating a texture
	 * resource" (TextureResource.h:169-171), written only from
	 * FDynamicRHI::RHIUpdateTextureReference (RHITextureReference.h:47-51). So a render
	 * target that resizes, or a streamed texture that swaps mips, keeps this binding.
	 * VaCuusWorldComponent.h:40 already leans on the same property for its material.
	 *
	 * THE REFERENCE IS TAKEN ON THE RENDER THREAD, not here, and that ordering is why a
	 * caller may register a texture in the same function that created it.
	 * TextureReferenceRHI is created by FTextureReference::InitRHI — a render command — so
	 * on this thread, right after UpdateResource(), it is still null. What is checked here
	 * instead is GetResource(), which UpdateResource() sets inline (Texture.cpp:336-339):
	 * a caller who never ran it is REFUSED BY NAME rather than silently bound to nothing,
	 * because the alternative — a black rectangle with no log — is the failure mode this
	 * whole file is trying to avoid.
	 *
	 * bLive is the cost dial and it defaults OFF: see MarkTextureDirty.
	 */
	static bool RegisterTexture(const FString& Key, UTexture* Texture, bool bLive = false,
		EVaCuusTextureEncoding Encoding = EVaCuusTextureEncoding::Auto,
		EVaCuusTextureAlpha Alpha = EVaCuusTextureAlpha::Auto);

	/**
	 * GAME THREAD. Removes the key and republishes. THE DEFERRED-RELEASE DISCIPLINE, the
	 * style registry's verbatim: live views may still carry draws naming this id, so the
	 * game-side root is parked behind a render fence begun AFTER the mirror replacement
	 * and dropped only once the render thread has provably stopped resolving it. A draw
	 * whose id no longer resolves binds the RHI's global black texture
	 * (RHITextureReference.h:59-63) and counts itself — the named refusal, not a crash.
	 */
	static void UnregisterTexture(const FString& Key);

	/**
	 * GAME THREAD. "The pixels behind this key changed once" — costs the views that
	 * actually draw it exactly ONE published frame.
	 *
	 * WHY THIS EXISTS AT ALL. A texture's pixels change without changing one byte of the
	 * recorded command stream, so neither term of the idle gate can see it: not the
	 * content hash, not the resource-traffic predicate. Left alone, a document showing a
	 * render target publishes once and freezes on whatever it had. This is the same class
	 * of problem as a live material decorator and it takes the same remedy
	 * (VaCuusRecordingRenderInterface.cpp:1626-1665) — the only question is what triggers
	 * it, and an explicit signal is the honest answer: the game knows when it captured.
	 *
	 * bLive at registration is sugar for "dirty every frame" and is IMPLEMENTED as that,
	 * so there is one mechanism and one place for it to be wrong.
	 */
	static void MarkTextureDirty(const FString& Key);

	/** GAME THREAD. Current version; 0 = nothing ever registered. */
	static uint64 GetVersion_GameThread();

	/** GAME THREAD. Live registered keys. */
	static int32 GetNumEntries_GameThread();

	/** GAME THREAD. Unregistered roots still held behind their render fence. */
	static int32 GetNumPendingReleases_GameThread();

	/** GAME THREAD. Keys refused because their id was already held by another key. */
	static int32 GetNumCollisionsRefused_GameThread();

	/** GAME THREAD. The current immutable snapshot; null before the first registration. */
	static TSharedPtr<const FVaCuusTextureSnapshot> GetSnapshot_GameThread();

	/** GAME THREAD, once per subsystem tick: drops roots whose fence completed. */
	static void TickDeferredReleases_GameThread();

	/** GAME THREAD. Re-enqueues the current state to a (re)started UI thread. */
	static void PublishToUIThread(FVaCuusUIThread& UIThread);

	/**
	 * THE UI-FRAME-THREAD INSTALL — the drain's handler for a SetTextureSnapshot command,
	 * and the test seam for recorders driven on the test thread. Same single-owner
	 * contract as the style registry's: whichever thread drives recorders owns this.
	 */
	static void InstallSnapshot(const TSharedPtr<const FVaCuusTextureSnapshot>& Snapshot);

	/** The installed snapshot, same thread contract as InstallSnapshot. Null = none yet. */
	static TSharedPtr<const FVaCuusTextureSnapshot> GetInstalledSnapshot();

	/**
	 * UI-FRAME THREAD. The monotonic dirty counter for one id.
	 *
	 * A COUNTER RATHER THAN A FLAG, and that is what makes "exactly one publish" true for
	 * every view independently. A flag would have to be cleared, and nothing can know when
	 * every view has seen it — a two-view document would drop one view's refresh or
	 * refresh the other forever. A recorder instead remembers the value it last published
	 * at, per handle, and republishes while the two differ. Nothing is ever cleared.
	 */
	static uint64 GetDirtyCounter_UIThread(uint64 StableId);

	/** UI-FRAME THREAD. The drain's handler for a MarkTextureDirty command. */
	static void MarkDirty_UIThread(uint64 StableId);

	/**
	 * RENDER THREAD. The binding for a stable id, or false once unregistered / never
	 * known. Out is untouched on a miss.
	 */
	static bool ResolveBinding_RenderThread(uint64 StableId, FVaCuusExternalTextureBinding& Out);

	/**
	 * GAME THREAD, module shutdown: flushes rendering commands, clears the mirror and
	 * every root (registered and pending alike). After this the registry is empty but
	 * versions keep counting — they must never regress.
	 */
	static void Shutdown_GameThread();

	/** GAME THREAD. Every live key with its id, size and mode — what vacuus.TextureRegistry prints. */
	static void DescribeEntries_GameThread(TArray<FString>& OutLines);
};
