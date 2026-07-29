// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "VaCuusEngine.h"
#include <RmlUi/Core.h>

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusBootTest, "VaCuus.Core.Boot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusBootTest::RunTest(const FString& Parameters)
{
	FVaCuusEngine& Engine = FVaCuusEngine::Get();
	TestTrue(TEXT("Initialized"), Engine.Initialize());

	Rml::Context* Ctx = Rml::CreateContext("boot_test", Rml::Vector2i(1280, 720));
	TestNotNull(TEXT("Context"), Ctx);

	Rml::ElementDocument* Doc = Ctx->LoadDocumentFromMemory(
		R"(<rml><head><style>body{display:block;width:100px;}</style></head>)"
		R"(<body><div id="probe">hello</div></body></rml>)");
	TestNotNull(TEXT("Document"), Doc);
	Doc->Show();
	Ctx->Update();
	TestNotNull(TEXT("Probe element"), Doc->GetElementById("probe"));

	Rml::RemoveContext("boot_test");
	Engine.Shutdown();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
