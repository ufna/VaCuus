// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusEngine.h"
#include "VaCuusRmlDocumentHost.h"
#include "VaCuusSlateElement.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "HAL/PlatformProcess.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE PRODUCTION HOST'S SCRIPT SEAM, END TO END (M4 Task 6): a real
 * FVaCuusRmlDocumentHost on the real UI thread with the PRODUCTION script host
 * -- the factory FVaCuusJsModule::StartupModule registered -- driven by the
 * production commands a UVaCuusView would enqueue. What this pins that
 * VaCuus.Js.Documents.* cannot: the seam calls INSIDE
 * FVaCuusRmlDocumentHost::AdoptDocument and ::CloseDocument themselves (the
 * VaCuusJs suite substitutes a mirroring probe host, because the production
 * host's module depends on VaCuus and not the reverse).
 *
 * Observability is the log alone -- console.log reaches LogVaCuusJS at Display,
 * which the automation matcher captures -- because quickjs and the script-host
 * internals are deliberately unreachable from this module (the IVaCuusScriptHost
 * seam comment). That is enough for the three claims: head scripts run on load
 * against the shown body, a reload re-runs them fresh, and vacuus.onUnload
 * fires at close time.
 */
namespace VaCuusJsDocHostTest
{
static const TCHAR* GScriptedDoc = TEXT(
	"<rml><head><script>"
	"globalThis.runCount = (globalThis.runCount === undefined) ? 1 : (globalThis.runCount + 1);"
	"console.log('VaCuusJsDocHost script run ' + globalThis.runCount + ' body=' + (document.body ? document.body.id : 'none'));"
	"vacuus.onUnload = function(){ console.log('VaCuusJsDocHost unload fired'); };"
	"</script></head><body id=\"host\"><div/></body></rml>");

static const FIntPoint GViewSize(320, 240);

/** One frame per trigger; the wake event coalesces (the close-test helper verbatim). */
static bool RunFrames(FVaCuusUIThread& UIThread, int32 NumFrames)
{
	for (int32 Index = 0; Index < NumFrames; ++Index)
	{
		const uint64 Before = UIThread.GetFrameCount();
		UIThread.Trigger();
		if (!UIThread.WaitForFrameCount(Before + 1, 5.0))
		{
			return false;
		}
	}

	return true;
}
}	 // namespace VaCuusJsDocHostTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsDocHostSeamTest, "VaCuus.Render.JsDocumentSeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusJsDocHostSeamTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsDocHostTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no worker thread to drive"));
		return true;
	}

	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	FVaCuusModule& Module = FVaCuusModule::Get();
	FVaCuusUIThread* UIThread = Module.GetOrStartUIThread();
	if (!TestNotNull(TEXT("UI thread"), UIThread))
	{
		return false;
	}

	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	if (!UIThread->HasScriptHost())
	{
		// vacuus.Js.Enable was 0 at this boot (or VaCuusJs is absent): the seam
		// under test does not exist in this configuration.
		AddInfo(TEXT("Skipped: this UI thread booted without a script host"));
		return true;
	}

	// ALL expectations up front (one pattern may only be registered once), and
	// every count is EXACT -- which is what makes an ABSENCE assertable:
	//
	//  - "script run 1" exactly TWICE: the first load, and the reload re-running
	//    the scripts in a FRESH context. A context that survived the replace
	//    would log "script run 2" instead, the "run 1" count would come up one
	//    short, and the matcher fails the test -- the surviving-context bug,
	//    pinned through a count rather than an un-expectable absence.
	//  - "unload fired" exactly TWICE: once from the replace's internal close
	//    (OnDocumentClosing inside AdoptDocument's CloseDocument), once from the
	//    explicit close command -- both fired BEFORE Document->Close(), which is
	//    why they land although nothing ever pumps this view afterwards.
	AddExpectedMessagePlain(TEXT("VaCuusJsDocHost script run 1 body=host"), ELogVerbosity::Display,
		EAutomationExpectedMessageFlags::Contains, 2);
	AddExpectedMessagePlain(TEXT("VaCuusJsDocHost unload fired"), ELogVerbosity::Display,
		EAutomationExpectedMessageFlags::Contains, 2);

	const TSharedRef<FVaCuusSlateElement> Element = MakeShared<FVaCuusSlateElement>();
	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MakeUnique<FVaCuusRmlDocumentHost>(Element), GViewSize, Status);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GScriptedDoc, /*LoadSerial=*/1);
	if (!TestTrue(TEXT("UI frames ran"), RunFrames(*UIThread, 3)))
	{
		return false;
	}
	if (!TestTrue(TEXT("the document loaded"),
			Status->LoadCompletedSerial.load(std::memory_order_acquire) == 1 &&
				Status->LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded)))
	{
		return false;
	}

	// Reload = replace through AdoptDocument: the OLD context's unload fires
	// (OnDocumentClosing inside the replace's close), the context recycles, and
	// the scripts RE-RUN FRESH -- the second "run 1" line the up-front
	// expectation demands.
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GScriptedDoc, /*LoadSerial=*/2);
	if (!TestTrue(TEXT("UI frames ran"), RunFrames(*UIThread, 3)))
	{
		return false;
	}

	// Explicit close, the way UVaCuusView::Close() issues it: the second unload
	// (the first was the replace's) -- fired from CloseDocument BEFORE
	// Document->Close(), so it lands even though nothing ever pumps this view
	// again.
	UIThread->EnqueueCloseDocument(ViewId);
	if (!TestTrue(TEXT("UI frames ran"), RunFrames(*UIThread, 2)))
	{
		return false;
	}

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
