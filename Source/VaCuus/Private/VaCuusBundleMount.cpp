// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusBundleMount.h"

#include "VaCuusContentPaths.h"
#include "VaCuusDefines.h"

#include "Async/MappedFileHandle.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProperties.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DateTime.h"
#include "Misc/ScopeLock.h"
#include "UObject/UObjectGlobals.h"

namespace VaCuusBundleMountPrivate
{
/**
 * Guards the published lookup pointer and the record list. Held only for pointer
 * swaps and list walks -- the heavy work (validation, the steal, a pack) happens on
 * the game thread outside any reader's critical path, and readers only ever copy the
 * TSharedPtr out. The records themselves are immutable after publication (the
 * FVaCuusBundleMount contract), so no lock guards their contents.
 */
static FCriticalSection GLock;

/** The immutable published lookup; replaced whole on every mount/unmount (the snapshot pattern, spec M6 section 4). */
static TSharedPtr<const FVaCuusBundleLookup> GLookup;

/**
 * Every record that ever stole a payload region, PLUS the currently-mounted transient
 * ones. Steal-backed records stay here past their unmount -- the steal is once-only,
 * so the record is the only thing that can ever serve that asset again (spec M6 2(b)).
 * Transient records leave on unmount: nothing was stolen and a re-pack is cheap.
 */
static TArray<TSharedRef<FVaCuusBundleMount>> GRecords;

static bool IsMountedLocked(const FString& BundleName)
{
	if (!GLookup.IsValid())
	{
		return false;
	}
	for (const TSharedRef<FVaCuusBundleMount>& Mount : GLookup->Mounts)
	{
		if (Mount->BundleName == BundleName)
		{
			return true;
		}
	}
	return false;
}

static TSharedRef<FVaCuusBundleMount>* FindRecordLocked(const FString& BundleName)
{
	return GRecords.FindByPredicate(
		[&BundleName](const TSharedRef<FVaCuusBundleMount>& Record) { return Record->BundleName == BundleName; });
}

/** Publishes a new lookup = the current one plus Mount at the end (mount order; first hit wins). */
static void PublishMountLocked(const TSharedRef<FVaCuusBundleMount>& Mount)
{
	TSharedRef<FVaCuusBundleLookup> NewLookup = MakeShared<FVaCuusBundleLookup>();
	if (GLookup.IsValid())
	{
		NewLookup->Mounts = GLookup->Mounts;
	}
	NewLookup->Mounts.Add(Mount);
	GLookup = NewLookup;
}

/** Builds the lookup/entry map shared by both mount paths, and the one mount log line. */
static void FinishAndPublishLocked(const TSharedRef<FVaCuusBundleMount>& Mount)
{
	Mount->PathToEntry.Reserve(Mount->Entries.Num());
	for (int32 EntryIndex = 0; EntryIndex < Mount->Entries.Num(); ++EntryIndex)
	{
		Mount->PathToEntry.Add(Mount->Entries[EntryIndex].Path, EntryIndex);
	}

	GRecords.Add(Mount);
	PublishMountLocked(Mount);

	// The mapped-vs-resident branch is SAID, not implied: the packaged Linux gate
	// greps this line for the resident path (the !IsMapped branch, spec M6 2(b)).
	//
	// The resident branch reports THE PROPERTY IT READ, not a platform it assumes.
	// "Only Win64 maps" is false and was false when it was written: three of the four
	// property structs answer true -- Win64 (WindowsPlatformProperties.h:76-79),
	// Android (AndroidPlatformProperties.h:118-121) and iOS
	// (IOSPlatformProperties.h:71-74). Only Linux and Mac map to false, and by
	// inheritance rather than choice: neither declares the member, so both get
	// FGenericPlatformProperties::SupportsMemoryMappedFiles() (GenericPlatformProperties.h:258-261).
	//
	// Why the branch and not one string: on a platform that CAN map, "resident" is not
	// a platform fact, it is a failure -- the mapping was refused or was never requested
	// at cook -- and this is the line someone reads at 2am with no source tree open. A
	// line that blames a property which is actually true sends that reader to the wrong
	// half of the system. So each branch states only what is true where it prints.
	UE_LOG(LogVaCuus, Log, TEXT("Mounted bundle '%s': %d entries, %lld bytes, %s, hash %s"),
		*Mount->BundleName, Mount->Entries.Num(), Mount->PayloadSize,
		Mount->bMemoryMapped
			? TEXT("memory-mapped region")
			: (FPlatformProperties::SupportsMemoryMappedFiles()
					? TEXT("resident buffer (this platform DOES support memory mapping -- the load returned no mapped "
						   "region, so the mapping was refused or the payload was not cooked memory-mappable)")
					: TEXT("resident buffer (FPlatformProperties::SupportsMemoryMappedFiles() is false on this platform)")),
		*Mount->ContentHashHex);
}
}	 // namespace VaCuusBundleMountPrivate

