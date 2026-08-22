// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusTextureRegistry.h"

#include "VaCuus.h"
#include "VaCuusDefines.h"
#include "VaCuusUIThread.h"

#include "Engine/Texture.h"
#include "Engine/TextureRenderTarget.h"
#include "Hash/CityHash.h" // CityHash64, the stable id
#include "Misc/App.h" // FApp::CanEverRender -- see the reference check in RegisterTexture
#include "RenderResource.h" // FTextureReference::IsInitialized_GameThread
#include "RenderCommandFence.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#include "UObject/StrongObjectPtr.h"

/**
 * A NAMED namespace, not an anonymous one, and that is a unity-build requirement rather
 * than a style choice: VaCuusStyleSet.cpp declares GEntries / GVersion / RepublishAll with
 * the same obvious names, and UBT merges both files into one Module.VaCuus.N.cpp where the
 * two anonymous namespaces ARE the same namespace. Found by the compiler, as intended.
 */
namespace VaCuusTextureRegistryPrivate
{
/** One registered key: the root, its identity, and the two resolved modes. */
struct FVaCuusTextureEntry
{
	FString Key;
	TStrongObjectPtr<UTexture> Texture;
	uint64 StableId = 0;
	FIntPoint Size = FIntPoint::ZeroValue;
	bool bLive = false;
	EVaCuusTextureEncoding Encoding = EVaCuusTextureEncoding::Raw;
	EVaCuusTextureAlpha Alpha = EVaCuusTextureAlpha::Opaque;

