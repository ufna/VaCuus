// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusBoundModel.h"
#include "VaCuusDataVariable.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusNodeCount.h"
#include "VaCuusScriptHost.h"
#include "VaCuusStats.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "VaCuusCountingMalloc.h"
#include "VaCuusRefHudTestTypes.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"

#include <atomic>

#include <RmlUi/Core.h>

#if WITH_DEV_AUTOMATION_TESTS

/*
 * THE REFERENCE HUD's THREE OBSERVABLES (M6 Task 4), through the real pipeline:
 * the REAL RefHud/refhud.rml loaded from FILE over the real VFS (link + script
 * resolution included), the production script host if one is registered, the
 * production bound-model path -- the M3b rig discipline (VaCuusDataForTest.cpp)
 * plus the router test's phase mechanism for on-UI-thread observations.
 *
 *  - Exp-REF-COUNT: the steady-state recursive node count, by the stated method
 *    (VaCuusNodeCount.h), asserted in [1,650, 1,850] -- and asserted STABLE,
 *    because "steady state" is a claim about the future, not one frame.
 *  - The dirty-scope proof: the two-array scoreboard topology, by EXACT eval-
 *    counter deltas (spec 2(h)) -- one panel's bump must cost that panel's
 *    bindings and not one evaluation more.
 *  - Exp-BLIP-DRIVER: the 64-blip rAF run BOTH ways (single transform write vs
 *    left+top) for ~500 frames each, numbers recorded -- the experiment behind
 *    refhud_logic.js's choice of idiom.
 */
namespace VaCuusRefHudTest
{
static const FString GModelName(TEXT("refhud"));

/** Everything one recorded frame showed; the VaCuusDataForTest FFrameRecord shape + the cost bracket. */
struct FRefHudFrameRecord
{
	uint64 FieldsApplied = 0;

	//~ The spec 3.5 evaluation counters, absolute; per-frame deltas between records.
	int32 ScalarGets = 0;
	int32 ArraySizes = 0;
	int32 ArrayChilds = 0;

	/** This frame's Context::Update() bracket, ms (the JsCost host's instrument). */
	double UpdateMs = 0.0;

	/** This frame's JsPump sample -- the pump ran earlier in the same RunFrame. */
	double PumpMs = 0.0;
};

/**
 * File-loading probe host: FDataForProbeHost's frame log + the JsDoc seam calls
 * (OnDocumentReady after Show, OnDocumentClosing before Close -- refhud_logic.js
 * must actually run) + the router test's numbered-phase actions, which run at the
 * top of RecordAndPublishFrame -- i.e. against the tree exactly as the PREVIOUS
 * frame's Update left it.
 */
class FRefHudProbeHost final : public IVaCuusDocumentHost
{
public:
	explicit FRefHudProbeHost(const TCHAR* InContextPrefix)
		: ContextPrefix(InContextPrefix)
	{
	}

	virtual bool Initialize(uint32 InViewId, const TSharedRef<FVaCuusViewStatus>& InStatus) override
	{
		check(FVaCuusUIThread::IsInUIThread());
		ViewId = InViewId;
		Status = InStatus;
		ContextName = FString::Printf(TEXT("%s_%u"), *ContextPrefix, InViewId);
		Context = Rml::CreateContext(Rml::String(TCHAR_TO_UTF8(*ContextName)), Rml::Vector2i(1, 1));

		// Reserved once, never reallocated (the M3b straggler rule): the test thread
		// reads settled records while a coalesced trigger may append one more.
		FrameLog.Reserve(8192);
		return Context != nullptr;
	}

	virtual void Shutdown() override
	{
		check(FVaCuusUIThread::IsInUIThread());
		CloseDocument();
		if (Context != nullptr)
		{
			Rml::RemoveContext(Rml::String(TCHAR_TO_UTF8(*ContextName)));
			Context = nullptr;
		}
	}

	virtual void SetViewSize(FIntPoint InViewSize) override
	{
		check(FVaCuusUIThread::IsInUIThread());
		ViewSize = InViewSize;
		if (Context != nullptr)
		{
			Context->SetDimensions(Rml::Vector2i(ViewSize.X, ViewSize.Y));
		}
	}

	virtual void LoadDocumentFromFile(const FString& VfsPath, uint64 LoadSerial) override
	{
		check(FVaCuusUIThread::IsInUIThread());
		if (Context == nullptr)
		{
			Report(LoadSerial, /*bSuccess=*/false);
			return;
		}

		// The production FVaCuusRmlDocumentHost::LoadDocumentFromFile shape: through
		// Rml::GetFileInterface() -- links and rcss resolve against the DevUI roots /
		// mounted bundles exactly as they do in the shipping path. Load FIRST, close
		// second (the AdoptDocument order), then the spec 2(f) seam call that runs
		// the document's captured scripts.
		Rml::ElementDocument* NewDocument = Context->LoadDocument(Rml::String(TCHAR_TO_UTF8(*VfsPath)));
		if (NewDocument == nullptr)
		{
			Report(LoadSerial, /*bSuccess=*/false);
			return;
		}

		CloseDocument();
		RmlDocument = NewDocument;
		RmlDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document);
		if (IVaCuusScriptHost* ScriptHost = FVaCuusUIThread::GetActiveScriptHost())
		{
			ScriptHost->OnDocumentReady(ViewId, RmlDocument);
		}
		Report(LoadSerial, /*bSuccess=*/true);
	}

