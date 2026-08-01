// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusFileInterface.h"

#include "VaCuusBundleMount.h"
#include "VaCuusContentPaths.h"
#include "VaCuusDefines.h"

#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

namespace VaCuusFileInterfacePrivate
{
/**
 * An open file plus the LOGICAL read position, which IFileHandle cannot hold.
 *
 * WHY THE POSITION IS OURS AND NOT THE HANDLE'S: for a read-only handle
 * FFileHandleUnix::Seek clamps to the last byte --
 * `FileOffset = NewPosition >= FileSize ? FileSize - 1 : NewPosition`
 * (Unix/UnixPlatformFile.cpp:177) -- and Tell() returns that same member verbatim
 * (:152-157). So seeking to exact EOF leaves the handle at Size - 1: Tell() reports one
 * byte short and the next Read() hands back the LAST BYTE AGAIN instead of nothing.
 * SeekFromEnd() has the same off-by-one (:200), and on an EMPTY file the clamp goes
 * NEGATIVE (FileSize - 1 == -1), which Tell() then returns and our size_t cast turns into
 * ~0. None of that is a Unix quirk we may pass on: Rml::FileInterface::Tell is specified as
 * "the number of bytes from the origin of the file" with no clamp
 * (ThirdParty/RmlUi/Include/RmlUi/Core/FileInterface.h:43-46), and RmlUi's own default
 * Length() is Seek-to-end followed by Tell (:48-52), so a one-short Tell is a truncated
 * document.
 *
 * IT IS ALSO A UNIX-ONLY DIVERGENCE, which is why tracking the position here brings the two
 * platforms together rather than inventing a third contract: both Windows read handles
 * accept exact EOF and report it. FAsyncBufferedFileReaderWindows (what OpenRead returns in
 * a game build) asserts only `InPos <= FileSize` (Windows/WindowsPlatformFile.cpp:397) and
 * Tell() returns its own FilePos (:446-450); FFileHandleWindows (the editor/program build)
 * stores NewPosition unclamped (:748-756) and does the same (:738-742).
 *
 * Size is cached rather than re-read per call because a read-only FFileHandleUnix caches it
 * too (fstat in the constructor, :130-135; Size() returns the member at :340-345), so this
 * changes nothing about what a caller observes -- and it gives the [0, Size] clamp below a
 * domain that cannot shift between a Seek and the Read that follows it.
 */
struct FOpenFile
{
	IFileHandle* Handle = nullptr;

	/** Always in [0, Size]. The handle's own position cannot represent Size. */
	int64 Position = 0;

	int64 Size = 0;

	/**
	 * BUNDLE-SPAN MODE (M6): Span non-null means the bytes live in a mounted bundle's
	 * payload region and Handle is null. The SAME [0, Size] position model carries
	 * over unchanged -- a span trivially represents exact EOF, so the whole
	 * FFileHandleUnix clamp saga above simply does not apply -- and Read() becomes a
	 * clamped memcpy. The span was bounds-validated against the payload at mount
	 * (VaCuusBundleFormat::ValidateEntries), which is what lets Read() trust
	 * Span + Position without re-checking.
	 */
	const uint8* Span = nullptr;

	/**
	 * Keeps the span's region alive for this handle's whole life: unmounting removes
	 * the record from the LOOKUP only, and the last strong reference -- possibly this
	 * one, mid-read -- is what actually frees the bytes (spec M6 section 4; the
	 * unmount-race test observes exactly this).
	 */
	TSharedPtr<FVaCuusBundleMount> Mount;
};

FOpenFile* ToFile(Rml::FileHandle File)
{
	return reinterpret_cast<FOpenFile*>(File);
}
} // namespace VaCuusFileInterfacePrivate

FVaCuusFileInterface::~FVaCuusFileInterface()
{
	// The M==0 line (spec M6 2(d)): the bundle-path acceptance gates grep this for a
	// zero loose count, because with a bundle mounted every loose-served open is a
	// potential stale shadow. Per-bundle counts follow so a nonzero split is
	// attributable; the records outlive this interface (module-shutdown rule), so
	// reading them here is safe.
	const uint64 BundleOpens = GetNumBundleOpens();
	const uint64 LooseOpens = GetNumLooseOpens();
	UE_LOG(LogVaCuus, Log, TEXT("VaCuus VFS teardown: %llu open(s) served by mounted bundles, %llu by loose roots"),
		BundleOpens, LooseOpens);
	for (const TSharedRef<FVaCuusBundleMount>& Record : FVaCuusBundleMountTable::GetAllRecords())
	{
		UE_LOG(LogVaCuus, Log, TEXT("  bundle '%s' served %llu open(s)"), *Record->BundleName,
			static_cast<uint64>(Record->ServedOpens.load(std::memory_order_relaxed)));
	}
}

