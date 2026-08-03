// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusBoundModel.h"
#include "VaCuusDataVariable.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "VaCuusModelLayoutTestTypes.h"

#include "HAL/PlatformProcess.h"
#include "Misc/ScopeExit.h"

#include <atomic>

#include <RmlUi/Core.h>

#if WITH_DEV_AUTOMATION_TESTS

/*
 * `data-for` END TO END (M3b Task 5): a TArray that starts life in a plain USTRUCT on the
 * game thread becomes DOM rows through the REAL pipeline -- sampler, channel, DataApply,
 * DirtyVariable, DataViewFor -- and the whole thing goes exactly quiet when nothing changes.
 *
 * WHY A THIRD HOST, when FArrayProbeHost and FProbeHost exist (the choice Task 5 leaves
 * open). FArrayProbeHost replaces everything ABOVE the apply: its phase actions write the
 * UI shadow natively on the UI thread and call DirtyVariable themselves, so it can prove the
 * adapter but never that a game-thread sample ARRIVES. FProbeHost rides the real pipeline
 * but captures one title/health pair and keeps no history -- and the claim under test here
 * is a SAME-FRAME claim: rows must appear in the very UI frame whose DataApply consumed the
 * publish. That needs, per recorded frame, both the rows and evidence of the apply, taken
 * together on the UI thread. So this host keeps a per-frame log: ApplyModelUpdates runs
 * before the per-view record loop inside one RunFrame (VaCuusUIThread.cpp:1097-1106), so the
 * frame's record already includes its own apply, and "the frame whose cumulative
 * fields-applied counter moved also shows the rows" IS the same-frame property -- with no
 * race on how many frames a coalesced trigger actually granted.
 *
 * Only the driving is a rig: the layout, both shadows, the channel, the sampler, the bind,
 * the apply, the UI thread and the Rml::Context are the production ones.
 */
namespace VaCuusDataForTest
{
static const FString GModelName(TEXT("feed"));

/**
 * Everything one recorded frame showed, written on the UI thread inside
 * RecordAndPublishFrame and read on the test thread only at indices below the settled
 * count (SettledFrames below). WaitForFrameCount's release/acquire hand-off is NOT
 * sufficient on its own -- see SettledFrames for the straggler frame it lets through.
 */
struct FFrameRecord
{
	/** ObservedModel's cumulative fields-applied counter, as of this frame's DataApply. */
	uint64 FieldsApplied = 0;

	//~ The spec 3.5 evaluation counters, absolute; deltas between records are per-window.
	//~ UI-THREAD-ONLY accessors (they assert it), which is why they are sampled here and
	//~ never on the test thread.
	int32 ScalarGets = 0;
	int32 ArraySizes = 0;
	int32 ArrayChilds = 0;

	FString Count;
	TArray<FString> Rows;
};

class FDataForProbeHost final : public IVaCuusDocumentHost
{
public:
	explicit FDataForProbeHost(const TCHAR* InContextPrefix)
		: ContextPrefix(InContextPrefix)
	{
	}

	virtual bool Initialize(uint32 InViewId, const TSharedRef<FVaCuusViewStatus>& InStatus) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		Status = InStatus;
		ContextName = FString::Printf(TEXT("%s_%u"), *ContextPrefix, InViewId);
		Context = Rml::CreateContext(Rml::String(TCHAR_TO_UTF8(*ContextName)), Rml::Vector2i(1, 1));

		// RESERVED ONCE, NEVER REALLOCATED: the test thread reads earlier records while the
		// UI thread may still append one more (a coalesced trigger can grant a frame after
		// WaitForFrameCount returns), and Reserve is what keeps those EARLIER-record reads
		// valid -- a growth realloc would move the buffer out from under them. Reserve does
		// NOT make the newest record readable: AddDefaulted_GetRef bumps ArrayNum before the
		// record is constructed, so only the settled-count clamp (SettledFrames) may name
		// it. Both halves are needed. 1024 covers the largest window here (idle: ~310
		// frames) three times over.
		FrameLog.Reserve(1024);

		return Context != nullptr;
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

	virtual void LoadDocumentFromFile(const FString& VfsPath, uint64 LoadSerial) override { Report(LoadSerial, /*bSuccess=*/false); }

