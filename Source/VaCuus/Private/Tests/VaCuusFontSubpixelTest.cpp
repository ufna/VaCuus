// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "VaCuusEngine.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include <RmlUi/Core.h>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The re-vendoring check for RmlUi Patch #15 (Source/ThirdParty/RmlUi/VENDORED_TAG.txt): text is laid out on a
 * fractional pen. The mechanism lives in that entry; this header holds only what each test is sensitive to.
 *
 * LETTER SPACING. Upstream casts the spacing to int per glyph (GetStringWidth, GenerateString), so half a pixel over
 * eleven glyphs adds nothing at all; patched, it adds five and a half, and the whole-pixel answer moves by 5 or 6.
 *
 * FRACTIONAL PEN. Upstream rounds every quad to the grid (FontFaceLayer::GenerateGeometry), so a pen moved by a
 * quarter of a pixel draws the very same quad. Patched, the quad stays on the grid and its texture coordinates move
 * instead, so the shift is read back from both: (x2 - x1) - (u2 - u1) / texel, the texel being the quad's u per
 * pixel. That needs no reference position for the glyph, and it is the same expression whether or not the quad was
 * widened.
 *
 * LINE WIDTH. Upstream measures a line as the sum of its tokens, each rounded up, and so overstates the line
 * GenerateString draws by up to a pixel a token. This check alone catches the ElementText.cpp hunk lost while the
 * float pen survived; with the whole patch gone every width is whole again and the two agree, which the first two
 * checks catch. Its control asks that the text actually tell the two measurements apart.
 *
 * ADVANCE. Upstream advances each glyph by its hinted horiAdvance, a whole pixel, so a run of one glyph is that many
 * times its rounding. LatoLatin's H is 1527/2000 em in its hmtx table, 12.216 px at 16 px, so a hundred of them are
 * 1221.6 px and measure 1222; on the hinted advance they measure 1200.
 *
 * KERNING. FT_KERNING_DEFAULT scales a pair by ppem/25 below 25 ppem and then rounds it to a whole pixel (FreeType
 * 2.14.1 src/base/ftobjs.c:3630-3646); UNFITTED is the face's value scaled to the size and nothing else, so the
 * kerning of a run is proportional to the size. LatoLatin kerns "To" by -244 and "oT" by -247 units of 2000, which is
 * -0.49 px at 10 px after DEFAULT's damping and so rounds to nothing: the run at 10 px reads 0 against about -47.
 *
 * FALLBACK ADVANCE. A glyph the styled face lacks is copied from a fallback face with FontGlyph::WeakCopy
 * (FontFaceHandleDefault.cpp:385-389). A copy without advance_exact advances by 0 on the fractional pen, so the next
 * glyph lands on top of it. The repository ships one face (VaCuusFontRegistryTest.cpp:197-198 says why), so the
 * fallback here is the engine's own Slate font, read from disk for the test and never added to the repository.
 *
 * REPLACEMENT ADVANCE. LatoLatin has no U+FFFD, so for a character no face covers the engine draws its own box
 * (FreeTypeInterface.cpp:264-272), whose advance is size / 3 + 2 -- 7 px at 16 px. Without advance_exact it advances
 * by 0 too.
 *
 * The face is LatoLatin, the one FVaCuusEngine::Initialize loads (VaCuusEngine.cpp:136-148); kerning is off wherever
 * it would only add noise.
 */
namespace VaCuusFontSubpixelTest
{
/** LOWER CASE, as the provider keys its families (FontProvider.cpp:247-258) and as a document's font-family arrives
 *  (ComputeProperty.cpp:122). The lookup asserts it only in RmlUi's debug build (FontProvider.cpp:77); anywhere else
 *  "LatoLatin" finds no family and the handle is 0. */
static const char* GFamily = "latolatin";

/** RmlUi up with one context for as long as the rig lives. */
struct FRig
{
	Rml::String ContextName;
	Rml::Context* Context = nullptr;
	bool bBooted = false;

