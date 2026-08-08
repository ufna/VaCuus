// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusContentPaths.h"

#include "VaCuusBundleMount.h"
#include "VaCuusDefines.h"

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

namespace VaCuusContentPaths
{
namespace Private
{
/** Subdirectory both roots share; the documents themselves are addressed relative to it. */
static const TCHAR* GDevUISubDir = TEXT("DevUI");

static TArray<FString> BuildDocumentRoots()
{
	TArray<FString> Roots;

	// 1. The plugin's own content -- canonical (D19).
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("VaCuus")))
	{
		Roots.Add(FPaths::ConvertRelativePathToFull(Plugin->GetContentDir() / GDevUISubDir));
	}
	else
	{
		// Not fatal: the project root below is still a valid place for documents, and a
		// missing descriptor means something much larger is wrong (VaCuusRender's
		// StartupModule check()s on the same lookup for its shader directory).
		UE_LOG(LogVaCuus, Error,
			TEXT("VaCuus plugin descriptor not found, so the plugin's Content/DevUI cannot be a document root; ")
			TEXT("only <Project>/Content/DevUI will be searched"));
	}

	// 2. The project's, for documents a project adds itself.
	const FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / GDevUISubDir);
	Roots.AddUnique(ProjectRoot);

	UE_LOG(LogVaCuus, Log, TEXT("VaCuus document roots (in order): %s"), *FString::Join(Roots, TEXT(" | ")));
	return Roots;
}
}	 // namespace Private

const TArray<FString>& GetDocumentRoots()
{
	// Function-local static: initialized exactly once, thread-safely, and primed from
	// FVaCuusModule::StartupModule() so the first call is on the game thread.
	static const TArray<FString> Roots = Private::BuildDocumentRoots();
	return Roots;
}

FString ResolveExistingDocument(const FString& VfsPath, FString* OutRoot, bool bIncludeMountedBundles)
{
	if (OutRoot)
	{
		OutRoot->Reset();
	}

	if (VfsPath.IsEmpty())
	{
		return FString();
	}

	if (!FPaths::IsRelative(VfsPath))
	{
		// Absolute passthrough: no root is involved, so there is nothing to report in
		// OutRoot either.
		return FPaths::FileExists(VfsPath) ? VfsPath : FString();
	}

	if (bIncludeMountedBundles)
	{
		// Bundle-first, matching FVaCuusFileInterface::Open exactly (see the header):
		// a caller that existence-checks here and then opens must get the same answer
		// twice. The pseudo-path is deliberately unopenable -- it names the serving
		// bundle for logs, nothing more.
		FString BundleName;
		if (FVaCuusBundleMountTable::ContainsPath(VfsPath, &BundleName))
		{
			const FString BundleRoot = FString::Printf(TEXT("bundle://%s"), *BundleName);
			if (OutRoot)
			{
				*OutRoot = BundleRoot;
			}
			return BundleRoot / VaCuusBundleFormat::NormalizePath(VfsPath);
		}
	}

	for (const FString& Root : GetDocumentRoots())
	{
		const FString Candidate = Root / VfsPath;
		if (FPaths::FileExists(Candidate))
		{
			if (OutRoot)
			{
				*OutRoot = Root;
			}
			return Candidate;
		}
	}

	return FString();
}

