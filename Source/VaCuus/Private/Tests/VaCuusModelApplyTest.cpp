// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusBoundModel.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusModelLayout.h"
#include "VaCuusModelShadow.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "VaCuusModelLayoutTestTypes.h"

#include "HAL/PlatformProcess.h"
#include "Misc/ScopeExit.h"

#include <atomic>
#include "UObject/StrProperty.h"
#include "UObject/UnrealType.h"

#include <RmlUi/Core.h>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE UI-THREAD APPLY (M3a Task 6): for every published bit, copy the field into the UI
 * shadow and dirty its top-level name -- on every view, at the frame's DataApply phase,
 * before Context::Update().
 *
 * Three properties, and each has a restore-the-bug target in FVaCuusUIThread::RunFrame():
 *
 *  1. It runs at all, and the values reach a real DOM. Delete ApplyModelUpdates() from the
 *     DataApply scope and VaCuus.Model.Apply's `{{Title}}` assertion fails while everything
 *     else about the view keeps working -- which is the failure this milestone is built
 *     against, seen from the outside.
 *  2. It is NOT inside the per-view record loop. That loop is gated on HasView(), which
 *     additionally requires a non-degenerate view size -- and every UMG view fails it until
 *     its first Slate tick (UVaCuusWidget::RebuildWidget creates its view with
 *     FIntPoint::ZeroValue on purpose, VaCuusUMGWidget.cpp:70-78). The SIZELESS VIEW below is
 *     that case, made testable: move the apply into the record loop and its assertions fail
 *     while the sized view's all pass.
 *  3. An unchanging model costs nothing. Nothing is applied, nothing is dirtied, and the DOM
 *     is byte-identical for a hundred frames -- which is what spec 9's "idle -> 0 published
 *     frames" row reduces to on this side of the recorder. The render-side half of that row
 *     (FramesPublished actually stopping) is VaCuus.Model.View.Idle, in VaCuusRender, because
 *     the recorder that decides it is private to that module.
 *
 * What is production here and what is not: only the driving is a rig (the probe host, which
 * stands in for FVaCuusRmlDocumentHost so that this module can test without the recorder). The
 * layout, both shadows, the channel, the sampler, the bind, the apply, the UI thread and the
 * Rml::Context are the real ones.
 */
namespace VaCuusModelApplyTest
{
/** The model name in the document's `data-model` attribute. */
static const char* GModelName = "hud";

/**
 * `{{Title}}` is the end-to-end evidence -- a data expression in element text, resolved by
 * RmlUi's own DataViewText and read back out of the DOM. Health rides along through
 * `data-attr-p` because an attribute is readable without a laid-out text run, so a failure
 * tells the two apart.
 */
static const TCHAR* GDocument = TEXT(R"(<rml>
<head><style>body { display: block; } div { display: block; }</style></head>
<body data-model="hud">
	<div id="title">{{Title}}</div>
	<div id="health" data-attr-p="Health"/>
</body>
</rml>)");

/** What the document is showing, as of the last Context::Update(). */
struct FObserved
{
	FString Title;
	FString Health;

