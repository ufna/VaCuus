// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusJsScriptSource.h"

#include "VaCuusBundleMount.h"
#include "VaCuusContentPaths.h"

// LogVaCuus, not LogVaCuusJS, for the two serving lines in this file: they are VFS
// observability -- the same accounting FVaCuusFileInterface prints -- and the packaged
// gates grep ONE category for it (VaCuusDefines.h exports it VACUUS_API).
#include "VaCuusDefines.h"

#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

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

bool ReadScriptByVfsPath(const FString& VfsPath, FString& OutSource, FString* OutServedFrom)
{
	// Bundle-first for relative paths -- the FVaCuusFileInterface::Open precedence,
	// verbatim, so a script and the document that references it can never be served
	// from two different generations of the same tree.
	if (FPaths::IsRelative(VfsPath))
	{
		if (const TSharedPtr<const FVaCuusBundleLookup> Lookup = FVaCuusBundleMountTable::GetLookup())
		{
			const FString NormalizedPath = VaCuusBundleFormat::NormalizePath(VfsPath);
			for (const TSharedRef<FVaCuusBundleMount>& Mount : Lookup->Mounts)
			{
				const FVaCuusBundleEntry* Entry = Mount->FindEntry(NormalizedPath);
				if (Entry == nullptr)
				{
					continue;
				}
				if (Entry->Size > MAX_int32)
				{
					return false;
				}
				Mount->ServedOpens.fetch_add(1, std::memory_order_relaxed);
				VaCuusScriptServing::NoteBundleScriptServe();
				FFileHelper::BufferToString(OutSource, Mount->Base + Entry->Offset, static_cast<int32>(Entry->Size));
				if (OutServedFrom)
				{
					*OutServedFrom = FString::Printf(TEXT("bundle://%s/%s"), *Mount->BundleName, *NormalizedPath);
				}
				return true;
			}

			if (Lookup->Mounts.Num() > 0)
			{
				// The silent-miss killer, FVaCuusFileInterface::Open's Warning mirrored
				// (spec M6 2(d)): with bundles mounted, a loose script fallback usually
				// means the pack missed a .js -- and a script that quietly serves loose
				// here is invisible to the document-side accounting, because script
				// reads never touch the Rml file interface. Say WHICH bundles were
				// probed, at Warning, before the loose read below.
				TArray<FString> ProbedNames;
				for (const TSharedRef<FVaCuusBundleMount>& Mount : Lookup->Mounts)
				{
					ProbedNames.Add(Mount->BundleName);
				}
				UE_LOG(LogVaCuus, Warning,
					TEXT("Script '%s' is in NO mounted bundle (probed: %s); falling back to the loose roots"),
					*VfsPath, *FString::Join(ProbedNames, TEXT(", ")));
			}
		}
	}

	// Loose fallback. bIncludeMountedBundles = false for the reason
	// FVaCuusFileInterface::Open gives: the bundle probe above already ran against
	// this call's own snapshot, and a pseudo-path is not something the raw
	// IPlatformFile read below could open.
	const FString Resolved =
		VaCuusContentPaths::ResolveExistingDocument(VfsPath, nullptr, /*bIncludeMountedBundles*/ false);
	if (Resolved.IsEmpty() || !ReadScriptFile(Resolved, OutSource))
	{
		return false;
	}

	// Counted only on SUCCESS (a miss served nothing), into the process-wide script
	// tally ~FVaCuusFileInterface prints beside its own open counts -- the parallel
	// line that makes the packaged gates' M==0 cover scripts too. Absolute paths
	// count as loose exactly like Open's absolute passthrough does.
	VaCuusScriptServing::NoteLooseScriptServe();
	if (OutServedFrom)
	{
		*OutServedFrom = Resolved;
	}
	return true;
}
}	 // namespace VaCuusJsScriptSource
