// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusTranslation.h"

#include "VaCuus.h"
#include "VaCuusDefines.h"
#include "VaCuusUIThread.h"

namespace
{
// ---------------------------------------------------------------------------------------
// GAME-THREAD state (registration rate). The style registry's shape
// (VaCuusStyleSet.cpp) minus the entries/mirror it does not need: the whole table
// arrives at once, so the snapshot IS the state.
// ---------------------------------------------------------------------------------------

/** Monotonic across the process; survives table replacement and UI-thread restarts. */
uint64 GTranslationVersion = 0;

TSharedPtr<const FVaCuusTranslationSnapshot> GCurrentTranslationSnapshot;

// ---------------------------------------------------------------------------------------
// UI-FRAME-THREAD state: the installed snapshot. Same single-owner contract as the
// style registry's GInstalledSnapshot (VaCuusStyleSet.cpp) — production installs from
// the drain and reads from the JS thunk / TranslateString; a rig test's closures run
// on the same UI thread. Distinctly named: unity builds merge this anonymous
// namespace with VaCuusStyleSet.cpp's into one TU.
// ---------------------------------------------------------------------------------------
TSharedPtr<const FVaCuusTranslationSnapshot> GInstalledTranslationSnapshot;
}	 // namespace

void FVaCuusTranslationRegistry::SetTable(const TMap<FString, FString>& Table)
{
	check(IsInGameThread());

	++GTranslationVersion;

	TSharedRef<FVaCuusTranslationSnapshot> Snapshot = MakeShared<FVaCuusTranslationSnapshot>();
	Snapshot->Version = GTranslationVersion;
	Snapshot->Table = Table;
	GCurrentTranslationSnapshot = Snapshot;

	UE_LOG(LogVaCuus, Log, TEXT("VaCuus translation: published table v%llu (%d entries)"), GTranslationVersion, Table.Num());

	// The queue crossing (never a lock): only when a UI thread is up; a thread started
	// later gets the same snapshot from PublishToUIThread in GetOrStartUIThread.
	if (FVaCuusModule* Module = FVaCuusModule::GetPtr())
	{
		if (FVaCuusUIThread* UIThread = Module->GetUIThread())
		{
			UIThread->EnqueueSetTranslationSnapshot(GCurrentTranslationSnapshot);
		}
	}
}

uint64 FVaCuusTranslationRegistry::GetVersion_GameThread()
{
	check(IsInGameThread());
	return GTranslationVersion;
}

TSharedPtr<const FVaCuusTranslationSnapshot> FVaCuusTranslationRegistry::GetSnapshot_GameThread()
{
	check(IsInGameThread());
	return GCurrentTranslationSnapshot;
}

void FVaCuusTranslationRegistry::PublishToUIThread(FVaCuusUIThread& UIThread)
{
	check(IsInGameThread());

	if (GCurrentTranslationSnapshot.IsValid())
	{
		UIThread.EnqueueSetTranslationSnapshot(GCurrentTranslationSnapshot);
	}
}

void FVaCuusTranslationRegistry::InstallSnapshot(const TSharedPtr<const FVaCuusTranslationSnapshot>& Snapshot)
{
	checkf(Snapshot.IsValid(), TEXT("InstallSnapshot(null) — the drain never enqueues one and neither may a test"));

	// THE IMMUTABILITY OBSERVABLE (the style snapshot's rule, FVaCuusStyleRegistry::
	// InstallSnapshot): publish-by-replacement means versions only move forward.
	// Equality is legal — the same snapshot re-published to a restarted UI thread — a
	// regression means somebody mutated or replayed history.
	checkf(!GInstalledTranslationSnapshot.IsValid() || Snapshot->Version >= GInstalledTranslationSnapshot->Version,
		TEXT("Translation snapshot version regressed (%llu after %llu) — snapshots are publish-by-replacement and immutable"),
		Snapshot->Version, GInstalledTranslationSnapshot.IsValid() ? GInstalledTranslationSnapshot->Version : 0);

	GInstalledTranslationSnapshot = Snapshot;
}

TSharedPtr<const FVaCuusTranslationSnapshot> FVaCuusTranslationRegistry::GetInstalledSnapshot()
{
	return GInstalledTranslationSnapshot;
}

bool FVaCuusTranslationRegistry::TranslateKey(const FString& Key, FString& Out)
{
	if (!GInstalledTranslationSnapshot.IsValid())
	{
		return false;
	}

	if (const FString* Found = GInstalledTranslationSnapshot->Table.Find(Key))
	{
		Out = *Found;
		return true;
	}
	return false;
}
