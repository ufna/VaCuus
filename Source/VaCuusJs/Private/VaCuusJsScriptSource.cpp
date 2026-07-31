// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusJsScriptSource.h"

#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"

namespace VaCuusJsScriptSource
{
bool ReadScriptFile(const FString& AbsolutePath, FString& OutSource)
{
	// IPlatformFile::OpenRead, not IFileManager convenience wrappers, to stay on
	// the exact pak-transparent path the document VFS already uses
	// (FVaCuusFileInterface::Open, VaCuusFileInterface.cpp).
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	const TUniquePtr<IFileHandle> File(PlatformFile.OpenRead(*AbsolutePath));
	if (!File.IsValid())
	{
		return false;
	}

	const int64 Size = File->Size();
	if (Size < 0 || Size > MAX_int32)
	{
		return false;
	}

	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(static_cast<int32>(Size));
	if (Size > 0 && !File->Read(Bytes.GetData(), Size))
	{
		return false;
	}

	// UTF-8 in, UTF-16/UTF-8 BOMs tolerated: BufferToString sniffs the BOM and
	// decodes the BOM-less remainder as UTF-8 (FileHelper.cpp:147-184).
	FFileHelper::BufferToString(OutSource, Bytes.GetData(), Bytes.Num());
	return true;
}
}	 // namespace VaCuusJsScriptSource
