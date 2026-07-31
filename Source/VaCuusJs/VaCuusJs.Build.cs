// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class VaCuusJs : ModuleRules
{
	public VaCuusJs(ReadOnlyTargetRules Target) : base(Target)
	{
		// The VaCuusRml third-party preamble: no PCH/unity (PCHs do not apply to C
		// TUs anyway), warnings relaxed for vendored code, no exceptions (quickjs
		// is plain C).
		PCHUsage = PCHUsageMode.NoPCHs;
		bUseUnity = false;
		CppCompileWarningSettings.UndefinedIdentifierWarningLevel = WarningLevel.Off;
		CppCompileWarningSettings.ShadowVariableWarningLevel = WarningLevel.Off;
		bWarningsAsErrors = false;
		bEnableExceptions = false;
		CppStandard = CppStandardVersion.Cpp20;

		// The relay .c TUs in Private/Gen (see gen_relays.sh). Upstream builds C11
		// with GNU extensions (quickjs-ng CMakeLists.txt:9-11); UBT's C11 is strict
		// -std=c11 (ClangToolChain.cs:559-561), which clang accepts for these
		// sources with warnings, and warnings are not errors here.
		CStandard = CStandardVersion.C11;

		// PRIVATE include path, deliberately: patch #1 (VENDORED_TAG.txt) strips
		// JS_* symbol visibility, so nothing outside this module could link against
		// quickjs anyway -- keeping the headers unreachable makes the wrong include
		// fail at compile time instead of at load time. All script access goes
		// through the IVaCuusScriptHost seam (M4 spec 3.1).
		string QjsRoot = Path.Combine(ModuleDirectory, "../ThirdParty/quickjs-ng");
		PrivateIncludePaths.Add(QjsRoot);

		// Upstream compile definitions for the core library (quickjs-ng
		// CMakeLists.txt:278-282). Neither BUILDING_QJS_SHARED nor
		// USING_QJS_SHARED: this is a static-style embed; the Win32 JS_EXTERN
		// branch degrades to nothing without them, and the non-Win32 branch is
		// patch #1's subject.
		PrivateDefinitions.Add("_GNU_SOURCE");
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDefinitions.Add("WIN32_LEAN_AND_MEAN");
			PrivateDefinitions.Add("_WIN32_WINNT=0x0601");
			// Upstream needs /experimental:c11atomics under MSVC (quickjs-ng
			// CMakeLists.txt:128); revisit when a Win64 build of this module
			// first runs -- clang-cl and MSVC differ here.
		}

		// EXPORT CHECK (Linux modular builds): patch #1 must keep every JS_*
		// symbol out of the module's dynamic symbol table. After a build, verify:
		//   nm -D Binaries/Linux/libUnrealEditor-VaCuusJs.so | grep ' JS_' -> empty
		// If any JS_ symbol appears, the vendored quickjs.h lost patch #1
		// (VENDORED_TAG.txt has the diff and the re-vendoring procedure).

		// VaCuus for the seam this module implements (IVaCuusScriptHost, declared in
		// core) and the stats/perf-log surface (VACUUS_API). STRICTLY one-way: core
		// must never depend back on this module -- the seam is what keeps quickjs
		// unreachable from everywhere else.
		//
		// VaCuusRml from M4 Task 4 on: the DOM facade wraps Rml::Element directly
		// (ObserverPtr handles, ElementPtr ownership, the OnElementDestroy plugin),
		// and VaCuus keeps its RmlUi dependency Private, so the headers do not
		// arrive transitively. Private here too, same as VaCuus and VaCuusRender:
		// nothing under a Public/ header may name an Rml type by include.
		PrivateDependencyModuleNames.AddRange(new[] { "Core", "VaCuus", "VaCuusRml" });
	}
}
