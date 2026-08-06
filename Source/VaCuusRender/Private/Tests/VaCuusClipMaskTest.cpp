// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusCommandBuffer.h"
#include "VaCuusDefines.h"
#include "VaCuusReplayRenderer.h"

#include "HAL/IConsoleManager.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * VaCuus-4ik: the clip-mask stencil pass — `overflow` clipping that survives a transform.
 *
 * THE DEFECT THESE TESTS EXIST FOR, both halves verified in the vendored source rather than
 * repeated: ElementUtilities.cpp:174-175 sets `disable_scissor_clipping = true` UNCONDITIONALLY
 * whenever a transform is active on the clipping chain, and :162-169 emits a clip MASK in its
 * place whenever `has_border_radius || (transform && has_clipping_content)`. Before this bead the
 * replayer skipped both clip-mask commands, so the original clipping was off and its replacement
 * never landed — a transformed ancestor, or merely a `border-radius` on a scroll container,
 * silently unclipped the whole subtree with no Error and no Warning.
 *
 * WHAT MAKES IT TESTABLE, in two independent layers, because the feature has two independent
 * ways to be wrong:
 *
 *  - THE VALUE PROTOCOL (ValueProtocol below) is pure arithmetic over an 8-bit stencil and needs
 *    no GPU at all, so it runs in the -nullrhi suite where the drawing tests cannot. It is also
 *    the layer whose bug would be INVISIBLE at small scale: reusing stencil value 1 for every
 *    mask works perfectly until a second mask appears in the same frame, and then leaks the
 *    first one's shape into the second. The test drives a SIMULATED stencil buffer so the leak
 *    is an assertion rather than a screenshot.
 *
 *  - THE PIXELS (every other test here) go through the production FVaCuusReplayRenderer::Replay,
 *    the same way VaCuusViewMSAATest.cpp and VaCuusGradientAATest.cpp do — so what is measured is
 *    the shipped pass, its render-pass info, its stencil attachment and its PSOs, not a replica.
 *    Each one carries its own NEGATIVE CONTROL in the same test: the identical buffer with the
 *    two mask commands removed, which is exactly the pre-bead behaviour and must fail the clip
 *    assertion. A clipping test that has never been seen to not-clip is not evidence.
 *
 * VENUE. The drawing tests read back and report themselves skipped under -nullrhi (the contract
 * VaCuusGradientAATest.cpp:201-210 established); the real-RHI run is where they carry evidence.
 */
