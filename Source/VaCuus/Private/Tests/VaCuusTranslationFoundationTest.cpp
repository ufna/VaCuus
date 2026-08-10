// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusTranslation.h"

#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/StringTableRegistry.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * THE GAME-THREAD HALF of the localization seam (spec 2026-08-09 §2, §4): the change signal a
 * game hangs its FText re-push on, and the bridge that makes UE's own String Table assets --
 * and therefore the project's .locres -- serve VaCuus with no glue.
 *
 * NO UI THREAD HERE ON PURPOSE. Both surfaces are pure game-thread state, and testing them
 * without booting RmlUi is what keeps the failure legible: a break in this file is a break in
 * the registry, never in a context or a document.
 */

namespace VaCuusTranslationFoundationTest
{
/**
 * FStringTable::SetSourceString's ARITY is engine-dependent, and no single #if states it.
 * 5.8 declares the three-parameter form (key, source, dev notes) under WITH_EDITORONLY_DATA and
 * the two-parameter form otherwise (StringTableCore.h:143-149, and the definition is the same
 * one function under that switch, StringTableCore.cpp:183-187). 5.6 declares ONLY the
 * two-parameter form, unconditionally and regardless of editor-only data (5.6
 * StringTableCore.h:123). So a WITH_EDITORONLY_DATA test -- which is what stood here -- is
 * right on 5.8 and wrong on a 5.6 editor build, which is how the 5.6 port found this.
 *
 * Overload ranking asks the compiler the question instead: the `int` candidate wins whenever
 * the three-argument call is well-formed, and the `long` one is the fallback. That states the
 * only fact anyone here has verified -- what each engine's header accepts -- and needs no
 * re-judging per release. Dev notes are empty either way; nothing in these tests reads them.
 * The monolithic game target compiles this file too (automation tests are on in Development),
 * so the branch has to live here rather than at each call.
 */
template <typename TTable>
auto SetSourceImpl(TTable& Table, const TCHAR* Key, const TCHAR* Value, int)
	-> decltype(Table->SetSourceString(Key, Value, FString()))
{
	return Table->SetSourceString(Key, Value, FString());
}

template <typename TTable>
void SetSourceImpl(TTable& Table, const TCHAR* Key, const TCHAR* Value, long)
{
	Table->SetSourceString(Key, Value);
}

inline void SetSource(FStringTableRef Table, const TCHAR* Key, const TCHAR* Value)
{
	SetSourceImpl(Table, Key, Value, 0);
}
}	 // namespace VaCuusTranslationFoundationTest

/**
 * The registry's change signal: it fires, it carries what was pushed, and it fires however the
 * table was pushed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusTranslationSignalTest, "VaCuus.Translation.Signal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusTranslationSignalTest::RunTest(const FString& Parameters)
{
	int32 NumFired = 0;
	FString LastTag;
	uint64 LastVersion = 0;

	FDelegateHandle Handle = FVaCuusTranslationRegistry::OnTableChanged().AddLambda(
		[&NumFired, &LastTag, &LastVersion](const FString& Tag, uint64 Version)
		{
			++NumFired;
			LastTag = Tag;
			LastVersion = Version;
		});

	// The registry outlives this test; a subscription left behind would fire into dead stack.
	ON_SCOPE_EXIT
	{
		FVaCuusTranslationRegistry::OnTableChanged().Remove(Handle);
	};

	const uint64 VersionBefore = FVaCuusTranslationRegistry::GetVersion_GameThread();

	{
		TMap<FString, FString> Table;
		Table.Add(TEXT("vf_hello"), TEXT("vf_privet"));
		FVaCuusTranslationRegistry::SetTable(Table, TEXT("ru"));
	}

	TestEqual(TEXT("the signal fired once"), NumFired, 1);
	TestEqual(TEXT("carrying the tag the pusher chose, uninterpreted"), LastTag, FString(TEXT("ru")));
	TestEqual(TEXT("and the version the snapshot got"), LastVersion, VersionBefore + 1);

	// THE ORDER THE HANDLER SEES: the snapshot is already the current one by the time it runs,
	// so a handler that reads back what it is reacting to gets the NEW table, not the old.
	FVaCuusTranslationRegistry::OnTableChanged().Remove(Handle);
	FString SeenDuringBroadcast;
	Handle = FVaCuusTranslationRegistry::OnTableChanged().AddLambda(
		[&SeenDuringBroadcast](const FString& /*Tag*/, uint64 /*Version*/)
		{
			const TSharedPtr<const FVaCuusTranslationSnapshot> Now = FVaCuusTranslationRegistry::GetSnapshot_GameThread();
			const FString* Found = Now.IsValid() ? Now->Table.Find(TEXT("vf_hello")) : nullptr;
			SeenDuringBroadcast = Found != nullptr ? *Found : FString(TEXT("<absent>"));
		});

	{
		TMap<FString, FString> Table;
		Table.Add(TEXT("vf_hello"), TEXT("vf_bonjour"));
		FVaCuusTranslationRegistry::SetTable(Table, TEXT("fr"));
	}

	TestEqual(TEXT("the handler sees the snapshot it is being told about, not the previous one"),
		SeenDuringBroadcast, FString(TEXT("vf_bonjour")));

	return true;
}

