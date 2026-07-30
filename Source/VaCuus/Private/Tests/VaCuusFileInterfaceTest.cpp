// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusContentPaths.h"
#include "VaCuusFileInterface.h"

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include <cstdio>

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusFileInterfaceTest, "VaCuus.Core.FileInterface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusFileInterfaceTest::RunTest(const FString& Parameters)
{
	const FString TestDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VaCuusTest"));
	const FString TestFilePath = TestDir / TEXT("file_interface_probe.txt");

	// 16 ASCII characters -> 16 bytes on disk.
	const FString Payload = TEXT("0123456789ABCDEF");
	if (!TestTrue(TEXT("Temp file saved"), FFileHelper::SaveStringToFile(Payload, *TestFilePath)))
	{
		return false;
	}

	const auto ToRmlPath = [](const FString& Path) { return Rml::String(TCHAR_TO_UTF8(*Path)); };

	// Standalone instance; no Rml boot required for the file interface itself.
	FVaCuusFileInterface FileInterface;

	// Open existing file (absolute path passthrough).
	const Rml::FileHandle File = FileInterface.Open(ToRmlPath(TestFilePath));
	if (TestTrue(TEXT("Open existing file returns nonzero handle"), File != Rml::FileHandle(0)))
	{
		// Length.
		TestEqual(TEXT("Length matches payload size"), FileInterface.Length(File), (size_t)16);

		// Valid seek + tell.
		TestTrue(TEXT("Seek SEEK_SET 10 succeeds"), FileInterface.Seek(File, 10, SEEK_SET));
		TestEqual(TEXT("Tell after valid seek"), FileInterface.Tell(File), (size_t)10);

		// Partial read: request more than remaining -> returns remaining count
		// (Rml::FileInterface::Read contract: "total number of bytes read").
		char Buffer[64] = {0};
		TestEqual(TEXT("Read request past EOF returns remaining byte count"), FileInterface.Read(Buffer, sizeof(Buffer), File), (size_t)6);
		TestTrue(TEXT("Read content matches payload tail"), FMemory::Memcmp(Buffer, "ABCDEF", 6) == 0);
		TestEqual(TEXT("Read at EOF returns 0"), FileInterface.Read(Buffer, sizeof(Buffer), File), (size_t)0);

		// SEEK_END with negative offset lands back in range.
		TestTrue(TEXT("Seek SEEK_END -16 succeeds"), FileInterface.Seek(File, -16, SEEK_END));
		TestEqual(TEXT("Tell after SEEK_END -16"), FileInterface.Tell(File), (size_t)0);

		// Out-of-range seeks must fail without touching IFileHandle (which asserts).
		TestFalse(TEXT("Seek SEEK_SET beyond size fails"), FileInterface.Seek(File, 17, SEEK_SET));
		TestFalse(TEXT("Seek SEEK_SET negative fails"), FileInterface.Seek(File, -1, SEEK_SET));
		TestFalse(TEXT("Seek SEEK_CUR below zero fails"), FileInterface.Seek(File, -1, SEEK_CUR));
		TestFalse(TEXT("Seek SEEK_END past end fails"), FileInterface.Seek(File, 1, SEEK_END));
		TestEqual(TEXT("Tell unchanged after failed seeks"), FileInterface.Tell(File), (size_t)0);

		FileInterface.Close(File);
	}

	// Directories and missing files must not produce handles.
	TestEqual(TEXT("Open directory returns 0"), FileInterface.Open(ToRmlPath(TestDir)), Rml::FileHandle(0));
	TestEqual(TEXT("Open missing file returns 0"), FileInterface.Open(ToRmlPath(TestDir / TEXT("does_not_exist.txt"))), Rml::FileHandle(0));

	// Clean up.
	IFileManager::Get().Delete(*TestFilePath);
	IFileManager::Get().DeleteDirectory(*TestDir);

	return true;
}

