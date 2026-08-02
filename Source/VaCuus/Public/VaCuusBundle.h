// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Hash/Blake3.h"
#include "Serialization/BulkData.h"
#include "UObject/Object.h"
#include "UObject/ObjectSaveContext.h"

#if WITH_EDITOR
#include "Cooker/CookEvents.h"
#endif

#include "VaCuusBundle.generated.h"

/**
 * One file inside a bundle payload. Path is NORMALIZED (relative, '/'-separated,
 * lowercase -- VaCuusBundleFormat::NormalizePath is the one definition); Offset/Size
 * address the payload blob and are validated against its bounds at mount, never trusted.
 */
struct FVaCuusBundleEntry
{
	FString Path;
	int64 Offset = 0;
	int64 Size = 0;
};

/**
 * The cooked on-disk format, shared by the asset's Serialize, the pack, and the mount
 * table's validation -- and exposed (rather than buried in UVaCuusBundle) so the
 * corrupted-fixture tests can drive the exact production reader over hand-crafted bytes.
 */
namespace VaCuusBundleFormat
{
/**
 * Bumped on ANY layout change of the cooked index block below. Checked at load; a
 * mismatch refuses the whole bundle with an Error naming it and both versions (spec
 * M6 2(a)). It also feeds the cook-dependency args, so bumping it recooks every bundle.
 */
inline constexpr uint32 Version = 1;

/**
 * Every entry's payload starts on this boundary (gaps zero-filled). Our own format
 * choice, not an RmlUi requirement: it is nearly free at ~50 entries and keeps any
 * future SIMD/hash read over an entry span alignment-clean.
 */
inline constexpr int64 EntryAlignment = 64;

/** The index + hash + declared payload size -- everything the cooked block carries besides the bulk data. */
struct FCookedIndex
{
	TArray<FVaCuusBundleEntry> Entries;
	FBlake3Hash ContentHash;
	int64 PayloadSize = 0;

	void Reset()
	{
		Entries.Empty();
		ContentHash = FBlake3Hash();
		PayloadSize = 0;
	}
};

/**
 * The one path spelling the bundle lookup uses: '/'-separated, lowercase, no leading
 * "./", no duplicate slashes. Both the pack and FVaCuusFileInterface::Open normalize
 * through here, which is what makes "the same relative path" mean one thing.
 *
 * Lowercasing makes bundle lookups case-insensitive -- a superset of the loose
 * behavior on Linux (ext4 is case-sensitive) and a match for it on Windows/macOS.
 * Two source files differing only in case therefore COLLIDE in a bundle; the pack
 * logs the loser as shadowed instead of silently keeping both.
 */
VACUUS_API FString NormalizePath(const FString& InPath);

/**
 * True for automation fixtures: any path segment equal to "tests" (the DevUI
 * convention -- Content/DevUI/Tests/). Excluded from every pack; this is where the
 * VaCuus.Build.cs "Tests fixtures ride along" staging caveat actually retires.
 */
VACUUS_API bool IsExcludedTestPath(const FString& NormalizedPath);

/**
 * The loose formats a bundle packs -- MUST track the RuntimeDependencies staging
 * globs in Source/VaCuus/VaCuus.Build.cs (and the live-reload watcher's whitelist),
 * for the reason recorded there: these are the formats the VFS actually reads.
 */
VACUUS_API TConstArrayView<const TCHAR*> GetPackedExtensions();

/**
 * The cook-only index block: Version, then a byte count for the remainder, then
 * hash + payload size + entries. Returns false on refusal -- version mismatch, or
 * implausible counts in a corrupted stream -- after logging one Error naming
 * BundleNameForErrors and, for a version mismatch, both versions. On refusal the
 * archive is left positioned AFTER the block (the byte count is what makes the skip
 * possible), so the payload serialization that follows stays aligned and a refused
 * bundle still loads as an object -- it just refuses to mount.
 */
VACUUS_API bool SerializeCookedIndex(FArchive& Ar, FCookedIndex& Index, const TCHAR* BundleNameForErrors);

/**
 * Every entry checked against the payload bounds: non-negative offset/size,
 * Offset + Size <= PayloadSize (overflow-safe), alignment as packed. One Error names
 * the bundle, the entry and the numbers on the first violation; the caller refuses
 * the mount (spec M6 2(a): "refusal on violation").
 */
VACUUS_API bool ValidateEntries(const TArray<FVaCuusBundleEntry>& Entries, int64 PayloadSize, const TCHAR* BundleNameForErrors);

/** Lowercase hex of the content hash -- what vacuus.DumpBundle prints and the determinism test compares. */
VACUUS_API FString HashToHex(const FBlake3Hash& Hash);
}	 // namespace VaCuusBundleFormat

#if WITH_EDITOR
/**
 * The pack: loose DevUI tree -> index + payload. WITH_EDITOR in the RUNTIME module,
 * deliberately (research note bundle-cook.md section 1): the cooker runs an editor
 * target, so the code is present where PreSave needs it, while the class the cooked
 * game deserializes still lives in a Runtime module. VaCuusEditor contributes only
 * the factory. The same code backs the PIE pack-on-demand mount and the tests.
 */