	/**
	 * The texture's FTextureReference, NOT its FRHITextureReference.
	 *
	 * THE INDIRECTION IS THE FIX for a real ordering trap. TextureReferenceRHI is created by
	 * FTextureReference::InitRHI — on the RENDER THREAD — so immediately after a caller's
	 * UpdateResource() it is still null on the game thread, and a registry that read it here
	 * would refuse every texture registered in the same function that created it. Found by
	 * running the tests against a real RHI, where the -nullrhi suite could not see it.
	 *
	 * The mirror command reads it on the render thread instead, and FIFO is what makes that
	 * correct: UTexture's constructor already enqueued BeginInitResource for this reference
	 * (Texture.cpp:205-208), and both commands come from the game thread, so the init has
	 * necessarily run by the time the mirror command does.
	 *
	 * Safe to dereference there because the entry roots the UTexture that owns it
	 * (Texture.cpp:152 constructs it, :281 deletes it), and unregistration replaces the
	 * mirror before it fences and drops that root.
	 */
	FTextureReference* ReferenceOwner = nullptr;
};

/**
 * Unregistered roots parked behind their fence — FVaCuusPendingRelease in
 * VaCuusStyleSet.cpp, same discipline and the same reason. One batch per unregistration.
 */
struct FVaCuusTexturePendingRelease
{
	TArray<TStrongObjectPtr<UTexture>> Roots;
	FRenderCommandFence Fence;
};

// ---------------------------------------------------------------------------------------
// GAME-THREAD state (registration rate).
// ---------------------------------------------------------------------------------------
TArray<FVaCuusTextureEntry> GEntries;
TArray<FVaCuusTexturePendingRelease> GPendingReleases;
uint64 GVersion = 0;
int32 GCollisionsRefused = 0;
TSharedPtr<const FVaCuusTextureSnapshot> GCurrentSnapshot;

// ---------------------------------------------------------------------------------------
// UI-FRAME-THREAD state. Single-owner by the same contract as the style registry's
// installed snapshot: whichever single thread drives recorders owns both of these.
// ---------------------------------------------------------------------------------------
TSharedPtr<const FVaCuusTextureSnapshot> GInstalledSnapshot;

/**
 * id -> monotonic dirty count. NEVER CLEARED, and never needs to be: a recorder compares
 * against the value it last published at, so "seen" is per view and no producer has to
 * know how many consumers exist. Bounded by the number of keys ever marked dirty.
 */
TMap<uint64, uint64> GDirtyCounters_UIThread;

// ---------------------------------------------------------------------------------------
// RENDER-THREAD state: the binding mirror, replaced wholesale by each registration change.
// ---------------------------------------------------------------------------------------
TMap<uint64, FVaCuusExternalTextureBinding> GMirror_RenderThread;

/** Formats whose sampled values are LINEAR even though no _SRGB flag says so. */
bool IsLinearValuedFormat(EPixelFormat Format)
{
	switch (Format)
	{
	case PF_FloatRGB:
	case PF_FloatRGBA:
	case PF_FloatR11G11B10:
	case PF_A32B32G32R32F:
	case PF_R32_FLOAT:
	case PF_G16R16F:
	case PF_G32R32F:
	case PF_R16F:
		return true;
	default:
		return false;
	}
}

/**
 * Resolve Auto for both modes. The two questions are different and are answered from
 * different facts, which is why this is not one branch:
 *
 *  - ENCODING asks "does the shader receive linear values?". Two ways that happens: the
 *    resource is an _SRGB format and the SAMPLER decoded it, or the format is float and
 *    the content is linear by convention. Either way the pipeline's sRGB-encoded contract
 *    (VaCuusUIShaders.h:52-65) needs the curve re-applied.
 *  - ALPHA asks "what does the A channel mean?". A render target's usually means nothing
 *    at all (see EVaCuusTextureAlpha), an imported asset's is straight.
 */
void ResolveModes(const UTexture* Texture, EVaCuusTextureEncoding& Encoding, EVaCuusTextureAlpha& Alpha)
{
	const UTextureRenderTarget* RenderTarget = Cast<const UTextureRenderTarget>(Texture);

	if (Encoding == EVaCuusTextureEncoding::Auto)
	{
		const bool bSamplerDecodes = RenderTarget ? RenderTarget->IsSRGB() : (Texture->SRGB != 0);
		const bool bLinearFormat = RenderTarget && IsLinearValuedFormat(RenderTarget->GetFormat());
		Encoding = (bSamplerDecodes || bLinearFormat) ? EVaCuusTextureEncoding::EncodeFromLinear : EVaCuusTextureEncoding::Raw;
	}

	if (Alpha == EVaCuusTextureAlpha::Auto)
	{
		Alpha = RenderTarget ? EVaCuusTextureAlpha::Opaque : EVaCuusTextureAlpha::Straight;
	}
}

const TCHAR* EncodingName(EVaCuusTextureEncoding Encoding)
{
	return Encoding == EVaCuusTextureEncoding::EncodeFromLinear ? TEXT("EncodeFromLinear") : TEXT("Raw");
}

const TCHAR* AlphaName(EVaCuusTextureAlpha Alpha)
{
	switch (Alpha)
	{
	case EVaCuusTextureAlpha::Opaque:
		return TEXT("Opaque");
	case EVaCuusTextureAlpha::Premultiplied:
		return TEXT("Premultiplied");
	default:
		return TEXT("Straight");
	}
}

/**
 * Rebuild + publish everything a registry change moves: the game-side snapshot, the
 * render-thread mirror (replacement, via one render command), and the UI-thread snapshot
 * (via the command queue when a UI thread is running; PublishToUIThread covers the thread
 * that starts later). RepublishAll in VaCuusStyleSet.cpp, one for one.
 */
void RepublishAll()
{
	check(IsInGameThread());

	++GVersion;

	TSharedRef<FVaCuusTextureSnapshot> Snapshot = MakeShared<FVaCuusTextureSnapshot>();
	Snapshot->Version = GVersion;
	Snapshot->KeyToBinding.Reserve(GEntries.Num());

	/** {id, the reference to resolve on the render thread, and the two resolved modes}. */
	struct FMirrorRequest
	{
		uint64 StableId = 0;
		FTextureReference* ReferenceOwner = nullptr;
		EVaCuusTextureEncoding Encoding = EVaCuusTextureEncoding::Raw;
		EVaCuusTextureAlpha Alpha = EVaCuusTextureAlpha::Opaque;
	};
	TArray<FMirrorRequest> Mirror;
	Mirror.Reserve(GEntries.Num());

	for (const FVaCuusTextureEntry& Entry : GEntries)
	{
		FVaCuusTextureBinding& Binding = Snapshot->KeyToBinding.Add(Entry.Key);
		Binding.StableId = Entry.StableId;
		Binding.Size = Entry.Size;
		Binding.bLive = Entry.bLive;

		Mirror.Add({Entry.StableId, Entry.ReferenceOwner, Entry.Encoding, Entry.Alpha});
	}

	GCurrentSnapshot = Snapshot;

	ENQUEUE_RENDER_COMMAND(VaCuusTextureMirror)(
		[Mirror = MoveTemp(Mirror)](FRHICommandListImmediate&)
		{
			GMirror_RenderThread.Reset();
			for (const FMirrorRequest& Request : Mirror)
			{
				FVaCuusExternalTextureBinding Binding;

				// RESOLVED HERE, on the render thread, for the reason on
				// FVaCuusTextureEntry::ReferenceOwner. A null reference is not an error and
				// not filtered out: the entry stays in the mirror so the draw path can tell
				// "unregistered" from "registered but not yet renderable", and an
				// FRHITextureReference with nothing behind it draws the RHI's global black
				// texture rather than crashing (RHITextureReference.h:60-65).
				if (Request.ReferenceOwner)
				{
					Binding.Texture = Request.ReferenceOwner->TextureReferenceRHI.GetReference();
				}
				Binding.Encoding = Request.Encoding;
				Binding.Alpha = Request.Alpha;
				GMirror_RenderThread.Add(Request.StableId, MoveTemp(Binding));
			}
		});

	// The queue crossing (never a lock): only when a UI thread is up; a thread started
	// later gets the same snapshot from PublishToUIThread in GetOrStartUIThread.
	if (FVaCuusModule* Module = FVaCuusModule::GetPtr())
	{
		if (FVaCuusUIThread* UIThread = Module->GetUIThread())
		{
			UIThread->EnqueueSetTextureSnapshot(GCurrentSnapshot);
		}
	}
}
} // namespace VaCuusTextureRegistryPrivate

