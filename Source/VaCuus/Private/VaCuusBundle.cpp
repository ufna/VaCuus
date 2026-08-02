// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusBundle.h"

#include "VaCuusContentPaths.h"
#include "VaCuusDefines.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProperties.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "String/BytesToHex.h"

#if WITH_EDITOR
#include "Cooker/CookDependency.h"
#include "Cooker/CookDependencyContext.h"
#include "Interfaces/ITargetPlatform.h"
#include "Serialization/CompactBinaryWriter.h"
#endif

namespace VaCuusBundleFormat
{
FString NormalizePath(const FString& InPath)
{
	FString Out = InPath;
	Out.ReplaceInline(TEXT("\\"), TEXT("/"));
	FPaths::RemoveDuplicateSlashes(Out);
	while (Out.StartsWith(TEXT("./")))
	{
		Out.RightChopInline(2);
	}
	Out.ToLowerInline();
	return Out;
}

bool IsExcludedTestPath(const FString& NormalizedPath)
{
	// Segment match, not substring: "latest/x.rml" must not be excluded, "tests/x.js"
	// and "img/tests/y.png" must be. The path is already normalized (lowercase, '/').
	return NormalizedPath.StartsWith(TEXT("tests/")) || NormalizedPath.Contains(TEXT("/tests/"));
}

TConstArrayView<const TCHAR*> GetPackedExtensions()
{
	// One list, three consumers: this pack, the VaCuus.Build.cs staging globs
	// (non-Shipping), and -- for the reloadable subset -- the live-reload watcher.
	// The Build.cs comment records why exactly these formats.
	static const TCHAR* Extensions[] = {
		TEXT("rml"), TEXT("rcss"), TEXT("js"), TEXT("mjs"),
		TEXT("png"), TEXT("jpg"), TEXT("jpeg"), TEXT("ttf"), TEXT("otf")};
	return Extensions;
}

bool SerializeCookedIndex(FArchive& Ar, FCookedIndex& Index, const TCHAR* BundleNameForErrors)
{
	// A cap the reader refuses past, so a corrupted count cannot drive a
	// multi-gigabyte allocation loop before the bounds validation ever runs. Two
	// decimal orders above the ~50-entry design point.
	constexpr int32 MaxEntries = 65536;

	if (Ar.IsSaving())
	{
		uint32 SerializedVersion = Version;
		Ar << SerializedVersion;

		// Byte count of the block that follows, backpatched: it is what lets a reader
		// that refuses the version SKIP the block it cannot parse, leaving the archive
		// aligned for the payload serialization behind it.
		const int64 BlockBytesPos = Ar.Tell();
		uint64 BlockBytes = 0;
		Ar << BlockBytes;
		const int64 BlockStart = Ar.Tell();

		Ar.Serialize(Index.ContentHash.GetBytes(), sizeof(decltype(Index.ContentHash.GetBytes())));
		Ar << Index.PayloadSize;

		int32 NumEntries = Index.Entries.Num();
		Ar << NumEntries;
		for (FVaCuusBundleEntry& Entry : Index.Entries)
		{
			Ar << Entry.Path;
			Ar << Entry.Offset;
			Ar << Entry.Size;
		}

		const int64 BlockEnd = Ar.Tell();
		BlockBytes = static_cast<uint64>(BlockEnd - BlockStart);
		Ar.Seek(BlockBytesPos);
		Ar << BlockBytes;
		Ar.Seek(BlockEnd);
		return true;
	}

	Index.Reset();

	uint32 SerializedVersion = 0;
	Ar << SerializedVersion;
	uint64 BlockBytes = 0;
	Ar << BlockBytes;
	const int64 BlockStart = Ar.Tell();
	const int64 BlockEnd = BlockStart + static_cast<int64>(BlockBytes);

	// Every refusal below seeks to BlockEnd before returning: the byte count was
	// written by the (trusted) saver BEFORE the fields a corrupted stream garbles, so
	// it is the one thing that still lets the payload behind the block deserialize.
	if (SerializedVersion != Version)
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("Bundle '%s': cooked index format version %u does not match this build's %u; the bundle is refused ")
			TEXT("(recook it with this plugin version)"),
			BundleNameForErrors, SerializedVersion, Version);
		Ar.Seek(BlockEnd);
		return false;
	}

	Ar.Serialize(Index.ContentHash.GetBytes(), sizeof(decltype(Index.ContentHash.GetBytes())));
	Ar << Index.PayloadSize;

	int32 NumEntries = 0;
	Ar << NumEntries;

	if (Ar.IsError() || Index.PayloadSize < 0 || NumEntries < 0 || NumEntries > MaxEntries)
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("Bundle '%s': cooked index is corrupt (payload size %lld, entry count %d); the bundle is refused"),
			BundleNameForErrors, Index.PayloadSize, NumEntries);
		Index.Reset();
		Ar.Seek(BlockEnd);
		return false;
	}

	Index.Entries.Reserve(NumEntries);
	for (int32 EntryIndex = 0; EntryIndex < NumEntries; ++EntryIndex)
	{
		FVaCuusBundleEntry& Entry = Index.Entries.AddDefaulted_GetRef();
		Ar << Entry.Path;
		Ar << Entry.Offset;
		Ar << Entry.Size;

		// Position-checked per entry, not once at the end: a corrupted count or a
		// garbled string length must not read past the block into the payload bytes.
		if (Ar.IsError() || Ar.Tell() > BlockEnd)
		{
			UE_LOG(LogVaCuus, Error,
				TEXT("Bundle '%s': cooked index entry %d of %d reads past the index block; the bundle is refused"),
				BundleNameForErrors, EntryIndex, NumEntries);
			Index.Reset();
			Ar.Seek(BlockEnd);
			return false;
		}
	}

	if (Ar.Tell() != BlockEnd)
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("Bundle '%s': cooked index block declares %llu bytes but its entries end %lld short; the bundle is refused"),
			BundleNameForErrors, BlockBytes, BlockEnd - Ar.Tell());
		Index.Reset();
		Ar.Seek(BlockEnd);
		return false;
	}

	return true;
}

