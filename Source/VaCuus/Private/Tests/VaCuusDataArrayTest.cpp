// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusDataVariable.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusModelLayout.h"
#include "VaCuusModelShadow.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "VaCuusModelLayoutTestTypes.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"

#include <atomic>
#include "UObject/UnrealType.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/DataModelHandle.h>

#if WITH_DEV_AUTOMATION_TESTS

/*
 * THE ARRAY ADAPTER (M3b Task 4), THROUGH A REAL Rml::Context: FVaCuusArrayDefinition's
 * whole contract -- {{Arr.size}}, data-for rows with it/it_index, static indexing into
 * struct rows, both named diagnostics, the non-array data-for diagnostic, the write
 * refusal (I3) -- and the statelessness the per-type registry makes load-bearing.
 *
 * Same rig as VaCuus.Model.Binding and for the same reason: the definitions may only be
 * built and evaluated on the real UI thread (FVaCuusDefinitionRegistry asserts it), so the
 * probe host drives phases on that thread and the test thread reads the captures after the
 * phase counter's release/acquire hand-off. Only the driving is a rig; the layout, the
 * shadow, the definitions, the registry, the bind and the Rml::Context are the real ones.
 */
namespace VaCuusDataArrayTest
{
/** The model name in the documents' `data-model` attribute. */
static const char* GModelName = "feed";

/**
 * Everything either document exposes, read after one Context::Update(). Attributes for
 * single values (readable without a font engine, exactly as VaCuus.Model.Binding argues);
 * InnerRML for data-for rows, where the substituted text is the row's content.
 */
struct FCaptured
{
	FString Count;
	FString K0;
	FString K2;
	FString Oob;
	FString Miss;
	FString SizeRow;
	FString SizeMiss;
	FString SizeCount;
	FString N0;
	TArray<FString> Rows;
	TArray<FString> ScalarRows;
	TArray<FString> StructRows;
	TArray<FString> PanelRows;
};

/**
 * A real Rml::Context on the real UI thread, running the real bind over a
 * FVaCuusArrayBindModel shadow, with test-authored actions run as numbered phases.
 *
 * THREAD HAND-OFF: everything below is configured on the test thread BEFORE
 * EnqueueAddView and immutable after; phase results are plain members written on the UI
 * thread and read on the test thread only after CompletedPhase (release) was seen
 * (acquire) -- the VaCuus.Model.Binding pattern, verbatim.
 */
class FArrayProbeHost final : public IVaCuusDocumentHost
{
public:
	explicit FArrayProbeHost(const TCHAR* InContextPrefix)
		: ContextPrefix(InContextPrefix)
	{
	}

	virtual bool Initialize(uint32 InViewId, const TSharedRef<FVaCuusViewStatus>& InStatus) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		Status = InStatus;
		ContextName = FString::Printf(TEXT("%s_%u"), *ContextPrefix, InViewId);

		Context = Rml::CreateContext(Rml::String(TCHAR_TO_UTF8(*ContextName)), Rml::Vector2i(1, 1));
		if (Context == nullptr)
		{
			return false;
		}

		Layout = FVaCuusModelLayout(FVaCuusArrayBindModel::StaticStruct());
		Shadow = FVaCuusModelShadow(FVaCuusArrayBindModel::StaticStruct());
		if (Seed)
		{
			Seed(Model());
		}

		if (bProbeRegistry)
		{
			// THE SHARING PROOF, taken while the registry is reachable (UI thread only).
			// Building this model's definitions registers THREE types: the model and its two
			// row types -- struct elements live in the ELEMENT type's own set, keyed on the
			// element UScriptStruct. A second model over the same row type then costs one
			// entry (itself), and the row type looked up directly is a pure cache hit: one
			// definition set per row type, however many models share it, which is the fact
			// that makes the array definition's statelessness load-bearing.
			RegistryNumBefore = FVaCuusDefinitionRegistry::Num();
			Definitions = FVaCuusDefinitionRegistry::GetOrCreate(Layout);
			RegistryNumAfterFirst = FVaCuusDefinitionRegistry::Num();
			SecondLookup = FVaCuusDefinitionRegistry::GetOrCreate(Layout);
			RegistryNumAfterSecond = FVaCuusDefinitionRegistry::Num();

			const FVaCuusModelLayout SharingLayout(FVaCuusArrayTestModel::StaticStruct());
			FVaCuusDefinitionRegistry::GetOrCreate(SharingLayout);
			RegistryNumAfterSharing = FVaCuusDefinitionRegistry::Num();

			const FVaCuusModelLayout RowLayout(FVaCuusTestKillfeedRow::StaticStruct());
			RowLookup = FVaCuusDefinitionRegistry::GetOrCreate(RowLayout);
			RegistryNumAfterRow = FVaCuusDefinitionRegistry::Num();
		}