bool FVaCuusBundleMountTable::MountBundle(UVaCuusBundle* Bundle)
{
	using namespace VaCuusBundleMountPrivate;
	check(IsInGameThread());

	if (Bundle == nullptr)
	{
		UE_LOG(LogVaCuus, Error, TEXT("MountBundle refused: null bundle"));
		return false;
	}

	const FString BundleName = Bundle->GetPathName();
	FScopeLock Lock(&GLock);

	if (IsMountedLocked(BundleName))
	{
		// Idempotent by design: N game instances all auto-mount the one config-listed
		// bundle at their subsystem's Initialize.
		UE_LOG(LogVaCuus, Verbose, TEXT("Bundle '%s' is already mounted"), *BundleName);
		return true;
	}

	if (TSharedRef<FVaCuusBundleMount>* Retained = FindRecordLocked(BundleName))
	{
		// The re-mount path the steal-once rule forces: the region was stolen into
		// this record on the first mount and CANNOT be stolen again, so the record is
		// re-published as-is (spec M6 2(b), spec 9's finding 4).
		PublishMountLocked(*Retained);
		UE_LOG(LogVaCuus, Log,
			TEXT("Re-mounted bundle '%s' from its retained record (%d entries; the payload steal is once-only, so the ")
			TEXT("record is reused rather than re-stolen)"),
			*BundleName, (*Retained)->Entries.Num());
		return true;
	}

	if (Bundle->WasCookedLoadRefused())
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("MountBundle refused: '%s' failed its cooked-index load (see the Error logged at load time)"),
			*BundleName);
		return false;
	}

	const VaCuusBundleFormat::FCookedIndex& CookedIndex = Bundle->GetCookedIndex();
	FByteBulkData& Payload = Bundle->GetPayload();
	const int64 BulkSize = Payload.GetBulkDataSize();

	if (CookedIndex.Entries.Num() == 0 && BulkSize == 0)
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("MountBundle refused: '%s' carries no cooked payload. Editor saves carry none by design -- in the ")
			TEXT("editor, `vacuus.Bundle.Enable 1` packs the loose tree on demand instead"),
			*BundleName);
		return false;
	}

	if (CookedIndex.PayloadSize != BulkSize)
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("MountBundle refused: '%s' index declares %lld payload bytes but the asset carries %lld"),
			*BundleName, CookedIndex.PayloadSize, BulkSize);
		return false;
	}

	// The bounds gate (spec M6 2(a)): after this, every FindEntry hit is a span the
	// clamped reads below can trust without re-checking.
	if (!VaCuusBundleFormat::ValidateEntries(CookedIndex.Entries, BulkSize, *BundleName))
	{
		return false;
	}

	if (BulkSize > 0 && !Payload.IsBulkDataLoaded())
	{
		// Defensive: inline payloads (Linux/macOS) load during Serialize, and a Win64
		// mapping failure already fell back to resident inside the loader -- but a
		// payload that is somehow neither must be made resident before the steal
		// wraps whatever allocation exists.
		Payload.ForceBulkDataResident();
	}

	TUniquePtr<FOwnedBulkDataPtr> OwnedRegion(Payload.StealFileMapping());
	const uint8* Base = OwnedRegion.IsValid() ? static_cast<const uint8*>(OwnedRegion->GetPointer()) : nullptr;

	if (Base == nullptr && BulkSize > 0)
	{
		// The genuine second steal (spec M6 2(b)): a nonzero declared size whose
		// allocation is already gone means some earlier steal took it and its record
		// no longer exists -- refusing is the only honest answer, because the bytes
		// are unreachable through this object forever.
		UE_LOG(LogVaCuus, Error,
			TEXT("MountBundle refused: '%s' declares %lld payload bytes but its allocation is already stolen ")
			TEXT("(StealFileMapping is destructive and once-only; the record that owns the first steal is gone)"),
			*BundleName, BulkSize);
		return false;
	}

	TSharedRef<FVaCuusBundleMount> Mount = MakeShared<FVaCuusBundleMount>();
	Mount->BundleName = BundleName;
	Mount->Provenance = Bundle->SourceNote;
	Mount->ContentHashHex = VaCuusBundleFormat::HashToHex(CookedIndex.ContentHash);
	Mount->bMemoryMapped = OwnedRegion.IsValid() && OwnedRegion->IsDataMemoryMapped();
	Mount->Base = Base;
	Mount->PayloadSize = BulkSize;
	Mount->Entries = CookedIndex.Entries;

	if (Mount->bMemoryMapped)
	{
		// Prefault the whole region so page-fault IO does not land on the UI thread
		// mid-document-load. "Hint" is literal and the word carries the caveat: the
		// base IMappedFileRegion::PreloadHint is an EMPTY BODY (MappedFileHandle.h:75-77)
		// and the header says so ("some platforms might ignore it", :70). What each
		// platform we can reach actually does:
		//   Win64 -- FMappedFileRegionWindows overrides it and touches one byte per page
		//            (WindowsPlatformFile.cpp:984, :1000-1017). This is the only platform
		//            where our mapped branch runs today, so this call is why it is here.
		//   Apple -- overridden with the same per-page touch loop (ApplePlatformFile.cpp:415-430).
		//   Android -- NOT overridden: FAndroidMappedFileRegion declares no PreloadHint
		//            (AndroidPlatformFile.cpp:1324-1339), so this call compiles to the
		//            empty base and prefaults nothing. Android's mechanism would be
		//            MAP_POPULATE at map time, which nothing here requests.
		// And the cost claim is desktop-shaped: this is SYNCHRONOUS blocking IO on the
		// GAME thread. A <=10 MB bundle is pennies off an SSD; off cold phone storage it
		// is a real hitch, so if the mapped branch ever runs on a phone this call is the
		// first thing to move or drop.
		OwnedRegion->GetMappedRegion()->PreloadHint(0, BulkSize);
	}
	Mount->OwnedRegion = MoveTemp(OwnedRegion);

	FinishAndPublishLocked(Mount);
	return true;
}

