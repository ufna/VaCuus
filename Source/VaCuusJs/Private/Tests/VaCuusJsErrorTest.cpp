// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

/*
 * M4 Task 8 -- errors that show themselves, and retract when handled (spec
 * 3.8): the dev overlay (a host-owned element in the view's document, last N
 * errors, newest first), the rejection tracker's RETRACT path (is_handled=true
 * re-fire, quickjs.c:55164-55169), reason-level coalescing of the engine's
 * sync-module double-fire, the TLA refusal's overlay entry, the cvar off
 * switch, and host-owned entry survival across a document replace.
 *
 * Everything drives the PRODUCTION path: documents through the command queue
 * with the seam-calling probe host, scripts through EnqueueExecuteScript,
 * overlay state read back through the JS facade (a test's own allocation --
 * the error PATH itself never allocates JS, which is the overlay's design
 * rule, not this file's concern to prove).
 *
 * THE SHIPPING GATE IS A COMPILE-ONLY CONCERN: the overlay UI sits behind
 * #if !UE_BUILD_SHIPPING (VaCuusJsScriptHost.cpp), while the sink, entries and
 * counters stay in all builds. This suite compiles only where
 * WITH_DEV_AUTOMATION_TESTS does, so the gate is exercised by the Shipping
 * build compiling at all, not by a test here.
 */

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "VaCuusJsDocumentTestHost.h"

#include "VaCuusContentPaths.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsErrorThrowOverlayTest, "VaCuus.Js.Errors.ThrowShowsOverlay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsErrorRetractTest, "VaCuus.Js.Errors.RejectionRetracts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsErrorCoalesceTest, "VaCuus.Js.Errors.SyncModuleCoalesces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsErrorTlaTest, "VaCuus.Js.Errors.TlaRefusalShows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsErrorCvarOffTest, "VaCuus.Js.Errors.OverlayCvarOff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsErrorReplaceTest, "VaCuus.Js.Errors.EntriesSurviveReplace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace VaCuusJsErrorTest
{
using namespace VaCuusJsDomTest;
using namespace VaCuusJsDocumentTest;

/**
 * No <script>, no text nodes of its own; the overlay's OWN text is safe in this
 * bare document because the overlay names the engine's default font face when
 * the document declares none (RebuildOverlay's font-family probe).
 */
const TCHAR* GBareDoc = TEXT("<rml><head/><body id=\"root\"/></rml>");

/**
 * The whole overlay as one string: its inner markup, or '<none>' when the
 * element does not exist. The facade lookup is BY THE RESERVED ID -- the same
 * refind the host's refresh uses -- so "absent" here is exactly the state the
 * refresh left behind, not a stale pointer's opinion.
 */
const char* GReadOverlay =
	"(function(){ var o = document.getElementById('vacuus-js-overlay');"
	" return o === null ? '<none>' : o.innerRML; })()";

/**
 * The RAW read: the same lookup as GReadOverlay but straight through Rml on
 * the UI thread, no JS context required. This is the only honest probe for
 * "no overlay" on a view whose context does not exist -- an unscripted
 * document leaves the context lazy-unmaterialized (spec 3.4), and the facade
 * probe would answer '<no context>' about a tree it never got to see.
 */
inline FString ReadOverlayRaw(FDomTestRig& Rig, FJsDocProbeHost* Probe)
{
	FString Out(TEXT("<ui-timeout>"));
	Rig.RunOnUI(
		[&Out, Probe]()
		{
			Rml::ElementDocument* Doc = Probe->GetDocument();
			if (Doc == nullptr)
			{
				Out = TEXT("<no-doc>");
				return;
			}
			Rml::Element* Overlay = Doc->GetElementById("vacuus-js-overlay");
			Out = Overlay == nullptr ? TEXT("<none>") : FString(UTF8_TO_TCHAR(Overlay->GetInnerRML().c_str()));
		});
	return Out;
}

/** The runtime's total-error counter (the Task 6/7 counter-read shape). */
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

/** The module tests' planter, minimal: this file plants at most one module per test. */
struct FModulePlant
{
	FString Path;

	bool Plant(FAutomationTestBase& Test, const FString& InPath, const TCHAR* Content)
	{
		Path = InPath;
		if (!FFileHelper::SaveStringToFile(Content, *Path))
		{
			Test.AddError(FString::Printf(TEXT("could not plant '%s'"), *Path));
			return false;
		}
		return true;
	}

	~FModulePlant()
	{
		if (!Path.IsEmpty())
		{
			IFileManager::Get().Delete(*Path, /*bRequireExists=*/false);
		}
	}
};

/** The first DevUI root, or empty (= skip: nowhere to plant). */
inline FString GetPlantRoot()
{
	const TArray<FString>& Roots = VaCuusContentPaths::GetDocumentRoots();
	if (Roots.IsEmpty() || !IFileManager::Get().DirectoryExists(*Roots[0]))
	{
		return FString();
	}
	return Roots[0];
}
}	 // namespace VaCuusJsErrorTest

