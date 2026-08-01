// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusBundle.h"
#include "VaCuusBundleMount.h"
#include "VaCuusContentPaths.h"

#include "../VaCuusJsScriptSource.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The script read path's bundle branch (M6): scripts do NOT flow through RmlUi's
 * FileInterface -- this module reads <script src> and module files itself -- so
 * the bundle-first precedence has to exist HERE too, or a bundle-only Shipping
 * build boots documents whose every script is dead. Found by the packaged
 * Development gate (the '<script src="M5Hud/hud_bundle.js"> ... did not resolve'
 * Error over a mounted bundle); this test is the failure's automation twin: the
 * unmounted leg below IS the pre-fix behavior for a bundle-only tree.
 *
 * It also asserts the path's OBSERVABILITY, both ways (the M==0 contract, spec M6
 * 2(d)): a loose serve with bundles mounted fires the Warning naming every probed
 * bundle and counts into VaCuusScriptServing's loose tally -- and a serve with NO
 * bundles mounted does neither of the first and only the second. The Warning
 * expectation is pinned to EXACTLY ONE occurrence, so a warning leaking into the
 * unmounted legs fails the count just as its absence fails the mounted leg.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsBundleScriptTest, "VaCuus.Js.BundleScript",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusJsBundleScriptTest::RunTest(const FString& Parameters)
{
	static const ANSICHAR* ScriptBody = "let vacuusBundleProbe = 42;";

	// A transient bundle carrying one script that exists NOWHERE on disk.
	{
		VaCuusBundleFormat::FCookedIndex Index;
		TArray64<uint8> Payload;
		Payload.Append(reinterpret_cast<const uint8*>(ScriptBody), FCStringAnsi::Strlen(ScriptBody));
		Index.Entries.Add(FVaCuusBundleEntry{TEXT("m6_script_probe.js"), 0, Payload.Num()});
		Index.PayloadSize = Payload.Num();
		if (!TestTrue(TEXT("The script bundle mounts"),
				FVaCuusBundleMountTable::MountTransient(TEXT("<TestJsBundle>"), TEXT("VaCuusJsBundleScriptTest"),
					MoveTemp(Index), MoveTemp(Payload))))
		{
			return false;
		}
	}

	ON_SCOPE_EXIT
	{
		FVaCuusBundleMountTable::UnmountAll();
	};

	// The counters are process-wide and monotonic; everything below asserts DELTAS.
	const uint64 BundleServes0 = VaCuusScriptServing::GetNumBundleScriptServes();
	const uint64 LooseServes0 = VaCuusScriptServing::GetNumLooseScriptServes();

	// Bundle-served: the bytes, the provenance and the COUNT all come from the mount.
	FString Source;
	FString ServedFrom;
	TestTrue(TEXT("A bundle-only script reads"),
		VaCuusJsScriptSource::ReadScriptByVfsPath(TEXT("M6_Script_Probe.js"), Source, &ServedFrom));
	TestEqual(TEXT("...with the packed bytes"), Source, FString(TEXT("let vacuusBundleProbe = 42;")));
	TestTrue(TEXT("...and names the bundle as its source"),
		ServedFrom.StartsWith(TEXT("bundle://<TestJsBundle>/")));
	TestEqual(TEXT("...counting ONE bundle script serve"),
		VaCuusScriptServing::GetNumBundleScriptServes() - BundleServes0, (uint64)1);
	TestEqual(TEXT("...and ZERO loose ones"),
		VaCuusScriptServing::GetNumLooseScriptServes() - LooseServes0, (uint64)0);

	// The loose file both loose legs read; written now so the MOUNTED leg can miss the
	// bundle and still serve.
	const TArray<FString>& Roots = VaCuusContentPaths::GetDocumentRoots();
	if (!TestTrue(TEXT("There is a loose root"), Roots.Num() > 0))
	{
		return false;
	}
	const FString LoosePath = Roots[0] / TEXT("m6_loose_script_probe.js.tmptest");
	if (!TestTrue(TEXT("Loose probe written"), FFileHelper::SaveStringToFile(TEXT("loose();"), *LoosePath)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*LoosePath);
	};

	// LOOSE SERVE WITH A BUNDLE MOUNTED -- the silent-miss killer's script twin, and
	// the leg the packaged M==0 gate is really about: the read succeeds from disk, the
	// Warning names the probed bundle, and the serve lands in the LOOSE tally the
	// teardown line prints. Occurrence count 1 is load-bearing: the unmounted loose
	// leg below runs the same read with no mounts, and a second firing would fail it.
	AddExpectedMessagePlain(TEXT("is in NO mounted bundle (probed: <TestJsBundle>)"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	{
		FString MountedLooseSource;
		FString MountedLooseServedFrom;
		TestTrue(TEXT("A loose-only script still serves while a bundle is mounted"),
			VaCuusJsScriptSource::ReadScriptByVfsPath(
				TEXT("m6_loose_script_probe.js.tmptest"), MountedLooseSource, &MountedLooseServedFrom));
		TestEqual(TEXT("...the loose bytes"), MountedLooseSource, FString(TEXT("loose();")));
		TestFalse(TEXT("...from a disk path, not a bundle"), MountedLooseServedFrom.StartsWith(TEXT("bundle://")));
		TestEqual(TEXT("...counting ONE loose script serve"),
			VaCuusScriptServing::GetNumLooseScriptServes() - LooseServes0, (uint64)1);
		TestEqual(TEXT("...and no further bundle one"),
			VaCuusScriptServing::GetNumBundleScriptServes() - BundleServes0, (uint64)1);
	}

	// RESTORE-THE-BUG: unmounted, the same read is exactly the pre-fix bundle-only
	// failure -- no loose copy exists, so a reader without the bundle branch can
	// only miss. Seen to fail, then seen to pass above. A miss SERVES nothing, so
	// neither counter may move.
	TestTrue(TEXT("Unmount succeeds"), FVaCuusBundleMountTable::UnmountBundle(TEXT("<TestJsBundle>")));
	FString Missed;
	TestFalse(TEXT("Without the mount the bundle-only script cannot be read"),
		VaCuusJsScriptSource::ReadScriptByVfsPath(TEXT("m6_script_probe.js"), Missed));
	TestEqual(TEXT("A failed read serves nothing: loose count unchanged"),
		VaCuusScriptServing::GetNumLooseScriptServes() - LooseServes0, (uint64)1);
	TestEqual(TEXT("...bundle count unchanged"),
		VaCuusScriptServing::GetNumBundleScriptServes() - BundleServes0, (uint64)1);

	// The loose fallback with NO bundles mounted: serves, counts loose -- and fires no
	// Warning (the count-1 expectation above is what would catch one).
	{
		FString LooseSource;
		FString LooseServedFrom;
		TestTrue(TEXT("The loose fallback reads with no mounts"),
			VaCuusJsScriptSource::ReadScriptByVfsPath(
				TEXT("m6_loose_script_probe.js.tmptest"), LooseSource, &LooseServedFrom));
		TestEqual(TEXT("...the loose bytes"), LooseSource, FString(TEXT("loose();")));
		TestFalse(TEXT("...and names a disk path, not a bundle"), LooseServedFrom.StartsWith(TEXT("bundle://")));
		TestEqual(TEXT("...counting the SECOND loose serve"),
			VaCuusScriptServing::GetNumLooseScriptServes() - LooseServes0, (uint64)2);
	}

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
