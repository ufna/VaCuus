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
			// an event: nothing in a packaged game or a -game process pumps the watcher
			// (in the editor that is UEditorEngine::Tick, EditorEngine.cpp:1948).
			"DirectoryWatcher",

			"Engine",

			// IPluginManager, to locate this plugin's own directory when checking whether a
			// SECOND checkout of it exists (see WarnAboutSecondWorkingTree). UnrealEd below
			// happens to re-export it, but the dependency is ours, so it is declared rather
			// than inherited -- the same rule Source/VaCuus/VaCuus.Build.cs states for
			// ApplicationCore and InputCore.
			"Projects",

			"Slate",
			"SlateCore",
			"UnrealEd"
		});
	}
}