// AN ALIAS, NOT A using-directive. In a unity build this file's file-scope declarations are
// visible to every file compiled after it in the same Module.VaCuus.N.cpp; a using-directive
// would make VaCuusStyleSet.cpp's own unqualified GEntries ambiguous. An alias introduces
// only the name below.
namespace VcTexReg = VaCuusTextureRegistryPrivate;

uint64 FVaCuusTextureRegistry::IdForKey(const FString& Key)
{
	// UTF-8 rather than TCHAR bytes so the id is the same on every platform — TCHAR is
	// 2 bytes on Windows and 4 on Linux, and a key with any non-ASCII character would
	// otherwise hash differently per platform. Nothing today compares ids across
	// machines, but a hash that silently is not the same function is a bad thing to
	// leave lying around.
	const FTCHARToUTF8 Utf8(*Key);
	const uint64 Hash = CityHash64(reinterpret_cast<const char*>(Utf8.Get()), Utf8.Length());

	// 0 is the command buffer's "no texture" sentinel (FVaCuusCommand::Texture), so it
	// must never be a valid id. Folding it to 1 costs one key's worth of collision
	// probability and keeps the sentinel meaning exactly one thing.
	return Hash == 0 ? 1 : Hash;
}

bool FVaCuusTextureRegistry::RegisterTexture(
	const FString& Key, UTexture* Texture, bool bLive, EVaCuusTextureEncoding Encoding, EVaCuusTextureAlpha Alpha)
{
	check(IsInGameThread());

	if (Key.IsEmpty())
	{
		UE_LOG(LogVaCuus, Error, TEXT("RegisterTexture: empty key refused"));
		return false;
	}

	if (!Texture)
	{
		UE_LOG(LogVaCuus, Error, TEXT("RegisterTexture('%s'): null texture refused"), *Key);
		return false;
	}

	const uint64 StableId = IdForKey(Key);

	// THE COLLISION CHECK, and this is the one place in the process where it can happen:
	// both keys are visible, on one thread, with no lock. See IdForKey.
	for (const VcTexReg::FVaCuusTextureEntry& Entry : VcTexReg::GEntries)
	{
		if (Entry.StableId == StableId && Entry.Key != Key)
		{
			++VcTexReg::GCollisionsRefused;
			UE_LOG(LogVaCuus, Error,
				TEXT("RegisterTexture('%s'): stable id %llu is already held by key '%s' — refused. Rename one of them."),
				*Key, StableId, *Entry.Key);
			return false;
		}
	}

	// The reference, not the resource — the whole reason a resized render target keeps
	// working. See the header. A texture whose reference was never initialised from the
	// game thread has had no UpdateResource(), and binding it would be a black rectangle
	// with nothing in any log.
	// .GetReference() rather than the TRefCountPtr directly: TRefCountPtr<FRHITextureReference>
	// does not convert to TRefCountPtr<FRHITexture>, but the RAW pointer does -- an
	// FRHITextureReference IS an FRHITexture (RHITextureReference.h:7), which is the whole
	// reason this binds without a special case in the replayer.
	// GetResource(), NOT the RHI reference: this is the synchronous, game-thread-truthful
	// question. UpdateResource() creates the resource inline (Texture.cpp:336-339) while the
	// RHI reference it will be attached to is only created on the render thread, so asking
	// about the reference here refuses textures that are perfectly fine — see
	// FVaCuusTextureEntry::ReferenceOwner.
	if (Texture->GetResource() == nullptr)
	{
		// THE REFUSAL IS CONDITIONAL ON THE PROCESS BEING ABLE TO RENDER AT ALL, and that is
		// not a loophole -- it is what makes the message true. UpdateResource() creates a
		// resource only under FApp::CanEverRender() (Texture.cpp:336), so in a process where
		// that is false -- -nullrhi, a dedicated server, most commandlets -- NO texture ever
		// has one, and refusing here would refuse every registration for a reason that has
		// nothing to do with the caller while telling them to call a function that would not
		// help. Nothing draws in such a process either.
		//
		// Where rendering IS possible, a missing resource means exactly one thing and the
		// refusal says it: the caller never ran UpdateResource(). Registering anyway would be
		// a black rectangle with nothing in any log, which is the failure this replaces.
		if (FApp::CanEverRender())
		{
			UE_LOG(LogVaCuus, Error,
				TEXT("RegisterTexture('%s'): '%s' has no render resource — call UpdateResource() first. Refused."),
				*Key, *Texture->GetPathName());
			return false;
		}

		UE_LOG(LogVaCuus, Verbose,
			TEXT("RegisterTexture('%s'): registered with no render resource — this process cannot render"), *Key);
	}

	VcTexReg::ResolveModes(Texture, Encoding, Alpha);

	// Re-registration replaces in place, keeping the id: a document already drawing this
	// key must follow the swap, and it can only do that if the id does not move.
	VcTexReg::FVaCuusTextureEntry* Entry = VcTexReg::GEntries.FindByPredicate(
		[&Key](const VcTexReg::FVaCuusTextureEntry& Candidate) { return Candidate.Key == Key; });
	if (!Entry)
	{
		Entry = &VcTexReg::GEntries.AddDefaulted_GetRef();
	}

	Entry->Key = Key;
	Entry->Texture = TStrongObjectPtr<UTexture>(Texture);
	Entry->StableId = StableId;
	Entry->Size = FIntPoint(int32(Texture->GetSurfaceWidth()), int32(Texture->GetSurfaceHeight()));
	Entry->bLive = bLive;
	Entry->Encoding = Encoding;
	Entry->Alpha = Alpha;
	Entry->ReferenceOwner = &Texture->TextureReference;

	VcTexReg::RepublishAll();

	UE_LOG(LogVaCuus, Log, TEXT("RegisterTexture('%s'): '%s' %dx%d, %s, %s, %s — id %llu, snapshot v%llu (%d live total)"),
		*Key, *Texture->GetPathName(), Entry->Size.X, Entry->Size.Y, bLive ? TEXT("live") : TEXT("static"),
		VcTexReg::EncodingName(Encoding), VcTexReg::AlphaName(Alpha), StableId, VcTexReg::GVersion, VcTexReg::GEntries.Num());
	return true;
}