bool FVaCuusBundleMountTable::MountTransient(const FString& Name, const FString& Provenance,
	VaCuusBundleFormat::FCookedIndex&& Index, TArray64<uint8>&& PayloadBytes)
{
	using namespace VaCuusBundleMountPrivate;
	check(IsInGameThread());

	if (Index.PayloadSize != PayloadBytes.Num())
	{
		UE_LOG(LogVaCuus, Error, TEXT("MountTransient refused: '%s' index declares %lld payload bytes but %lld arrived"),
			*Name, Index.PayloadSize, static_cast<int64>(PayloadBytes.Num()));
		return false;
	}

	if (!VaCuusBundleFormat::ValidateEntries(Index.Entries, PayloadBytes.Num(), *Name))
	{
		return false;
	}

	FScopeLock Lock(&GLock);

	// A transient re-mount REPLACES: the whole point of packing on demand is picking
	// up fresh loose edits, so the stale record leaves the lookup and the list. Any
	// open handle still holds its own reference to the old record -- the bytes
	// outlive the replacement, exactly as they outlive an unmount.
	if (TSharedRef<FVaCuusBundleMount>* Existing = FindRecordLocked(Name))
	{
		UE_LOG(LogVaCuus, Log, TEXT("Transient bundle '%s' is being replaced by a fresh pack"), *Name);
		const TSharedRef<FVaCuusBundleMount> Old = *Existing;
		GRecords.Remove(Old);
		if (GLookup.IsValid())
		{
			TSharedRef<FVaCuusBundleLookup> NewLookup = MakeShared<FVaCuusBundleLookup>();
			NewLookup->Mounts = GLookup->Mounts;
			NewLookup->Mounts.Remove(Old);
			GLookup = NewLookup;
		}
	}

	TSharedRef<FVaCuusBundleMount> Mount = MakeShared<FVaCuusBundleMount>();
	Mount->BundleName = Name;
	Mount->Provenance = Provenance;
	Mount->ContentHashHex = VaCuusBundleFormat::HashToHex(Index.ContentHash);
	Mount->bMemoryMapped = false;
	Mount->TransientBytes = MoveTemp(PayloadBytes);
	Mount->Base = Mount->TransientBytes.GetData();
	Mount->PayloadSize = Mount->TransientBytes.Num();
	Mount->Entries = MoveTemp(Index.Entries);

	FinishAndPublishLocked(Mount);
	return true;
}