		// BEFORE LoadDocument: `data-model` is read exactly once, in Element::SetParent
		// (Element.cpp:2203-2219), with no retry.
		Rml::DataModelConstructor Constructor = Context->CreateDataModel(GModelName);
		if (!Constructor)
		{
			return false;
		}

		ModelHandle = Constructor.GetModelHandle();
		NumBound = VaCuusData::BindModelVariables(Constructor, Layout, Shadow);

		return true;
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

		// The context goes first: RmlUi holds a raw void* into the shadow and there is no
		// unbind API. Same ordering argument as VaCuus.Model.Binding's host.
		Shadow.Reset();
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
			Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(*RmlSource)), "vacuus://data_array.rml");
		if (NewDocument == nullptr)
		{
			Report(LoadSerial, /*bSuccess=*/false);
			return;
		}

		CloseDocument();
		RmlDocument = NewDocument;
		RmlDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document);
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

		// Phases are requested, not counted -- the enqueue-wake coalescing argument at
		// VaCuusDataVariableTest's host applies unchanged.
		const int32 Requested = RequestedPhase.load(std::memory_order_acquire);
		if (Requested > CompletedPhase.load(std::memory_order_relaxed))
		{
			RunPhase(Requested);
			CompletedPhase.store(Requested, std::memory_order_release);
		}
		else
		{
			Context->Update();
		}

		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

	//~ Helpers for phase actions. UI thread only, like the actions themselves.

	/**
	 * The shadow AS the native fixture type. Legal because the shadow really is an
	 * initialised instance of StaticStruct() -- InitializeStruct memzeroes and then runs
	 * the C++ constructor (Class.cpp:3783, :3798), and the buffer is aligned to the type's
	 * MinAlignment (FVaCuusModelShadow) -- so this is the sampler tests' native-access
	 * shortcut taken one step further.
	 */
	FVaCuusArrayBindModel& Model()
	{
		check(FVaCuusUIThread::IsInUIThread());
		return *static_cast<FVaCuusArrayBindModel*>(Shadow.GetData());
	}

	void Dirty(const char* Name) { ModelHandle.DirtyVariable(Name); }
	void UpdateContext() { Context->Update(); }
	Rml::ElementDocument* GetDocument() { return RmlDocument; }
	const void* GetShadowData() const { return Shadow.GetData(); }
	int32 GetShadowSize() const { return Shadow.GetStruct()->GetStructureSize(); }

	/** Clicks the RowIndex-th GENERATED row under ContainerId (the template is skipped). */
	void ClickRow(const char* ContainerId, int32 RowIndex)
	{
		check(FVaCuusUIThread::IsInUIThread());

		Rml::Element* Container = RmlDocument != nullptr ? RmlDocument->GetElementById(ContainerId) : nullptr;
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

	//~ Configured on the test thread BEFORE EnqueueAddView; immutable after.

	/** Fills the shadow inside Initialize(), before the bind. */
	TUniqueFunction<void(FVaCuusArrayBindModel&)> Seed;

	/** Actions[N] runs at the start of phase N, before the phase's Context::Update(). */
	TArray<TUniqueFunction<void(FArrayProbeHost&)>> Actions;

	/** The one phase bracketed by the evaluation counters; INDEX_NONE for none. */
	int32 CounterProbePhase = INDEX_NONE;

	bool bProbeRegistry = false;

	/** Test thread -> UI thread: the phase to run on the next recorded frame. */
	std::atomic<int32> RequestedPhase{0};

	/** UI thread -> test thread: the highest phase that has run. */
	std::atomic<int32> CompletedPhase{-1};

	//~ Post-phase observations; see the class comment for why plain members are safe.
	TArray<FCaptured> Captures;
	int32 NumBound = 0;
	int32 RegistryNumBefore = 0;
	int32 RegistryNumAfterFirst = 0;
	int32 RegistryNumAfterSecond = 0;
	int32 RegistryNumAfterSharing = 0;
	int32 RegistryNumAfterRow = 0;
	const FVaCuusModelDefinitions* Definitions = nullptr;
	const FVaCuusModelDefinitions* SecondLookup = nullptr;
	const FVaCuusModelDefinitions* RowLookup = nullptr;
	int32 ScalarGetsBefore = 0;
	int32 ScalarGetsAfter = 0;
	int32 ArraySizesBefore = 0;
	int32 ArraySizesAfter = 0;
	int32 ArrayChildsBefore = 0;
	int32 ArrayChildsAfter = 0;
	int32 RefusedSetsBefore = 0;
	int32 RefusedSetsAfter = 0;
	bool bShadowBytesIdentical = false;
	bool bKillerElementIntact = false;
	bool bNumberElementIntact = false;

private:
	void RunPhase(int32 Phase)
	{
		const bool bProbeCounters = (Phase == CounterProbePhase);
		if (bProbeCounters)
		{
			ScalarGetsBefore = VaCuusData::GetNumScalarGets();
			ArraySizesBefore = VaCuusData::GetNumArraySizes();
			ArrayChildsBefore = VaCuusData::GetNumArrayChilds();
		}

		if (Actions.IsValidIndex(Phase) && Actions[Phase])
		{
			Actions[Phase](*this);
		}

		// An action that already updated (the click phase must bracket its own Update) gets
		// a second, idle Update here -- which is exactly what an idle frame does anyway.
		Context->Update();

		if (bProbeCounters)
		{
			ScalarGetsAfter = VaCuusData::GetNumScalarGets();
			ArraySizesAfter = VaCuusData::GetNumArraySizes();
			ArrayChildsAfter = VaCuusData::GetNumArrayChilds();
		}

		Captures.SetNum(FMath::Max(Captures.Num(), Phase + 1));
		Captures[Phase] = Capture();
	}

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
		// (DataViewDefault.cpp:486-491) -- and rows are inserted BEFORE it (:523), so "every
		// child without data-for, in order" is exactly the rows, in row order.
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

	FCaptured Capture() const
	{
		FCaptured Out;
		Out.Count = Attribute("count");
		Out.K0 = Attribute("k0");
		Out.K2 = Attribute("k2");
		Out.Oob = Attribute("oob");
		Out.Miss = Attribute("miss");
		Out.SizeRow = Attribute("szrow");
		Out.SizeMiss = Attribute("szmiss");
		Out.SizeCount = Attribute("szcount");
		Out.N0 = Attribute("n0");
		Out.Rows = RowTexts("rows");
		Out.ScalarRows = RowTexts("scalarrows");
		Out.StructRows = RowTexts("structrows");
		Out.PanelRows = RowTexts("panelrows");
		return Out;
	}

	TSharedPtr<FVaCuusViewStatus> Status;
	FString ContextPrefix;
	FString ContextName;
	Rml::Context* Context = nullptr;
	Rml::ElementDocument* RmlDocument = nullptr;
	Rml::DataModelHandle ModelHandle;
	FVaCuusModelLayout Layout;
	FVaCuusModelShadow Shadow;
	FIntPoint ViewSize = FIntPoint::ZeroValue;
};

