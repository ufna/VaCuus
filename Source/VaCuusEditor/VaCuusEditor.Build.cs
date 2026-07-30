// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;

public class VaCuusEditor : ModuleRules
{
	public VaCuusEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"VaCuus"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			"CoreUObject",

			// The DevUI file watcher (controller decision D20). Unconditional, which is
			// what an "Editor" module may do -- UnrealEd itself depends on it directly.
			// A Runtime module would need the bBuildEditor guard AND would never receive
			// an event, since only UEditorEngine::Tick pumps the watcher.
			"DirectoryWatcher",

			"Engine",
			"Slate",
			"SlateCore",
			"UnrealEd"
		});
	}
}
