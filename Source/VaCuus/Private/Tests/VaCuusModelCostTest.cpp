// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusBoundModel.h"
#include "VaCuusDefines.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusModelLayout.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "VaCuusModelLayoutTestTypes.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"

#include <RmlUi/Core.h>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * SPEC 9's TWO UI-THREAD ROWS, MEASURED: the apply (copy + DirtyVariable) and the
 * re-evaluation that dirtying causes inside Context::Update().
 *
 *   | UI-thread copy + DirtyVariable       | <= 0.02 ms |
 *   | UI-thread re-evaluation from dirtying| <= 0.05 ms |
 *
 * WHY THE SECOND ONE NEEDS A DELTA AND NOT A SCOPE. It is not paid where VaCuus can put a
 * timer: DirtyVariable only records a name in DataModel::dirty_variables (DataModel.cpp:325-331),
 * and the work happens later, inside Context::Update(), when DataModel::Update hands that set to
 * DataViews::Update -- which looks each dirtied name up in name_view_map and runs the views it
 * finds (DataView.cpp:90-112). So the cost lands in the Update scope, indistinguishable from the
 * layout and animation work already there, and the only way to isolate it is to run two
 * otherwise identical contexts and subtract.
 *
 * THREE VIEWS, BECAUSE THE ROW IS TWO DIFFERENT QUESTIONS AND THE SPEC ONLY BUDGETS ONE.
 *
 *   STILL    -- a bound model that never changes. Nothing is dirtied. This is the baseline: the
 *               cost of Update() for this document with the data binding contributing nothing.
 *   REDRAWN  -- 32 float variables dirtied every frame with values that RENDER IDENTICALLY.
 *               RmlUi ships a bound double through "%.3f" plus TrimTrailingDotZeros
 *               (TypeConverter.inl:282-295), so 0.0001 and 0.0002 are both the string "0";
 *               DataViewText::Update then evaluates the expression and skips SetText because
 *               `entry.value != value` is false (DataViewDefault.cpp:354). Every expression runs
 *               and the DOM never moves. REDRAWN - STILL is therefore the PURE re-evaluation
 *               cost, which is exactly what spec 9's third row names -- and exactly the
 *               experiment the spec's own note describes, where dirtying everything every frame
 *               left the render-level idle test passing with identical numbers.
 *   CHANGING -- all 64 fields moving to genuinely different values. CHANGING - STILL is the
 *               real-world total: re-evaluation PLUS the SetText it causes PLUS the relayout
 *               that follows. Reported because it is the number a game actually pays, and it is
 *               an upper bound on the budgeted one.
 *
 * The DOM guard is not decoration: if RmlUi ever renders more decimals, REDRAWN stops being a
 * no-DOM-change case and its number silently becomes CHANGING's. NumDomChanges catches that.
 *
 * WHAT IS PRODUCTION HERE. The layout, both shadows, the channel, the sampler, the bind, the
 * apply (FVaCuusBoundModel::ApplyPendingUpdate, the same call FVaCuusUIThread::ApplyModelUpdates
 * makes) and the Rml::Context are the real ones. Only the host is a rig, and it is a rig for one
 * reason: it has to bracket Update() with a timer, which the production host has no business
 * doing. The model is bound by the host rather than by an EnqueueBindModel so that this test's
 * apply is not ALSO run by the UI thread's own loop -- one applier, one measurement.
 *
 * CEILINGS ARE LOOSE (10x), like VaCuus.Model.Sampler.Cost's and for the same reason: this runs
 * on whatever machine the suite runs on, inside an editor doing other things. The numbers are
 * the deliverable; the assertions are tripwires for a structural regression.
 */
namespace VaCuusModelCostTest
{
/** How many float fields the cost fixture has; REDRAWN dirties exactly these. */
static constexpr int32 NumFloatFields = 32;

/** Measured frames per view. Enough that FPlatformTime's own cost is far below the signal. */
static constexpr int32 NumMeasuredFrames = 200;

/**
 * A probe host that owns one bound model, binds it to its own context before any document
 * loads, and brackets the two UI-thread phases spec 9 budgets.
 */
class FCostHost final : public IVaCuusDocumentHost
{
public:
	FCostHost(const TCHAR* InContextName, TSharedRef<FVaCuusBoundModel> InModel, FString InRml)
		: ContextName(InContextName)
		, Model(MoveTemp(InModel))
		, Rml(MoveTemp(InRml))
	{
	}

	virtual bool Initialize(uint32 InViewId, const TSharedRef<FVaCuusViewStatus>& InStatus) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		Status = InStatus;
		Context = Rml::CreateContext(Rml::String(TCHAR_TO_UTF8(*ContextName)), Rml::Vector2i(1, 1));
		if (Context == nullptr)
		{
			return false;
		}

		// BOUND HERE, WHICH IS BEFORE ANY DOCUMENT EXISTS -- the ordering `data-model` requires.
		// Element::SetParent resolves the attribute exactly once, when the body is parented into
		// the context (Element.cpp:2202-2219); a model created after that attaches to nothing.
		bBound = Model->BindToContext(*Context);
		return bBound;
	}

	virtual void Shutdown() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		CloseDocument();
		if (Context)
		{
			Rml::RemoveContext(Rml::String(TCHAR_TO_UTF8(*ContextName)));
			Context = nullptr;
		}
	}

	virtual void SetViewSize(FIntPoint InViewSize) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		ViewSize = InViewSize;
		if (Context)
		{
			Context->SetDimensions(Rml::Vector2i(ViewSize.X, ViewSize.Y));
		}
	}

	virtual void LoadDocumentFromFile(const FString& VfsPath, uint64 LoadSerial) override {}

	virtual void LoadDocumentFromMemory(const FString& RmlSource, uint64 LoadSerial) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		if (Context == nullptr)
		{
			return;
		}

		Document = Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(*RmlSource)), "vacuus://model_cost.rml");
		if (Document)
		{
			Document->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document);
		}
	}

	virtual void CloseDocument() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		if (Document)
		{
			Document->Close();
			Document = nullptr;
		}
	}

	virtual void SetVisible(bool bVisible) override {}

	virtual bool HasView() const override
	{
		check(FVaCuusUIThread::IsInUIThread());
		return Context != nullptr && Document != nullptr && ViewSize.X > 0 && ViewSize.Y > 0;
	}

	virtual Rml::Context* GetContext() const override
	{
		check(FVaCuusUIThread::IsInUIThread());
		return Context;
	}

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		// THE APPLY, THE REAL ONE. This is the same call FVaCuusUIThread::ApplyModelUpdates()
		// makes for every registered model, at the same point in the frame -- after the drains
		// and before Update(), so the re-evaluation it triggers is paid inside the next bracket
		// rather than in this one.
		const double BeforeApply = FPlatformTime::Seconds();
		Model->ApplyPendingUpdate();
		const double AfterApply = FPlatformTime::Seconds();

		Context->Update();
		const double AfterUpdate = FPlatformTime::Seconds();

		// OUTSIDE BOTH BRACKETS. Reading the DOM is the guard that REDRAWN really is a
		// no-DOM-change case, and folding its cost into Update's would corrupt the very number
		// the guard protects.
		if (Document)
		{
			if (const Rml::Element* Probe = Document->GetElementById("f00"))
			{
				const FString Now(UTF8_TO_TCHAR(Probe->GetInnerRML().c_str()));
				if (NumFrames > 0 && Now != ProbeText)
				{
					++NumDomChanges;
				}
				ProbeText = Now;
			}
		}

		if (bMeasuring)
		{
			ApplySeconds += AfterApply - BeforeApply;
			UpdateSeconds += AfterUpdate - AfterApply;
			++NumMeasured;
		}

		++NumFrames;
		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

	/** Starts/stops accumulation. Written from the test thread between UI frames only. */
	void SetMeasuring(bool bInMeasuring) { bMeasuring = bInMeasuring; }

	double GetApplyMsPerFrame() const { return NumMeasured > 0 ? (ApplySeconds / NumMeasured) * 1000.0 : 0.0; }
	double GetUpdateMsPerFrame() const { return NumMeasured > 0 ? (UpdateSeconds / NumMeasured) * 1000.0 : 0.0; }

	//~ Post-frame observations. Plain members written on the UI thread and read on the test
	//~ thread only after WaitForFrameCount() saw the counter advance, which the UI thread stores
	//~ with release ordering -- the same rule VaCuusModelTestHost.h states.
	int32 NumFrames = 0;
	int32 NumMeasured = 0;
	int32 NumDomChanges = 0;
	bool bBound = false;