void FVaCuusTextureRegistry::UnregisterTexture(const FString& Key)
{
	check(IsInGameThread());

	const int32 Index = VcTexReg::GEntries.IndexOfByPredicate(
		[&Key](const VcTexReg::FVaCuusTextureEntry& Candidate) { return Candidate.Key == Key; });
	if (Index == INDEX_NONE)
	{
		UE_LOG(LogVaCuus, Verbose, TEXT("UnregisterTexture('%s'): nothing registered under it"), *Key);
		return;
	}

	VcTexReg::FVaCuusTexturePendingRelease Pending;
	Pending.Roots.Add(MoveTemp(VcTexReg::GEntries[Index].Texture));
	VcTexReg::GEntries.RemoveAt(Index);

	// Replacement mirror first, then the fence: BeginFence() enqueues behind the mirror
	// command, so a completed fence proves the render thread can no longer resolve the
	// removed id — only then may the root drop. Same AFTER-the-last-consumer shape as
	// UnregisterStyleSet and as RetireBufferResources' deferred release.
	VcTexReg::RepublishAll();
	Pending.Fence.BeginFence();
	VcTexReg::GPendingReleases.Add(MoveTemp(Pending));

	UE_LOG(LogVaCuus, Log,
		TEXT("UnregisterTexture('%s'): removed, snapshot v%llu (%d live; live draws naming it now bind black and count)"),
		*Key, VcTexReg::GVersion, VcTexReg::GEntries.Num());
}