	virtual void LoadDocumentFromMemory(const FString& RmlSource, uint64 LoadSerial) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		if (Context == nullptr)
		{
			Report(LoadSerial, /*bSuccess=*/false);
			return;
		}

		Rml::ElementDocument* NewDocument =
			Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(*RmlSource)), "vacuus://data_for.rml");
		if (NewDocument == nullptr)
		{
			Report(LoadSerial, /*bSuccess=*/false);
			return;
		}

		// LOAD FIRST, CLOSE SECOND, exactly as FVaCuusRmlDocumentHost::AdoptDocument -- the
		// ordering that makes "do nothing to the model on reload" safe: the new document is
		// parented, and therefore resolves `data-model`, while the model is fully live.
		CloseDocument();
		RmlDocument = NewDocument;
		RmlDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document);
		++NumDocumentsLoaded;
		Report(LoadSerial, /*bSuccess=*/true);
	}

	virtual void CloseDocument() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		if (RmlDocument)
		{
			RmlDocument->Close();
			RmlDocument = nullptr;
		}
	}

	virtual void SetVisible(bool bVisible) override {}

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

		Context->Update();

		FFrameRecord& Frame = FrameLog.AddDefaulted_GetRef();
		Frame.FieldsApplied = ObservedModel.IsValid() ? ObservedModel->GetNumFieldsApplied() : 0;
		Frame.ScalarGets = VaCuusData::GetNumScalarGets();
		Frame.ArraySizes = VaCuusData::GetNumArraySizes();
		Frame.ArrayChilds = VaCuusData::GetNumArrayChilds();
		Frame.Count = Attribute("count");
		Frame.Rows = RowTexts("rows");

		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

	//~ Configured on the test thread BEFORE EnqueueAddView; immutable after.

	/**
	 * The model whose apply this host witnesses. GetNumFieldsApplied() is written by
	 * ApplyUpdate on the UI thread and read here on the same thread, later in the same
	 * RunFrame -- no cross-thread read anywhere on this path.
	 */
	TSharedPtr<FVaCuusBoundModel> ObservedModel;

	//~ Post-frame observations; see FFrameRecord for the hand-off rule.
	TArray<FFrameRecord> FrameLog;
	int32 NumDocumentsLoaded = 0;

private:
	void Report(uint64 LoadSerial, bool bSuccess)
	{
		if (Status.IsValid() && LoadSerial != 0)
		{
			Status->LoadResult.store(
				static_cast<uint8>(bSuccess ? EVaCuusLoadResult::Succeeded : EVaCuusLoadResult::Failed), std::memory_order_relaxed);
			Status->LoadCompletedSerial.store(LoadSerial, std::memory_order_release);
		}
	}

	FString Attribute(const char* ElementId) const
	{
		if (RmlDocument == nullptr)
		{
			return FString();
		}

		Rml::Element* Element = RmlDocument->GetElementById(ElementId);
		return Element != nullptr ? FString(UTF8_TO_TCHAR(Element->GetAttribute<Rml::String>("p", Rml::String()).c_str())) : FString();
	}

	TArray<FString> RowTexts(const char* ContainerId) const
	{
		TArray<FString> Out;
		if (RmlDocument == nullptr)
		{
			return Out;
		}

		Rml::Element* Container = RmlDocument->GetElementById(ContainerId);
		if (Container == nullptr)
		{
			return Out;
		}

		// The data-for TEMPLATE keeps its attribute -- only the generated rows drop it
		// (DataViewDefault.cpp:486-491) -- and rows are inserted BEFORE it (:522-523), so
		// "every child without data-for, in order" is exactly the rows, in row order.
		const int NumChildren = Container->GetNumChildren();
		for (int Index = 0; Index < NumChildren; ++Index)
		{
			Rml::Element* Child = Container->GetChild(Index);
			if (Child != nullptr && !Child->HasAttribute("data-for"))
			{
				Out.Add(FString(UTF8_TO_TCHAR(Child->GetInnerRML().c_str())));
			}
		}

		return Out;
	}

	TSharedPtr<FVaCuusViewStatus> Status;
	FString ContextPrefix;
	FString ContextName;
	Rml::Context* Context = nullptr;
	Rml::ElementDocument* RmlDocument = nullptr;
	FIntPoint ViewSize = FIntPoint::ZeroValue;
};

