// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusDefines.h"
#include "VaCuusModelChannel.h"
#include "VaCuusModelLayout.h"
#include "VaCuusModelLayoutTestTypes.h"
#include "VaCuusModelSampler.h"
#include "VaCuusModelShadow.h"

#include "HAL/PlatformTime.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusModelSamplerTest
{
/**
 * The whole game-thread pipeline for one model, plus the UI-side shadow the channel feeds.
 *
 * The applier is bit-driven, exactly as Task 6's will be: a slot's non-dirty fields hold
 * whatever an earlier publish left there, so an applier that copied the struct would agree
 * with every assertion below for the wrong reason.
 */
struct FPipeline
{
	explicit FPipeline(const UScriptStruct* InStruct)
		: Layout(InStruct)
		, Sampler(Layout)
		, Channel(Layout)
		, UIShadow(InStruct)
	{
	}

	/** One frame: diff, publish, apply. Returns the field indices the UI was told about. */
	template <typename ModelType>
	TArray<int32> RunFrame(const ModelType& Live)
	{
		Sampler.Sample(ModelType::StaticStruct(), &Live, Channel);

		TArray<int32> Applied;
		if (Channel.Publish(Sampler.GetShadow()))
		{
			Channel.ConsumeUpdate(
				[this, &Applied](const FVaCuusModelUpdate& Update)
				{
					const TConstArrayView<FVaCuusModelField> Fields = Layout.GetFields();
					for (TConstSetBitIterator<> It(Update.DirtyFields); It; ++It)
					{
						Fields[It.GetIndex()].CopyValue(UIShadow.GetData(), Update.Values.GetData());
						Applied.Add(It.GetIndex());
					}
				});
		}

		return Applied;
	}

	FVaCuusModelLayout Layout;
	FVaCuusModelSampler Sampler;
	FVaCuusModelChannel Channel;
	FVaCuusModelShadow UIShadow;
};

static int32 IndexOf(const FVaCuusModelLayout& Layout, const TCHAR* WireName)
{
	const TConstArrayView<FVaCuusModelField> Fields = Layout.GetFields();
	for (int32 Index = 0; Index < Fields.Num(); ++Index)
	{
		if (Fields[Index].WireName == WireName)
		{
			return Index;
		}
	}

	return INDEX_NONE;
}

/** Reads a field out of any instance of the model type, live or shadow. */
template <typename T>
static const T& Read(const FVaCuusModelLayout& Layout, const void* Base, const TCHAR* WireName)
{
	const FVaCuusModelField* Field = Layout.FindField(WireName);
	check(Field != nullptr);
	return *Field->Property->ContainerPtrToValuePtr<T>(Field->ContainerPtr(Base));
}

static bool ReadBool(const FVaCuusModelLayout& Layout, const void* Base, const TCHAR* WireName)
{
	const FVaCuusModelField* Field = Layout.FindField(WireName);
	check(Field != nullptr);
	return CastFieldChecked<FBoolProperty>(Field->Property)
		->GetPropertyValue(Field->Property->ContainerPtrToValuePtr<void>(Field->ContainerPtr(Base)));
}

/** Every field the fixture has, paired with a mutation that changes exactly that one. */
struct FKindCase
{
	const TCHAR* WireName;
	TFunction<void(FVaCuusLayoutTestModel&)> Mutate;

	/** Only when two cases drive the same field, so the assertion messages stay distinguishable. */
	const TCHAR* Label = nullptr;
};

static TArray<FKindCase> MakeKindCases()
{
	return {
		{TEXT("Ratio"), [](FVaCuusLayoutTestModel& M) { M.Ratio = 0.25f; }},
		{TEXT("Score"), [](FVaCuusLayoutTestModel& M) { M.Score = 7; }},
		{TEXT("Level"), [](FVaCuusLayoutTestModel& M) { M.Level = 3; }},
		{TEXT("bNativeBool"), [](FVaCuusLayoutTestModel& M) { M.bNativeBool = true; }},
		{TEXT("bBitfieldBool"), [](FVaCuusLayoutTestModel& M) { M.bBitfieldBool = 1; }},
		{TEXT("bBitfieldTwo"), [](FVaCuusLayoutTestModel& M) { M.bBitfieldTwo = 1; }},
		{TEXT("Title"), [](FVaCuusLayoutTestModel& M) { M.Title = TEXT("armour"); }},
		{TEXT("Tag"), [](FVaCuusLayoutTestModel& M) { M.Tag = TEXT("armour"); }},
		{TEXT("Caption"), [](FVaCuusLayoutTestModel& M) { M.Caption = FText::FromString(TEXT("armour")); }},
		{TEXT("Utf8Note"), [](FVaCuusLayoutTestModel& M) { M.Utf8Note = FUtf8String(UTF8TEXT("armour")); }},
		{TEXT("AnsiNote"), [](FVaCuusLayoutTestModel& M) { M.AnsiNote = FAnsiString("armour"); }},
		{TEXT("Colour"), [](FVaCuusLayoutTestModel& M) { M.Colour = EVaCuusTestColour::Green; }},
		{TEXT("Icon"), [](FVaCuusLayoutTestModel& M) { M.Icon = TSoftObjectPtr<UObject>(FSoftObjectPath(TEXT("/Game/UI/Icon.Icon"))); }},
		{TEXT("Origin.X"), [](FVaCuusLayoutTestModel& M) { M.Origin.X = 5.f; }},
		{TEXT("Origin.Y"), [](FVaCuusLayoutTestModel& M) { M.Origin.Y = 5.f; }},

		// NaN, AND IT IS FRAME 4 OF THE RIG THAT MATTERS -- "the change settles".
		//
		// `NaN != NaN` is true for every pair, including a value compared with itself, so the
		// obvious `!=` differ reports this field changed on EVERY frame from here on: one
		// publish, one DirtyVariable and one view re-evaluation per frame for a value that is
		// not moving. That is spec 9's idle row lost to a comparison rather than to a change,
		// and it is invisible without this case -- the sampler compares bit patterns
		// (FMemory::Memcmp) precisely to prevent it, and before this line no test anywhere set
		// a float to NaN, so reverting that Memcmp to `!=` broke nothing.
		//
		// A percentage over a zero maximum is the everyday way a UI-bound float goes NaN.
		{TEXT("Ratio"), [](FVaCuusLayoutTestModel& M) { M.Ratio = std::numeric_limits<float>::quiet_NaN(); },
			TEXT("Ratio (NaN)")},
	};
}
}	 // namespace VaCuusModelSamplerTest