/**
 * The ordered DevUI roots and the precedence between them (controller decision D19,
 * bead VaCuus-akj.6.3).
 *
 * WHAT THIS IS REALLY GUARDING: the plugin's Content/DevUI is now canonical, and the
 * duplicated copy under <Project>/Content/DevUI has been deleted. If the order ever
 * flipped, a project copy someone re-creates would silently shadow the plugin document
 * the editor file watcher is watching -- live reload would stop working with no error
 * anywhere. So the order is asserted, not just the resolution.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusContentRootsTest, "VaCuus.Core.ContentRoots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusContentRootsTest::RunTest(const FString& Parameters)
{
	const TArray<FString>& Roots = VaCuusContentPaths::GetDocumentRoots();
	if (!TestEqual(TEXT("Exactly two DevUI roots (plugin, project)"), Roots.Num(), 2))
	{
		return false;
	}

	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("VaCuus"));
	if (!TestTrue(TEXT("VaCuus plugin descriptor found"), Plugin.IsValid()))
	{
		return false;
	}

	const FString ExpectedPluginRoot =
		FPaths::ConvertRelativePathToFull(Plugin->GetContentDir() / TEXT("DevUI"));
	const FString ExpectedProjectRoot =
		FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("DevUI"));

	TestEqual(TEXT("Root 0 is the PLUGIN's Content/DevUI"), Roots[0], ExpectedPluginRoot);
	TestEqual(TEXT("Root 1 is the PROJECT's Content/DevUI"), Roots[1], ExpectedProjectRoot);

	// The shipped HUD document must resolve, and must resolve to the PLUGIN copy: that is
	// the concrete claim "the plugin's Content/DevUI is canonical" makes.
	FString HudRoot;
	const FString HudPath = VaCuusContentPaths::ResolveExistingDocument(TEXT("m1_hud.rml"), &HudRoot);
	TestFalse(TEXT("m1_hud.rml resolves"), HudPath.IsEmpty());
	TestEqual(TEXT("m1_hud.rml comes from the plugin root"), HudRoot, ExpectedPluginRoot);

	// Precedence, proved by shadowing rather than asserted: the same relative name exists
	// under both roots with different contents, and the plugin's must win.
	const FString ProbeName = TEXT("vacuus_root_order_probe.rml.tmptest");
	const FString PluginProbe = Roots[0] / ProbeName;
	const FString ProjectProbe = Roots[1] / ProbeName;

	const bool bWrotePlugin = FFileHelper::SaveStringToFile(TEXT("PLUGIN"), *PluginProbe);
	const bool bWroteProject = FFileHelper::SaveStringToFile(TEXT("PROJECTROOT"), *ProjectProbe);
	if (TestTrue(TEXT("Probe written under both roots"), bWrotePlugin && bWroteProject))
	{
		FVaCuusFileInterface FileInterface;
		const auto ToRmlPath = [](const FString& Path) { return Rml::String(TCHAR_TO_UTF8(*Path)); };

		// Length is the discriminator: "PLUGIN" is 6 bytes, "PROJECTROOT" is 11.
		const Rml::FileHandle Shadowed = FileInterface.Open(ToRmlPath(ProbeName));
		if (TestTrue(TEXT("Shadowed probe opens"), Shadowed != Rml::FileHandle(0)))
		{
			TestEqual(TEXT("The PLUGIN copy wins when both roots have the file"),
				FileInterface.Length(Shadowed), (size_t)6);
			FileInterface.Close(Shadowed);
		}

		// With the plugin copy gone, the project root is a real fallback rather than dead
		// code -- that is the "extension point" half of D19.
		IFileManager::Get().Delete(*PluginProbe);

		const Rml::FileHandle Fallback = FileInterface.Open(ToRmlPath(ProbeName));
		if (TestTrue(TEXT("Project-root probe opens once the plugin copy is gone"), Fallback != Rml::FileHandle(0)))
		{
			TestEqual(TEXT("The PROJECT copy answers as the second root"),
				FileInterface.Length(Fallback), (size_t)11);
			FileInterface.Close(Fallback);
		}

		// A relative name under no root at all must not produce a handle.
		TestEqual(TEXT("Unknown relative name returns 0"),
			FileInterface.Open(ToRmlPath(TEXT("vacuus_no_such_document.rml"))), Rml::FileHandle(0));
	}

	IFileManager::Get().Delete(*PluginProbe);
	IFileManager::Get().Delete(*ProjectProbe);

	// The project DevUI directory may not have existed before this test created the probe
	// in it (it is deliberately empty in this repo now); leave it as we found it.
	if (IFileManager::Get().DirectoryExists(*Roots[1]))
	{
		TArray<FString> Remaining;
		IFileManager::Get().FindFilesRecursive(Remaining, *Roots[1], TEXT("*"), true, true);
		if (Remaining.Num() == 0)
		{
			IFileManager::Get().DeleteDirectory(*Roots[1]);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
