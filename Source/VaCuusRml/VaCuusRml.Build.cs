// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class VaCuusRml : ModuleRules
{
	public VaCuusRml(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.NoPCHs;
		bUseUnity = false;
		CppCompileWarningSettings.UndefinedIdentifierWarningLevel = WarningLevel.Off;
		CppCompileWarningSettings.ShadowVariableWarningLevel = WarningLevel.Off;
		bWarningsAsErrors = false;
		bEnableExceptions = false;
		CppStandard = CppStandardVersion.Cpp20;

		string RmlRoot = Path.Combine(ModuleDirectory, "../ThirdParty/RmlUi");
		PublicIncludePaths.Add(Path.Combine(RmlRoot, "Include"));
		PublicDefinitions.Add("RMLUI_STATIC_LIB=1");
		PublicDefinitions.Add("RMLUI_CUSTOM_RTTI=1");

		// Compile-time switch in Core.cpp that installs FontEngineInterfaceDefault
		// (FreeType) as the default font engine; only read by .cpp files, so Private.
		PrivateDefinitions.Add("RMLUI_FONT_ENGINE_FREETYPE=1");

		PrivateDependencyModuleNames.AddRange(new[] { "Core" });
		AddEngineThirdPartyPrivateStaticDependencies(Target, "FreeType2");

		foreach (string Dir in new[] { "Source/Core", "Source/Core/Elements", "Source/Core/Layout", "Source/Core/FontEngineDefault" })
		{
			string Abs = Path.Combine(RmlRoot, Dir);
			if (Directory.Exists(Abs)) PrivateIncludePaths.Add(Abs);
		}
	}
}