bool ValidateEntries(const TArray<FVaCuusBundleEntry>& Entries, int64 PayloadSize, const TCHAR* BundleNameForErrors)
{
	TSet<FString> SeenPaths;
	SeenPaths.Reserve(Entries.Num());

	for (int32 EntryIndex = 0; EntryIndex < Entries.Num(); ++EntryIndex)
	{
		const FVaCuusBundleEntry& Entry = Entries[EntryIndex];

		// Overflow-safe span check: with Offset already proven <= PayloadSize, the
		// subtraction cannot underflow, so `Size > PayloadSize - Offset` is the
		// `Offset + Size > PayloadSize` comparison without the wrapping addition.
		if (Entry.Offset < 0 || Entry.Size < 0 || Entry.Offset > PayloadSize || Entry.Size > PayloadSize - Entry.Offset)
		{
			UE_LOG(LogVaCuus, Error,
				TEXT("Bundle '%s': entry %d ('%s') spans [%lld, %lld+%lld) outside the %lld-byte payload; the bundle is refused"),
				BundleNameForErrors, EntryIndex, *Entry.Path, Entry.Offset, Entry.Offset, Entry.Size, PayloadSize);
			return false;
		}

		if (Entry.Offset % EntryAlignment != 0)
		{
			// The pack always aligns (see VaCuusBundlePack::Pack), so a misaligned
			// offset is corruption, not a format variant to tolerate.
			UE_LOG(LogVaCuus, Error,
				TEXT("Bundle '%s': entry %d ('%s') offset %lld is not %lld-byte aligned; the bundle is refused"),
				BundleNameForErrors, EntryIndex, *Entry.Path, Entry.Offset, EntryAlignment);
			return false;
		}

		if (Entry.Path.IsEmpty() || !FPaths::IsRelative(Entry.Path))
		{
			UE_LOG(LogVaCuus, Error,
				TEXT("Bundle '%s': entry %d has an empty or non-relative path ('%s'); the bundle is refused"),
				BundleNameForErrors, EntryIndex, *Entry.Path);
			return false;
		}

		bool bAlreadySeen = false;
		SeenPaths.Add(Entry.Path, &bAlreadySeen);
		if (bAlreadySeen)
		{
			UE_LOG(LogVaCuus, Error,
				TEXT("Bundle '%s': entry %d duplicates the path '%s'; the bundle is refused"),
				BundleNameForErrors, EntryIndex, *Entry.Path);
			return false;
		}
	}

	return true;
}

