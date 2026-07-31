// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

/*
 * M4 Task 7 -- ES modules over the VFS (spec 3.7): the `.mjs` entry convention,
 * relative resolution against the importer's directory, vfs:// stripped before
 * root resolution, import.meta.url = the canonical module name, top-level await
 * refused (E1's observed signal), module runtime throws routed through the
 * rejection tracker exactly once, missing imports yielding both diagnostics,
 * and the module cache dying with the context on reload.
 *
 * Every test drives the PRODUCTION path: temp module files planted in the first
 * DevUI root (the Task 6 pattern), documents loaded through the command queue,
 * entries run by OnDocumentReady's captured-script walk.
 */

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "VaCuusJsDocumentTestHost.h"

#include "VaCuusContentPaths.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsModuleChainTest, "VaCuus.Js.Modules.ImportChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsModuleSubdirTest, "VaCuus.Js.Modules.SubdirRelative",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsModuleTlaTest, "VaCuus.Js.Modules.TlaRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsModuleThrowTest, "VaCuus.Js.Modules.ThrowRejects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsModuleMissingTest, "VaCuus.Js.Modules.MissingImport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsModuleCacheTest, "VaCuus.Js.Modules.CacheDiesWithContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace VaCuusJsModuleTest
{
using namespace VaCuusJsDomTest;
using namespace VaCuusJsDocumentTest;

/** The first DevUI root, or empty (= skip: nowhere to plant). The Task 6 pattern's precondition. */
inline FString GetPlantRoot()
{
	const TArray<FString>& Roots = VaCuusContentPaths::GetDocumentRoots();
	if (Roots.IsEmpty() || !IFileManager::Get().DirectoryExists(*Roots[0]))
	{
		return FString();
	}
	return Roots[0];
}

/** Plants module files, deletes them (and any created subdirs) whatever happens. */
struct FPlanter
{
	TArray<FString> Files;
	TArray<FString> Dirs;

	bool Plant(FAutomationTestBase& Test, const FString& Path, const TCHAR* Content)
	{
		Files.Add(Path);
		if (!FFileHelper::SaveStringToFile(Content, *Path))
		{
			Test.AddError(FString::Printf(TEXT("could not plant '%s'"), *Path));
			return false;
		}
		return true;
	}

	bool MakeDir(FAutomationTestBase& Test, const FString& Path)
	{
		Dirs.Add(Path);
		if (!IFileManager::Get().MakeDirectory(*Path, /*Tree=*/true))
		{
			Test.AddError(FString::Printf(TEXT("could not create '%s'"), *Path));
			return false;
		}
		return true;
	}

	~FPlanter()
	{
		for (const FString& File : Files)
		{
			IFileManager::Get().Delete(*File, /*bRequireExists=*/false);
		}
		for (const FString& Dir : Dirs)
		{
			IFileManager::Get().DeleteDirectory(*Dir, /*bRequireExists=*/false, /*Tree=*/false);
		}
	}
};

/** The runtime's total-error counter, via a UI-thread closure (the Task 6 counter-read shape). */
inline uint64 ReadErrorCount(FDomTestRig& Rig)
{
	uint64 Count = MAX_uint64;
	Rig.RunOnUI(
		[&Count]()
		{
			FVaCuusJsRuntime* Runtime = FWrappedDomHost::Inner->GetRuntime();
			Count = Runtime != nullptr ? Runtime->GetNumErrors() : 0;
		});
	return Count;
}
}	 // namespace VaCuusJsModuleTest

/**
 * a.mjs -> ./b.js -> ./c.js: the chain resolves, bodies execute dependencies
 * first, import.meta.url is the canonical vfs name at every depth. a.mjs ALSO
 * imports c by its 'vfs://...' spelling: the two spellings normalize to ONE
 * cache key, so c's body runs ONCE ('cba', not 'ccba') -- pinning both the
 * vfs:// strip and the canonical-name-as-cache-key design in a single letter.
 */
bool FVaCuusJsModuleChainTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsModuleTest;

	const FString Root = GetPlantRoot();
	if (Root.IsEmpty())
	{
		AddInfo(TEXT("Skipped: no DevUI root exists on disk to plant modules in"));
		return true;
	}

	FPlanter Planter;
	if (!Planter.Plant(*this, Root / TEXT("vacuus_js_mod_c.js"),
			TEXT("globalThis.modOrder = (globalThis.modOrder || '') + 'c';\n")
			TEXT("globalThis.metaC = import.meta.url;\n")
			TEXT("export const cval = 3;\n")) ||
		!Planter.Plant(*this, Root / TEXT("vacuus_js_mod_b.js"),
			TEXT("import { cval } from './vacuus_js_mod_c.js';\n")
			TEXT("globalThis.modOrder = (globalThis.modOrder || '') + 'b';\n")
			TEXT("globalThis.metaB = import.meta.url;\n")
			TEXT("export const bval = cval + 1;\n")) ||
		!Planter.Plant(*this, Root / TEXT("vacuus_js_mod_a.mjs"),
			TEXT("import { bval } from './vacuus_js_mod_b.js';\n")
			TEXT("import { cval } from 'vfs://vacuus_js_mod_c.js';\n")
			TEXT("globalThis.modOrder = (globalThis.modOrder || '') + 'a';\n")
			TEXT("globalThis.metaA = import.meta.url;\n")
			TEXT("globalThis.chainVal = '' + bval + cval;\n")))
	{
		return false;
	}

	FDomTestRig Rig;
	if (const FDomTestRig::EBoot Boot = Rig.Boot(*this); Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	FJsDocProbeHost* Probe = nullptr;
	const uint32 ViewId = AddSeamViewWithDocument(Rig, Probe, TEXT("JsModChain"),
		TEXT("<rml><head><script src=\"vacuus_js_mod_a.mjs\"></script></head><body id=\"m\"/></rml>"));
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}

	TestEqual(TEXT("bodies executed dependencies-first, c exactly once despite two import spellings"),
		Rig.Eval(ViewId, "globalThis.modOrder"), FString(TEXT("cba")));
	TestEqual(TEXT("values flowed through the chain (and both c imports see one instance)"),
		Rig.Eval(ViewId, "globalThis.chainVal"), FString(TEXT("43")));
	TestEqual(TEXT("the entry's import.meta.url is its canonical vfs name"), Rig.Eval(ViewId, "globalThis.metaA"),
		FString(TEXT("vfs://vacuus_js_mod_a.mjs")));
	TestEqual(TEXT("an imported module's import.meta.url likewise"), Rig.Eval(ViewId, "globalThis.metaB"),
		FString(TEXT("vfs://vacuus_js_mod_b.js")));
	TestEqual(TEXT("...at every depth"), Rig.Eval(ViewId, "globalThis.metaC"), FString(TEXT("vfs://vacuus_js_mod_c.js")));
	TestEqual(TEXT("a clean chain surfaces zero errors"), ReadErrorCount(Rig), uint64(0));

	return true;
}

/**
 * Relative specifiers resolve against the IMPORTING module's directory, not the
 * VFS root -- proven with a decoy: a same-named sibling at the root exporting a
 * different value. Root-based resolution would import the decoy (a hit, not a
 * miss), so the assertion distinguishes the two resolutions, it does not just
 * detect failure.
 */
bool FVaCuusJsModuleSubdirTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsModuleTest;

	const FString Root = GetPlantRoot();
	if (Root.IsEmpty())
	{
		AddInfo(TEXT("Skipped: no DevUI root exists on disk to plant modules in"));
		return true;
	}

	const FString Subdir = Root / TEXT("vacuus_js_mod_sub");
	FPlanter Planter;
	if (!Planter.MakeDir(*this, Subdir) ||
		!Planter.Plant(*this, Root / TEXT("vacuus_js_mod_sibling.js"),
			TEXT("globalThis.decoyRan = 1;\n")
			TEXT("export const sib = 'root-decoy';\n")) ||
		!Planter.Plant(*this, Subdir / TEXT("vacuus_js_mod_sibling.js"),
			TEXT("globalThis.sibMeta = import.meta.url;\n")
			TEXT("export const sib = 'subdir-sibling';\n")) ||
		!Planter.Plant(*this, Subdir / TEXT("vacuus_js_mod_entry.mjs"),
			TEXT("import { sib } from './vacuus_js_mod_sibling.js';\n")
			TEXT("globalThis.subMeta = import.meta.url;\n")
			TEXT("globalThis.sibSeen = sib;\n")))
	{
		return false;
	}

	FDomTestRig Rig;
	if (const FDomTestRig::EBoot Boot = Rig.Boot(*this); Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	FJsDocProbeHost* Probe = nullptr;
	const uint32 ViewId = AddSeamViewWithDocument(Rig, Probe, TEXT("JsModSub"),
		TEXT("<rml><head><script src=\"vacuus_js_mod_sub/vacuus_js_mod_entry.mjs\"></script></head>")
		TEXT("<body id=\"s\"/></rml>"));
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}

	TestEqual(TEXT("'./sibling' resolved against the importer's directory, not the root"),
		Rig.Eval(ViewId, "globalThis.sibSeen"), FString(TEXT("subdir-sibling")));
	TestEqual(TEXT("the root decoy never executed"), Rig.Eval(ViewId, "typeof globalThis.decoyRan"),
		FString(TEXT("undefined")));
	TestEqual(TEXT("the subdir entry's import.meta.url carries the subdir"), Rig.Eval(ViewId, "globalThis.subMeta"),
		FString(TEXT("vfs://vacuus_js_mod_sub/vacuus_js_mod_entry.mjs")));
	TestEqual(TEXT("and so does the sibling's"), Rig.Eval(ViewId, "globalThis.sibMeta"),
		FString(TEXT("vfs://vacuus_js_mod_sub/vacuus_js_mod_sibling.js")));
	TestEqual(TEXT("zero errors"), ReadErrorCount(Rig), uint64(0));

	return true;
}