#if WITH_EDITOR
bool FVaCuusBundleMountTable::MountPackedOnDemand()
{
	check(IsInGameThread());

	int32 NumShadowed = 0;
	int32 NumTestsExcluded = 0;
	TArray<VaCuusBundlePack::FSourceFile> Files = VaCuusBundlePack::EnumerateTree(
		VaCuusContentPaths::GetDocumentRoots(), &NumShadowed, &NumTestsExcluded);

	VaCuusBundleFormat::FCookedIndex Index;
	TArray64<uint8> PayloadBytes;
	FString Error;
	if (!VaCuusBundlePack::Pack(MoveTemp(Files), Index, PayloadBytes, &Error))
	{
		UE_LOG(LogVaCuus, Error, TEXT("Pack-on-demand FAILED: %s"), *Error);
		return false;
	}

	if (Index.Entries.Num() == 0)
	{
		UE_LOG(LogVaCuus, Warning, TEXT("Pack-on-demand found nothing to pack under: %s"),
			*FString::Join(VaCuusContentPaths::GetDocumentRoots(), TEXT(" | ")));
		return false;
	}

	// The timestamp is fine HERE (and would not be in SourceNote or the cook): a
	// transient mount is never serialized, so nothing about determinism is at stake.
	const FString Provenance = FString::Printf(TEXT("packed on demand at %s from: %s (%d shadowed, %d test fixtures excluded)"),
		*FDateTime::Now().ToString(), *FString::Join(VaCuusContentPaths::GetDocumentRoots(), TEXT(" | ")),
		NumShadowed, NumTestsExcluded);

	return MountTransient(TEXT("<PackedOnDemand>"), Provenance, MoveTemp(Index), MoveTemp(PayloadBytes));
}
#endif	  // WITH_EDITOR

bool FVaCuusBundleMountTable::UnmountBundle(const FString& BundleName)
{
	using namespace VaCuusBundleMountPrivate;
	check(IsInGameThread());

	FScopeLock Lock(&GLock);
	if (!GLookup.IsValid())
	{
		return false;
	}

	TSharedRef<FVaCuusBundleLookup> NewLookup = MakeShared<FVaCuusBundleLookup>();
	NewLookup->Mounts = GLookup->Mounts;
	const int32 NumRemoved = NewLookup->Mounts.RemoveAll(
		[&BundleName](const TSharedRef<FVaCuusBundleMount>& Mount) { return Mount->BundleName == BundleName; });
	if (NumRemoved == 0)
	{
		return false;
	}
	GLookup = NewLookup;

	// The record's fate splits by backing (the class comment's release rule): a
	// steal-backed record is retained -- it is the only holder of a region that can
	// never be re-stolen -- while a transient record leaves, because re-packing is
	// how a fresh mount picks up loose edits. The ref is COPIED out of GRecords
	// before the Remove: TArray::Remove asserts on an element reference aliasing the
	// array being modified (Array.h:2196), and FindRecordLocked returns exactly that.
	bool bTransient = false;
	if (const TSharedRef<FVaCuusBundleMount>* RecordPtr = FindRecordLocked(BundleName))
	{
		const TSharedRef<FVaCuusBundleMount> Record = *RecordPtr;
		bTransient = !Record->OwnedRegion.IsValid();
		if (bTransient)
		{
			GRecords.Remove(Record);
		}
	}

	UE_LOG(LogVaCuus, Log, TEXT("Unmounted bundle '%s' (%s)"), *BundleName,
		bTransient ? TEXT("transient record dropped; re-mounting re-packs")
				   : TEXT("record retained; open reads keep their bytes and a re-mount reuses it"));
	return true;
}

int32 FVaCuusBundleMountTable::UnmountAll()
{
	using namespace VaCuusBundleMountPrivate;
	check(IsInGameThread());

	TArray<FString> Names;
	{
		FScopeLock Lock(&GLock);
		if (!GLookup.IsValid())
		{
			return 0;
		}
		for (const TSharedRef<FVaCuusBundleMount>& Mount : GLookup->Mounts)
		{
			Names.Add(Mount->BundleName);
		}
	}

	int32 NumUnmounted = 0;
	for (const FString& Name : Names)
	{
		NumUnmounted += UnmountBundle(Name) ? 1 : 0;
	}
	return NumUnmounted;
}