	virtual void LoadDocumentFromMemory(const FString& RmlSource, uint64 LoadSerial) override
	{
		check(FVaCuusUIThread::IsInUIThread());
		if (Context == nullptr)
		{
			Report(LoadSerial, /*bSuccess=*/false);
			return;
		}

		Rml::ElementDocument* NewDocument =
			Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(*RmlSource)), "vacuus://refhud_test.rml");
		if (NewDocument == nullptr)
		{
			Report(LoadSerial, /*bSuccess=*/false);
			return;
		}

		CloseDocument();
		RmlDocument = NewDocument;
		RmlDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document);
		if (IVaCuusScriptHost* ScriptHost = FVaCuusUIThread::GetActiveScriptHost())
		{
			ScriptHost->OnDocumentReady(ViewId, RmlDocument);
		}
		Report(LoadSerial, /*bSuccess=*/true);
	}

	virtual void CloseDocument() override
	{
		check(FVaCuusUIThread::IsInUIThread());
		if (RmlDocument != nullptr)
		{
			if (IVaCuusScriptHost* ScriptHost = FVaCuusUIThread::GetActiveScriptHost())
			{
				ScriptHost->OnDocumentClosing(ViewId);
			}
			RmlDocument->Close();
			RmlDocument = nullptr;
		}
	}

	virtual void SetVisible(bool /*bVisible*/) override {}

	virtual bool HasView() const override
	{
		check(FVaCuusUIThread::IsInUIThread());
		return Context != nullptr && RmlDocument != nullptr && ViewSize.X > 0 && ViewSize.Y > 0;
	}

	virtual Rml::Context* GetContext() const override
	{
		check(FVaCuusUIThread::IsInUIThread());
		return Context;
	}

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		// Phase actions first, the router-test placement: the action sees the tree as
		// the previous frame's Update left it -- for a settled document, THE steady
		// state.
		const int32 Requested = RequestedPhase.load(std::memory_order_acquire);
		if (Requested > CompletedPhase.load(std::memory_order_relaxed))
		{
			if (Actions.IsValidIndex(Requested) && Actions[Requested])
			{
				Actions[Requested](*this);
			}
			CompletedPhase.store(Requested, std::memory_order_release);
		}

		const double BeforeUpdate = FPlatformTime::Seconds();
		Context->Update();
		const double AfterUpdate = FPlatformTime::Seconds();

		FRefHudFrameRecord& Frame = FrameLog.AddDefaulted_GetRef();
		Frame.FieldsApplied = ObservedModel.IsValid() ? ObservedModel->GetNumFieldsApplied() : 0;
		Frame.ScalarGets = VaCuusData::GetNumScalarGets();
		Frame.ArraySizes = VaCuusData::GetNumArraySizes();
		Frame.ArrayChilds = VaCuusData::GetNumArrayChilds();
		Frame.UpdateMs = (AfterUpdate - BeforeUpdate) * 1000.0;
		Frame.PumpMs = FVaCuusPerfLog::GetLastSampleMs(FVaCuusPerfLog::JsPump);

		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

	/** UI-thread only (phase actions). */
	Rml::ElementDocument* GetDocument() const
	{
		check(FVaCuusUIThread::IsInUIThread());
		return RmlDocument;
	}

	//~ Configured on the test thread BEFORE EnqueueAddView; immutable after.
	TSharedPtr<FVaCuusBoundModel> ObservedModel;
	TArray<TUniqueFunction<void(FRefHudProbeHost&)>> Actions;

	std::atomic<int32> RequestedPhase{0};
	std::atomic<int32> CompletedPhase{-1};

	//~ Post-frame observations; read below SettledFrames() only.
	TArray<FRefHudFrameRecord> FrameLog;

	//~ Phase-action results; read after the phase's CompletedPhase was acquired.
	int32 PhaseNodeCount[4] = {0, 0, 0, 0};
	FString PhaseProbeValue[4];

private:
	void Report(uint64 LoadSerial, bool bSuccess)
	{
		if (Status.IsValid() && LoadSerial != 0)
		{
			Status->LoadResult.store(
				static_cast<uint8>(bSuccess ? EVaCuusLoadResult::Succeeded : EVaCuusLoadResult::Failed),
				std::memory_order_relaxed);
			Status->LoadCompletedSerial.store(LoadSerial, std::memory_order_release);
		}
	}

	TSharedPtr<FVaCuusViewStatus> Status;
	FString ContextPrefix;
	FString ContextName;
	uint32 ViewId = 0;
	Rml::Context* Context = nullptr;
	Rml::ElementDocument* RmlDocument = nullptr;
	FIntPoint ViewSize = FIntPoint::ZeroValue;
};

/** One UI frame at a time; the wake event coalesces (the M3b idiom, verbatim). */
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

/** Records the test thread may read; the M3b SettledFrames straggler rule. */
static int32 SettledFrames(const FVaCuusViewStatus& Status)
{
	return int32(Status.FramesRecorded.load(std::memory_order_acquire));
}

/** Asks for a phase and pumps frames until the host reports it done (router-test shape). */
static bool RunPhase(FVaCuusUIThread& UIThread, FRefHudProbeHost& Host, int32 Phase)
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

/** The one frame in [FirstFrame, EndFrame) whose fields-applied moved; the DataForTest helper verbatim. */
static int32 FindSingleApplyFrame(const TArray<FRefHudFrameRecord>& Log, int32 FirstFrame, int32 EndFrame, uint64 Before)
{
	int32 Found = INDEX_NONE;
	uint64 Prev = Before;
	for (int32 Index = FirstFrame; Index < EndFrame; ++Index)
	{
		if (Log[Index].FieldsApplied != Prev)
		{
			if (Found != INDEX_NONE)
			{
				return INDEX_NONE;
			}
			Found = Index;
		}
		Prev = Log[Index].FieldsApplied;
	}
	return Found;
}

/** Serial-deterministic panel seed -- the driver's shape, test-local values. */
static void SeedPanel(TArray<FVaCuusRefHudTestRow>& Rows, int32 PanelSeed)
{
	Rows.Reset();
	for (int32 Index = 0; Index < 24; ++Index)
	{
		FVaCuusRefHudTestRow& Row = Rows.AddDefaulted_GetRef();
		Row.Rank = Index + 1;
		Row.Name = FString::Printf(TEXT("T%d-OP-%02d"), PanelSeed, Index + 1);
		Row.Kills = (37 * (Index + PanelSeed)) % 40;
		Row.Deaths = (23 * Index + PanelSeed) % 30;
		Row.Assists = (11 * Index) % 15;
		Row.Score = 2500 - Index * 85;
		Row.Ping = 20 + (Index * 7) % 90;
	}
}

