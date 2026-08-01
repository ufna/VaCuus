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

	// Bundle-served: the bytes and the provenance both come from the mount.
	FString Source;
	FString ServedFrom;
	TestTrue(TEXT("A bundle-only script reads"),
		VaCuusJsScriptSource::ReadScriptByVfsPath(TEXT("M6_Script_Probe.js"), Source, &ServedFrom));
	TestEqual(TEXT("...with the packed bytes"), Source, FString(TEXT("let vacuusBundleProbe = 42;")));
	TestTrue(TEXT("...and names the bundle as its source"),
		ServedFrom.StartsWith(TEXT("bundle://<TestJsBundle>/")));

	// RESTORE-THE-BUG: unmounted, the same read is exactly the pre-fix bundle-only
	// failure -- no loose copy exists, so a reader without the bundle branch can
	// only miss. Seen to fail, then seen to pass above.
	TestTrue(TEXT("Unmount succeeds"), FVaCuusBundleMountTable::UnmountBundle(TEXT("<TestJsBundle>")));
	FString Missed;
	TestFalse(TEXT("Without the mount the bundle-only script cannot be read"),
		VaCuusJsScriptSource::ReadScriptByVfsPath(TEXT("m6_script_probe.js"), Missed));

	// The loose fallback still serves what bundles do not: a real file under the
	// first DevUI root, no bundle mounted.
	const TArray<FString>& Roots = VaCuusContentPaths::GetDocumentRoots();
	if (TestTrue(TEXT("There is a loose root"), Roots.Num() > 0))
	{
		const FString LoosePath = Roots[0] / TEXT("m6_loose_script_probe.js.tmptest");
		if (TestTrue(TEXT("Loose probe written"), FFileHelper::SaveStringToFile(TEXT("loose();"), *LoosePath)))
		{
			FString LooseSource;
			FString LooseServedFrom;
			TestTrue(TEXT("The loose fallback reads"),
				VaCuusJsScriptSource::ReadScriptByVfsPath(
					TEXT("m6_loose_script_probe.js.tmptest"), LooseSource, &LooseServedFrom));
			TestEqual(TEXT("...the loose bytes"), LooseSource, FString(TEXT("loose();")));
			TestFalse(TEXT("...and names a disk path, not a bundle"), LooseServedFrom.StartsWith(TEXT("bundle://")));
		}
		IFileManager::Get().Delete(*LoosePath);
	}

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
