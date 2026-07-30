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
	}
}
