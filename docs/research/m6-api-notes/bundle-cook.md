All sources are read. Delivering the design.

# UVaCuusBundle — asset + cook + memory-mapped read design (M6 Task A)

## 0. Decisions at a glance

1. **Asset shape:** a plain `UObject` asset (`UVaCuusBundle`) in the `VaCuus` runtime module carrying **one `FByteBulkData`** (single blob = one mapped region) plus a UPROPERTY path index (`TArray<FVaCuusBundleEntry>{Path, Offset, Size}`), serialized manually in `Serialize()`. Not `FEditorBulkData` (that is the virtualization/DDC author-time shape; our author-time payload is *empty* — the source of truth is the loose tree).
2. **Cook hook:** pack the tree in `PreSave()` when `SaveContext.IsCooking()`; serialize with `BULKDATA_Force_NOT_InlinePayload | BULKDATA_MemoryMappedPayload` when the target platform supports mapping, inline otherwise (the exact `USoundWave` pattern). Staleness is handled with `OnCookEvent(PlatformCookDependencies)` + one `FCookDependency::Function` that hashes the enumerated tree — **verified to exist in 5.8**, and needed, because incremental cook is **default-on** in 5.8.
3. **Runtime:** load the asset → the loader memory-maps the payload automatically where supported, else falls back to a resident buffer — **on 5.8, "supported" means Win64 yes, Linux/macOS NO** (headline finding below). The VFS is span-based either way; `StealFileMapping()` decouples the region's lifetime from the UObject.
4. **Precedence:** bundle-first when mounted; loose roots as fallback; nothing mounted in the editor by default.
5. **Staging:** keep the `RuntimeDependencies` globs for `Target.Configuration != Shipping`; Shipping ships bundle-only.

---

## 1. Q1 — Asset shape

### The bulk-data contract (all from `Engine/Source/Runtime/CoreUObject/Public/Serialization/BulkData.h`)

- `FByteBulkData = TBulkData<uint8>` (BulkData.h:1123); `TBulkData::Serialize(Ar, Owner, Idx, bAttemptFileMapping, FileRegionType)` (BulkData.h:1093-1096) and `SerializeWithFlags(..., SaveOverrideFlags, ...)` which ORs flags for the save and restores them (BulkData.h:1107-1120).
- Cook-relevant flags: `BULKDATA_Force_NOT_InlinePayload = 1<<10` (BulkData.h:127), `BULKDATA_MemoryMappedPayload = 1<<12` — "During cooking this flag indicates that the payload should work with memory mapping at runtime **if the target cooking platform supports it** so the payload should be stored in a `.m.ubulk` file" (BulkData.h:133-138). Runtime-assigned: `BULKDATA_DataIsMemoryMapped = 1<<30` (BulkData.h:166), `BULKDATA_UsesIoDispatcher = 1<<31` (BulkData.h:164).
- Chunk-type routing: `GetIoChunkTypeFromFlags` returns `EIoChunkType::MemoryMappedBulkData` for the flag (BulkData.cpp:107-109).
- The mapped allocation lives as `FOwnedBulkDataPtr{IMappedFileHandle*, IMappedFileRegion*}` (BulkData.h:219-271) inside a union with the malloc pointer (BulkData.h:501-509); `StealFileMapping()` (BulkData.h:863-867) hands you the `FOwnedBulkDataPtr` and works for both backings — mapped: transfers the handle pair; resident: wraps the raw allocation (BulkData.cpp:446-462).

### Precedents