TSharedPtr<const FVaCuusBundleLookup> FVaCuusBundleMountTable::GetLookup()
{
	using namespace VaCuusBundleMountPrivate;
	FScopeLock Lock(&GLock);
	return GLookup;
}

bool FVaCuusBundleMountTable::ContainsPath(const FString& RelativePath, FString* OutBundleName)
{
	const TSharedPtr<const FVaCuusBundleLookup> Lookup = GetLookup();
	if (!Lookup.IsValid())
	{
		return false;
	}

	const FString NormalizedPath = VaCuusBundleFormat::NormalizePath(RelativePath);
	for (const TSharedRef<FVaCuusBundleMount>& Mount : Lookup->Mounts)
	{
		if (Mount->FindEntry(NormalizedPath) != nullptr)
		{
			if (OutBundleName)
			{
				*OutBundleName = Mount->BundleName;
			}
			return true;
		}
	}
	return false;
}

TArray<TSharedRef<FVaCuusBundleMount>> FVaCuusBundleMountTable::GetAllRecords()
{
	using namespace VaCuusBundleMountPrivate;
	FScopeLock Lock(&GLock);
	return GRecords;
}

void FVaCuusBundleMountTable::DestroyRecords()
{
	using namespace VaCuusBundleMountPrivate;
	check(IsInGameThread());

	FScopeLock Lock(&GLock);
	const int32 NumRecords = GRecords.Num();
	GLookup.Reset();
	GRecords.Empty();
	if (NumRecords > 0)
	{
		UE_LOG(LogVaCuus, Log, TEXT("Bundle mount table: %d record(s) destroyed at module shutdown"), NumRecords);
	}
}

namespace VaCuusBundleConfig
{
FString GetConfiguredBundleAssetPath()
{
	FString Path;
	if (GConfig != nullptr)
	{
		GConfig->GetString(TEXT("VaCuus"), TEXT("BundleAssetPath"), Path, GGameIni);
	}
	return Path;
}
}	 // namespace VaCuusBundleConfig

namespace VaCuusScriptServing
{
// Relaxed like the FVaCuusFileInterface members and Mount::ServedOpens: each counter
// is an independent monotonic tally, read for a log line -- no ordering ties them.
static std::atomic<uint64> GNumBundleScriptServes{0};
static std::atomic<uint64> GNumLooseScriptServes{0};

void NoteBundleScriptServe()
{
	GNumBundleScriptServes.fetch_add(1, std::memory_order_relaxed);
}

void NoteLooseScriptServe()
{
	GNumLooseScriptServes.fetch_add(1, std::memory_order_relaxed);
}

uint64 GetNumBundleScriptServes()
{
	return GNumBundleScriptServes.load(std::memory_order_relaxed);
}

uint64 GetNumLooseScriptServes()
{
	return GNumLooseScriptServes.load(std::memory_order_relaxed);
}
}	 // namespace VaCuusScriptServing

