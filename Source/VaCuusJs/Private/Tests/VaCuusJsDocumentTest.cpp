// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

/*
 * M4 Task 6 -- the document seam: captured <head> scripts run host-ordered at
 * OnDocumentReady (spec 2(f)), reload recycles the context, ExecuteScript rides
 * the command queue, vacuus.onUnload fires at close time. Everything here
 * drives the PRODUCTION command path (EnqueueLoadDocumentFromMemory /
 * EnqueueExecuteScript / EnqueueCloseDocument through DrainCommands); the one
 * stand-in is FJsDocProbeHost, which mirrors FVaCuusRmlDocumentHost's
 * AdoptDocument/CloseDocument seam calls line for line because the production
 * host itself is unreachable from this module (VaCuusRender depends on VaCuus,
 * not the reverse -- the VaCuusModelTestHost.h probe argument). The production
 * host's own wiring is proven end-to-end from its home module:
 * VaCuusRender/Private/Tests/VaCuusJsDocHostTest.cpp.
 */

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "VaCuusJsDocumentTestHost.h"

#include "VaCuusContentPaths.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsDocHeadScriptsTest, "VaCuus.Js.Documents.HeadScripts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsDocReloadRecyclesTest, "VaCuus.Js.Documents.ReloadRecycles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsDocExecuteScriptTest, "VaCuus.Js.Documents.ExecuteScript",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsDocExternalScriptTest, "VaCuus.Js.Documents.ExternalScript",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsDocUnloadTest, "VaCuus.Js.Documents.Unload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsDocWatchdogSkipTest, "VaCuus.Js.Documents.WatchdogSkipsRest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace VaCuusJsDocumentTest
{
using namespace VaCuusJsDomTest;

/**
 * Head scripts against a shown body; a body <script> stays inert.
 *
 * The shown-ness half of the spec 8 row is structural -- the probe (like
 * AdoptDocument) calls Show() the line before OnDocumentReady -- and observed
 * from its consequences: the script reaches document.body (which does not even
 * exist at LoadInlineScript time, XMLNodeHandlerHead.cpp:98-110), and the
 * document reports visible on the frame after.
 */
const TCHAR* GHeadScriptsDoc = TEXT(
	"<rml><head>"
	"<script>globalThis.order = 'a'; globalThis.bodyId = (document === null) ? 'no-doc' : (document.body ? document.body.id : 'no-body');</script>"
	"<script>globalThis.order = globalThis.order + 'b';</script>"
	"</head><body id=\"root\">"
	"<div id=\"content\"/>"
	"<script>globalThis.bodyScriptRan = true;</script>"
	"</body></rml>");

/** Reload fixtures: every context mutation is observably fresh-or-stale. */
const TCHAR* GReloadDocA = TEXT(
	"<rml><head><script>"
	"globalThis.tag = 'A';"
	"globalThis.runCount = (globalThis.runCount === undefined) ? 1 : (globalThis.runCount + 1);"
	"setTimeout(function(){ globalThis.timerNote = globalThis.tag; }, 0);"
	"</script></head><body id=\"a\"/></rml>");

const TCHAR* GReloadDocB = TEXT(
	"<rml><head><script>"
	"globalThis.tag = 'B';"
	"globalThis.runCount = (globalThis.runCount === undefined) ? 1 : (globalThis.runCount + 1);"
	"setTimeout(function(){ globalThis.timerNote = globalThis.tag; }, 0);"
	"</script></head><body id=\"b\"/></rml>");

const TCHAR* GReloadDocC = TEXT(
	"<rml><head><script>globalThis.cFresh = 1;</script></head><body id=\"c\"/></rml>");

const TCHAR* GTargetDoc = TEXT(
	"<rml><head><script>globalThis.docScript = 1;</script></head>"
	"<body id=\"root\"><div id=\"target\"/></body></rml>");

const TCHAR* GUnscriptedDoc = TEXT("<rml><head/><body id=\"plain\"/></rml>");
}	 // namespace VaCuusJsDocumentTest

bool FVaCuusJsDocHeadScriptsTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsDocumentTest;

	FDomTestRig Rig;
	if (const FDomTestRig::EBoot Boot = Rig.Boot(*this); Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	FJsDocProbeHost* Probe = nullptr;
	const uint32 ViewId = AddSeamViewWithDocument(Rig, Probe, TEXT("JsDocHead"), GHeadScriptsDoc);
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}

	// Both head scripts ran, in document order, at OnDocumentReady -- against a
	// body that exists. Neither is observable at LoadInlineScript time: that
	// hook fires when </head> closes, mid-parse, body-less.
	TestEqual(TEXT("head scripts ran in document order"), Rig.Eval(ViewId, "globalThis.order"), FString(TEXT("ab")));
	TestEqual(TEXT("the first script saw the parsed, current body"), Rig.Eval(ViewId, "globalThis.bodyId"),
		FString(TEXT("root")));

	// Shown: Show() precedes OnDocumentReady (the probe mirrors AdoptDocument),
	// and the document reports visible once a frame has resolved styles.
	bool bVisible = false;
	Rig.RunOnUI([&bVisible, Probe]() { bVisible = Probe->GetDocument() != nullptr && Probe->GetDocument()->IsVisible(); });
	TestTrue(TEXT("the document the scripts ran against is shown"), bVisible);

	// The <body> script is INERT -- documented-observed (spec 3.4): RmlUi has no
	// body script handler (only <head> collects scripts,
	// XMLNodeHandlerHead.cpp:84-91, :126-130), so the element parses as a plain
	// unknown tag and its text never reaches any hook.
	TestEqual(TEXT("a body <script> never runs"), Rig.Eval(ViewId, "typeof globalThis.bodyScriptRan"),
		FString(TEXT("undefined")));

	return true;
}

/**
 * THE spec 2(f) ORDERING TEST -- reload recycles, scripts re-run fresh, the old
 * context's timer dies unfired.
 *
 * RESTORE-THE-BUG (v1 spec 12.1 -- the inverted replace ordering), run and
 * reverted on 2026-07-31: FVaCuusJsScriptHost::OnDocumentReady was temporarily
 * edited to run the captured scripts at Plugin::OnDocumentLoad's effective
 * timing -- into the context AS IT IS WHEN LoadDocument returns, i.e. the OLD
 * context on a replace (materialized on demand for a first load) -- and to skip
 * the post-recycle run, exactly the design v1 prescribed. The recycle then
 * frees everything the scripts just built, on first load and reload alike.
 * This test went red with (verbatim, VcHost.log):
 *
 *   Error: Expected 'the A->B burst left the fresh context's script run
 *     intact' to be "1", but it was "undefined".
 *   Error: Expected 'B's own timer fired in B's own context' to be "B", but it
 *     was "undefined".
 *   Error: exactly the surviving context's timer fired: the timer counter
 *     shows: The two values are not equal.
 *   Error: Expected 'the replace rebuilt a fresh world' to be "1", but it was
 *     "undefined".
 *
 * -- every global "undefined", the timer never fired, no crash, no log line of
 * its own: JS silently dead after every reload, v1's bug observed. (The
 * ExecuteScript-planted-global assertions stayed green in the red state, which
 * is itself the diagnosis: the machinery all works, only the DOCUMENT scripts
 * ran into the context the recycle frees.) The production ordering -- scripts
 * from OnDocumentReady, after the recycle -- restores every line.
 */
