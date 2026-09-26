// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusTestDocumentHost.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "HAL/PlatformProcess.h"
#include "Misc/ScopeExit.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Traits.h>

#if WITH_DEV_AUTOMATION_TESTS

/*
 * TEXT-TRANSFORM ON UNICODE LETTERS (RmlUi Patch #9, Source/ThirdParty/RmlUi/VENDORED_TAG.txt).
 *
 * Vendored RmlUi's BuildToken() walked the UTF-8 text byte by byte and mapped case only for
 * a-z / A-Z, so text-transform: uppercase/lowercase left every non-ASCII letter untouched: an
 * English caption went to upper case but a translated one did not -- a localised game whose
 * RCSS capitalises captions and buttons through the transform shows every translated string in
 * mixed case.
 *
 * A FONT-FAMILY AND A SIZE ARE REQUIRED in the document, not decoration: without them RmlUi
 * logs "No font face defined" and never lays the text out, so GetLines() stays empty whether
 * the patch is present or not (see VaCuusFontRegistryTest.cpp's FVaCuusFontMissingGlyphTest
 * comment at the same point). What is read back is Rml::ElementText::GetLines(), whose `text`
 * is the transformed string BEFORE glyph lookup -- missing glyphs (patch #5's U+FFFD
 * substitution) happen later, in FontFaceHandleDefault::GetOrAppendGlyph, and do not matter
 * here: the shipped LatoLatin face need not cover Cyrillic or Greek for this test to be valid.
 *
 * NO NON-ASCII BYTES IN THIS SOURCE FILE. Every non-ASCII letter is built at runtime from its
 * Unicode code point (AppendCodepoints below) instead of being typed into a string literal, so
 * the .cpp itself stays plain ASCII. This sidesteps both known escape traps in this codebase:
 * \x inside a narrow u8"" literal is re-encoded by MSVC into the wrong bytes
 * (VaCuusDataVariableTest.cpp's SeedShadow() comment), and a \u universal-character-name
 * literal risks the same kind of silent mojibake if it is ever copy-pasted through a non-UTF-8
 * tool. A plain hex integer has one meaning everywhere.
 */
