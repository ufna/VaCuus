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

#endif	  // WITH_DEV_AUTOMATION_TESTS