- **`FFormatContainer`** — the engine's literal "named blobs in one asset" shape: `TSortedMap<FName, FByteBulkData*>` (BulkData.h:1128-1180). Its cook path is the flag recipe we copy: `bMapped` → `SetBulkDataFlags(BULKDATA_Force_NOT_InlinePayload)` + `BULKDATA_MemoryMappedPayload`, else `BULKDATA_ForceInlinePayload` (BulkData.cpp:1730-1744); its load path passes `bAttemptFileMapping=true` (`SerializeAttemptMappedLoad`, BulkData.cpp:1758-1770). **We deliberately do NOT copy the one-bulkdata-per-entry layout**: each `FByteBulkData` is its own IoStore chunk with its own 16 KiB alignment (see §3), so ~50 small UI files would waste TOC entries and padding — one blob + our own index gives one region and page-sharing.
- **`USoundWave`** — the per-platform cook decision: `bool bMapped = CookingTarget->SupportsFeature(ETargetPlatformFeatures::MemoryMappedFiles) && ...` then `CompressedFormatData.Serialize(Ar, this, &Formats, true, DEFAULT_ALIGNMENT, !bMapped /*inline if not mapped*/, bMapped)` (SoundWave.cpp:1456-1460); load side attempts mapping only when `FPlatformProperties::SupportsMemoryMappedFiles()` (SoundWave.cpp:1465-1468).
- **`UNNEModelData`** — the "asset that wraps a source file's bytes and rebuilds a cooked payload in `Serialize`" precedent: raw imported file bytes in `TArray64<uint8> FileData` (NNEModelData.h:247), cook transform per-runtime behind a `Serialize` override (NNEModelData.h:108) with `CreateModelData(RuntimeName, TargetPlatform)` (NNEModelData.h:272). It proves the shape works but uses plain TArrays — no mapping; we keep bulk data.
- **`UTexture`** is the async-DDC precedent (`BeginCacheForCookedPlatformData` override, Texture.h:1894) — see §2 for why we don't need it.

### The asset