	bool Boot(FAutomationTestBase& Test, const char* Name)
	{
		if (!Test.TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
		{
			return false;
		}

		if (!Test.TestTrue(TEXT("Initialized"), FVaCuusEngine::Get().Initialize()))
		{
			return false;
		}
		bBooted = true;

		ContextName = Name;
		Context = Rml::CreateContext(ContextName, Rml::Vector2i(800, 200));
		return Test.TestNotNull(TEXT("Context"), Context);
	}

	~FRig()
	{
		if (Context != nullptr)
		{
			Rml::RemoveContext(ContextName);
		}

		if (bBooted)
		{
			FVaCuusEngine::Get().Shutdown();
		}
	}
};

static Rml::FontFaceHandle GetFace(int Size)
{
	return Rml::GetFontEngineInterface()->GetFontFaceHandle(GFamily, Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Normal, Size);
}

/** The top edge of the one quad GenerateString draws for a single glyph with its pen at PenX. */
static bool DrawGlyph(Rml::Context& Context, Rml::FontFaceHandle Face, const Rml::String& Glyph, float PenX,
	const Rml::TextShapingContext& Shaping, Rml::Vertex& OutTopLeft, Rml::Vertex& OutTopRight)
{
	Rml::TexturedMeshList Meshes;
	Rml::GetFontEngineInterface()->GenerateString(Context.GetRenderManager(), Face, 0, Glyph, Rml::Vector2f(PenX, 40.f),
		Rml::ColourbPremultiplied(255, 255), 1.f, Shaping, Meshes);

	const Rml::Mesh* Quad = nullptr;
	for (const Rml::TexturedMesh& Mesh : Meshes)
	{
		if (Mesh.mesh.vertices.empty())
		{
			continue;
		}

		if (Quad != nullptr)
		{
			return false;
		}
		Quad = &Mesh.mesh;
	}

	// MeshUtilities::GenerateQuad writes top left, top right, bottom right, bottom left.
	if (Quad == nullptr || Quad->vertices.size() != 4)
	{
		return false;
	}

	OutTopLeft = Quad->vertices[0];
	OutTopRight = Quad->vertices[1];
	return true;
}
}	 // namespace VaCuusFontSubpixelTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusFontSubpixelLetterSpacingTest, "VaCuus.Font.Subpixel.LetterSpacing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusFontSubpixelLetterSpacingTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusFontSubpixelTest;

	FRig Rig;
	if (!Rig.Boot(*this, "vacuus_font_subpixel_spacing"))
	{
		return false;
	}

	const Rml::FontFaceHandle Face = GetFace(16);
	if (!TestTrue(TEXT("LatoLatin at 16 px"), Face != 0))
	{
		return false;
	}

	const Rml::String Language;
	const Rml::String Text = "INTERACTIVE";
	const Rml::TextShapingContext Bare{ Language, Rml::Style::Direction::Auto, Rml::Style::FontKerning::None, 0.f };
	const Rml::TextShapingContext Spaced{ Language, Rml::Style::Direction::Auto, Rml::Style::FontKerning::None, 0.5f };

	Rml::FontEngineInterface* FontEngine = Rml::GetFontEngineInterface();
	const int32 BareWidth = FontEngine->GetStringWidth(Face, Text, Bare);
	const int32 SpacedWidth = FontEngine->GetStringWidth(Face, Text, Spaced);

	if (!TestTrue(TEXT("the bare string has a width"), BareWidth > 0))
	{
		return false;
	}