namespace VaCuusBundleCommands
{
/**
 * `vacuus.Bundle.Enable` -- the manual door over the mount predicate (spec M6 2(d)).
 * -1 (the default) means AUTO and never fires this callback: cooked builds mount the
 * config-listed bundle at subsystem Initialize, the editor and uncooked -game mount
 * nothing. An explicit value overrides:
 *   1: mount NOW -- in the editor/PIE, pack the loose tree on demand (the parity
 *      workflow; the editor asset has no payload to mount); in a cooked build,
 *      (re-)mount the config-listed bundle (the retained-record reuse path).
 *   0: unmount everything -- in packaged Development the loose staged files then
 *      serve, which is the A/B debugging story the staging gate keeps possible.
 * Setting 0 BEFORE subsystem init (-dpcvars) suppresses the cooked auto-mount.
 */
static void OnBundleEnableChanged(IConsoleVariable* Variable)
{
	const int32 Value = Variable->GetInt();

	if (Value == 0)
	{
		const int32 NumUnmounted = FVaCuusBundleMountTable::UnmountAll();
		UE_LOG(LogVaCuus, Log, TEXT("vacuus.Bundle.Enable 0: unmounted %d bundle(s); loose roots serve"), NumUnmounted);
		return;
	}

	if (Value < 1)
	{
		return;
	}

	if (FPlatformProperties::RequiresCookedData())
	{
		const FString Path = VaCuusBundleConfig::GetConfiguredBundleAssetPath();
		if (Path.IsEmpty())
		{
			UE_LOG(LogVaCuus, Warning,
				TEXT("vacuus.Bundle.Enable 1: no [VaCuus] BundleAssetPath is configured, so there is nothing to mount"));
			return;
		}
		UVaCuusBundle* Bundle = LoadObject<UVaCuusBundle>(nullptr, *Path);
		if (Bundle == nullptr)
		{
			UE_LOG(LogVaCuus, Error, TEXT("vacuus.Bundle.Enable 1: configured bundle '%s' resolves to no asset"), *Path);
			return;
		}
		FVaCuusBundleMountTable::MountBundle(Bundle);
		return;
	}

#if WITH_EDITOR
	FVaCuusBundleMountTable::MountPackedOnDemand();
#else
	UE_LOG(LogVaCuus, Warning,
		TEXT("vacuus.Bundle.Enable 1: this uncooked build has no editor data, so it cannot pack on demand"));
#endif
}

static TAutoConsoleVariable<int32> CVarBundleEnable(
	TEXT("vacuus.Bundle.Enable"),
	-1,
	TEXT("VaCuus bundle mounting. -1 (default): auto -- cooked builds mount the config-listed bundle ([VaCuus] ")
	TEXT("BundleAssetPath) at game-instance init; the editor and uncooked -game mount nothing. 1: mount now (editor/PIE ")
	TEXT("packs the loose DevUI tree on demand -- the cook parity check; cooked builds (re-)mount the configured ")
	TEXT("bundle). 0: unmount everything (packaged Development then serves from the loose staged files -- A/B)."),
	FConsoleVariableDelegate::CreateStatic(&OnBundleEnableChanged),
	ECVF_Default);

/**
 * `vacuus.DumpBundle` (spec section 3.1): every record -- mounted or retained -- with
 * its index, provenance and content hash. The hash is how pack determinism stays
 * observable in the field; the served counter is the per-bundle half of the
 * M==0 serving observability (the per-interface totals print at VFS teardown).
 * PRINTS A HEADER EVEN WHEN NOTHING IS MOUNTED -- the DumpModel rule: a diagnostic
 * that answers nothing is indistinguishable from one that did not run.
 */
static void DumpBundles()
{
	const TArray<TSharedRef<FVaCuusBundleMount>> Records = FVaCuusBundleMountTable::GetAllRecords();
	const TSharedPtr<const FVaCuusBundleLookup> Lookup = FVaCuusBundleMountTable::GetLookup();
	const int32 NumMounted = Lookup.IsValid() ? Lookup->Mounts.Num() : 0;

	UE_LOG(LogVaCuus, Display, TEXT("DumpBundle: %d record(s), %d mounted"), Records.Num(), NumMounted);

	for (const TSharedRef<FVaCuusBundleMount>& Record : Records)
	{
		const bool bMounted = Lookup.IsValid() && Lookup->Mounts.Contains(Record);
		UE_LOG(LogVaCuus, Display, TEXT("  '%s' [%s]: %d entries, %lld bytes, %s, hash %s, served %llu open(s)"),
			*Record->BundleName, bMounted ? TEXT("MOUNTED") : TEXT("retained, unmounted"), Record->Entries.Num(),
			Record->PayloadSize, Record->bMemoryMapped ? TEXT("memory-mapped") : TEXT("resident"),
			*Record->ContentHashHex, static_cast<uint64>(Record->ServedOpens.load(std::memory_order_relaxed)));
		UE_LOG(LogVaCuus, Display, TEXT("    provenance: %s"),
			Record->Provenance.IsEmpty() ? TEXT("(none recorded)") : *Record->Provenance);
		for (const FVaCuusBundleEntry& Entry : Record->Entries)
		{
			UE_LOG(LogVaCuus, Display, TEXT("    %10lld +%10lld  %s"), Entry.Offset, Entry.Size, *Entry.Path);
		}
	}
}

static FAutoConsoleCommand GDumpBundleCommand(
	TEXT("vacuus.DumpBundle"),
	TEXT("Print every VaCuus bundle record: mount state, index (path/offset/size), provenance, content hash and how ")
	TEXT("many opens it served. Records outlive unmounts for steal-backed bundles; see vacuus.Bundle.Enable."),
	FConsoleCommandDelegate::CreateStatic(&DumpBundles));
}	 // namespace VaCuusBundleCommands