```
UCLASS() class VACUUS_API UVaCuusBundle : public UObject {
    UPROPERTY() TArray<FVaCuusBundleEntry> Index;   // Path (normalized, '/'-separated, lowercase), Offset, Size
    UPROPERTY() FString SourceNote;                  // provenance: roots + timestamp, for humans
    FByteBulkData Payload;                           // not a UPROPERTY — serialized in Serialize()
    virtual void Serialize(FArchive& Ar) override;   // Super::Serialize (UPROPERTYs) then Payload.Serialize*
#if WITH_EDITOR
    virtual void PreSave(FObjectPreSaveContext) override;                    // packs when cooking
    virtual void OnCookEvent(UE::Cook::ECookEvent, FCookEventContext&) override; // declares deps
#endif
};
```
Entries inside the blob are 64-byte aligned at pack time ([inference] — our own format choice; nothing in RmlUi requires it, it's free and keeps any future SIMD/hash reads happy). `CanContainContent` is already true (VaCuus.uplugin: `"CanContainContent": true`), so the asset can live in plugin content; §15's "confirm at M6" is hereby confirmed as *required*, not just kept.

**Spec-wording correction (arch §9):** "built … at cook time **by `VaCuusEditor`**" cannot be literally true — the class the cooked game deserializes must live in a Runtime module, and the packing must run inside that class's own `PreSave`/`Serialize`. So: class + `WITH_EDITOR` packing code in `VaCuus` (the cooker runs an editor target, so the code is present); `VaCuusEditor` contributes the asset **factory**/UI only.

## 2. Q2 — Cook-time generation

### Where the payload is built

`UObject::PreSave(FObjectPreSaveContext)` (Object.h:278, with the warning at Object.h:272-274 that **Serialize runs three times per object in SavePackage** — which is exactly why the pack happens once in PreSave, not in Serialize). `FObjectPreSaveContext::IsCooking()` / `GetTargetPlatform()` (ObjectSaveContext.h:266-269). When cooking: enumerate the DevUI roots in the same order the runtime resolves them — plugin-first then project (VaCuusContentPaths.cpp:22-39, D19) — with plugin-first duplicate-wins and every shadowed file logged (the D19 stale-duplicate visibility rule, same rationale as VaCuusFileInterface.cpp:73-79); sort entries by normalized path for **deterministic** output (multi-process/incremental cooks compare hashes of results); exclude `Tests/` (automation fixtures have no business in a shipping bundle — this is where the Build.cs:115-116 caveat actually retires). Fill `Index` + a scratch buffer; in `Serialize`, when `Ar.IsCooking()`:

```cpp
const ITargetPlatform* TP = Ar.CookingTarget();
const bool bMapped = TP->SupportsFeature(ETargetPlatformFeatures::MemoryMappedFiles); // SoundWave.cpp:1456
Payload.SerializeWithFlags(Ar, this,
    bMapped ? (BULKDATA_Force_NOT_InlinePayload | BULKDATA_MemoryMappedPayload)
            : BULKDATA_ForceInlinePayload);        // flag recipe: BulkData.cpp:1730-1744
```
`SupportsFeature(MemoryMappedFiles)` resolves to `TPlatformProperties::SupportsMemoryMappedFiles()` (TargetPlatformBase.h:493-494) — per-target, so a multi-platform cook correctly diverges per platform. Editor (non-cooking) saves write the empty bulk data — the `.uasset` stays bytes-sized and loose files stay authoritative.

`BeginCacheForCookedPlatformData` (Object.h:1186-1192, "called when cooking before serialization … prepare platform specific data", polled via `IsCachedCookedPlatformDataLoaded` Object.h:1199) is the right hook only when the build is *slow* and belongs on a task + DDC (textures, Texture.h:1894). Packing ≤10 MB of files is file IO we'd re-read anyway; synchronous PreSave is the honest choice. Revisit only if reference-HUD cooks ever show the pack in profiles.

### Staleness — the part that actually bites

**Incremental cook is the default in 5.8**: `bool bDefaultIncremental = true;` overridable by `[CookSettings] CookIncrementalDefaultIncremental` (CookOnTheFlyServer.cpp:10544-10548), with `-fullcook`/`-forcerecook` as the opt-outs (:10574-10578). So a changed `.rml` with an untouched `.uasset` **will be skipped** unless the dependency is declared. The 5.8 mechanism:

- `UE::Cook::FCookDependency::File(FStringView)` — **verified**: declared CookDependency.h:119 ("Contents are loaded via IFileManager … and contents are hashed", :115-118), constructed CookDependency.cpp:83-88, hashed by streaming the file through Blake3 at validation time (CookDependency.cpp:421-460). Note: **zero engine callers** outside its own implementation — API is real but battle-tested only by Epic's tests, hence the experiment below.
- `FCookDependency::Function(FName, FCbFieldIterator&&)` (CookDependency.h:131) with `UE_COOK_DEPENDENCY_FUNCTION(Name, Fn)` registration (CookDependency.h:375-377; fn type :333) — re-executed at validation to recompute the hash.
- Declared from `UObject::OnCookEvent` (Object.h:290) for `ECookEvent::PlatformCookDependencies` — "called for each object immediately after PreSave" (CookEvents.h:27-32) — via `FCookEventContext::AddSaveBuildDependency` ("Incremental cooks will invalidate the package and recook it if the CookDependency changes", CookEvents.h:95-100). Engine precedent for the OnCookEvent+dependency pattern: WorldPartitionRuntimeLevelStreamingCell.cpp:564-569 (`AddSaveBuildDependency(FCookDependency::Package(...))`).

**Design:** one `FCookDependency::Function("VaCuusBundleTree", Args=[roots, extension list, excludes])` whose implementation hashes, in sorted order, every matched file's *relative path and contents*. A single Function dep covers **edits, adds, deletes and renames**; per-file `File` deps alone would miss added files. If the package is invalidated it is fully re-saved (PreSave runs again → repack); if not, nothing runs and the prior oplog result ships — which is correct.

Caveat to document: without ZenStore the cooker drops to legacy build dependencies (`bLegacyBuildDependenciesDueToNoZenStore`, CookOnTheFlyServer.cpp:10590-10598) — whether the Function dep is honored there is not provable from reading; covered by Exp-COOK-FILEDEP below.

Cook inclusion ([inference — standard UE, not re-verified here]): a config-soft-path-only bundle is invisible to the cooker; the reference HUD project must hard-reference it or list it in `DirectoriesToAlwaysCook`/primary-asset rules. Put this in the M6 reference-project checklist.

## 3. Q3 — Runtime reading

### What the engine gives us

- **IoStore game (the default packaged shape):** the Zen loader sees `BULKDATA_MemoryMappedPayload`, builds the `MemoryMappedBulkData` chunk id and — because we pass `bAttemptFileMapping=true` — calls `IoDispatcher.OpenMapped(ChunkId, FIoReadOptions(Offset, Size))`; on failure it logs a warning and `ForceBulkDataResident()` (AsyncLoading2.cpp:3283-3306). **A pak/IoStore-backed memory-map returns `FIoMappedRegion{IMappedFileHandle*, IMappedFileRegion*}`** (IoDispatcher.h:253-258; `OpenMapped` at :315), i.e. a live view into the mounted **`.ucas` container itself**: `FFileIoStore::OpenMapped` resolves the chunk's offset and `MapRegion`s the container file at that offset (IoDispatcherFileBackend.cpp:2122-2166). The pointer/size API is `IMappedFileRegion::GetMappedPtr()/GetMappedSize()` plus a `PreloadHint` prefault hint (MappedFileHandle.h:53-77).
- **Pak-without-IoStore:** LinkerLoad maps the `.m.ubulk` package segment via `IPackageResourceManager::OpenMappedHandleToPackage` with the same resident fallback (LinkerLoad.cpp:8003-8021). `FPakPlatformFile::OpenMappedEx` refuses **compressed or encrypted** pak entries (IPlatformFilePak.cpp:4694-4697), maps the whole pak once per pak and returns an offset proxy (:4698-4710), and is globally gated by `pak.EnableMMIO` (`GMMIO_Enable`, :4026).
- **Writer-side guarantees (IoStore):** memory-mapped chunks are **never compressed** (`ContainerSettings.IsCompressed() && !Options.bIsMemoryMapped` gate, IoStoreWriter.cpp:3293) and are aligned to `MemoryMappingAlignment` (IoStoreWriter.cpp:3615-3616, misalignment check :993), default **16 KiB** (`DefaultMemoryMappingAlignment = 16 << 10`, IoStoreUtilities.cpp:147, applied via `-alignformemorymapping` parse-with-default at :9562). **Pak-side:** UnrealPak only strips compression/aligns `.m.ubulk` when `-AlignForMemoryMapping>0` (PakFileUtilities.cpp:1435-1442), which UAT passes only if the platform ini has `[MemoryMappedFiles] Enable + Alignment` (CopyBuildToStagingDirectory.Automation.cs:4242-4254) — shipped only by iOS and Android (`Engine/Config/IOS/BaseIOSEngine.ini:88`, `Engine/Config/Android/BaseAndroidEngine.ini:39`).

### The headline: platform truth table

`FGenericPlatformProperties::SupportsMemoryMappedFiles()` returns **false** (GenericPlatformProperties.h:258-261). **Windows overrides to true** (WindowsPlatformProperties.h:76-79). **Linux and Mac do not override it** — both property structs derive from the generic (LinuxPlatformProperties.h:20-21, MacPlatformProperties.h:19-20; no `MemoryMapped` token in either file). `FFileIoStore::OpenMapped` hard-fails on that same property (IoDispatcherFileBackend.cpp:2124-2127), and the cook-side feature check resolves from the same properties (TargetPlatformBase.h:493-494) — so Linux/macOS cooks won't even set the flag. The OS capability exists (full `FUnixPlatformFile::OpenMappedEx2`/`MapRegion`, UnixPlatformFile.cpp:1331, :901) — it is the engine's *property gate* that says no. **Consequence: arch §9's "VFS reads memory-mapped bundle entries" is literally true only on Win64.** On Linux/macOS the identical asset loads inline/resident. The honest product statement for the docs: *"memory-mapped on platforms the engine maps (Win64); a single resident buffer elsewhere — the VFS reads a span either way."* Optionally file the one-line `SupportsMemoryMappedFiles() => true` Linux/Mac property override as an engine suggestion — but v1 must not depend on engine patches.

### The VFS branch

Load side of `UVaCuusBundle::Serialize`: `Payload.Serialize(Ar, this, INDEX_NONE, /*bAttemptFileMapping*/ FPlatformProperties::SupportsMemoryMappedFiles())` — mapping is only legal while loading (`check(!bAttemptFileMapping || Ar.IsLoading())`, BulkData.cpp:1223), which is why mounting **must** capture whatever the load produced rather than re-request mapping later. At mount: `FOwnedBulkDataPtr* Owned = Payload.StealFileMapping()` (BulkData.h:863-867) → wrap in a ref-counted `FVaCuusBundleMount{ TUniquePtr<FOwnedBulkDataPtr>, const uint8* Base, int64 Size, TMap<FString, FVaCuusBundleEntry> }`. `GetPointer()` returns the region or the allocation transparently (BulkData.h:238; allocation-vs-mapped union BulkData.h:260-270). After the steal, the UObject can be GC'd without touching the mount — the SoundWave family does exactly this steal-and-own move (SoundWave.cpp:2610-2613, engine's own inverted-looking branch noted and not imitated).

