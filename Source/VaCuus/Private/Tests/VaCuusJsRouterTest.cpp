// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusBoundModel.h"
#include "VaCuusDataVariable.h"
#include "VaCuusDefines.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusGameBridge.h"
#include "VaCuusScriptHost.h"
#include "VaCuusTestDocumentHost.h"
#include "VaCuusUIThread.h"
#include "VaCuusView.h"
#include "VaCuusViewStatus.h"
#include "VaCuusWriteRouter.h"

#include "VaCuusJsRouterTestTypes.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"

#include <atomic>

#include <RmlUi/Core.h>

#if WITH_DEV_AUTOMATION_TESTS

/*
 * THE WRITE ROUTER AND THE READ SURFACE (M4 Task 9, spec 3.10/3.11), end to end over
 * the PRODUCTION model path: FVaCuusBoundModel, EnqueueBindModel (which is what
 * registers the model with the router -- the M3b fixtures' direct binds deliberately
 * never do), a real document over a real Rml::Context on the real UI thread, and the
 * real game-thread drain.
 *
 * This is the router-PRESENT counterpart of VaCuus.Model.ArrayBinding's I3 phase: the
 * same alias spelling (`kill.Killer = 'hacked'` on a data-for row), the same
 * byte-compare-and-element-read assertions -- but here the writes ATTRIBUTE, so they
 * land in OnModelWrite instead of the refusal counter, and the revert-dirty snaps the
 * mutated control back. That test keeps proving the router-absent half in this same
 * binary (its models are unregistered, its refusal wording asserted verbatim).
 */
namespace VaCuusJsRouterTest
{
static const char* GModelName = "hud";

/** What the write test's document shows, read after one Context::Update(). */
struct FRouterCaptured
{
	/** The checkbox's `checked` attribute -- the control state the revert-dirty governs. */
	bool bChecked = false;

	/** The data-for rows' texts -- {{kill.Killer}} per row. */
	TArray<FString> Rows;

	/** The UI shadow's bPaused, read natively -- what the control must snap back TO. */
	bool bShadowPaused = false;
};

/**
 * The probe host: a real context running the PRODUCTION bound model (handed in before
 * AddView, bound by the BindModel command, never by this host), with test-authored
 * actions run as numbered phases -- the VaCuus.Model.ArrayBinding rig minus the
 * fixture-side bind, plus the Task 6 script-host seam calls (OnDocumentReady /
 * OnDocumentClosing), so ExecuteScript sees `document` and vacuus.view sees its size.
 *
 * THREAD HAND-OFF: configured on the test thread BEFORE EnqueueAddView, immutable
 * after; phase results are plain members written on the UI thread and read on the test
 * thread only after CompletedPhase (release) was seen (acquire).
 */
class FRouterProbeHost final : public FVaCuusTestDocumentHost
{
public:
	explicit FRouterProbeHost(const TCHAR* InContextPrefix)
		: FVaCuusTestDocumentHost(InContextPrefix, "vacuus://router_test.rml", Rml::FocusFlag::Document)
	{
	}

	//~ The spec 2(f) script-host seam, at the production AdoptDocument/CloseDocument
	//~ placements the base guarantees: ready AFTER the old close and the Show(), closing
	//~ while the outgoing document is still current. FJsDocProbeHost mirrors the same pair.
	virtual void OnDocumentAdopted() override
	{
		if (IVaCuusScriptHost* ScriptHost = FVaCuusUIThread::GetActiveScriptHost())
		{
			ScriptHost->OnDocumentReady(ViewId, RmlDocument);
		}
	}

	virtual void OnDocumentClosing() override
	{
		if (IVaCuusScriptHost* ScriptHost = FVaCuusUIThread::GetActiveScriptHost())
		{
			ScriptHost->OnDocumentClosing(ViewId);
		}
	}

	virtual void SetVisible(bool /*bVisible*/) override {}

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		const int32 Requested = RequestedPhase.load(std::memory_order_acquire);
		if (Requested > CompletedPhase.load(std::memory_order_relaxed))
		{
			if (Actions.IsValidIndex(Requested) && Actions[Requested])
			{
				Actions[Requested](*this);
			}
			Context->Update();
			Captures.SetNum(FMath::Max(Captures.Num(), Requested + 1));
			Captures[Requested] = Capture();
			CompletedPhase.store(Requested, std::memory_order_release);
		}
		else
		{
			Context->Update();
		}

		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

	//~ Helpers for phase actions. UI thread only, like the actions themselves.

	void ClickCheckbox()
	{
		check(FVaCuusUIThread::IsInUIThread());
		if (Rml::Element* Checkbox = RmlDocument != nullptr ? RmlDocument->GetElementById("chk") : nullptr)
		{
			Checkbox->Click();
		}
	}

