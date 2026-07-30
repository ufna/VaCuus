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
		// includes /Content/... (:465) -- so the DevUI documents were never missing from a
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
		// KNOWN CAVEAT, AND IT IS SHARPER THAN A BUILD-ORDER NOTE: a document added to DevUI
		// can be missing from an ordinary package, silently. "Packaging builds first" is not
		// the protection it looks like.
		//
		// The wildcards below are expanded during MAKEFILE GENERATION, not on every build:
		// UEBuildTarget calls Binary.PrepareRuntimeDependencies once while constructing the
		// makefile (UEBuildTarget.cs:3139) and serialises the resulting receipt into the
		// WriteMetadata action's input file right there (UEBuildTarget.cs:3627), which is that
		// action's ONLY input. A build that reuses a cached makefile therefore re-emits the
		// receipt frozen at the last generation; it re-expands nothing.
		//
		// And a new .rml does not invalidate that cache. UBT's invalidation set is broad but
		// it is a set of BUILD INPUTS, never of content: directories that hold .cpp/.h, and
		// only when files are ADDED or REMOVED from them (TargetMakefile.cs:922-957 -- editing
		// a source file recompiles through the action graph without touching the makefile);
		// every module's .Build.cs, the .Target.cs, every .uplugin, Build.version and every
		// ini in the Engine/Game/Encryption/Crypto (plus, for an editor target, Editor)
		// hierarchies, all of which arrive as ExternalDependencies
		// (UEBuildTarget.cs:3455-3487) and are timestamp-compared at TargetMakefile.cs:971-982;
		// plus the platform SDK, the UHT markup set, the command line, BuildConfiguration.xml
		// and the UBT/UHT assemblies themselves. A plugin's Content directory is in none of
		// them. So a SECOND BuildCookRun, after a document was added since the first, can
		// legitimately reuse the makefile, re-emit the old receipt and stage every document
		// except the new one, with no warning anywhere.
		//
		// SO, WHEN A DOCUMENT IS ADDED: touch this file -- it is Module.RulesFile, i.e. an
		// ExternalDependency (UEBuildTarget.cs:3459), so its timestamp alone forces
		// regeneration -- or pass -Rebuild, which deletes the makefile outright
		// (TargetDescriptor.cs:132 -> BuildMode.cs:196-201 -> CleanMode.cs:181). A commit that
		// also adds or removes a .cpp/.h happens to do it for free, which is the likeliest
		// reason this has not bitten yet.
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
