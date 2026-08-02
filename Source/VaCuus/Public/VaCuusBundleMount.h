// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusBundle.h"

#include "Serialization/BulkData.h"

#include <atomic>

class UVaCuusBundle;

/**
 * One mounted (or once-mounted) bundle: the index lookup plus the payload region and
 * ITS OWNERSHIP. Records are immutable after construction except for the served-opens
 * counter -- readers on the UI thread see a frozen index over a frozen region.
 *
 * WHY THE RECORD, NOT THE ASSET, OWNS THE REGION (spec M6 2(b)): mounting STEALS the
 * payload via FBulkData::StealFileMapping (BulkData.h:863-867), which transfers either
 * the mapped handle/region pair or the raw allocation and NULLS the source
 * (BulkData.cpp:446-462) -- destructive and once-only. After the steal the UVaCuusBundle
 * can be GC'd; this record is what keeps the bytes alive, and every open VFS handle
 * holds a TSharedPtr to it, so an unmount cannot pull the region out from under a read
 * in flight (Exp-BUNDLE-UNMOUNT-RACE is that claim's observable).
 *
 * Destructor ordering inside FOwnedBulkDataPtr is region-before-handle
 * (BulkData.cpp:555-569), which is the order IMappedFileHandle requires.
 */
struct FVaCuusBundleMount
{
	/** Asset path name for steal-backed mounts; a "<...>" tag for transient ones. The unmount/dump key. */
	FString BundleName;

	/** SourceNote for cooked bundles; a pack-on-demand note for transient ones. */
	FString Provenance;

	/** Lowercase hex of the pack's content hash -- the determinism observable, printed by vacuus.DumpBundle. */
	FString ContentHashHex;

	/** True when the region is a live file mapping (Win64 IoStore); false for the resident path (Linux/macOS). */
	bool bMemoryMapped = false;

	/** The stolen region (cooked mounts). Null for transient mounts, which own TransientBytes instead. */
	TUniquePtr<FOwnedBulkDataPtr> OwnedRegion;

	/** Pack-on-demand / test backing. Empty for cooked mounts. */
	TArray64<uint8> TransientBytes;

	/** The payload span every entry offsets into. Valid for the record's whole life. */
	const uint8* Base = nullptr;
	int64 PayloadSize = 0;

	/** Sorted by path (the pack's order); bounds-validated against PayloadSize BEFORE the record was published. */
	TArray<FVaCuusBundleEntry> Entries;

	/** Normalized path -> index into Entries. Built once at mount. */
	TMap<FString, int32> PathToEntry;

	/** Opens served from this record, ever. Any thread; part of the M==0 serving observability. */
	std::atomic<uint64> ServedOpens{0};

	/** Entry for an ALREADY-NORMALIZED path, or null. Any thread (the map is frozen after mount). */
	const FVaCuusBundleEntry* FindEntry(const FString& NormalizedPath) const
	{
		const int32* EntryIndex = PathToEntry.Find(NormalizedPath);
		return EntryIndex ? &Entries[*EntryIndex] : nullptr;
	}
};

/** The published lookup: mounted records in MOUNT ORDER. First hit wins (spec M6 2(d)). Immutable once published. */
struct FVaCuusBundleLookup
{
	TArray<TSharedRef<FVaCuusBundleMount>> Mounts;
};

/**
 * The process-wide mount table -- a core static beside the content paths rather than
 * subsystem state, because everything that reads it is process-wide too: the VFS (one
 * RmlUi file interface per process), the UI thread that calls it, and N game instances
 * whose subsystems all mount the same config-listed bundle (idempotently). The
 * subsystem carries thin MountBundle/UnmountBundle doors to this, the RegisterStyleSet
 * pattern.
 *
 * THREADING (spec M6 section 4): mounts and unmounts are GAME THREAD only (checked) and
 * publish a new immutable FVaCuusBundleLookup; GetLookup()/ContainsPath() are
 * any-thread (a pointer copy under a short lock) -- the snapshot pattern, so every
 * probe in one Open() sees one frozen table.
 *
 * THE RELEASE RULE (spec M6 2(b)): UnmountBundle removes the LOOKUP entry only; a
 * steal-backed record is RETAINED for the process's lifetime, because the steal cannot
 * be repeated -- a remount reuses the retained record, and the records die in
 * DestroyRecords() at module shutdown, after the UI thread (and so every open handle)
 * is gone. Transient records ARE dropped on unmount: nothing was stolen, and a re-pack
 * is cheap and picks up fresh loose edits.
 */