`FVaCuusFileInterface::Open` (VaCuusFileInterface.cpp:59-100) grows one branch ahead of the root scan: normalize the relative path → look up the current mount table → on hit return a span-backed handle `FOpenSpan{const uint8* Data; int64 Size; int64 Position}`. `Read` = clamped memcpy; `Seek/Tell/Length` over `Position/Size` exactly as today's `FOpenFile` contract (the whole Unix `FFileHandleUnix` EOF-clamp saga, VaCuusFileInterface.cpp:14-51, simply does not exist for a span — the `[0, Size]` position model carries over unchanged, per Rml's Tell/Length spec cited there at :26-29). RmlUi's Read/Seek/Tell/Length needs nothing a span can't give; FreeType font loads and image reads go through the same interface.

## 4. Q4 — Load/registration flow, thread safety, precedence

- **Who loads:** `UVaCuusSubsystem::Initialize` (VaCuusSubsystem.cpp:29) resolves a config-listed soft path (new `UVaCuusRuntimeSettings`, `[inference]` — no settings object exists yet in `Source/VaCuus/Public/`) and mounts; plus a public `MountBundle(UVaCuusBundle*)/UnmountBundle` for games that manage their own loading. Mount is idempotent per asset; multiple bundles mount in order (first hit wins).
- **UI-thread safety:** the mount table lives behind the same pattern as everything else in this plugin — game thread mutates a pending list, the UI thread adopts it at the top of its frame; `Open()` (UI thread, during `LoadDocument`) only ever sees the adopted table. Every open span handle holds a strong ref to its `FVaCuusBundleMount`; unmount removes the mount from the table and drops the subsystem's ref — the region is destroyed when the **last open handle closes**, so a document mid-read cannot lose its bytes. Destructor ordering inside the mount: region before handle ("the only way to close the file handle" is the destructor, MappedFileHandle.h:124-128). Page-fault IO lands on the UI thread by construction; acceptable (that thread owns document loading already), and `PreloadHint(0, Size)` at mount (MappedFileHandle.h:69-77) prefaults the whole ≤10 MB bundle for pennies.
- **Precedence (the honest decision arch §9 ducked):** **bundle-first when mounted.** Rationale: the bundle exists to make shipping deterministic; if a stale loose file could shadow it in a packaged Development build, the config closest to shipping would be the least tested. Editor/PIE mounts nothing by default → loose-first behavior is preserved where live-reload matters. `vacuus.Bundle.Enable 1` force-mounts in PIE for parity testing. Every bundle hit logs Verbose naming the bundle, exactly like the root log at VaCuusFileInterface.cpp:73-79 — same stale-duplicate-visibility principle, third source added.
- Live reload never applies to bundle-served content (the M2 watcher watches loose roots only) — documented, not "fixed".