Rml::FileHandle FVaCuusFileInterface::Open(const Rml::String& Path)
{
	const FString RequestedPath = UTF8_TO_TCHAR(Path.c_str());

	// BUNDLE-FIRST for relative paths (spec M6 2(d)), and the precedence is the
	// decision, not an accident: the bundle exists to make shipping deterministic, so
	// a stale loose file staged next to it (packaged Development stages both) must
	// not shadow it -- the config closest to Shipping would otherwise be the least
	// tested. In the editor nothing is mounted by default, so loose-first behavior
	// (and live reload) is preserved exactly where it matters.
	if (FPaths::IsRelative(RequestedPath))
	{
		if (const TSharedPtr<const FVaCuusBundleLookup> Lookup = FVaCuusBundleMountTable::GetLookup())
		{
			const FString NormalizedPath = VaCuusBundleFormat::NormalizePath(RequestedPath);
			for (const TSharedRef<FVaCuusBundleMount>& Mount : Lookup->Mounts)
			{
				const FVaCuusBundleEntry* Entry = Mount->FindEntry(NormalizedPath);
				if (Entry == nullptr)
				{
					continue;
				}

				// WHICH copy answered -- the same stale-duplicate visibility rule the
				// root log below follows, third source added.
				UE_LOG(LogVaCuus, Verbose, TEXT("Resolved '%s' in bundle '%s' (%lld bytes)"),
					*RequestedPath, *Mount->BundleName, Entry->Size);

				Mount->ServedOpens.fetch_add(1, std::memory_order_relaxed);
				NumBundleOpens.fetch_add(1, std::memory_order_relaxed);

				auto* Open = new VaCuusFileInterfacePrivate::FOpenFile();
				Open->Span = Mount->Base + Entry->Offset;
				Open->Size = Entry->Size;
				Open->Mount = Mount;
				return reinterpret_cast<Rml::FileHandle>(Open);
			}

			if (Lookup->Mounts.Num() > 0)
			{
				// The silent-miss killer (spec M6 2(d)): with bundles mounted, a loose
				// fallback usually means the pack missed a file -- say WHICH bundles
				// were probed, at Warning, before quietly serving from disk.
				TArray<FString> ProbedNames;
				for (const TSharedRef<FVaCuusBundleMount>& Mount : Lookup->Mounts)
				{
					ProbedNames.Add(Mount->BundleName);
				}
				UE_LOG(LogVaCuus, Warning,
					TEXT("'%s' is in NO mounted bundle (probed: %s); falling back to the loose roots"),
					*RequestedPath, *FString::Join(ProbedNames, TEXT(", ")));
			}
		}
	}

	// Ordered roots, plugin first (D19). ResolveExistingDocument() existence-checks as it
	// goes, so a hit is a file that opened a moment ago; a miss falls through to the FIRST
	// root below purely so the failure log names a concrete path rather than nothing.
	// bIncludeMountedBundles = false: the bundle probe already ran above against THIS
	// call's own lookup snapshot; letting the resolver probe a fresh one could hand
	// back a bundle pseudo-path for a mount published between the two reads, and a
	// pseudo-path is not something PlatformFile below can open.
	FString SatisfyingRoot;
	FString FullPath =
		VaCuusContentPaths::ResolveExistingDocument(RequestedPath, &SatisfyingRoot, /*bIncludeMountedBundles*/ false);
	if (FullPath.IsEmpty())
	{
		const TArray<FString>& Roots = VaCuusContentPaths::GetDocumentRoots();
		FullPath = (FPaths::IsRelative(RequestedPath) && Roots.Num() > 0) ? Roots[0] / RequestedPath : RequestedPath;
	}
	else if (!SatisfyingRoot.IsEmpty())
	{
		// WHICH copy answered, which is the whole point of the ordered list: a document
		// that unexpectedly comes from the project root is exactly the stale-duplicate
		// situation D19 exists to make visible.
		UE_LOG(LogVaCuus, Verbose, TEXT("Resolved '%s' under root '%s'"), *RequestedPath, *SatisfyingRoot);
	}

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	// Unix open() happily opens directories, which would false-succeed here and
	// hand RmlUi an unreadable "file"; reject them explicitly.
	if (PlatformFile.DirectoryExists(*FullPath))
	{
		UE_LOG(LogVaCuus, Warning, TEXT("Refusing to open directory '%s' as a file"), *FullPath);
		return Rml::FileHandle(0);
	}

	IFileHandle* Handle = PlatformFile.OpenRead(*FullPath);
	if (Handle == nullptr)
	{
		UE_LOG(LogVaCuus, Warning, TEXT("Failed to open file '%s' (resolved to '%s')"), *RequestedPath, *FullPath);
		return Rml::FileHandle(0);
	}

	NumLooseOpens.fetch_add(1, std::memory_order_relaxed);

	auto* Open = new VaCuusFileInterfacePrivate::FOpenFile();
	Open->Handle = Handle;
	Open->Size = Handle->Size();
	return reinterpret_cast<Rml::FileHandle>(Open);
}