private:
	FString ContextName;
	TSharedRef<FVaCuusBoundModel> Model;
	FString Rml;
	FString ProbeText;

	TSharedPtr<FVaCuusViewStatus> Status;
	Rml::Context* Context = nullptr;
	Rml::ElementDocument* Document = nullptr;
	FIntPoint ViewSize = FIntPoint::ZeroValue;

	double ApplySeconds = 0.0;
	double UpdateSeconds = 0.0;
	bool bMeasuring = false;
};

/**
 * One `<div id="fNN">{{Name}}</div>` per bound field.
 *
 * ONE ELEMENT PER VARIABLE, NOT ONE ELEMENT LISTING ALL OF THEM, and the difference is the
 * shape of the cost being measured. A single element's DataViewText holds every `{{...}}` in its
 * text as separate entries and re-runs ALL of them whenever ANY of its variables is dirtied
 * (DataViewDefault.cpp:348-359), so N variables in one element would measure N^2-ish work that
 * no real document does. Separate elements give RmlUi's name_view_map one view per name
 * (DataView.cpp:82-83), which is the O(views under every dirtied name) the spec budgets.
 *
 * No font-family, deliberately: RmlUi lays out no text without a font, so this measures the data
 * path rather than the glyph cache -- and every view here is identical in that respect, so the
 * subtraction is unaffected either way.
 */