/**
 * The String Table bridge: keys and CULTURE-RESOLVED values, from an engine asset.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusTranslationStringTableTest, "VaCuus.Translation.StringTableBridge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusTranslationStringTableTest::RunTest(const FString& Parameters)
{
	// A real UStringTable, built in memory. Rooted for the test's lifetime because the bridge
	// reads it through FText::FromStringTable, which goes back through the global registry the
	// asset registers itself with in its constructor.
	TStrongObjectPtr<UStringTable> Asset(NewObject<UStringTable>(GetTransientPackage(), TEXT("VaCuusBridgeTestTable")));
	if (!TestTrue(TEXT("the string table asset was created"), Asset.IsValid()))
	{
		return false;
	}

	FStringTableRef Source = Asset->GetMutableStringTable();
	VaCuusTranslationFoundationTest::SetSource(Source, TEXT("vb_health"), TEXT("Health"));
	VaCuusTranslationFoundationTest::SetSource(Source, TEXT("vb_ammo"), TEXT("Ammo"));

	// REGISTERED BY HAND, ONLY IF THE ENGINE DID NOT ALREADY DO IT -- and that "if" is an
	// engine-version difference, found by the 5.6 port as a hard check() that killed the whole
	// automation run.
	//
	// 5.8 auto-registers a UStringTable in its constructor only when `IsAsset() &&
	// ShouldAutoRegister(...)` (StringTable.cpp:401), and an object in the transient package is
	// not an asset -- so this test has to stand in for the saved asset that would register
	// itself. 5.6's constructor guards on `!HasAnyFlags(RF_ClassDefaultObject)` instead (5.6
	// StringTable.cpp:356-360), so EVERY non-CDO table auto-registers, transient or not. On 5.6
	// the unconditional call below therefore re-registered an ID already in the map, and
	// FStringTableRegistry::RegisterStringTable checkf's exactly that ("String table ID '%s' is
	// already in use!", StringTableRegistry.cpp:71 -- byte-identical in both engines, which is
	// why the difference is in the CALLER and not in the registry).
	//
	// Asking the registry is the portable form, and it is the same question production code
	// already asks (VaCuusSubsystem.cpp:320). Unregistering only what we registered matters just
	// as much: on 5.6 an unconditional unregister would tear down the engine's own entry.
	const FName TableId = Asset->GetStringTableId();
	const bool bRegisteredHere = !FStringTableRegistry::Get().FindStringTable(TableId).IsValid();
	if (bRegisteredHere)
	{
		FStringTableRegistry::Get().RegisterStringTable(TableId, Source);
	}
	ON_SCOPE_EXIT
	{
		if (bRegisteredHere)
		{
			FStringTableRegistry::Get().UnregisterStringTable(TableId);
		}
	};

	// Reached through the registry rather than the subsystem: UVaCuusSubsystem is a
	// UGameInstanceSubsystem and there is no game instance in a -nullrhi automation run. The
	// subsystem method is a two-line forward over exactly this (see its declaration), so what is
	// worth proving is the enumeration and the culture resolution, both of which live here.
	TMap<FString, FString> Built;
	Asset->GetStringTable()->EnumerateKeysAndSourceStrings(
		[&Built, TableId](const FTextKey& Key, const FString& /*SourceString*/) -> bool
		{
			Built.Add(Key.ToString(), FText::FromStringTable(TableId, Key).ToString());
			return true;
		});

	TestEqual(TEXT("every entry crossed"), Built.Num(), 2);

	const FString* Health = Built.Find(TEXT("vb_health"));
	const FString* Ammo = Built.Find(TEXT("vb_ammo"));
	if (!TestTrue(TEXT("both keys are present under their own names"), Health != nullptr && Ammo != nullptr))
	{
		return false;
	}

	// WITH NO .locres FOR THIS CULTURE the resolved value IS the source string, and that is the
	// correct answer rather than a degenerate one: FText::FromStringTable falls back to the
	// source entry. What the assertion pins is that the value came from a TEXT RESOLUTION and
	// not from the enumerator's source-string parameter -- which the bridge deliberately ignores,
	// because ignoring it is what makes a real .locres decide the answer in a shipped game.
	TestEqual(TEXT("the value is the text resolution for the current culture"), *Health, FString(TEXT("Health")));
	TestEqual(TEXT("...for every key"), *Ammo, FString(TEXT("Ammo")));

	// And it really is the string-table path, not a copy: change the entry and the resolution
	// follows, which a snapshot of the source string taken at enumeration time would not.
	VaCuusTranslationFoundationTest::SetSource(Source, TEXT("vb_health"), TEXT("Vitality"));
	TestEqual(TEXT("the resolution tracks the table, so it is a live lookup"),
		FText::FromStringTable(TableId, TEXT("vb_health")).ToString(), FString(TEXT("Vitality")));

	// Publishing it is the registry call the subsystem makes; proven end to end here so the
	// bridge's OUTPUT is known to be a legal table, not merely a well-shaped map.
	FVaCuusTranslationRegistry::SetTable(Built, TEXT("bridge"));
	const TSharedPtr<const FVaCuusTranslationSnapshot> Published = FVaCuusTranslationRegistry::GetSnapshot_GameThread();
	if (TestTrue(TEXT("the built table published"), Published.IsValid()))
	{
		TestEqual(TEXT("with its tag"), Published->Tag, FString(TEXT("bridge")));
		TestEqual(TEXT("and its entries"), Published->Table.Num(), 2);
	}

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
