// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusFileInterface.h"

#include "HAL/FileManager.h"
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

#endif // WITH_DEV_AUTOMATION_TESTS
