// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "VaCuusEngine.h"

#include "Misc/ScopeExit.h"

#include <RmlUi/Core.h>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A FRACTIONAL FONT-SIZE TAKES THE NEAREST WHOLE-PIXEL FACE (RmlUi Patch #14, Source/ThirdParty/RmlUi/VENDORED_TAG.txt).
 *
 * The font engine keys a face by a whole pixel size (FontEngineInterface.h:68), and the computed font-size is a float
 * that is rarely whole once it comes from anything but px: `dp` is the number times the context's ratio
 * (ComputeProperty.cpp:62), which VaCuus sets per view from the widget's DPI scale (VaCuusUIThread.cpp:1720-1724), and
 * em, rem, vw and vh scale the same way. Upstream asked for the face at `(int)font_size` (ElementStyle::ComputeValues, the
 * call at ElementStyle.cpp:1037-1038 now), so 10.95px drew at 10 and 13dp at a ratio of 1.5 drew at 19 -- always down,
 * by up to a whole pixel, while the line height kept the float.
 *
 * WHAT IS READ BACK is the size of the face each element's computed values hold, through the public
 * FontEngineInterface::GetFontMetrics, so the test needs no layout and no glyph.
 *
 * RESTORE-THE-BUG: put `(int)values.font_size()` back in ElementStyle::ComputeValues and the three fractional probes
 * read one pixel small; the two controls -- a size that rounds down and a whole one -- read the same either way, so a
 * face that is never resolved at all cannot pass for the fix.
 *
 * The face is LatoLatin, the one FVaCuusEngine::Initialize loads (VaCuusEngine.cpp:136-148).
 */
namespace VaCuusFontSizeTest
{
static const char* GDocument = R"(<rml>
<head><style>body { display: block; font-family: LatoLatin; font-size: 16px; } div { display: block; }</style></head>
<body>
<div id="frac" style="font-size: 10.95px;">Interactive</div>
<div id="half" style="font-size: 10.5px;">Interactive</div>
<div id="dp" style="font-size: 13dp;">Interactive</div>
<div id="down" style="font-size: 10.4px;">Interactive</div>
<div id="whole" style="font-size: 11px;">Interactive</div>
</body>
</rml>)";

struct FProbe
{
	const char* Id;
	int32 Expected;
	const TCHAR* Why;
};
}	 // namespace VaCuusFontSizeTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusFontFractionalSizeTest, "VaCuus.Font.FractionalSize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusFontFractionalSizeTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusFontSizeTest;

	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	if (!TestTrue(TEXT("Initialized"), FVaCuusEngine::Get().Initialize()))
	{
		return false;
	}

	const Rml::String ContextName = "vacuus_font_fractional_size";
	Rml::Context* Context = Rml::CreateContext(ContextName, Rml::Vector2i(800, 200));
	ON_SCOPE_EXIT
	{
		if (Context != nullptr)
		{
			Rml::RemoveContext(ContextName);
		}
		FVaCuusEngine::Get().Shutdown();
	};

	if (!TestNotNull(TEXT("Context"), Context))
	{
		return false;
	}

	// Before the document loads, so its dp lengths compute against this ratio from the first update.
	Context->SetDensityIndependentPixelRatio(1.5f);

	Rml::ElementDocument* Document = Context->LoadDocumentFromMemory(GDocument, "vacuus://font_fractional_size.rml");
	if (!TestNotNull(TEXT("Document"), Document))
	{
		return false;
	}
	Document->Show();
	Context->Update();

	const FProbe Probes[] = {
		{ "frac", 11, TEXT("10.95px is a hair under 11; truncated it drew at 10") },
		{ "half", 11, TEXT("10.5px rounds half up (Math.cpp:121-127); truncated it drew at 10") },
		{ "dp", 20, TEXT("13dp at a ratio of 1.5 is 19.5px; truncated it drew at 19") },
		{ "down", 10, TEXT("control: 10.4px rounds down, as truncation did") },
		{ "whole", 11, TEXT("control: a whole size is itself") },
	};

	Rml::FontEngineInterface* FontEngine = Rml::GetFontEngineInterface();
	for (const FProbe& Probe : Probes)
	{
		Rml::Element* Element = Document->GetElementById(Probe.Id);
		if (!TestNotNull(FString::Printf(TEXT("#%s"), *FString(Probe.Id)), Element))
		{
			return false;
		}

		const Rml::FontFaceHandle Face = Element->GetFontFaceHandle();
		if (!TestTrue(FString::Printf(TEXT("#%s has a face"), *FString(Probe.Id)), Face != 0))
		{
			return false;
		}

		TestEqual(FString::Printf(TEXT("#%s at %.2f px loads the %d px face (%s)"), *FString(Probe.Id), Element->GetComputedValues().font_size(),
					  Probe.Expected, Probe.Why),
			FontEngine->GetFontMetrics(Face).size, Probe.Expected);
	}

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