namespace VaCuusBundlePack
{
/** A file the pack will read: where it is on disk, and the normalized path it serves under. */
struct FSourceFile
{
	FString NormalizedPath;
	FString DiskPath;
};

/**
 * Walks Roots IN ORDER collecting files matching GetPackedExtensions(), excluding
 * Tests/ fixtures. Duplicate normalized paths are first-root-wins -- plugin-first,
 * the same D19 precedence the loose VFS resolves with -- and EVERY shadowed loser is
 * logged with both disk paths (the stale-duplicate visibility rule, third venue).
 * The returned order is the enumeration order; Pack() sorts, so callers need not.
 */
VACUUS_API TArray<FSourceFile> EnumerateTree(const TArray<FString>& Roots, int32* OutNumShadowed = nullptr,
	int32* OutNumTestsExcluded = nullptr);

/**
 * Deterministic pack: sorts Files by normalized path (they are already lowercase, so
 * the byte order IS the case-insensitive order), 64-byte-aligns each entry, hashes
 * every entry's path + offset + size + the payload bytes into Index.ContentHash.
 * Determinism is the contract multi-process/incremental cooks compare hashes over --
 * the double-pack test feeds this differently-ordered inputs and asserts
 * byte-identical output. Takes Files by value because it sorts them.
 * Returns false (with OutError set) if any file fails to read.
 */
VACUUS_API bool Pack(TArray<FSourceFile> Files, VaCuusBundleFormat::FCookedIndex& OutIndex, TArray64<uint8>& OutPayload,
	FString* OutError = nullptr);
}	 // namespace VaCuusBundlePack
#endif	  // WITH_EDITOR

/**
 * The shipped UI tree as ONE asset: every loose DevUI file the VFS serves, packed at
 * cook time into a single memory-mappable payload plus an index (M6 spec 2(a-d)).
 *
 * WHAT AN EDITOR SAVE CARRIES: SourceNote, and nothing else. The index and payload are
 * NOT UPROPERTYs and serialize ONLY into cooks -- a packed live object would otherwise
 * leak its index into ordinary editor saves (nondeterministic, undiffable,
 * source-control churn; spec 9's finding 2). The loose tree stays authoritative in the
 * editor; PreSave(IsCooking) packs it, Serialize writes it under Ar.IsCooking(), and
 * PostSaveRoot clears the transients again.
 *
 * WHY PostSaveRoot AND NOT "PostSave": 5.8's UObject has no per-object post-save
 * virtual (Object.h declares PreSave at :278 and PreSaveRoot/PostSaveRoot at :262/:270;
 * there is no PostSave member). PostSaveRoot runs on the package's asset -- this object,
 * a one-asset package -- after the save, including failed ones
 * (SavePackage2.cpp:4102, :4115-4119), which is exactly the cleanup slot the spec's
 * "PostSave clears the transients" wants.
 *
 * MOUNTING is the mount table's job (VaCuusBundleMount.h), not this class's: the mount
 * STEALS the payload region (FBulkData::StealFileMapping, BulkData.h:863-867), which is
 * destructive and once-only -- after it, this object is an empty shell the GC may take
 * while the region lives on in the mount record.
 */
UCLASS(MinimalAPI)
class UVaCuusBundle : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Provenance, for humans: who created the asset and from what. The ONLY thing an
	 * editor save serializes beyond the empty object; carried into cooks as an ordinary
	 * tagged property, where vacuus.DumpBundle prints it. Written once at creation
	 * (factory / vacuus.Bundle.CreateAsset) and never touched by the cook -- a per-cook
	 * rewrite (say, a timestamp) would make cooked output nondeterministic.
	 */
	UPROPERTY(VisibleAnywhere, Category = "VaCuus")
	FString SourceNote;

	//~ Begin UObject
	VACUUS_API virtual void Serialize(FArchive& Ar) override;
#if WITH_EDITOR
	VACUUS_API virtual void PreSave(FObjectPreSaveContext SaveContext) override;
	VACUUS_API virtual void PostSaveRoot(FObjectPostSaveRootContext ObjectSaveContext) override;
	VACUUS_API virtual void OnCookEvent(UE::Cook::ECookEvent CookEvent, UE::Cook::FCookEventContext& Context) override;
#endif
	//~ End UObject

	/** The cooked index: filled by PreSave when cooking, or by a cooked load. Consumed by the mount table. */
	const VaCuusBundleFormat::FCookedIndex& GetCookedIndex() const { return CookedIndex; }

	/** The cooked payload. Non-const because mounting steals from it (see the class comment). */
	FByteBulkData& GetPayload() { return Payload; }

	/**
	 * True when a cooked load refused the index block (format-version mismatch or
	 * corruption -- the Error was logged there). The mount table checks this and
	 * refuses to mount, so a refused bundle can never serve garbage spans.
	 */
	bool WasCookedLoadRefused() const { return bCookedLoadRefused; }

private:
	/**
	 * Cook-only state -- deliberately NOT UPROPERTYs (see the class comment). During a
	 * cook save these are transient: PreSave fills, Serialize writes (three times, all
	 * from this one pack -- Object.h:272-274 warns Serialize runs thrice per object in
	 * SavePackage, which is exactly why the pack is in PreSave), PostSaveRoot clears.
	 * During a cooked LOAD they are the loaded data the mount table consumes.
	 */
	VaCuusBundleFormat::FCookedIndex CookedIndex;
	FByteBulkData Payload;
	bool bCookedLoadRefused = false;

#if WITH_EDITOR
	/** PreSave's pack (cooking only): enumerate + pack the loose tree into the members above. */
	void PackForCook();
#endif
};