	/** Clicks the RowIndex-th GENERATED row under "rows" (the data-for template is skipped). */
	void ClickRow(int32 RowIndex)
	{
		check(FVaCuusUIThread::IsInUIThread());

		Rml::Element* Container = RmlDocument != nullptr ? RmlDocument->GetElementById("rows") : nullptr;
		if (Container == nullptr)
		{
			return;
		}

		int32 Seen = 0;
		const int NumChildren = Container->GetNumChildren();
		for (int Index = 0; Index < NumChildren; ++Index)
		{
			Rml::Element* Child = Container->GetChild(Index);
			if (Child != nullptr && !Child->HasAttribute("data-for") && Seen++ == RowIndex)
			{
				Child->Click();
				return;
			}
		}
	}

	/** One element's InnerRML, for phase actions that probe DOM the fixed Capture() does not cover. */
	FString ReadInnerRml(const char* Id) const
	{
		check(FVaCuusUIThread::IsInUIThread());
		if (Rml::Element* Element = RmlDocument != nullptr ? RmlDocument->GetElementById(Id) : nullptr)
		{
			return FString(UTF8_TO_TCHAR(Element->GetInnerRML().c_str()));
		}
		return FString();
	}

	const FVaCuusRouterTestModel& ShadowModel() const
	{
		check(FVaCuusUIThread::IsInUIThread());
		return *static_cast<const FVaCuusRouterTestModel*>(Model->GetUIShadow().GetData());
	}

	//~ Configured on the test thread BEFORE EnqueueAddView; immutable after.

	/** The PRODUCTION bound model, shared with the test thread and the UI thread's Models map. */
	TSharedPtr<FVaCuusBoundModel> Model;

	/** Actions[N] runs at the start of phase N, before the phase's Context::Update(). */
	TArray<TUniqueFunction<void(FRouterProbeHost&)>> Actions;

	std::atomic<int32> RequestedPhase{0};
	std::atomic<int32> CompletedPhase{-1};

	//~ Post-phase observations; the class comment's hand-off rule.
	TArray<FRouterCaptured> Captures;
	int32 RefusedSetsBefore = 0;
	int32 RefusedSetsAfter = 0;
	uint64 RoutedWritesBefore = 0;
	uint64 RoutedWritesAfter = 0;
	uint64 EchoesBefore = 0;
	uint64 EchoesAfterRevert = 0;
	bool bShadowBytesIdentical = false;
	bool bKillerElementIntact = false;

private:
	FRouterCaptured Capture() const
	{
		FRouterCaptured Out;
		if (RmlDocument == nullptr)
		{
			return Out;
		}

		if (Rml::Element* Checkbox = RmlDocument->GetElementById("chk"))
		{
			Out.bChecked = Checkbox->HasAttribute("checked");
		}
		if (Rml::Element* Container = RmlDocument->GetElementById("rows"))
		{
			const int NumChildren = Container->GetNumChildren();
			for (int Index = 0; Index < NumChildren; ++Index)
			{
				Rml::Element* Child = Container->GetChild(Index);
				if (Child != nullptr && !Child->HasAttribute("data-for"))
				{
					Out.Rows.Add(FString(UTF8_TO_TCHAR(Child->GetInnerRML().c_str())));
				}
			}
		}
		if (Model.IsValid())
		{
			Out.bShadowPaused = ShadowModel().bPaused;
		}
		return Out;
	}
};

/**
 * The write test's document: a data-checked checkbox over a top-level bool (the control
 * RmlUi's default action mutates BEFORE any controller runs -- the revert-dirty's whole
 * reason), and the M3b I3 fixture's alias spelling over struct rows, which is the write
 * whose value pointer lands in HEAP memory and exercises the span walk's element branch.
 */
static const TCHAR* GWriteDocument = TEXT(R"(<rml>
<head><style>body { display: block; } div { display: block; } input { display: block; }</style></head>
<body data-model="hud">
	<input id="chk" type="checkbox" data-checked="bPaused"/>
	<div id="rows"><div data-for="kill : Killfeed" data-event-click="kill.Killer = 'hacked'">{{ kill.Killer }}</div></div>
</body>
</rml>)");

/** The read-surface test needs only the model resolved; reads never touch the DOM. */
static const TCHAR* GReadDocument = TEXT(R"(<rml>
<head><style>body { display: block; }</style></head>
<body data-model="hud"/>
</rml>)");

/** The queue test binds no model, so its document must reference none -- an unresolved `data-model` is an [Rml] ERROR. */
static const TCHAR* GBareDocument = TEXT(R"(<rml>
<head><style>body { display: block; }</style></head>
<body/>
</rml>)");

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

/** Asks the host for a phase and pumps frames until it reports the phase done. */
static bool RunPhase(FVaCuusUIThread& UIThread, FRouterProbeHost& Host, int32 Phase)
{
	Host.RequestedPhase.store(Phase, std::memory_order_release);

	const double Deadline = FPlatformTime::Seconds() + 10.0;
	while (Host.CompletedPhase.load(std::memory_order_acquire) < Phase)
	{
		if (FPlatformTime::Seconds() > Deadline || !RunFrames(UIThread, 1))
		{
			return false;
		}
	}

	return true;
}

/** The live struct every test seeds and publishes: the values the read surface must echo back. */
static void SeedLive(FVaCuusRouterTestModel& Live)
{
	Live.bPaused = false;
	Live.Health = 100.5f;
	Live.Ammo = 7;
	Live.Title = TEXT("Hello");
	Live.Tag = FName(TEXT("Alpha"));
	Live.Caption = FText::FromString(TEXT("Cap"));
	Live.Colour = EVaCuusTestColour::Green;
	// A concrete path rather than the default: what an EMPTY soft pointer stringifies
	// to is an engine detail this test must not depend on.
	Live.Icon = TSoftObjectPtr<UObject>(FSoftObjectPath(TEXT("/Game/Foo.Foo")));
	Live.Origin.X = 1.5f;
	Live.Origin.Y = 2.5f;
	Live.Numbers = {1, 4};
	Live.Killfeed.SetNum(2);
	Live.Killfeed[0].Killer = TEXT("K0");
	Live.Killfeed[1].Killer = TEXT("K1");
}
}	 // namespace VaCuusJsRouterTest

