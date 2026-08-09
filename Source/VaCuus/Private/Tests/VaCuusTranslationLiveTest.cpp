// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusBoundModel.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusTestDocumentHost.h"
#include "VaCuusTranslation.h"
#include "VaCuusTranslationVariable.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "VaCuusModelLayoutTestTypes.h"

#include "HAL/PlatformProcess.h"
#include "Misc/ScopeExit.h"

#include <RmlUi/Core.h>

#if WITH_DEV_AUTOMATION_TESTS

/*
 * THE LIVE LOCALIZATION ROUTE (spec 2026-08-09 §1): a table pushed while documents are already
 * open re-translates their text IN PLACE, with no reload and therefore no lost state.
 *
 * The claim has three halves and each is asserted against a real Rml::Context on the real UI
 * thread:
 *
 *  1. `{{ t.key }}` shows the table's value, in a model-bound document and in a document that
 *     binds no struct at all (the standalone `vacuus` model -- a settings screen, the one place
 *     a language actually changes, very often binds nothing).
 *  2. A SECOND push changes the text while NumDocumentsLoaded stays at 1. That counter is what
 *     makes "no reload" an assertion rather than a claim: today's alternative,
 *     ClearAssetCachesAndReloadAllViews, would move it.
 *  3. Reserved-name collision: a struct field spelled `t` is refused for that field alone.
 *
 * RESTORE-THE-BUG, and it is one line: delete the model walk from the SetTranslationSnapshot
 * branch of FVaCuusUIThread::DrainCommands (or the VaCuusTranslationVariable::Dirty call inside
 * FVaCuusBoundModel::DirtyTranslations). Section 1 still passes -- the first evaluation reads
 * whatever table is installed by then -- and section 2 fails with the OLD strings still on
 * screen, which is exactly the bug this feature exists to prevent. Verified both ways.
 *
 * KEYS ARE PREFIXED `vt_` AND CHOSEN UNTRANSLATABLE BY ANY OTHER SUITE MEMBER, because the
 * translation table is process-wide and publish-by-replacement: suite order must not matter.
 */
namespace VaCuusTranslationLiveTest
{
/** Every id the probe reads back, in one place so document and probe cannot drift. */
static const TCHAR* GFlatId = TEXT("flat");
static const TCHAR* GDottedId = TEXT("dotted");
static const TCHAR* GMissingId = TEXT("missing");
static const TCHAR* GFieldId = TEXT("field");

/**
 * Reads the resolved inner RML of a fixed set of ids once per UI frame.
 *
 * INNER RML AND NOT A LAID-OUT TEXT RUN, the reason VaCuusModelTestHost.h already gives:
 * DataViewText::Update calls ElementText::SetText, and GetInnerRML appends the CURRENT text, so
 * the div's inner RML is the resolved value rather than the `{{ ... }}` source -- and it needs
 * no font, which keeps this test honest under -nullrhi.
 */
class FTextProbeHost final : public FVaCuusTestDocumentHost
{
public:
	FTextProbeHost(const TCHAR* InContextPrefix, TArray<FString> InIds)
		: FVaCuusTestDocumentHost(InContextPrefix, "vacuus://translation_live.rml", Rml::FocusFlag::Document)
		, Ids(MoveTemp(InIds))
	{
	}

	virtual void SetVisible(bool /*bVisible*/) override {}

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		Context->Update();

		Latest.Reset();
		if (RmlDocument != nullptr)
		{
			for (const FString& Id : Ids)
			{
				if (Rml::Element* Element = RmlDocument->GetElementById(TCHAR_TO_UTF8(*Id)))
				{
					Latest.Add(Id, FString(UTF8_TO_TCHAR(Element->GetInnerRML().c_str())));
				}
			}
		}

		// UI-THREAD-ONLY ACCESSORS, so they are sampled here rather than from the test thread —
		// the same rule VaCuusDataForTest's frame record follows for the evaluation counters.
		NumDirties = VaCuusTranslationVariable::GetNumDirties();
		NumInternedKeys = VaCuusTranslationVariable::GetNumInternedKeys();
		NumStandaloneModels = VaCuusTranslationVariable::GetNumStandaloneModels();

		++NumRecordedFrames;
		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

	/** Id -> resolved inner RML, as of the last Context::Update(). */
	TMap<FString, FString> Latest;

	int32 NumRecordedFrames = 0;

	/** THE "NO RELOAD" OBSERVABLE: a reload adopts a new document and moves this. */
	int32 NumDocumentsLoaded = 0;