static void SeedModel(FVaCuusRefHudTestModel& Live)
{
	SeedPanel(Live.TeamAlpha, 0);
	SeedPanel(Live.TeamBravo, 1);
	Live.Health = 76.f;
	Live.Mana = 42.f;
	Live.Ammo = 30;
	Live.AmmoReserve = 120;
	Live.Level = 17;
	Live.PlayerName = TEXT("REFHUD-TEST");
	Live.Objective = TEXT("HOLD THE LINE // TEST");
}

/** The three eval-counter deltas of one apply frame, compared field-wise below. */
struct FEvalDelta
{
	int32 Gets = 0;
	int32 Sizes = 0;
	int32 Childs = 0;

	bool operator==(const FEvalDelta& Other) const
	{
		return Gets == Other.Gets && Sizes == Other.Sizes && Childs == Other.Childs;
	}

	FEvalDelta operator+(const FEvalDelta& Other) const
	{
		return FEvalDelta{Gets + Other.Gets, Sizes + Other.Sizes, Childs + Other.Childs};
	}

	FString ToString() const
	{
		return FString::Printf(TEXT("gets=%d sizes=%d childs=%d"), Gets, Sizes, Childs);
	}
};
}	 // namespace VaCuusRefHudTest

/**
 * Exp-REF-COUNT (spec M6 2(g)): the REAL RefHud/refhud.rml at declared steady
 * state counts into [1,650, 1,850] by the stated method -- and holds that count
 * over further live sim (killfeed churn nets zero, the damage pool recycles,
 * blips only move). The warm-up is serial-deterministic BY CONSTRUCTION:
 * refhud_logic.js seeds the saturated killfeed history and full damage pool at
 * document-ready (the boot IS the warm-up -- ~78 sim-seconds of feed applied at
 * once), and the C++ side's panels arrive through the real model pipeline before
 * the first frame. The published arithmetic in refhud.rml's comment block sums
 * to 1,732; this is the assertion that keeps that table honest.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusRefHudCountTest, "VaCuus.RefHud.Count",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusRefHudCountTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusRefHudTest;

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
		// A third of the composition is JS-built (64 blips, 52 killfeed rows, the
		// 24-slot pool); without a script host the count would measure a different
		// document. The router-test precedent: skip with a name, never a false red.
		AddInfo(TEXT("Skipped: no script host (VaCuusJs absent or vacuus.Js.Enable 0), so the JS-built third cannot exist"));
		return true;
	}

	const UScriptStruct* Type = FVaCuusRefHudTestModel::StaticStruct();
	const TSharedRef<FVaCuusBoundModel> Model = MakeShared<FVaCuusBoundModel>(GModelName, Type);
	if (!TestTrue(TEXT("the model built"), Model->IsValid()))
	{
		return false;
	}

	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
	TUniquePtr<FRefHudProbeHost> OwnedHost = MakeUnique<FRefHudProbeHost>(TEXT("vacuus_refhud_count"));
	FRefHudProbeHost* Host = OwnedHost.Get();
	Host->ObservedModel = Model;

	// Phase 1 and 2: the steady-state count, twice, some sim apart.
	Host->Actions.SetNum(3);
	Host->Actions[1] = [](FRefHudProbeHost& Self)
	{ Self.PhaseNodeCount[1] = VaCuusNodeCount::CountNodes(Self.GetDocument()); };
	Host->Actions[2] = [](FRefHudProbeHost& Self)
	{ Self.PhaseNodeCount[2] = VaCuusNodeCount::CountNodes(Self.GetDocument()); };

	// Bind before load (the Element::SetParent contract); the panels are published
	// before the first UI frame, so the data-for clones instantiate at the document's
	// very first Update -- steady state from frame one, JS half included.
	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), FIntPoint(1280, 720), Status);
	UIThread->EnqueueBindModel(ViewId, Model);
	UIThread->EnqueueLoadDocumentFile(ViewId, TEXT("RefHud/refhud.rml"), /*LoadSerial=*/1);

	FVaCuusRefHudTestModel Live;
	SeedModel(Live);
	TestTrue(TEXT("the first sample marks fields"), Model->Sample(Type, &Live) > 0);
	TestTrue(TEXT("...and publishes"), Model->PublishPending());

	// Pump until the load reports; a parse failure in the real document is a red here.
	bool bLoaded = false;
	for (int32 Attempt = 0; Attempt < 20 && !bLoaded; ++Attempt)
	{
		if (!TestTrue(TEXT("frames ran"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		bLoaded = Status->LoadCompletedSerial.load(std::memory_order_acquire) == 1;
	}
	if (!TestTrue(TEXT("RefHud/refhud.rml loaded through the VFS"),
			bLoaded && Status->LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded)))
	{
		return false;
	}

	// A few settle frames (the apply frame + convergence), then the count.
	if (!TestTrue(TEXT("settle frames ran"), RunFrames(*UIThread, 5)))
	{
		return false;
	}
	if (!TestTrue(TEXT("count phase 1 ran"), RunPhase(*UIThread, *Host, 1)))
	{
		return false;
	}

	const int32 Count1 = Host->PhaseNodeCount[1];
	AddInfo(FString::Printf(TEXT("Exp-REF-COUNT: %d nodes at steady state (window [1650, 1850]; published arithmetic 1732)"), Count1));
	UE_LOG(LogVaCuus, Display, TEXT("Exp-REF-COUNT: %d nodes at steady state"), Count1);
	TestTrue(TEXT("the steady-state count is in [1650, 1850]"), Count1 >= 1650 && Count1 <= 1850);

	// 40 more frames of live sim -- blips move, churn beats may land -- then the
	// SAME count: steady state is a property, not a snapshot.
	if (!TestTrue(TEXT("sim frames ran"), RunFrames(*UIThread, 40)))
	{
		return false;
	}
	if (!TestTrue(TEXT("count phase 2 ran"), RunPhase(*UIThread, *Host, 2)))
	{
		return false;
	}
	TestEqual(TEXT("the count HELD across live sim"), Host->PhaseNodeCount[2], Count1);

	UIThread->EnqueueRemoveView(ViewId);
	RunFrames(*UIThread, 1);
	return true;
}

/**
 * The two-array dirty-scope proof (spec M6 2(h)), on the REAL document by EXACT
 * counter deltas: bumping one panel's array re-evaluates exactly that panel's
 * bindings; the two panels cost the same (identical shapes); a both-panel bump
 * costs exactly the sum; a plate scalar touches no array evaluation at all. If
 * the scoreboard were ONE 48-row array -- the design spec 2(h) rejects -- the
 * one-panel delta would EQUAL the both-panel delta, and the equal-split
 * assertions here would fail: this test is the topology's teeth, not a
 * demonstration.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusRefHudDirtyScopeTest, "VaCuus.RefHud.DirtyScope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusRefHudDirtyScopeTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusRefHudTest;

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

	const UScriptStruct* Type = FVaCuusRefHudTestModel::StaticStruct();
	const TSharedRef<FVaCuusBoundModel> Model = MakeShared<FVaCuusBoundModel>(GModelName, Type);
	if (!TestTrue(TEXT("the model built"), Model->IsValid()))
	{
		return false;
	}

	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
	TUniquePtr<FRefHudProbeHost> OwnedHost = MakeUnique<FRefHudProbeHost>(TEXT("vacuus_refhud_scope"));
	FRefHudProbeHost* Host = OwnedHost.Get();
	Host->ObservedModel = Model;

	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), FIntPoint(1280, 720), Status);
	UIThread->EnqueueBindModel(ViewId, Model);
	UIThread->EnqueueLoadDocumentFile(ViewId, TEXT("RefHud/refhud.rml"), /*LoadSerial=*/1);

	FVaCuusRefHudTestModel Live;
	SeedModel(Live);
	TestTrue(TEXT("the first sample marks fields"), Model->Sample(Type, &Live) > 0);
	TestTrue(TEXT("...and publishes"), Model->PublishPending());
	if (!TestTrue(TEXT("boot frames ran"), RunFrames(*UIThread, 5)))
	{
		return false;
	}
	if (!TestTrue(TEXT("RefHud/refhud.rml loaded through the VFS"),
			Status->LoadCompletedSerial.load(std::memory_order_acquire) == 1
				&& Status->LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded)))
	{
		return false;
	}

	// Settle: the M3b stable-frames loop over fields-applied AND all three eval
	// counters. This is also the JS-noise proof the deltas below lean on: with
	// refhud_logic.js pumping blips every frame, ten straight stable frames say the
	// JS surfaces touch NO evaluation counter -- so an apply frame's delta below is
	// attributable to the bump and nothing else.
	int32 SettleFrames = 0;
	int32 StableFrames = 0;
	while (SettleFrames < 200 && StableFrames < 10)
	{
		TestEqual(TEXT("an unchanged struct marks nothing"), Model->Sample(Type, &Live), 0);
		TestFalse(TEXT("so there is nothing to publish"), Model->PublishPending());
		if (!TestTrue(TEXT("settle frames ran"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		++SettleFrames;

		const int32 Settled = SettledFrames(*Status);
		if (Settled < 2)
		{
			continue;
		}
		const FRefHudFrameRecord& Now = Host->FrameLog[Settled - 1];
		const FRefHudFrameRecord& Prev = Host->FrameLog[Settled - 2];
		const bool bStable = Now.FieldsApplied == Prev.FieldsApplied && Now.ScalarGets == Prev.ScalarGets
			&& Now.ArraySizes == Prev.ArraySizes && Now.ArrayChilds == Prev.ArrayChilds;
		StableFrames = bStable ? StableFrames + 1 : 0;
	}
	if (!TestTrue(TEXT("the view reached a steady state"), StableFrames >= 10))
	{
		return false;
	}

	// The still-frame Update baseline for the ms half below: median of the last 10
	// settled frames (blips + keyframes animating, no model change) — the same frames
	// the counter-stability loop just certified.
	double StillUpdateMedianMs = 0.0;
	{
		TArray<double> StillUpdates;
		const int32 SettledNow = SettledFrames(*Status);
		for (int32 Index = FMath::Max(0, SettledNow - 10); Index < SettledNow; ++Index)
		{
			StillUpdates.Add(Host->FrameLog[Index].UpdateMs);
		}
		StillUpdates.Sort();
		StillUpdateMedianMs = StillUpdates.Num() > 0 ? StillUpdates[StillUpdates.Num() / 2] : 0.0;
	}

	// One bump -> the apply frame's exact eval delta (counters are sampled per frame
	// AFTER Update, so the apply frame's record minus its predecessor is exactly what
	// the dirty evaluation cost). OutBumpUpdateMs is the SOAK-HALF of Exp-REF-SCALE
	// (M6 Task 5): the apply frame's whole Context::Update() bracket, read against
	// StillUpdateMedianMs — the controlled venue for the ~0.53 µs/binding law's
	// prediction (192 bindings ⇒ ~0.10 ms), which the field soak cannot resolve
	// because a 2-second beat is 2-3 frames per 1,000 and vanishes above p99 into
	// the animation tail.
	const auto BumpAndMeasure = [&](const TCHAR* What, int32 ExpectedMarkedFields,
							  TFunctionRef<void(FVaCuusRefHudTestModel&)> Mutate, FEvalDelta& OutDelta,
							  double* OutBumpUpdateMs = nullptr) -> bool
	{
		const int32 StepStart = SettledFrames(*Status);
		const uint64 Before = Host->FrameLog[StepStart - 1].FieldsApplied;

		Mutate(Live);
		if (!TestEqual(FString::Printf(TEXT("%s: marked fields"), What), Model->Sample(Type, &Live), ExpectedMarkedFields))
		{
			return false;
		}
		if (!TestTrue(FString::Printf(TEXT("%s: published"), What), Model->PublishPending()))
		{
			return false;
		}
		if (!TestTrue(FString::Printf(TEXT("%s: frames ran"), What), RunFrames(*UIThread, 3)))
		{
			return false;
		}

		const int32 ApplyFrame = FindSingleApplyFrame(Host->FrameLog, StepStart, SettledFrames(*Status), Before);
		if (!TestTrue(FString::Printf(TEXT("%s: exactly one apply"), What), ApplyFrame != INDEX_NONE && ApplyFrame >= 1))
		{
			return false;
		}

		const FRefHudFrameRecord& Applied = Host->FrameLog[ApplyFrame];
		const FRefHudFrameRecord& Prev = Host->FrameLog[ApplyFrame - 1];
		OutDelta = FEvalDelta{
			Applied.ScalarGets - Prev.ScalarGets, Applied.ArraySizes - Prev.ArraySizes, Applied.ArrayChilds - Prev.ArrayChilds};
		if (OutBumpUpdateMs != nullptr)
		{
			*OutBumpUpdateMs = Applied.UpdateMs;
		}
		AddInfo(FString::Printf(TEXT("%s: %s"), What, *OutDelta.ToString()));
		return true;
	};

	FEvalDelta DeltaAlpha, DeltaBravo, DeltaBoth, DeltaPlate;
	double AlphaBumpUpdateMs = 0.0, BravoBumpUpdateMs = 0.0;
	if (!BumpAndMeasure(TEXT("alpha bump"), 1,
			[](FVaCuusRefHudTestModel& M) { M.TeamAlpha[3].Kills += 1; }, DeltaAlpha, &AlphaBumpUpdateMs))
	{
		return false;
	}
	if (!BumpAndMeasure(TEXT("bravo bump"), 1,
			[](FVaCuusRefHudTestModel& M) { M.TeamBravo[3].Kills += 1; }, DeltaBravo, &BravoBumpUpdateMs))
	{
		return false;
	}
	if (!BumpAndMeasure(TEXT("both-panel bump"), 2,
			[](FVaCuusRefHudTestModel& M)
			{
				M.TeamAlpha[9].Score += 100;
				M.TeamBravo[9].Score += 100;
			},
			DeltaBoth))
	{
		return false;
	}
	if (!BumpAndMeasure(TEXT("plate scalar"), 1,
			[](FVaCuusRefHudTestModel& M) { M.Health = 55.f; }, DeltaPlate))
	{
		return false;
	}

	// The topology, in four exact statements.
	TestTrue(TEXT("a one-panel bump evaluates SOMETHING"), DeltaAlpha.Gets > 0 && DeltaAlpha.Childs > 0);
	TestTrue(FString::Printf(TEXT("the two panels cost the SAME (alpha %s, bravo %s)"), *DeltaAlpha.ToString(),
				 *DeltaBravo.ToString()),
		DeltaAlpha == DeltaBravo);
	TestTrue(FString::Printf(TEXT("a both-panel bump costs EXACTLY the sum (both %s, alpha+bravo %s)"),
				 *DeltaBoth.ToString(), *(DeltaAlpha + DeltaBravo).ToString()),
		DeltaBoth == DeltaAlpha + DeltaBravo);
	TestTrue(FString::Printf(TEXT("a plate scalar touches NO array evaluation (%s)"), *DeltaPlate.ToString()),
		DeltaPlate.Sizes == 0 && DeltaPlate.Childs == 0 && DeltaPlate.Gets > 0 && DeltaPlate.Gets < DeltaAlpha.Gets);

	UE_LOG(LogVaCuus, Display,
		TEXT("Exp-REF-SCALE dirty scope: one-panel %s | other panel %s | both-panel %s | plate scalar %s"),
		*DeltaAlpha.ToString(), *DeltaBravo.ToString(), *DeltaBoth.ToString(), *DeltaPlate.ToString());

	// The soak half's ms verdict: the one-panel bump frame's Update against the still
	// median, next to the law's prediction. Loose 10x tripwire only — the number
	// itself is the record, and this venue (probe host, 1280x720, blips live) is the
	// controlled one the passport cites.
	const double AlphaExtraMs = AlphaBumpUpdateMs - StillUpdateMedianMs;
	const double BravoExtraMs = BravoBumpUpdateMs - StillUpdateMedianMs;
	UE_LOG(LogVaCuus, Display,
		TEXT("Exp-REF-SCALE soak half: one-panel bump-frame Update %.3f / %.3f ms (alpha/bravo) vs still median %.3f ms ")
		TEXT("=> extra %.3f / %.3f ms; the ~0.53 us/binding law predicts ~0.10 ms for 192 bindings"),
		AlphaBumpUpdateMs, BravoBumpUpdateMs, StillUpdateMedianMs, AlphaExtraMs, BravoExtraMs);
	TestTrue(TEXT("one-panel bump extra under the 10x tripwire (1.0 ms)"), AlphaExtraMs < 1.0 && BravoExtraMs < 1.0);

	UIThread->EnqueueRemoveView(ViewId);
	RunFrames(*UIThread, 1);
	return true;
}

namespace VaCuusRefHudTest
{
/**
 * The Exp-BLIP-DRIVER documents: 64 blips and a rAF that repositions every one
 * each frame from a FRAME COUNTER (not the timestamp), so the two modes write
 * byte-identical position sequences and differ in exactly one thing -- ONE
 * transform write per blip vs TWO box-property writes (left+top). The arithmetic
 * is refhud_logic.js's orbital shape, verbatim.
 */
static FString MakeBlipDocument(bool bTransformMode)
{
	return FString::Printf(TEXT(R"(<rml>
<head><style>body { display: block; font-family: LatoLatin; font-size: 14px; } div { display: block; }
#mm { position: absolute; left: 0; top: 0; width: 220px; height: 220px; }
.blip { position: absolute; left: 0; top: 0; width: 5px; height: 5px; background-color: #e04f4f; }</style>
<script>
'use strict';
var MODE_TRANSFORM = %s;
var blips = [];
var frame = 0;
var mm = document.getElementById('mm');
for (var i = 0; i < 64; i++)
{
	var b = document.createElement('div');
	b.classList.add('blip');
	mm.appendChild(b);
	blips.push(b);
}
function tick()
{
	requestAnimationFrame(tick);
	frame++;
	var t = frame * 0.016;
	for (var i = 0; i < 64; i++)
	{
		var radius = 18 + (i * 13) %% 78;
		var rate = 0.2 + (i %% 7) * 0.09;
		var phase = i * 2.399;
		var x = 107 + radius * Math.cos(t * rate + phase);
		var y = 107 + radius * Math.sin(t * rate + phase);
		if (MODE_TRANSFORM)
		{
			blips[i].style.transform = 'translate(' + x.toFixed(1) + 'px, ' + y.toFixed(1) + 'px)';
		}
		else
		{
			blips[i].style.left = x.toFixed(1) + 'px';
			blips[i].style.top = y.toFixed(1) + 'px';
		}
	}
	globalThis.blipFrames = frame;
}
requestAnimationFrame(tick);
</script>
</head>
<body><div id="mm"></div></body>
</rml>)"),
		bTransformMode ? TEXT("true") : TEXT("false"));
}

/** Blip 0's live position property, as a string -- the motion observable. */
static FString ReadBlipProbe(FRefHudProbeHost& Self, bool bTransformMode)
{
	Rml::ElementDocument* Document = Self.GetDocument();
	Rml::Element* Container = Document ? Document->GetElementById("mm") : nullptr;
	Rml::Element* Blip = (Container && Container->GetNumChildren() > 0) ? Container->GetChild(0) : nullptr;
	if (Blip == nullptr)
	{
		return TEXT("<no blip>");
	}
	const Rml::Property* Property = Blip->GetProperty(bTransformMode ? "transform" : "left");
	return Property ? FString(UTF8_TO_TCHAR(Property->ToString().c_str())) : TEXT("<no property>");
}

static double Mean(const TArray<double>& Values)
{
	double Sum = 0.0;
	for (const double Value : Values)
	{
		Sum += Value;
	}
	return Values.IsEmpty() ? 0.0 : Sum / Values.Num();
}

static double Percentile(TArray<double> Values, double P)
{
	if (Values.IsEmpty())
	{
		return 0.0;
	}
	Values.Sort();
	return Values[FMath::Clamp(int32(P * Values.Num()), 0, Values.Num() - 1)];
}
}	 // namespace VaCuusRefHudTest

/**
 * Exp-BLIP-DRIVER (spec 2(h), plan 4.2): the 64-blip rAF run BOTH ways for ~500
 * frames each, per-frame JsPump and Context::Update() costs recorded -- the
 * numbers behind refhud_logic.js's single-transform-write idiom (and the ones
 * the passport's blip row cites). Assertions are structural tripwires (frames
 * ran, blips MOVED, costs under a 10x ceiling); the numbers are the deliverable,
 * reported via AddInfo and the log.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusRefHudBlipDriverTest, "VaCuus.RefHud.BlipDriver",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusRefHudBlipDriverTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusRefHudTest;

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
		AddInfo(TEXT("Skipped: no script host (VaCuusJs absent or vacuus.Js.Enable 0), so no rAF can run"));
		return true;
	}

	struct FModeResult
	{
		double PumpMeanMs = 0.0, PumpP99Ms = 0.0, UpdateMeanMs = 0.0, UpdateP99Ms = 0.0;
		int32 Frames = 0;
	};

	const auto RunMode = [&](const TCHAR* ModeName, bool bTransformMode, FModeResult& Out) -> bool
	{
		const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
		TUniquePtr<FRefHudProbeHost> OwnedHost =
			MakeUnique<FRefHudProbeHost>(bTransformMode ? TEXT("vacuus_blip_transform") : TEXT("vacuus_blip_lefttop"));
		FRefHudProbeHost* Host = OwnedHost.Get();

		Host->Actions.SetNum(3);
		Host->Actions[1] = [bTransformMode](FRefHudProbeHost& Self)
		{ Self.PhaseProbeValue[1] = ReadBlipProbe(Self, bTransformMode); };
		Host->Actions[2] = [bTransformMode](FRefHudProbeHost& Self)
		{ Self.PhaseProbeValue[2] = ReadBlipProbe(Self, bTransformMode); };

		const uint32 ViewId = UIThread->AllocateViewId();
		UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), FIntPoint(400, 300), Status);
		UIThread->EnqueueLoadDocumentFromMemory(ViewId, MakeBlipDocument(bTransformMode), /*LoadSerial=*/1);

		if (!TestTrue(FString::Printf(TEXT("%s: boot frames ran"), ModeName), RunFrames(*UIThread, 5)))
		{
			return false;
		}
		if (!TestTrue(FString::Printf(TEXT("%s: the blip document loaded"), ModeName),
				Status->LoadCompletedSerial.load(std::memory_order_acquire) == 1
					&& Status->LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded)))
		{
			return false;
		}

		// Warm up out of the boot's parse/first-layout shadow, then the soak window.
		if (!TestTrue(FString::Printf(TEXT("%s: warm-up ran"), ModeName), RunFrames(*UIThread, 40)))
		{
			return false;
		}
		if (!TestTrue(FString::Printf(TEXT("%s: probe phase 1"), ModeName), RunPhase(*UIThread, *Host, 1)))
		{
			return false;
		}

		const int32 SoakStart = SettledFrames(*Status);
		if (!TestTrue(FString::Printf(TEXT("%s: 500 soak frames ran"), ModeName), RunFrames(*UIThread, 500)))
		{
			return false;
		}
		const int32 SoakEnd = SettledFrames(*Status);

		if (!TestTrue(FString::Printf(TEXT("%s: probe phase 2"), ModeName), RunPhase(*UIThread, *Host, 2)))
		{
			return false;
		}

		// The blips MOVED: the same property read at the two beats differs -- without
		// this, a dead rAF would "measure" a splendidly cheap idle loop.
		TestTrue(FString::Printf(TEXT("%s: blip 0 moved across the soak ('%s' -> '%s')"), ModeName,
					 *Host->PhaseProbeValue[1], *Host->PhaseProbeValue[2]),
			Host->PhaseProbeValue[1] != Host->PhaseProbeValue[2] && !Host->PhaseProbeValue[1].StartsWith(TEXT("<no")));

		TArray<double> Pump, Update;
		Pump.Reserve(SoakEnd - SoakStart);
		Update.Reserve(SoakEnd - SoakStart);
		for (int32 Index = SoakStart; Index < SoakEnd; ++Index)
		{
			Pump.Add(Host->FrameLog[Index].PumpMs);
			Update.Add(Host->FrameLog[Index].UpdateMs);
		}

		Out.Frames = Pump.Num();
		Out.PumpMeanMs = Mean(Pump);
		Out.PumpP99Ms = Percentile(Pump, 0.99);
		Out.UpdateMeanMs = Mean(Update);
		Out.UpdateP99Ms = Percentile(Update, 0.99);

		UIThread->EnqueueRemoveView(ViewId);
		RunFrames(*UIThread, 1);
		return true;
	};

	FModeResult Transform, LeftTop;
	if (!RunMode(TEXT("transform"), /*bTransformMode=*/true, Transform)
		|| !RunMode(TEXT("left+top"), /*bTransformMode=*/false, LeftTop))
	{
		return false;
	}

	const FString Report = FString::Printf(
		TEXT("Exp-BLIP-DRIVER (64 blips/frame): transform single-write pump %.3f ms mean / %.3f p99, update %.3f mean / %.3f p99 ")
		TEXT("(%d frames) | left+top pump %.3f mean / %.3f p99, update %.3f mean / %.3f p99 (%d frames)"),
		Transform.PumpMeanMs, Transform.PumpP99Ms, Transform.UpdateMeanMs, Transform.UpdateP99Ms, Transform.Frames,
		LeftTop.PumpMeanMs, LeftTop.PumpP99Ms, LeftTop.UpdateMeanMs, LeftTop.UpdateP99Ms, LeftTop.Frames);
	AddInfo(Report);
	UE_LOG(LogVaCuus, Display, TEXT("%s"), *Report);

	// Loose tripwires only (the M3a 10x convention): the blip budget row is 0.50 ms,
	// so a 5 ms mean pump is a structural break, not jitter; the numbers themselves
	// are recorded above, not gated here.
	TestTrue(TEXT("transform-mode pump under the 10x tripwire"), Transform.PumpMeanMs < 5.0);
	TestTrue(TEXT("left+top-mode pump under the 10x tripwire"), LeftTop.PumpMeanMs < 5.0);
	TestEqual(TEXT("both soaks ran the full window"), Transform.Frames, LeftTop.Frames);

	return true;
}

