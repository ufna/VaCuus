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

		// SYSTEM, not plain, and the first Win64 build is what proved it has to be. The
		// vendored headers are compiled by OUR modules' TUs, not just this one, and those
		// modules cannot relax warnings: UEBuildModuleCPP.cs:2669 is
		// `Result.bWarningsAsErrors |= Rules.bWarningsAsErrors`, an OR, so a module can only
		// ever turn warnings-as-errors ON. (Which also means the `bWarningsAsErrors = false`
		// above never did anything -- kept only because it still documents the intent for
		// this module's own vendored .cpp TUs.) MSVC then runs /W4 /WX over RmlUi's headers
		// and stops the build on C4800 at DataVariable.h:25 and DataModelHandle.h:22 --
		// `explicit operator bool() const { return definition; }`, a pointer-to-bool
		// conversion that is idiomatic C++ and that clang does not warn about at all, which
		// is exactly why Linux and macOS never saw this.
		//
		// A system include path is the engine's own answer: VCToolChain.cs:281-289 turns it
		// into `/external:I` plus `/external:W0` on MSVC (warning level 0 INSIDE those
		// headers, so nothing is emitted for /WX to promote), and on clang the flag is
		// `-isystem`: ClangToolChain.cs:649 maps every SystemIncludePath through
		// GetSystemIncludePathArgument, and that method is where the literal `-isystem`
		// is written (:636-638). Cite both -- opening :649 alone shows a dispatch and not
		// the flag, which is how this citation read as evidence it did not carry. This
		// covers every clang target, not just the two we ship: AndroidToolChain
		// (AndroidToolChain.cs:17) and IOSToolChain via AppleToolChain
		// (IOSToolChain.cs:15, AppleToolChain.cs:20) both derive from ClangToolChain.
		// Warnings in our OWN code are untouched on every platform; only the vendored
		// headers go quiet.
		PublicSystemIncludePaths.Add(Path.Combine(RmlRoot, "Include"));
		PublicDefinitions.Add("RMLUI_CUSTOM_RTTI=1");

		// RMLUI_DEBUG: DELIBERATELY NOT DEFINED HERE, IN ANY CONFIGURATION -- this comment is
		// the decision record (bead VaCuus-akj.11). The vendored default chain is
		// Include/RmlUi/Core/Platform.h:20-21 (`#if !defined NDEBUG && !defined RMLUI_DEBUG`
		// -> define it), and UBT compiles every configuration this plugin ships with NDEBUG
		// set, so RMLUI_DEBUG is off everywhere and RMLUI_ASSERT / RMLUI_ASSERTMSG /
		// RMLUI_ERROR(MSG) expand to nothing (Debug.h:45-52).
		//
		// Why that is acceptable -- the M2-corrected premise, decided rather than drifted
		// into: what the asserts guard is NOT RmlUi's only diagnostic. Rml::Log::Message is an
		// ordinary unconditional function and every LT_ERROR/LT_WARNING in the library reaches
		// LogVaCuus through FVaCuusSystemInterface::LogMessage (VaCuusSystemInterface.cpp
		// carries the whole argument, :68-90). Only the ASSERT family is gone, so the standing
		// rule is: an RmlUi API whose sole failure signal is an assert is silent here, and
		// VaCuus checks that precondition itself at the call site.
		//
		// Local-debug recipe, when chasing a bug inside the vendored library: add
		//     PublicDefinitions.Add("RMLUI_DEBUG=1");
		// beside this comment (Public, because the assert macros appear in RmlUi HEADERS that
		// every dependent module compiles -- a Private define would build this module's TUs
		// with asserts and everyone else's without). Rml::Assert then logs LT_ASSERT through
		// the system interface and RMLUI_BREAK traps. Do not ship it: the assert bodies cost
		// real work on hot paths, which is why the shipped answer stays OFF.

		// itlib (vendored inside RmlUi) throws in a few container paths; monolithic/non-editor
		// targets compile with exceptions disabled and UBT only lets a module turn exceptions ON
		// (bEnableExceptions |= ...), so the library's no-throw mode is the only option.
		// Public because Config.h pulls flat_map.hpp into every consumer of the RmlUi headers.
		PublicDefinitions.Add("ITLIB_FLAT_MAP_NO_THROW=1");

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

		// FreeType2 plus the two libraries IT needs, named here rather than inherited.
		//
		// FreeType2.Build.cs:66-68 already asks for zlib and UElibPNG on its own behalf
		// ("FreeType needs these to deal with bitmap fonts") -- but that request is made
		// through AddEngineThirdPartyPrivateStaticDependencies, and that helper is a
		// NO-OP for an engine module in an Installed engine:
		// ModuleRules.cs:1601-1607 gates the whole body on
		// `if (!bUsePrecompiled || target.LinkType == TargetLinkType.Monolithic)`.
		// A Launcher/Installed engine sets bUsePrecompiled on every engine module, so in a
		// modular (editor) target FreeType2's own two lines vanish and libfreetype.a arrives
		// with its libpng references unresolved. On a SOURCE engine bUsePrecompiled is false,
		// the lines run, and this module inherits them -- which is why the Linux dev box never
		// saw it. First observed as ~23 `Undefined symbols for architecture arm64: _png_*,
		// referenced from _Load_SBit_Png in libfreetype.a[35](sfnt.c.o)` linking
		// libUnrealEditor-VaCuusRml.dylib against the 5.8.1 Launcher engine on macOS.
		//
		// The references are real and present in the shipped binaries on both platforms
		// (`nm -u` finds png_* in the Mac and the Linux libfreetype.a alike), even though the
		// shipped ftoption.h:276 leaves `FT_CONFIG_OPTION_USE_PNG` commented out -- the header
		// does not describe how Epic's prebuilt archives were configured, so the archive is the
		// authority. macOS's linker rejects an unresolved symbol in a dylib outright; ld.lld
		// does not, which is the second half of why this is a Mac-first failure.
		//
		// The calls below are OUR module's, where bUsePrecompiled is false, so they always run.
		AddEngineThirdPartyPrivateStaticDependencies(Target, "FreeType2");
		AddEngineThirdPartyPrivateStaticDependencies(Target, "zlib");
		AddEngineThirdPartyPrivateStaticDependencies(Target, "UElibPNG");

		foreach (string Dir in new[] { "Source/Core", "Source/Core/Elements", "Source/Core/Layout", "Source/Core/FontEngineDefault" })
		{
			string Abs = Path.Combine(RmlRoot, Dir);
			PrivateIncludePaths.Add(Abs);
		}
	}
}