/**
 * The router core: a data-checked toggle and the I3 alias write, with the model
 * production-bound. Both writes route (counted, refusal counter still), the shadow
 * stays byte-identical, the control REVERTS on the next apply, the game-thread drain
 * delivers the exact (Model, Path, Value) tuples, and a drain with no handler warns
 * once per (model, path).
 *
 * RESTORE-THE-BUG (the revert-dirty, spec 3.10's load-bearing half): comment out the
 * GPendingReverts.AddUnique line in FVaCuusWriteRouter::TryRouteScalarSet and the
 * checkbox stays visually toggled against an unchanged shadow forever -- phase 2's
 * "the checkbox snapped back" assertion goes red (checked still true one frame after
 * the click, and every frame after). Broken and observed during Task 9; the report
 * carries the red output verbatim.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusRouterWriteTest, "VaCuus.Js.Router.WriteRoutesAndReverts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusRouterWriteTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsRouterTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no UI thread to drive"));
		return true;
	}

	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	// The zero-handler Warnings, one per (model, path): phase 1's two writes are
	// drained BEFORE any delegate binds. The counts are exact -- phase 3 repeats the
	// same writes WITH a handler, and a re-warn there would fail these at test end.
	AddExpectedMessagePlain(TEXT("routed a write to model 'hud' path 'bPaused', but nothing is bound to OnModelWrite"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("routed a write to model 'hud' path 'Killfeed[0].Killer', but nothing is bound to OnModelWrite"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

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

	// The PRODUCTION model object -- the same construction UVaCuusView::BindModel
	// performs -- bound through the BindModel command below, which is what registers it
	// with the router.
	const TSharedRef<FVaCuusBoundModel> Model =
		MakeShared<FVaCuusBoundModel>(TEXT("hud"), FVaCuusRouterTestModel::StaticStruct());
	if (!TestTrue(TEXT("the model built"), Model->IsValid()))
	{
		return false;
	}

	FVaCuusRouterTestModel Live;
	SeedLive(Live);
	Model->Sample(FVaCuusRouterTestModel::StaticStruct(), &Live);
	Model->PublishPending();

	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
	TUniquePtr<FRouterProbeHost> OwnedHost = MakeUnique<FRouterProbeHost>(TEXT("vacuus_router_view"));
	FRouterProbeHost* Host = OwnedHost.Get();
	Host->Model = Model;

	// Phase 1: THE TWO WRITES. The checkbox toggle: RmlUi's default action sets the
	// `checked` attribute (InputTypeCheckbox.cpp:43-46), the change controller's Set is
	// routed, the shadow is untouched. The row click: the I3 alias spelling, whose
	// value pointer lands inside Killfeed's element block -- the span walk's heap branch.
	Host->Actions.SetNum(4);
	Host->Actions[1] = [](FRouterProbeHost& Host)
	{
		Host.RefusedSetsBefore = VaCuusData::GetNumRefusedSets();
		Host.RoutedWritesBefore = FVaCuusWriteRouter::GetNumRoutedWrites();
		Host.EchoesBefore = FVaCuusWriteRouter::GetNumSwallowedEchoes();

		TArray<uint8> Before;
		Before.Append(static_cast<const uint8*>(Host.Model->GetUIShadow().GetData()),
			Host.Model->GetUIShadow().GetStruct()->GetStructureSize());
		const FString KillerBefore = Host.ShadowModel().Killfeed[0].Killer;

		Host.ClickCheckbox();	 // bPaused = true, via the change controller
		Host.ClickRow(0);		 // kill.Killer = 'hacked', via the data-event alias

		Host.RefusedSetsAfter = VaCuusData::GetNumRefusedSets();
		Host.RoutedWritesAfter = FVaCuusWriteRouter::GetNumRoutedWrites();
		Host.bShadowBytesIdentical =
			FMemory::Memcmp(Before.GetData(), Host.Model->GetUIShadow().GetData(), Before.Num()) == 0;
		Host.bKillerElementIntact = Host.ShadowModel().Killfeed[0].Killer.Equals(KillerBefore, ESearchCase::CaseSensitive);
	};

	// Phase 3: the same two writes again, now with a game-side handler bound -- the
	// payload assertions' half. (Phase 2 is an idle frame: the revert runs in it, and
	// its change-controller re-fire is the ECHO the counter capture below pins.)
	Host->Actions[3] = [](FRouterProbeHost& Host)
	{
		// Read BEFORE this phase's clicks: everything between phase 1's capture and
		// here is phase 2's revert -- whose checkbox re-fire must have been swallowed,
		// not routed (the router's echo rule; unswallowed it is a third OnModelWrite).
		Host.EchoesAfterRevert = FVaCuusWriteRouter::GetNumSwallowedEchoes();

		Host.ClickCheckbox();
		Host.ClickRow(0);
	};

	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), FIntPoint(400, 300), Status);
	UIThread->EnqueueBindModel(ViewId, Model);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GWriteDocument, /*LoadSerial=*/1);

	if (!TestTrue(TEXT("the initial phase ran"), RunPhase(*UIThread, *Host, 0)))
	{
		return false;
	}
	if (!TestTrue(TEXT("the document loaded"),
			Status->LoadCompletedSerial.load(std::memory_order_acquire) == 1
				&& Status->LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded)))
	{
		return false;
	}

	// ---- 1. The initial state: seeded values applied, control agreeing with the shadow. ----

	const FRouterCaptured& Initial = Host->Captures[0];
	TestFalse(TEXT("the checkbox starts unchecked"), Initial.bChecked);
	TestFalse(TEXT("agreeing with the shadow"), Initial.bShadowPaused);
	if (TestEqual(TEXT("one row per killfeed element"), Initial.Rows.Num(), 2))
	{
		TestEqual(TEXT("row 0"), Initial.Rows[0], FString(TEXT("K0")));
	}

	// ---- 2. The clicks: routed, not refused; the shadow untouched; the control mutated. ----

	if (!TestTrue(TEXT("the click phase ran"), RunPhase(*UIThread, *Host, 1)))
	{
		return false;
	}

	TestEqual(TEXT("both writes ROUTED (the legal channel, spec 3.10)"), Host->RoutedWritesAfter, Host->RoutedWritesBefore + 2);
	TestEqual(TEXT("and neither was counted as a refusal"), Host->RefusedSetsAfter, Host->RefusedSetsBefore);
	TestTrue(TEXT("the shadow's inline span is byte-identical (I3 stands)"), Host->bShadowBytesIdentical);
	TestTrue(TEXT("and the heap element the alias write aimed at is intact"), Host->bKillerElementIntact);

	const FRouterCaptured& AfterClicks = Host->Captures[1];
	TestTrue(TEXT("the control DID move -- RmlUi's default action toggled it before anyone was asked"), AfterClicks.bChecked);
	TestFalse(TEXT("while the shadow did not"), AfterClicks.bShadowPaused);
	TestTrue(TEXT("and the rows did not re-render"), AfterClicks.Rows == Initial.Rows);

	// ---- 3. THE REVERT (spec 3.10's load-bearing half): next apply, the control snaps back. ----

	if (!TestTrue(TEXT("the revert frame ran"), RunPhase(*UIThread, *Host, 2)))
	{
		return false;
	}
	const FRouterCaptured& AfterRevert = Host->Captures[2];
	TestFalse(TEXT("the checkbox snapped back to the unchanged shadow (drop the revert-dirty and THIS goes red -- ")
			  TEXT("v1's 12.6 divergence, the checkbox toggled forever)"),
		AfterRevert.bChecked);
	TestTrue(TEXT("the rows re-ran from the shadow, unchanged"), AfterRevert.Rows == Initial.Rows);

	// ---- 4. The game-thread drain, zero-handler first. ----

	UVaCuusView* GameView = NewObject<UVaCuusView>();
	GameView->InitializeView(nullptr, ViewId, Status, FIntPoint(400, 300));

	// Phase 1's two writes are in the queue; nothing is bound yet -- the two expected
	// Warnings at the top of this test are THIS drain's.
	FVaCuusWriteRouter::DrainGameThread();

	UVaCuusRouterTestListener* Listener = NewObject<UVaCuusRouterTestListener>();
	GameView->OnModelWrite.AddDynamic(Listener, &UVaCuusRouterTestListener::HandleModelWrite);

	if (!TestTrue(TEXT("the second click phase ran"), RunPhase(*UIThread, *Host, 3)))
	{
		return false;
	}
	FVaCuusWriteRouter::DrainGameThread();

	// The echo rule's observable: phase 2's revert re-fired the checkbox's change
	// controller (a programmatic RemoveAttribute dispatches change like a click,
	// InputTypeCheckbox.cpp:22-37), and that Set was SWALLOWED -- which is also why the
	// drain below sees exactly two writes and not three (the first run of this test,
	// before the rule existed, saw three: the red that forced it).
	TestEqual(TEXT("the revert's controller re-fire was swallowed as an echo"), Host->EchoesAfterRevert, Host->EchoesBefore + 1);

	if (TestEqual(TEXT("the handler saw both writes"), Listener->Writes.Num(), 2))
	{
		TestEqual(TEXT("the toggle's model"), Listener->Writes[0].Model, FName(TEXT("hud")));
		TestEqual(TEXT("the toggle's path"), Listener->Writes[0].Path, FString(TEXT("bPaused")));
		TestTrue(TEXT("the toggle's value: Bool true (the checkbox had just re-toggled ON)"),
			Listener->Writes[0].Value.Kind == EVaCuusJsValueKind::Bool && Listener->Writes[0].Value.bBool);

		TestEqual(TEXT("the alias write's model"), Listener->Writes[1].Model, FName(TEXT("hud")));
		TestEqual(TEXT("the alias write's path -- field, index and row leaf from the span walk"), Listener->Writes[1].Path,
			FString(TEXT("Killfeed[0].Killer")));
		TestTrue(TEXT("the alias write's value: String 'hacked'"),
			Listener->Writes[1].Value.Kind == EVaCuusJsValueKind::String
				&& Listener->Writes[1].Value.String == TEXT("hacked"));
	}

	GameView->Invalidate();
	UIThread->EnqueueRemoveView(ViewId);
	RunFrames(*UIThread, 1);

	return true;
}

