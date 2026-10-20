// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.
using UnrealBuildTool;
using System.IO;

public class VaCuusVectorDependencies : ModuleRules
{
	public VaCuusVectorDependencies(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;
		// Imported privately by VaCuusRml: vendor headers stay system headers without exposing them to clients.
		foreach (string Dir in new[] {
			"lunasvg/include", "lunasvg/source", "lunasvg/plutovg/include", "lunasvg/plutovg/source",
			"rlottie", "rlottie/inc", "rlottie/src/vector", "rlottie/src/vector/freetype",
			"rlottie/src/vector/pixman", "rlottie/src/lottie", "rlottie/src/lottie/zip"
		})
		{
			PublicSystemIncludePaths.Add(Path.GetFullPath(Path.Combine(ModuleDirectory, "..", Dir)));
		}
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.Add("Shlwapi.lib");
		}
	}
}
