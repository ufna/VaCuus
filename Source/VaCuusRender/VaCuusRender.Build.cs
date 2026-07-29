// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;

public class VaCuusRender : ModuleRules
{
	public VaCuusRender(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",

			// Public: VaCuusRecordingRenderInterface.h derives from Rml::RenderInterface.
			"VaCuusRml"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			// RHI/Slate side of the render backend (replayer, widget) lands in later
			// tasks; declared up front so the module shape is final.
			"RHI",
			"RenderCore",
			"Renderer",
			"SlateCore",
			"Slate",
			"Projects",

			// M1 LoadTexture: synchronous image decode on the game thread.
			"ImageWrapper",

			"VaCuus"
		});
	}
}