/**
 * Every adapter behaviour in one document. The data-for over Numbers carries the default
 * `it`/`it_index` aliases (DataViewDefault.cpp:462-466); the two non-array data-for
 * targets and the two probes that must MISS (an out-of-bounds index, a named non-`size`
 * child) each have a named diagnostic the test registers as expected.
 *
 * THE THREE size SPELLINGS ARE THE POINT OF THE SizeRows TRIO (spec 3.6, narrowed -- the
 * fixture comment carries the argument): SizeRows.size is the count, always;
 * SizeRows[0].size ROUTES to the element struct and misses there, because an element
 * top-level member named Size cannot exist under the shared-layout root rule; the
 * reachable spelling is the NESTED SizeRows[0].Panel.Size.
 *
 * THE ELEMENT WRITES GO THROUGH data-for ALIASES BECAUSE NOTHING ELSE CAN SPELL THEM. The
 * spec's `Items[0] = 3` form does not parse: an assignment TARGET is read by
 * VariableOrFunctionName, whose character set is letters, digits, '_' and '.' only
 * (DataExpression.cpp:315-331, :333), and Assignment() errors at the '['
 * (DataExpression.cpp:386-417) -- brackets exist only in r-value expressions. So the click
 * controllers live on the data-for templates, are copied onto every generated row
 * (DataViewDefault.cpp:486-491) and instantiate per row (ElementUtilities.cpp:439-448
 * cancels them only on the template itself), and the alias resolves to the clicked row's
 * element at event time -- which is also the only spelling a real document would use.
 */