/**
 * Every bound field of a row in one text node, so a row's captured InnerRML is a complete
 * value-level statement about it. The captures need no font -- InnerRML is the resolved
 * text whether or not a glyph was laid out (the FProbeHost capture argument) -- but the
 * font-family is NOT decoration: a fontless text element logs "No font face defined" per
 * layout pass, which for the 200-row fixture is ~1600 lines per suite run, in a log this
 * project reads test results from. LatoLatin is registered by FVaCuusEngine::Initialize
 * (VaCuus.Model.View.Idle's document comment carries the details).
 */
static const TCHAR* GDocument = TEXT(R"(<rml>
<head><style>body { display: block; font-family: LatoLatin; font-size: 16px; } div { display: block; }</style></head>
<body data-model="feed">
	<div id="count" data-attr-p="Killfeed.size"/>
	<div id="rows"><div data-for="kill : Killfeed">{{ kill.Killer }}|{{ kill.Victim }}|{{ kill.Weapon }}|{{ kill.bHeadshot }}</div></div>
</body>
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

/**
 * How many FrameLog records the test thread may read. Every test-thread index into
 * FrameLog must stay below this, and never trust FrameLog.Num() or Last().
 *
 * WHY WaitForFrameCount IS NOT ENOUGH: every Enqueue* ends in Trigger()
 * (VaCuusUIThread.cpp:549-555), the wake event is a binary AutoReset latch
 * (VaCuusUIThread.h:473-474), and FrameCount increments only AFTER RunFrame returns
 * (VaCuusUIThread.cpp:963-964). A trigger that lands mid-frame therefore leaves the event
 * set, and the worker runs ONE MORE frame concurrent with test-thread code that already saw
 * its awaited count -- a frame whose AddDefaulted_GetRef bumps ArrayNum BEFORE the record's
 * FStrings and TArrays are constructed, so FrameLog.Num()/Last() can name a record that is
 * mid-construction. FramesRecorded cannot: RecordAndPublishFrame increments it with release
 * ordering AFTER the record is completely filled, exactly once per append, and the acquire
 * here pairs with that release. Read through the test's own TSharedRef (the same object the
 * host publishes on), not through the host, whose Status member is written on the UI thread.
 */
static int32 SettledFrames(const FVaCuusViewStatus& Status)
{
	return int32(Status.FramesRecorded.load(std::memory_order_acquire));
}

/** What the document must render for Row: bool crosses as "1"/"0" (TypeConverter.inl:340-347). */
static FString RowText(const FVaCuusCostKillfeedRow& Row)
{
	return FString::Printf(TEXT("%s|%s|%s|%s"), *Row.Killer, *Row.Victim, *Row.Weapon, Row.bHeadshot ? TEXT("1") : TEXT("0"));
}

/**
 * THE frame whose DataApply consumed a publish: the one index in [FirstFrame, EndFrame)
 * where the cumulative fields-applied counter moved off Before. INDEX_NONE for none OR more
 * than one, so a hit doubles as "exactly one apply happened in this step". EndFrame must be
 * a SettledFrames() load, never Log.Num() -- the bound is what keeps the scan off a record
 * a straggler frame is still constructing.
 */
static int32 FindSingleApplyFrame(const TArray<FFrameRecord>& Log, int32 FirstFrame, int32 EndFrame, uint64 Before)
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
}	 // namespace VaCuusDataForTest

