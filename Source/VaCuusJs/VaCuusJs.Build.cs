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

			// _WIN32_WINNT IS DELIBERATELY NOT SET HERE. It used to be forced to 0x0601
			// (Windows 7), which the first Win64 build showed to be both redundant and
			// harmful: UBT already puts `#define _WIN32_WINNT 0x0A00` in the module's
			// generated Definitions.h, so ours landed 26 lines below it and won, producing
			// C4005 "macro redefinition" 23 times AND compiling this module's UE headers
			// against a Windows 7 API level while every other module in the build saw
			// Windows 10. Upstream only ever needed a floor; the engine's value is higher
			// than the floor, so the correct action is to leave it alone.

			// THE /experimental:c11atomics QUESTION, ANSWERED -- this is the first Win64
			// compile of this module, which is what the open item was waiting for.
			//
			// The flag is NOT needed, and the reason matters more than the answer.
			// quickjs.c:73 guards the whole atomics feature on
			//     #if !defined(__TINYC__) && ... && !__STDC_NO_ATOMICS__ && ...
			//     #include "quickjs-c-atomics.h"
			//     #define CONFIG_ATOMICS
			//     #endif
			// and MSVC defines __STDC_NO_ATOMICS__ unless /experimental:c11atomics is
			// passed. So under MSVC the guard is false, CONFIG_ATOMICS never gets defined,
			// and quickjs.c:60921-61409 -- the _Atomic/atomic_fetch_* code that would have
			// demanded the flag -- is preprocessed away. It compiles because the feature
			// is gone, not because MSVC accepted it.
			//
			// THE CONSEQUENCE, WHICH IS A BEHAVIOURAL DIFFERENCE AND NOT A BUILD DETAIL:
			// the JavaScript `Atomics` global is absent on Win64 and present on Linux and
			// macOS, where clang leaves __STDC_NO_ATOMICS__ undefined.
			//
			// DECIDED 2026-08-03 (owner, bead akj.10.4): ACCEPT THE SPLIT, FOR NOW. We do NOT
			// pass /experimental:c11atomics. It would opt a shipped module into an explicitly
			// experimental MSVC switch to enable a builtin that no VaCuus document, test or
			// buyer-facing API uses. This comment states the decision instead of asking the
			// question, and the evidence below is why it is safe to state.
			//
			// "FOR NOW" IS THE OWNER'S WORD AND IT IS LOAD-BEARING: the decision is provisional
			// on nothing needing `Atomics`. What overturns it is a buyer, a document or a test
			// that does — at which point the flag is the answer and this comment is where the
			// reasoning already sits. What must NOT happen is the decision quietly hardening
			// into a rule because it was written down once.
			//
			// CONFIRMED AT RUNTIME, not just deduced from the preprocessor -- the check the
			// first Win64 pass could not reach, because no session survived long enough.
			// A console-session `vacuus.RefHud` build with a temporary console.log probe:
			//     ATOMICS-PROBE: typeof Atomics=undefined typeof SharedArrayBuffer=function
			//                    typeof WeakRef=function
			//
			// READ THAT SECOND FIELD BEFORE CHANGING ANYTHING HERE. `SharedArrayBuffer` is
			// PRESENT on Win64 while `Atomics` is missing -- the two do not travel together,
			// because SharedArrayBuffer is not behind the CONFIG_ATOMICS guard. So the
			// idiomatic feature test, `typeof SharedArrayBuffer !== 'undefined'`, PASSES on
			// Windows and then `Atomics.load` throws. That trap is the reason this split has
			// to be documented for buyers rather than merely accepted (docs/buyer/gotchas.md);
			// it is sharper than a plain "Win64 lacks Atomics" would be.
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
		//
		// InputCore from M4 Task 5 on, tests only: the event tests synthesize
		// gamepad/keyboard presses through the production input path, and
		// FVaCuusInputEvent's factories take FKeys (EKeys:: symbols live in
		// InputCore, which VaCuus itself links Publicly for the same reason).
		PrivateDependencyModuleNames.AddRange(new[] { "Core", "InputCore", "VaCuus", "VaCuusRml" });
	}
}