/**
 * An uncaught throw in ExecuteScript reaches the overlay as "source: message",
 * within the same frame (the error lands in DrainCommands, the refresh in that
 * frame's pump -- both before any Context::Update).
 */
bool FVaCuusJsErrorThrowOverlayTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsErrorTest;

	FDomTestRig Rig;
	if (const FDomTestRig::EBoot Boot = Rig.Boot(*this); Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	FJsDocProbeHost* Probe = nullptr;
	const uint32 ViewId = AddSeamViewWithDocument(Rig, Probe, TEXT("JsErrThrow"), GBareDoc);
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}

	// RAW read: the bare document materialized no context yet, so only the
	// Rml-side probe can testify (ReadOverlayRaw's comment).
	TestEqual(TEXT("no errors, no overlay"), ReadOverlayRaw(Rig, Probe), FString(TEXT("<none>")));

	AddExpectedMessagePlain(TEXT("JS exception in 'err-throw': Error: boom overlay"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);
	Rig.Thread->EnqueueExecuteScript(ViewId, TEXT("throw new Error('boom overlay');"), TEXT("err-throw"));
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}

	const FString Overlay = Rig.Eval(ViewId, GReadOverlay);
	TestTrue(TEXT("the overlay exists and carries source + message"),
		Overlay.Contains(TEXT("err-throw: Error: boom overlay")));
	TestEqual(TEXT("one counted error"), ReadErrorCount(Rig), uint64(1));

	return true;
}

/**
 * THE RETRACT PATH (spec 3.8: the overlay must not lie). A rejection with no
 * handler fires the tracker (is_handled=false, quickjs.c:54370-54375) and the
 * overlay shows it; a LATER .catch on the same promise re-fires with
 * is_handled=true (quickjs.c:55164-55169) and the entry is WITHDRAWN -- while
 * the error COUNTER stands, because it counts fired diagnostics, not
 * currently-standing ones.
 */
bool FVaCuusJsErrorRetractTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsErrorTest;

	FDomTestRig Rig;
	if (const FDomTestRig::EBoot Boot = Rig.Boot(*this); Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	FJsDocProbeHost* Probe = nullptr;
	const uint32 ViewId = AddSeamViewWithDocument(Rig, Probe, TEXT("JsErrRetract"), GBareDoc);
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}

	// Step 1: reject, keep the promise reachable, attach nothing. The tracker
	// fires synchronously inside the eval; the overlay entry lands at that
	// frame's refresh.
	AddExpectedMessagePlain(TEXT("Unhandled JS promise rejection: Error: later-handled"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);
	Rig.Thread->EnqueueExecuteScript(
		ViewId, TEXT("globalThis.p = Promise.reject(new Error('later-handled'));"), TEXT("err-reject"));
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}

	TestTrue(TEXT("the unhandled rejection shows"),
		Rig.Eval(ViewId, GReadOverlay).Contains(TEXT("promise: Error: later-handled")));
	TestEqual(TEXT("one counted error"), ReadErrorCount(Rig), uint64(1));

	// Step 2: the late handler. perform_promise_then on the already-rejected,
	// still-unhandled promise re-fires the tracker with is_handled=true
	// (quickjs.c:55164-55169; the is_handled latch at :55182 makes it at most
	// once per promise) -- the entry retracts, and the .catch callback itself
	// runs as an ordinary reaction job in the pump.
	Rig.Thread->EnqueueExecuteScript(
		ViewId, TEXT("globalThis.p.catch(function(){ globalThis.caught = 1; });"), TEXT("err-catch"));
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}

	TestEqual(TEXT("the late handler actually ran"), Rig.Eval(ViewId, "globalThis.caught"), FString(TEXT("1")));
	TestEqual(TEXT("the entry RETRACTED -- and the empty overlay is removed, not left as an empty box"),
		Rig.Eval(ViewId, GReadOverlay), FString(TEXT("<none>")));
	TestEqual(TEXT("the counter still stands: it counts fired diagnostics, not standing ones"),
		ReadErrorCount(Rig), uint64(1));

	return true;
}