/**
 * GROWTH, SHRINK, ONE-ROW CHANGE AND FRONT-TRIM, each through the whole pipeline, each
 * asserted IN the UI frame that applied it (spec 8's data-for end-to-end rows).
 *
 * Value-level assertions only, also per spec 8: a write-side "no spurious SetText" claim is
 * not observable without patching vendored RmlUi, and is not claimed. What CAN be said about
 * the one-row change is said here: RmlUi re-evaluates every view in every row on any dirty
 * of the root, but the DOM write gates on compare -- `if (result && entry.value != value)`
 * before SetText (DataViewDefault.cpp:354) -- so every untouched row's captured text must
 * come back byte-identical.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusDataForRowsTest, "VaCuus.Model.DataForRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusDataForRowsTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusDataForTest;
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
	const TSharedRef<FVaCuusBoundModel> Model = MakeShared<FVaCuusBoundModel>(GModelName, Type);
	if (!TestTrue(TEXT("the model built"), Model->IsValid()))
	{
		return false;
	}

	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
	TUniquePtr<FDataForProbeHost> OwnedHost = MakeUnique<FDataForProbeHost>(TEXT("vacuus_datafor_view"));
	FDataForProbeHost* Host = OwnedHost.Get();
	Host->ObservedModel = Model;

	// BIND BEFORE LOAD: `data-model` is read exactly once, in Element::SetParent
	// (Element.cpp:2203-2219), with no retry; FIFO on the single-producer queue turns
	// "enqueued before" into "drained before".
	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), FIntPoint(400, 300), Status);
	UIThread->EnqueueBindModel(ViewId, Model);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GDocument, /*LoadSerial=*/1);

	// Checks one step's apply frame in full: the count text, the row count, every row's
	// value recomputed from the live struct -- case-SENSITIVE, since FString::operator==
	// folds case and these are byte-level value claims.
	const auto CheckApplyFrame = [&](const TCHAR* What, int32 ApplyFrame, const FVaCuusCostFeedModel& Live) -> bool
	{
		if (!TestTrue(FString::Printf(TEXT("%s: exactly one apply, in a recorded frame"), What), ApplyFrame != INDEX_NONE))
		{
			return false;
		}

		const FFrameRecord& Applied = Host->FrameLog[ApplyFrame];
		TestEqual(FString::Printf(TEXT("%s: {{Killfeed.size}} in the apply frame"), What), Applied.Count,
			FString::FromInt(Live.Killfeed.Num()));
		if (!TestEqual(FString::Printf(TEXT("%s: rows in the apply frame"), What), Applied.Rows.Num(), Live.Killfeed.Num()))
		{
			return false;
		}

		for (int32 Index = 0; Index < Live.Killfeed.Num(); ++Index)
		{
			const FString Expected = RowText(Live.Killfeed[Index]);
			if (!Applied.Rows[Index].Equals(Expected, ESearchCase::CaseSensitive))
			{
				AddError(FString::Printf(
					TEXT("%s: row %d shows '%s', expected '%s'"), What, Index, *Applied.Rows[Index], *Expected));
				return false;
			}
		}

		return true;
	};

	// One publish, pumped through, located: where the fields-applied counter moved.
	const auto PublishAndFindApply = [&](const TCHAR* What) -> int32
	{
		const int32 StepStart = SettledFrames(*Status);
		const uint64 Before = StepStart > 0 ? Host->FrameLog[StepStart - 1].FieldsApplied : 0;

		if (!TestTrue(FString::Printf(TEXT("%s: published"), What), Model->PublishPending()))
		{
			return INDEX_NONE;
		}
		if (!TestTrue(FString::Printf(TEXT("%s: frames ran"), What), RunFrames(*UIThread, 3)))
		{
			return INDEX_NONE;
		}

		return FindSingleApplyFrame(Host->FrameLog, StepStart, SettledFrames(*Status), Before);
	};

	// ---- 1. Three rows, published before the first UI frame. ----

	FVaCuusCostFeedModel Live;
	Fill(Live, 3);

	TestEqual(TEXT("the first sample marks the one field (I1)"), Model->Sample(Type, &Live), 1);
	if (!CheckApplyFrame(TEXT("initial"), PublishAndFindApply(TEXT("initial")), Live))
	{
		return false;
	}
	TestTrue(TEXT("the document loaded"),
		Status->LoadCompletedSerial.load(std::memory_order_acquire) == 1
			&& Status->LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded));

	// ---- 2. Growth: two appended rows, visible in the frame that applied them. ----

	Live.Killfeed.Add(MakeRow(3));
	Live.Killfeed.Add(MakeRow(4));
	TestEqual(TEXT("growth marks exactly the array's one bit"), Model->Sample(Type, &Live), 1);

	{
		const int32 ApplyFrame = PublishAndFindApply(TEXT("growth"));
		if (!CheckApplyFrame(TEXT("growth"), ApplyFrame, Live))
		{
			return false;
		}

		// The frame BEFORE the apply still showed three rows: the rows appeared exactly AT
		// the apply, which pins "same UI frame" from the other side.
		TestEqual(TEXT("growth: the previous frame still showed three rows"), Host->FrameLog[ApplyFrame - 1].Rows.Num(), 3);
	}

	// ---- 3. Shrink from the tail: the row disappears in the apply frame. ----

	Live.Killfeed.RemoveAt(Live.Killfeed.Num() - 1);
	TestEqual(TEXT("shrink marks exactly the array's one bit"), Model->Sample(Type, &Live), 1);
	if (!CheckApplyFrame(TEXT("shrink"), PublishAndFindApply(TEXT("shrink")), Live))
	{
		return false;
	}

	// ---- 4. One changed element: that row's text changes, every other row byte-identical. ----

	const TArray<FString> RowsBefore = Host->FrameLog[SettledFrames(*Status) - 1].Rows;

	Live.Killfeed[2].Victim = TEXT("Nemesis");
	TestEqual(TEXT("one element marks exactly the array's one bit"), Model->Sample(Type, &Live), 1);

	{
		const int32 ApplyFrame = PublishAndFindApply(TEXT("one-element"));
		if (!CheckApplyFrame(TEXT("one-element"), ApplyFrame, Live))
		{
			return false;
		}

		const FFrameRecord& Applied = Host->FrameLog[ApplyFrame];
		TestFalse(TEXT("the changed row really changed"), Applied.Rows[2].Equals(RowsBefore[2], ESearchCase::CaseSensitive));
		for (int32 Index = 0; Index < Applied.Rows.Num(); ++Index)
		{
			if (Index != 2 && !Applied.Rows[Index].Equals(RowsBefore[Index], ESearchCase::CaseSensitive))
			{
				AddError(FString::Printf(TEXT("untouched row %d moved: '%s' was '%s'"), Index, *Applied.Rows[Index],
					*RowsBefore[Index]));
			}
		}
	}

	// ---- 5. Front-trim: every row equals the SHIFTED expectation, by design. ----
	//
	// Row identity is positional and frozen -- `it` aliases Arr[i] with the creation-time i,
	// forever (DataViewDefault.cpp:509-518; spec 3.6) -- so removing the front shifts every
	// value under fixed row indices: all surviving rows re-render with their successor's
	// values and the tail row disappears. CheckApplyFrame's recompute-from-Live IS the
	// shifted expectation; asserting anything else would be fighting RmlUi's semantics.

	Live.Killfeed.RemoveAt(0);
	TestEqual(TEXT("front-trim marks exactly the array's one bit"), Model->Sample(Type, &Live), 1);
	if (!CheckApplyFrame(TEXT("front-trim"), PublishAndFindApply(TEXT("front-trim")), Live))
	{
		return false;
	}

	// Five publishes, five applies, and the echo drained after each: the channel never
	// backed up across the whole walk.
	TestEqual(TEXT("five updates were applied in all"), int32(Model->GetNumUpdatesApplied()), 5);
	TestEqual(TEXT("and nothing is outstanding"), Model->NumOutstandingFields(), 0);

	UIThread->EnqueueRemoveView(ViewId);
	RunFrames(*UIThread, 1);

	return true;
}