namespace Private
{
/**
 * Enough bytes for the longest thing we test for. The Git-LFS v1 pointer's first line is
 * 42 characters (`version https://git-lfs.github.com/spec/v1`); the image signatures are
 * 8 and 3. 64 keeps the read to one block with room to spare.
 */
static constexpr int32 GImageProbeBytes = 64;

/**
 * The v1 pointer's MANDATORY first line, verbatim from the spec
 * (https://github.com/git-lfs/git-lfs/blob/main/docs/spec.md): the `version` key comes
 * first and its value is this exact URL. Matching the line rather than merely "starts
 * with `version`" is what keeps a text file that happens to begin with that word from
 * being reported as a pointer.
 */
static const ANSICHAR* GLfsPointerFirstLine = "version https://git-lfs.github.com/spec/v1";

/** PNG signature, RFC 2083 section 3.1 -- the first eight bytes of every PNG datastream. */
static constexpr uint8 GPngSignature[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

/** JPEG SOI followed by the first marker's 0xFF -- true of JFIF and Exif alike. */
static constexpr uint8 GJpegSignature[] = {0xFF, 0xD8, 0xFF};

static bool HeadMatches(const uint8* Head, int32 NumRead, const uint8* Signature, int32 SignatureLen)
{
	return NumRead >= SignatureLen && FMemory::Memcmp(Head, Signature, SignatureLen) == 0;
}

/**
 * The first bytes a MOUNTED bundle serves for VfsPath, or false when no mount carries it.
 *
 * The bundle's own pseudo-path cannot go through the platform file layer (see
 * ResolveExistingDocument), so the span is read directly. Precedence is not re-derived
 * here: ContainsPath already answered WHICH bundle serves the path under the first-hit-
 * wins rule, and this only finds that record's entry.
 */
static bool ReadBundleHead(const FString& VfsPath, uint8* Head, int32& OutNumRead)
{
	FString BundleName;
	if (!FVaCuusBundleMountTable::ContainsPath(VfsPath, &BundleName))
	{
		return false;
	}

	const TSharedPtr<const FVaCuusBundleLookup> Lookup = FVaCuusBundleMountTable::GetLookup();
	if (!Lookup)
	{
		return false;
	}

	const FString NormalizedPath = VaCuusBundleFormat::NormalizePath(VfsPath);
	for (const TSharedRef<FVaCuusBundleMount>& Mount : Lookup->Mounts)
	{
		if (Mount->BundleName != BundleName)
		{
			continue;
		}
		const FVaCuusBundleEntry* Entry = Mount->FindEntry(NormalizedPath);
		if (!Entry || !Mount->Base)
		{
			return false;
		}
		OutNumRead = static_cast<int32>(FMath::Min<int64>(Entry->Size, GImageProbeBytes));
		if (OutNumRead > 0)
		{
			FMemory::Memcpy(Head, Mount->Base + Entry->Offset, OutNumRead);
		}
		return true;
	}

	return false;
}
}	 // namespace Private

EVaCuusImageProbe ProbeImage(const FString& VfsPath, FString* OutDiagnosis)
{
	if (OutDiagnosis)
	{
		OutDiagnosis->Reset();
	}

	const auto Diagnose = [OutDiagnosis](EVaCuusImageProbe Result, FString&& Sentence) {
		if (OutDiagnosis)
		{
			*OutDiagnosis = MoveTemp(Sentence);
		}
		return Result;
	};

	FString Root;
	const FString ResolvedPath = ResolveExistingDocument(VfsPath, &Root);
	if (ResolvedPath.IsEmpty())
	{
		return Diagnose(EVaCuusImageProbe::Missing,
			FString::Printf(
				TEXT("'%s' is not served by any DevUI root (%s) and no mounted bundle carries it"),
				*VfsPath, *FString::Join(GetDocumentRoots(), TEXT(" | "))));
	}

	uint8 Head[Private::GImageProbeBytes] = {};
	int32 NumRead = 0;
	bool bRead = false;

	if (Root.StartsWith(TEXT("bundle://")))
	{
		bRead = Private::ReadBundleHead(VfsPath, Head, NumRead);
	}
	else if (const TUniquePtr<FArchive> Reader{IFileManager::Get().CreateFileReader(*ResolvedPath)})
	{
		NumRead = static_cast<int32>(FMath::Min<int64>(Reader->TotalSize(), Private::GImageProbeBytes));
		if (NumRead > 0)
		{
			Reader->Serialize(Head, NumRead);
		}
		bRead = !Reader->IsError();
	}

	if (!bRead)
	{
		return Diagnose(EVaCuusImageProbe::Unreadable,
			FString::Printf(TEXT("'%s' resolves to '%s' but its bytes could not be read"), *VfsPath, *ResolvedPath));
	}

	if (Private::HeadMatches(Head, NumRead, Private::GPngSignature, UE_ARRAY_COUNT(Private::GPngSignature)) ||
		Private::HeadMatches(Head, NumRead, Private::GJpegSignature, UE_ARRAY_COUNT(Private::GJpegSignature)))
	{
		return EVaCuusImageProbe::Ok;
	}

	const int32 PointerLen = FCStringAnsi::Strlen(Private::GLfsPointerFirstLine);
	if (NumRead >= PointerLen && FMemory::Memcmp(Head, Private::GLfsPointerFirstLine, PointerLen) == 0)
	{
		return Diagnose(EVaCuusImageProbe::GitLfsPointer,
			FString::Printf(
				TEXT("'%s' (%s) is a Git-LFS POINTER, not an image -- this tree was cloned without git-lfs ")
				TEXT("installed, so every LFS-tracked file is a stub. Run `git lfs pull` to fetch the real bytes"),
				*VfsPath, *ResolvedPath));
	}

	return Diagnose(EVaCuusImageProbe::NotAnImage,
		FString::Printf(
			TEXT("'%s' (%s) begins with neither a PNG nor a JPEG signature and is not a Git-LFS pointer ")
			TEXT("either -- it is truncated, corrupt, or not the file it claims to be"),
			*VfsPath, *ResolvedPath));
}
}	 // namespace VaCuusContentPaths
