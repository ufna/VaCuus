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
			"VaCuusRml",

			// Public: VaCuusReplayRenderer.h exposes FTextureRHIRef/FBufferRHIRef.
			"RHI"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			// Slate side of the render backend (widget, composite) lands in later
			// tasks; declared up front so the module shape is final.
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