FString HashToHex(const FBlake3Hash& Hash)
{
	TStringBuilder<80> Builder;
	UE::String::BytesToHexLower(Hash.GetBytes(), Builder);
	return FString(Builder);
}
}	 // namespace VaCuusBundleFormat

#if WITH_EDITOR
namespace VaCuusBundlePack
{
TArray<FSourceFile> EnumerateTree(const TArray<FString>& Roots, int32* OutNumShadowed, int32* OutNumTestsExcluded)
{
	int32 NumShadowed = 0;
	int32 NumTestsExcluded = 0;

	struct FClaim
	{
		int32 RootIndex = 0;
		int32 OutIndex = 0;
	};

	TArray<FSourceFile> Out;
	TMap<FString, FClaim> ClaimedBy;
	IFileManager& FileManager = IFileManager::Get();

	for (int32 RootIndex = 0; RootIndex < Roots.Num(); ++RootIndex)
	{
		const FString Root = FPaths::ConvertRelativePathToFull(Roots[RootIndex]);

		TArray<FString> Found;
		for (const TCHAR* Extension : VaCuusBundleFormat::GetPackedExtensions())
		{
			FileManager.FindFilesRecursive(Found, *Root, *(FString(TEXT("*.")) + Extension),
				/*Files*/ true, /*Directories*/ false, /*bClearFileNames*/ false);
		}

		for (const FString& DiskPath : Found)
		{
			const FString FullPath = FPaths::ConvertRelativePathToFull(DiskPath);
			if (!FullPath.StartsWith(Root + TEXT("/")))
			{
				continue;
			}

			const FString NormalizedPath = VaCuusBundleFormat::NormalizePath(FullPath.Mid(Root.Len() + 1));

			if (VaCuusBundleFormat::IsExcludedTestPath(NormalizedPath))
			{
				// Automation fixtures never ship (spec M6 2(a)); this exclusion is
				// where the Build.cs "Tests/*.js rides along" staging caveat retires.
				++NumTestsExcluded;
				continue;
			}

			if (const FClaim* Existing = ClaimedBy.Find(NormalizedPath))
			{
				// Duplicate-wins is DETERMINISTIC: an earlier root always wins (the
				// D19 plugin-first precedence, third venue), and inside one root --
				// files differing only in case, folded together by NormalizePath --
				// the lexicographically smaller disk path wins, because OS
				// enumeration order is not a thing a deterministic pack may consume.
				FSourceFile& Winner = Out[Existing->OutIndex];
				// FCString::Strcmp, NOT FString::operator< -- the latter compares
				// case-insensitively, and the only way two paths collide inside ONE
				// root is by differing in case, so an insensitive tiebreak would
				// answer "equal" and quietly hand the win back to OS enumeration
				// order, the exact nondeterminism this branch exists to remove.
				const bool bNewWins =
					Existing->RootIndex == RootIndex && FCString::Strcmp(*FullPath, *Winner.DiskPath) < 0;
				const FString& Kept = bNewWins ? FullPath : Winner.DiskPath;
				const FString& Dropped = bNewWins ? Winner.DiskPath : FullPath;

				UE_LOG(LogVaCuus, Log,
					TEXT("Bundle pack: '%s' is SHADOWED by '%s' (both normalize to '%s'); the shadowed copy is not packed"),
					*Dropped, *Kept, *NormalizedPath);

				if (bNewWins)
				{
					Winner.DiskPath = FullPath;
				}
				++NumShadowed;
				continue;
			}

			ClaimedBy.Add(NormalizedPath, FClaim{RootIndex, Out.Num()});
			Out.Add(FSourceFile{NormalizedPath, FullPath});
		}
	}

	if (OutNumShadowed)
	{
		*OutNumShadowed = NumShadowed;
	}
	if (OutNumTestsExcluded)
	{
		*OutNumTestsExcluded = NumTestsExcluded;
	}
	return Out;
}

bool Pack(TArray<FSourceFile> Files, VaCuusBundleFormat::FCookedIndex& OutIndex, TArray64<uint8>& OutPayload,
	FString* OutError)
{
	OutIndex.Reset();
	OutPayload.Empty();

	// THE determinism step: normalized paths are unique (EnumerateTree claims them)
	// and lowercase, so this byte-wise sort is total and input-order-independent --
	// the property the double-pack test feeds shuffled inputs to observe, and the
	// property incremental/multi-process cooks compare result hashes over.
	Files.Sort([](const FSourceFile& A, const FSourceFile& B) { return A.NormalizedPath < B.NormalizedPath; });

	for (const FSourceFile& File : Files)
	{
		TArray64<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *File.DiskPath))
		{
			if (OutError)
			{
				*OutError = FString::Printf(TEXT("failed to read '%s'"), *File.DiskPath);
			}
			OutIndex.Reset();
			OutPayload.Empty();
			return false;
		}

