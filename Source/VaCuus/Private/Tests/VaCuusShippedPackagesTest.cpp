// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/EngineVersion.h"
#include "Templates/UniquePtr.h"
#include "UObject/ObjectVersion.h"
#include "UObject/PackageFileSummary.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusShippedPackagesTest, "VaCuus.Core.ShippedPackages",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusShippedPackagesTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("VaCuus"));
	if (!TestTrue(TEXT("VaCuus plugin descriptor found"), Plugin.IsValid()))
	{
		return false;
	}

	TArray<FString> Packages;
	IFileManager::Get().FindFilesRecursive(Packages, *Plugin->GetContentDir(), TEXT("*.uasset"), true, false);
	IFileManager::Get().FindFilesRecursive(Packages, *Plugin->GetContentDir(), TEXT("*.umap"), true, false, false);
	if (!TestTrue(TEXT("the plugin ships packages"), Packages.Num() > 0))
	{
		return false;
	}

	// 5.6 is the oldest engine the plugin supports. It writes up to OS_SUB_OBJECT_SHADOW_SERIALIZATION
	// (ObjectVersion.h:100) and refuses any newer package (PackageFileSummary.h:337), so a re-save on a
	// later editor silently drops 5.6.
	const int32 OldestEngineFileVersionUE5 = static_cast<int32>(EUnrealEngineObjectUE5Version::OS_SUB_OBJECT_SHADOW_SERIALIZATION);

	for (const FString& Path : Packages)
	{
		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Path));
		if (!TestTrue(FString::Printf(TEXT("%s opens"), *Path), Reader.IsValid()))
		{
			continue;
		}
		FPackageFileSummary Summary;
		*Reader << Summary;
		// An LFS pointer checked out without its content fails here, not later in the loader.
		if (!TestEqual(FString::Printf(TEXT("%s is a package"), *Path), static_cast<uint32>(Summary.Tag), static_cast<uint32>(PACKAGE_FILE_TAG)))
		{
			continue;
		}
		// An empty engine version logs a warning on every load (LinkerLoad.cpp:1618).
		TestFalse(FString::Printf(TEXT("%s carries the engine version that saved it"), *Path),
			Summary.SavedByEngineVersion.IsEmpty());
		TestTrue(FString::Printf(TEXT("%s loads on 5.6 (FileVersionUE5 %d)"), *Path, Summary.GetFileVersionUE().FileVersionUE5),
			Summary.GetFileVersionUE().FileVersionUE5 <= OldestEngineFileVersionUE5);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