	uint64 NumDirties = 0;
	int32 NumInternedKeys = 0;
	int32 NumStandaloneModels = 0;

protected:
	virtual void OnDocumentAdopted() override { ++NumDocumentsLoaded; }

private:
	TArray<FString> Ids;
};

/** A game field and three translation shapes: flat key, dotted key, and a key with no entry. */
static const TCHAR* GBoundDocument = TEXT(R"(<rml>
<head><style>body { display: block; } div { display: block; }</style></head>
<body data-model="hud">
	<div id="field">{{Title}}</div>
	<div id="flat">{{ t.vt_health }}</div>
	<div id="dotted">{{ t.vt_menu.settings.title }}</div>
	<div id="missing">{{ t.vt_absent }}</div>
</body>
</rml>)");

/** No game model at all — this is what the standalone `vacuus` model exists for. */
static const TCHAR* GStandaloneDocument = TEXT(R"(<rml>
<head><style>body { display: block; } div { display: block; }</style></head>
<body data-model="vacuus">
	<div id="flat">{{ t.vt_health }}</div>
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

static FString Shown(const FTextProbeHost& Host, const TCHAR* Id)
{
	const FString* Found = Host.Latest.Find(Id);
	return Found != nullptr ? *Found : FString(TEXT("<no element>"));
}
}	 // namespace VaCuusTranslationLiveTest

/**
 * A language change reaches open documents without reloading them.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusTranslationLiveTest, "VaCuus.Translation.Live",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusTranslationLiveTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusTranslationLiveTest;

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

	// ---- 0. A table BEFORE any document, so the first evaluation has something to find. ----

	{
		TMap<FString, FString> Table;
		Table.Add(TEXT("vt_health"), TEXT("Health"));
		Table.Add(TEXT("vt_menu.settings.title"), TEXT("Settings"));
		FVaCuusTranslationRegistry::SetTable(Table, TEXT("en"));
	}

	const UScriptStruct* Type = FVaCuusSamplerDefaultsModel::StaticStruct();
	const TSharedRef<FVaCuusBoundModel> Model = MakeShared<FVaCuusBoundModel>(TEXT("hud"), Type);
	if (!TestTrue(TEXT("the model built"), Model->IsValid()))
	{
		return false;
	}

	const TArray<FString> BoundIds = {GFieldId, GFlatId, GDottedId, GMissingId};
	TUniquePtr<FTextProbeHost> OwnedBound = MakeUnique<FTextProbeHost>(TEXT("vacuus_translation_live"), BoundIds);
	TUniquePtr<FTextProbeHost> OwnedStandalone =
		MakeUnique<FTextProbeHost>(TEXT("vacuus_translation_live_standalone"), TArray<FString>{GFlatId});
	FTextProbeHost* Bound = OwnedBound.Get();
	FTextProbeHost* Standalone = OwnedStandalone.Get();

	const TSharedRef<FVaCuusViewStatus> BoundStatus = MakeShared<FVaCuusViewStatus>();
	const TSharedRef<FVaCuusViewStatus> StandaloneStatus = MakeShared<FVaCuusViewStatus>();

	const uint32 BoundViewId = UIThread->AllocateViewId();
	const uint32 StandaloneViewId = UIThread->AllocateViewId();

	// BIND BEFORE LOAD: `data-model` is read once, in Element::SetParent (Element.cpp:2203-2218),
	// and FIFO on a single-producer queue is what makes "enqueued before" mean "drained before".
	UIThread->EnqueueAddView(BoundViewId, MoveTemp(OwnedBound), FIntPoint(400, 300), BoundStatus);
	UIThread->EnqueueBindModel(BoundViewId, Model);
	UIThread->EnqueueLoadDocumentFromMemory(BoundViewId, GBoundDocument, /*LoadSerial=*/1);

	UIThread->EnqueueAddView(StandaloneViewId, MoveTemp(OwnedStandalone), FIntPoint(400, 300), StandaloneStatus);
	UIThread->EnqueueLoadDocumentFromMemory(StandaloneViewId, GStandaloneDocument, /*LoadSerial=*/1);

	FVaCuusSamplerDefaultsModel Live;
	Live.Title = TEXT("Alpha");
	Model->Sample(Type, &Live);
	Model->PublishPending();

	if (!TestTrue(TEXT("frames ran"), RunFrames(*UIThread, 3)))
	{
		return false;
	}

	// ---- 1. Both routes resolve, and they coexist with the game's own variables. ----

	TestEqual(TEXT("the game's own variable still resolves in the same model"), Shown(*Bound, GFieldId), FString(TEXT("Alpha")));
	TestEqual(TEXT("a flat key translates"), Shown(*Bound, GFlatId), FString(TEXT("Health")));
	TestEqual(TEXT("a DOTTED key translates (ParseAddress splits on '.', so Child() ran three times)"),
		Shown(*Bound, GDottedId), FString(TEXT("Settings")));
	TestEqual(TEXT("a key with no entry renders as ITSELF — identity, the contract the other two readers keep"),
		Shown(*Bound, GMissingId), FString(TEXT("vt_absent")));
	TestEqual(TEXT("the standalone `vacuus` model serves a document that binds no struct"),
		Shown(*Standalone, GFlatId), FString(TEXT("Health")));
	TestEqual(TEXT("...and there is one standalone model per view"), Standalone->NumStandaloneModels, 2);

	const int32 DocumentsLoadedBefore = Bound->NumDocumentsLoaded;
	TestEqual(TEXT("one document has been loaded so far"), DocumentsLoadedBefore, 1);

	// ---- 2. THE CLAIM: a second push changes open documents, with no reload. ----

	{
		TMap<FString, FString> Table;
		Table.Add(TEXT("vt_health"), TEXT("Здоровье"));
		Table.Add(TEXT("vt_menu.settings.title"), TEXT("Настройки"));
		FVaCuusTranslationRegistry::SetTable(Table, TEXT("ru"));
	}

	if (!TestTrue(TEXT("frames ran after the push"), RunFrames(*UIThread, 2)))
	{
		return false;
	}

	TestEqual(TEXT("the flat key now shows the NEW language"), Shown(*Bound, GFlatId), FString(TEXT("Здоровье")));
	TestEqual(TEXT("the dotted key too"), Shown(*Bound, GDottedId), FString(TEXT("Настройки")));
	TestEqual(TEXT("and so does the document with no game model"), Shown(*Standalone, GFlatId), FString(TEXT("Здоровье")));

	// THE POINT OF THE WHOLE FEATURE, in one assertion: nothing reloaded, so nothing was reset.
	TestEqual(TEXT("NO DOCUMENT WAS RELOADED — the text changed in place"),
		Bound->NumDocumentsLoaded, DocumentsLoadedBefore);
	TestEqual(TEXT("the game's own variable was untouched by the language change"),
		Shown(*Bound, GFieldId), FString(TEXT("Alpha")));

	// ---- 3. Replacement, not merge, reaches the live route too. ----

	{
		TMap<FString, FString> Table;
		Table.Add(TEXT("vt_health"), TEXT("Vida"));
		FVaCuusTranslationRegistry::SetTable(Table, TEXT("es"));
	}

	if (!TestTrue(TEXT("frames ran after the replacement"), RunFrames(*UIThread, 2)))
	{
		return false;
	}

	TestEqual(TEXT("the surviving key shows its new value"), Shown(*Bound, GFlatId), FString(TEXT("Vida")));
	TestEqual(TEXT("the DROPPED key falls back to identity, live, without a reload"),
		Shown(*Bound, GDottedId), FString(TEXT("vt_menu.settings.title")));

	// ---- 4. The observables. ----

	TestTrue(TEXT("models were dirtied by the pushes"), Bound->NumDirties >= 3);
	TestEqual(TEXT("the interned pool holds exactly the DISTINCT key paths the documents evaluate ")
			  TEXT("(vt_health, vt_menu, vt_menu.settings, vt_menu.settings.title, vt_absent)"),
		Bound->NumInternedKeys, 5);

	return true;
}