/**
 * THE COALESCING RULE, against the engine's own duplication: a module throw
 * BEFORE the first await rejects TWO promises with one reason -- the body
 * promise (never .then'd by the sync path, js_execute_sync_module,
 * quickjs.c:31390-31410) and m->promise (held only by the host) -- so the
 * tracker fires twice, identically, and NEITHER can ever retract. The overlay
 * shows ONE line with a multiplicity suffix; the counter honestly says two.
 */
bool FVaCuusJsErrorCoalesceTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsErrorTest;

	const FString Root = GetPlantRoot();
	if (Root.IsEmpty())
	{
		AddInfo(TEXT("Skipped: no DevUI root exists on disk to plant modules in"));
		return true;
	}

	FModulePlant Plant;
	if (!Plant.Plant(*this, Root / TEXT("vacuus_js_err_boom.mjs"), TEXT("throw new Error('coalesce boom');\n")))
	{
		return false;
	}

	FDomTestRig Rig;
	if (const FDomTestRig::EBoot Boot = Rig.Boot(*this); Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	AddExpectedMessagePlain(TEXT("Unhandled JS promise rejection: Error: coalesce boom"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 2);

	FJsDocProbeHost* Probe = nullptr;
	const uint32 ViewId = AddSeamViewWithDocument(Rig, Probe, TEXT("JsErrCoalesce"),
		TEXT("<rml><head><script src=\"vacuus_js_err_boom.mjs\"></script></head><body id=\"c\"/></rml>"));
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}

	// One line, suffixed x2 -- counted JS-side so the assertion reads the
	// overlay exactly as a developer would.
	TestEqual(TEXT("the double-fire coalesces to ONE line with the x2 suffix"),
		Rig.Eval(ViewId,
			"(function(){ var o = document.getElementById('vacuus-js-overlay');"
			" if (o === null) return '<none>';"
			" var rml = o.innerRML;"
			" return (rml.split('coalesce boom').length - 1) + '|' + (rml.indexOf('\\u00D72') >= 0); })()"),
		FString(TEXT("1|true")));
	TestEqual(TEXT("the counter is not coalesced: two fired diagnostics"), ReadErrorCount(Rig), uint64(2));

	return true;
}

/**
 * The TLA refusal reaches the overlay through the NAMED-REFUSAL funnel
 * (ReportSurfacedRefusal), not the tracker and not ReportException -- there is
 * no exception to consume and no rejection to track, only a module promise
 * still pending after a full drain (E1's observed signal, spec 3.7).
 */
bool FVaCuusJsErrorTlaTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsErrorTest;

	const FString Root = GetPlantRoot();
	if (Root.IsEmpty())
	{
		AddInfo(TEXT("Skipped: no DevUI root exists on disk to plant modules in"));
		return true;
	}

	FModulePlant Plant;
	if (!Plant.Plant(*this, Root / TEXT("vacuus_js_err_tla.mjs"), TEXT("await new Promise(function() {});\n")))
	{
		return false;
	}

	FDomTestRig Rig;
	if (const FDomTestRig::EBoot Boot = Rig.Boot(*this); Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	AddExpectedMessagePlain(TEXT("module 'vfs://vacuus_js_err_tla.mjs' is still pending after the job drain"),
		ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 1);

	FJsDocProbeHost* Probe = nullptr;
	const uint32 ViewId = AddSeamViewWithDocument(Rig, Probe, TEXT("JsErrTla"),
		TEXT("<rml><head><script src=\"vacuus_js_err_tla.mjs\"></script></head><body id=\"t\"/></rml>"));
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}

	const FString Overlay = Rig.Eval(ViewId, GReadOverlay);
	TestTrue(TEXT("the refusal shows, named by module"),
		Overlay.Contains(TEXT("vfs://vacuus_js_err_tla.mjs: top-level await refused")));
	TestEqual(TEXT("one counted error"), ReadErrorCount(Rig), uint64(1));

	return true;
}

