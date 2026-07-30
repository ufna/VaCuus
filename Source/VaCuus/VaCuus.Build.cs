// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;

public class VaCuus : ModuleRules
{
	public VaCuus(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			// EMouseCursor::Type is part of the published interactive snapshot
			// (VaCuusInteractiveSnapshot.h) and lives in ApplicationCore. Engine
			// re-exports ApplicationCore only when bCompileAgainstApplicationCore is
			// set, so depend on it directly rather than inheriting it by luck.
			"ApplicationCore",
			"Core",
			"CoreUObject",
			"Engine",

			// FKey is part of the published input-event payload
			// (VaCuusInputEvent.h). Engine happens to re-export InputCore, but the
			// dependency is ours, so it is declared rather than inherited.
			"InputCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			// IPluginManager, for the plugin's own Content/DevUI document root (D19).
			// Runtime module, so a packaged game resolves the same root.
			"Projects",

			"VaCuusRml"
		});

		// ------------------------------------------------------------------
		// PACKAGING THE UI DOCUMENTS (Task 14, item A).
		//
		// WHY IT IS HERE AND NOT IN Config/FilterPlugin.ini, which is where the plan expected
		// it. That file is read ONLY by `RunUAT BuildPlugin`, i.e. it decides the contents of
		// a redistributable plugin zip (Scripts/BuildPluginCommand.Automation.cs:472 loads it,
		// :453 reads the section), and the default filter that command builds first already
		// includes /Content/... (:464) -- so the DevUI documents were never missing from a
		// plugin zip, and nothing in CopyBuildToStagingDirectory or DeploymentContext consults
		// that file at all. Editing it would have changed nothing about a packaged game.
		//
		// THIS is what stages loose files into a COOKED GAME: UBT expands the wildcards below
		// at build time (Configuration/UEBuildBinary.cs:211-219) and writes the results into
		// the .target receipt, and CopyBuildToStagingDirectory hands that receipt to
		// DeploymentContext.StageRuntimeDependenciesFromReceipt
		// (CopyBuildToStagingDirectory.Automation.cs:1796, DeploymentContext.cs:1040-1056).
		// ProjectPackagingSettings' DirectoriesToAlwaysStageAsUFS cannot do it either: those
		// entries are resolved against the PROJECT's content root
		// (CopyBuildToStagingDirectory.Automation.cs:2054), never a plugin's.
		//
		// UFS, not NonUFS: FVaCuusFileInterface::Open goes through
		// FPlatformFileManager::Get().GetPlatformFile(), i.e. through the pak layer, and
		// IPlugin::GetContentDir() resolves the same way in a staged build as in the editor
		// (PluginManager.cpp's GetContentDir is FPaths::GetPath(FileName)/Content, and it is
		// in the Runtime `Projects` module). So a document inside the pak opens with no
		// special casing anywhere in VaCuus.
		//
		// EXTENSION-SCOPED RATHER THAN `Content/DevUI/...`, deliberately: RuntimeDependencies
		// bypasses the cooker, so sweeping a whole content directory is how a .uasset ends up
		// staged raw and unusable. Listing the loose formats VaCuus actually reads keeps that
		// impossible by construction. The image list is exactly what the recorder accepts
		// (VaCuusRecordingRenderInterface.cpp:294 -- PNG, JPEG and UEJPEG; every other format
		// is refused at the probe), and the font list is what RmlUi's FreeType interface
		// loads.
		//
		// KNOWN CAVEAT, stated because it is a build-order trap rather than a bug: the
		// wildcards are resolved when UBT runs, so a document added to DevUI after the last
		// build is not in the receipt until the next one. Packaging always builds first, so
		// this only bites someone staging from a stale receipt by hand.
		string DevUIDir = "$(PluginDir)/Content/DevUI";
		foreach (string Pattern in new string[] { "*.rml", "*.rcss", "*.png", "*.jpg", "*.jpeg", "*.ttf", "*.otf" })
		{
			// `.../` before the pattern so subdirectories are included -- img/ and fonts/
			// today, and whatever a document references tomorrow. FileFilter resolves `...`
			// as "any depth" (EpicGames.Core/FileFilter.cs).
			RuntimeDependencies.Add(DevUIDir + "/.../" + Pattern, StagedFileType.UFS);
		}
	}
}