bool FVaCuusJsDocReloadRecyclesTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsDocumentTest;

	FDomTestRig Rig;
	if (const FDomTestRig::EBoot Boot = Rig.Boot(*this); Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	// ONE enqueue burst, ZERO pumps between: load A, replace with B. Both drain
	// in the same DrainCommands, before that frame's pump -- so A's 0 ms timer
	// is armed and then dies with A's context inside B's OnDocumentReady,
	// having never seen a pump. If any A-context callback survived the recycle,
	// the pump would fire it and timerNote would read 'A' (or the counter 2).
	FJsDocProbeHost* Probe = nullptr;
	const uint32 ViewId = AddSeamViewWithDocument(Rig, Probe, TEXT("JsDocReload"), GReloadDocA);
	Rig.Thread->EnqueueLoadDocumentFromMemory(ViewId, GReloadDocB, /*LoadSerial=*/2);
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 3)))
	{
		return false;
	}

	TestEqual(TEXT("the A->B burst left the fresh context's script run intact"),
		Rig.Eval(ViewId, "globalThis.runCount"), FString(TEXT("1")));
	TestEqual(TEXT("B's own timer fired in B's own context"), Rig.Eval(ViewId, "globalThis.timerNote"),
		FString(TEXT("B")));

	uint64 TimersFired = MAX_uint64;
	Rig.RunOnUI([&TimersFired]() { TimersFired = FWrappedDomHost::Inner->GetRuntime()->GetNumTimersFired(); });
	TestEqual(TEXT("exactly the surviving context's timer fired: the timer counter shows"), TimersFired, uint64(1));

	// A global planted through the production ExecuteScript command dies with
	// its world on the next replace -- browser-refresh semantics (spec 3.4).
	Rig.Thread->EnqueueExecuteScript(ViewId, TEXT("globalThis.mine = 'planted';"), TEXT("reload-test-plant"));
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 1)))
	{
		return false;
	}
	TestEqual(TEXT("the planted global exists before the replace"), Rig.Eval(ViewId, "globalThis.mine"),
		FString(TEXT("planted")));

	Rig.Thread->EnqueueLoadDocumentFromMemory(ViewId, GReloadDocC, /*LoadSerial=*/3);
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}
	TestEqual(TEXT("the replace rebuilt a fresh world"), Rig.Eval(ViewId, "globalThis.cFresh"), FString(TEXT("1")));
	TestEqual(TEXT("the planted global is gone with the old context"), Rig.Eval(ViewId, "typeof globalThis.mine"),
		FString(TEXT("undefined")));
	TestEqual(TEXT("B's globals are gone with B"), Rig.Eval(ViewId, "typeof globalThis.tag"),
		FString(TEXT("undefined")));

	return true;
}

bool FVaCuusJsDocExecuteScriptTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsDocumentTest;

	FDomTestRig Rig;
	if (const FDomTestRig::EBoot Boot = Rig.Boot(*this); Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	// A view with NO document: ExecuteScript is legal and `document` is null
	// (spec 3.4, the tested contract).
	FJsDocProbeHost* Probe = nullptr;
	const uint32 ViewId = AddSeamView(Rig, Probe, TEXT("JsDocExec"));
	Rig.Thread->EnqueueExecuteScript(
		ViewId, TEXT("globalThis.pre = (document === null) ? 'null-doc' : 'has-doc';"), TEXT("exec-pre"));
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}
	TestEqual(TEXT("before any document, document === null"), Rig.Eval(ViewId, "globalThis.pre"),
		FString(TEXT("null-doc")));

	// FIFO after LoadDocument, one enqueue burst: the script sees the document
	// the load in front of it produced -- the single-producer FIFO argument
	// (EVaCuusCommandKind::ExecuteScript). Note the load RECYCLES the context
	// (the `pre` global above dies with it), so `seen` landing proves both the
	// ordering and that the script ran in the post-recycle world.
	Rig.Thread->EnqueueLoadDocumentFromMemory(ViewId, GTargetDoc, /*LoadSerial=*/1);
	Rig.Thread->EnqueueExecuteScript(ViewId,
		TEXT("globalThis.seen = document.getElementById('target') ? 'found' : 'missing';"), TEXT("exec-fifo"));
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}
	TestEqual(TEXT("an ExecuteScript behind a LoadDocument sees the loaded DOM"), Rig.Eval(ViewId, "globalThis.seen"),
		FString(TEXT("found")));
	TestEqual(TEXT("the load's own head script ran first"), Rig.Eval(ViewId, "globalThis.docScript"),
		FString(TEXT("1")));

	// An UNSCRIPTED load binds the view without materializing a context (the
	// lazy rule); the first ExecuteScript afterwards materializes one and must
	// still see the DOM -- the EnsureViewContext bind path.
	FJsDocProbeHost* Probe2 = nullptr;
	const uint32 ViewId2 = AddSeamViewWithDocument(Rig, Probe2, TEXT("JsDocExec2"), GUnscriptedDoc);
	Rig.Thread->EnqueueExecuteScript(ViewId2,
		TEXT("globalThis.plain = (document === null) ? 'null-doc' : document.body.id;"), TEXT("exec-unscripted"));
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}
	TestEqual(TEXT("a context materialized by ExecuteScript after an unscripted load sees the document"),
		Rig.Eval(ViewId2, "globalThis.plain"), FString(TEXT("plain")));

	// Unknown view: the Error-level drop (the BindModel precedent,
	// VaCuusUIThread.cpp's drain) -- losing a script silently is this seam's
	// quietest failure.
	AddExpectedMessagePlain(TEXT("ExecuteScript('exec-lost') for unknown view"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);
	Rig.Thread->EnqueueExecuteScript(0xBADBEEFu, TEXT("globalThis.never = 1;"), TEXT("exec-lost"));
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 1)))
	{
		return false;
	}

	return true;
}

bool FVaCuusJsDocExternalScriptTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsDocumentTest;

	// The temp-file pattern from the live-reload tests
	// (VaCuusLiveReloadTest.cpp): plant the file in the FIRST DevUI root -- the
	// plugin's own Content/DevUI, the same root <script src> resolves through
	// -- and delete it whatever happens. The watcher cannot misfire on it even
	// in an editor session: .js is not a watched extension (Task 7 adds it).
	const TArray<FString>& Roots = VaCuusContentPaths::GetDocumentRoots();
	if (Roots.IsEmpty() || !IFileManager::Get().DirectoryExists(*Roots[0]))
	{
		AddInfo(TEXT("Skipped: no DevUI root exists on disk to plant a script in"));
		return true;
	}
	const FString ScriptPath = Roots[0] / TEXT("vacuus_js_doc_test_external.js");
	if (!TestTrue(TEXT("planted the external script"),
			FFileHelper::SaveStringToFile(TEXT("globalThis.externalRan = 42;\n"), *ScriptPath)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*ScriptPath, /*bRequireExists=*/false);
	};

	FDomTestRig Rig;
	if (const FDomTestRig::EBoot Boot = Rig.Boot(*this); Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	// External-then-inline: the inline script's read of the external's global
	// pins DOCUMENT ORDER across the two kinds (one vector, one walk --
	// ElementDocument.cpp:217-228).
	const FString ExternalDoc(TEXT(
		"<rml><head>"
		"<script src=\"vacuus_js_doc_test_external.js\"></script>"
		"<script>globalThis.afterExternal = (globalThis.externalRan === 42) ? 'yes' : 'no';</script>"
		"</head><body id=\"ext\"/></rml>"));

	FJsDocProbeHost* Probe = nullptr;
	const uint32 ViewId = AddSeamViewWithDocument(Rig, Probe, TEXT("JsDocExt"), *ExternalDoc);
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}
	TestEqual(TEXT("the external script resolved through the DevUI roots and ran"),
		Rig.Eval(ViewId, "globalThis.externalRan"), FString(TEXT("42")));
	TestEqual(TEXT("the inline script ran after it, in document order"),
		Rig.Eval(ViewId, "globalThis.afterExternal"), FString(TEXT("yes")));

	// A missing src is ONE Error naming document and path, and the LATER script
	// still runs (spec 3.4).
	AddExpectedMessagePlain(
		TEXT("<script src=\"vacuus_js_doc_missing_xyz.js\"> in vacuus://js_doc_test.rml did not resolve"),
		ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 1);
	const FString MissingDoc(TEXT(
		"<rml><head>"
		"<script src=\"vacuus_js_doc_missing_xyz.js\"></script>"
		"<script>globalThis.laterRan = 1;</script>"
		"</head><body id=\"miss\"/></rml>"));
	Rig.Thread->EnqueueLoadDocumentFromMemory(ViewId, MissingDoc, /*LoadSerial=*/2);
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}
	TestEqual(TEXT("the script after the missing one still ran"), Rig.Eval(ViewId, "globalThis.laterRan"),
		FString(TEXT("1")));

	return true;
}

bool FVaCuusJsDocUnloadTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsDocumentTest;

	FDomTestRig Rig;
	if (const FDomTestRig::EBoot Boot = Rig.Boot(*this); Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	// The callback proves its own timing: it runs at Close() time, BEFORE the
	// deferred teardown, so `document` is still current and its body reachable.
	const TCHAR* UnloadDoc = TEXT(
		"<rml><head><script>"
		"vacuus.onUnload = function(){ console.log('VaCuusJsDocTest unload saw body=' + (document.body ? document.body.id : 'gone')); };"
		"</script></head><body id=\"u\"/></rml>");

	AddExpectedMessagePlain(TEXT("VaCuusJsDocTest unload saw body=u"), ELogVerbosity::Display,
		EAutomationExpectedMessageFlags::Contains, 1);

	FJsDocProbeHost* Probe = nullptr;
	const uint32 ViewId = AddSeamViewWithDocument(Rig, Probe, TEXT("JsDocUnload"), UnloadDoc);
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}

	// The production close command; the probe's CloseDocument (like the
	// production host's) fires OnDocumentClosing before Document->Close().
	Rig.Thread->EnqueueCloseDocument(ViewId);
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}

	uint64 UnloadsRun = 0;
	Rig.RunOnUI([&UnloadsRun]() { UnloadsRun = FWrappedDomHost::Inner->GetRuntime()->GetNumUnloadCallbacksRun(); });
	TestEqual(TEXT("exactly one unload callback ran at close time"), UnloadsRun, uint64(1));

	// The context SURVIVES a plain close (only a replace recycles); its
	// `document` is the documented dead wrapper -- reads answer the dead-handle
	// shape, not a crash and not null-the-global.
	TestEqual(TEXT("after the close, document is a dead wrapper"),
		Rig.Eval(ViewId, "(document === null) ? 'null' : ((document.body === null) ? 'dead' : 'alive')"),
		FString(TEXT("dead")));

	// The graceful half: a LIVE JS document rides the in-band Shutdown drain
	// (the rig teardown's StopUIThread), whose CloseDocument loop fires
	// OnDocumentClosing while the frame loop, the contexts and the runtime are
	// all still up (spec 5) -- the unload below is dispatched from that drain.
	// The leak gate rides the same teardown: Exit destroys the runtime, whose
	// destructor checkf()s the malloc-hook live-byte counter back to zero -- a
	// leaked context or callback ref crashes the test right here.
	AddExpectedMessagePlain(TEXT("VaCuusJsDocTest shutdown unload fired"), ELogVerbosity::Display,
		EAutomationExpectedMessageFlags::Contains, 1);
	const TCHAR* ShutdownDoc = TEXT(
		"<rml><head><script>"
		"setInterval(function(){}, 3600000);"
		"vacuus.onUnload = function(){ console.log('VaCuusJsDocTest shutdown unload fired'); };"
		"</script></head><body id=\"s\"/></rml>");
	Rig.Thread->EnqueueLoadDocumentFromMemory(ViewId, ShutdownDoc, /*LoadSerial=*/2);
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}

	// Rig destruction runs StopUIThread -> RequestGracefulShutdown -> the
	// in-band drain: the expectation above is consumed there, before this
	// function returns to the framework.
	return true;
}

bool FVaCuusJsDocWatchdogSkipTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsDocumentTest;

	// A tight watchdog so the trip costs 50 ms, not the shipping budget.
	FVaCuusJsScriptHost::FParams Params;
	Params.RuntimeParams.WatchdogMs = 50;

	FDomTestRig Rig;
	if (const FDomTestRig::EBoot Boot = Rig.Boot(*this, Params); Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	// Script 1 records that it started, then spins past the deadline; scripts 2
	// and 3 must be SKIPPED with one counted Error naming how many (spec 3.3) --
	// the per-entry budget must not be burned once per remaining script.
	AddExpectedMessagePlain(TEXT("ran past its 50 ms deadline"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("JS exception in 'vacuus://js_doc_test.rml"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("skipping the remaining 2 document script(s)"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);

	const TCHAR* TrippingDoc = TEXT(
		"<rml><head>"
		"<script>globalThis.first = 1; for(;;) {}</script>"
		"<script>globalThis.second = 1;</script>"
		"<script>globalThis.third = 1;</script>"
		"</head><body id=\"w\"/></rml>");

	FJsDocProbeHost* Probe = nullptr;
	const uint32 ViewId = AddSeamViewWithDocument(Rig, Probe, TEXT("JsDocWatchdog"), TrippingDoc);
	if (!TestTrue(TEXT("UI frames ran"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}

	TestEqual(TEXT("the tripping script ran up to its spin"), Rig.Eval(ViewId, "globalThis.first"),
		FString(TEXT("1")));
	TestEqual(TEXT("the second script was skipped"), Rig.Eval(ViewId, "typeof globalThis.second"),
		FString(TEXT("undefined")));
	TestEqual(TEXT("the third script was skipped"), Rig.Eval(ViewId, "typeof globalThis.third"),
		FString(TEXT("undefined")));

	// The thread survived the trip: the next entry runs normally.
	TestEqual(TEXT("the view still evaluates after the trip"), Rig.Eval(ViewId, "1 + 1"), FString(TEXT("2")));

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