static FString BuildDocument(const FVaCuusModelLayout& Layout)
{
	FString Body;
	const TConstArrayView<FVaCuusModelField> Fields = Layout.GetFields();
	for (int32 Index = 0; Index < Fields.Num(); ++Index)
	{
		Body += FString::Printf(TEXT("\t<div id=\"f%02d\">{{%s}}</div>\n"), Index, *Fields[Index].WireName);
	}

	return FString::Printf(TEXT("<rml>\n<head><style>body { display: block; } div { display: block; }</style></head>\n")
						   TEXT("<body data-model=\"hud\">\n%s</body>\n</rml>"),
		*Body);
}

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
}	 // namespace VaCuusModelCostTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelUICostTest, "VaCuus.Model.Cost.UIApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelUICostTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelCostTest;

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

	const UScriptStruct* Type = FVaCuusSamplerCostModel::StaticStruct();
	const FName ModelName(TEXT("hud"));

	const TSharedRef<FVaCuusBoundModel> StillModel = MakeShared<FVaCuusBoundModel>(ModelName, Type);
	const TSharedRef<FVaCuusBoundModel> RedrawnModel = MakeShared<FVaCuusBoundModel>(ModelName, Type);
	const TSharedRef<FVaCuusBoundModel> ChangingModel = MakeShared<FVaCuusBoundModel>(ModelName, Type);
	if (!TestTrue(TEXT("the three models built"),
			StillModel->IsValid() && RedrawnModel->IsValid() && ChangingModel->IsValid()))
	{
		return false;
	}

	const int32 NumFields = StillModel->GetLayout().GetFields().Num();
	if (!TestEqual(TEXT("the cost fixture has 64 bound fields"), NumFields, 64))
	{
		return false;
	}

	const FString Document = BuildDocument(StillModel->GetLayout());

	TUniquePtr<FCostHost> OwnedStill = MakeUnique<FCostHost>(TEXT("vacuus_cost_still"), StillModel, Document);
	TUniquePtr<FCostHost> OwnedRedrawn = MakeUnique<FCostHost>(TEXT("vacuus_cost_redrawn"), RedrawnModel, Document);
	TUniquePtr<FCostHost> OwnedChanging = MakeUnique<FCostHost>(TEXT("vacuus_cost_changing"), ChangingModel, Document);
	FCostHost* Still = OwnedStill.Get();
	FCostHost* Redrawn = OwnedRedrawn.Get();
	FCostHost* Changing = OwnedChanging.Get();

	const uint32 StillViewId = UIThread->AllocateViewId();
	const uint32 RedrawnViewId = UIThread->AllocateViewId();
	const uint32 ChangingViewId = UIThread->AllocateViewId();

	UIThread->EnqueueAddView(StillViewId, MoveTemp(OwnedStill), FIntPoint(400, 800), MakeShared<FVaCuusViewStatus>());
	UIThread->EnqueueLoadDocumentFromMemory(StillViewId, Document, /*LoadSerial=*/1);
	UIThread->EnqueueAddView(RedrawnViewId, MoveTemp(OwnedRedrawn), FIntPoint(400, 800), MakeShared<FVaCuusViewStatus>());
	UIThread->EnqueueLoadDocumentFromMemory(RedrawnViewId, Document, /*LoadSerial=*/1);
	UIThread->EnqueueAddView(ChangingViewId, MoveTemp(OwnedChanging), FIntPoint(400, 800), MakeShared<FVaCuusViewStatus>());
	UIThread->EnqueueLoadDocumentFromMemory(ChangingViewId, Document, /*LoadSerial=*/1);

	FVaCuusSamplerCostModel StillLive;
	FVaCuusSamplerCostModel RedrawnLive;
	FVaCuusSamplerCostModel ChangingLive;

	// Addressable members, so the mutation loops stay short. Same list the sampler cost test
	// uses, and the bitfields are absent for the same reason: a bitfield has no address.
	float* const RedrawnFloats[] = {&RedrawnLive.F00, &RedrawnLive.F01, &RedrawnLive.F02, &RedrawnLive.F03, &RedrawnLive.F04,
		&RedrawnLive.F05, &RedrawnLive.F06, &RedrawnLive.F07, &RedrawnLive.F08, &RedrawnLive.F09, &RedrawnLive.F10,
		&RedrawnLive.F11, &RedrawnLive.F12, &RedrawnLive.F13, &RedrawnLive.F14, &RedrawnLive.F15, &RedrawnLive.F16,
		&RedrawnLive.F17, &RedrawnLive.F18, &RedrawnLive.F19, &RedrawnLive.F20, &RedrawnLive.F21, &RedrawnLive.F22,
		&RedrawnLive.F23, &RedrawnLive.F24, &RedrawnLive.F25, &RedrawnLive.F26, &RedrawnLive.F27, &RedrawnLive.F28,
		&RedrawnLive.F29, &RedrawnLive.F30, &RedrawnLive.F31};
	float* const ChangingFloats[] = {&ChangingLive.F00, &ChangingLive.F01, &ChangingLive.F02, &ChangingLive.F03,
		&ChangingLive.F04, &ChangingLive.F05, &ChangingLive.F06, &ChangingLive.F07, &ChangingLive.F08, &ChangingLive.F09,
		&ChangingLive.F10, &ChangingLive.F11, &ChangingLive.F12, &ChangingLive.F13, &ChangingLive.F14, &ChangingLive.F15,
		&ChangingLive.F16, &ChangingLive.F17, &ChangingLive.F18, &ChangingLive.F19, &ChangingLive.F20, &ChangingLive.F21,
		&ChangingLive.F22, &ChangingLive.F23, &ChangingLive.F24, &ChangingLive.F25, &ChangingLive.F26, &ChangingLive.F27,
		&ChangingLive.F28, &ChangingLive.F29, &ChangingLive.F30, &ChangingLive.F31};
	int32* const ChangingInts[] = {&ChangingLive.I00, &ChangingLive.I01, &ChangingLive.I02, &ChangingLive.I03,
		&ChangingLive.I04, &ChangingLive.I05, &ChangingLive.I06, &ChangingLive.I07, &ChangingLive.I08, &ChangingLive.I09,
		&ChangingLive.I10, &ChangingLive.I11, &ChangingLive.I12, &ChangingLive.I13, &ChangingLive.I14, &ChangingLive.I15};
	bool* const ChangingBools[] = {&ChangingLive.bNative0, &ChangingLive.bNative1, &ChangingLive.bNative2, &ChangingLive.bNative3};
	FString* const ChangingStrings[] = {&ChangingLive.S0, &ChangingLive.S1, &ChangingLive.S2, &ChangingLive.S3};
	FName* const ChangingNames[] = {&ChangingLive.N0, &ChangingLive.N1};
	FText* const ChangingTexts[] = {&ChangingLive.T0, &ChangingLive.T1};

	static_assert(UE_ARRAY_COUNT(RedrawnFloats) == NumFloatFields);

	// One frame of each view's mutation, then a sample and a publish for all three.
	auto DriveFrame = [&](int32 Iteration)
	{
		// REDRAWN: every float moves, and every one of them still renders as "0". RmlUi's
		// TypeConverter<double, String> is "%.3f" + TrimTrailingDotZeros
		// (TypeConverter.inl:282-295), so 0.0001 through 0.0004 are one string. The differ marks
		// all 32 (it compares BITS, VaCuusModelSampler.cpp's FloatingPoint branch), the apply
		// copies and dirties all 32, every expression is re-run -- and nothing is written to the
		// DOM.
		const float Tiny = float(Iteration % 4 + 1) * 1.0e-4f;
		for (float* Field : RedrawnFloats)
		{
			*Field = Tiny;
		}

		const bool bFlag = (Iteration & 1) != 0;
		for (float* Field : ChangingFloats)
		{
			*Field = float(Iteration) + 1.f;
		}
		for (int32* Field : ChangingInts)
		{
			*Field = Iteration + 1;
		}
		for (bool* Field : ChangingBools)
		{
			*Field = bFlag;
		}
		ChangingLive.bBit0 = bFlag;
		ChangingLive.bBit1 = bFlag;
		ChangingLive.bBit2 = bFlag;
		ChangingLive.bBit3 = bFlag;

		const FString Text = FString::Printf(TEXT("v%d"), Iteration);
		for (FString* Field : ChangingStrings)
		{
			*Field = Text;
		}
		for (FName* Field : ChangingNames)
		{
			*Field = FName(*Text);
		}
		for (FText* Field : ChangingTexts)
		{
			*Field = FText::FromString(Text);
		}

		StillModel->Sample(Type, &StillLive);
		RedrawnModel->Sample(Type, &RedrawnLive);
		ChangingModel->Sample(Type, &ChangingLive);

		StillModel->PublishPending();
		RedrawnModel->PublishPending();
		ChangingModel->PublishPending();
	};

	// ---- Warm up. ----
	//
	// The forced first publish (invariant I1) carries every field, the three triple-buffer slots
	// and every string inside them are allocated here, the documents are parsed and laid out
	// once, and RmlUi's own caches fill. None of that is per-frame cost and none of it belongs
	// in the numbers below.
	for (int32 Iteration = 0; Iteration < 8; ++Iteration)
	{
		DriveFrame(Iteration);
		if (!TestTrue(TEXT("warm-up frames ran"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
	}

	if (!TestTrue(TEXT("all three models bound to their contexts"), Still->bBound && Redrawn->bBound && Changing->bBound))
	{
		return false;
	}

	const int32 DomChangesAfterWarmup = Redrawn->NumDomChanges;

	// AS A DELTA, NOT AS AN ABSOLUTE. The warm-up's own applies are not a fixed number: the
	// forced first publish (invariant I1) carries all 64 fields, and it happens to coincide with
	// the first iteration's float writes, which are therefore folded into it rather than counted
	// again. Anchoring here says what this assertion is actually about -- what the MEASURED
	// window applied -- without encoding that coincidence.
	const uint64 RedrawnFieldsAppliedAfterWarmup = RedrawnModel->GetNumFieldsApplied();

	Still->SetMeasuring(true);
	Redrawn->SetMeasuring(true);
	Changing->SetMeasuring(true);

	for (int32 Iteration = 0; Iteration < NumMeasuredFrames; ++Iteration)
	{
		DriveFrame(Iteration + 8);
		if (!TestTrue(TEXT("measured frames ran"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
	}

	Still->SetMeasuring(false);
	Redrawn->SetMeasuring(false);
	Changing->SetMeasuring(false);

	// ---- The guard. ----
	//
	// REDRAWN's whole claim is that it dirties without changing the DOM. If RmlUi ever renders a
	// bound double with more precision this stops being true, its number silently becomes
	// CHANGING's, and the "pure re-evaluation" line below would be a lie. This is what says so.
	TestEqual(TEXT("REDRAWN never wrote the DOM, so its delta is re-evaluation and nothing else"),
		Redrawn->NumDomChanges, DomChangesAfterWarmup);
	TestTrue(TEXT("...while CHANGING wrote it on essentially every frame"),
		Changing->NumDomChanges >= NumMeasuredFrames - 2);
	TestEqual(TEXT("...and STILL never wrote it at all"), Still->NumDomChanges, 0);

	// ---- The apply: spec 9's second row. ----

	// The apply number below is per FRAME; this is what says how many fields that frame carried,
	// and therefore what the per-variable figures may be divided by.
	TestEqual(TEXT("REDRAWN applied exactly 32 fields on each measured frame"),
		int32(RedrawnModel->GetNumFieldsApplied() - RedrawnFieldsAppliedAfterWarmup), NumMeasuredFrames * NumFloatFields);

	const double StillApplyMs = Still->GetApplyMsPerFrame();
	const double RedrawnApplyMs = Redrawn->GetApplyMsPerFrame();
	const double ChangingApplyMs = Changing->GetApplyMsPerFrame();

	// ---- The re-evaluation: spec 9's third row, by subtraction. ----

	const double StillUpdateMs = Still->GetUpdateMsPerFrame();
	const double RedrawnUpdateMs = Redrawn->GetUpdateMsPerFrame();
	const double ChangingUpdateMs = Changing->GetUpdateMsPerFrame();

	const double ReevaluationMs = RedrawnUpdateMs - StillUpdateMs;
	const double ReevaluationPlusDomMs = ChangingUpdateMs - StillUpdateMs;

	AddInfo(FString::Printf(TEXT("apply (copy + DirtyVariable), ms/frame: still %.5f (0 fields), redrawn %.5f (%d fields), ")
							TEXT("changing %.5f (%d fields); budget 0.02 ms"),
		StillApplyMs, RedrawnApplyMs, NumFloatFields, ChangingApplyMs, NumFields));
	AddInfo(FString::Printf(TEXT("Context::Update(), ms/frame: still %.5f, redrawn %.5f, changing %.5f"), StillUpdateMs,
		RedrawnUpdateMs, ChangingUpdateMs));
	AddInfo(FString::Printf(
		TEXT("re-evaluation caused by dirtying %d variables with no DOM change: %.5f ms/frame (%.3f us per variable); ")
		TEXT("budget 0.05 ms"),
		NumFloatFields, ReevaluationMs, (ReevaluationMs * 1000.0) / NumFloatFields));
	AddInfo(FString::Printf(TEXT("re-evaluation + SetText + relayout for %d changing variables: %.5f ms/frame"), NumFields,
		ReevaluationPlusDomMs));

	// Logged as well as added: AddInfo lands in the automation report, and a headless acceptance
	// run reads Saved/Logs/VcHost.log.
	UE_LOG(LogVaCuus, Display,
		TEXT("VaCuus M3a UI cost (%d frames): apply still=%.5f redrawn(%d)=%.5f changing(%d)=%.5f ms | ")
		TEXT("Update still=%.5f redrawn=%.5f changing=%.5f ms | re-evaluation=%.5f ms (%.3f us/var) | ")
		TEXT("re-evaluation+DOM=%.5f ms"),
		NumMeasuredFrames, StillApplyMs, NumFloatFields, RedrawnApplyMs, NumFields, ChangingApplyMs, StillUpdateMs,
		RedrawnUpdateMs, ChangingUpdateMs, ReevaluationMs, (ReevaluationMs * 1000.0) / NumFloatFields, ReevaluationPlusDomMs);

	TestTrue(*FString::Printf(TEXT("the 64-field apply stays inside 10x the budget (%.5f ms)"), ChangingApplyMs),
		ChangingApplyMs < 0.2);
	TestTrue(*FString::Printf(TEXT("the re-evaluation stays inside 10x the budget (%.5f ms)"), ReevaluationMs),
		ReevaluationMs < 0.5);

	// ---- The idle side of the same measurement, at the apply layer. ----
	//
	// STILL is a bound model whose struct is handed to Sample() on every one of those frames and
	// never changes: after the forced first publish it must cost exactly nothing on both threads.
	TestEqual(TEXT("STILL published exactly once, ever (the forced first publish)"), int32(StillModel->GetNumPublishes()), 1);
	TestEqual(TEXT("and applied exactly one update"), int32(StillModel->GetNumUpdatesApplied()), 1);
	TestEqual(TEXT("carrying every field once and never again"), int32(StillModel->GetNumFieldsApplied()), NumFields);

	UIThread->EnqueueRemoveView(StillViewId);
	UIThread->EnqueueRemoveView(RedrawnViewId);
	UIThread->EnqueueRemoveView(ChangingViewId);
	RunFrames(*UIThread, 1);

	return true;
}

/*
 * ---- M3b: THE 200-ROW ARRAY MEASUREMENTS (spec 9, plan Task 6.4) ----
 *
 * Same architecture as the scalar cost test above -- everything is production except the
 * host -- but the host is a new class rather than FCostHost for one structural reason: two
 * of the four array rows (grow 0->200, shrink 200->0) are SINGLE-FRAME numbers, and a
 * whole-run accumulator cannot see one frame. So this host keeps a PER-FRAME log of both
 * brackets next to the model's cumulative fields-applied counter -- the FDataForProbeHost
 * FrameLog idea -- and the test selects the one frame whose counter moved
 * (FindSingleApplyFrame). That is the "SetMeasuring around exactly the one growth frame"
 * protocol with the arming race removed: nothing has to be toggled at the right moment,
 * because every frame is recorded and the apply frame is identified after the fact.
 */
namespace VaCuusModelCostTest
{
/** The model name GKillfeedDocument's data-model attribute resolves. */
static const FName GFeedModelName(TEXT("feed"));

/** Rows changed / probed by the three-view run; mid-array, like the DataForIdle control row. */
static constexpr int32 ProbeRow = 137;

/**
 * The Task 5 killfeed document without the size probe: one data-for, four `{{...}}`
 * entries per row in ONE text node -- so each row owns one DataViewText with four entries,
 * all four re-run whenever the root is dirtied (DataViewDefault.cpp:348-359). 200 rows
 * therefore cost ~800 expression evaluations per dirty, which is the "~4 bindings/row"
 * basis spec 9's re-evaluation row is priced in.
 *
 * font-family is load-bearing, not decoration: a fontless text element logs "No font face
 * defined" per layout pass, which at 200 rows would bury the log this project reads test
 * results from (the VaCuus.Model.DataFor* documents carry the same argument).
 */
static const TCHAR* GKillfeedDocument = TEXT(R"(<rml>
<head><style>body { display: block; font-family: LatoLatin; font-size: 16px; } div { display: block; }</style></head>
<body data-model="feed">
	<div id="rows"><div data-for="kill : Killfeed">{{ kill.Killer }}|{{ kill.Victim }}|{{ kill.Weapon }}|{{ kill.bHeadshot }}</div></div>
</body>
</rml>)");

/**
 * One recorded UI frame: both spec-9 brackets, the apply counter that locates a publish's
 * frame, and the DOM observations the guards need. Written on the UI thread inside
 * RecordAndPublishFrame, read on the test thread only at indices below the settled count
 * (SettledFrames below) -- WaitForFrameCount's release/acquire hand-off alone can leave one
 * unconsumed straggler frame appending; see SettledFrames.
 */
struct FArrayFrameRecord
{
	/** The model's cumulative fields-applied counter, as of this frame's apply. */
	uint64 FieldsApplied = 0;

	//~ This frame's two brackets, seconds. Per frame rather than accumulated, because the
	//~ grow/shrink rows are the cost of ONE identified frame, not a mean.
	double ApplySeconds = 0.0;
	double UpdateSeconds = 0.0;

	/** Generated data-for rows in the DOM after this frame's Update. */
	int32 NumRows = 0;

	//~ Row ProbeRow's InnerRML and its left neighbour's, when the host captures them: the
	//~ DOM-change guard for the three-view subtraction (which row moved, and no other).
	FString ProbeText;
	FString NeighborText;
};

/**
 * The probe host for the array rows: FCostHost's bracketing (the model is bound by the
 * host and applied by the host, so this test's apply is not ALSO run by the UI thread's
 * own loop -- one applier, one measurement) plus the per-frame log described above.
 */
class FArrayCostHost final : public IVaCuusDocumentHost
{
public:
	FArrayCostHost(FString InContextName, TSharedRef<FVaCuusBoundModel> InModel, bool bInCaptureProbeRows)
		: ContextName(MoveTemp(InContextName))
		, Model(MoveTemp(InModel))
		, bCaptureProbeRows(bInCaptureProbeRows)
	{
	}

	virtual bool Initialize(uint32 InViewId, const TSharedRef<FVaCuusViewStatus>& InStatus) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		Status = InStatus;
		Context = Rml::CreateContext(Rml::String(TCHAR_TO_UTF8(*ContextName)), Rml::Vector2i(1, 1));
		if (Context == nullptr)
		{
			return false;
		}

		// RESERVED ONCE, NEVER REALLOCATED, for the FDataForProbeHost reason: the test
		// thread reads settled records while a coalesced trigger may still append one more,
		// and Reserve is what keeps those EARLIER-record reads valid -- a growth realloc
		// would move the buffer out from under them. Reserve does NOT make the newest record
		// readable: AddDefaulted_GetRef bumps ArrayNum before the record is constructed, so
		// only the settled-count clamp (SettledFrames) may name it. Both halves are needed.
		// The longest run here is ~210 frames.
		FrameLog.Reserve(1024);

		// Before any document, as `data-model` requires (Element.cpp:2202-2219).
		bBound = Model->BindToContext(*Context);
		return bBound;
	}

	virtual void Shutdown() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		CloseDocument();
		if (Context)
		{
			Rml::RemoveContext(Rml::String(TCHAR_TO_UTF8(*ContextName)));
			Context = nullptr;
		}
	}

	virtual void SetViewSize(FIntPoint InViewSize) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		ViewSize = InViewSize;
		if (Context)
		{
			Context->SetDimensions(Rml::Vector2i(ViewSize.X, ViewSize.Y));
		}
	}

	virtual void LoadDocumentFromFile(const FString& VfsPath, uint64 LoadSerial) override {}

	virtual void LoadDocumentFromMemory(const FString& RmlSource, uint64 LoadSerial) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		if (Context == nullptr)
		{
			return;
		}

		Document = Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(*RmlSource)), "vacuus://array_cost.rml");
		if (Document)
		{
			Document->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document);
		}
	}

	virtual void CloseDocument() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		if (Document)
		{
			Document->Close();
			Document = nullptr;
		}
	}

	virtual void SetVisible(bool bVisible) override {}

	virtual bool HasView() const override
	{
		check(FVaCuusUIThread::IsInUIThread());
		return Context != nullptr && Document != nullptr && ViewSize.X > 0 && ViewSize.Y > 0;
	}

	virtual Rml::Context* GetContext() const override
	{
		check(FVaCuusUIThread::IsInUIThread());
		return Context;
	}

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		// The apply, the real one -- the same call FVaCuusUIThread::ApplyModelUpdates()
		// makes, at the same point in the frame: before Update(), so the re-evaluation a
		// dirtied variable causes is paid inside the next bracket rather than this one.
		const double BeforeApply = FPlatformTime::Seconds();
		Model->ApplyPendingUpdate();
		const double AfterApply = FPlatformTime::Seconds();

		Context->Update();
		const double AfterUpdate = FPlatformTime::Seconds();

		// OUTSIDE BOTH BRACKETS, like FCostHost's probe: the DOM reads are the guards'
		// instrument, and folding their cost into Update's would corrupt the number they
		// guard.
		FArrayFrameRecord& Frame = FrameLog.AddDefaulted_GetRef();
		Frame.FieldsApplied = Model->GetNumFieldsApplied();
		Frame.ApplySeconds = AfterApply - BeforeApply;
		Frame.UpdateSeconds = AfterUpdate - AfterApply;
		CaptureRows(Frame);

		if (bMeasuring)
		{
			ApplySeconds += AfterApply - BeforeApply;
			UpdateSeconds += AfterUpdate - AfterApply;
			++NumMeasured;
		}

		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

	/** Starts/stops the whole-run accumulators. Test thread, between UI frames only. */
	void SetMeasuring(bool bInMeasuring) { bMeasuring = bInMeasuring; }

	double GetApplyMsPerFrame() const { return NumMeasured > 0 ? (ApplySeconds / NumMeasured) * 1000.0 : 0.0; }
	double GetUpdateMsPerFrame() const { return NumMeasured > 0 ? (UpdateSeconds / NumMeasured) * 1000.0 : 0.0; }

	//~ Post-frame observations; see FArrayFrameRecord for the hand-off rule.
	TArray<FArrayFrameRecord> FrameLog;
	int32 NumMeasured = 0;
	bool bBound = false;