namespace VaCuusClipMaskTest
{
/** Reports the skip once, in the words the gradient and MSAA GPU tests use. */
static bool SkippedUnderNullRHI(const TCHAR* TestName)
{
	if (!GUsingNullRHI)
	{
		return false;
	}
	UE_LOG(LogVaCuus, Display, TEXT("%s: SKIPPED under NullRHI (no draw, no readback)"), TestName);
	return true;
}

static constexpr int32 ViewExtent = 128;

/** Opaque white = premultiplied white, so a covered pixel reads 255 alpha and a clipped one 0. */
static FVaCuusGeometryData MakeQuad(float MinX, float MinY, float MaxX, float MaxY)
{
	FVaCuusGeometryData Data;
	const FColor White(255, 255, 255, 255);

	Data.Vertices.Add({FVector2f(MinX, MinY), White, FVector2f::ZeroVector});
	Data.Vertices.Add({FVector2f(MaxX, MinY), White, FVector2f::ZeroVector});
	Data.Vertices.Add({FVector2f(MaxX, MaxY), White, FVector2f::ZeroVector});
	Data.Vertices.Add({FVector2f(MinX, MaxY), White, FVector2f::ZeroVector});

	Data.Indices.Append({0, 1, 2, 0, 2, 3});
	return Data;
}

/** Row-vector scale about the origin, the convention MakePixelToClipMatrix composes with. */
static FMatrix44f MakeScale(float S)
{
	FMatrix44f M = FMatrix44f::Identity;
	M.M[0][0] = S;
	M.M[1][1] = S;
	return M;
}

/** Geometry handles the builders below share, so a test can name a shape by number. */
enum : FVaCuusGeometryHandle
{
	GeomFullView = 1,
	GeomLeftHalf = 2,
	GeomTopHalf = 3,
	GeomLeftQuarterLocal = 4 // 0..32 in x; a scale of 2 puts it over the left HALF
};

/**
 * A buffer with every shape uploaded, so the command list is the only thing a test varies.
 *
 * The whole delta between a clipped and an unclipped run is which commands get appended, which
 * is what makes the negative control in each test a two-line change rather than a second fixture.
 */
static FVaCuusCommandBuffer MakeBufferWithShapes()
{
	FVaCuusCommandBuffer Buffer;
	Buffer.Generation = 1;
	Buffer.ViewSize = FIntPoint(ViewExtent, ViewExtent);

	Buffer.NewGeometry.Add(GeomFullView, MakeQuad(0.0f, 0.0f, float(ViewExtent), float(ViewExtent)));
	Buffer.NewGeometry.Add(GeomLeftHalf, MakeQuad(0.0f, 0.0f, float(ViewExtent) * 0.5f, float(ViewExtent)));
	Buffer.NewGeometry.Add(GeomTopHalf, MakeQuad(0.0f, 0.0f, float(ViewExtent), float(ViewExtent) * 0.5f));
	Buffer.NewGeometry.Add(GeomLeftQuarterLocal, MakeQuad(0.0f, 0.0f, float(ViewExtent) * 0.25f, float(ViewExtent)));

	return Buffer;
}

static void AddEnableClipMask(FVaCuusCommandBuffer& Buffer, bool bEnable)
{
	FVaCuusCommand& Command = Buffer.Commands.AddDefaulted_GetRef();
	Command.Type = EVaCuusCommandType::EnableClipMask;
	Command.bClipMaskEnable = bEnable ? 1 : 0;
}

static void AddRenderToClipMask(FVaCuusCommandBuffer& Buffer, EVaCuusClipMaskOp Op, FVaCuusGeometryHandle Geometry)
{
	FVaCuusCommand& Command = Buffer.Commands.AddDefaulted_GetRef();
	Command.Type = EVaCuusCommandType::RenderToClipMask;
	Command.ClipMaskOp = Op;
	Command.Geometry = Geometry;
}

static void AddDraw(FVaCuusCommandBuffer& Buffer, FVaCuusGeometryHandle Geometry)
{
	FVaCuusCommand& Command = Buffer.Commands.AddDefaulted_GetRef();
	Command.Type = EVaCuusCommandType::DrawGeometry;
	Command.Geometry = Geometry;
}

static void AddSetTransform(FVaCuusCommandBuffer& Buffer, const FMatrix44f& Transform)
{
	FVaCuusCommand& Command = Buffer.Commands.AddDefaulted_GetRef();
	Command.Type = EVaCuusCommandType::SetTransform;
	Command.Transform = Transform;
}

/** What one replayed buffer produced, plus the two numbers the allocation claims rest on. */
struct FReplayResult
{
	bool bRead = false;
	TArray<FColor> Pixels;
	int32 SampleCount = 1;
	uint64 StencilBytes = 0;

	/** Alpha at a pixel; 255 = drawn, 0 = clipped or never covered. */
	int32 AlphaAt(int32 X, int32 Y) const
	{
		return Pixels.IsValidIndex(Y * ViewExtent + X) ? int32(Pixels[Y * ViewExtent + X].A) : -1;
	}
};

/**
 * Replays one buffer at a requested sample count and reads the per-view RT back.
 *
 * A REPLAYER PER CALL, so every run starts at generation 1 with no RT and no stencil to inherit —
 * the same isolation a fresh recorder/replayer pair gets in production, and the only way
 * StencilBytes can honestly answer "did THIS buffer cause the allocation".
 *
 * The cvar is set on the game thread and read on the render thread without a flush for the reason
 * VaCuusViewMSAATest.cpp:112-115 gives: `vacuus.ViewSampleCount` is not ECVF_RenderThreadSafe, so
 * OnCVarChange writes the render-thread shadow directly (ConsoleManager.cpp:487-496).
 */
static FReplayResult ReplayAndRead(const FVaCuusCommandBuffer& Buffer, int32 RequestedSampleCount)
{
	FReplayResult Result;

	IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("vacuus.ViewSampleCount"));
	if (CVar == nullptr)
	{
		return Result;
	}
	const int32 Saved = CVar->GetInt();
	CVar->Set(RequestedSampleCount, ECVF_SetByCode);

