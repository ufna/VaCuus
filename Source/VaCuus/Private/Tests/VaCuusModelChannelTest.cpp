// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusModelChannel.h"
#include "VaCuusModelLayout.h"
#include "VaCuusModelLayoutTestTypes.h"
#include "VaCuusModelShadow.h"

#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusModelChannelTest
{
/**
 * The game side of the pipeline, minus the differ: a shadow the test writes into by hand and
 * a channel that publishes out of it. Task 5's sampler replaces the "by hand" part; the
 * channel's contract does not depend on it, and testing it separately is what keeps this
 * file's failures attributable to the channel.
 */
struct FProducer
{
	explicit FProducer(const FVaCuusModelLayout& InLayout)
		: Shadow(InLayout.GetStruct())
		, Channel(InLayout)
	{
	}

	FVaCuusModelShadow Shadow;
	FVaCuusModelChannel Channel;
};

/** Index into GetFields(), by dotted wire name. Fails loudly rather than returning a wrong field. */
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

/** Writes a value into a model instance the way the sampler will: through the field's own addressing. */
template <typename PropertyType, typename ValueType>
static void Write(const FVaCuusModelLayout& Layout, void* Base, const TCHAR* WireName, ValueType Value)
{
	const FVaCuusModelField* Field = Layout.FindField(WireName);
	check(Field != nullptr);
	CastFieldChecked<PropertyType>(Field->Property)->SetPropertyValue_InContainer(Field->ContainerPtr(Base), Value);
}

template <typename PropertyType>
static auto Read(const FVaCuusModelLayout& Layout, const void* Base, const TCHAR* WireName)
{
	const FVaCuusModelField* Field = Layout.FindField(WireName);
	check(Field != nullptr);
	return CastFieldChecked<PropertyType>(Field->Property)->GetPropertyValue_InContainer(Field->ContainerPtr(Base));
}

/**
 * A native reference to a field's value inside a shadow instance -- how the array tests
 * write and read whole TArrays. Legal because the fixture is a native USTRUCT, so the shadow
 * really is an instance of it and the property's value pointer really is a TArray<T>; the
 * sampler tests take the same shortcut (VaCuusModelSamplerTest.cpp, Read<TArray<int32>>).
 * The REFERENCE stays valid across publishes -- the shadow buffer is malloc'd once and never
 * moves; only the array's element allocation does, which is spec 2(c)'s point.
 */
template <typename T>
static T& ValueRef(const FVaCuusModelLayout& Layout, void* Base, const TCHAR* WireName)
{
	const FVaCuusModelField* Field = Layout.FindField(WireName);
	check(Field != nullptr);
	return *Field->Property->ContainerPtrToValuePtr<T>(Field->ContainerPtr(Base));
}

/**
 * The UI side: exactly what Task 6's apply loop will be, minus DirtyVariable.
 *
 * DRIVEN BY THE BITS, never by the struct -- a slot's non-dirty fields hold whatever an
 * earlier publish left there, so an applier that copied the whole struct would pass the
 * regression test below while shipping stale values for every field it was not told about.
 */
static int32 ApplyInto(const FVaCuusModelLayout& Layout, const FVaCuusModelUpdate& Update, FVaCuusModelShadow& Target)
{
	const TConstArrayView<FVaCuusModelField> Fields = Layout.GetFields();
	int32 NumApplied = 0;
	for (TConstSetBitIterator<> It(Update.DirtyFields); It; ++It)
	{
		Fields[It.GetIndex()].CopyValue(Target.GetData(), Update.Values.GetData());
		++NumApplied;
	}

	return NumApplied;
}
}	 // namespace VaCuusModelChannelTest