## 5. Q5 — Retiring the staging globs

`RuntimeDependencies` is a plain list on `ModuleRules` (ModuleRules.cs:1420-1424) with **no configuration awareness of its own** — but rules code runs per-target-per-configuration and `Target.Configuration` is right there (`ReadOnlyTargetRules.Configuration => Inner.Configuration`, ReadOnlyTargetRules.cs:58; backing field TargetRules.cs:700-702). So the clean split is code, not config:

```csharp
if (Target.Configuration != UnrealTargetConfiguration.Shipping)
{
    foreach (string Pattern in ...) { RuntimeDependencies.Add(DevUIDir + "/.../" + Pattern, StagedFileType.UFS); }
}
```
- **Shipping:** bundle only. The `Tests/*.js` rides-along caveat (VaCuus.Build.cs:115-116) retires because the globs are gone *and* the pack step excludes `Tests/` anyway.
- **Non-Shipping packaged:** both present; bundle-first precedence (§4) makes behavior match Shipping while the loose files stay available for `vacuus.Bundle.Enable 0` A/B debugging.
- The makefile-staleness trap stays for dev builds and keeps its Build.cs documentation (the add-a-document/touch-the-rules dance, VaCuus.Build.cs:79-109 — verified analysis recorded in that file); it now matters strictly less because Shipping no longer depends on the receipt at all.