	ENQUEUE_RENDER_COMMAND(VaCuusClipMaskDraw)
	([&Result, &Buffer](FRHICommandListImmediate& RHICmdList)
		{
			FVaCuusReplayRenderer Replayer;
			Replayer.Replay(RHICmdList, Buffer);

			Result.SampleCount = Replayer.GetReplaySampleCount();
			Result.StencilBytes = Replayer.GetClipMaskStencilBytes();

			FRHITexture* Target = Replayer.GetOutputRT();
			if (Target != nullptr)
			{
				RHICmdList.Transition(FRHITransitionInfo(Target, ERHIAccess::SRVMask, ERHIAccess::CopySrc));

				FRHIGPUTextureReadback Readback(TEXT("VaCuusClipMaskReadback"));
				Readback.EnqueueCopy(RHICmdList, Target);
				RHICmdList.SubmitAndBlockUntilGPUIdle();

				int32 RowPitchInPixels = 0;
				if (Readback.IsReady())
				{
					if (const void* Data = Readback.Lock(RowPitchInPixels))
					{
						// The staging row pitch is the RHI's, not the extent — copy row by row.
						const FColor* Rows = static_cast<const FColor*>(Data);
						Result.Pixels.SetNumUninitialized(ViewExtent * ViewExtent);
						for (int32 Y = 0; Y < ViewExtent; ++Y)
						{
							FMemory::Memcpy(&Result.Pixels[Y * ViewExtent], Rows + int64(Y) * RowPitchInPixels,
								ViewExtent * sizeof(FColor));
						}
						Readback.Unlock();
						Result.bRead = true;
					}
				}
			}

			Replayer.ReleaseResources();
		});
	FlushRenderingCommands();

	CVar->Set(Saved, ECVF_SetByCode);
	return Result;
}

/** The two sample counts the bead is required to behave identically at. */
static const int32 SampleCounts[] = {1, 4};
} // namespace VaCuusClipMaskTest