namespace VaCuusTextTransformTest
{
/** Appends each of Codepoints as one native TCHAR. BMP-only (U+0000-U+FFFF), which covers every
 *  letter this test needs, so no surrogate-pair handling is required. */
static void AppendCodepoints(FString& Out, std::initializer_list<uint32> Codepoints)
{
	for (uint32 Codepoint : Codepoints)
	{
		Out.AppendChar(static_cast<TCHAR>(Codepoint));
	}
}

/**
 * Builds the fixture document. body/div carry font-family + font-size (required, see above)
 * and white-space: nowrap, so each element's text lays out as exactly one line and the whole
 * string can be read back as a single concatenation of GetLines().
 *
 * "upper" (text-transform: uppercase) mixes in, lower case apart from one capital initial: the
 * Cyrillic word for "accept" (7 letters, capitalised), the Cyrillic word for "hedgehog" (2 letters, one of them needing a 2-code-point
 * -apart mapping), the Spanish word for a rhea-like bird (has n-with-tilde and u-with-acute),
 * the Greek word for "hour" (3 letters, one bearing a tonos accent), the German word for
 * "street" spelled with a sharp s (which has no single-code-point upper case and must stay
 * unchanged), and the Turkish dotless i (which upper-cases to plain ASCII 'I', not to 'I' with
 * a dot).
 * "lower" (text-transform: lowercase) is the upper-case form of the same set, plus the Turkish
 * dotted capital I (which lower-cases to plain ASCII 'i').
 * "none" (no text-transform at all) is the capitalised Cyrillic "accept" followed by the English
 * word -- this is the negative control this test also carries: it must read back byte-for-byte
 * unchanged both before and after the patch.
 */
static FString BuildDocument()
{
	FString Doc;
	Doc += TEXT("<rml>\n<head><style>\n");
	Doc += TEXT("body { display: block; font-family: LatoLatin; font-size: 16px; }\n");
	Doc += TEXT("div  { display: block; white-space: nowrap; }\n");
	Doc += TEXT("</style></head>\n<body>\n");

	Doc += TEXT("<div id=\"upper\" style=\"text-transform: uppercase;\">Accept ");
	AppendCodepoints(Doc, {0x41F, 0x440, 0x438, 0x43D, 0x44F, 0x442, 0x44C}); // Cyrillic "accept", capital initial
	Doc += TEXT(" ");
	AppendCodepoints(Doc, {0x451, 0x436}); // Cyrillic "hedgehog", lower case
	Doc += TEXT(" ");
	AppendCodepoints(Doc, {0xF1}); // n with tilde
	Doc += TEXT("and");
	AppendCodepoints(Doc, {0xFA}); // u with acute
	Doc += TEXT(" ");
	AppendCodepoints(Doc, {0x3CE, 0x3C1, 0x3B1}); // Greek "hour", lower case
	Doc += TEXT(" stra");
	AppendCodepoints(Doc, {0xDF}); // sharp s
	Doc += TEXT("e ");
	AppendCodepoints(Doc, {0x131}); // Turkish dotless i
	Doc += TEXT("</div>\n");

	Doc += TEXT("<div id=\"lower\" style=\"text-transform: lowercase;\">ACCEPT ");
	AppendCodepoints(Doc, {0x41F, 0x420, 0x418, 0x41D, 0x42F, 0x422, 0x42C}); // Cyrillic "ACCEPT", upper case
	Doc += TEXT(" ");
	AppendCodepoints(Doc, {0x401, 0x416}); // Cyrillic "HEDGEHOG", upper case
	Doc += TEXT(" ");
	AppendCodepoints(Doc, {0xD1}); // N with tilde
	Doc += TEXT("AND");
	AppendCodepoints(Doc, {0xDA}); // U with acute
	Doc += TEXT(" ");
	AppendCodepoints(Doc, {0x38F, 0x3A1, 0x391}); // Greek "HOUR", upper case
	Doc += TEXT(" ");
	AppendCodepoints(Doc, {0x130}); // Turkish dotted capital I
	Doc += TEXT("</div>\n");

	Doc += TEXT("<div id=\"none\">");
	AppendCodepoints(Doc, {0x41F, 0x440, 0x438, 0x43D, 0x44F, 0x442, 0x44C}); // Cyrillic "accept", capital initial
	Doc += TEXT(" Accept</div>\n");

	Doc += TEXT("</body>\n</rml>\n");
	return Doc;
}

static FString ExpectedUpper()
{
	FString Out = TEXT("ACCEPT ");
	AppendCodepoints(Out, {0x41F, 0x420, 0x418, 0x41D, 0x42F, 0x422, 0x42C}); // Cyrillic "ACCEPT"
	Out += TEXT(" ");
	AppendCodepoints(Out, {0x401, 0x416}); // Cyrillic "HEDGEHOG"
	Out += TEXT(" ");
	AppendCodepoints(Out, {0xD1});
	Out += TEXT("AND");
	AppendCodepoints(Out, {0xDA});
	Out += TEXT(" ");
	AppendCodepoints(Out, {0x38F, 0x3A1, 0x391}); // Greek "HOUR"
	Out += TEXT(" STRA");
	AppendCodepoints(Out, {0xDF}); // sharp s has no single-code-point upper case and stays
	Out += TEXT("E I");
	return Out;
}

static FString ExpectedLower()
{
	FString Out = TEXT("accept ");
	AppendCodepoints(Out, {0x43F, 0x440, 0x438, 0x43D, 0x44F, 0x442, 0x44C}); // Cyrillic "accept"
	Out += TEXT(" ");
	AppendCodepoints(Out, {0x451, 0x436}); // Cyrillic "hedgehog"
	Out += TEXT(" ");
	AppendCodepoints(Out, {0xF1});
	Out += TEXT("and");
	AppendCodepoints(Out, {0xFA});
	Out += TEXT(" ");
	AppendCodepoints(Out, {0x3CE, 0x3C1, 0x3B1}); // Greek "hour"
	Out += TEXT(" i");
	return Out;
}

static FString ExpectedNone()
{
	FString Out;
	AppendCodepoints(Out, {0x41F, 0x440, 0x438, 0x43D, 0x44F, 0x442, 0x44C}); // Cyrillic "accept", unchanged
	Out += TEXT(" Accept");
	return Out;
}

/** Samples the transformed text of the three probe elements once per frame. */
class FTextTransformProbeHost final : public FVaCuusTestDocumentHost
{
public:
	explicit FTextTransformProbeHost(const TCHAR* InContextPrefix)
		: FVaCuusTestDocumentHost(InContextPrefix, "vacuus://text_transform.rml", Rml::FocusFlag::Document)
	{
	}