class VACUUS_API FVaCuusBundleMountTable
{
public:
	/**
	 * Mounts a loaded bundle asset, stealing its payload region on first mount and
	 * reusing the retained record on a re-mount. Idempotent for an already-mounted
	 * bundle. Refuses (one Error each, false returned): a null or load-refused asset,
	 * an asset with no cooked payload (editor saves carry none -- pack-on-demand is
	 * the editor door), an index that fails bounds validation, a payload whose size
	 * disagrees with its index, and the genuine second-steal (null allocation with a
	 * nonzero declared size -- the record that owned the first steal is gone).
	 * Game thread.
	 */
	static bool MountBundle(UVaCuusBundle* Bundle);

	/** Removes the lookup entry for BundleName. The record outlives this (see the class comment). Game thread. */
	static bool UnmountBundle(const FString& BundleName);

	/** Unmounts everything (vacuus.Bundle.Enable 0). Returns how many. Game thread. */
	static int32 UnmountAll();

	/**
	 * Mounts an index + payload that never touched an asset: the PIE pack-on-demand
	 * path and the tests. Validates bounds exactly like MountBundle. Game thread.
	 */
	static bool MountTransient(const FString& Name, const FString& Provenance,
		VaCuusBundleFormat::FCookedIndex&& Index, TArray64<uint8>&& PayloadBytes);

#if WITH_EDITOR
	/**
	 * The PIE parity door (spec M6 2(d), the fix for spec 9's finding 3): the editor
	 * asset has NO payload, so `vacuus.Bundle.Enable 1` in the editor packs the loose
	 * DevUI tree into a transient buffer and mounts THAT -- the bundle read path,
	 * exercised on real content, without a cook. Game thread.
	 */
	static bool MountPackedOnDemand();
#endif

	/** The current immutable lookup (may be null when nothing is mounted). Any thread. */
	static TSharedPtr<const FVaCuusBundleLookup> GetLookup();

	/**
	 * True when any MOUNTED bundle serves RelativePath (normalized here). Any thread.
	 * OutBundleName names the serving bundle -- what the live-reload shadow Warning
	 * and the ResolveExistingDocument bundle probe print.
	 */
	static bool ContainsPath(const FString& RelativePath, FString* OutBundleName = nullptr);

	/** Every record, mounted or retained -- the teardown serving line and vacuus.DumpBundle read these. Any thread. */
	static TArray<TSharedRef<FVaCuusBundleMount>> GetAllRecords();

	/**
	 * Drops the lookup and every retained record. FVaCuusModule::ShutdownModule only,
	 * AFTER StopUIThread(): the regions die here, and by then no open handle can
	 * exist because the UI thread that held them is joined. Game thread.
	 */
	static void DestroyRecords();
};

namespace VaCuusBundleConfig
{
/**
 * The config-listed bundle soft path -- `[VaCuus] BundleAssetPath=/VaCuus/Bundles/...`
 * in the project's *Game.ini (a config-read beside DirectoriesToAlwaysCook, which is
 * the OTHER half every project must set: a config-soft-path-only bundle is invisible
 * to the cooker, spec M6 2(d)). Empty when not configured. A plain config read, not a
 * UDeveloperSettings: the in-tree pattern is the content-roots one -- resolve once,
 * no settings object.
 */
VACUUS_API FString GetConfiguredBundleAssetPath();
}	 // namespace VaCuusBundleConfig

namespace VaCuusScriptServing
{
/*
 * The SCRIPT half of the M==0 serving accounting (spec M6 2(d)). Scripts do not flow
 * through Rml::FileInterface -- VaCuusJs reads <script src> and module files itself
 * (VaCuusJsScriptSource::ReadScriptByVfsPath) -- so FVaCuusFileInterface's own
 * NumBundleOpens/NumLooseOpens counters cannot see them, and without these a loose
 * script serve in a bundle-mounted build would be invisible to the packaged gates'
 * "0 by loose roots" grep: exactly the silent class the M==0 line exists to kill.
 *
 * Process-wide statics rather than members, because the reader and the writer are in
 * different modules with only this one (VaCuusJs depends on VaCuus, never the
 * reverse): the script source bumps them, and ~FVaCuusFileInterface prints them on
 * the teardown line the gates grep. Any thread; monotonic for the process's life.
 */
VACUUS_API void NoteBundleScriptServe();
VACUUS_API void NoteLooseScriptServe();
VACUUS_API uint64 GetNumBundleScriptServes();
VACUUS_API uint64 GetNumLooseScriptServes();
}	 // namespace VaCuusScriptServing
