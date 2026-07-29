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
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			"VaCuusRml"
		});
	}
}