	// Eleven glyphs at half a pixel each is 5.5 px; both sums are rounded up, so the difference is 5 or 6.
	const int32 Added = SpacedWidth - BareWidth;
	if (Added < 5 || Added > 6)
	{
		AddError(FString::Printf(TEXT("0.5 px of letter-spacing over %d glyphs added %d px (%d -> %d); 5 or 6 expected, 0 means "
			"the spacing is cast to int again"), static_cast<int32>(Text.size()), Added, BareWidth, SpacedWidth));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusFontSubpixelFractionalPenTest, "VaCuus.Font.Subpixel.FractionalPen",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusFontSubpixelFractionalPenTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusFontSubpixelTest;

	FRig Rig;
	if (!Rig.Boot(*this, "vacuus_font_subpixel_pen"))
	{
		return false;
	}

	const Rml::FontFaceHandle Face = GetFace(16);
	if (!TestTrue(TEXT("LatoLatin at 16 px"), Face != 0))
	{
		return false;
	}

	const Rml::String Language;
	const Rml::TextShapingContext Shaping{ Language, Rml::Style::Direction::Auto, Rml::Style::FontKerning::None, 0.f };
	const Rml::String Glyph = "H";
	constexpr float BasePen = 10.f;

	Rml::Vertex BaseLeft{};
	Rml::Vertex BaseRight{};
	if (!TestTrue(TEXT("one quad for the glyph on a whole pen"), DrawGlyph(*Rig.Context, Face, Glyph, BasePen, Shaping, BaseLeft, BaseRight)))
	{
		return false;
	}

	const float QuadWidth = BaseRight.position.x - BaseLeft.position.x;
	if (!TestTrue(TEXT("the glyph's quad has a width"), QuadWidth > 0.f))
	{
		return false;
	}

	const float TexelU = (BaseRight.tex_coord.x - BaseLeft.tex_coord.x) / QuadWidth;
	if (!TestTrue(TEXT("the glyph's quad spans texels"), TexelU > 0.f))
	{
		return false;
	}

	const float Fractions[] = { 0.25f, 0.5f, 0.75f };
	for (const float Fraction : Fractions)
	{
		Rml::Vertex Left{};
		Rml::Vertex Right{};
		if (!TestTrue(FString::Printf(TEXT("one quad for the glyph at pen + %.2f"), Fraction),
				DrawGlyph(*Rig.Context, Face, Glyph, BasePen + Fraction, Shaping, Left, Right)))
		{
			return false;
		}

		const float Shift = (Left.position.x - BaseLeft.position.x) - (Left.tex_coord.x - BaseLeft.tex_coord.x) / TexelU;
		if (!FMath::IsNearlyEqual(Shift, Fraction, 1e-3f))
		{
			AddError(FString::Printf(TEXT("a pen moved by %.2f px moved the glyph by %.4f px (quad x %.2f -> %.2f, u %.6f -> %.6f); "
				"0 or 1 means the quad is rounded to the grid again"), Fraction, Shift, BaseLeft.position.x, Left.position.x,
				BaseLeft.tex_coord.x, Left.tex_coord.x));
		}

		TestEqual(FString::Printf(TEXT("the quad at pen + %.2f stays on a whole pixel"), Fraction), Left.position.x,
			FMath::RoundToFloat(Left.position.x));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusFontSubpixelLineWidthTest, "VaCuus.Font.Subpixel.LineWidth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusFontSubpixelLineWidthTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusFontSubpixelTest;

	FRig Rig;
	if (!Rig.Boot(*this, "vacuus_font_subpixel_line"))
	{
		return false;
	}

	static const char* GDocument = R"(<rml>
<head><style>body { display: block; font-family: LatoLatin; font-size: 13px; letter-spacing: 0.37px; } div { display: block; }</style></head>
<body><div id="t">Salvage the wreck before the hull drifts into the asteroid field again</div></body>
</rml>)";

	Rml::ElementDocument* Document = Rig.Context->LoadDocumentFromMemory(GDocument, "vacuus://font_subpixel_line.rml");
	if (!TestNotNull(TEXT("Document"), Document))
	{
		return false;
	}
	Document->Show();
	Rig.Context->Update();

	Rml::Element* Div = Document->GetElementById("t");
	Rml::ElementText* Text = Div != nullptr ? rmlui_dynamic_cast<Rml::ElementText*>(Div->GetFirstChild()) : nullptr;
	if (!TestNotNull(TEXT("the text node"), Text) || !TestTrue(TEXT("the text has a face"), Text->GetFontFaceHandle() != 0))
	{
		return false;
	}

	Rml::String Line;
	int LineLength = 0;
	float LineWidth = 0.f;
	Text->GenerateLine(Line, LineLength, LineWidth, 0, 100000.f, 0.f, true, true, false);

	if (!TestEqual(TEXT("the whole text fits one line"), LineLength, static_cast<int32>(Text->GetText().size())))
	{
		return false;
	}

	// ComputedValues::letter_spacing() is out of line in a class VaCuusRml does not export, so a modular build cannot link
	// it; this is its body on the exported Element API.
	const Rml::Property* LetterSpacing = Text->GetProperty(Rml::PropertyId::LetterSpacing);
	if (!TestNotNull(TEXT("the text inherits a letter-spacing"), LetterSpacing))
	{
		return false;
	}

	const Rml::ComputedValues& Computed = Text->GetComputedValues();
	const Rml::TextShapingContext Shaping{ Computed.language(), Computed.direction(), Computed.font_kerning(),
		Text->ResolveLength(LetterSpacing->GetNumericValue()) };

	Rml::FontEngineInterface* FontEngine = Rml::GetFontEngineInterface();
	Rml::TexturedMeshList Meshes;
	const int32 Drawn = FontEngine->GenerateString(Rig.Context->GetRenderManager(), Text->GetFontFaceHandle(), 0, Line, Rml::Vector2f(0.f, 20.f),
		Rml::ColourbPremultiplied(255, 255), 1.f, Shaping, Meshes);

	// THE CONTROL: the tokens as GenerateLine builds them (a word, then a space and a word), each measured on its own
	// and rounded up, as upstream summed them.
	TArray<FString> Words;
	FString(UTF8_TO_TCHAR(Line.c_str())).ParseIntoArray(Words, TEXT(" "));
	int32 TokenSum = 0;
	for (int32 Index = 0; Index < Words.Num(); ++Index)
	{
		const FString Token = Index == 0 ? Words[Index] : TEXT(" ") + Words[Index];
		TokenSum += FontEngine->GetStringWidth(Text->GetFontFaceHandle(), Rml::String(TCHAR_TO_UTF8(*Token)), Shaping);
	}

	if (!TestTrue(FString::Printf(TEXT("the text tells the measurements apart: %d words summed to %d px, drawn %d px"), Words.Num(), TokenSum,
			Drawn), Words.Num() > 1 && TokenSum > Drawn))
	{
		return false;
	}

	// THE ASSERTION.
	TestEqual(TEXT("GenerateLine measures the line GenerateString draws"), LineWidth, static_cast<float>(Drawn));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusFontSubpixelAdvanceTest, "VaCuus.Font.Subpixel.Advance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusFontSubpixelAdvanceTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusFontSubpixelTest;

	FRig Rig;
	if (!Rig.Boot(*this, "vacuus_font_subpixel_advance"))
	{
		return false;
	}

	const Rml::FontFaceHandle Face = GetFace(16);
	if (!TestTrue(TEXT("LatoLatin at 16 px"), Face != 0))
	{
		return false;
	}

	const Rml::String Language;
	const Rml::TextShapingContext Bare{ Language, Rml::Style::Direction::Auto, Rml::Style::FontKerning::None, 0.f };
	const Rml::String Run(100, 'H');

	// 100 x 1527 / 2000 x 16 = 1221.6, rounded up once for the whole run.
	TestEqual(TEXT("a hundred H measure a hundred of the face's own advances, rounded up once (1200 means the hinted advance again)"),
		Rml::GetFontEngineInterface()->GetStringWidth(Face, Run, Bare), 1222);

	return true;
}

namespace VaCuusFontSubpixelTest
{
/** What kerning adds to Text, in whole pixels: the kerned width less the bare one. */
static int32 KerningOver(Rml::FontFaceHandle Face, const Rml::String& Text)
{
	const Rml::String Language;
	const Rml::TextShapingContext Kerned{ Language, Rml::Style::Direction::Auto, Rml::Style::FontKerning::Normal, 0.f };
	const Rml::TextShapingContext Bare{ Language, Rml::Style::Direction::Auto, Rml::Style::FontKerning::None, 0.f };

	Rml::FontEngineInterface* FontEngine = Rml::GetFontEngineInterface();
	return FontEngine->GetStringWidth(Face, Text, Kerned) - FontEngine->GetStringWidth(Face, Text, Bare);
}
}	 // namespace VaCuusFontSubpixelTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusFontSubpixelKerningTest, "VaCuus.Font.Subpixel.Kerning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusFontSubpixelKerningTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusFontSubpixelTest;