private:
	void CaptureRows(FArrayFrameRecord& Frame) const
	{
		Rml::Element* Container = Document != nullptr ? Document->GetElementById("rows") : nullptr;
		if (Container == nullptr)
		{
			return;
		}

		// Generated rows are every child WITHOUT data-for, in row order -- the template
		// keeps its attribute and rows are inserted before it (DataViewDefault.cpp:486-491,
		// :522-523; the FDataForProbeHost capture states the argument in full).
		const int NumChildren = Container->GetNumChildren();
		int32 RowIndex = 0;
		for (int Index = 0; Index < NumChildren; ++Index)
		{
			Rml::Element* Child = Container->GetChild(Index);
			if (Child == nullptr || Child->HasAttribute("data-for"))
			{
				continue;
			}

			if (bCaptureProbeRows)
			{
				if (RowIndex == ProbeRow)
				{
					Frame.ProbeText = FString(UTF8_TO_TCHAR(Child->GetInnerRML().c_str()));
				}
				else if (RowIndex == ProbeRow - 1)
				{
					Frame.NeighborText = FString(UTF8_TO_TCHAR(Child->GetInnerRML().c_str()));
				}
			}
			++RowIndex;
		}

		Frame.NumRows = RowIndex;
	}

	FString ContextName;
	TSharedRef<FVaCuusBoundModel> Model;
	bool bCaptureProbeRows = false;

	TSharedPtr<FVaCuusViewStatus> Status;
	Rml::Context* Context = nullptr;
	Rml::ElementDocument* Document = nullptr;
	FIntPoint ViewSize = FIntPoint::ZeroValue;

	double ApplySeconds = 0.0;
	double UpdateSeconds = 0.0;
	bool bMeasuring = false;
};