/**
 * E1 grounded, both halves in one pump: a module awaiting a host event is still
 * PENDING after the drain and is REFUSED with one counted Error naming it --
 * while a TLA-free async module (awaiting an already-settled promise) reaches
 * FULFILLED in that same drain. The refusal skips nothing: the later entry
 * still runs (only a watchdog trip skips a document's remaining scripts).
 */
bool FVaCuusJsModuleTlaTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsModuleTest;

	const FString Root = GetPlantRoot();
	if (Root.IsEmpty())
	{
		AddInfo(TEXT("Skipped: no DevUI root exists on disk to plant modules in"));
		return true;
	}

	FPlanter Planter;
	if (!Planter.Plant(*this, Root / TEXT("vacuus_js_mod_tla.mjs"),
			TEXT("globalThis.tlaReached = 1;\n")
			TEXT("await new Promise(function() {});\n")
			TEXT("globalThis.tlaAfter = 1;\n")) ||
		!Planter.Plant(*this, Root / TEXT("vacuus_js_mod_free.mjs"),
			TEXT("globalThis.freeBefore = 1;\n")
			TEXT("await Promise.resolve();\n")
			TEXT("globalThis.freeAfter = 1;\n")))
	{
		return false;
	}

	FDomTestRig Rig;
	if (const FDomTestRig::EBoot Boot = Rig.Boot(*this); Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	AddExpectedMessagePlain(TEXT("module 'vfs://vacuus_js_mod_tla.mjs' is still pending after the job drain"),
		ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 1);

	FJsDocProbeHost* Probe = nullptr;
	const uint32 ViewId = AddSeamViewWithDocument(Rig, Probe, TEXT("JsModTla"),
		TEXT("<rml><head>")
		TEXT("<script src=\"vacuus_js_mod_tla.mjs\"></script>")
		TEXT("<script src=\"vacuus_js_mod_free.mjs\"></script>")
		TEXT("</head><body id=\"t\"/></rml>"));
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}

	TestEqual(TEXT("the TLA module's body ran up to the await"), Rig.Eval(ViewId, "globalThis.tlaReached"),
		FString(TEXT("1")));
	TestEqual(TEXT("nothing after the unresolvable await ever runs"), Rig.Eval(ViewId, "typeof globalThis.tlaAfter"),
		FString(TEXT("undefined")));
	TestEqual(TEXT("a TLA-free async module fulfilled within the same pump's drain"),
		Rig.Eval(ViewId, "globalThis.freeAfter"), FString(TEXT("1")));
	TestEqual(TEXT("the refusal is ONE counted error"), ReadErrorCount(Rig), uint64(1));

	return true;
}