	virtual void SetVisible(bool /*bVisible*/) override {}

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		Context->Update();

		if (RmlDocument != nullptr)
		{
			ReadElement("upper", bUpperTextFound, UpperResult);
			ReadElement("lower", bLowerTextFound, LowerResult);
			ReadElement("none", bNoneTextFound, NoneResult);
		}

		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

	//~ Post-frame observations; see VaCuusFontRegistryTest.cpp's class comment for the
	//~ release/acquire hand-off argument these plain members rely on.
	bool bUpperTextFound = false;
	bool bLowerTextFound = false;
	bool bNoneTextFound = false;
	FString UpperResult;
	FString LowerResult;
	FString NoneResult;

private:
	/**
	 * Concatenates GetLines() of Id's ElementText first child. Leaves OutFound false (and
	 * OutText untouched) if the element has no ElementText child or no line has been laid out
	 * yet, so an empty layout can never be mistaken for a passing read.
	 */
	void ReadElement(const char* Id, bool& OutFound, FString& OutText) const
	{
		Rml::Element* Div = RmlDocument->GetElementById(Id);
		Rml::ElementText* Text = Div != nullptr ? rmlui_dynamic_cast<Rml::ElementText*>(Div->GetFirstChild()) : nullptr;
		if (Text == nullptr || Text->GetLines().empty())
		{
			return;
		}

		Rml::String Concatenated;
		for (const Rml::ElementText::Line& Line : Text->GetLines())
		{
			Concatenated += Line.text;
		}

		OutFound = true;
		OutText = FString(UTF8_TO_TCHAR(Concatenated.c_str())).TrimEnd();
	}
};

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
}	 // namespace VaCuusTextTransformTest

/**
 * THE NEGATIVE CONTROL, per the restore-the-bug standard: "none" must read back unchanged both
 * before and after the patch, so a passing "upper"/"lower" cannot be an artifact of a transform
 * that fires unconditionally. RED is expected on "upper" and "lower" against the unpatched
 * vendored RmlUi -- Cyrillic, Greek and Latin-1 letters keep their source case -- and GREEN on
 * "none"; this test must go GREEN on all three only because of the patch in ElementText.cpp.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusTextTransformTest, "VaCuus.Rml.TextTransform.Unicode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusTextTransformTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusTextTransformTest;

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

	TUniquePtr<FTextTransformProbeHost> Owned = MakeUnique<FTextTransformProbeHost>(TEXT("vacuus_text_transform"));
	FTextTransformProbeHost* Probe = Owned.Get();
	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
	const uint32 ViewId = UIThread->AllocateViewId();

	UIThread->EnqueueAddView(ViewId, MoveTemp(Owned), FIntPoint(400, 200), Status);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, BuildDocument(), /*LoadSerial=*/1);
	if (!TestTrue(TEXT("frames ran over the document"), RunFrames(*UIThread, 3)))
	{
		return false;
	}

	if (!TestTrue(TEXT("document loaded"),
			Status->LoadCompletedSerial.load(std::memory_order_acquire) == 1 &&
				Status->LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded)))
	{
		return false;
	}

	if (!TestTrue(TEXT("'none' has a laid-out ElementText child"), Probe->bNoneTextFound))
	{
		return false;
	}
	TestEqual(TEXT("text-transform: none leaves mixed-script text untouched"), Probe->NoneResult, ExpectedNone());

	// THE CLAIM. Every letter -- ASCII, Cyrillic, Greek, accented Latin, Turkish dotless i --
	// maps case, not only a-z / A-Z; sharp s has no single-code-point upper case and stays.
	if (!TestTrue(TEXT("'upper' has a laid-out ElementText child"), Probe->bUpperTextFound))
	{
		return false;
	}
	TestEqual(TEXT("text-transform: uppercase maps Unicode letters, not only ASCII"), Probe->UpperResult, ExpectedUpper());

	if (!TestTrue(TEXT("'lower' has a laid-out ElementText child"), Probe->bLowerTextFound))
	{
		return false;
	}
	TestEqual(TEXT("text-transform: lowercase maps Unicode letters, not only ASCII"), Probe->LowerResult, ExpectedLower());

	return true;
}