void FVaCuusTextureRegistry::MarkTextureDirty(const FString& Key)
{
	check(IsInGameThread());

	// NOT GATED ON REGISTRATION, deliberately. The id is a pure function of the key
	// (IdForKey), so a game that marks dirty before it registers — or after it
	// unregisters — is harmless: the counter moves, and any recorder drawing that key
	// republishes once. Refusing here would only add an ordering rule nobody needs.
	const uint64 StableId = IdForKey(Key);

	if (FVaCuusModule* Module = FVaCuusModule::GetPtr())
	{
		if (FVaCuusUIThread* UIThread = Module->GetUIThread())
		{
			UIThread->EnqueueMarkTextureDirty(StableId);
			return;
		}
	}

	// No UI thread: nothing is drawing, so there is nothing to refresh. A counter bumped
	// now would be lost with the (not yet existing) thread's state anyway.
	UE_LOG(LogVaCuus, Verbose, TEXT("MarkTextureDirty('%s'): no UI thread running, ignored"), *Key);
}

uint64 FVaCuusTextureRegistry::GetVersion_GameThread()
{
	check(IsInGameThread());
	return VcTexReg::GVersion;
}

int32 FVaCuusTextureRegistry::GetNumEntries_GameThread()
{
	check(IsInGameThread());
	return VcTexReg::GEntries.Num();
}

int32 FVaCuusTextureRegistry::GetNumPendingReleases_GameThread()
{
	check(IsInGameThread());
	return VcTexReg::GPendingReleases.Num();
}

int32 FVaCuusTextureRegistry::GetNumCollisionsRefused_GameThread()
{
	check(IsInGameThread());
	return VcTexReg::GCollisionsRefused;
}

TSharedPtr<const FVaCuusTextureSnapshot> FVaCuusTextureRegistry::GetSnapshot_GameThread()
{
	check(IsInGameThread());
	return VcTexReg::GCurrentSnapshot;
}

void FVaCuusTextureRegistry::TickDeferredReleases_GameThread()
{
	check(IsInGameThread());

	for (int32 Index = VcTexReg::GPendingReleases.Num() - 1; Index >= 0; --Index)
	{
		if (VcTexReg::GPendingReleases[Index].Fence.IsFenceComplete())
		{
			VcTexReg::GPendingReleases.RemoveAt(Index);
		}
	}
}