/**
 * How many FrameLog records the test thread may read; every test-thread index must stay
 * below it, and FrameLog.Num()/Last() are never trusted. WaitForFrameCount alone is not
 * enough: every Enqueue* ends in Trigger() (VaCuusUIThread.cpp:640), the wake event is a
 * binary AutoReset latch (VaCuusUIThread.h:347), and FrameCount increments only AFTER
 * RunFrame returns (VaCuusUIThread.cpp:774-775) -- so a trigger landing mid-frame leaves
 * the event set and the worker runs one more frame concurrent with test-thread code, whose
 * AddDefaulted_GetRef bumps ArrayNum before the record's FStrings are constructed.
 * FramesRecorded is incremented with release AFTER the record is completely filled, exactly
 * once per append; the acquire here pairs with that release. Read through the test's own
 * TSharedRef (the same object the host publishes on), not through the host, whose Status
 * member is written on the UI thread. The VaCuusDataForTest helper, duplicated because test
 * translation units share only the fixture header.
 */
static int32 SettledFrames(const FVaCuusViewStatus& Status)
{
	return int32(Status.FramesRecorded.load(std::memory_order_acquire));
}

/**
 * THE frame whose apply consumed a publish: the one index in [FirstFrame, EndFrame) where
 * the cumulative fields-applied counter moved off Before; INDEX_NONE for none or several.
 * EndFrame must be a SettledFrames() load, never Log.Num(). The VaCuusDataForTest helper,
 * duplicated because test translation units share only the fixture header -- drift is
 * caught by both call sites asserting the same protocol.
 */