		const int64 AlignedOffset = Align(OutPayload.Num(), VaCuusBundleFormat::EntryAlignment);
		OutPayload.AddZeroed(AlignedOffset - OutPayload.Num());
		OutPayload.Append(Bytes);

		OutIndex.Entries.Add(FVaCuusBundleEntry{File.NormalizedPath, AlignedOffset, Bytes.Num()});
	}

	OutIndex.PayloadSize = OutPayload.Num();

	// The content hash covers the index AND the payload: paths delimited by their
	// entry's offset/size fields (so "ab"+"c" cannot collide with "a"+"bc"), then the
	// blob itself. Surfaced by vacuus.DumpBundle so the determinism observable
	// persists into the field (spec M6 2(c)).
	FBlake3 Hasher;
	for (const FVaCuusBundleEntry& Entry : OutIndex.Entries)
	{
		const FTCHARToUTF8 PathUtf8(*Entry.Path);
		Hasher.Update(PathUtf8.Get(), PathUtf8.Length());
		Hasher.Update(&Entry.Offset, sizeof(Entry.Offset));
		Hasher.Update(&Entry.Size, sizeof(Entry.Size));
	}
	if (OutPayload.Num() > 0)
	{
		Hasher.Update(OutPayload.GetData(), OutPayload.Num());
	}
	OutIndex.ContentHash = Hasher.Finalize();

	return true;
}
}	 // namespace VaCuusBundlePack

namespace VaCuusBundleCook
{
/**
 * The incremental-cook staleness answer (spec M6 2(c)): one Function dependency whose
 * hash covers the sorted path list AND every file's contents, so an EDIT, an ADD, a
 * DELETE and a RENAME all change the hash -- per-file File() dependencies alone would
 * miss added files. Re-executed by the cooker when validating whether the bundle
 * package may be incrementally skipped -- and reaching that validation at all takes
 * TWO opt-ins a stock 5.8 does not make (bundle-cook-experiments.md, errata 1-2):
 * the CODE default is incremental-on (`bool bDefaultIncremental = true;`,
 * CookOnTheFlyServer.cpp:10544-10548) but the SHIPPED config overrides it off
 * (`CookIncrementalDefaultIncremental=false`, BaseEditor.ini:393 -- a stock cook is a
 * full cook unless `-CookIncremental` is passed), and skipping is class-gated to an
 * allowlist that stock config fills with engine script packages only
 * (BaseEditor.ini:475); a project plugin's class repacks on EVERY cook -- safe, never
 * skipped -- until the project opts it in, as the host does
 * (VcHost/Config/DefaultEditor.ini:14, `+IncrementalClassAllowList=
 * /Script/VaCuus.VaCuusBundle`). Once both doors are open: a changed hash
 * invalidates the package, PreSave runs again, the tree repacks -- and an unchanged
 * hash is what lets the cooker skip it, which errata row Z2b shows the legacy
 * `-legacyiterative` path can NEVER honestly decide (no ZenStore storage for
 * dependency data: a deleted file ships stale, silently).
 */
static void HashBundleTree(FCbFieldViewIterator Args, UE::Cook::FCookDependencyContext& Context)
{
	// The args first (the serialized-format version): bumping
	// VaCuusBundleFormat::Version must recook even an unchanged tree, because the
	// cooked bytes it produces are different.
	for (FCbFieldViewIterator It = Args; It; ++It)
	{
		const uint32 Value = It->AsUInt32();
		Context.Update(&Value, sizeof(Value));
	}

	TArray<VaCuusBundlePack::FSourceFile> Files =
		VaCuusBundlePack::EnumerateTree(VaCuusContentPaths::GetDocumentRoots());
	Files.Sort([](const VaCuusBundlePack::FSourceFile& A, const VaCuusBundlePack::FSourceFile& B) {
		return A.NormalizedPath < B.NormalizedPath;
	});

	for (const VaCuusBundlePack::FSourceFile& File : Files)
	{
		const FTCHARToUTF8 PathUtf8(*File.NormalizedPath);
		Context.Update(PathUtf8.Get(), PathUtf8.Length());

		TArray64<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *File.DiskPath))
		{
			// LogError marks the hash unstorable, so the package recooks next time
			// rather than being skipped over an unreadable input
			// (CookDependencyContext.h:44-49).
			Context.LogError(FString::Printf(TEXT("VaCuusBundleTree: failed to read '%s'"), *File.DiskPath));
			return;
		}