/**
 * RELOAD WITH LIVE ROWS (spec 8): the model survives, the rows rebuild, and updates
 * continue -- structured exactly as VaCuus.Model.Reload, and against the same heisenbug: a
 * newly added DataView is updated UNCONDITIONALLY (DataViews::Update pushes everything in
 * `views_to_add` into the dirty set before it looks at a single dirty variable,
 * DataView.cpp:70-88), so the rows-rebuilt assertion would hold even with dirtying broken.
 * The write that proves the model still WORKS therefore happens AFTER the reload, with no
 * reload between it and its assertion.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusDataForReloadTest, "VaCuus.Model.DataForReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusDataForReloadTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusDataForTest;
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
	const TSharedRef<FVaCuusBoundModel> Model = MakeShared<FVaCuusBoundModel>(GModelName, Type);
	if (!TestTrue(TEXT("the model built"), Model->IsValid()))
	{
		return false;
	}

	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
	TUniquePtr<FDataForProbeHost> OwnedHost = MakeUnique<FDataForProbeHost>(TEXT("vacuus_datafor_reload"));
	FDataForProbeHost* Host = OwnedHost.Get();
	Host->ObservedModel = Model;

	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), FIntPoint(400, 300), Status);
	UIThread->EnqueueBindModel(ViewId, Model);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GDocument, /*LoadSerial=*/1);

	const auto LastRowsEqual = [&](const TCHAR* What, const FVaCuusCostFeedModel& Live) -> bool
	{
		// The newest SETTLED record, never Last(): a straggler frame may be appending.
		const FFrameRecord& Last = Host->FrameLog[SettledFrames(*Status) - 1];
		if (!TestEqual(FString::Printf(TEXT("%s: row count"), What), Last.Rows.Num(), Live.Killfeed.Num()))
		{
			return false;
		}

		for (int32 Index = 0; Index < Live.Killfeed.Num(); ++Index)
		{
			const FString Expected = RowText(Live.Killfeed[Index]);
			if (!Last.Rows[Index].Equals(Expected, ESearchCase::CaseSensitive))
			{
				AddError(
					FString::Printf(TEXT("%s: row %d shows '%s', expected '%s'"), What, Index, *Last.Rows[Index], *Expected));
				return false;
			}
		}

		return true;
	};

	// ---- Two rows up, then grown to three: the document has LIVE data-for rows. ----

	FVaCuusCostFeedModel Live;
	Fill(Live, 2);
	TestEqual(TEXT("the first sample marks the field"), Model->Sample(Type, &Live), 1);
	Model->PublishPending();
	if (!TestTrue(TEXT("frames ran"), RunFrames(*UIThread, 3)))
	{
		return false;
	}

	Live.Killfeed.Add(MakeRow(2));
	TestEqual(TEXT("growth marks the field"), Model->Sample(Type, &Live), 1);
	Model->PublishPending();
	if (!TestTrue(TEXT("frames ran"), RunFrames(*UIThread, 3)))
	{
		return false;
	}
	if (!LastRowsEqual(TEXT("before the reload"), Live))
	{
		return false;
	}
	TestEqual(TEXT("one document loaded so far"), Host->NumDocumentsLoaded, 1);

	// ---- The reload. NOTHING is done to the model: no unbind (there is none), no rebind. ----

	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GDocument, /*LoadSerial=*/2);
	if (!TestTrue(TEXT("frames ran"), RunFrames(*UIThread, 3)))
	{
		return false;
	}

	TestEqual(TEXT("the document really was rebuilt"), Host->NumDocumentsLoaded, 2);
	TestEqual(TEXT("still one model on the UI thread, not two"), UIThread->GetNumBoundModels(), 1);

	// The weak half, kept for what it CAN pin (see the class comment): the fresh document's
	// data-for resolved against the same model, read the same UI shadow, and rebuilt all
	// three rows -- the model survived at all.
	if (!LastRowsEqual(TEXT("after the reload"), Live))
	{
		return false;
	}

	// ---- The load-bearing half: a write AFTER the reload, no reload in between. ----

	Live.Killfeed.Add(MakeRow(3));
	Live.Killfeed[0].Killer = TEXT("Phoenix");
	TestEqual(TEXT("both edits are the same field: one bit"), Model->Sample(Type, &Live), 1);
	Model->PublishPending();
	if (!TestTrue(TEXT("frames ran"), RunFrames(*UIThread, 3)))
	{
		return false;
	}

	if (!LastRowsEqual(TEXT("after the post-reload write"), Live))
	{
		return false;
	}
	TestEqual(TEXT("and no further reload happened around the write"), Host->NumDocumentsLoaded, 2);
	TestEqual(TEXT("the echo still comes back"), Model->NumOutstandingFields(), 0);

	UIThread->EnqueueRemoveView(ViewId);
	RunFrames(*UIThread, 1);

	return true;
}