	FRig Rig;
	if (!Rig.Boot(*this, "vacuus_font_subpixel_kerning"))
	{
		return false;
	}

	const Rml::FontFaceHandle Large = GetFace(50);
	const Rml::FontFaceHandle Small = GetFace(10);
	if (!TestTrue(TEXT("LatoLatin at 50 and 10 px"), Large != 0 && Small != 0))
	{
		return false;
	}

	// Twenty "To": 39 kerned pairs of about -0.12 em.
	Rml::String Text;
	for (int32 Index = 0; Index < 20; ++Index)
	{
		Text += "To";
	}

	const int32 AtLarge = KerningOver(Large, Text);
	const int32 AtSmall = KerningOver(Small, Text);

	// THE CONTROL: at 50 px, past DEFAULT's damping, the face kerns this text by about -239 px either way, so the claim
	// below is about the small size and not about a face that never kerns.
	if (!TestTrue(FString::Printf(TEXT("LatoLatin kerns the run at 50 px (%d px)"), AtLarge), AtLarge <= -200))
	{
		return false;
	}

	// THE ASSERTION. Each width is rounded up once, so each difference is off by less than a pixel, and the scaled one by
	// less than a fifth of one.
	const float Expected = AtLarge * 10.f / 50.f;
	if (FMath::Abs(AtSmall - Expected) > 1.5f)
	{
		AddError(FString::Printf(TEXT("kerning at 10 px is %d px, %.1f px expected from %d px at 50 px; 0 means the pairs are damped "
									  "below 25 ppem and rounded to whole pixels again"),
			AtSmall, Expected, AtLarge));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusFontSubpixelFallbackAdvanceTest, "VaCuus.Font.Subpixel.FallbackAdvance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusFontSubpixelFallbackAdvanceTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusFontSubpixelTest;

	// Declared before the rig, so it outlives Rml::Shutdown: RmlUi reads a face loaded from memory until then (Core.h:133).
	TArray<uint8> FallbackBytes;
	const FString FallbackPath = FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf");
	if (!TestTrue(FString::Printf(TEXT("the engine's %s is readable"), *FallbackPath), FFileHelper::LoadFileToArray(FallbackBytes, *FallbackPath)))
	{
		return false;
	}

	FRig Rig;
	if (!Rig.Boot(*this, "vacuus_font_subpixel_fallback"))
	{
		return false;
	}

	const Rml::String FallbackFamily = "vacuustestfallback";
	if (!TestTrue(TEXT("Roboto loads as a fallback face"),
			Rml::LoadFontFace(Rml::Span<const Rml::byte>(FallbackBytes.GetData(), FallbackBytes.Num()), FallbackFamily,
				Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Normal, /*fallback_face=*/true)))
	{
		return false;
	}

	const Rml::FontFaceHandle Face = GetFace(16);
	const Rml::FontFaceHandle Fallback = Rml::GetFontEngineInterface()->GetFontFaceHandle(FallbackFamily, Rml::Style::FontStyle::Normal,
		Rml::Style::FontWeight::Normal, 16);
	if (!TestTrue(TEXT("LatoLatin and the fallback at 16 px"), Face != 0 && Fallback != 0))
	{
		return false;
	}

	// U+0416, CYRILLIC CAPITAL LETTER ZHE: Roboto has it, LatoLatin does not.
	const Rml::String Zhe = Rml::StringUtilities::ToUTF8(static_cast<Rml::Character>(0x416));
	const Rml::String Language;
	const Rml::TextShapingContext Bare{ Language, Rml::Style::Direction::Auto, Rml::Style::FontKerning::None, 0.f };

	Rml::FontEngineInterface* FontEngine = Rml::GetFontEngineInterface();
	const int32 OwnWidth = FontEngine->GetStringWidth(Fallback, Zhe, Bare);
	const int32 Added = FontEngine->GetStringWidth(Face, "A" + Zhe + "A", Bare) - FontEngine->GetStringWidth(Face, "AA", Bare);

	// THE CONTROL: the glyph is the fallback's and not the 7 px box a missing one would be.
	if (!TestTrue(FString::Printf(TEXT("the fallback's own ZHE is wider than the replacement box (%d px)"), OwnWidth), OwnWidth > 8))
	{
		return false;
	}

	// THE ASSERTION. Both answers round up once, so the copied glyph adds its own width or a pixel less.
	if (Added < OwnWidth - 1 || Added > OwnWidth)
	{
		AddError(FString::Printf(TEXT("a ZHE taken from the fallback face added %d px between two A, %d or %d expected; 0 means the "
									  "weak copy lost its advance"),
			Added, OwnWidth - 1, OwnWidth));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusFontSubpixelReplacementAdvanceTest, "VaCuus.Font.Subpixel.ReplacementAdvance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusFontSubpixelReplacementAdvanceTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusFontSubpixelTest;

	FRig Rig;
	if (!Rig.Boot(*this, "vacuus_font_subpixel_replacement"))
	{
		return false;
	}

	const Rml::FontFaceHandle Face = GetFace(16);
	if (!TestTrue(TEXT("LatoLatin at 16 px"), Face != 0))
	{
		return false;
	}

	// U+4E00: no loaded face has it, so it draws as the engine's own replacement box.
	const Rml::String Missing = Rml::StringUtilities::ToUTF8(static_cast<Rml::Character>(0x4E00));
	const Rml::String Language;
	const Rml::TextShapingContext Bare{ Language, Rml::Style::Direction::Auto, Rml::Style::FontKerning::None, 0.f };

	Rml::FontEngineInterface* FontEngine = Rml::GetFontEngineInterface();
	const int32 Added = FontEngine->GetStringWidth(Face, "x" + Missing + "x", Bare) - FontEngine->GetStringWidth(Face, "xx", Bare);

	// 16 / 3 + 2 is whole, so it survives the rounding exactly.
	TestEqual(TEXT("the replacement box adds its advance between two x (0 means it lost it)"), Added, 7);

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
