// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "VaCuusEngine.h"
#include <RmlUi/Core.h>

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusBootTest, "VaCuus.Core.Boot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusBootTest::RunTest(const FString& Parameters)
{
	// This test BOOTS RmlUi on the test thread and owns it for its duration, so nobody
	// else may hold it: a live UI thread (a PIE session with vacuus.M1HUD up) owns the
	// library on its own thread, and Initialize() from here would then trip the
	// owner-thread check() inside FVaCuusEngine rather than fail politely. Same
	// precondition, same wording, as every other test that takes the library.
	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	FVaCuusEngine& Engine = FVaCuusEngine::Get();
	if (!TestTrue(TEXT("Initialized"), Engine.Initialize()))
	{
		return false;
	}

	Rml::Context* Ctx = Rml::CreateContext("boot_test", Rml::Vector2i(1280, 720));
	if (!TestNotNull(TEXT("Context"), Ctx))
	{
		Engine.Shutdown();
		return false;
	}

	Rml::ElementDocument* Doc = Ctx->LoadDocumentFromMemory(
		R"(<rml><head><style>body{display:block;width:100px;}</style></head>)"
		R"(<body><div id="probe">hello</div></body></rml>)");
	if (!TestNotNull(TEXT("Document"), Doc))
	{
		Rml::RemoveContext("boot_test");
		Engine.Shutdown();
		return false;
	}
	Doc->Show();
	Ctx->Update();
	TestNotNull(TEXT("Probe element"), Doc->GetElementById("probe"));

	Rml::RemoveContext("boot_test");
	Engine.Shutdown();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
