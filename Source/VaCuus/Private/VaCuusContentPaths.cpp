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
}	 // namespace VaCuusContentPaths