/**
 * ONE CASE PER KIND: change exactly one field, and exactly its bit must set.
 *
 * Both halves matter and they fail differently. A missing bit is a value that never reaches
 * the screen and never will -- this milestone's signature failure, and completely silent
 * because the idle gate correctly withholds a frame that did not change. An EXTRA bit is a
 * publish, a DirtyVariable and a view re-evaluation every frame for a value that did not move,
 * which is spec 9's idle row quietly undone.
 *
 * Every kind is here rather than a representative sample because the kinds do not share a
 * comparison: four of them have a plausible wrong implementation that a same-kind test would
 * still pass (see the .cpp), and a fifth -- the bitfields -- fails through its NEIGHBOUR.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelSamplerPerKindTest, "VaCuus.Model.Sampler.PerKind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelSamplerPerKindTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelSamplerTest;

	for (const FKindCase& Case : MakeKindCases())
	{
		// Two cases drive `Ratio` (an ordinary value and a NaN), so the messages carry the
		// case's label rather than its field name -- otherwise a failure names a field that
		// appears twice and says nothing about which mutation produced it.
		const TCHAR* const Name = Case.Label != nullptr ? Case.Label : Case.WireName;

		FPipeline Pipeline(FVaCuusLayoutTestModel::StaticStruct());
		const int32 Expected = IndexOf(Pipeline.Layout, Case.WireName);
		if (!TestTrue(*FString::Printf(TEXT("the fixture has '%s'"), Name), Expected != INDEX_NONE))
		{
			continue;
		}

		FVaCuusLayoutTestModel Live;

		// Frame 1 is the forced full publish (I1); it says nothing about the differ.
		Pipeline.RunFrame(Live);

		// Frame 2, unchanged: the differ must find nothing, and nothing must be published.
		const TArray<int32> Quiet = Pipeline.RunFrame(Live);
		TestEqual(*FString::Printf(TEXT("'%s': an unchanged frame publishes nothing"), Name), Quiet.Num(), 0);

		// Frame 3: exactly one field moves.
		Case.Mutate(Live);
		const TArray<int32> Changed = Pipeline.RunFrame(Live);

		if (TestEqual(*FString::Printf(TEXT("'%s': exactly one field is marked"), Name), Changed.Num(), 1))
		{
			TestEqual(*FString::Printf(TEXT("'%s': and it is the one that changed"), Name), Changed[0], Expected);
		}
		else
		{
			// Name the extras, because "2 != 1" on its own does not say which neighbour leaked.
			for (const int32 Index : Changed)
			{
				if (Index != Expected)
				{
					AddError(FString::Printf(TEXT("'%s' also marked '%s'"), Name, *Pipeline.Layout.GetFields()[Index].WireName));
				}
			}
		}

		// Frame 4: quiet again. A differ that stored the wrong thing (or nothing) into its
		// shadow would keep re-reporting the same field forever, which is a publish per frame.
		// This is also the ONLY assertion the NaN case can fail: `NaN != NaN` still reports the
		// change on frame 3, and only stops settling here.
		const TArray<int32> Settled = Pipeline.RunFrame(Live);
		TestEqual(*FString::Printf(TEXT("'%s': the change settles"), Name), Settled.Num(), 0);
	}

	return true;
}

/**
 * THE BITFIELD TRAP, given its own test because it is the one kind whose false positive comes
 * from a NEIGHBOUR rather than from itself.
 *
 * `uint8 b : 1` stores its Offset_Internal pointing at the containing integer and its
 * GetElementSize() as that integer's size (PropertyBool.cpp:67-93), so bBitfieldBool and
 * bBitfieldTwo share both. Any comparison keyed on (offset, size) -- a memcmp of the value
 * bytes, or of a scratch buffer copied out of them -- reports BOTH as changed when either
 * moves, and the copy back has to be masked or it drags the neighbour's old bit along.
 *
 * Two assertions, because a memcmp differ passes one of them: the bit set, and the neighbour's
 * VALUE surviving the round trip into the UI shadow.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelSamplerBitfieldTest, "VaCuus.Model.Sampler.Bitfields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelSamplerBitfieldTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelSamplerTest;

	FPipeline Pipeline(FVaCuusLayoutTestModel::StaticStruct());
	const int32 FirstIndex = IndexOf(Pipeline.Layout, TEXT("bBitfieldBool"));
	const int32 SecondIndex = IndexOf(Pipeline.Layout, TEXT("bBitfieldTwo"));

	// The premise, asserted rather than assumed -- if the two ever stopped sharing storage this
	// test would still pass while testing nothing.
	const FVaCuusModelField* FirstField = Pipeline.Layout.FindField(TEXT("bBitfieldBool"));
	const FVaCuusModelField* SecondField = Pipeline.Layout.FindField(TEXT("bBitfieldTwo"));
	if (!TestTrue(TEXT("both bitfields resolved"), FirstField != nullptr && SecondField != nullptr))
	{
		return false;
	}
	TestEqual(TEXT("the two bitfields share one storage offset"), SecondField->Property->GetOffset_ForInternal(),
		FirstField->Property->GetOffset_ForInternal());

	FVaCuusLayoutTestModel Live;
	Pipeline.RunFrame(Live);

	// Set the FIRST bitfield, and the second must be untouched in both senses.
	Live.bBitfieldBool = 1;
	const TArray<int32> AfterFirst = Pipeline.RunFrame(Live);
	TestEqual(TEXT("only the first bitfield is marked"), AfterFirst.Num(), 1);
	if (AfterFirst.Num() == 1)
	{
		TestEqual(TEXT("and it is the first one"), AfterFirst[0], FirstIndex);
	}
	TestTrue(TEXT("the first bitfield arrived"), ReadBool(Pipeline.Layout, Pipeline.UIShadow.GetData(), TEXT("bBitfieldBool")));
	TestFalse(TEXT("the second bitfield is still false"), ReadBool(Pipeline.Layout, Pipeline.UIShadow.GetData(), TEXT("bBitfieldTwo")));

	// Now the SECOND, and the first must survive the masked copy.
	Live.bBitfieldTwo = 1;
	const TArray<int32> AfterSecond = Pipeline.RunFrame(Live);
	TestEqual(TEXT("only the second bitfield is marked"), AfterSecond.Num(), 1);
	if (AfterSecond.Num() == 1)
	{
		TestEqual(TEXT("and it is the second one"), AfterSecond[0], SecondIndex);
	}
	TestTrue(TEXT("the first bitfield survived the neighbour's copy"),
		ReadBool(Pipeline.Layout, Pipeline.UIShadow.GetData(), TEXT("bBitfieldBool")));
	TestTrue(TEXT("and the second arrived"), ReadBool(Pipeline.Layout, Pipeline.UIShadow.GetData(), TEXT("bBitfieldTwo")));

	// Clearing one is a change too, and must not disturb the other.
	Live.bBitfieldBool = 0;
	const TArray<int32> AfterClear = Pipeline.RunFrame(Live);
	TestEqual(TEXT("clearing the first marks only the first"), AfterClear.Num(), 1);
	TestFalse(TEXT("the first cleared"), ReadBool(Pipeline.Layout, Pipeline.UIShadow.GetData(), TEXT("bBitfieldBool")));
	TestTrue(TEXT("the second is still set"), ReadBool(Pipeline.Layout, Pipeline.UIShadow.GetData(), TEXT("bBitfieldTwo")));

	return true;
}

/**
 * CASE-ONLY CHANGES, for every kind whose shipped value is a string.
 *
 * Each of them ships its DISPLAY form to RmlUi, and each of them has an obvious comparison that
 * is case-INSENSITIVE: FString/FUtf8String/FAnsiString all get operator== from one template
 * whose implementation is `Equals(Rhs, ESearchCase::IgnoreCase)` (UnrealString.h.inl:906-915),
 * and FName::operator== compares the case-insensitive comparison index (NameTypes.h:1624-1626).
 * "hp" -> "HP" is then a change the screen shows and the differ denies -- one stale label,
 * forever, with no diagnostic.
 *
 * FSoftObjectPath BELONGS HERE FOR THE SAME REASON, and this is the case that was missing: its
 * operator== is `AssetPath == Other.AssetPath && SubPathString == Other.SubPathString`
 * (SoftObjectPath.cpp:590-593) -- two FName compares plus an FUtf8String compare, and the last
 * of those is Stricmp again. The UI ships FSoftObjectPtr::ToString(), i.e. the display form, so
 * a sub-path that differs only in case renders differently and compares equal. Before this the
 * only soft-reference mutation in any test was empty -> populated, which every candidate
 * comparison catches, so FVaCuusModelSampler's componentwise case-sensitive compare could be
 * reverted to operator== with nothing failing anywhere.
 *
 * THE SUB-PATH IS WHAT IS MUTATED, DELIBERATELY. A case-only change in the package or asset
 * name would be undetectable without WITH_CASE_PRESERVING_NAME (it is WITH_EDITORONLY_DATA,
 * NameTypes.h:32-33) and would not render differently in a cooked build either -- the same
 * caveat FName carries below. SubPathString is an FUtf8String and always keeps its case, so
 * this assertion holds in every configuration.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelSamplerStringCaseTest, "VaCuus.Model.Sampler.StringCase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelSamplerStringCaseTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelSamplerTest;

	FPipeline Pipeline(FVaCuusLayoutTestModel::StaticStruct());

	// NOT TestEqual, AND THAT IS THE WHOLE POINT OF THIS TEST ARRIVING THROUGH THE FRAMEWORK.
	// FAutomationTestBase::TestEqual on strings is FCString::Stricmp
	// (AutomationTest.cpp:2161-2171), i.e. case-INSENSITIVE -- the same trap the sampler is
	// defending against, one layer up. Every read-back below has to compare case-sensitively or
	// it passes on the stale string and asserts nothing at all. (Found the hard way: with the
	// componentwise compare deliberately reverted, the "a case-only change is a change"
	// assertion failed and the read-back assertion next to it still passed.)
	auto TestEqualCased = [this](const TCHAR* What, const FString& Actual, const FString& Expected)
	{
		return TestTrue(*FString::Printf(TEXT("%s (expected '%s', got '%s')"), What, *Expected, *Actual),
			Actual.Equals(Expected, ESearchCase::CaseSensitive));
	};

	FVaCuusLayoutTestModel Live;
	Live.Title = TEXT("hp");
	Live.Tag = TEXT("hp");
	Live.Caption = FText::FromString(TEXT("hp"));
	Live.Utf8Note = FUtf8String(UTF8TEXT("hp"));
	Live.AnsiNote = FAnsiString("hp");
	Live.Icon = TSoftObjectPtr<UObject>(FSoftObjectPath(TEXT("/Game/UI/Icons.Icons:hp")));
	Pipeline.RunFrame(Live);
	Pipeline.RunFrame(Live);

	// The premise, asserted rather than assumed: if the sub-path stopped surviving the round
	// trip into the shadow, the soft-reference assertion below would pass for the wrong reason.
	TestEqualCased(TEXT("the sub-path reached the UI shadow"),
		Read<TSoftObjectPtr<UObject>>(Pipeline.Layout, Pipeline.UIShadow.GetData(), TEXT("Icon")).ToString(),
		FString(TEXT("/Game/UI/Icons.Icons:hp")));

	// Case, and nothing else, changes on all six.
	Live.Title = TEXT("HP");
	Live.Tag = TEXT("HP");
	Live.Caption = FText::FromString(TEXT("HP"));
	Live.Utf8Note = FUtf8String(UTF8TEXT("HP"));
	Live.AnsiNote = FAnsiString("HP");
	Live.Icon = TSoftObjectPtr<UObject>(FSoftObjectPath(TEXT("/Game/UI/Icons.Icons:HP")));

	const TArray<int32> Changed = Pipeline.RunFrame(Live);

	auto WasMarked = [&Pipeline, &Changed](const TCHAR* WireName) { return Changed.Contains(IndexOf(Pipeline.Layout, WireName)); };

	TestTrue(TEXT("FString: a case-only change is a change"), WasMarked(TEXT("Title")));
	TestTrue(TEXT("FUtf8String: a case-only change is a change"), WasMarked(TEXT("Utf8Note")));
	TestTrue(TEXT("FAnsiString: a case-only change is a change"), WasMarked(TEXT("AnsiNote")));
	TestTrue(TEXT("FText: a case-only change is a change"), WasMarked(TEXT("Caption")));
	TestTrue(TEXT("FSoftObjectPath: a case-only sub-path change is a change"), WasMarked(TEXT("Icon")));

#if WITH_CASE_PRESERVING_NAME
	TestTrue(TEXT("FName: a case-only change is a change where the case is kept"), WasMarked(TEXT("Tag")));
#endif

	TestEqualCased(TEXT("the new casing reached the UI"),
		Read<FString>(Pipeline.Layout, Pipeline.UIShadow.GetData(), TEXT("Title")), FString(TEXT("HP")));
	TestEqualCased(TEXT("and so did the new sub-path casing"),
		Read<TSoftObjectPtr<UObject>>(Pipeline.Layout, Pipeline.UIShadow.GetData(), TEXT("Icon")).ToString(),
		FString(TEXT("/Game/UI/Icons.Icons:HP")));

	return true;
}

/**
 * FText, whose generic comparison is wrong in exactly the builds that ship.
 *
 * FTextProperty::Identical selects EIdenticalLexicalCompareMethod::None when !GIsEditor
 * (TextProperty.cpp:63-67) and then falls through to "the texts don't share the same pointer,
 * which means that they can't share the same identity" -> not equal (:119-120). So a label
 * rebuilt each frame from the same string -- FText::Format, FText::AsNumber, FromString on a
 * cached FString, all of them -- would be reported changed EVERY FRAME in a packaged build and
 * never in the editor. A heisenbug that only exists where nobody is attached to a debugger.
 *
 * The third assertion is about the projection: the shadow's FText must be the culture-invariant
 * form built on the game thread, because that is what makes the UI thread's read of it plain
 * value data rather than a call into the localization manager (see FVaCuusModelSampler's header).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelSamplerTextTest, "VaCuus.Model.Sampler.Text",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelSamplerTextTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelSamplerTest;

	FPipeline Pipeline(FVaCuusLayoutTestModel::StaticStruct());
	const int32 CaptionIndex = IndexOf(Pipeline.Layout, TEXT("Caption"));

	FVaCuusLayoutTestModel Live;
	Live.Caption = FText::FromString(TEXT("100 / 100"));
	Pipeline.RunFrame(Live);

	// A DIFFERENT FText OBJECT with the SAME display string: a new identity every time, which
	// is what any per-frame FText::Format produces.
	for (int32 Frame = 0; Frame < 5; ++Frame)
	{
		Live.Caption = FText::FromString(TEXT("100 / 100"));
		const TArray<int32> Changed = Pipeline.RunFrame(Live);
		TestEqual(TEXT("a rebuilt FText with the same display string is not a change"), Changed.Num(), 0);
	}

	// A real change still gets through.
	Live.Caption = FText::FromString(TEXT("90 / 100"));
	const TArray<int32> Changed = Pipeline.RunFrame(Live);
	if (TestEqual(TEXT("a new display string is a change"), Changed.Num(), 1))
	{
		TestEqual(TEXT("and it is Caption"), Changed[0], CaptionIndex);
	}
	TestEqual(TEXT("the new text reached the UI"),
		Read<FText>(Pipeline.Layout, Pipeline.UIShadow.GetData(), TEXT("Caption")).ToString(), FString(TEXT("90 / 100")));

	// THE PROJECTION. Both shadows must hold a text with no localization identity behind it, so
	// that nothing downstream can be made to consult FTextLocalizationManager.
	TestTrue(TEXT("the game shadow holds the culture-invariant projection"),
		Read<FText>(Pipeline.Layout, Pipeline.Sampler.GetShadow().GetData(), TEXT("Caption")).IsCultureInvariant());
	TestTrue(TEXT("and so does the UI shadow, because the channel copied it"),
		Read<FText>(Pipeline.Layout, Pipeline.UIShadow.GetData(), TEXT("Caption")).IsCultureInvariant());

	return true;
}

/**
 * SPEC 4 / INVARIANT I1, END TO END: a struct whose defaults differ from zero shows its
 * defaults on frame 1.
 *
 * The live instance is default-constructed and so is the sampler's shadow, so the differ is
 * RIGHT to mark nothing -- the assertion below checks that it marks nothing, because that is
 * the premise. What reaches the UI is then decided entirely by the channel starting fully
 * dirty. Without that, frame 1 publishes nothing, the UI side's values are whatever its own
 * construction produced, and the game thread has confirmed none of them; every later diff is
 * relative to a state nobody checked.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelSamplerFirstFrameTest, "VaCuus.Model.Sampler.FirstFrameDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelSamplerFirstFrameTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelSamplerTest;

	FPipeline Pipeline(FVaCuusSamplerDefaultsModel::StaticStruct());
	const int32 NumFields = Pipeline.Layout.GetFields().Num();

	const FVaCuusSamplerDefaultsModel Live;

	// The premise: the differ finds nothing, because the live struct and the shadow really are
	// equal -- UScriptStruct::InitializeStruct memzeroes and then runs the C++ constructor
	// (Class.cpp:3775-3800), so the shadow has the same 100.f that the live one has.
	FVaCuusModelChannel Probe(Pipeline.Layout);
	FVaCuusModelSampler ProbeSampler(Pipeline.Layout);
	TestEqual(TEXT("the differ marks nothing on frame 1, and is right to"),
		ProbeSampler.Sample(FVaCuusSamplerDefaultsModel::StaticStruct(), &Live, Probe), 0);

	const TArray<int32> Frame1 = Pipeline.RunFrame(Live);
	TestEqual(TEXT("frame 1 nevertheless delivers every field"), Frame1.Num(), NumFields);

	const void* UI = Pipeline.UIShadow.GetData();
	TestEqual(TEXT("Health is 100, not 0"), Read<float>(Pipeline.Layout, UI, TEXT("Health")), 100.f);
	TestEqual(TEXT("Ammo is 30, not 0"), Read<int32>(Pipeline.Layout, UI, TEXT("Ammo")), 30);
	TestTrue(TEXT("bAlive is true, not false"), ReadBool(Pipeline.Layout, UI, TEXT("bAlive")));
	TestTrue(TEXT("the bitfield is set, not clear"), ReadBool(Pipeline.Layout, UI, TEXT("bFlagged")));
	TestEqual(TEXT("Title is 'Ready', not empty"), Read<FString>(Pipeline.Layout, UI, TEXT("Title")), FString(TEXT("Ready")));
	TestEqual(TEXT("Tag is 'hp', not None"), Read<FName>(Pipeline.Layout, UI, TEXT("Tag")), FName(TEXT("hp")));
	TestEqual(TEXT("Caption is 'Ready', not empty"), Read<FText>(Pipeline.Layout, UI, TEXT("Caption")).ToString(), FString(TEXT("Ready")));
	TestEqual(TEXT("Colour is Blue, not Red"),
		int32(Read<EVaCuusTestColour>(Pipeline.Layout, UI, TEXT("Colour"))), int32(EVaCuusTestColour::Blue));

	// And frame 2 is quiet: the forced publish is a one-off, not a standing cost.
	TestEqual(TEXT("frame 2 publishes nothing"), Pipeline.RunFrame(Live).Num(), 0);

	return true;
}

/**
 * SPEC 9's game-thread budget row: sample + diff over 64 scalar fields, <= 0.02 ms.
 *
 * Reported rather than merely asserted, because the budget it sits inside (0.10 ms/frame) is
 * currently met by inference from margin rather than by complete measurement -- so the number
 * is the deliverable and the assertion is only a tripwire.
 *
 * THE CEILING IS DELIBERATELY LOOSE (10x the budget). This runs on whatever machine the suite
 * runs on, inside an editor that is doing other things, and a tight bound would fail for
 * reasons that have nothing to do with the differ. A 10x breach is a structural regression --
 * somebody reached for FStructProperty::Identical, or made the diff allocate -- not jitter.
 *
 * Two numbers, because they are the two frames that exist: the idle frame, which is what runs
 * on almost every frame of a real HUD, and the worst case where all 64 fields move at once.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelSamplerCostTest, "VaCuus.Model.Sampler.Cost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelSamplerCostTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelSamplerTest;

	FPipeline Pipeline(FVaCuusSamplerCostModel::StaticStruct());
	if (!TestEqual(TEXT("the cost fixture has 64 bound fields"), Pipeline.Layout.GetFields().Num(), 64))
	{
		return false;
	}

	FVaCuusSamplerCostModel Live;

	// Addressable members, so the mutation loop is a few lines rather than 64. The bitfields
	// are absent for the reason they are interesting at all: a bitfield has no address.
	float* const Floats[] = {&Live.F00, &Live.F01, &Live.F02, &Live.F03, &Live.F04, &Live.F05, &Live.F06, &Live.F07, &Live.F08,
		&Live.F09, &Live.F10, &Live.F11, &Live.F12, &Live.F13, &Live.F14, &Live.F15, &Live.F16, &Live.F17, &Live.F18, &Live.F19,
		&Live.F20, &Live.F21, &Live.F22, &Live.F23, &Live.F24, &Live.F25, &Live.F26, &Live.F27, &Live.F28, &Live.F29, &Live.F30,
		&Live.F31};
	int32* const Ints[] = {&Live.I00, &Live.I01, &Live.I02, &Live.I03, &Live.I04, &Live.I05, &Live.I06, &Live.I07, &Live.I08,
		&Live.I09, &Live.I10, &Live.I11, &Live.I12, &Live.I13, &Live.I14, &Live.I15};
	bool* const NativeBools[] = {&Live.bNative0, &Live.bNative1, &Live.bNative2, &Live.bNative3};
	FString* const Strings[] = {&Live.S0, &Live.S1, &Live.S2, &Live.S3};
	FName* const Names[] = {&Live.N0, &Live.N1};
	FText* const Texts[] = {&Live.T0, &Live.T1};

	// 32 + 16 + 4 native bools + 4 bitfields + 4 + 2 + 2 == 64, and the fixture's field count
	// was already asserted above -- this is the composition behind that number.
	static_assert(UE_ARRAY_COUNT(Floats) + UE_ARRAY_COUNT(Ints) + UE_ARRAY_COUNT(NativeBools) + 4 + UE_ARRAY_COUNT(Strings)
			+ UE_ARRAY_COUNT(Names) + UE_ARRAY_COUNT(Texts)
		== 64);

	auto ApplyOneUpdate = [&Pipeline]()
	{
		Pipeline.Channel.ConsumeUpdate(
			[&Pipeline](const FVaCuusModelUpdate& Update)
			{
				const TConstArrayView<FVaCuusModelField> Fields = Pipeline.Layout.GetFields();
				for (TConstSetBitIterator<> It(Update.DirtyFields); It; ++It)
				{
					Fields[It.GetIndex()].CopyValue(Pipeline.UIShadow.GetData(), Update.Values.GetData());
				}
			});
	};

	// Warm up: drain the forced first publish and let every allocation that will ever happen
	// happen -- the three slot buffers, the strings inside them, the bit arrays.
	for (int32 Warmup = 0; Warmup < 4; ++Warmup)
	{
		Live.S0 = FString::Printf(TEXT("warm %d"), Warmup);
		Live.T0 = FText::FromString(FString::Printf(TEXT("warm %d"), Warmup));
		Pipeline.Sampler.Sample(FVaCuusSamplerCostModel::StaticStruct(), &Live, Pipeline.Channel);
		Pipeline.Channel.Publish(Pipeline.Sampler.GetShadow());
		ApplyOneUpdate();
	}

	constexpr int32 Iterations = 2000;

	// (1) THE IDLE FRAME. Nothing changed, so this is 64 comparisons and a publish that
	// declines -- the cost a bound model imposes on every frame it does not change, which is
	// almost all of them.
	double IdleSeconds = 0.0;
	for (int32 It = 0; It < Iterations; ++It)
	{
		const double Start = FPlatformTime::Seconds();
		Pipeline.Sampler.Sample(FVaCuusSamplerCostModel::StaticStruct(), &Live, Pipeline.Channel);
		Pipeline.Channel.Publish(Pipeline.Sampler.GetShadow());
		IdleSeconds += FPlatformTime::Seconds() - Start;
	}

	// (2) THE WORST CASE. Every one of the 64 fields moves, so this is 64 comparisons, 64
	// shadow writes and 64 copies into the slot. Mutation happens outside the timer; the
	// consume does too, because it is UI-thread work and this budget is the game thread's.
	double BusySeconds = 0.0;
	for (int32 It = 0; It < Iterations; ++It)
	{
		const bool bFlag = (It & 1) != 0;
		for (float* Field : Floats)
		{
			*Field = float(It) + 1.f;
		}
		for (int32* Field : Ints)
		{
			*Field = It + 1;
		}
		for (bool* Field : NativeBools)
		{
			*Field = bFlag;
		}

		Live.bBit0 = bFlag;
		Live.bBit1 = bFlag;
		Live.bBit2 = bFlag;
		Live.bBit3 = bFlag;

		const FString Text = FString::Printf(TEXT("value %d"), It);
		for (FString* Field : Strings)
		{
			*Field = Text;
		}
		for (FName* Field : Names)
		{
			*Field = FName(*Text);
		}
		for (FText* Field : Texts)
		{
			*Field = FText::FromString(Text);
		}

		const double Start = FPlatformTime::Seconds();
		Pipeline.Sampler.Sample(FVaCuusSamplerCostModel::StaticStruct(), &Live, Pipeline.Channel);
		Pipeline.Channel.Publish(Pipeline.Sampler.GetShadow());
		BusySeconds += FPlatformTime::Seconds() - Start;

		ApplyOneUpdate();
	}

	const double IdleMs = (IdleSeconds / Iterations) * 1000.0;
	const double BusyMs = (BusySeconds / Iterations) * 1000.0;

	const FString Report = FString::Printf(
		TEXT("64-field sample+diff+publish: idle %.5f ms/frame, every-field-changed %.5f ms/frame (%d iterations); budget 0.02 ms"),
		IdleMs, BusyMs, Iterations);
	AddInfo(Report);
	UE_LOG(LogVaCuus, Display, TEXT("VaCuus model cost: %s"), *Report);

	TestTrue(*FString::Printf(TEXT("the idle diff stays inside 10x the budget (%.5f ms)"), IdleMs), IdleMs < 0.2);
	TestTrue(*FString::Printf(TEXT("the every-field frame stays inside 10x the budget (%.5f ms)"), BusyMs), BusyMs < 0.2);

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