/**
 * The vacuus.* surface (spec 3.11): the read surface's typed per-kind gets, array
 * element and `.size` length, the miss latches, the emit round-trip with a flat
 * payload, and the view/stats shapes -- all through ExecuteScript against the
 * production stack, asserted through console output and the game-thread drain.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusRouterReadSurfaceTest, "VaCuus.Js.Router.EmitAndReadSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusRouterReadSurfaceTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsRouterTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no UI thread to drive"));
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
		AddInfo(TEXT("Skipped: no script host (VaCuusJs absent or vacuus.Js.Enable 0), so there is no vacuus.* to test"));
		return true;
	}

	const TSharedRef<FVaCuusBoundModel> Model =
		MakeShared<FVaCuusBoundModel>(TEXT("hud"), FVaCuusRouterTestModel::StaticStruct());
	if (!TestTrue(TEXT("the model built"), Model->IsValid()))
	{
		return false;
	}

	FVaCuusRouterTestModel Live;
	SeedLive(Live);
	Model->Sample(FVaCuusRouterTestModel::StaticStruct(), &Live);
	Model->PublishPending();

	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
	TUniquePtr<FRouterProbeHost> OwnedHost = MakeUnique<FRouterProbeHost>(TEXT("vacuus_read_view"));
	FRouterProbeHost* Host = OwnedHost.Get();
	Host->Model = Model;

	const uint32 ViewId = UIThread->AllocateViewId();

	// EVERY EXPECTED LINE, exact: the typed per-kind reads (float/int/bool/string/
	// name/enum/text/soft-path/nested/element/size -- JSON keeps the types honest:
	// quotes mean string, bare means number/bool), the miss quartet each answering
	// null, the emit's enqueue truth, the view triple, and stats' shape.
	AddExpectedMessagePlain(TEXT("RS1 [100.5,7,false,\"Hello\",\"Alpha\",\"Green\",\"Cap\",\"/Game/Foo.Foo\",1.5,4,\"K1\",2]"),
		ELogVerbosity::Display, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("RS2 true"), ELogVerbosity::Display, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("RS3 true"), ELogVerbosity::Display, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(*FString::Printf(TEXT("RS4 %ux400x300"), ViewId), ELogVerbosity::Display,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("RS5 true true true"), ELogVerbosity::Display, EAutomationExpectedMessageFlags::Contains, 1);

	// The read-miss Warnings: ONE per (model, path) however often the script asks --
	// RS2 asks 'Nope' twice, and a count of 1 is the latch assertion.
	AddExpectedMessagePlain(TEXT("model 'hud', path 'Nope': no bound field has that path"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("no model of that name is bound to this view"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("path 'Numbers[9]': the index is out of bounds"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("path 'Numbers': a bare array has no value"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);

	UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), FIntPoint(400, 300), Status);
	UIThread->EnqueueBindModel(ViewId, Model);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GReadDocument, /*LoadSerial=*/1);

	// One frame: commands drain (view, bind, load) and the seeded publish APPLIES
	// (DataApply follows the drain in the same frame) -- so the script below, which
	// runs in the NEXT frame's drain, reads applied values.
	if (!TestTrue(TEXT("the setup frame ran"), RunFrames(*UIThread, 1)))
	{
		return false;
	}

	const FString Script(TEXT(R"(
		var m = vacuus.model('hud');
		console.log('RS1 ' + JSON.stringify([m.get('Health'), m.get('Ammo'), m.get('bPaused'), m.get('Title'),
			m.get('Tag'), m.get('Colour'), m.get('Caption'), m.get('Icon'), m.get('Origin.X'), m.get('Numbers[1]'),
			m.get('Killfeed[1].Killer'), m.get('Killfeed.size')]));
		console.log('RS2 ' + ((m.get('Nope') === null) && (m.get('Nope') === null)
			&& (vacuus.model('ghost').get('X') === null) && (m.get('Numbers[9]') === null) && (m.get('Numbers') === null)));
		console.log('RS3 ' + vacuus.emit('score', {points: 12.5, label: 'x', dead: true, nested: {a: 1}}));
		console.log('RS4 ' + vacuus.view.id + 'x' + vacuus.view.width + 'x' + vacuus.view.height);
		var s = vacuus.stats();
		console.log('RS5 ' + (typeof s.updateMs === 'number') + ' ' + (typeof s.renderMs === 'number')
			+ ' ' + (typeof s.fps === 'number'));
	)"));
	UIThread->EnqueueExecuteScript(ViewId, Script, TEXT("router_read_surface.js"));

	if (!TestTrue(TEXT("the script frame ran"), RunFrames(*UIThread, 2)))
	{
		return false;
	}

	// The emit's game half: register a game-side view, drain, and the flat payload
	// arrives -- three pairs, the nested object SKIPPED (the documented dispatchEvent
	// rule, one seat over).
	UVaCuusView* GameView = NewObject<UVaCuusView>();
	GameView->InitializeView(nullptr, ViewId, Status, FIntPoint(400, 300));
	UVaCuusRouterTestListener* Listener = NewObject<UVaCuusRouterTestListener>();
	GameView->OnJsEvent.AddDynamic(Listener, &UVaCuusRouterTestListener::HandleJsEvent);

	FVaCuusWriteRouter::DrainGameThread();

	if (TestEqual(TEXT("one event arrived"), Listener->Events.Num(), 1))
	{
		const UVaCuusRouterTestListener::FEvent& Event = Listener->Events[0];
		TestEqual(TEXT("named as emitted"), Event.Name, FName(TEXT("score")));
		if (TestEqual(TEXT("three pairs -- the nested object was skipped, not flattened"), Event.Payload.Num(), 3))
		{
			TestTrue(TEXT("points: Number 12.5"),
				Event.Payload[0].Key == TEXT("points") && Event.Payload[0].Value.Kind == EVaCuusJsValueKind::Number
					&& Event.Payload[0].Value.Number == 12.5);
			TestTrue(TEXT("label: String 'x'"),
				Event.Payload[1].Key == TEXT("label") && Event.Payload[1].Value.Kind == EVaCuusJsValueKind::String
					&& Event.Payload[1].Value.String == TEXT("x"));
			TestTrue(TEXT("dead: Bool true"),
				Event.Payload[2].Key == TEXT("dead") && Event.Payload[2].Value.Kind == EVaCuusJsValueKind::Bool
					&& Event.Payload[2].Value.bBool);
		}
	}

	GameView->Invalidate();
	UIThread->EnqueueRemoveView(ViewId);
	RunFrames(*UIThread, 1);

	return true;
}