void FVaCuusFileInterface::Close(Rml::FileHandle File)
{
	VaCuusFileInterfacePrivate::FOpenFile* Open = VaCuusFileInterfacePrivate::ToFile(File);
	if (Open == nullptr)
	{
		// Defensive, not required: Rml::StreamFile guards every Close() call with
		// `if (file_handle)` (ThirdParty/RmlUi/Source/Core/StreamFile.cpp:16-17, :25-26,
		// :44-46), so RmlUi's own file stream never hands back the 0 that Open() returns on
		// failure. Kept because the previous `delete (IFileHandle*)0` tolerated it and every
		// other method here null-checks too -- a custom stream is free to be less careful.
		return;
	}

	delete Open->Handle;
	delete Open;
}

size_t FVaCuusFileInterface::Read(void* Buffer, size_t Size, Rml::FileHandle File)
{
	VaCuusFileInterfacePrivate::FOpenFile* Open = VaCuusFileInterfacePrivate::ToFile(File);
	if (Open == nullptr)
	{
		return 0;
	}

	// IFileHandle::Read is all-or-nothing, so clamp to the bytes actually left. Counted
	// from OUR position: at exact EOF the handle's own is Size - 1 (see FOpenFile) and
	// this would read the last byte a second time.
	const int64 BytesToRead = FMath::Min(static_cast<int64>(Size), Open->Size - Open->Position);
	if (BytesToRead <= 0)
	{
		return 0;
	}

	if (Open->Span != nullptr)
	{
		// A bundle span: the clamp above plus the mount-time bounds validation is the
		// whole safety argument, and the strong Mount reference is what makes the
		// source pointer valid even if the bundle was unmounted mid-read.
		FMemory::Memcpy(Buffer, Open->Span + Open->Position, BytesToRead);
		Open->Position += BytesToRead;
		return static_cast<size_t>(BytesToRead);
	}

	// Re-sync the handle, which Seek() deliberately left where it was. Guarded on a mismatch
	// rather than done unconditionally because Seek() is not always free: on Unix a read-mode
	// one is a bare assignment (UnixPlatformFile.cpp:175-179), but
	// FAsyncBufferedFileReaderWindows::Seek waits out the in-flight read and, for a target
	// outside its buffer, starts a NEW async read (WindowsPlatformFile.cpp:407-433). The seek
	// here cannot hit the Unix clamp: BytesToRead > 0 means Position < Size.
	if (Open->Handle->Tell() != Open->Position && !Open->Handle->Seek(Open->Position))
	{
		return 0;
	}

	if (!Open->Handle->Read(static_cast<uint8*>(Buffer), BytesToRead))
	{
		return 0;
	}

	Open->Position += BytesToRead;
	return static_cast<size_t>(BytesToRead);
}

bool FVaCuusFileInterface::Seek(Rml::FileHandle File, long Offset, int Origin)
{
	VaCuusFileInterfacePrivate::FOpenFile* Open = VaCuusFileInterfacePrivate::ToFile(File);
	if (Open == nullptr)
	{
		return false;
	}

	// The three origins are the whole contract -- Rml::FileInterface::Seek documents
	// exactly SEEK_SET, SEEK_END and SEEK_CUR (FileInterface.h:36-42) -- so anything else
	// is a caller bug to refuse rather than a mode to guess at.
	int64 Target = 0;
	switch (Origin)
	{
	case SEEK_SET:
		Target = Offset;
		break;
	case SEEK_CUR:
		Target = Open->Position + Offset;
		break;
	case SEEK_END:
		Target = Open->Size + Offset;
		break;
	default:
		return false;
	}

	if (Target < 0 || Target > Open->Size)
	{
		return false;
	}

	// THE HANDLE IS NOT MOVED HERE. It cannot hold Target == Size (see FOpenFile), and
	// Read() re-syncs it from Position anyway -- which also means a refused seek above
	// cannot leave the handle somewhere Tell() does not describe.
	Open->Position = Target;
	return true;
}

size_t FVaCuusFileInterface::Tell(Rml::FileHandle File)
{
	const VaCuusFileInterfacePrivate::FOpenFile* Open = VaCuusFileInterfacePrivate::ToFile(File);
	return Open ? static_cast<size_t>(Open->Position) : 0;
}

size_t FVaCuusFileInterface::Length(Rml::FileHandle File)
{
	const VaCuusFileInterfacePrivate::FOpenFile* Open = VaCuusFileInterfacePrivate::ToFile(File);
	return Open ? static_cast<size_t>(Open->Size) : 0;
}