/**
 * THE VALUE PROTOCOL, asserted against a simulated stencil buffer rather than argued.
 *
 * WHY A SIMULATION AND NOT A TABLE OF EXPECTED VALUES: the property that matters is not which
 * numbers FVaCuusClipMaskState picks, it is that a mask built earlier in the frame CANNOT leak
 * into one built later. That is a statement about the buffer's contents, so the test keeps a
 * buffer — one byte per cell — applies each step's real GPU semantics to it (Replace at the write
 * value, saturated +1, a full clear), and then asks the only question that matters: which cells
 * pass `stencil == TestValue`. Change the numbering scheme and this test still holds; break the
 * isolation and it fails immediately, whatever the numbering.
 *
 * Runs everywhere, including -nullrhi: no RHI type appears in it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusClipMaskValueProtocolTest, "VaCuus.Render.ClipMask.ValueProtocol",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusClipMaskValueProtocolTest::RunTest(const FString& Parameters_)
{
	using VaCuusReplay::FVaCuusClipMaskState;
	using VaCuusReplay::FVaCuusClipMaskStep;

	// A 1-D "screen" of 16 cells is enough: every shape below is an interval.
	constexpr int32 NumCells = 16;

	struct FSimulatedStencil
	{
		uint8 Cells[NumCells] = {};

		/** BeginRenderPass's Clear action, and DrawClearQuad when a step asks for one. */
		void Clear(uint32 Value)
		{
			for (uint8& Cell : Cells)
			{
				Cell = uint8(Value);
			}
		}

		/** One mask draw: the step's op, applied where the shape covers. */
		void Draw(const FVaCuusClipMaskStep& Step, int32 First, int32 Last)
		{
			for (int32 Index = First; Index <= Last; ++Index)
			{
				Cells[Index] = Step.bReplace ? uint8(Step.WriteValue) : uint8(FMath::Min<int32>(Cells[Index] + 1, 255));
			}
		}

		/** The masked draw's stencil test: EQUAL, read mask 0xFF. */
		bool Passes(int32 Index, uint32 TestValue) const { return Cells[Index] == uint8(TestValue); }
	};

	// The whole protocol in one helper, so no test body can accidentally apply a step's clear and
	// forget its draw (which is precisely the bug that would make the isolation look fine).
	const auto Apply = [](FVaCuusClipMaskState& State, FSimulatedStencil& Stencil, EVaCuusClipMaskOp Op, int32 First,
						   int32 Last)
	{
		const FVaCuusClipMaskStep Step = State.Step(Op);
		if (Step.bNeedsClear)
		{
			Stencil.Clear(Step.ClearValue);
		}
		Stencil.Draw(Step, First, Last);
		return Step;
	};

	// ---------------------------------------------------------------------------------------
	// 1. Set alone: exactly the covered interval passes.
	// ---------------------------------------------------------------------------------------
	{
		FVaCuusClipMaskState State;
		FSimulatedStencil Stencil;
		State.BeginPass();
		Stencil.Clear(0); // the render pass's own clear

		const FVaCuusClipMaskStep Step = Apply(State, Stencil, EVaCuusClipMaskOp::Set, 4, 11);
		for (int32 Index = 0; Index < NumCells; ++Index)
		{
			const bool bExpected = (Index >= 4 && Index <= 11);
			TestEqual(*FString::Printf(TEXT("Set: cell %d"), Index), Stencil.Passes(Index, Step.TestValue), bExpected);
		}
	}

	// ---------------------------------------------------------------------------------------
	// 2. Intersect nested inside a Set: only the OVERLAP passes. This is the shape RmlUi builds
	//    for nested clip containers (ElementUtilities.cpp:165 picks Set for the first entry and
	//    Intersect for every one after).
	// ---------------------------------------------------------------------------------------
	{
		FVaCuusClipMaskState State;
		FSimulatedStencil Stencil;
		State.BeginPass();
		Stencil.Clear(0);

		Apply(State, Stencil, EVaCuusClipMaskOp::Set, 2, 9);
		const FVaCuusClipMaskStep Step = Apply(State, Stencil, EVaCuusClipMaskOp::Intersect, 6, 13);
		for (int32 Index = 0; Index < NumCells; ++Index)
		{
			const bool bExpected = (Index >= 6 && Index <= 9);
			TestEqual(*FString::Printf(TEXT("Set+Intersect: cell %d"), Index), Stencil.Passes(Index, Step.TestValue),
				bExpected);
		}
	}

	// ---------------------------------------------------------------------------------------
	// 3. THREE-DEEP NESTING, which is where a naive "increment and test one more" scheme first
	//    goes wrong: a cell covered by the two Intersects but NOT by the Set reaches 2, and must
	//    not be mistaken for a cell that reached 2 by being in the mask all along.
	// ---------------------------------------------------------------------------------------
	{
		FVaCuusClipMaskState State;
		FSimulatedStencil Stencil;
		State.BeginPass();
		Stencil.Clear(0);

		Apply(State, Stencil, EVaCuusClipMaskOp::Set, 0, 7);
		Apply(State, Stencil, EVaCuusClipMaskOp::Intersect, 4, 15);
		const FVaCuusClipMaskStep Step = Apply(State, Stencil, EVaCuusClipMaskOp::Intersect, 5, 15);
		for (int32 Index = 0; Index < NumCells; ++Index)
		{
			const bool bExpected = (Index >= 5 && Index <= 7);
			TestEqual(*FString::Printf(TEXT("Three-deep: cell %d"), Index), Stencil.Passes(Index, Step.TestValue),
				bExpected);
		}
	}

	// ---------------------------------------------------------------------------------------
	// 4. SetInverse: everything EXCEPT the covered interval passes (RenderInterface.h:12). This
	//    is box-shadow's operation (GeometryBoxShadow.cpp:206, :215, :221) and the one that must
	//    always clear.
	// ---------------------------------------------------------------------------------------
	{
		FVaCuusClipMaskState State;
		FSimulatedStencil Stencil;
		State.BeginPass();
		Stencil.Clear(0);

		const FVaCuusClipMaskStep Step = Apply(State, Stencil, EVaCuusClipMaskOp::SetInverse, 3, 8);
		TestTrue(TEXT("SetInverse always clears"), Step.bNeedsClear);
		for (int32 Index = 0; Index < NumCells; ++Index)
		{
			const bool bExpected = !(Index >= 3 && Index <= 8);
			TestEqual(*FString::Printf(TEXT("SetInverse: cell %d"), Index), Stencil.Passes(Index, Step.TestValue),
				bExpected);
		}
	}

	// ---------------------------------------------------------------------------------------
	// 5. THE ISOLATION PROPERTY, and the reason this whole test exists. Two masks in one pass,
	//    disjoint: after the SECOND Set, the first mask's region must not pass. An implementation
	//    that writes 1 for every Set and never clears passes tests 1-4 and fails here.
	// ---------------------------------------------------------------------------------------
	{
		FVaCuusClipMaskState State;
		FSimulatedStencil Stencil;
		State.BeginPass();
		Stencil.Clear(0);

		Apply(State, Stencil, EVaCuusClipMaskOp::Set, 0, 3);
		const FVaCuusClipMaskStep Second = Apply(State, Stencil, EVaCuusClipMaskOp::Set, 10, 13);

		for (int32 Index = 0; Index <= 3; ++Index)
		{
			TestFalse(*FString::Printf(TEXT("First mask does not leak into the second: cell %d"), Index),
				Stencil.Passes(Index, Second.TestValue));
		}
		for (int32 Index = 10; Index <= 13; ++Index)
		{
			TestTrue(*FString::Printf(TEXT("Second mask passes: cell %d"), Index), Stencil.Passes(Index, Second.TestValue));
		}
	}

	// ---------------------------------------------------------------------------------------
	// 6. THE INVARIANT, driven far past anything a document produces: hundreds of masks in one
	//    pass, mixing all three operations, with the buffer checked after EVERY step. "Every
	//    value present is below NextFreeValue" is what makes value reuse safe, and it is the one
	//    claim the isolation in test 5 rests on.
	// ---------------------------------------------------------------------------------------
	{
		FVaCuusClipMaskState State;
		FSimulatedStencil Stencil;
		State.BeginPass();
		Stencil.Clear(0);

		int32 NumClears = 0;
		bool bInvariantHeld = true;
		bool bIsolationHeld = true;

		FRandomStream Random(0x4181C0DE);
		for (int32 Iteration = 0; Iteration < 600; ++Iteration)
		{
			// Roughly RmlUi's own mix: a Set opens every list, Intersects nest under it.
			const int32 Roll = Random.RandRange(0, 9);
			const EVaCuusClipMaskOp Op = (Roll < 6) ? EVaCuusClipMaskOp::Set
				: (Roll < 9)                        ? EVaCuusClipMaskOp::Intersect
													: EVaCuusClipMaskOp::SetInverse;

			const int32 First = Random.RandRange(0, NumCells - 1);
			const int32 Last = Random.RandRange(First, NumCells - 1);
			const FVaCuusClipMaskStep Step = Apply(State, Stencil, Op, First, Last);
			NumClears += Step.bNeedsClear ? 1 : 0;

			for (int32 Index = 0; Index < NumCells; ++Index)
			{
				bInvariantHeld &= (uint32(Stencil.Cells[Index]) < State.GetNextFreeValue());
			}

			// A Set is a fresh mask, so exactly its own interval may pass right after it.
			if (Op == EVaCuusClipMaskOp::Set)
			{
				for (int32 Index = 0; Index < NumCells; ++Index)
				{
					const bool bExpected = (Index >= First && Index <= Last);
					bIsolationHeld &= (Stencil.Passes(Index, Step.TestValue) == bExpected);
				}
			}
		}

		TestTrue(TEXT("Every stencil value stays below NextFreeValue across 600 mixed operations"), bInvariantHeld);
		TestTrue(TEXT("Every Set yields exactly its own region, whatever came before it"), bIsolationHeld);

		// The clears are the cost this scheme exists to avoid. Logged as a NUMBER because that is
		// the claim: the reference backend would have paid one full-target clear per Set
		// (RmlUi_Renderer_GL3.cpp:1135-1137), i.e. ~360 of them for this run.
		AddInfo(FString::Printf(TEXT("600 mixed operations needed %d full-target clears; the reference backend's ")
								TEXT("clear-on-every-Set would have needed one per Set"),
			NumClears));
		TestTrue(TEXT("Clears are rare: far fewer than one per operation"), NumClears < 100);
	}

	// ---------------------------------------------------------------------------------------
	// 7. THE WRAP. 8 bits runs out, and when it does the clear must come back — otherwise the
	//    invariant above is unenforceable and masks start leaking after ~250 of them in a frame.
	// ---------------------------------------------------------------------------------------
	{
		FVaCuusClipMaskState State;
		State.BeginPass();

		bool bSawClear = false;
		for (int32 Iteration = 0; Iteration < FVaCuusClipMaskState::HighWaterValue + 4; ++Iteration)
		{
			const FVaCuusClipMaskStep Step = State.Step(EVaCuusClipMaskOp::Set);
			bSawClear |= Step.bNeedsClear;
			TestTrue(TEXT("Every value the protocol hands out fits in 8 bits"), Step.TestValue <= 255);
		}
		TestTrue(TEXT("The counter wraps with a clear rather than reusing a live value"), bSawClear);
	}

	return true;
}

