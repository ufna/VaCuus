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
		PublicDefinitions.Add("RMLUI_CUSTOM_RTTI=1");

		if (Target.LinkType == TargetLinkType.Monolithic)
		{
			PublicDefinitions.Add("RMLUI_STATIC_LIB=1");
		}
		else
		{
			// Shared-library decorations (see Include/RmlUi/Core/Header.h): without
			// RMLUI_STATIC_LIB, RMLUICORE_API is visibility("default") on non-Windows,
			// and on Win32 RMLUI_CORE_EXPORTS flips dllexport (compiling this module)
			// vs dllimport (consumers), so Rml symbols cross the module boundary.
			PrivateDefinitions.Add("RMLUI_CORE_EXPORTS=1");
		}

		// Compile-time switch in Core.cpp that installs FontEngineInterfaceDefault
		// (FreeType) as the default font engine; only read by .cpp files, so Private.
		PrivateDefinitions.Add("RMLUI_FONT_ENGINE_FREETYPE=1");

		PrivateDependencyModuleNames.AddRange(new[] { "Core" });
		AddEngineThirdPartyPrivateStaticDependencies(Target, "FreeType2");

		foreach (string Dir in new[] { "Source/Core", "Source/Core/Elements", "Source/Core/Layout", "Source/Core/FontEngineDefault" })
		{
			string Abs = Path.Combine(RmlRoot, Dir);
			PrivateIncludePaths.Add(Abs);
		}
	}
}