		// The size delimits path from contents (and file from file), for the same
		// no-collision reason the pack's hash carries offset/size.
		const int64 Size = Bytes.Num();
		Context.Update(&Size, sizeof(Size));
		if (Size > 0)
		{
			Context.Update(Bytes.GetData(), Size);
		}
	}
}
}	 // namespace VaCuusBundleCook

// At file scope, not inside the namespace: UE_COOK_DEPENDENCY_FUNCTION_CALL below
// expands to the registration object's bare identifier, so the object must be
// visible from UVaCuusBundle::OnCookEvent's (global) scope.
UE_COOK_DEPENDENCY_FUNCTION(VaCuusBundleTree, VaCuusBundleCook::HashBundleTree);
#endif	  // WITH_EDITOR

void UVaCuusBundle::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

	// The UStaticMesh::Serialize shape (StaticMesh.cpp:7383-7384): the saver writes
	// whether cook data follows, the loader reads it, and every non-persistent archive
	// (reference collectors, memory counters) sees IsCooking() == false and skips.
	bool bCooked = Ar.IsCooking();
	Ar << bCooked;

	if (!bCooked)
	{
		// An editor save (or the load of one) carries the SourceNote tagged property
		// from Super::Serialize and NOTHING else -- the loose tree is authoritative,
		// and a packed index leaking into editor saves is spec 9's finding 2.
		return;
	}

	const FString BundleName = GetPathName();

	if (Ar.IsSaving())
	{
#if WITH_EDITOR
		// Only the cooker takes this branch (a cooked game never saves packages, and
		// bCooked gates every editor save out above). The transients were filled by
		// PreSave -- once -- and are written identically by each of SavePackage's
		// serialization passes.
		VaCuusBundleFormat::SerializeCookedIndex(Ar, CookedIndex, *BundleName);

		// The USoundWave flag recipe minus its audio-feature check
		// (SoundWave.cpp:1456-1460; the flag branch itself is FFormatContainer's,
		// BulkData.cpp:1730-1744): mapped payloads must NOT be inline -- inline wins
		// over the mapping flag (BulkData.h:133-138) -- and the feature resolves
		// per cooking target, so a multi-platform cook diverges correctly (Win64
		// maps, Linux/macOS inherit Generic's false and cook inline).
		const bool bMapped = Ar.CookingTarget()->SupportsFeature(ETargetPlatformFeatures::MemoryMappedFiles);
		Payload.SerializeWithFlags(Ar, this,
			bMapped ? (BULKDATA_Force_NOT_InlinePayload | BULKDATA_MemoryMappedPayload) : BULKDATA_ForceInlinePayload);
#endif
	}
	else if (Ar.IsLoading())
	{
		// A refused index (version mismatch, corruption) still deserializes the
		// payload -- bulk data is self-describing and engine-versioned, and the index
		// reader parked the archive right in front of it -- but the refusal is
		// remembered and the mount table will not touch this object.
		bCookedLoadRefused = !VaCuusBundleFormat::SerializeCookedIndex(Ar, CookedIndex, *BundleName);

		// Attempt mapping only where the runtime can map (the SoundWave.cpp:1465-1468
		// load-side check): Win64 true, Linux/macOS false -- there the payload was
		// cooked inline and loads resident, and the mount logs which branch it got.
		// Mapping is only legal while loading (check at BulkData.cpp:1223), which is
		// why the mount later STEALS what this load produced instead of re-requesting.
		Payload.Serialize(Ar, this, INDEX_NONE, /*bAttemptFileMapping*/ FPlatformProperties::SupportsMemoryMappedFiles());
	}
}