	bool operator==(const FObserved& Other) const { return Title == Other.Title && Health == Other.Health; }
	bool operator!=(const FObserved& Other) const { return !(*this == Other); }
};

/**
 * A real Rml::Context on the real UI thread, updated once per recorded frame.
 *
 * WHY A PROBE AND NOT FVaCuusRmlDocumentHost: that host lives in VaCuusRender along with the
 * recorder it needs, and VaCuusRender depends on VaCuus rather than the other way round --
 * FVaCuusBoundModel is a Private header of THIS module, so a test that constructs one cannot
 * live over there. The split is the same one IVaCuusDocumentHost exists for.
 *
 * THREAD HAND-OFF: plain members written on the UI thread, read on the test thread only after
 * WaitForFrameCount() saw the frame counter advance -- which the UI thread stores with release
 * ordering after RunFrame() returns. Same rule as VaCuus.Model.Binding's probe.
 */
class FProbeHost final : public IVaCuusDocumentHost
{
public:
	virtual bool Initialize(uint32 InViewId, const TSharedRef<FVaCuusViewStatus>& InStatus) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		Status = InStatus;
		ContextName = FString::Printf(TEXT("vacuus_apply_view_%u"), InViewId);
		Context = Rml::CreateContext(Rml::String(TCHAR_TO_UTF8(*ContextName)), Rml::Vector2i(1, 1));
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
			Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(*RmlSource)), "vacuus://apply.rml");
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

		// THE SAME SHAPE AS THE PRODUCTION HOST'S, and the size clause is the one that matters
		// here: FVaCuusRmlDocumentHost::HasView() also requires ViewSize.X > 0 && ViewSize.Y > 0,
		// so a view that has not been laid out yet is not recorded. The sizeless view in the test
		// below never satisfies this, which is exactly the case the apply must not be gated on.
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

		const FObserved Now = Capture();
		if (NumRecordedFrames == 0 || Now != Latest)
		{
			++NumDomChanges;
		}
		Latest = Now;
		++NumRecordedFrames;

		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

	//~ Post-frame observations; see the class comment for why plain members are safe.

	/** What the document is showing right now. */
	FObserved Latest;

	/**
	 * Frames whose DOM differed from the previous frame's (the first counts as one).
	 *
	 * THE IDLE ASSERTION'S OBSERVABLE. It stands in for the recorder's published-frame counter
	 * one level up: what makes an idle UI frame free is that nothing writes the DOM, so the
	 * command list -- and therefore the frame hash -- does not move. If a bound model dirtied a
	 * variable every frame, this would climb even though nothing changed.
	 */
	int32 NumDomChanges = 0;
	int32 NumRecordedFrames = 0;

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

	FObserved Capture() const
	{
		FObserved Out;
		if (RmlDocument == nullptr)
		{
			return Out;
		}

		// InnerRML, because that is where a `{{Field}}` substitution lands: DataViewText::Update
		// calls ElementText::SetText (DataViewDefault.cpp), and ElementText::GetRML appends the
		// CURRENT text (ElementText.cpp), so the div's inner RML is the resolved value rather
		// than the `{{Title}}` source.
		if (Rml::Element* TitleElement = RmlDocument->GetElementById("title"))
		{
			Out.Title = FString(UTF8_TO_TCHAR(TitleElement->GetInnerRML().c_str()));
		}
		if (Rml::Element* HealthElement = RmlDocument->GetElementById("health"))
		{
			Out.Health = FString(UTF8_TO_TCHAR(HealthElement->GetAttribute<Rml::String>("p", Rml::String()).c_str()));
		}
		return Out;
	}

	TSharedPtr<FVaCuusViewStatus> Status;
	FString ContextName;
	Rml::Context* Context = nullptr;
	Rml::ElementDocument* RmlDocument = nullptr;
	FIntPoint ViewSize = FIntPoint::ZeroValue;
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

/** One FString field, read out of any instance of the model type through the layout. */
static FString ReadString(const FVaCuusModelLayout& Layout, const FVaCuusModelShadow& Shadow, const TCHAR* WireName)
{
	const FVaCuusModelField* Field = Layout.FindField(WireName);
	const FStrProperty* Property = Field != nullptr ? CastField<FStrProperty>(Field->Property) : nullptr;
	return (Property != nullptr && Shadow.IsValid())
		? Property->GetPropertyValue_InContainer(Field->ContainerPtr(Shadow.GetData()))
		: FString();
}
}	 // namespace VaCuusModelApplyTest