namespace VaCuusTextTransformTest
{
/**
 * A text field asked for `text-transform: uppercase` twice -- inherited from body and in its own
 * style attribute -- holding "kap" + dotless i, the Turkish word for "door". Dotless i (2 UTF-8
 * bytes) upper-cases to ASCII 'I' (1 byte) under Patch #9, so IF the transform reached the field
 * its displayed line would be one byte shorter than its value.
 */
static FString BuildInputDocument()
{
	FString Doc;
	Doc += TEXT("<rml>\n<head><style>\n");
	Doc += TEXT("body { display: block; font-family: LatoLatin; font-size: 16px; text-transform: uppercase; }\n");
	Doc += TEXT("input { display: inline-block; width: 300px; }\n");
	Doc += TEXT("</style></head>\n<body>\n");
	Doc += TEXT("<input id=\"field\" type=\"text\" style=\"text-transform: uppercase;\" value=\"kap");
	AppendCodepoints(Doc, {0x131});
	Doc += TEXT("\"/>\n</body>\n</rml>\n");
	return Doc;
}

static FString ExpectedInputValue()
{
	FString Out = TEXT("kap");
	AppendCodepoints(Out, {0x131});
	Out += TEXT("x");
	return Out;
}

/** Frame 0 lays the field out; frame 1 focuses it, presses End, types 'x' and reads the value. */
class FInputByteParityProbeHost final : public FVaCuusTestDocumentHost
{
public:
	FInputByteParityProbeHost()
		: FVaCuusTestDocumentHost(TEXT("vacuus_text_transform_input"), "vacuus://text_transform_input.rml", Rml::FocusFlag::Document)
	{
	}

	virtual void SetVisible(bool /*bVisible*/) override {}

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		Context->Update();

		if (RmlDocument != nullptr && FrameIndex++ == 1)
		{
			if (Rml::ElementFormControlInput* Field = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(RmlDocument->GetElementById("field")))
			{
				Field->Focus();
				Context->ProcessKeyDown(Rml::Input::KI_END, 0);
				Context->ProcessTextInput("x");
				Context->Update();
				bFieldFound = true;
				FieldValue = UTF8_TO_TCHAR(Field->GetValue().c_str());
			}
		}

		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

	//~ Post-frame observations, same hand-off as FTextTransformProbeHost.
	bool bFieldFound = false;
	FString FieldValue;

private:
	int32 FrameIndex = 0;
};
}	 // namespace VaCuusTextTransformTest

/**
 * GUARD FOR AN UPSTREAM LINE PATCH #9 RELIES ON. Four of Patch #9's mappings change a letter's
 * UTF-8 length (dotless i, dotted capital I, long s, capital sharp s), and the text-input widget
 * cannot survive that: it takes the displayed line's byte count as a length in its value
 * (WidgetTextInput.cpp:1294, used at :873 and :1098). It never sees a transform only because it
 * pins `text-transform: none` on its element as an inline property (WidgetTextInput.cpp:169),
 * which neither an inherited value nor the element's own style attribute overrides.
 *
 * Restore-the-bug, 2026-09-26: with :169 removed the field displays "KAPI", End stops one byte
 * short and snaps BACK to the boundary before the dotless i (EndLine seeks backward, :881), and
 * the value reads "kap" + 'x' + dotless i. With :169 in place, End lands after it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusTextTransformInputTest, "VaCuus.Rml.TextTransform.InputByteParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusTextTransformInputTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusTextTransformTest;

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

	TUniquePtr<FInputByteParityProbeHost> Owned = MakeUnique<FInputByteParityProbeHost>();
	FInputByteParityProbeHost* Probe = Owned.Get();
	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
	const uint32 ViewId = UIThread->AllocateViewId();

	UIThread->EnqueueAddView(ViewId, MoveTemp(Owned), FIntPoint(400, 200), Status);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, BuildInputDocument(), /*LoadSerial=*/1);
	if (!TestTrue(TEXT("frames ran over the document"), RunFrames(*UIThread, 3)))
	{
		return false;
	}

	if (!TestTrue(TEXT("document loaded"),
			Status->LoadCompletedSerial.load(std::memory_order_acquire) == 1 &&
				Status->LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded)))
	{
		return false;
	}

	if (!TestTrue(TEXT("the field was found and driven"), Probe->bFieldFound))
	{
		return false;
	}
	TestEqual(TEXT("End + 'x' appends after the dotless i: the field stays untransformed"), Probe->FieldValue, ExpectedInputValue());

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