/**
 * Exp-RAM-DELTA's Dev-only PROXY CROSS-CHECK + THE GMALLOC CANARY (M6 Task 5, spec
 * §2(i)). The passport's RAM row is primarily the A/B two-run UsedPhysical delta in
 * cooked Shipping — the venue this WITH_DEV_AUTOMATION_TESTS test can never see; what
 * it CAN pin, with the symmetric quantized ledger, is the plugin-visible GMalloc side:
 *
 *  1. THE CANARY, first (spec risk table: "the canary before the Dev cross-check
 *     window"): the research's [inference] — RmlUi's plain operator new lands in
 *     GMalloc via UE's per-module operator-new replacement — is verified by watching a
 *     context created and destroyed by VENDORED code move the ledger both directions.
 *     Without this, a silently bypassing allocator would make the boot window below
 *     read splendidly small and mean nothing.
 *  2. THE BOOT WINDOW: pre-boot quiesced baseline -> the full RefHud boot (real
 *     document over the VFS, both panels applied, JS third seeded) -> settled steady
 *     state. The delta is the row's cross-check figure, asserted under the §11 32 MiB
 *     CPU gate (the automation venue has no view RT — GPU is reported separately by
 *     design, the owner decision the passport records).
 *  3. THE STILL WINDOW: 60 more settled frames must move the ledger by less than
 *     256 KiB — the quiesced-window claim made checkable; without it "steady state"
 *     in (2) is a hope, and a per-frame leak would silently date the boot figure.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusRefHudMemProxyTest, "VaCuus.RefHud.MemProxy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusRefHudMemProxyTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusRefHudTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no UI thread to drive"));
		return true;
	}
	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	// (1) THE CANARY — vendored-code allocations on the test thread (the glass-test
	// venue: single-threaded RmlUi use before any UI thread exists). CreateContext
	// allocates inside libRmlUi's own compiled objects, so the ledger moving proves the
	// per-module operator-new replacement routes RmlUi through GMalloc.
	{
		FVaCuusEngine& Engine = FVaCuusEngine::Get();
		if (!TestTrue(TEXT("canary: RmlUi initialized"), Engine.Initialize()))
		{
			return false;
		}

		Rml::Context* Canary = nullptr;
		if (TestTrue(TEXT("canary: alloc window opened"), VaCuusAllocWindow::Begin()))
		{
			Canary = Rml::CreateContext("vacuus_memproxy_canary", Rml::Vector2i(64, 64));
			const VaCuusAllocWindow::FCounts Create = VaCuusAllocWindow::End();
			TestNotNull(TEXT("canary: context created"), Canary);
			TestTrue(FString::Printf(TEXT("canary: CreateContext allocated through GMalloc (mallocs=%llu, bytes=%+lld)"),
						 Create.Mallocs, Create.LiveQuantizedBytesDelta),
				Create.Mallocs > 0 && Create.LiveQuantizedBytesDelta > 0);
			TestTrue(TEXT("canary: every block's size resolved"), Create.SizeLookupFailures == 0);
		}
		if (Canary != nullptr && TestTrue(TEXT("canary: release window opened"), VaCuusAllocWindow::Begin()))
		{
			Rml::RemoveContext("vacuus_memproxy_canary");
			const VaCuusAllocWindow::FCounts Remove = VaCuusAllocWindow::End();
			TestTrue(FString::Printf(TEXT("canary: RemoveContext freed through GMalloc (frees=%llu, bytes=%+lld)"),
						 Remove.Frees, Remove.LiveQuantizedBytesDelta),
				Remove.Frees > 0 && Remove.LiveQuantizedBytesDelta < 0);
		}
		Engine.Shutdown();
	}

	// (2) THE BOOT WINDOW, through the production-shaped UI-thread pipeline.
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
		// The Count test's rule: without the JS third this measures a different
		// document; skip with a name rather than record a number for the wrong workload.
		AddInfo(TEXT("Skipped: no script host (VaCuusJs absent or vacuus.Js.Enable 0), so the JS-built third cannot exist"));
		return true;
	}

	const UScriptStruct* Type = FVaCuusRefHudTestModel::StaticStruct();
	const TSharedRef<FVaCuusBoundModel> Model = MakeShared<FVaCuusBoundModel>(GModelName, Type);
	if (!TestTrue(TEXT("the model built"), Model->IsValid()))
	{
		return false;
	}

	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
	TUniquePtr<FRefHudProbeHost> OwnedHost = MakeUnique<FRefHudProbeHost>(TEXT("vacuus_refhud_memproxy"));
	FRefHudProbeHost* Host = OwnedHost.Get();
	Host->ObservedModel = Model;

	// Quiesce the fresh UI thread OUTSIDE the window so thread start-up cost never
	// masquerades as document cost.
	const uint32 ViewId = UIThread->AllocateViewId();
	if (!TestTrue(TEXT("boot: alloc window opened"), VaCuusAllocWindow::Begin()))
	{
		return false;
	}

	UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), FIntPoint(1280, 720), Status);
	UIThread->EnqueueBindModel(ViewId, Model);
	UIThread->EnqueueLoadDocumentFile(ViewId, TEXT("RefHud/refhud.rml"), /*LoadSerial=*/1);

	FVaCuusRefHudTestModel Live;
	SeedModel(Live);
	TestTrue(TEXT("the first sample marks fields"), Model->Sample(Type, &Live) > 0);
	TestTrue(TEXT("...and publishes"), Model->PublishPending());

	bool bLoaded = false;
	for (int32 Attempt = 0; Attempt < 20 && !bLoaded; ++Attempt)
	{
		if (!TestTrue(TEXT("frames ran"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		bLoaded = Status->LoadCompletedSerial.load(std::memory_order_acquire) == 1;
	}
	if (!TestTrue(TEXT("RefHud/refhud.rml loaded through the VFS"),
			bLoaded && Status->LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded)))
	{
		VaCuusAllocWindow::End();
		return false;
	}

	// Settle to declared steady state inside the window (clones built, pools filled,
	// first collections done), then close it.
	if (!TestTrue(TEXT("settle frames ran"), RunFrames(*UIThread, 30)))
	{
		VaCuusAllocWindow::End();
		return false;
	}
	const VaCuusAllocWindow::FCounts Boot = VaCuusAllocWindow::End();

	const double BootMiB = double(Boot.LiveQuantizedBytesDelta) / (1024.0 * 1024.0);
	const FString BootReport = FString::Printf(
		TEXT("Exp-RAM-DELTA proxy cross-check: RefHud boot -> steady state added %+lld live quantized bytes (%.2f MiB; ")
		TEXT("mallocs=%llu reallocs=%llu frees=%llu, size-lookup failures=%llu)"),
		Boot.LiveQuantizedBytesDelta, BootMiB, Boot.Mallocs, Boot.Reallocs, Boot.Frees, Boot.SizeLookupFailures);
	AddInfo(BootReport);
	UE_LOG(LogVaCuus, Display, TEXT("%s"), *BootReport);

	TestTrue(TEXT("boot window: every block's size resolved (the ledger's exactness bit)"), Boot.SizeLookupFailures == 0);
	TestTrue(TEXT("boot window: the delta is positive and sane (> 256 KiB — a bypassing allocator would read near zero)"),
		Boot.LiveQuantizedBytesDelta > 256 * 1024);
	TestTrue(TEXT("boot window: under the spec 11 CPU-side 32 MiB gate"), Boot.LiveQuantizedBytesDelta < int64(32) * 1024 * 1024);

	// (3) THE STILL WINDOW.
	if (!TestTrue(TEXT("still: alloc window opened"), VaCuusAllocWindow::Begin()))
	{
		return false;
	}
	const bool bStillRan = RunFrames(*UIThread, 60);
	const VaCuusAllocWindow::FCounts Still = VaCuusAllocWindow::End();
	if (!TestTrue(TEXT("still frames ran"), bStillRan))
	{
		return false;
	}
	const FString StillReport = FString::Printf(
		TEXT("Exp-RAM-DELTA still window: 60 settled frames moved the ledger %+lld bytes (mallocs=%llu frees=%llu)"),
		Still.LiveQuantizedBytesDelta, Still.Mallocs, Still.Frees);
	AddInfo(StillReport);
	UE_LOG(LogVaCuus, Display, TEXT("%s"), *StillReport);
	TestTrue(TEXT("still window: steady state holds (|delta| < 256 KiB over 60 frames)"),
		FMath::Abs(Still.LiveQuantizedBytesDelta) < 256 * 1024);

	UIThread->EnqueueRemoveView(ViewId);
	RunFrames(*UIThread, 1);
	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