/**
 * IDLE, THREE LAYERS, ALL EXACT (spec 8 / spec 9's last row): a bound, unchanging 200-row
 * model over a settled window publishes NOTHING (GetNumPublishes), applies NOTHING
 * (GetNumFieldsApplied) and evaluates NOTHING (all three spec 3.5 counters) -- exact
 * counter-deltas of zero, not bounds.
 *
 * WHY EXACT ZERO IS SOUND FOR THE EVALUATION LAYER. Idle means no dirty variable, and with
 * no dirty DataViews::Update runs its convergence loop once over an empty dirty set -- NO
 * definition virtual runs at all (spec 3.6). That is load-bearing for the exactness: a
 * data-for over a scalar or a struct evaluates FVaCuusScalarDefinition::Size /
 * FVaCuusStructDefinition::Size, and those overrides increment NOTHING
 * (VaCuusDataVariable.cpp:302-307, :466-470) -- so if idle merely meant "only cheap
 * evaluations run", uncounted virtuals could hide a regression under a delta of zero. The
 * window asserts the stronger fact: nothing was dirty, so nothing was evaluated at all,
 * counted or not.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusDataForIdleTest, "VaCuus.Model.DataForIdle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusDataForIdleTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusDataForTest;
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
	const TSharedRef<FVaCuusBoundModel> Model = MakeShared<FVaCuusBoundModel>(GModelName, Type);
	if (!TestTrue(TEXT("the model built"), Model->IsValid()))
	{
		return false;
	}

	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
	TUniquePtr<FDataForProbeHost> OwnedHost = MakeUnique<FDataForProbeHost>(TEXT("vacuus_datafor_idle"));
	FDataForProbeHost* Host = OwnedHost.Get();
	Host->ObservedModel = Model;

	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), FIntPoint(400, 300), Status);
	UIThread->EnqueueBindModel(ViewId, Model);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GDocument, /*LoadSerial=*/1);

	// ---- 1. 200 rows, built in the one frame that applied them. ----

	FVaCuusCostFeedModel Live;
	Fill(Live, 200);
	TestEqual(TEXT("the first sample marks the field"), Model->Sample(Type, &Live), 1);
	Model->PublishPending();
	if (!TestTrue(TEXT("frames ran"), RunFrames(*UIThread, 3)))
	{
		return false;
	}

	{
		const int32 ApplyFrame = FindSingleApplyFrame(Host->FrameLog, 0, SettledFrames(*Status), 0);
		if (!TestTrue(TEXT("exactly one apply built the table"), ApplyFrame != INDEX_NONE))
		{
			return false;
		}

		// All 200 in the apply frame itself: growth creates every missing row in ONE update
		// (spec 3.6; the convergence loop DataView.cpp:70-88 resolves it in two iterations,
		// nowhere near its cap of 10).
		const FFrameRecord& Applied = Host->FrameLog[ApplyFrame];
		TestEqual(TEXT("all 200 rows exist in the apply frame"), Applied.Rows.Num(), 200);
		TestEqual(TEXT("and the count reads 200"), Applied.Count, FString(TEXT("200")));
		if (Applied.Rows.Num() == 200)
		{
			TestTrue(TEXT("the first row's values"), Applied.Rows[0].Equals(RowText(Live.Killfeed[0]), ESearchCase::CaseSensitive));
			TestTrue(
				TEXT("the last row's values"), Applied.Rows[199].Equals(RowText(Live.Killfeed[199]), ESearchCase::CaseSensitive));
		}
	}

	// ---- 2. Settle: the game keeps pushing the unchanged struct until every counter stops. ----
	//
	// The VaCuus.Model.View.Idle settle-loop shape, over THIS side's observables: a window
	// only counts once fields-applied and all three evaluation counters have sat still for
	// ten consecutive frames. In practice evaluations stop one frame after the apply (the
	// dirty set empties); the loop is armour against unknowns, not an expected wait.
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
		const FFrameRecord& Now = Host->FrameLog[Settled - 1];
		const FFrameRecord& Prev = Host->FrameLog[Settled - 2];
		const bool bStable = Now.FieldsApplied == Prev.FieldsApplied && Now.ScalarGets == Prev.ScalarGets
			&& Now.ArraySizes == Prev.ArraySizes && Now.ArrayChilds == Prev.ArrayChilds;
		StableFrames = bStable ? StableFrames + 1 : 0;
	}

	if (!TestTrue(TEXT("the view reached a steady state"), StableFrames >= 10))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("settled after %d frames"), SettleFrames));

	// ---- 3. THE WINDOW: a hundred frames of a bound, unchanging 200-row model. ----
	//
	// The differ runs every frame, exactly as a game would drive it; it just finds nothing.

	const FFrameRecord WindowStart = Host->FrameLog[SettledFrames(*Status) - 1];
	const uint64 PublishesBefore = Model->GetNumPublishes();

	for (int32 Frame = 0; Frame < 100; ++Frame)
	{
		TestEqual(TEXT("an unchanged 200-row model marks nothing"), Model->Sample(Type, &Live), 0);
		TestFalse(TEXT("and has nothing to publish"), Model->PublishPending());
		if (!TestTrue(TEXT("a hundred frames ran"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
	}

	const FFrameRecord WindowEnd = Host->FrameLog[SettledFrames(*Status) - 1];

	TestEqual(TEXT("layer 1: published NOT ONCE across the window"), int32(Model->GetNumPublishes() - PublishesBefore), 0);
	TestEqual(TEXT("layer 2: applied NOT ONE field"), int32(WindowEnd.FieldsApplied - WindowStart.FieldsApplied), 0);
	TestEqual(TEXT("layer 3a: NOT ONE scalar Get"), WindowEnd.ScalarGets - WindowStart.ScalarGets, 0);
	TestEqual(TEXT("layer 3b: NOT ONE array Size"), WindowEnd.ArraySizes - WindowStart.ArraySizes, 0);
	TestEqual(TEXT("layer 3c: NOT ONE array Child"), WindowEnd.ArrayChilds - WindowStart.ArrayChilds, 0);
	TestEqual(TEXT("and the 200 rows are still on screen"), WindowEnd.Rows.Num(), 200);

	// ---- 4. The positive control -- what separates "idle" from "dead". ----
	//
	// One element changes: the publish, the apply and the evaluations all move again, and a
	// dirty of the root re-evaluates EVERY row's views (spec 3.6) while only the changed
	// row's DOM text moves (the compare-before-write gate, DataViewDefault.cpp:354).

	Live.Killfeed[137].Killer = TEXT("Control");
	TestEqual(TEXT("the control change marks the field"), Model->Sample(Type, &Live), 1);

	{
		const int32 StepStart = SettledFrames(*Status);
		TestTrue(TEXT("and publishes"), Model->PublishPending());
		if (!TestTrue(TEXT("control frames ran"), RunFrames(*UIThread, 3)))
		{
			return false;
		}

		const int32 ApplyFrame = FindSingleApplyFrame(Host->FrameLog, StepStart, SettledFrames(*Status), WindowEnd.FieldsApplied);
		if (!TestTrue(TEXT("exactly one control apply"), ApplyFrame != INDEX_NONE))
		{
			return false;
		}

		const FFrameRecord& Applied = Host->FrameLog[ApplyFrame];
		TestTrue(TEXT("the changed row tracked"),
			Applied.Rows.IsValidIndex(137) && Applied.Rows[137].Equals(RowText(Live.Killfeed[137]), ESearchCase::CaseSensitive));
		TestTrue(TEXT("its neighbour is byte-identical"),
			Applied.Rows.IsValidIndex(136) && Applied.Rows[136].Equals(WindowEnd.Rows[136], ESearchCase::CaseSensitive));
		TestTrue(TEXT("scalar Gets moved again"), Applied.ScalarGets > WindowEnd.ScalarGets);
		TestTrue(TEXT("array Sizes moved again"), Applied.ArraySizes > WindowEnd.ArraySizes);
		TestTrue(TEXT("array Childs moved again"), Applied.ArrayChilds > WindowEnd.ArrayChilds);
	}

	UIThread->EnqueueRemoveView(ViewId);
	RunFrames(*UIThread, 1);

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