#if WITH_EDITOR

void UVaCuusBundle::PreSave(FObjectPreSaveContext SaveContext)
{
	Super::PreSave(SaveContext);

	// The pack lives HERE and not in Serialize because Serialize runs three times per
	// object inside SavePackage (the Object.h:272-274 warning); and it is synchronous
	// because packing <=10 MB of loose files is plain file IO -- the async
	// BeginCacheForCookedPlatformData machinery is for DDC-built data like textures.
	if (SaveContext.IsCooking())
	{
		PackForCook();
	}
}

void UVaCuusBundle::PackForCook()
{
	int32 NumShadowed = 0;
	int32 NumTestsExcluded = 0;
	TArray<VaCuusBundlePack::FSourceFile> Files =
		VaCuusBundlePack::EnumerateTree(VaCuusContentPaths::GetDocumentRoots(), &NumShadowed, &NumTestsExcluded);

	TArray64<uint8> PayloadBytes;
	FString Error;
	if (!VaCuusBundlePack::Pack(MoveTemp(Files), CookedIndex, PayloadBytes, &Error))
	{
		UE_LOG(LogVaCuus, Error, TEXT("Bundle '%s': cook-time pack FAILED (%s); the cooked bundle will be empty"),
			*GetPathName(), *Error);
		CookedIndex.Reset();
		Payload.RemoveBulkData();
		return;
	}

	if (PayloadBytes.Num() > 0)
	{
		Payload.Lock(LOCK_READ_WRITE);
		void* Dest = Payload.Realloc(PayloadBytes.Num());
		FMemory::Memcpy(Dest, PayloadBytes.GetData(), PayloadBytes.Num());
		Payload.Unlock();
	}
	else
	{
		Payload.RemoveBulkData();
	}

	UE_LOG(LogVaCuus, Display,
		TEXT("Bundle '%s': packed %d file(s), %lld bytes, hash %s (%d shadowed duplicate(s), %d test fixture(s) excluded)"),
		*GetPathName(), CookedIndex.Entries.Num(), CookedIndex.PayloadSize,
		*VaCuusBundleFormat::HashToHex(CookedIndex.ContentHash), NumShadowed, NumTestsExcluded);
}

void UVaCuusBundle::PostSaveRoot(FObjectPostSaveRootContext ObjectSaveContext)
{
	// The cleanup half of the PreSave pack -- see the class comment for why this is
	// PostSaveRoot (5.8 has no per-object PostSave; this runs on the asset after the
	// save, SavePackage2.cpp:4115-4119, even when the save failed, :4102). Without it
	// a cook-packed live object would sit on the whole payload for the rest of the
	// cooker's session per platform, and the next platform's PreSave would repack over
	// stale transients instead of from empty.
	if (ObjectSaveContext.IsCooking())
	{
		CookedIndex.Reset();
		Payload.RemoveBulkData();
	}

	Super::PostSaveRoot(ObjectSaveContext);
}

void UVaCuusBundle::OnCookEvent(UE::Cook::ECookEvent CookEvent, UE::Cook::FCookEventContext& Context)
{
	Super::OnCookEvent(CookEvent, Context);

	// "Called for each object immediately after PreSave" (Cooker/CookEvents.h:27-32).
	// The dependency registered here is what makes an incremental cook recook this
	// package when the loose tree changes while the .uasset itself is untouched
	// (AddSaveBuildDependency contract: Cooker/CookEvents.h:95-100).
	if (CookEvent == UE::Cook::ECookEvent::PlatformCookDependencies)
	{
		FCbWriter Args;
		Args << "FormatVersion" << VaCuusBundleFormat::Version;
		Context.AddSaveBuildDependency(
			UE::Cook::FCookDependency::Function(UE_COOK_DEPENDENCY_FUNCTION_CALL(VaCuusBundleTree), Args.Save()));
	}
}

#endif	  // WITH_EDITOR
