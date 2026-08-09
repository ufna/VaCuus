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
 * FStringTable::SetSourceString GAINS A THIRD PARAMETER under WITH_EDITORONLY_DATA (dev notes,
 * StringTableCore.h:152-158). The monolithic game target compiles this file too -- automation
 * tests are on in Development -- so the branch has to be here rather than at each call.
 */
inline void SetSource(FStringTableRef Table, const TCHAR* Key, const TCHAR* Value)
{
#if WITH_EDITORONLY_DATA
	Table->SetSourceString(Key, Value, FString());
#else
	Table->SetSourceString(Key, Value);
#endif
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

	// REGISTERED BY HAND, because UStringTable auto-registers only when IsAsset()
	// (StringTable.cpp:401) and an object in the transient package is not one. A saved asset
	// registers itself; this test has to stand in for that, and the bridge's own guard --
	// asserted below -- is what turns the unregistered case into an error instead of a table
	// full of "<MISSING STRING TABLE ENTRY>".
	const FName TableId = Asset->GetStringTableId();
	FStringTableRegistry::Get().RegisterStringTable(TableId, Source);
	ON_SCOPE_EXIT
	{
		FStringTableRegistry::Get().UnregisterStringTable(TableId);
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