/**
 * SPEC 4 / INVARIANT I2, AND THE REASON THIS FILE EXISTS: publish twice without consuming,
 * consume, then publish again -- and no field may regress.
 *
 * This is the exact shape TTripleBuffer recycles into. SwapWriteBuffers swaps the write index
 * with the TEMP index (TripleBuffer.h:182-191, :255-259), so the fourth publish below is
 * written into the buffer the FIRST publish went out in -- a buffer nobody ever cleared,
 * still carrying that publish's bits and its values. A channel that ORs its dirty bits into
 * the slot and only writes a field when that field changed will therefore announce
 * bit(Ratio) with the value 90 in a frame where Ratio has been 80 for some time, and the UI
 * will hold 90 until Ratio next changes.
 *
 * Nothing in that failure is loud: the bit was true, the applier was faithful to it, the
 * value was simply older than the bit.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelChannelNoRegressionTest, "VaCuus.Model.Channel.NoRegression",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelChannelNoRegressionTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelChannelTest;

	const FVaCuusModelLayout Layout(FVaCuusLayoutTestModel::StaticStruct());
	if (!TestTrue(TEXT("the layout resolved its struct"), Layout.IsValid()))
	{
		return false;
	}

	const int32 RatioIndex = IndexOf(Layout, TEXT("Ratio"));
	const int32 ScoreIndex = IndexOf(Layout, TEXT("Score"));
	if (!TestTrue(TEXT("the fixture has Ratio and Score"), RatioIndex != INDEX_NONE && ScoreIndex != INDEX_NONE))
	{
		return false;
	}

	FProducer Producer(Layout);
	FVaCuusModelShadow UIShadow(Layout.GetStruct());

	// Drain the forced full-bit first publish (I1) so what follows is about I2 alone.
	Write<FFloatProperty>(Layout, Producer.Shadow.GetData(), TEXT("Ratio"), 100.f);
	TestTrue(TEXT("the first publish goes out"), Producer.Channel.Publish(Producer.Shadow));
	Producer.Channel.ConsumeUpdate([&Layout, &UIShadow](const FVaCuusModelUpdate& Update) { ApplyInto(Layout, Update, UIShadow); });
	TestEqual(TEXT("the UI starts at 100"), Read<FFloatProperty>(Layout, UIShadow.GetData(), TEXT("Ratio")), 100.f);

	// (1) 100 -> 90, published, NOT consumed.
	Write<FFloatProperty>(Layout, Producer.Shadow.GetData(), TEXT("Ratio"), 90.f);
	Producer.Channel.MarkFieldDirty(RatioIndex);
	TestTrue(TEXT("publish 90"), Producer.Channel.Publish(Producer.Shadow));

	// (2) 90 -> 80, published into the OTHER slot, then consumed. The UI is now at 80 and the
	// producer holds the slot that publish (1) went out in.
	Write<FFloatProperty>(Layout, Producer.Shadow.GetData(), TEXT("Ratio"), 80.f);
	Producer.Channel.MarkFieldDirty(RatioIndex);
	TestTrue(TEXT("publish 80"), Producer.Channel.Publish(Producer.Shadow));

	TestTrue(TEXT("the consumer takes an update"),
		Producer.Channel.ConsumeUpdate([&Layout, &UIShadow](const FVaCuusModelUpdate& Update) { ApplyInto(Layout, Update, UIShadow); }));
	TestEqual(TEXT("latest-wins: the UI sees 80, not 90"), Read<FFloatProperty>(Layout, UIShadow.GetData(), TEXT("Ratio")), 80.f);

	// (3) A DIFFERENT field changes. Ratio is untouched, so nothing in a "write what changed"
	// producer would overwrite the 90 still sitting in the recycled slot.
	Write<FIntProperty>(Layout, Producer.Shadow.GetData(), TEXT("Score"), 7);
	Producer.Channel.MarkFieldDirty(ScoreIndex);
	TestTrue(TEXT("publish the Score change"), Producer.Channel.Publish(Producer.Shadow));

	TestTrue(TEXT("the consumer takes the Score update"),
		Producer.Channel.ConsumeUpdate([&Layout, &UIShadow](const FVaCuusModelUpdate& Update) { ApplyInto(Layout, Update, UIShadow); }));

	TestEqual(TEXT("Score arrived"), Read<FIntProperty>(Layout, UIShadow.GetData(), TEXT("Score")), 7);

	// THE ASSERTION. Whatever the third publish chose to carry, the value the UI holds for
	// Ratio may not go backwards.
	TestEqual(TEXT("no field regresses: Ratio is still 80"), Read<FFloatProperty>(Layout, UIShadow.GetData(), TEXT("Ratio")), 80.f);

	// And once more with the roles swapped, so the proof does not rest on one slot ordering:
	// two unconsumed publishes of Score followed by an unrelated change.
	Write<FIntProperty>(Layout, Producer.Shadow.GetData(), TEXT("Score"), 8);
	Producer.Channel.MarkFieldDirty(ScoreIndex);
	Producer.Channel.Publish(Producer.Shadow);

	Write<FIntProperty>(Layout, Producer.Shadow.GetData(), TEXT("Score"), 9);
	Producer.Channel.MarkFieldDirty(ScoreIndex);
	Producer.Channel.Publish(Producer.Shadow);

	Producer.Channel.ConsumeUpdate([&Layout, &UIShadow](const FVaCuusModelUpdate& Update) { ApplyInto(Layout, Update, UIShadow); });
	TestEqual(TEXT("the UI sees 9"), Read<FIntProperty>(Layout, UIShadow.GetData(), TEXT("Score")), 9);

	Write<FFloatProperty>(Layout, Producer.Shadow.GetData(), TEXT("Ratio"), 70.f);
	Producer.Channel.MarkFieldDirty(RatioIndex);
	Producer.Channel.Publish(Producer.Shadow);
	Producer.Channel.ConsumeUpdate([&Layout, &UIShadow](const FVaCuusModelUpdate& Update) { ApplyInto(Layout, Update, UIShadow); });

	TestEqual(TEXT("no field regresses: Score is still 9"), Read<FIntProperty>(Layout, UIShadow.GetData(), TEXT("Score")), 9);
	TestEqual(TEXT("and Ratio moved to 70"), Read<FFloatProperty>(Layout, UIShadow.GetData(), TEXT("Ratio")), 70.f);

	return true;
}

/**
 * SPEC 4 / INVARIANT I1, in its unit form: a channel is born fully dirty, so its first
 * publish carries every field whatever the differ concluded.
 *
 * The end-to-end form -- a struct whose defaults differ from zero shows its defaults on frame
 * 1 -- is VaCuus.Model.Sampler.FirstFrameDefaults, because it needs the differ to be the thing
 * that does NOT mark them.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelChannelFirstPublishTest, "VaCuus.Model.Channel.FirstPublish",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelChannelFirstPublishTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelChannelTest;

	const FVaCuusModelLayout Layout(FVaCuusLayoutTestModel::StaticStruct());
	const int32 NumFields = Layout.GetFields().Num();
	if (!TestTrue(TEXT("the fixture has fields"), NumFields > 0))
	{
		return false;
	}

	FProducer Producer(Layout);

	TestEqual(TEXT("a fresh channel owes every field"), Producer.Channel.NumOutstandingFields(), NumFields);
	TestEqual(TEXT("and has published nothing"), int32(Producer.Channel.GetLastPublishedGeneration()), 0);

	TestTrue(TEXT("the first publish goes out"), Producer.Channel.Publish(Producer.Shadow));

	int32 NumInFirstUpdate = 0;
	Producer.Channel.ConsumeUpdate([&NumInFirstUpdate](const FVaCuusModelUpdate& Update) { NumInFirstUpdate = Update.DirtyFields.CountSetBits(); });

	TestEqual(TEXT("the first update carries EVERY field"), NumInFirstUpdate, NumFields);
	TestEqual(TEXT("generation 1"), int32(Producer.Channel.GetLastPublishedGeneration()), 1);

	return true;
}

/**
 * SPEC 9's idle row, at the channel: a model nobody changes publishes nothing, and a consumer
 * that polls it does no work.
 *
 * A correctness gate wearing a performance costume -- if merely HAVING a model published every
 * frame, every frame would dirty a variable, and M2's whole idle short-circuit would be undone
 * silently.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelChannelIdleTest, "VaCuus.Model.Channel.Idle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelChannelIdleTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelChannelTest;

	const FVaCuusModelLayout Layout(FVaCuusLayoutTestModel::StaticStruct());
	FProducer Producer(Layout);
	FVaCuusModelShadow UIShadow(Layout.GetStruct());

	// The forced first publish, applied and echoed.
	Producer.Channel.Publish(Producer.Shadow);
	Producer.Channel.ConsumeUpdate([&Layout, &UIShadow](const FVaCuusModelUpdate& Update) { ApplyInto(Layout, Update, UIShadow); });

	TestEqual(TEXT("the echo cleared the outstanding set"), Producer.Channel.NumOutstandingFields(), 0);

	for (int32 Frame = 0; Frame < 100; ++Frame)
	{
		TestFalse(TEXT("an unchanged model publishes nothing"), Producer.Channel.Publish(Producer.Shadow));
		TestFalse(TEXT("and the consumer finds nothing to apply"),
			Producer.Channel.ConsumeUpdate([](const FVaCuusModelUpdate&) { checkNoEntry(); }));
	}

	TestEqual(TEXT("exactly one publish over 101 frames"), int32(Producer.Channel.GetNumPublishes()), 1);
	TestEqual(TEXT("and exactly one applied update"), int32(Producer.Channel.GetNumUpdatesApplied()), 1);

	return true;
}

/**
 * The echo half of the protocol: a bit clears ONLY when the consumer has confirmed the
 * generation that carried it, and a field re-dirtied after its publish survives that echo.
 *
 * The second half is the one that is easy to get wrong. "Clear the published bits on echo" is
 * the obvious rule and it silently drops the update that arrived between the publish and the
 * echo -- one lost value, once, which is exactly the shape of bug this milestone is built
 * against.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelChannelEchoTest, "VaCuus.Model.Channel.Echo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelChannelEchoTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelChannelTest;

	const FVaCuusModelLayout Layout(FVaCuusLayoutTestModel::StaticStruct());
	const int32 RatioIndex = IndexOf(Layout, TEXT("Ratio"));

	FProducer Producer(Layout);
	FVaCuusModelShadow UIShadow(Layout.GetStruct());

	Producer.Channel.Publish(Producer.Shadow);
	Producer.Channel.ConsumeUpdate([&Layout, &UIShadow](const FVaCuusModelUpdate& Update) { ApplyInto(Layout, Update, UIShadow); });

	// Unconsumed publishes keep carrying the field, because nothing has confirmed it.
	Write<FFloatProperty>(Layout, Producer.Shadow.GetData(), TEXT("Ratio"), 1.f);
	Producer.Channel.MarkFieldDirty(RatioIndex);
	Producer.Channel.Publish(Producer.Shadow);

	for (int32 Frame = 0; Frame < 5; ++Frame)
	{
		// No new mark, no consume: the value is still outstanding, so a publish still happens
		// and still carries it.
		TestTrue(TEXT("an unacknowledged field is republished"), Producer.Channel.Publish(Producer.Shadow));
		TestEqual(TEXT("and stays outstanding"), Producer.Channel.NumOutstandingFields(), 1);
	}

	// THE RE-DIRTY RACE: the value moves again AFTER the publish the consumer is about to
	// confirm. The echo must not swallow it.
	const uint64 GenerationToConfirm = Producer.Channel.GetLastPublishedGeneration();
	Write<FFloatProperty>(Layout, Producer.Shadow.GetData(), TEXT("Ratio"), 2.f);
	Producer.Channel.MarkFieldDirty(RatioIndex);

	Producer.Channel.ConsumeUpdate([&Layout, &UIShadow](const FVaCuusModelUpdate& Update) { ApplyInto(Layout, Update, UIShadow); });
	TestEqual(TEXT("the consumer confirmed the publish it read"), int64(Producer.Channel.GetAppliedGeneration()), int64(GenerationToConfirm));
	TestEqual(TEXT("the UI holds the value that publish carried"), Read<FFloatProperty>(Layout, UIShadow.GetData(), TEXT("Ratio")), 1.f);

	TestEqual(TEXT("the re-dirty survived the echo"), Producer.Channel.NumOutstandingFields(), 1);
	TestTrue(TEXT("so it publishes again"), Producer.Channel.Publish(Producer.Shadow));

	Producer.Channel.ConsumeUpdate([&Layout, &UIShadow](const FVaCuusModelUpdate& Update) { ApplyInto(Layout, Update, UIShadow); });
	TestEqual(TEXT("and the newer value arrives"), Read<FFloatProperty>(Layout, UIShadow.GetData(), TEXT("Ratio")), 2.f);
	TestEqual(TEXT("nothing is outstanding once it is confirmed"), Producer.Channel.NumOutstandingFields(), 0);

	return true;
}

/**
 * INVARIANT I2 WITH AN ARRAY FIELD (M3b): the recycled slot's stale value is now a whole
 * ARRAY -- every element and the Num() regress together, and nothing about applying a stale
 * array looks wrong from inside the copy: a shrink is legal, an element assignment is legal,
 * the bit was true.
 *
 * Same shape as NoRegression above -- publish twice without consuming, consume, then change
 * an unrelated field -- and the same lever breaks it: a Publish() that ORs its bits into the
 * recycled slot and copies only what changed this frame announces bit(Numbers) with the
 * FIRST publish's elements, so the UI walks Num() back from 2 to 3 while resurrecting values
 * it already superseded. What is under test is the protocol that prevents it: PublishSet =
 * Pending | Unacked assigned (never OR'd) into the slot, and EVERY bit's value rewritten at
 * every publish (VaCuusModelChannel.cpp, Publish()) -- for an Array field that rewrite is a
 * whole-element SyncCopy, reached through the same CopyValue funnel as every other stage.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelChannelArrayNoRegressionTest, "VaCuus.Model.Channel.ArrayNoRegression",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelChannelArrayNoRegressionTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelChannelTest;

	const FVaCuusModelLayout Layout(FVaCuusArrayTestModel::StaticStruct());
	if (!TestTrue(TEXT("the layout resolved its struct"), Layout.IsValid()))
	{
		return false;
	}

	const int32 ScalarIndex = IndexOf(Layout, TEXT("Scalar"));
	const int32 NumbersIndex = IndexOf(Layout, TEXT("Numbers"));
	const int32 LabelsIndex = IndexOf(Layout, TEXT("Labels"));
	if (!TestTrue(TEXT("the fixture has Scalar, Numbers and Labels"),
			ScalarIndex != INDEX_NONE && NumbersIndex != INDEX_NONE && LabelsIndex != INDEX_NONE))
	{
		return false;
	}

	FProducer Producer(Layout);
	FVaCuusModelShadow UIShadow(Layout.GetStruct());

	TArray<int32>& ShadowNumbers = ValueRef<TArray<int32>>(Layout, Producer.Shadow.GetData(), TEXT("Numbers"));
	TArray<FString>& ShadowLabels = ValueRef<TArray<FString>>(Layout, Producer.Shadow.GetData(), TEXT("Labels"));

	// Drain the forced full-bit first publish (I1) so what follows is about I2 alone.
	ShadowNumbers = {1};
	TestTrue(TEXT("the first publish goes out"), Producer.Channel.Publish(Producer.Shadow));
	Producer.Channel.ConsumeUpdate([&Layout, &UIShadow](const FVaCuusModelUpdate& Update) { ApplyInto(Layout, Update, UIShadow); });

	// (1) Both arrays move, published, NOT consumed. This slot -- three elements, "first" --
	// is the one the third publish gets back, still carrying these bits and these elements.
	ShadowNumbers = {10, 20, 30};
	ShadowLabels = {TEXT("first")};
	Producer.Channel.MarkFieldDirty(NumbersIndex);
	Producer.Channel.MarkFieldDirty(LabelsIndex);
	TestTrue(TEXT("publish the first contents"), Producer.Channel.Publish(Producer.Shadow));

	// (2) Both move again -- different elements AND a different Num -- published into the
	// OTHER slot, then consumed.
	ShadowNumbers = {40, 50};
	ShadowLabels = {TEXT("second"), TEXT("SECOND")};
	Producer.Channel.MarkFieldDirty(NumbersIndex);
	Producer.Channel.MarkFieldDirty(LabelsIndex);
	TestTrue(TEXT("publish the second contents"), Producer.Channel.Publish(Producer.Shadow));

	TestTrue(TEXT("the consumer takes an update"),
		Producer.Channel.ConsumeUpdate([&Layout, &UIShadow](const FVaCuusModelUpdate& Update) { ApplyInto(Layout, Update, UIShadow); }));
	TestTrue(TEXT("latest-wins: the UI sees the second elements"),
		ValueRef<TArray<int32>>(Layout, UIShadow.GetData(), TEXT("Numbers")) == TArray<int32>({40, 50}));

	// (3) An UNRELATED field changes; the arrays are untouched, so nothing in a
	// write-what-changed producer would overwrite the stale elements in the recycled slot.
	Write<FIntProperty>(Layout, Producer.Shadow.GetData(), TEXT("Scalar"), 7);
	Producer.Channel.MarkFieldDirty(ScalarIndex);
	TestTrue(TEXT("publish the Scalar change"), Producer.Channel.Publish(Producer.Shadow));

	TestTrue(TEXT("the consumer takes the Scalar update"),
		Producer.Channel.ConsumeUpdate([&Layout, &UIShadow](const FVaCuusModelUpdate& Update) { ApplyInto(Layout, Update, UIShadow); }));
	TestEqual(TEXT("Scalar arrived"), Read<FIntProperty>(Layout, UIShadow.GetData(), TEXT("Scalar")), 7);

	// THE ASSERTIONS. No element may regress to the first publish's values, and the Num may
	// not walk back up to the first publish's.
	TestTrue(TEXT("no element regresses: Numbers is still {40, 50}"),
		ValueRef<TArray<int32>>(Layout, UIShadow.GetData(), TEXT("Numbers")) == TArray<int32>({40, 50}));

	// Per element and case-sensitively: TArray<FString>::operator== compares elements through
	// FString's operator==, which is Stricmp (UnrealString.h.inl:906-915), and a regression to
	// an equal-ignoring-case value must not pass.
	const TArray<FString>& UILabels = ValueRef<TArray<FString>>(Layout, UIShadow.GetData(), TEXT("Labels"));
	TestTrue(TEXT("no element regresses: Labels is still {second, SECOND}, case and all"),
		UILabels.Num() == 2 && UILabels[0].Equals(TEXT("second"), ESearchCase::CaseSensitive)
			&& UILabels[1].Equals(TEXT("SECOND"), ESearchCase::CaseSensitive));

	return true;
}

/**
 * SLOT SELF-SUFFICIENCY WITH A MIXED SCALAR+ARRAY OUTSTANDING SET (M3b): dirty an array and
 * a scalar in DIFFERENT frames, publish every frame, consume nothing until the end, then
 * consume ONCE. The one slot read must announce BOTH fields and carry both CURRENT values --
 * the array's from its latest change, not from the frame its bit was first set.
 *
 * This is the "every published slot is self-sufficient" invariant with an array among the
 * outstanding fields: every publish while the bit is outstanding re-runs the array's whole-
 * element SyncCopy into the slot, which is exactly the O(elements)-per-republish cost the
 * channel header's stall paragraph now scopes.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelChannelArrayMixedSelfSufficiencyTest,
	"VaCuus.Model.Channel.ArrayMixedSelfSufficiency", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelChannelArrayMixedSelfSufficiencyTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusModelChannelTest;

	const FVaCuusModelLayout Layout(FVaCuusArrayTestModel::StaticStruct());
	const int32 ScalarIndex = IndexOf(Layout, TEXT("Scalar"));
	const int32 NumbersIndex = IndexOf(Layout, TEXT("Numbers"));
	if (!TestTrue(TEXT("the fixture has Scalar and Numbers"), ScalarIndex != INDEX_NONE && NumbersIndex != INDEX_NONE))
	{
		return false;
	}

	FProducer Producer(Layout);
	FVaCuusModelShadow UIShadow(Layout.GetStruct());

	TArray<int32>& ShadowNumbers = ValueRef<TArray<int32>>(Layout, Producer.Shadow.GetData(), TEXT("Numbers"));

	// Drain the forced full-bit first publish (I1).
	Producer.Channel.Publish(Producer.Shadow);
	Producer.Channel.ConsumeUpdate([&Layout, &UIShadow](const FVaCuusModelUpdate& Update) { ApplyInto(Layout, Update, UIShadow); });

	// Frame 1: only the ARRAY moves. Published, not consumed.
	ShadowNumbers = {1, 2};
	Producer.Channel.MarkFieldDirty(NumbersIndex);
	TestTrue(TEXT("frame 1 publishes"), Producer.Channel.Publish(Producer.Shadow));

	// Frame 2: only the SCALAR moves. The array is NOT re-marked -- it is unacked, and the
	// publish must still rewrite its whole current contents into this frame's slot.
	Write<FIntProperty>(Layout, Producer.Shadow.GetData(), TEXT("Scalar"), 5);
	Producer.Channel.MarkFieldDirty(ScalarIndex);
	TestTrue(TEXT("frame 2 publishes"), Producer.Channel.Publish(Producer.Shadow));

	// Frame 3: the array moves AGAIN. Frame 1's published elements are now superseded while
	// the bit is still outstanding -- the exact state a stale-slot design ships stale from.
	ShadowNumbers = {7, 8, 9};
	Producer.Channel.MarkFieldDirty(NumbersIndex);
	TestTrue(TEXT("frame 3 publishes"), Producer.Channel.Publish(Producer.Shadow));

	TestEqual(TEXT("both fields are outstanding"), Producer.Channel.NumOutstandingFields(), 2);

	// ONE consume, of the newest slot only -- the two earlier publishes are skipped, which is
	// latest-wins working as designed and why the slot has to be self-sufficient.
	bool bScalarBit = false;
	bool bNumbersBit = false;
	TestTrue(TEXT("the consumer takes one update"),
		Producer.Channel.ConsumeUpdate(
			[&Layout, &UIShadow, &bScalarBit, &bNumbersBit, ScalarIndex, NumbersIndex](const FVaCuusModelUpdate& Update)
			{
				bScalarBit = Update.DirtyFields[ScalarIndex];
				bNumbersBit = Update.DirtyFields[NumbersIndex];
				ApplyInto(Layout, Update, UIShadow);
			}));

	TestTrue(TEXT("the one slot announces the scalar dirtied two publishes ago"), bScalarBit);
	TestTrue(TEXT("and the array"), bNumbersBit);
	TestEqual(TEXT("the scalar arrived with its current value"), Read<FIntProperty>(Layout, UIShadow.GetData(), TEXT("Scalar")), 5);
	TestTrue(TEXT("the array arrived with frame 3's elements, not frame 1's"),
		ValueRef<TArray<int32>>(Layout, UIShadow.GetData(), TEXT("Numbers")) == TArray<int32>({7, 8, 9}));

	// The echo settles everything: nothing outstanding, and the next publish declines.
	TestEqual(TEXT("nothing is outstanding after the echo"), Producer.Channel.NumOutstandingFields(), 0);
	TestFalse(TEXT("so the next publish declines"), Producer.Channel.Publish(Producer.Shadow));

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