static int32 FindSingleApplyFrame(const TArray<FArrayFrameRecord>& Log, int32 FirstFrame, int32 EndFrame, uint64 Before)
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
}	 // namespace VaCuusModelCostTest

/**
 * SPEC 9's TWO CHANGED-ROW UI ROWS AT 200 ROWS (plan 6.4 a+b), by the three-view
 * subtraction of the scalar test above:
 *
 *   STILL    -- a bound 200-row model that never changes: the Update() baseline, plus the
 *               proof that an idle array costs exactly nothing at this layer too.
 *   TOGGLE   -- one row's bool flips each frame. The differ marks the array, the apply
 *               SyncCopys all 200 rows and dirties the root, every row's four expressions
 *               re-evaluate -- and the only DOM write is a one-character text ("1"<->"0").
 *               TOGGLE - STILL is therefore re-evaluation plus a floor-sized DOM write:
 *               the closest an array can come to the scalar test's REDRAWN, because a
 *               genuinely DOM-less array dirty does not exist -- any element change the
 *               case-sensitive differ can see is a content change the document shows.
 *   CHANGING -- one row's Victim gets a fresh same-length string each frame: re-evaluation
 *               plus a real SetText and its relayout. CHANGING - STILL is spec 9's
 *               "re-evaluation + DOM for one changed row" -- THE number the one-bit
 *               granularity decision is taken on.
 *
 * The apply brackets of TOGGLE and CHANGING are spec 9's third row (the third SyncCopy +
 * DirtyVariable): both carry the whole 200-row array every measured frame, by design --
 * that is what one bit per array means, and pricing it is this test's purpose.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelArrayUICostTest, "VaCuus.Model.Cost.ArrayUIApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelArrayUICostTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelCostTest;
	using namespace VaCuusKillfeedFixture;

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

	const UScriptStruct* Type = FVaCuusCostFeedModel::StaticStruct();

	const TSharedRef<FVaCuusBoundModel> StillModel = MakeShared<FVaCuusBoundModel>(GFeedModelName, Type);
	const TSharedRef<FVaCuusBoundModel> ToggleModel = MakeShared<FVaCuusBoundModel>(GFeedModelName, Type);
	const TSharedRef<FVaCuusBoundModel> ChangingModel = MakeShared<FVaCuusBoundModel>(GFeedModelName, Type);
	if (!TestTrue(TEXT("the three models built"),
			StillModel->IsValid() && ToggleModel->IsValid() && ChangingModel->IsValid()))
	{
		return false;
	}

	TUniquePtr<FArrayCostHost> OwnedStill =
		MakeUnique<FArrayCostHost>(TEXT("vacuus_arraycost_still"), StillModel, /*bCaptureProbeRows=*/true);
	TUniquePtr<FArrayCostHost> OwnedToggle =
		MakeUnique<FArrayCostHost>(TEXT("vacuus_arraycost_toggle"), ToggleModel, /*bCaptureProbeRows=*/true);
	TUniquePtr<FArrayCostHost> OwnedChanging =
		MakeUnique<FArrayCostHost>(TEXT("vacuus_arraycost_changing"), ChangingModel, /*bCaptureProbeRows=*/true);
	FArrayCostHost* Still = OwnedStill.Get();
	FArrayCostHost* Toggle = OwnedToggle.Get();
	FArrayCostHost* Changing = OwnedChanging.Get();

	const uint32 StillViewId = UIThread->AllocateViewId();
	const uint32 ToggleViewId = UIThread->AllocateViewId();
	const uint32 ChangingViewId = UIThread->AllocateViewId();

	// Named, not inline: each status carries its host's FramesRecorded, which is what
	// SettledFrames clamps that host's FrameLog reads by.
	const TSharedRef<FVaCuusViewStatus> StillStatus = MakeShared<FVaCuusViewStatus>();
	const TSharedRef<FVaCuusViewStatus> ToggleStatus = MakeShared<FVaCuusViewStatus>();
	const TSharedRef<FVaCuusViewStatus> ChangingStatus = MakeShared<FVaCuusViewStatus>();

	UIThread->EnqueueAddView(StillViewId, MoveTemp(OwnedStill), FIntPoint(400, 800), StillStatus);
	UIThread->EnqueueLoadDocumentFromMemory(StillViewId, GKillfeedDocument, /*LoadSerial=*/1);
	UIThread->EnqueueAddView(ToggleViewId, MoveTemp(OwnedToggle), FIntPoint(400, 800), ToggleStatus);
	UIThread->EnqueueLoadDocumentFromMemory(ToggleViewId, GKillfeedDocument, /*LoadSerial=*/1);
	UIThread->EnqueueAddView(ChangingViewId, MoveTemp(OwnedChanging), FIntPoint(400, 800), ChangingStatus);
	UIThread->EnqueueLoadDocumentFromMemory(ChangingViewId, GKillfeedDocument, /*LoadSerial=*/1);

	FVaCuusCostFeedModel StillLive;
	FVaCuusCostFeedModel ToggleLive;
	FVaCuusCostFeedModel ChangingLive;
	Fill(StillLive, 200);
	Fill(ToggleLive, 200);
	Fill(ChangingLive, 200);

	// One frame of mutation, then a sample and a publish for all three. The differ runs on
	// the game thread for all of them, exactly as a game would drive it; only the two
	// changed models ever mark, and each marks exactly its one array bit.
	auto DriveFrame = [&](int32 Iteration)
	{
		ToggleLive.Killfeed[ProbeRow].bHeadshot = (Iteration & 1) != 0;
		ChangingLive.Killfeed[ProbeRow].Victim = FString::Printf(TEXT("Vic%06d"), Iteration);

		StillModel->Sample(Type, &StillLive);
		ToggleModel->Sample(Type, &ToggleLive);
		ChangingModel->Sample(Type, &ChangingLive);

		StillModel->PublishPending();
		ToggleModel->PublishPending();
		ChangingModel->PublishPending();
	};

	// Warm up: the forced first publish carries the whole array, the 200 rows are created
	// and laid out once (the grow spike, measured by its own test below), and every buffer
	// on the apply path reaches capacity. None of that is per-frame cost.
	for (int32 Iteration = 0; Iteration < 8; ++Iteration)
	{
		DriveFrame(Iteration);
		if (!TestTrue(TEXT("warm-up frames ran"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
	}

	if (!TestTrue(TEXT("all three models bound to their contexts"), Still->bBound && Toggle->bBound && Changing->bBound))
	{
		return false;
	}

	const int32 StillWarmupFrames = SettledFrames(*StillStatus);
	const int32 ToggleWarmupFrames = SettledFrames(*ToggleStatus);
	const int32 ChangingWarmupFrames = SettledFrames(*ChangingStatus);
	const uint64 ToggleFieldsAppliedAfterWarmup = ToggleModel->GetNumFieldsApplied();
	const uint64 ChangingFieldsAppliedAfterWarmup = ChangingModel->GetNumFieldsApplied();

	Still->SetMeasuring(true);
	Toggle->SetMeasuring(true);
	Changing->SetMeasuring(true);

	for (int32 Iteration = 0; Iteration < NumMeasuredFrames; ++Iteration)
	{
		DriveFrame(Iteration + 8);
		if (!TestTrue(TEXT("measured frames ran"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
	}

	Still->SetMeasuring(false);
	Toggle->SetMeasuring(false);
	Changing->SetMeasuring(false);

	// ---- The guards: which row moved, and no other. ----

	auto CountProbeChanges = [](const FArrayCostHost& Host, int32 FirstFrame, int32 EndFrame, bool bNeighbor)
	{
		int32 Changes = 0;
		for (int32 Index = FirstFrame; Index < EndFrame; ++Index)
		{
			const FString& Now = bNeighbor ? Host.FrameLog[Index].NeighborText : Host.FrameLog[Index].ProbeText;
			const FString& Prev = bNeighbor ? Host.FrameLog[Index - 1].NeighborText : Host.FrameLog[Index - 1].ProbeText;
			Changes += Now.Equals(Prev, ESearchCase::CaseSensitive) ? 0 : 1;
		}
		return Changes;
	};

	const int32 StillSettled = SettledFrames(*StillStatus);
	const int32 ToggleSettled = SettledFrames(*ToggleStatus);
	const int32 ChangingSettled = SettledFrames(*ChangingStatus);

	TestEqual(
		TEXT("STILL never wrote the DOM across the window"), CountProbeChanges(*Still, StillWarmupFrames, StillSettled, false), 0);
	TestTrue(TEXT("TOGGLE's probe row moved on essentially every frame"),
		CountProbeChanges(*Toggle, ToggleWarmupFrames, ToggleSettled, false) >= NumMeasuredFrames - 2);
	TestTrue(TEXT("CHANGING's probe row moved on essentially every frame"),
		CountProbeChanges(*Changing, ChangingWarmupFrames, ChangingSettled, false) >= NumMeasuredFrames - 2);
	TestEqual(TEXT("TOGGLE's untouched neighbour row never moved"),
		CountProbeChanges(*Toggle, ToggleWarmupFrames, ToggleSettled, true), 0);
	TestEqual(TEXT("CHANGING's untouched neighbour row never moved"),
		CountProbeChanges(*Changing, ChangingWarmupFrames, ChangingSettled, true), 0);
	TestEqual(TEXT("all 200 rows stayed on screen"), Changing->FrameLog[ChangingSettled - 1].NumRows, 200);

	// The apply numbers below are per FRAME; this is what says each measured frame carried
	// exactly the one whole-array field -- and, with it, that frames and iterations stayed
	// in lockstep across the window.
	TestEqual(TEXT("TOGGLE applied the one array field on each measured frame"),
		int32(ToggleModel->GetNumFieldsApplied() - ToggleFieldsAppliedAfterWarmup), NumMeasuredFrames);
	TestEqual(TEXT("CHANGING applied the one array field on each measured frame"),
		int32(ChangingModel->GetNumFieldsApplied() - ChangingFieldsAppliedAfterWarmup), NumMeasuredFrames);

	// The idle side, exact, at 200 rows: an unchanging bound array costs nothing here either.
	TestEqual(TEXT("STILL published exactly once, ever"), int32(StillModel->GetNumPublishes()), 1);
	TestEqual(TEXT("and applied exactly one update"), int32(StillModel->GetNumUpdatesApplied()), 1);
	TestEqual(TEXT("carrying its one field once"), int32(StillModel->GetNumFieldsApplied()), 1);

	// ---- The numbers. ----

	const double StillApplyMs = Still->GetApplyMsPerFrame();
	const double ToggleApplyMs = Toggle->GetApplyMsPerFrame();
	const double ChangingApplyMs = Changing->GetApplyMsPerFrame();

	const double StillUpdateMs = Still->GetUpdateMsPerFrame();
	const double ToggleUpdateMs = Toggle->GetUpdateMsPerFrame();
	const double ChangingUpdateMs = Changing->GetUpdateMsPerFrame();

	const double ToggleDeltaMs = ToggleUpdateMs - StillUpdateMs;
	const double OneRowMs = ChangingUpdateMs - StillUpdateMs;

	AddInfo(FString::Printf(TEXT("apply (200-row SyncCopy + DirtyVariable), ms/frame: still %.5f (idle), toggle %.5f, ")
							TEXT("changing %.5f; budget 0.10 ms"),
		StillApplyMs, ToggleApplyMs, ChangingApplyMs));
	AddInfo(FString::Printf(TEXT("Context::Update(), ms/frame: still %.5f, toggle %.5f, changing %.5f"), StillUpdateMs,
		ToggleUpdateMs, ChangingUpdateMs));
	AddInfo(FString::Printf(TEXT("re-evaluation + minimal DOM (bool toggle, one row): %.5f ms/frame (%.3f us per binding ")
							TEXT("over 800)"),
		ToggleDeltaMs, (ToggleDeltaMs * 1000.0) / 800.0));
	AddInfo(FString::Printf(TEXT("re-evaluation + DOM for one changed row (THE decision number): %.5f ms/frame; ")
							TEXT("budget 0.50 ms"),
		OneRowMs));

	UE_LOG(LogVaCuus, Display,
		TEXT("VaCuus M3b UI array cost (%d frames, 200x4 rows): apply still=%.5f toggle=%.5f changing=%.5f ms | ")
		TEXT("Update still=%.5f toggle=%.5f changing=%.5f ms | re-eval+minimal-DOM=%.5f ms | ")
		TEXT("re-eval+DOM one changed row=%.5f ms"),
		NumMeasuredFrames, StillApplyMs, ToggleApplyMs, ChangingApplyMs, StillUpdateMs, ToggleUpdateMs, ChangingUpdateMs,
		ToggleDeltaMs, OneRowMs);

	TestTrue(*FString::Printf(TEXT("the 200-row apply stays inside 10x the budget (%.5f ms)"), ChangingApplyMs),
		ChangingApplyMs < 1.0);
	TestTrue(*FString::Printf(TEXT("the one-changed-row Update delta stays inside 10x the budget (%.5f ms)"), OneRowMs),
		OneRowMs < 5.0);

	UIThread->EnqueueRemoveView(StillViewId);
	UIThread->EnqueueRemoveView(ToggleViewId);
	UIThread->EnqueueRemoveView(ChangingViewId);
	RunFrames(*UIThread, 1);

	return true;
}

/**
 * SPEC 9's TWO SINGLE-FRAME ROWS (plan 6.4 c+d): grow 0->200 -- 200 SetInnerRML row parses
 * in one Update (DataViewFor::Update creates every missing row inline,
 * DataViewDefault.cpp:509-527), a load spike by design -- and shrink 200->0, whose view
 * cleanup RmlUi itself flags `@performance: Horrible` (DataView.cpp:117-132): quadratic in
 * rows, the reason spec 3.6 documents "don't clear per frame". Measured, no target, no
 * tripwire: these are numbers for the spec table and the demo's design margins, not
 * regression gates.
 *
 * PROTOCOL: five fresh contexts (fresh model, fresh view, fresh document each), and in
 * each the one growth frame and the one shrink frame are identified by the fields-applied
 * counter (FindSingleApplyFrame) out of the host's per-frame log -- the "SetMeasuring
 * around exactly the one frame" protocol with nothing to arm. Reported as min/median/max
 * across the five, because the first repetition also warms process-wide caches the later
 * ones inherit, and that spread is part of the answer.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelArrayGrowShrinkTest, "VaCuus.Model.Cost.ArrayGrowShrink",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelArrayGrowShrinkTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelCostTest;
	using namespace VaCuusKillfeedFixture;

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

	const UScriptStruct* Type = FVaCuusCostFeedModel::StaticStruct();
	constexpr int32 NumRepetitions = 5;

	TArray<double> GrowApplyMs, GrowUpdateMs, ShrinkApplyMs, ShrinkUpdateMs;

	for (int32 Rep = 0; Rep < NumRepetitions; ++Rep)
	{
		const TSharedRef<FVaCuusBoundModel> Model = MakeShared<FVaCuusBoundModel>(GFeedModelName, Type);
		if (!TestTrue(TEXT("the model built"), Model->IsValid()))
		{
			return false;
		}

		TUniquePtr<FArrayCostHost> OwnedHost = MakeUnique<FArrayCostHost>(
			FString::Printf(TEXT("vacuus_arraycost_growshrink_%d"), Rep), Model, /*bCaptureProbeRows=*/false);
		FArrayCostHost* Host = OwnedHost.Get();

		const uint32 ViewId = UIThread->AllocateViewId();
		const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
		UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), FIntPoint(400, 800), Status);
		UIThread->EnqueueLoadDocumentFromMemory(ViewId, GKillfeedDocument, /*LoadSerial=*/1);

		// ---- Empty first: the document exists, bound, with zero rows. ----
		//
		// The forced first publish (I1) carries the EMPTY array -- the differ itself finds
		// nothing to mark, which pins that the born-dirty channel, not the diff, is what
		// establishes the UI's starting state.
		FVaCuusCostFeedModel Live;
		TestEqual(TEXT("an empty live struct marks nothing"), Model->Sample(Type, &Live), 0);
		TestTrue(TEXT("but the born-dirty channel publishes anyway (I1)"), Model->PublishPending());
		if (!TestTrue(TEXT("initial frames ran"), RunFrames(*UIThread, 3)))
		{
			return false;
		}
		if (!TestTrue(TEXT("the host bound its model"), Host->bBound))
		{
			return false;
		}
		{
			const int32 InitialApply = FindSingleApplyFrame(Host->FrameLog, 0, SettledFrames(*Status), 0);
			if (!TestTrue(TEXT("exactly one initial apply"), InitialApply != INDEX_NONE))
			{
				return false;
			}
			TestEqual(TEXT("and it showed zero rows"), Host->FrameLog[InitialApply].NumRows, 0);
		}

		// ---- GROW 0 -> 200, one publish, one frame. ----

		int32 StepStart = SettledFrames(*Status);
		uint64 Before = Host->FrameLog[StepStart - 1].FieldsApplied;

		Fill(Live, 200);
		TestEqual(TEXT("growth marks the one bit"), Model->Sample(Type, &Live), 1);
		TestTrue(TEXT("and publishes"), Model->PublishPending());
		if (!TestTrue(TEXT("growth frames ran"), RunFrames(*UIThread, 3)))
		{
			return false;
		}

		{
			const int32 GrowFrame = FindSingleApplyFrame(Host->FrameLog, StepStart, SettledFrames(*Status), Before);
			if (!TestTrue(TEXT("exactly one growth apply, in a recorded frame"), GrowFrame != INDEX_NONE))
			{
				return false;
			}

			// All 200 rows in the apply frame itself, none the frame before: the whole
			// spike really is inside the one bracketed Update.
			TestEqual(TEXT("the growth frame shows all 200 rows"), Host->FrameLog[GrowFrame].NumRows, 200);
			TestEqual(TEXT("and the previous frame showed none"), Host->FrameLog[GrowFrame - 1].NumRows, 0);

			GrowApplyMs.Add(Host->FrameLog[GrowFrame].ApplySeconds * 1000.0);
			GrowUpdateMs.Add(Host->FrameLog[GrowFrame].UpdateSeconds * 1000.0);
		}

		if (!TestTrue(TEXT("settle frames ran"), RunFrames(*UIThread, 2)))
		{
			return false;
		}

		// ---- SHRINK 200 -> 0, one publish, one frame. ----

		StepStart = SettledFrames(*Status);
		Before = Host->FrameLog[StepStart - 1].FieldsApplied;

		Live.Killfeed.Empty();
		TestEqual(TEXT("the clear marks the one bit"), Model->Sample(Type, &Live), 1);
		TestTrue(TEXT("and publishes"), Model->PublishPending());
		if (!TestTrue(TEXT("shrink frames ran"), RunFrames(*UIThread, 3)))
		{
			return false;
		}

		{
			const int32 ShrinkFrame = FindSingleApplyFrame(Host->FrameLog, StepStart, SettledFrames(*Status), Before);
			if (!TestTrue(TEXT("exactly one shrink apply, in a recorded frame"), ShrinkFrame != INDEX_NONE))
			{
				return false;
			}

			TestEqual(TEXT("the shrink frame shows no rows"), Host->FrameLog[ShrinkFrame].NumRows, 0);
			TestEqual(TEXT("and the previous frame still showed all 200"), Host->FrameLog[ShrinkFrame - 1].NumRows, 200);

			ShrinkApplyMs.Add(Host->FrameLog[ShrinkFrame].ApplySeconds * 1000.0);
			ShrinkUpdateMs.Add(Host->FrameLog[ShrinkFrame].UpdateSeconds * 1000.0);
		}

		UIThread->EnqueueRemoveView(ViewId);
		RunFrames(*UIThread, 1);
	}

	auto Describe = [](const TCHAR* What, TArray<double>& Values)
	{
		Values.Sort();
		FString Raw;
		for (const double Value : Values)
		{
			Raw += FString::Printf(TEXT(" %.3f"), Value);
		}
		return FString::Printf(TEXT("%s min %.3f / median %.3f / max %.3f ms (raw:%s)"), What, Values[0],
			Values[Values.Num() / 2], Values.Last(), *Raw);
	};

	const FString GrowReport = FString::Printf(TEXT("grow 0->200 one frame: %s; %s"),
		*Describe(TEXT("Update"), GrowUpdateMs), *Describe(TEXT("apply"), GrowApplyMs));
	const FString ShrinkReport = FString::Printf(TEXT("shrink 200->0 one frame: %s; %s"),
		*Describe(TEXT("Update"), ShrinkUpdateMs), *Describe(TEXT("apply"), ShrinkApplyMs));
	AddInfo(GrowReport);
	AddInfo(ShrinkReport);
	UE_LOG(LogVaCuus, Display, TEXT("VaCuus M3b array grow/shrink (%d fresh contexts): %s | %s"), NumRepetitions,
		*GrowReport, *ShrinkReport);

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
