// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusBundleFactory.h"

#include "VaCuusBundle.h"
#include "VaCuusContentPaths.h"
#include "VaCuusDefines.h"

#include "HAL/IConsoleManager.h"
#include "Misc/DateTime.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace VaCuusBundleFactoryPrivate
{
/**
 * Written ONCE, at creation, and never by the cook: a per-cook rewrite (a fresh
 * timestamp, say) would make cooked packages nondeterministic, which is exactly the
 * churn the marker-only editor save exists to avoid. The creation timestamp is fine
 * because it is constant for the asset's whole life.
 */
static FString MakeSourceNote(const TCHAR* CreatedBy)
{
	return FString::Printf(TEXT("Created by %s at %s. Packs the DevUI roots at cook: %s"),
		CreatedBy, *FDateTime::Now().ToString(),
		*FString::Join(VaCuusContentPaths::GetDocumentRoots(), TEXT(" | ")));
}
}	 // namespace VaCuusBundleFactoryPrivate

UVaCuusBundleFactory::UVaCuusBundleFactory()
{
	// The UDataAssetFactory shape (EditorFactories.cpp:7357-7363), minus the class
	// picker it needs and this asset does not: bCreateNew puts the type in the
	// Content Browser's New menu; nothing to edit after creation, because the asset
	// is a marker (see the class comment).
	bCreateNew = true;
	bEditAfterNew = false;
	SupportedClass = UVaCuusBundle::StaticClass();
}

UObject* UVaCuusBundleFactory::FactoryCreateNew(
	UClass* Class, UObject* InParent, FName Name, EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn)
{
	UVaCuusBundle* Bundle = NewObject<UVaCuusBundle>(InParent, Class, Name, Flags);
	Bundle->SourceNote = VaCuusBundleFactoryPrivate::MakeSourceNote(TEXT("UVaCuusBundleFactory"));
	return Bundle;
}

namespace VaCuusBundleCreateCommand
{
/**
 * `vacuus.Bundle.CreateAsset [/LongPackage/Path]` -- the factory, headless. The M6
 * cook experiments (and any buyer scripting their pipeline) need a bundle asset
 * without a Content Browser session; this creates the marker asset and saves its
 * package in one -ExecCmds step. Default path matches the host's config
 * ([VaCuus] BundleAssetPath) and cook listing (DirectoriesToAlwaysCook).
 */
static void CreateBundleAsset(const TArray<FString>& Args)
{
	const FString PackagePath = Args.Num() > 0 ? Args[0] : TEXT("/VaCuus/Bundles/DevUIBundle");

	if (!FPackageName::IsValidLongPackageName(PackagePath))
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("vacuus.Bundle.CreateAsset: '%s' is not a long package name (expected e.g. /VaCuus/Bundles/DevUIBundle)"),
			*PackagePath);
		return;
	}

	const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
	UPackage* Package = CreatePackage(*PackagePath);
	if (Package == nullptr)
	{
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.Bundle.CreateAsset: could not create package '%s'"), *PackagePath);
		return;
	}

	UVaCuusBundle* Bundle = FindObject<UVaCuusBundle>(Package, *AssetName);
	if (Bundle == nullptr)
	{
		Bundle = NewObject<UVaCuusBundle>(Package, *AssetName, RF_Public | RF_Standalone);
		Bundle->SourceNote = VaCuusBundleFactoryPrivate::MakeSourceNote(TEXT("vacuus.Bundle.CreateAsset"));
	}

	FString Filename;
	if (!FPackageName::TryConvertLongPackageNameToFilename(PackagePath, Filename, FPackageName::GetAssetPackageExtension()))
	{
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.Bundle.CreateAsset: no filename mapping for '%s' (is the mount point valid?)"),
			*PackagePath);
		return;
	}

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	const bool bSaved = UPackage::SavePackage(Package, Bundle, *Filename, SaveArgs);

	UE_LOG(LogVaCuus, Display, TEXT("vacuus.Bundle.CreateAsset: %s '%s' (%s)"),
		bSaved ? TEXT("saved") : TEXT("FAILED to save"), *PackagePath, *Filename);
}

static FAutoConsoleCommand GCreateBundleAssetCommand(
	TEXT("vacuus.Bundle.CreateAsset"),
	TEXT("Create and save a UVaCuusBundle marker asset at the given long package path (default ")
	TEXT("/VaCuus/Bundles/DevUIBundle). The asset packs the loose DevUI tree when cooked; an editor save carries ")
	TEXT("only provenance."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&CreateBundleAsset));
}	 // namespace VaCuusBundleCreateCommand