/**
 * vacuus.Js.Overlay 0: no element ever appears, while the log line and the
 * counter -- which live UPSTREAM of the overlay gate, in all builds -- are
 * untouched.
 */
bool FVaCuusJsErrorCvarOffTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsErrorTest;

	IConsoleVariable* OverlayCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("vacuus.Js.Overlay"));
	if (OverlayCVar == nullptr)
	{
		AddError(TEXT("vacuus.Js.Overlay does not exist"));
		return false;
	}
	const int32 SavedOverlay = OverlayCVar->GetInt();
	OverlayCVar->Set(0, ECVF_SetByConsole);
	ON_SCOPE_EXIT
	{
		OverlayCVar->Set(SavedOverlay, ECVF_SetByConsole);
	};

	FDomTestRig Rig;
	if (const FDomTestRig::EBoot Boot = Rig.Boot(*this); Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	FJsDocProbeHost* Probe = nullptr;
	const uint32 ViewId = AddSeamViewWithDocument(Rig, Probe, TEXT("JsErrCvar"), GBareDoc);
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}

	AddExpectedMessagePlain(TEXT("JS exception in 'err-cvar': Error: hidden boom"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);
	Rig.Thread->EnqueueExecuteScript(ViewId, TEXT("throw new Error('hidden boom');"), TEXT("err-cvar"));
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}

	TestEqual(TEXT("no overlay element with the cvar off"), Rig.Eval(ViewId, GReadOverlay), FString(TEXT("<none>")));
	TestEqual(TEXT("still logged (matched above) and still counted"), ReadErrorCount(Rig), uint64(1));

	return true;
}

/**
 * Entries live on the HOST per-view, deliberately NOT on the context: a
 * document replace recycles the context AND kills the overlay element with the
 * old tree, but the record survives, and the overlay rebuilds from it -- all
 * entries, newest first -- at the next error's refresh. This is the honesty
 * property across reloads: reloading a broken document does not launder its
 * history.
 */
bool FVaCuusJsErrorReplaceTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsErrorTest;

	FDomTestRig Rig;
	if (const FDomTestRig::EBoot Boot = Rig.Boot(*this); Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	FJsDocProbeHost* Probe = nullptr;
	const uint32 ViewId = AddSeamViewWithDocument(Rig, Probe, TEXT("JsErrReplace"), GBareDoc);
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}

	AddExpectedMessagePlain(TEXT("JS exception in 'err-first': Error: first-err"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);
	Rig.Thread->EnqueueExecuteScript(ViewId, TEXT("throw new Error('first-err');"), TEXT("err-first"));
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}
	TestTrue(TEXT("the first error shows"), Rig.Eval(ViewId, GReadOverlay).Contains(TEXT("first-err")));

	// The replace: the overlay element dies with the old tree; nothing rebuilds
	// it yet -- the refresh is lazy on the next arrival, not on document ready.
	Rig.Thread->EnqueueLoadDocumentFromMemory(ViewId, GBareDoc, /*LoadSerial=*/2);
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}
	// RAW read: the replace recycled the context and the unscripted new document
	// did not re-materialize one, so the facade probe cannot see the tree.
	TestEqual(TEXT("after the replace: no overlay until the next error"), ReadOverlayRaw(Rig, Probe),
		FString(TEXT("<none>")));

	AddExpectedMessagePlain(TEXT("JS exception in 'err-second': Error: second-err"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);
	Rig.Thread->EnqueueExecuteScript(ViewId, TEXT("throw new Error('second-err');"), TEXT("err-second"));
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}

	const FString Overlay = Rig.Eval(ViewId, GReadOverlay);
	TestTrue(TEXT("the PRE-replace entry survived the recycle (host-owned)"), Overlay.Contains(TEXT("first-err")));
	TestTrue(TEXT("the new entry shows too"), Overlay.Contains(TEXT("second-err")));
	TestTrue(TEXT("newest first"), Overlay.Find(TEXT("second-err")) < Overlay.Find(TEXT("first-err")));
	TestEqual(TEXT("two counted errors"), ReadErrorCount(Rig), uint64(2));

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