## 6. Experiments (named; unsettleable by reading)

| Name | Question | Method | Where |
|---|---|---|---|
| **Exp-COOK-FILEDEP** | Does the Function/File dependency actually recook on a loose-file edit with the `.uasset` untouched — under ZenStore on AND off (legacy path, CookOnTheFlyServer.cpp:10590-10598)? | Cook VcHost twice, edit one `.rml` between, assert bundle repack log line in cook 2; repeat with `bUseZenStore=false` | this machine |
| **Exp-COOK-ADDFILE** | Does the tree-hash Function dep catch a file *added* between cooks? | Cook, add `new.rcss`, cook, assert repack | this machine |
| **Exp-MMAP-FALLBACK-LINUX** | Linux cooked build: flag never set, payload inline-resident, VFS serves spans; measure load-time cost of the resident path | Linux Shipping cook + run, assert `!IsDataMemoryMapped()`, gate timings | this machine |
| **Exp-MMAP-WIN64** | Win64 IoStore Shipping: `BULKDATA_DataIsMemoryMapped` actually set; `stat MappedFileMemory` (MappedFileHandle.h:10) shows the bundle; §11 disk/RAM gates | cooked Win64 run | **owner hardware** |
| **Exp-PAK-NOIOSTORE-WIN64** | Pak-only Win64 (no `[MemoryMappedFiles]` ini → possibly compressed `.m.ubulk`): confirm graceful resident fallback via IPlatformFilePak.cpp:4694-4697 refusal | cooked pak-only Win64 run | **owner hardware** |
| **Exp-BUNDLE-UNMOUNT-RACE** | Unmount while a document load is mid-read on the UI thread: last-handle-closes teardown holds | automation test, two threads, fault injection | this machine |
| **Exp-COOK-56-57** | Whole cook-hook surface on 5.6/5.7 (`OnCookEvent`/`FCookDependency` age; File dep exists since ~5.4/5.5 but **unverified on those trees here**) | compile + Exp-COOK-FILEDEP on 5.6/5.7 clones | **owner hardware** (no 5.6/5.7 on this machine) |

**Handoff-checklist items produced by this task:** Exp-MMAP-WIN64, Exp-PAK-NOIOSTORE-WIN64, Exp-COOK-56-57, plus macOS mirror of Exp-MMAP-FALLBACK-LINUX (identical expected outcome: property false, MacPlatformProperties.h:19-20).

**Files referenced (plugin):** `/w/Unreal/VcHost/Plugins/VaCuus/Source/VaCuus/Private/VaCuusFileInterface.cpp`, `/w/Unreal/VcHost/Plugins/VaCuus/Source/VaCuus/Private/VaCuusContentPaths.cpp`, `/w/Unreal/VcHost/Plugins/VaCuus/Source/VaCuus/VaCuus.Build.cs`, `/w/Unreal/VcHost/Plugins/VaCuus/VaCuus.uplugin`, `/w/Unreal/VcHost/Plugins/VaCuus/docs/superpowers/specs/2026-07-29-vacuus-architecture-design.md` (§9 wording correction + §15 CanContainContent confirmation).