static const TCHAR* GDocumentMain = TEXT(R"(<rml>
<head><style>body { display: block; } div { display: block; }</style></head>
<body data-model="feed">
	<div id="count"   data-attr-p="Numbers.size"/>
	<div id="rows"><div data-for="Numbers">{{ it }}:{{ it_index }}</div></div>
	<div id="k0"      data-attr-p="Killfeed[0].Killer"/>
	<div id="k2"      data-attr-p="Killfeed[2].Killer"/>
	<div id="oob"     data-attr-p="Numbers[9]"/>
	<div id="miss"    data-attr-p="Labels.first"/>
	<div id="szrow"   data-attr-p="SizeRows[0].Panel.Size"/>
	<div id="szmiss"  data-attr-p="SizeRows[0].size"/>
	<div id="szcount" data-attr-p="SizeRows.size"/>
	<div id="scalarrows"><div data-for="s : Scalar">X</div></div>
	<div id="structrows"><div data-for="q : Panel">Y</div></div>
	<div id="panelrows"><div data-for="i : Panel.Items">{{ i }}</div></div>
	<div id="killrows"><div data-for="kill : Killfeed" data-event-click="kill.Killer = 'hacked'">{{ kill.Killer }}</div></div>
	<div id="numrows"><div data-for="n : Numbers" data-event-click="n = 3">{{ n }}</div></div>
</body>
</rml>)");

/** The growth view: rows and count over Numbers, nothing else. */
static const TCHAR* GDocumentGrow = TEXT(R"(<rml>
<head><style>body { display: block; } div { display: block; }</style></head>
<body data-model="feed">
	<div id="count" data-attr-p="Numbers.size"/>
	<div id="rows"><div data-for="Numbers">{{ it }}:{{ it_index }}</div></div>
</body>
</rml>)");

/**
 * The passive view of the statelessness pair: it reads Numbers.size and Numbers[0] but
 * DELIBERATELY has no data-for over Numbers -- its rows iterate Labels. That asymmetry is
 * what makes the cached-Num restore-the-bug deterministic: this view's own update never
 * calls Size() on the Numbers definition, so a Num cached there by the OTHER view's
 * data-for has no chance to be refreshed before this view's Child("size") consumes it --
 * whereas a symmetric document would fail or pass with the intra-update view sort order.
 */
static const TCHAR* GDocumentPassive = TEXT(R"(<rml>
<head><style>body { display: block; } div { display: block; }</style></head>
<body data-model="feed">
	<div id="count" data-attr-p="Numbers.size"/>
	<div id="n0"    data-attr-p="Numbers[0]"/>
	<div id="rows"><div data-for="Labels">{{ it }}</div></div>
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

/** Asks a host for a phase and pumps frames until it reports the phase done. */
static bool RunPhase(FVaCuusUIThread& UIThread, FArrayProbeHost& Host, int32 Phase)
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
}	 // namespace VaCuusDataArrayTest