/**
 * `t` is reserved: the field that collides is refused, and only that field.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusTranslationReservedNameTest, "VaCuus.Translation.ReservedName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusTranslationReservedNameTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusTranslationLiveTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no UI thread to drive"));
		return true;
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

	{
		TMap<FString, FString> Table;
		Table.Add(TEXT("vt_reserved"), TEXT("FromTable"));
		FVaCuusTranslationRegistry::SetTable(Table, TEXT("en"));
	}

	// The bind logs a named Error for the colliding field. Expected, so the suite does not fail
	// on it — and asserted, because "it was refused loudly" is half the claim.
	AddExpectedError(TEXT("collides with the RESERVED translation variable"), EAutomationExpectedErrorFlags::Contains, 1);

	const UScriptStruct* Type = FVaCuusReservedNameModel::StaticStruct();
	const TSharedRef<FVaCuusBoundModel> Model = MakeShared<FVaCuusBoundModel>(TEXT("hud"), Type);
	if (!TestTrue(TEXT("the model built"), Model->IsValid()))
	{
		return false;
	}

	static const TCHAR* GCollisionDocument = TEXT(R"(<rml>
<head><style>body { display: block; } div { display: block; }</style></head>
<body data-model="hud">
	<div id="field">{{Title}}</div>
	<div id="flat">{{ t.vt_reserved }}</div>
</body>
</rml>)");

	const TArray<FString> Ids = {GFieldId, GFlatId};
	TUniquePtr<FTextProbeHost> Owned = MakeUnique<FTextProbeHost>(TEXT("vacuus_translation_reserved"), Ids);
	FTextProbeHost* Probe = Owned.Get();
	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
	const uint32 ViewId = UIThread->AllocateViewId();

	UIThread->EnqueueAddView(ViewId, MoveTemp(Owned), FIntPoint(400, 300), Status);
	UIThread->EnqueueBindModel(ViewId, Model);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GCollisionDocument, /*LoadSerial=*/1);

	FVaCuusReservedNameModel Live;
	Live.t = TEXT("ShouldNotWin");
	Live.Title = TEXT("Bound");
	Model->Sample(Type, &Live);
	Model->PublishPending();

	if (!TestTrue(TEXT("frames ran"), RunFrames(*UIThread, 3)))
	{
		return false;
	}

	// TRANSLATION WINS, and that is the designed outcome: `t` is bound first, so the struct's
	// field never reaches RmlUi and cannot silently disable the live route for this model.
	TestEqual(TEXT("`{{ t.key }}` still translates despite a struct field spelled `t`"),
		Shown(*Probe, GFlatId), FString(TEXT("FromTable")));
	TestEqual(TEXT("every OTHER field of the same struct bound normally"),
		Shown(*Probe, GFieldId), FString(TEXT("Bound")));

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