/**
 * THE HEADLINE CASE: a transform on the clipping chain. RmlUi hands the mask geometry over with
 * the clipping element's OWN transform applied (RenderManager::ApplyClipMask calls SetTransform
 * per element and restores the caller's afterwards, RenderManager.cpp:164-175), so the recorded
 * stream interleaves SetTransform with RenderToClipMask — and honouring it is the difference
 * between clipping at the element's scaled box and clipping at its unscaled one.
 *
 * THE DISCRIMINATOR IS x = 48. The mask shape is 0..32 wide in LOCAL space under a scale of 2, so
 * it covers 0..64 on screen. x = 48 is inside the scaled mask and outside the unscaled one, so:
 *   - transform ignored  -> x = 48 is clipped (alpha 0)
 *   - mask dropped       -> x = 96 is drawn   (alpha 255), the pre-bead behaviour
 *   - correct            -> 16 and 48 drawn, 96 clipped
 * All three are asserted, which is what makes the middle row evidence rather than decoration.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusClipMaskTransformTest, "VaCuus.Render.ClipMask.SurvivesTransform",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusClipMaskTransformTest::RunTest(const FString& Parameters_)
{
	using namespace VaCuusClipMaskTest;
	if (SkippedUnderNullRHI(TEXT("VaCuus.Render.ClipMask.SurvivesTransform")))
	{
		return true;
	}

	for (const int32 Requested : SampleCounts)
	{
		// THE NEGATIVE CONTROL, first: the same document with the two mask commands removed is
		// exactly what this replayer did before the bead, and it must fill the whole view.
		{
			FVaCuusCommandBuffer Buffer = MakeBufferWithShapes();
			AddSetTransform(Buffer, MakeScale(2.0f));
			AddSetTransform(Buffer, FMatrix44f::Identity);
			AddDraw(Buffer, GeomFullView);

			const FReplayResult Result = ReplayAndRead(Buffer, Requested);
			if (!TestTrue(TEXT("Readback completed (no mask)"), Result.bRead))
			{
				return false;
			}
			TestEqual(*FString::Printf(TEXT("%dx: with the mask dropped, x=96 is DRAWN (the defect)"), Result.SampleCount),
				Result.AlphaAt(96, 64), 255);
		}

		FVaCuusCommandBuffer Buffer = MakeBufferWithShapes();
		AddSetTransform(Buffer, MakeScale(2.0f));
		AddEnableClipMask(Buffer, true);
		AddRenderToClipMask(Buffer, EVaCuusClipMaskOp::Set, GeomLeftQuarterLocal);
		AddSetTransform(Buffer, FMatrix44f::Identity); // what ApplyClipMask restores
		AddDraw(Buffer, GeomFullView);

		const FReplayResult Result = ReplayAndRead(Buffer, Requested);
		if (!TestTrue(TEXT("Readback completed"), Result.bRead))
		{
			return false;
		}
		AddInfo(FString::Printf(TEXT("sample count %d granted (%d requested), stencil %llu bytes"), Result.SampleCount,
			Requested, Result.StencilBytes));

		TestEqual(*FString::Printf(TEXT("%dx: x=16 is inside the mask"), Result.SampleCount), Result.AlphaAt(16, 64), 255);
		TestEqual(*FString::Printf(TEXT("%dx: x=48 is inside the SCALED mask (the transform was honoured)"),
					  Result.SampleCount),
			Result.AlphaAt(48, 64), 255);
		TestEqual(*FString::Printf(TEXT("%dx: x=96 is clipped"), Result.SampleCount), Result.AlphaAt(96, 64), 0);
	}

	return true;
}

/**
 * NESTING: an Intersect inside a Set clips to the OVERLAP. Left half, then top half, so only the
 * top-left quadrant survives — and the two quadrants that are inside exactly one of the shapes are
 * the assertions that distinguish a real intersection from "the last mask wins".
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusClipMaskNestedTest, "VaCuus.Render.ClipMask.Nested",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusClipMaskNestedTest::RunTest(const FString& Parameters_)
{
	using namespace VaCuusClipMaskTest;
	if (SkippedUnderNullRHI(TEXT("VaCuus.Render.ClipMask.Nested")))
	{
		return true;
	}

	for (const int32 Requested : SampleCounts)
	{
		FVaCuusCommandBuffer Buffer = MakeBufferWithShapes();
		AddEnableClipMask(Buffer, true);
		AddRenderToClipMask(Buffer, EVaCuusClipMaskOp::Set, GeomLeftHalf);
		AddRenderToClipMask(Buffer, EVaCuusClipMaskOp::Intersect, GeomTopHalf);
		AddDraw(Buffer, GeomFullView);

		const FReplayResult Result = ReplayAndRead(Buffer, Requested);
		if (!TestTrue(TEXT("Readback completed"), Result.bRead))
		{
			return false;
		}

		const int32 SampleCount = Result.SampleCount;
		TestEqual(*FString::Printf(TEXT("%dx: top-left is inside both"), SampleCount), Result.AlphaAt(32, 32), 255);
		TestEqual(*FString::Printf(TEXT("%dx: top-right is inside the Intersect only"), SampleCount),
			Result.AlphaAt(96, 32), 0);
		TestEqual(*FString::Printf(TEXT("%dx: bottom-left is inside the Set only"), SampleCount), Result.AlphaAt(32, 96), 0);
		TestEqual(*FString::Printf(TEXT("%dx: bottom-right is inside neither"), SampleCount), Result.AlphaAt(96, 96), 0);
	}

	return true;
}

/**
 * TWO MASKS IN ONE FRAME — the pixel-level counterpart of ValueProtocol's test 5, and the case a
 * "always write 1, never clear" implementation gets wrong on real hardware while passing every
 * single-mask test above. Mask the left half, draw; then mask the top half, draw. The bottom-left
 * quadrant is inside the FIRST mask only, so it must carry only the first draw's pixels — and the
 * second draw must not reach it.
 *
 * Both draws are the same opaque white, so the observable is not "which draw won" but WHERE the
 * second draw landed: bottom-right is inside neither mask and must stay clear.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusClipMaskTwoMasksTest, "VaCuus.Render.ClipMask.TwoMasksOneFrame",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusClipMaskTwoMasksTest::RunTest(const FString& Parameters_)
{
	using namespace VaCuusClipMaskTest;
	if (SkippedUnderNullRHI(TEXT("VaCuus.Render.ClipMask.TwoMasksOneFrame")))
	{
		return true;
	}

	for (const int32 Requested : SampleCounts)
	{
		FVaCuusCommandBuffer Buffer = MakeBufferWithShapes();

		// First mask: left half. Nothing is drawn under it that the second could be confused
		// with, so the assertions below are purely about where the SECOND draw landed.
		AddEnableClipMask(Buffer, true);
		AddRenderToClipMask(Buffer, EVaCuusClipMaskOp::Set, GeomLeftHalf);
		AddEnableClipMask(Buffer, false);

		// Second mask: top half, replacing the first (a `Set`, as ElementUtilities emits for the
		// first clipping ancestor of any new chain).
		AddEnableClipMask(Buffer, true);
		AddRenderToClipMask(Buffer, EVaCuusClipMaskOp::Set, GeomTopHalf);
		AddDraw(Buffer, GeomFullView);

		const FReplayResult Result = ReplayAndRead(Buffer, Requested);
		if (!TestTrue(TEXT("Readback completed"), Result.bRead))
		{
			return false;
		}

		const int32 SampleCount = Result.SampleCount;
		TestEqual(*FString::Printf(TEXT("%dx: top-right is inside the live mask"), SampleCount), Result.AlphaAt(96, 32),
			255);
		TestEqual(*FString::Printf(TEXT("%dx: bottom-LEFT is the retired mask's region and must stay clear"), SampleCount),
			Result.AlphaAt(32, 96), 0);
		TestEqual(*FString::Printf(TEXT("%dx: bottom-right is inside neither"), SampleCount), Result.AlphaAt(96, 96), 0);
	}

	return true;
}

/**
 * SetInverse: the area OUTSIDE the geometry passes (RenderInterface.h:12). Box-shadow's operation
 * — box-shadow needs two more things this bead does not build, but the clip-mask half of it is
 * this, and an unimplemented SetInverse would silently invert every shadow's clip.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusClipMaskInverseTest, "VaCuus.Render.ClipMask.SetInverse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusClipMaskInverseTest::RunTest(const FString& Parameters_)
{
	using namespace VaCuusClipMaskTest;
	if (SkippedUnderNullRHI(TEXT("VaCuus.Render.ClipMask.SetInverse")))
	{
		return true;
	}

	for (const int32 Requested : SampleCounts)
	{
		FVaCuusCommandBuffer Buffer = MakeBufferWithShapes();
		AddEnableClipMask(Buffer, true);
		AddRenderToClipMask(Buffer, EVaCuusClipMaskOp::SetInverse, GeomLeftHalf);
		AddDraw(Buffer, GeomFullView);

		const FReplayResult Result = ReplayAndRead(Buffer, Requested);
		if (!TestTrue(TEXT("Readback completed"), Result.bRead))
		{
			return false;
		}

		const int32 SampleCount = Result.SampleCount;
		TestEqual(*FString::Printf(TEXT("%dx: inside the shape is CLIPPED by SetInverse"), SampleCount),
			Result.AlphaAt(32, 64), 0);
		TestEqual(*FString::Printf(TEXT("%dx: outside the shape is drawn"), SampleCount), Result.AlphaAt(96, 64), 255);
	}

	return true;
}

/**
 * THE ALLOCATION, which is the whole cost of this feature and therefore the thing a buyer will
 * measure. Two claims, and the first is the one that keeps the feature free for documents that do
 * not use it:
 *
 *   1. a buffer with no clip mask allocates NO stencil at all;
 *   2. a buffer with one allocates exactly extent x samples x the platform's bytes per sample.
 *
 * BufferUsesClipMask is asserted directly too, since it is the predicate claim (1) rests on and it
 * needs no GPU — so that half runs under -nullrhi with the rest of the suite.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusClipMaskAllocationTest, "VaCuus.Render.ClipMask.LazyStencilAllocation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusClipMaskAllocationTest::RunTest(const FString& Parameters_)
{
	using namespace VaCuusClipMaskTest;

	// The predicate, with no RHI in the room.
	{
		FVaCuusCommandBuffer Plain = MakeBufferWithShapes();
		AddDraw(Plain, GeomFullView);
		TestFalse(TEXT("A draw-only buffer needs no stencil"), VaCuusReplay::BufferUsesClipMask(Plain));

		// An enable/disable pair with no geometry is NOT a mask: RenderManager::ApplyClipMask
		// emits EnableClipMask(false) on every teardown (RenderManager.cpp:158-159), so a frame
		// that never masked anything still carries disable edges.
		FVaCuusCommandBuffer EnableOnly = MakeBufferWithShapes();
		AddEnableClipMask(EnableOnly, true);
		AddEnableClipMask(EnableOnly, false);
		AddDraw(EnableOnly, GeomFullView);
		TestFalse(TEXT("Enable edges alone need no stencil"), VaCuusReplay::BufferUsesClipMask(EnableOnly));

		FVaCuusCommandBuffer Masked = MakeBufferWithShapes();
		AddEnableClipMask(Masked, true);
		AddRenderToClipMask(Masked, EVaCuusClipMaskOp::Set, GeomLeftHalf);
		AddDraw(Masked, GeomFullView);
		TestTrue(TEXT("A recorded mask needs a stencil"), VaCuusReplay::BufferUsesClipMask(Masked));
	}

	if (SkippedUnderNullRHI(TEXT("VaCuus.Render.ClipMask.LazyStencilAllocation")))
	{
		return true;
	}

	for (const int32 Requested : SampleCounts)
	{
		{
			FVaCuusCommandBuffer Buffer = MakeBufferWithShapes();
			AddDraw(Buffer, GeomFullView);
			const FReplayResult Result = ReplayAndRead(Buffer, Requested);
			TestEqual(*FString::Printf(TEXT("%dx: a document that never masks pays nothing"), Result.SampleCount),
				Result.StencilBytes, uint64(0));
		}

		FVaCuusCommandBuffer Buffer = MakeBufferWithShapes();
		AddEnableClipMask(Buffer, true);
		AddRenderToClipMask(Buffer, EVaCuusClipMaskOp::Set, GeomLeftHalf);
		AddDraw(Buffer, GeomFullView);

		const FReplayResult Result = ReplayAndRead(Buffer, Requested);
		const uint64 Expected = uint64(ViewExtent) * uint64(ViewExtent) * uint64(GPixelFormats[PF_DepthStencil].BlockBytes)
			* uint64(Result.SampleCount);
		TestEqual(*FString::Printf(TEXT("%dx: the stencil is sized for the view and the sample count"), Result.SampleCount),
			Result.StencilBytes, Expected);

		// The buyer-facing number, at the resolution perf-guide.md quotes, printed rather than
		// derived by a reader: this is what the feature costs per view when it is in use.
		const double At1080p = double(1920) * double(1080) * double(GPixelFormats[PF_DepthStencil].BlockBytes)
			* double(Result.SampleCount) / (1024.0 * 1024.0);
		AddInfo(FString::Printf(TEXT("clip-mask stencil at %dx: %.2f MiB per view at 1920x1080"), Result.SampleCount,
			At1080p));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