/**
 * FVaCuusArrayDefinition end to end: size, rows, indexing, every named diagnostic, and the
 * I3 refusal -- all through a real document over a real Rml::Context.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusArrayBindingTest, "VaCuus.Model.ArrayBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusArrayBindingTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusDataArrayTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no UI thread to drive"));
		return true;
	}

	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	// EVERY DIAGNOSTIC THE DOCUMENT PROVOKES, REGISTERED WITH ITS COUNT -- these are the
	// spec 8 "observed diagnostics": an expected message whose count is not met fails the
	// test (HasMetExpectedMessages, AutomationTest.cpp:1808-1840), so each line below is an
	// assertion that its branch fired, exactly once, i.e. that the latch latched. Counts of
	// 1 are meaningful because each latch is per definition object and this test's
	// definitions are built fresh (the UI thread starts and stops inside the test, and
	// FVaCuusUIThread::Exit() releases the registry). The names carry no 'F': a diagnostic
	// path starts with UScriptStruct::GetName(), which drops the prefix.
	AddExpectedMessagePlain(TEXT("'VaCuusArrayBindModel.Numbers' was indexed out of bounds ([9] with 2 elements)"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("'VaCuusArrayBindModel.Labels' is an array and has no child 'first'"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("'VaCuusTestSizeNameRow' has no member 'size'"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("data-for over 'VaCuusArrayBindModel.Scalar', which is not an array"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("data-for over 'VaCuusArrayBindModel.Panel', which is a struct, not an array"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("refused a document write to 'VaCuusTestKillfeedRow.Killer'"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("refused a document write to 'VaCuusArrayBindModel.Numbers[]'"), ELogVerbosity::Warning,
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

	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();

	TUniquePtr<FArrayProbeHost> OwnedHost = MakeUnique<FArrayProbeHost>(TEXT("vacuus_array_view"));
	FArrayProbeHost* Host = OwnedHost.Get();

	Host->bProbeRegistry = true;
	Host->Seed = [](FVaCuusArrayBindModel& Model)
	{
		Model.Numbers = {1, 4};
		Model.Labels = {TEXT("a")};
		Model.Killfeed.SetNum(3);
		Model.Killfeed[0].Killer = TEXT("K0");
		Model.Killfeed[1].Killer = TEXT("K1");
		Model.Killfeed[2].Killer = TEXT("K2");
		Model.SizeRows.SetNum(1);
		Model.SizeRows[0].Kept = TEXT("sz");
		Model.SizeRows[0].Panel.Size = 42;
		Model.Panel.Items = {5, 6};
	};

	// Phase 1: one appended element, bracketed by the evaluation counters -- the spec 3.5
	// counters' first live assertion (their exact-zero idle use is Task 5's).
	Host->CounterProbePhase = 1;
	Host->Actions.SetNum(3);
	Host->Actions[1] = [](FArrayProbeHost& Host)
	{
		Host.Model().Numbers.Add(7);
		Host.Dirty("Numbers");
	};

	// Phase 2: the I3 refusal, byte-compared. Each click's assignment reaches
	// VariableDefinition::Set with no VaCuus code in between -- through the row alias, the
	// one spelling the assignment grammar admits (GDocumentMain's comment) -- and both RmlUi
	// call sites skip their DirtyVariable when Set refuses (DataControllerDefault.cpp:57-59,
	// DataExpression.cpp:1185-1197), so the DOM must not move either.
	Host->Actions[2] = [](FArrayProbeHost& Host)
	{
		Host.RefusedSetsBefore = VaCuusData::GetNumRefusedSets();

		TArray<uint8> Before;
		Before.Append(static_cast<const uint8*>(Host.GetShadowData()), Host.GetShadowSize());

		// THE TWO WRITE TARGETS, READ NATIVELY BEFORE THE CLICKS -- because the Memcmp below
		// cannot see them. Both refused assignments aim at HEAP memory: 'kill.Killer' resolves
		// through the alias to GetRawPtr(0) inside Killfeed's element allocation, 'n = 3' into
		// Numbers' -- while the compared span holds only each TArray's inline header. A Set()
		// that wrote the element and then returned false would leave that compare green.
		const FString KillerBefore = Host.Model().Killfeed[0].Killer;
		const int32 NumberBefore = Host.Model().Numbers[0];

		Host.ClickRow("killrows", 0);	  // kill.Killer = 'hacked' -> the row leaf's scalar Set
		Host.ClickRow("numrows", 0);	  // n = 3 -> the scalar element definition's Set
		Host.UpdateContext();

		Host.RefusedSetsAfter = VaCuusData::GetNumRefusedSets();
		Host.bShadowBytesIdentical = FMemory::Memcmp(Before.GetData(), Host.GetShadowData(), Host.GetShadowSize()) == 0;

		// Case-SENSITIVE, because FString::operator== folds case (ESearchCase::IgnoreCase) and
		// a corruption that only changed case would slip through it.
		Host.bKillerElementIntact = Host.Model().Killfeed[0].Killer.Equals(KillerBefore, ESearchCase::CaseSensitive);
		Host.bNumberElementIntact = Host.Model().Numbers[0] == NumberBefore;
	};

	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), FIntPoint(400, 300), Status);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GDocumentMain, /*LoadSerial=*/1);

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

	// ---- 1. The bind and the registry sharing. ----

	TestEqual(TEXT("every top-level name was bound, arrays included"), Host->NumBound, 6);
	TestEqual(TEXT("building the model registered it and its two row types"), Host->RegistryNumAfterFirst, Host->RegistryNumBefore + 3);
	TestEqual(TEXT("the second lookup was a cache hit"), Host->RegistryNumAfterSecond, Host->RegistryNumAfterFirst);
	TestTrue(TEXT("returning the same definitions"), Host->SecondLookup == Host->Definitions);
	TestEqual(TEXT("a second model over the same row type costs one entry: itself"), Host->RegistryNumAfterSharing,
		Host->RegistryNumAfterFirst + 1);
	TestEqual(TEXT("and the row type looked up directly is a pure cache hit"), Host->RegistryNumAfterRow,
		Host->RegistryNumAfterSharing);
	if (TestNotNull(TEXT("whose definitions exist"), Host->RowLookup))
	{
		TestTrue(TEXT("keyed on the row struct"), Host->RowLookup->GetStruct() == FVaCuusTestKillfeedRow::StaticStruct());
	}

	// ---- 2. The initial values (spec 8's adapter rows). ----

	if (!TestTrue(TEXT("phase 0 captured"), Host->Captures.IsValidIndex(0)))
	{
		return false;
	}
	const FCaptured& Initial = Host->Captures[0];

	// {{Numbers.size}} through the array's Child("size") -> MakeLiteralIntVariable.
	TestEqual(TEXT("{{Numbers.size}} renders the element count"), Initial.Count, FString(TEXT("2")));

	// data-for with the default aliases: {{it}} is the element, {{it_index}} the frozen
	// creation index (DataViewDefault.cpp:513-521).
	if (TestEqual(TEXT("one row per element"), Initial.Rows.Num(), 2))
	{
		TestEqual(TEXT("row 0 renders value and index"), Initial.Rows[0], FString(TEXT("1:0")));
		TestEqual(TEXT("row 1 too"), Initial.Rows[1], FString(TEXT("4:1")));
	}

	// Static {{Killfeed[2].Killer}}: array Child({2}) -> the row TYPE's root struct
	// definition over GetRawPtr(2) -> its own property definition -> scalar Get.
	TestEqual(TEXT("a static index into a struct row resolves"), Initial.K2, FString(TEXT("K2")));
	TestEqual(TEXT("its sibling row too"), Initial.K0, FString(TEXT("K0")));

	// The two misses: named Warnings (registered above), empty values, no crash.
	TestEqual(TEXT("an out-of-bounds index reads empty"), Initial.Oob, FString());
	TestEqual(TEXT("a named non-'size' child reads empty"), Initial.Miss, FString());

	// The three `size` spellings -- spec 3.6, narrowed (the fixture comment carries which
	// sentence and why): the count always, the element route misses, the nested member is
	// the reachable spelling.
	TestEqual(TEXT("Arr.size is the count even when a row carries a Size of its own"), Initial.SizeCount, FString(TEXT("1")));
	TestEqual(TEXT("Arr[0].size routes to the element struct and misses there"), Initial.SizeMiss, FString());
	TestEqual(TEXT("Arr[0].Panel.Size is the reachable spelling"), Initial.SizeRow, FString(TEXT("42")));

	// data-for over a non-array: 0 rows and the named Size() diagnostic (registered above),
	// for both overriding definitions.
	TestEqual(TEXT("data-for over a scalar leaf yields no rows"), Initial.ScalarRows.Num(), 0);
	TestEqual(TEXT("data-for over a struct yields no rows"), Initial.StructRows.Num(), 0);

	// An array nested inside a struct (Panel.Items) iterates like any other.
	if (TestEqual(TEXT("a nested array's rows"), Initial.PanelRows.Num(), 2))
	{
		TestEqual(TEXT("with its values"), Initial.PanelRows[0], FString(TEXT("5")));
		TestEqual(TEXT("in order"), Initial.PanelRows[1], FString(TEXT("6")));
	}

	// ---- 3. Growth tracks, and the counters moved. ----

	if (!TestTrue(TEXT("the growth phase ran"), RunPhase(*UIThread, *Host, 1)))
	{
		return false;
	}
	const FCaptured& Grown = Host->Captures[1];

	TestEqual(TEXT("{{Numbers.size}} tracks growth"), Grown.Count, FString(TEXT("3")));
	if (TestEqual(TEXT("a row appeared for the appended element"), Grown.Rows.Num(), 3))
	{
		TestEqual(TEXT("with its value and index"), Grown.Rows[2], FString(TEXT("7:2")));
		TestEqual(TEXT("and the existing rows untouched"), Grown.Rows[0], FString(TEXT("1:0")));
	}

	// The spec 3.5 counters are alive: a dirty array evaluates through Size (data-for),
	// Child (rows, the size text) and the scalar Get (each row's {{it}}).
	TestTrue(TEXT("scalar Gets were counted"), Host->ScalarGetsAfter > Host->ScalarGetsBefore);
	TestTrue(TEXT("array Sizes were counted"), Host->ArraySizesAfter > Host->ArraySizesBefore);
	TestTrue(TEXT("array Childs were counted"), Host->ArrayChildsAfter > Host->ArrayChildsBefore);

	// ---- 4. The I3 refusal, byte-identical shadow. ----

	if (!TestTrue(TEXT("the click phase ran"), RunPhase(*UIThread, *Host, 2)))
	{
		return false;
	}
	const FCaptured& AfterClicks = Host->Captures[2];

	TestEqual(TEXT("both assignments reached Set() and were refused"), Host->RefusedSetsAfter, Host->RefusedSetsBefore + 2);
	// The byte-compare's scope is the shadow's INLINE span only -- scalar fields and the
	// TArray headers; the element values the clicks aimed at live in heap blocks it never
	// touches, and the two element reads below are the assertions that guard those.
	TestTrue(TEXT("and the shadow's inline span is byte-identical"), Host->bShadowBytesIdentical);
	TestTrue(TEXT("the refused struct-row write left the element's value in place"), Host->bKillerElementIntact);
	TestTrue(TEXT("and the refused scalar-element write too"), Host->bNumberElementIntact);
	TestEqual(TEXT("the struct row's DOM did not move"), AfterClicks.K0, FString(TEXT("K0")));
	TestTrue(TEXT("nor did the scalar rows"), AfterClicks.Rows == Grown.Rows);

	UIThread->EnqueueRemoveView(ViewId);
	RunFrames(*UIThread, 1);

	return true;
}

