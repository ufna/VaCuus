// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;

public class VaCuus : ModuleRules
{
	public VaCuus(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			"VaCuusRml"
		});
	}
}
