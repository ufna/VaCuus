// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusRmlCasts.h"
#include "VaCuusTestDocumentHost.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "HAL/PlatformProcess.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusRmlCastTest
{
/**
 * The akj.22 canary's probe host: loads one document and runs the two VaCuusRmlCasts.h
 * helpers against it FROM THIS MODULE -- VaCuus.so calling into VaCuusRml.so, the exact
 * cross-.so topology of the live sites in VaCuusTextInput.cpp. The point of the test is
 * the boundary, so the helper calls must not move into a module that would not cross it.
 *
 * THREAD HAND-OFF: plain members written on the UI thread, read on the test thread only
 * after WaitForFrameCount() -- the shared base's rule (VaCuusInputRoutingTest.cpp's class
 * comment spells the release/acquire argument out in full).
 */
class FCastProbeHost final : public FVaCuusTestDocumentHost
{
public:
	FCastProbeHost()
		: FVaCuusTestDocumentHost(TEXT("vacuus_cast_view"), "vacuus://cast.rml", Rml::FocusFlag::Auto)
	{
	}

	virtual void SetVisible(bool bVisible) override {}

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		Context->Update();

		if (RmlDocument)
		{
			if (Rml::Element* Field = RmlDocument->GetElementById("field"))
			{
				bFieldIsFormControl = VaCuusCastFormControl(*Field) != nullptr;
				bFieldHasSelection = VaCuusGetFormControlSelection(*Field, FieldSelectionBegin, FieldSelectionEnd);
			}
			if (Rml::Element* Plain = RmlDocument->GetElementById("plain"))
			{
				bPlainElementFound = true;
				bPlainIsFormControl = VaCuusCastFormControl(*Plain) != nullptr;
				int32 Unused1 = 0;
				int32 Unused2 = 0;
				bPlainHasSelection = VaCuusGetFormControlSelection(*Plain, Unused1, Unused2);
			}
		}

		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

	//~ Post-frame observations; see the class comment for the hand-off argument.
	bool bFieldIsFormControl = false;
	bool bFieldHasSelection = false;
	int32 FieldSelectionBegin = -1;
	int32 FieldSelectionEnd = -1;
	bool bPlainElementFound = false;
	bool bPlainIsFormControl = false;
	bool bPlainHasSelection = false;

};

/** One UI frame at a time; the wake event coalesces, so N triggers are not N frames. */
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

/** A real text input (the positive half) and a plain div (the negative half). */
static const TCHAR* GDocument = TEXT(R"(<rml>
<head>
<style>
body { display: block; width: 100%; height: 100%; }
#field { display: block; width: 150px; height: 30px; }
#plain { display: block; width: 150px; height: 30px; }
</style>
</head>
<body>
	<input id="field" type="text"/>
	<div id="plain"/>
</body>
</rml>)");
}	 // namespace VaCuusRmlCastTest

/**
 * The akj.22 canary: VaCuusRml's exported cast helpers must resolve a REAL
 * <input type="text"> when called from ANOTHER module.
 *
 * What it guards, precisely: RmlUi's custom RTTI works across .so boundaries today only
 * because the current module load order happens to unify every identity static on one
 * dlopen root (the whole story: VaCuusRmlCasts.h). The helpers make the compare
 * load-order-independent; this test is the loud failure if that mechanism ever regresses
 * -- a helper answering null here is the same event that would otherwise surface as an
 * IME caret silently parked at end-of-text (VaCuusTextInput.cpp's conversion site 1) or
 * an IME that never activates (FillTextFieldState's "not a form control" fallback).
 *
 * HONESTY NOTE, per the restore-the-bug standard: this test has not been seen to fail,
 * because failing it requires an actually-divergent load order, which no in-process test
 * can arrange (dlopen order is the harness's, not ours). The evidence that the guarded
 * mechanism is real is the M4 SIGSEGV (VaCuusScriptDocument.h) and the dlopen probe
 * recorded in docs/research/m6-api-notes/p2-sweep.md section 3.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusRmlCastTest, "VaCuus.Rml.CrossModuleCast",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusRmlCastTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusRmlCastTest;

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

	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();

	TUniquePtr<FCastProbeHost> OwnedHost = MakeUnique<FCastProbeHost>();
	FCastProbeHost* Host = OwnedHost.Get();

	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), FIntPoint(400, 300), Status);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GDocument, /*LoadSerial=*/1);

	if (!TestTrue(TEXT("UI frames ran"), RunFrames(*UIThread, 2)))
	{
		return false;
	}

	if (!TestTrue(TEXT("Document loaded"),
			Status->LoadCompletedSerial.load(std::memory_order_acquire) == 1 &&
				Status->LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded)))
	{
		return false;
	}

	// The canary itself: a cross-module cast of a real text input answers non-null. If
	// this goes red with nothing else touched, suspect the identity statics diverging
	// again -- a load-order or visibility change, not a form-control change.
	TestTrue(TEXT("VaCuusCastFormControl resolves a real <input type=\"text\"> across the module boundary"),
		Host->bFieldIsFormControl);
	TestTrue(TEXT("VaCuusGetFormControlSelection resolves the same input's selection API"), Host->bFieldHasSelection);

	// A fresh untouched field reports the collapsed caret, both offsets at 0 -- which also
	// proves the values came back through the two concrete-control casts, not a default.
	TestEqual(TEXT("Selection begin of an untouched field"), Host->FieldSelectionBegin, 0);
	TestEqual(TEXT("Selection end of an untouched field"), Host->FieldSelectionEnd, 0);

	// The negative half: a plain div must NOT satisfy either helper, or the helpers have
	// degenerated into unchecked static_casts and the canary would never catch anything.
	TestTrue(TEXT("The plain element was found"), Host->bPlainElementFound);
	TestFalse(TEXT("A plain <div> is not a form control"), Host->bPlainIsFormControl);
	TestFalse(TEXT("A plain <div> has no selection API"), Host->bPlainHasSelection);

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