/**
 * The apply, end to end and on both kinds of view: a game-thread sample reaches a real DOM
 * through the UI thread's DataApply phase, and a view that is not recordable gets its updates
 * anyway.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelApplyTest, "VaCuus.Model.Apply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelApplyTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelApplyTest;

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

	const UScriptStruct* Type = FVaCuusSamplerDefaultsModel::StaticStruct();

	// TWO VIEWS, AND THE SECOND IS THE POINT. Sized: a real document, recorded every frame.
	// Sizeless: created with FIntPoint::ZeroValue and never given a document, exactly like a
	// UMG view before its first Slate tick, so HasView() is false for its whole life.
	const TSharedRef<FVaCuusBoundModel> SizedModel = MakeShared<FVaCuusBoundModel>(FName(TEXT("hud")), Type);
	const TSharedRef<FVaCuusBoundModel> SizelessModel = MakeShared<FVaCuusBoundModel>(FName(TEXT("hud")), Type);
	if (!TestTrue(TEXT("both models built"), SizedModel->IsValid() && SizelessModel->IsValid()))
	{
		return false;
	}

	const TSharedRef<FVaCuusViewStatus> SizedStatus = MakeShared<FVaCuusViewStatus>();
	const TSharedRef<FVaCuusViewStatus> SizelessStatus = MakeShared<FVaCuusViewStatus>();

	TUniquePtr<FProbeHost> OwnedSized = MakeUnique<FProbeHost>();
	TUniquePtr<FProbeHost> OwnedSizeless = MakeUnique<FProbeHost>();
	FProbeHost* Sized = OwnedSized.Get();
	FProbeHost* Sizeless = OwnedSizeless.Get();

	const uint32 SizedViewId = UIThread->AllocateViewId();
	const uint32 SizelessViewId = UIThread->AllocateViewId();

	// BIND BEFORE LOAD, which is RmlUi's ordering requirement and not ours: `data-model` is
	// read once, in Element::SetParent (Element.cpp:2202-2219). FIFO on a single-producer
	// queue is what makes "enqueued before" mean "drained before".
	UIThread->EnqueueAddView(SizedViewId, MoveTemp(OwnedSized), FIntPoint(400, 300), SizedStatus);
	UIThread->EnqueueBindModel(SizedViewId, SizedModel);
	UIThread->EnqueueLoadDocumentFromMemory(SizedViewId, GDocument, /*LoadSerial=*/1);

	UIThread->EnqueueAddView(SizelessViewId, MoveTemp(OwnedSizeless), FIntPoint::ZeroValue, SizelessStatus);
	UIThread->EnqueueBindModel(SizelessViewId, SizelessModel);

	// ---- 1. One game-thread sample, published before the first UI frame. ----

	FVaCuusSamplerDefaultsModel Live;
	Live.Title = TEXT("Alpha");
	Live.Health = 42.f;

	SizedModel->Sample(Type, &Live);
	SizelessModel->Sample(Type, &Live);
	TestTrue(TEXT("the sized model published"), SizedModel->PublishPending());
	TestTrue(TEXT("the sizeless model published"), SizelessModel->PublishPending());

	if (!TestTrue(TEXT("frames ran"), RunFrames(*UIThread, 3)))
	{
		return false;
	}

	TestEqual(TEXT("both models were bound on the UI thread"), UIThread->GetNumBoundModels(), 2);

	// THE FIRST END-TO-END EVIDENCE: a `{{Field}}` in a real document, showing a value that
	// started life in a plain USTRUCT on the game thread and travelled layout -> differ ->
	// channel -> UI shadow -> DirtyVariable -> DataViewText.
	TestEqual(TEXT("{{Title}} shows the sampled value"), Sized->Latest.Title, FString(TEXT("Alpha")));
	TestEqual(TEXT("and a data-attr expression shows the sampled float"), FCString::Atof(*Sized->Latest.Health), 42.f);

	const int32 NumFields = SizedModel->GetLayout().GetFields().Num();
	TestEqual(TEXT("one update applied"), int32(SizedModel->GetNumUpdatesApplied()), 1);
	TestEqual(TEXT("carrying every field, because the channel is born fully dirty (spec 4 / I1)"),
		int32(SizedModel->GetNumFieldsApplied()), NumFields);

	// THE ECHO. ConsumeUpdate stores the applied generation itself, after the apply; the
	// producer reaps it here. Without it the channel would republish every field forever --
	// correctly, silently, and at a cost that only grows.
	TestEqual(TEXT("the applied generation was echoed back, so nothing is outstanding"),
		SizedModel->NumOutstandingFields(), 0);
	TestFalse(TEXT("...and a publish with nothing outstanding declines"), SizedModel->PublishPending());

	// ---- 2. The view that is NOT recordable gets its updates anyway (spec 3.6). ----

	TestEqual(TEXT("the sizeless view recorded no frames at all"), Sizeless->NumRecordedFrames, 0);
	TestEqual(TEXT("but its model was applied"), int32(SizelessModel->GetNumUpdatesApplied()), 1);
	TestEqual(TEXT("...into its UI shadow"), ReadString(SizelessModel->GetLayout(), SizelessModel->GetUIShadow(), TEXT("Title")),
		FString(TEXT("Alpha")));
	TestEqual(TEXT("...and echoed, so its channel is not backing up either"), SizelessModel->NumOutstandingFields(), 0);

	// ---- 3. A second, single-field change. ----

	const int32 DomChangesBeforeUpdate = Sized->NumDomChanges;

	Live.Title = TEXT("Beta");
	TestEqual(TEXT("exactly one field changed"), SizedModel->Sample(Type, &Live), 1);
	TestTrue(TEXT("and it published"), SizedModel->PublishPending());

	if (!TestTrue(TEXT("frames ran"), RunFrames(*UIThread, 3)))
	{
		return false;
	}

	TestEqual(TEXT("{{Title}} follows the change"), Sized->Latest.Title, FString(TEXT("Beta")));
	TestEqual(TEXT("and the untouched field is untouched"), FCString::Atof(*Sized->Latest.Health), 42.f);
	TestEqual(TEXT("two updates applied"), int32(SizedModel->GetNumUpdatesApplied()), 2);
	TestEqual(TEXT("and exactly one more field copied"), int32(SizedModel->GetNumFieldsApplied()), NumFields + 1);
	TestEqual(TEXT("the DOM moved exactly once for it"), Sized->NumDomChanges, DomChangesBeforeUpdate + 1);

	// ---- 4. A hundred frames with nothing changing (spec 9's idle row, this side of it). ----

	const int32 DomChangesBeforeIdle = Sized->NumDomChanges;
	const int32 FramesBeforeIdle = Sized->NumRecordedFrames;

	// The differ still runs every frame, exactly as a game would drive it; it just finds
	// nothing. That is the case the idle row is about -- "merely HAVING a model" -- rather
	// than "nobody called UpdateModel".
	for (int32 Frame = 0; Frame < 100; ++Frame)
	{
		TestEqual(TEXT("an unchanged struct marks no field"), SizedModel->Sample(Type, &Live), 0);
		TestFalse(TEXT("so there is nothing to publish"), SizedModel->PublishPending());
	}

	if (!TestTrue(TEXT("a hundred frames ran"), RunFrames(*UIThread, 100)))
	{
		return false;
	}

	TestTrue(TEXT("the view really did record those frames"), Sized->NumRecordedFrames >= FramesBeforeIdle + 100);
	TestEqual(TEXT("and the DOM did not move once in any of them"), Sized->NumDomChanges, DomChangesBeforeIdle);
	TestEqual(TEXT("no further update was applied"), int32(SizedModel->GetNumUpdatesApplied()), 2);
	TestEqual(TEXT("no further field was copied"), int32(SizedModel->GetNumFieldsApplied()), NumFields + 1);
	TestEqual(TEXT("and only two updates were ever published"), int32(SizedModel->GetNumPublishes()), 2);

	// The values are still there afterwards, so "nothing happened" is not "the model went away".
	TestEqual(TEXT("and the document still shows the last value"), Sized->Latest.Title, FString(TEXT("Beta")));

	UIThread->EnqueueRemoveView(SizedViewId);
	UIThread->EnqueueRemoveView(SizelessViewId);
	RunFrames(*UIThread, 1);

	TestEqual(TEXT("removing the views dropped both models"), UIThread->GetNumBoundModels(), 0);

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
