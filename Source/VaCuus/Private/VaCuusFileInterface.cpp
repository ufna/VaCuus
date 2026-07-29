// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusFileInterface.h"

#include "VaCuusDefines.h"

#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

namespace VaCuusFileInterfacePrivate
{
IFileHandle* ToHandle(Rml::FileHandle File)
{
	return reinterpret_cast<IFileHandle*>(File);
}
} // namespace VaCuusFileInterfacePrivate

Rml::FileHandle FVaCuusFileInterface::Open(const Rml::String& Path)
{
	const FString RequestedPath = UTF8_TO_TCHAR(Path.c_str());

	FString FullPath = RequestedPath;
	if (FPaths::IsRelative(RequestedPath))
	{
		FullPath = FPaths::ProjectContentDir() / TEXT("DevUI") / RequestedPath;
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

	return reinterpret_cast<Rml::FileHandle>(Handle);
}

void FVaCuusFileInterface::Close(Rml::FileHandle File)
{
	delete VaCuusFileInterfacePrivate::ToHandle(File);
}

size_t FVaCuusFileInterface::Read(void* Buffer, size_t Size, Rml::FileHandle File)
{
	IFileHandle* Handle = VaCuusFileInterfacePrivate::ToHandle(File);
	if (Handle == nullptr)
	{
		return 0;
	}

	// IFileHandle::Read is all-or-nothing, so clamp to the bytes actually left.
	const int64 Remaining = Handle->Size() - Handle->Tell();
	const int64 BytesToRead = FMath::Min(static_cast<int64>(Size), Remaining);
	if (BytesToRead <= 0)
	{
		return 0;
	}

	return Handle->Read(static_cast<uint8*>(Buffer), BytesToRead) ? static_cast<size_t>(BytesToRead) : 0;
}

bool FVaCuusFileInterface::Seek(Rml::FileHandle File, long Offset, int Origin)
{
	IFileHandle* Handle = VaCuusFileInterfacePrivate::ToHandle(File);
	if (Handle == nullptr)
	{
		return false;
	}

	const int64 FileSize = Handle->Size();

	int64 Target = 0;
	switch (Origin)
	{
	case SEEK_SET:
		Target = Offset;
		break;
	case SEEK_CUR:
		Target = Handle->Tell() + Offset;
		break;
	case SEEK_END:
		Target = FileSize + Offset;
		break;
	default:
		return false;
	}

	// IFileHandle::Seek asserts on negative positions (and SeekFromEnd on positive
	// offsets), so validate the computed target before delegating.
	if (Target < 0 || Target > FileSize)
	{
		return false;
	}

	return Handle->Seek(Target);
}

size_t FVaCuusFileInterface::Tell(Rml::FileHandle File)
{
	IFileHandle* Handle = VaCuusFileInterfacePrivate::ToHandle(File);
	return Handle ? static_cast<size_t>(Handle->Tell()) : 0;
}

size_t FVaCuusFileInterface::Length(Rml::FileHandle File)
{
	IFileHandle* Handle = VaCuusFileInterfacePrivate::ToHandle(File);
	return Handle ? static_cast<size_t>(Handle->Size()) : 0;
}