/**
 * A module body's runtime throw never surfaces as an eval exception -- it
 * rejects promises, and the rejection tracker is the reporter; the module-eval
 * path adds NO report of its own, so every count below is the tracker's.
 *
 * THE OBSERVED ENGINE SHAPE, pinned because Task 8's overlay must know it (this
 * test's first version expected one fire for the sync throw and went red with
 * 2 -- the "open every line you cite" find of this task): a throw BEFORE the
 * first await fires the tracker TWICE for one throw. Every module body runs as
 * an async function, and on the sync path its promise gets NO handlers -- the
 * state is read off it, never .then'd (js_execute_sync_module,
 * quickjs.c:31390-31410) -- so the body promise rejects unhandled (fire 1,
 * quickjs.c:54371-54375), and then js_evaluate_module rejects m->promise,
 * which the host holds and never handles (fire 2, quickjs.c:31573-31575).
 * A throw AFTER an await fires ONCE: the async path DOES attach handlers to
 * the body promise (js_execute_async_module, quickjs.c:31377-31381), leaving
 * only m->promise to reject unhandled. Both reasons stringify identically, so
 * a sync throw is two IDENTICAL Error lines -- an overlay that wants one entry
 * per throw must coalesce, and neither fire ever retracts (nothing can reach
 * either promise to handle it).
 */
bool FVaCuusJsModuleThrowTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsModuleTest;

	const FString Root = GetPlantRoot();
	if (Root.IsEmpty())
	{
		AddInfo(TEXT("Skipped: no DevUI root exists on disk to plant modules in"));
		return true;
	}

	FPlanter Planter;
	if (!Planter.Plant(*this, Root / TEXT("vacuus_js_mod_boom.mjs"),
			TEXT("globalThis.boomBefore = 1;\n")
			TEXT("throw new Error('sync boom');\n")) ||
		!Planter.Plant(*this, Root / TEXT("vacuus_js_mod_boom2.mjs"),
			TEXT("globalThis.boom2Before = 1;\n")
			TEXT("await Promise.resolve();\n")
			TEXT("throw new Error('async boom');\n")))
	{
		return false;
	}

	FDomTestRig Rig;
	if (const FDomTestRig::EBoot Boot = Rig.Boot(*this); Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	// Distinct reason strings on purpose: a Contains matcher on a shared prefix
	// would fold the two scenarios' counts together.
	AddExpectedMessagePlain(TEXT("Unhandled JS promise rejection: Error: sync boom"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 2);
	AddExpectedMessagePlain(TEXT("Unhandled JS promise rejection: Error: async boom"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);

	FJsDocProbeHost* Probe = nullptr;
	const uint32 ViewId = AddSeamViewWithDocument(Rig, Probe, TEXT("JsModBoom"),
		TEXT("<rml><head><script src=\"vacuus_js_mod_boom.mjs\"></script></head><body id=\"b\"/></rml>"));
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}

	TestEqual(TEXT("the body ran to the sync throw"), Rig.Eval(ViewId, "globalThis.boomBefore"), FString(TEXT("1")));
	TestEqual(TEXT("a sync module throw is TWO tracker fires (body promise + module promise), nothing more"),
		ReadErrorCount(Rig), uint64(2));

	// The post-await throw, in a fresh context (the replace recycles): its
	// rejection lands during EvalModule's drain, and the async path's body
	// promise is handled -- exactly one new fire.
	Rig.Thread->EnqueueLoadDocumentFromMemory(ViewId,
		TEXT("<rml><head><script src=\"vacuus_js_mod_boom2.mjs\"></script></head><body id=\"b2\"/></rml>"),
		/*LoadSerial=*/2);
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}

	TestEqual(TEXT("the body ran to the await"), Rig.Eval(ViewId, "globalThis.boom2Before"), FString(TEXT("1")));
	TestEqual(TEXT("a post-await throw is ONE tracker fire"), ReadErrorCount(Rig), uint64(3));

	return true;
}

/**
 * A missing import yields BOTH diagnostics (spec 3.7): the loader's own Error
 * naming what it probed, and the ReferenceError that rides the exception
 * channel to the importer's eval, logged there with the module named. One
 * counter tick -- the loader line is a log, the surfaced exception is the
 * counted event. Later document scripts still run.
 */
bool FVaCuusJsModuleMissingTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsModuleTest;

	const FString Root = GetPlantRoot();
	if (Root.IsEmpty())
	{
		AddInfo(TEXT("Skipped: no DevUI root exists on disk to plant modules in"));
		return true;
	}

	FPlanter Planter;
	if (!Planter.Plant(*this, Root / TEXT("vacuus_js_mod_miss.mjs"),
			TEXT("import './vacuus_js_mod_nope.js';\n")
			TEXT("globalThis.never = 1;\n")))
	{
		return false;
	}

	FDomTestRig Rig;
	if (const FDomTestRig::EBoot Boot = Rig.Boot(*this); Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	AddExpectedMessagePlain(
		TEXT("module 'vfs://vacuus_js_mod_nope.js' did not resolve to a readable file under the DevUI roots"),
		ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("could not load module 'vfs://vacuus_js_mod_nope.js'"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);

	FJsDocProbeHost* Probe = nullptr;
	const uint32 ViewId = AddSeamViewWithDocument(Rig, Probe, TEXT("JsModMiss"),
		TEXT("<rml><head>")
		TEXT("<script src=\"vacuus_js_mod_miss.mjs\"></script>")
		TEXT("<script>globalThis.afterMiss = 1;</script>")
		TEXT("</head><body id=\"x\"/></rml>"));
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}

	TestEqual(TEXT("the failed entry's body never ran"), Rig.Eval(ViewId, "typeof globalThis.never"),
		FString(TEXT("undefined")));
	TestEqual(TEXT("the script after the broken module still ran"), Rig.Eval(ViewId, "globalThis.afterMiss"),
		FString(TEXT("1")));
	TestEqual(TEXT("one counted error (the surfaced ReferenceError; the loader line is a log)"),
		ReadErrorCount(Rig), uint64(1));

	return true;
}

/**
 * THE CACHE-LIFETIME PIN (spec 3.4/3.7): within one context, two importers of
 * one module share one execution (the per-context cache dedupes by canonical
 * name); across a reload, the replace recycles the context and the cache dies
 * with it (ctx->loaded_modules is freed inside JS_FreeContext), so the module
 * body EXECUTES AGAIN. The console side-effect is the survivor-side observable
 * -- globals cannot testify, they die with the same context. Expected count 2 =
 * exactly once per context: a surviving cache would log 1, a broken dedupe 3+.
 */
bool FVaCuusJsModuleCacheTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsModuleTest;

	const FString Root = GetPlantRoot();
	if (Root.IsEmpty())
	{
		AddInfo(TEXT("Skipped: no DevUI root exists on disk to plant modules in"));
		return true;
	}

	FPlanter Planter;
	if (!Planter.Plant(*this, Root / TEXT("vacuus_js_mod_counter.js"),
			TEXT("console.log('VaCuusJsModCounter executed');\n")
			TEXT("export const tick = 1;\n")) ||
		!Planter.Plant(*this, Root / TEXT("vacuus_js_mod_c2.js"),
			TEXT("import { tick } from './vacuus_js_mod_counter.js';\n")
			TEXT("export const c2 = tick;\n")) ||
		!Planter.Plant(*this, Root / TEXT("vacuus_js_mod_cache_entry.mjs"),
			TEXT("import { tick } from './vacuus_js_mod_counter.js';\n")
			TEXT("import { c2 } from './vacuus_js_mod_c2.js';\n")
			TEXT("globalThis.cacheTicks = tick + c2;\n")))
	{
		return false;
	}

	FDomTestRig Rig;
	if (const FDomTestRig::EBoot Boot = Rig.Boot(*this); Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	AddExpectedMessagePlain(TEXT("VaCuusJsModCounter executed"), ELogVerbosity::Display,
		EAutomationExpectedMessageFlags::Contains, 2);

	const TCHAR* CacheDoc =
		TEXT("<rml><head><script src=\"vacuus_js_mod_cache_entry.mjs\"></script></head><body id=\"c\"/></rml>");

	FJsDocProbeHost* Probe = nullptr;
	const uint32 ViewId = AddSeamViewWithDocument(Rig, Probe, TEXT("JsModCache"), CacheDoc);
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}
	TestEqual(TEXT("two importers, one execution: both saw the same instance"),
		Rig.Eval(ViewId, "globalThis.cacheTicks"), FString(TEXT("2")));

	// The reload: same document, fresh serial -- the Task 6 replace path, which
	// recycles the context. The counter module must execute AGAIN (the expected
	// message count above enforces exactly-twice at test end).
	Rig.Thread->EnqueueLoadDocumentFromMemory(ViewId, CacheDoc, /*LoadSerial=*/2);
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}
	TestEqual(TEXT("the fresh context re-imported and re-deduped"), Rig.Eval(ViewId, "globalThis.cacheTicks"),
		FString(TEXT("2")));
	TestEqual(TEXT("zero errors across both loads"), ReadErrorCount(Rig), uint64(0));

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
