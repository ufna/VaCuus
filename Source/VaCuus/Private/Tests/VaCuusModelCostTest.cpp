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

#endif	  // WITH_DEV_AUTOMATION_TESTS
