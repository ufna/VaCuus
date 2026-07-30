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
			// The EKeys::* FKey statics (mouse buttons in the widget's input path and in
			// VaCuus.Input.SlateRouting) are exported by InputCore, not by Engine's
			// re-export -- referencing them needs the link dependency.
			"InputCore",

			// Slate side of the render backend (widget, composite) lands in later
			// tasks; declared up front so the module shape is final.
			"RenderCore",
			"Renderer",
			"SlateCore",
			"Slate",
			"Projects",

			// LoadTexture: synchronous dimension probe on the UI thread, async decode
			// on a worker (both through the module pointer cached at startup).
			"ImageWrapper",

			// UVaCuusWidget derives from UWidget (VaCuusUMGWidget.h).
			"UMG",

			"VaCuus"
		});
	}
}