void FVaCuusTextureRegistry::PublishToUIThread(FVaCuusUIThread& UIThread)
{
	check(IsInGameThread());

	if (VcTexReg::GCurrentSnapshot.IsValid())
	{
		UIThread.EnqueueSetTextureSnapshot(VcTexReg::GCurrentSnapshot);
	}
}

void FVaCuusTextureRegistry::InstallSnapshot(const TSharedPtr<const FVaCuusTextureSnapshot>& Snapshot)
{
	checkf(Snapshot.IsValid(), TEXT("InstallSnapshot(null) — the drain never enqueues one and neither may a test"));

	// The immutability observable, FVaCuusStyleRegistry::InstallSnapshot's verbatim:
	// publish-by-replacement means versions only ever move forward. Equality is legal (a
	// re-publish to a restarted UI thread); a REGRESSION means somebody mutated a
	// published snapshot or replayed history.
	checkf(!VcTexReg::GInstalledSnapshot.IsValid() || Snapshot->Version >= VcTexReg::GInstalledSnapshot->Version,
		TEXT("Texture snapshot version regressed (%llu after %llu) — snapshots are publish-by-replacement and immutable"),
		Snapshot->Version, VcTexReg::GInstalledSnapshot.IsValid() ? VcTexReg::GInstalledSnapshot->Version : 0);

	VcTexReg::GInstalledSnapshot = Snapshot;
}

TSharedPtr<const FVaCuusTextureSnapshot> FVaCuusTextureRegistry::GetInstalledSnapshot()
{
	return VcTexReg::GInstalledSnapshot;
}

uint64 FVaCuusTextureRegistry::GetDirtyCounter_UIThread(uint64 StableId)
{
	const uint64* Found = VcTexReg::GDirtyCounters_UIThread.Find(StableId);
	return Found ? *Found : 0;
}

void FVaCuusTextureRegistry::MarkDirty_UIThread(uint64 StableId)
{
	++VcTexReg::GDirtyCounters_UIThread.FindOrAdd(StableId);
}

bool FVaCuusTextureRegistry::ResolveBinding_RenderThread(uint64 StableId, FVaCuusExternalTextureBinding& Out)
{
	check(IsInRenderingThread());

	if (const FVaCuusExternalTextureBinding* Found = VcTexReg::GMirror_RenderThread.Find(StableId))
	{
		Out = *Found;
		return true;
	}
	return false;
}

void FVaCuusTextureRegistry::Shutdown_GameThread()
{
	check(IsInGameThread());

	VcTexReg::GEntries.Empty();
	VcTexReg::GCurrentSnapshot.Reset();
	VcTexReg::GInstalledSnapshot.Reset();
	VcTexReg::GDirtyCounters_UIThread.Empty();

	// UNLIKE the style mirror, this one HOLDS RHI REFERENCES, so clearing it is a real
	// resource release and must happen on the render thread before the roots below drop.
	if (GIsThreadedRendering || IsInGameThread())
	{
		ENQUEUE_RENDER_COMMAND(VaCuusTextureMirrorShutdown)(
			[](FRHICommandListImmediate&) { VcTexReg::GMirror_RenderThread.Empty(); });
		FlushRenderingCommands();
	}
	else
	{
		VcTexReg::GMirror_RenderThread.Empty();
	}

	// The flush above completed every fence too.
	VcTexReg::GPendingReleases.Empty();
}

void FVaCuusTextureRegistry::DescribeEntries_GameThread(TArray<FString>& OutLines)
{
	check(IsInGameThread());

	for (const VcTexReg::FVaCuusTextureEntry& Entry : VcTexReg::GEntries)
	{
		OutLines.Add(FString::Printf(TEXT("  '%s' id=%llu %dx%d %s %s %s -> %s"), *Entry.Key, Entry.StableId, Entry.Size.X,
			Entry.Size.Y, Entry.bLive ? TEXT("live") : TEXT("static"), VcTexReg::EncodingName(Entry.Encoding), VcTexReg::AlphaName(Entry.Alpha),
			Entry.Texture.IsValid() ? *Entry.Texture->GetPathName() : TEXT("<dead>")));
	}
}
