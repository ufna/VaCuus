// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusFileInterface.h"

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
};

FOpenFile* ToFile(Rml::FileHandle File)
{
	return reinterpret_cast<FOpenFile*>(File);
}
} // namespace VaCuusFileInterfacePrivate

Rml::FileHandle FVaCuusFileInterface::Open(const Rml::String& Path)
{
	const FString RequestedPath = UTF8_TO_TCHAR(Path.c_str());

	// Ordered roots, plugin first (D19). ResolveExistingDocument() existence-checks as it
	// goes, so a hit is a file that opened a moment ago; a miss falls through to the FIRST
	// root below purely so the failure log names a concrete path rather than nothing.
	FString SatisfyingRoot;
	FString FullPath = VaCuusContentPaths::ResolveExistingDocument(RequestedPath, &SatisfyingRoot);
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

	return reinterpret_cast<Rml::FileHandle>(
		new VaCuusFileInterfacePrivate::FOpenFile{Handle, /*Position=*/0, Handle->Size()});
}

void FVaCuusFileInterface::Close(Rml::FileHandle File)
{
	VaCuusFileInterfacePrivate::FOpenFile* Open = VaCuusFileInterfacePrivate::ToFile(File);
	if (Open == nullptr)
	{
		// Rml::FileHandle(0) is the failure value Open() returns, and RmlUi does close
		// handles it never got (StreamFile's destructor runs regardless).
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

	// Re-sync the handle, which Seek() deliberately left where it was. Only when the two
	// disagree: on Unix a read-mode Seek is a bare assignment, but on Windows it is a real
	// SetFilePointerEx, and this runs per document read. The seek cannot be clamped here --
	// Position < Size is guaranteed by BytesToRead > 0.
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