/**
 * THE STATELESSNESS INVARIANT, MADE FALSIFIABLE (spec 8): two views over two instances of
 * one model type -- one definition set, per the registry -- updated interleaved, each
 * rendering its own data; and growth 0->200 in steps that cross container reallocation
 * boundaries, every value DOM-asserted after each step.
 *
 * The restore-the-bug this test exists for: cache a Num() member in
 * FVaCuusArrayDefinition (set in Size, used in Child) and the passive view renders the
 * OTHER view's count -- see GDocumentPassive's comment for why that failure is
 * deterministic here.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusArrayStatelessTest, "VaCuus.Model.ArrayStateless",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusArrayStatelessTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusDataArrayTest;

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

	const TSharedRef<FVaCuusViewStatus> GrowStatus = MakeShared<FVaCuusViewStatus>();
	const TSharedRef<FVaCuusViewStatus> PassiveStatus = MakeShared<FVaCuusViewStatus>();

	TUniquePtr<FArrayProbeHost> OwnedGrow = MakeUnique<FArrayProbeHost>(TEXT("vacuus_array_grow"));
	TUniquePtr<FArrayProbeHost> OwnedPassive = MakeUnique<FArrayProbeHost>(TEXT("vacuus_array_passive"));
	FArrayProbeHost* Grow = OwnedGrow.Get();
	FArrayProbeHost* Passive = OwnedPassive.Get();

	// The growth view starts EMPTY; each step extends the SAME live container, so the walk
	// 0 -> 1 -> 5 -> 64 -> 200 crosses the default allocator's slack boundaries and the
	// element block moves under the bound definitions -- which must not matter, because no
	// stage stores an element address (spec 2(c)).
	const auto GrowTo = [](int32 Target)
	{
		return [Target](FArrayProbeHost& Host)
		{
			TArray<int32>& Numbers = Host.Model().Numbers;
			for (int32 Index = Numbers.Num(); Index < Target; ++Index)
			{
				Numbers.Add(Index * 3 + 1);
			}
			Host.Dirty("Numbers");
		};
	};

	Grow->Actions.SetNum(6);
	Grow->Actions[1] = GrowTo(1);
	Grow->Actions[2] = GrowTo(5);
	Grow->Actions[3] = GrowTo(64);
	Grow->Actions[4] = GrowTo(200);
	Grow->Actions[5] = [](FArrayProbeHost& Host)
	{
		Host.Model().Numbers[0] = 999;
		Host.Dirty("Numbers");
	};

	Passive->Seed = [](FVaCuusArrayBindModel& Model)
	{
		Model.Numbers = {10, 20};
		Model.Labels = {TEXT("x"), TEXT("y"), TEXT("z")};
	};

	// The passive phases change NOTHING; they only dirty, forcing a re-evaluation AFTER the
	// growth view's latest Size() calls -- the exact moment a cached Num would leak across.
	Passive->Actions.SetNum(4);
	for (int32 Phase = 1; Phase <= 3; ++Phase)
	{
		Passive->Actions[Phase] = [](FArrayProbeHost& Host) { Host.Dirty("Numbers"); };
	}

	const uint32 GrowViewId = UIThread->AllocateViewId();
	const uint32 PassiveViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(GrowViewId, MoveTemp(OwnedGrow), FIntPoint(400, 300), GrowStatus);
	UIThread->EnqueueLoadDocumentFromMemory(GrowViewId, GDocumentGrow, /*LoadSerial=*/1);
	UIThread->EnqueueAddView(PassiveViewId, MoveTemp(OwnedPassive), FIntPoint(400, 300), PassiveStatus);
	UIThread->EnqueueLoadDocumentFromMemory(PassiveViewId, GDocumentPassive, /*LoadSerial=*/1);

	// Checks one growth step's capture in full: the count text, the row count, every row's
	// value and frozen index.
	const auto CheckStep = [&](int32 Phase, int32 ExpectedNum) -> bool
	{
		if (!TestTrue(FString::Printf(TEXT("growth phase %d captured"), Phase), Grow->Captures.IsValidIndex(Phase)))
		{
			return false;
		}

		const FCaptured& Cap = Grow->Captures[Phase];
		TestEqual(FString::Printf(TEXT("count at %d elements"), ExpectedNum), Cap.Count, FString::FromInt(ExpectedNum));
		if (!TestEqual(FString::Printf(TEXT("rows at %d elements"), ExpectedNum), Cap.Rows.Num(), ExpectedNum))
		{
			return false;
		}

		for (int32 Index = 0; Index < ExpectedNum; ++Index)
		{
			const FString Expected = FString::Printf(TEXT("%d:%d"), Index * 3 + 1, Index);
			if (Cap.Rows[Index] != Expected)
			{
				AddError(FString::Printf(
					TEXT("at %d elements, row %d shows '%s', expected '%s'"), ExpectedNum, Index, *Cap.Rows[Index], *Expected));
				return false;
			}
		}

		return true;
	};

	// The passive view's whole contract, asserted identically at every interleave point.
	const auto CheckPassive = [&](int32 Phase) -> bool
	{
		if (!TestTrue(FString::Printf(TEXT("passive phase %d captured"), Phase), Passive->Captures.IsValidIndex(Phase)))
		{
			return false;
		}

		const FCaptured& Cap = Passive->Captures[Phase];
		TestEqual(FString::Printf(TEXT("the passive view renders ITS OWN count (phase %d)"), Phase), Cap.Count,
			FString(TEXT("2")));
		TestEqual(FString::Printf(TEXT("and its own element (phase %d)"), Phase), Cap.N0, FString(TEXT("10")));
		if (TestEqual(FString::Printf(TEXT("and its own rows (phase %d)"), Phase), Cap.Rows.Num(), 3))
		{
			TestEqual(TEXT("x"), Cap.Rows[0], FString(TEXT("x")));
			TestEqual(TEXT("y"), Cap.Rows[1], FString(TEXT("y")));
			TestEqual(TEXT("z"), Cap.Rows[2], FString(TEXT("z")));
		}

		return true;
	};

	// ---- Interleaved from the first frame on. ----

	if (!TestTrue(TEXT("initial phases ran"),
			RunPhase(*UIThread, *Grow, 0) && RunPhase(*UIThread, *Passive, 0)))
	{
		return false;
	}
	TestEqual(TEXT("the growth view starts empty"), Grow->Captures[0].Count, FString(TEXT("0")));
	TestEqual(TEXT("with no rows"), Grow->Captures[0].Rows.Num(), 0);
	CheckPassive(0);

	if (!TestTrue(TEXT("step 1 ran"), RunPhase(*UIThread, *Grow, 1) && RunPhase(*UIThread, *Passive, 1)))
	{
		return false;
	}
	CheckStep(1, 1);
	CheckPassive(1);

	if (!TestTrue(TEXT("steps 5..200 ran"),
			RunPhase(*UIThread, *Grow, 2) && RunPhase(*UIThread, *Grow, 3) && RunPhase(*UIThread, *Grow, 4)))
	{
		return false;
	}
	CheckStep(2, 5);
	CheckStep(3, 64);
	CheckStep(4, 200);

	// Re-evaluated AFTER the growth view sized 200 elements: the sharpest moment for a
	// stale-Num leak, and the exact assertion the restore-the-bug run must break.
	if (!TestTrue(TEXT("the passive re-check ran"), RunPhase(*UIThread, *Passive, 2)))
	{
		return false;
	}
	CheckPassive(2);

	// One element changes in the grown view; the passive view re-evaluates after it.
	if (!TestTrue(TEXT("the mutation ran"), RunPhase(*UIThread, *Grow, 5) && RunPhase(*UIThread, *Passive, 3)))
	{
		return false;
	}
	if (TestEqual(TEXT("the mutated view still has all rows"), Grow->Captures[5].Rows.Num(), 200))
	{
		TestEqual(TEXT("with the mutation in place"), Grow->Captures[5].Rows[0], FString(TEXT("999:0")));
		TestEqual(TEXT("and its neighbour untouched"), Grow->Captures[5].Rows[1], FString(TEXT("4:1")));
	}
	CheckPassive(3);

	UIThread->EnqueueRemoveView(GrowViewId);
	UIThread->EnqueueRemoveView(PassiveViewId);
	RunFrames(*UIThread, 1);

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