/**
 * The queue bound (spec 3.10, the input-ring pattern): fill past QueueCapacity from a
 * UI-thread phase action, the overflow drops with the named diagnostic (once per
 * stall) and an exact drop count, and the drain then delivers exactly the capacity.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusRouterQueueBoundTest, "VaCuus.Js.Router.QueueBound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusRouterQueueBoundTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsRouterTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no UI thread to drive"));
		return true;
	}

	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	AddExpectedMessagePlain(TEXT("the game-thread queue is full"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);

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

	// Drain whatever an earlier test left queued, so the fill below starts from empty.
	FVaCuusWriteRouter::DrainGameThread();

	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
	TUniquePtr<FRouterProbeHost> OwnedHost = MakeUnique<FRouterProbeHost>(TEXT("vacuus_bound_view"));
	FRouterProbeHost* Host = OwnedHost.Get();

	constexpr int32 Overfill = 10;

	// Both phase actions, configured BEFORE EnqueueAddView (the host's immutability
	// rule). The fill runs as a phase action because the producer seat belongs to the
	// UI-frame thread (the router's queue comment); EnqueueJsEvent is the same
	// producer path a routed write uses, minus the attribution walk. The counters'
	// slots are reused as scratch -- this host runs no click phase.
	Host->Actions.SetNum(3);
	Host->Actions[1] = [](FRouterProbeHost& ProbeHost)
	{
		int32 Accepted = 0;
		for (int32 Index = 0; Index < FVaCuusWriteRouter::QueueCapacity + Overfill; ++Index)
		{
			TArray<FVaCuusJsKeyValue> Payload;
			if (VaCuusGameBridge::EnqueueJsEvent(1u, FName(TEXT("flood")), MoveTemp(Payload)))
			{
				++Accepted;
			}
		}
		ProbeHost.RoutedWritesAfter = static_cast<uint64>(Accepted);
	};
	Host->Actions[2] = [](FRouterProbeHost& ProbeHost)
	{
		TArray<FVaCuusJsKeyValue> Payload;
		ProbeHost.RoutedWritesBefore = VaCuusGameBridge::EnqueueJsEvent(1u, FName(TEXT("after")), MoveTemp(Payload)) ? 1u : 0u;
	};

	const uint32 ViewId = UIThread->AllocateViewId();
	const uint64 DroppedBefore = FVaCuusWriteRouter::GetNumDroppedItems();

	UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), FIntPoint(400, 300), Status);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GBareDocument, /*LoadSerial=*/1);

	if (!TestTrue(TEXT("the setup phase ran"), RunPhase(*UIThread, *Host, 0)))
	{
		return false;
	}
	if (!TestTrue(TEXT("the flood phase ran"), RunPhase(*UIThread, *Host, 1)))
	{
		return false;
	}

	TestEqual(TEXT("exactly the capacity was accepted"), static_cast<int32>(Host->RoutedWritesAfter),
		FVaCuusWriteRouter::QueueCapacity);
	TestEqual(TEXT("and exactly the overfill was dropped, counted"), FVaCuusWriteRouter::GetNumDroppedItems(),
		DroppedBefore + Overfill);

	// The drain empties the queue; the flood targeted a ViewId with no registered game
	// view, so every delivery is the Verbose unknown-view drop -- what matters here is
	// the COUNT, observed via the emptied queue accepting again below.
	FVaCuusWriteRouter::DrainGameThread();

	// Post-drain, the producer has room again: one more emit is accepted (proving the
	// count came back down -- the bound is a live gauge, not a ratchet).
	if (!TestTrue(TEXT("the post-drain phase ran"), RunPhase(*UIThread, *Host, 2)))
	{
		return false;
	}
	TestEqual(TEXT("the queue accepts again after the drain"), static_cast<int32>(Host->RoutedWritesBefore), 1);

	FVaCuusWriteRouter::DrainGameThread();
	UIThread->EnqueueRemoveView(ViewId);
	RunFrames(*UIThread, 1);

	return true;
}

/**
 * THE M4 IDLE ROW's MODEL HALVES, EXACT (spec 7: a JS-bearing idle document over a
 * settled window -- 0 published / 0 applied / 0 evaluated): a document that RAN a head
 * script through the production script host, over a production-bound model the game
 * keeps sampling byte-identically every frame. The M3b idle gates proved these zeros
 * for scriptless documents; this is the M4-specific claim that a live JS context under
 * the document -- pump running every frame -- does not break one of them. The JS-side
 * exact zeros of the same row (0 timers fired / 0 rAF run / 0 jobs executed) live in
 * VaCuus.Js.Cost.PumpIdle, in the VaCuusJs module where the fired-counters are
 * reachable; the two tests together are the row.
 *
 * The window's frames each carry a real Sample + PublishPending of the unchanged live
 * struct -- the vacuus.M3Demo.Freeze shape, "a game pushing its HUD struct every tick,
 * unchanged" -- not the weaker nobody-called-UpdateModel case.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsIdleExactZerosTest, "VaCuus.Js.Cost.IdleExactZeros",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusJsIdleExactZerosTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsRouterTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no UI thread to drive"));
		return true;
	}

	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	// A scripted document over the router-test model: two bound scalars (ScalarGets), a
	// data-for over the killfeed rows (ArraySizes/ArrayChilds), and a head script whose
	// DOM mark is the proof the JS side is genuinely present -- an idle test whose
	// script silently never ran would prove M3b again and call it M4.
	static const TCHAR* GIdleJsDocument = TEXT(R"(<rml>
<head><style>body { display: block; font-family: LatoLatin; font-size: 14px; } div { display: block; }</style>
<script>var m = document.getElementById('jsmark'); if (m !== null) { m.innerRML = 'js-ran'; }</script>
</head>
<body data-model="hud">
	<div id="jsmark">js-not-run</div>
	<div>{{Ammo}}</div>
	<div>{{Title}}</div>
	<div id="rows"><div data-for="kill : Killfeed">{{ kill.Killer }}</div></div>
</body>
</rml>)");

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

	const TSharedRef<FVaCuusBoundModel> Model =
		MakeShared<FVaCuusBoundModel>(TEXT("hud"), FVaCuusRouterTestModel::StaticStruct());
	if (!TestTrue(TEXT("the model built"), Model->IsValid()))
	{
		return false;
	}

	FVaCuusRouterTestModel Live;
	SeedLive(Live);
	Model->Sample(FVaCuusRouterTestModel::StaticStruct(), &Live);
	Model->PublishPending();

	// UI-thread observations, captured by the phase actions (the eval counters are
	// UI-thread-checked accessors). Captured through pointers to these locals, which is
	// safe under RunPhase's synchronous protocol: the test thread blocks until
	// CompletedPhase (release) is seen (acquire), and the locals outlive every phase.
	struct FCounterSnap
	{
		int32 ScalarGets = 0;
		int32 ArraySizes = 0;
		int32 ArrayChilds = 0;
		FString JsMark;
	};
	FCounterSnap Before, After;

	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
	TUniquePtr<FRouterProbeHost> OwnedHost = MakeUnique<FRouterProbeHost>(TEXT("vacuus_js_idle_view"));
	FRouterProbeHost* Host = OwnedHost.Get();
	Host->Model = Model;

	Host->Actions.SetNum(3);
	Host->Actions[1] = [&Before](FRouterProbeHost& InHost)
	{
		Before.ScalarGets = VaCuusData::GetNumScalarGets();
		Before.ArraySizes = VaCuusData::GetNumArraySizes();
		Before.ArrayChilds = VaCuusData::GetNumArrayChilds();
		Before.JsMark = InHost.ReadInnerRml("jsmark");
	};
	Host->Actions[2] = [&After](FRouterProbeHost& InHost)
	{
		After.ScalarGets = VaCuusData::GetNumScalarGets();
		After.ArraySizes = VaCuusData::GetNumArraySizes();
		After.ArrayChilds = VaCuusData::GetNumArrayChilds();
		After.JsMark = InHost.ReadInnerRml("jsmark");
	};

	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), FIntPoint(400, 300), Status);
	UIThread->EnqueueBindModel(ViewId, Model);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GIdleJsDocument, /*LoadSerial=*/1);

	if (!TestTrue(TEXT("the initial phase ran"), RunPhase(*UIThread, *Host, 0)))
	{
		return false;
	}
	if (!TestTrue(TEXT("the document loaded"),
			Status->LoadCompletedSerial.load(std::memory_order_acquire) == 1
				&& Status->LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded)))
	{
		return false;
	}

	// Settle: the forced first publish (I1) applies during these frames, the document
	// evaluates its bindings once, and everything that will ever allocate has.
	for (int32 Iteration = 0; Iteration < 8; ++Iteration)
	{
		Model->Sample(FVaCuusRouterTestModel::StaticStruct(), &Live);
		Model->PublishPending();
		if (!TestTrue(TEXT("settle frames ran"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
	}

	if (!TestTrue(TEXT("the window-open capture ran"), RunPhase(*UIThread, *Host, 1)))
	{
		return false;
	}
	if (!TestEqual(TEXT("the head script ran through the production host (the document IS JS-bearing)"),
			Before.JsMark, FString(TEXT("js-ran"))))
	{
		return false;
	}

	const uint64 PublishesBefore = Model->GetNumPublishes();
	const uint64 UpdatesAppliedBefore = Model->GetNumUpdatesApplied();
	const uint64 FieldsAppliedBefore = Model->GetNumFieldsApplied();

	// THE WINDOW: 200 frames, each one a full game-side push of the unchanged struct.
	for (int32 Iteration = 0; Iteration < 200; ++Iteration)
	{
		Model->Sample(FVaCuusRouterTestModel::StaticStruct(), &Live);
		Model->PublishPending();
		if (!TestTrue(TEXT("window frames ran"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
	}

	if (!TestTrue(TEXT("the window-close capture ran"), RunPhase(*UIThread, *Host, 2)))
	{
		return false;
	}

	// ---- The exact zeros. Deltas, not absolutes: the settle frames own their counts. ----
	TestEqual(TEXT("0 published across the window (the channel declined every unchanged push)"),
		int32(Model->GetNumPublishes() - PublishesBefore), 0);
	TestEqual(TEXT("0 updates applied"), int32(Model->GetNumUpdatesApplied() - UpdatesAppliedBefore), 0);
	TestEqual(TEXT("0 fields applied"), int32(Model->GetNumFieldsApplied() - FieldsAppliedBefore), 0);
	TestEqual(TEXT("0 scalar evaluations (no dirty -> DataViews touched nothing)"),
		After.ScalarGets - Before.ScalarGets, 0);
	TestEqual(TEXT("0 array-size evaluations"), After.ArraySizes - Before.ArraySizes, 0);
	TestEqual(TEXT("0 array-child evaluations"), After.ArrayChilds - Before.ArrayChilds, 0);
	TestEqual(TEXT("and the DOM mark is still the script's write (nothing re-rendered over it)"),
		After.JsMark, FString(TEXT("js-ran")));

	UE_LOG(LogVaCuus, Display,
		TEXT("VaCuus M4 cost: idle row (JS-bearing document, bound model, 200-frame settled window): ")
		TEXT("published=0 applied=0 evaluated=0 -- exact; the JS-side zeros are VaCuus.Js.Cost.PumpIdle's"));

	UIThread->EnqueueRemoveView(ViewId);
	RunFrames(*UIThread, 1);

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
