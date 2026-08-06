// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusReplayRenderer.h"

#include "VaCuusDefines.h"
#include "VaCuusMaterialDraw.h"
#include "VaCuusStats.h"
#include "VaCuusUIShaders.h"

#include "ClearQuad.h"
#include "DynamicRHI.h"
#include "GlobalRenderResources.h"
#include "GlobalShader.h"
#include "PipelineStateCache.h"
#include "RHICommandList.h"
#include "RHIResourceUtils.h"
#include "RHIStaticStates.h"
#include "ShaderParameterStruct.h"
#include "Tasks/Task.h"

/**
 * The size at which a texture payload stops being uploaded inline on the render thread and
 * starts going through a parallel command list (bead VaCuus-akj.6.25).
 *
 * THE DEFAULT IS THE MEASUREMENT, not a guess. VaCuus.Render.Upload.Cost times the two calls
 * UploadNewResources issues, on the real RHI, at six sizes; on Vulkan / RTX 3090 / Linux
 * (2026-08-03) it reported render-thread totals of
 *
 *   256^2   0.2 MB   0.071 ms      2048^2   16.0 MB    1.257 ms
 *   512^2   1.0 MB   0.107 ms      4096^2   64.0 MB   23.741 ms
 *   1024^2  4.0 MB   0.256 ms      6000^2  137.3 MB   42.922 ms
 *
 * of which CreateTexture is never more than 0.26 ms -- the cost is the staging memcpy inside
 * UpdateTexture2D, and it collapses from ~13 GB/s to ~3 GB/s once the payload stops fitting
 * whatever the driver was keeping close. 4 MB is the first size whose inline cost (0.26 ms) is
 * clearly above the noise floor of the two smallest rows, and is one 1024x1024 image.
 *
 * 0 disables the async path entirely -- the one-cvar way to rule it out when an image looks
 * wrong -- and every payload takes the inline route it took before this bead.
 */
static TAutoConsoleVariable<int32> CVarVaCuusAsyncTextureUploadBytes(
	TEXT("vacuus.AsyncTextureUploadBytes"),
	4 * 1024 * 1024,
	TEXT("Texture payloads of at least this many bytes upload through a parallel RHI command list ")
	TEXT("recorded on a worker, instead of memcpy'ing into staging on the render thread. ")
		TEXT("0 = always upload inline (the pre-akj.6.25 behaviour)."));

/**
 * OPT-IN GEOMETRY ANTIALIASING FOR THE PER-VIEW RENDER TARGET (bead VaCuus-3tg), and the
 * default is 1 because this plugin's economics are the product.
 *
 * WHAT ALIASES AND WHY MSAA IS THE INSTRUMENT FOR IT. RmlUi's layout boxes are axis-aligned,
 * so their edges do not alias; glyphs come out of an already-antialiased font atlas; and
 * since VaCuus-sw1 the gradient decorators antialias their own stop edges analytically in the
 * pixel shader. What is left is POLYGON EDGES: `border-radius` arcs, tessellated as a fan with
 * a radius-dependent point count (GeometryBackgroundBorder.cpp:139), and anything under a
 * non-axis-aligned `transform`. Multisampling is exactly a polygon-edge instrument -- it
 * multiplies coverage samples and leaves texture sampling alone -- so it is the whole of the
 * remaining defect and none of the rest.
 *
 * WHY NOT SUPERSAMPLE THE VIEW INSTEAD, which was the other candidate on the bead. Three
 * findings, each of which the other option fails:
 *
 *  1. RmlUi has no context-wide scale. Resizing the context to N x its arranged rect lays the
 *     document out at 1/N of its authored size -- SetDensityIndependentPixelRatio only moves
 *     `dp`/`em`, and every sheet here is written in `px` (the lobby demo's hybrid floor already
 *     records this, VaCuusLobbyDemo.cpp:633-635). So a genuine layout supersample is
 *     document-side, not a knob.
 *  2. Rasterizing at N x with the layout left alone magnifies the FONT ATLAS: at N=2 the
 *     bilinear magnify plus the 2:1 downsample compose to a [0.125, 0.75, 0.125] kernel on
 *     text that is crisp today. Text is the one thing here that is already antialiased, so
 *     supersampling cannot improve it and provably softens it.
 *  3. It would break world panels outright. FVaCuusWorldSink::CopyToDestination skips the copy
 *     whenever the RT extent and the destination extent disagree (VaCuusWorldSink.cpp:71-78),
 *     so an N x RT means a world panel that never updates again. MSAA changes no extent, so
 *     world panels get the knob for free and correctly -- their copy, their format check and
 *     their mip chain all see the same single-sampled ViewSize texture as before.
 *
 * THE THREE COSTS THE BEAD PREDICTED, RE-CHECKED. Memory is real and is the reason for the
 * default: the MSAA target ADDS to the RT it resolves into, +15.8 MiB (2x) or +31.6 MiB (4x)
 * per view at 1080p against 7.91 today. The "resolve pass plus rewiring all three consumers"
 * is not real -- the resolve is a STORE ACTION on the pass we already have
 * (ERenderTargetActions::Clear_Resolve) and it lands in OutputRT, so no consumer changes at
 * all. The gamma caveat is real but is not MSAA's: a box resolve of a display-encoded target
 * is off wherever it averages two DIFFERENT opaque colours -- and for the coverage case this
 * bead is about it is EXACT, because a premultiplied edge against transparency stores
 * coverage * encode(C) and the average of those is the right premultiplied value for the
 * averaged coverage. Any downsample of this target has the same property, supersampling
 * included.
 *
 * PSO: nothing. Every draw path here builds from ApplyCachedRenderTargets
 * (VaCuusReplayRenderer.cpp's BindPipeline, VaCuusMaterialDraw.cpp:243), which picks the
 * sample count up from the bound RT.
 *
 * Values other than 1/2/4/8 are rounded down rather than refused -- see ResolveViewSampleCount,
 * which also explains why an unclamped value would crash rather than degrade.
 */
static TAutoConsoleVariable<int32> CVarVaCuusViewSampleCount(
	TEXT("vacuus.ViewSampleCount"),
	1,
	TEXT("MSAA sample count for the per-view UI render target: 1 (default, no MSAA), 2, 4 or 8. ")
	TEXT("Antialiases border-radius arcs and transformed elements, which are the only geometry ")
	TEXT("here that is not antialiased already. Costs one extra multisampled target per view ")
	TEXT("(N x 7.91 MiB at 1080p) -- see docs/buyer/perf-guide.md. Other values round DOWN to a ")
	TEXT("power of two, and the platform maximum still applies."));

namespace VaCuusReplay
{
int32 ResolveViewSampleCount(int32 Requested, int32 PlatformMax)
{
	// Rounded DOWN to a power of two on both sides, then min'd: FMath::FloorLog2 of a
	// non-positive int is not something to rely on, so anything under 2 short-circuits to
	// "off" first and the shift below only ever sees a value >= 2.
	const auto FloorPow2 = [](int32 Value) { return Value < 2 ? 1 : (1 << FMath::FloorLog2(uint32(Value))); };

	const int32 Wanted = FMath::Min(FloorPow2(Requested), 8);
	const int32 Allowed = FMath::Min(FloorPow2(PlatformMax), 8);
	return FMath::Max(FMath::Min(Wanted, Allowed), 1);
}

FVaCuusClipMaskStep FVaCuusClipMaskState::Step(EVaCuusClipMaskOp Op)
{
	FVaCuusClipMaskStep Result;

	switch (Op)
	{
		case EVaCuusClipMaskOp::Set:
		{
			// The wrap. Nothing bounds the number of mask changes in a frame, so the 8-bit
			// counter has to come back round somewhere, and the only way back is a clear --
			// which is exactly what the reference backend pays on EVERY Set
			// (RmlUi_Renderer_GL3.cpp:1135-1137). Here it is amortised over 224 of them.
			if (NextFreeValue > HighWaterValue)
			{
				Result.bNeedsClear = true;
				Result.ClearValue = 0;
				NextFreeValue = 1;
			}

			TestValue = NextFreeValue;
			Result.bReplace = true;
			Result.WriteValue = TestValue;
			break;
		}

		case EVaCuusClipMaskOp::SetInverse:
		{
			// ALWAYS CLEARS, and it is not an optimisation that was skipped: the passing value
			// has to be present where nothing is drawn, and a draw cannot put it there. Clearing
			// also wipes every stale value, so the counter can safely restart from the bottom.
			Result.bNeedsClear = true;
			Result.ClearValue = 1;
			TestValue = 1;
			Result.bReplace = true;
			Result.WriteValue = 0; // covered -> fails the EQUAL 1 test, i.e. the INVERSE
			break;
		}

		case EVaCuusClipMaskOp::Intersect:
		{
			// SO_SaturatedIncrement, so "in the mask already" (== TestValue) plus "covered now"
			// is the only way to reach TestValue + 1. A stale pixel is at most TestValue - 1 by
			// the class invariant, so incrementing it lands at most ON TestValue, never past it.
			//
			// The ensure is the honest edge: past 255 the increment saturates while TestValue
			// keeps climbing, and every pixel would then fail -- the mask would clip everything
			// away. Fail-closed rather than fail-open, and unreachable short of 31 nested
			// clipping ancestors in one element's chain (ElementUtilities.cpp:165 pushes one
			// entry per clipping ancestor).
			ensureMsgf(TestValue < 255,
				TEXT("Clip-mask nesting exceeded the 8-bit stencil (%u levels); the mask will clip everything"),
				TestValue);
			TestValue = FMath::Min(TestValue + 1, 255u);
			Result.bReplace = false;
			Result.WriteValue = TestValue; // unused by the increment op; carried for the test
			break;
		}
	}

	// THE INVARIANT, maintained in one place for all three operations: every value that can now
	// be present in the buffer is below this. Set wrote TestValue; Intersect can have raised a
	// stale pixel to at most TestValue; SetInverse cleared to 1 == TestValue.
	NextFreeValue = TestValue + 1;
	Result.TestValue = TestValue;
	return Result;
}

bool BufferUsesClipMask(const FVaCuusCommandBuffer& Buffer)
{
	// RenderToClipMask, not EnableClipMask: RenderManager::ApplyClipMask calls
	// EnableClipMask(false) on every mask TEARDOWN as well (RenderManager.cpp:158-159, reached
	// from ResetState at :178-190), so a frame that never masked anything still carries disable
	// edges. Only a RenderToClipMask means a stencil was actually written.
	for (const FVaCuusCommand& Command : Buffer.Commands)
	{
		if (Command.Type == EVaCuusCommandType::RenderToClipMask)
		{
			return true;
		}
	}
	return false;
}


/**
 * Pixel-space -> clip-space ortho in UE row-vector convention (v' = v * M):
 * x: 0..W -> -1..1, y: 0..H -> 1..-1 (y-down pixels, y-up clip). Incoming z is
 * flattened and clip z pinned to 0.5 — no depth buffer, and 0.5 sits safely
 * inside every RHI's 0..1 clip range. Orientation is verified visually in the
 * Task 9 PIE check.
 */
FMatrix44f MakePixelToClipMatrix(FIntPoint ViewSize)
{
	FMatrix44f M = FMatrix44f::Identity;
	M.M[0][0] = 2.0f / float(ViewSize.X);
	M.M[1][1] = -2.0f / float(ViewSize.Y);
	M.M[2][2] = 0.0f;
	M.M[3][0] = -1.0f;
	M.M[3][1] = 1.0f;
	M.M[3][2] = 0.5f;
	return M;
}

/**
 * The create desc for a recorded UI texture, in ONE place because two paths now issue it and a
 * disagreement between them would be a format bug visible only on whichever path a given image
 * happened to take. PF_R8G8B8A8: the payload is RmlUi RGBA memory order (premultiplied alpha)
 * for both generated and loaded textures -- see FVaCuusTextureData.
 */
static FRHITextureCreateDesc MakeUITextureDesc(FIntPoint Size)
{
	return FRHITextureCreateDesc::Create2D(TEXT("VaCuusUITexture"), Size, PF_R8G8B8A8)
		.SetFlags(ETextureCreateFlags::ShaderResource)
		.SetInitialState(ERHIAccess::SRVMask);
}

/** Does this payload describe exactly Size.X * Size.Y RGBA8 texels? Both upload paths refuse anything else. */
static bool IsWellFormedPayload(const FVaCuusTextureData& Data)
{
	return Data.Size.X > 0 && Data.Size.Y > 0 && int64(Data.RGBA.Num()) == int64(Data.Size.X) * int64(Data.Size.Y) * 4;
}
} // namespace VaCuusReplay

void FVaCuusReplayRenderer::Replay(FRHICommandList& RHICmdList, const FVaCuusCommandBuffer& Buffer)
{
	check(IsInRenderingThread());
	VACUUS_PERF_SCOPE(Replay);

	if (!ShouldConsume(Buffer))
	{
		return;
	}

	// Resources first: a buffer may create geometry/textures and draw with
	// them in the same frame.
	UploadNewResources(RHICmdList, Buffer);

	if (Buffer.ViewSize.X > 0 && Buffer.ViewSize.Y > 0)
	{
		// Tripwire for the drain contract: the Slate element ensured the RT at
		// graph-build time from this same buffer's ViewSize and registered it
		// for the composite; if EnsureOutputRT would recreate the RT HERE (mid
		// pass), the composite is sampling a stale texture this frame.
		ensureMsgf(!OutputRT.IsValid() || OutputRT->GetSizeXY() == Buffer.ViewSize,
			TEXT("Replay is recreating the RT mid-pass (%dx%d -> %dx%d) — a registered composite would sample a stale texture. ")
			TEXT("Only the newest queued buffer may be drawn; drain older ones via ConsumeResources"),
			OutputRT.IsValid() ? OutputRT->GetSizeXY().X : 0, OutputRT.IsValid() ? OutputRT->GetSizeXY().Y : 0,
			Buffer.ViewSize.X, Buffer.ViewSize.Y);

		EnsureOutputRT(RHICmdList, Buffer.ViewSize);
		ReplayCommands(RHICmdList, Buffer);
	}
	else
	{
		// Degenerate view (minimized window etc.): keep the previous RT
		// contents, still consume the buffer's resource traffic below.
		UE_LOG(LogVaCuus, Verbose, TEXT("Replay skipped draw pass: degenerate view size %dx%d"),
			Buffer.ViewSize.X, Buffer.ViewSize.Y);
	}

	RetireBufferResources(Buffer);
}

void FVaCuusReplayRenderer::ConsumeResources(FRHICommandList& RHICmdList, const FVaCuusCommandBuffer& Buffer)
{
	check(IsInRenderingThread());
	if (!ShouldConsume(Buffer))
	{
		return;
	}

	// Resource traffic only — no RT, no draws. See the header for why
	// consuming releases of a skipped buffer before the next one draws is
	// sound (the next buffer was recorded after these releases were issued).
	UploadNewResources(RHICmdList, Buffer);
	RetireBufferResources(Buffer);
}

bool FVaCuusReplayRenderer::ShouldConsume(const FVaCuusCommandBuffer& Buffer) const
{
	// Idempotence guard: consuming the same published buffer twice (e.g. two
	// paints of one UI frame) must not re-run resource creation or releases.
	if (Buffer.Generation == LastConsumedGeneration)
	{
		return false;
	}

	// A backwards generation is a caller bug; refuse it rather than letting
	// RetireBufferResources drag the monotonic guard backwards (which would
	// defeat the idempotence check this ensure exists to protect).
	if (!ensureMsgf(Buffer.Generation > LastConsumedGeneration,
			TEXT("Buffer generation went backwards (%llu after %llu) — buffers must arrive in publish order"),
			Buffer.Generation, LastConsumedGeneration))
	{
		return false;
	}

	return true;
}

void FVaCuusReplayRenderer::RetireBufferResources(const FVaCuusCommandBuffer& Buffer)
{
	// Deferred release AFTER the buffer's commands ran (or were legitimately
	// skipped): that buffer was the last legal user of these handles. RHI refs
	// die with the map entries.
	for (const FVaCuusGeometryHandle Handle : Buffer.ReleasedGeometry)
	{
		Geometry.Remove(Handle);
	}
	for (const FVaCuusTextureHandle Handle : Buffer.ReleasedTextures)
	{
		Textures.Remove(Handle);
	}
	for (const FVaCuusShaderHandle Handle : Buffer.ReleasedShaders)
	{
		Shaders.Remove(Handle);
	}

	LastConsumedGeneration = Buffer.Generation;
}

void FVaCuusReplayRenderer::ReleaseResources()
{
	check(IsInRenderingThread());
	Geometry.Empty();
	Textures.Empty();
	Shaders.Empty();
	OutputRT.SafeRelease();
	MSAART.SafeRelease();
	StencilRT.SafeRelease();
	bLoggedSampleCount = false;
	bLoggedStencilTarget = false;

	// Textures created for a buffer that never reached its replay pass -- teardown between graph
	// build and pass execution. Nothing dangles either way (the upload task holds its own payload
	// and its own reference to the texture); dropping them here stops a fresh recorder, whose
	// handles restart at 1, from inheriting a parked texture that belongs to a dead one.
	PendingAsyncTextures.Empty();

	// Reset the guard: after teardown this replayer can only be paired with a fresh
	// recorder, whose generations restart from 1.
	//
	// LOAD-BEARING SINCE THE M2 TASK 12 IDLE GATE, not hygiene. OutputRT above was the only
	// copy of an idle UI's pixels -- the recorder withholds the frame that would resend them
	// -- so a path that dropped the RT and left this guard where it was would refuse the very
	// first buffer of the replacement recorder as "already consumed" and blank the UI for
	// good. See the note on the declaration for what any future partial release owes.
	LastConsumedGeneration = 0;
}

void FVaCuusReplayRenderer::EnsureOutputRT(FRHICommandList& RHICmdList, FIntPoint ViewSize)
{
	// Degenerate view (minimized window etc.): keep whatever RT exists.
	if (ViewSize.X <= 0 || ViewSize.Y <= 0)
	{
		return;
	}

	if (!OutputRT.IsValid() || OutputRT->GetSizeXY() != ViewSize)
	{
		// PF_B8G8R8A8 to match Slate's default backbuffer expectations for the
		// Task 8 composite; shaders write float4, so byte order is irrelevant here.
		const FRHITextureCreateDesc Desc =
			FRHITextureCreateDesc::Create2D(TEXT("VaCuusOutputRT"), ViewSize, PF_B8G8R8A8)
				.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource)
				.SetClearValue(FClearValueBinding::Transparent)
				// Invariant: outside Replay() the RT is always in SRV state.
				.SetInitialState(ERHIAccess::SRVMask);
		OutputRT = RHICmdList.CreateTexture(Desc);
	}

	EnsureSampleTarget(RHICmdList, ViewSize);
}

void FVaCuusReplayRenderer::EnsureSampleTarget(FRHICommandList& RHICmdList, FIntPoint ViewSize)
{
	// GDynamicRHI is null under the -nullrhi automation venue; the base class's answer (8) is
	// the right default for every RHI that does not lower it, and 1 is the right answer for no
	// RHI at all -- there is nothing to rasterize into.
	const int32 PlatformMax = GDynamicRHI ? int32(GDynamicRHI->RHIGetPlatformTextureMaxSampleCount()) : 1;
	const int32 SampleCount =
		VaCuusReplay::ResolveViewSampleCount(CVarVaCuusViewSampleCount.GetValueOnRenderThread(), PlatformMax);

	if (SampleCount <= 1)
	{
		// Released rather than kept: the knob's whole cost is this allocation, so turning it off
		// at runtime has to give the memory back on the next replayed frame or the cvar is a lie.
		if (MSAART.IsValid())
		{
			UE_LOG(LogVaCuus, Log, TEXT("VaCuus view MSAA off (vacuus.ViewSampleCount=1): released the multisampled target"));
			MSAART.SafeRelease();
			bLoggedSampleCount = false;
		}
		return;
	}

	if (MSAART.IsValid() && MSAART->GetSizeXY() == ViewSize && int32(MSAART->GetDesc().NumSamples) == SampleCount)
	{
		return;
	}

	// EXTENT AND CLEAR VALUE MATCH OutputRT EXACTLY, and both halves matter: a resolve
	// attachment must agree with its source on extent (the RHI validates it), and the pass
	// clears THIS target rather than OutputRT, so a different clear value would change what an
	// uncovered pixel resolves to.
	const FRHITextureCreateDesc Desc =
		FRHITextureCreateDesc::Create2D(TEXT("VaCuusOutputMSAA"), ViewSize, PF_B8G8R8A8)
			.SetFlags(ETextureCreateFlags::RenderTargetable)
			.SetClearValue(FClearValueBinding::Transparent)
			.SetNumSamples(uint8(SampleCount))
			// It is only ever a colour attachment, so RTV is both its initial and its only
			// state; see the declaration for why that needs no transitions.
			.SetInitialState(ERHIAccess::RTV);
	MSAART = RHICmdList.CreateTexture(Desc);

	if (!bLoggedSampleCount)
	{
		bLoggedSampleCount = true;

		// The requested value is printed next to the granted one because they can differ two
		// ways -- rounded down to a power of two, or capped by the platform -- and a silently
		// downgraded quality knob is exactly the thing a buyer would measure and disbelieve.
		const int64 Bytes = int64(ViewSize.X) * int64(ViewSize.Y) * 4 * int64(SampleCount);
		UE_LOG(LogVaCuus, Log,
			TEXT("VaCuus view MSAA on: %dx MSAA at %dx%d (+%.2f MiB per view, resolving into the same %dx%d RT). ")
			TEXT("vacuus.ViewSampleCount=%d, platform max %d"),
			SampleCount, ViewSize.X, ViewSize.Y, double(Bytes) / (1024.0 * 1024.0), ViewSize.X, ViewSize.Y,
			CVarVaCuusViewSampleCount.GetValueOnRenderThread(), PlatformMax);
	}
}

void FVaCuusReplayRenderer::EnsureStencilTarget(FRHICommandList& RHICmdList, FIntPoint ViewSize)
{
	// GetReplaySampleCount(), not the cvar: EnsureSampleTarget has already run for this frame,
	// so this is the count the colour attachment REALLY has after the platform cap and the
	// power-of-two floor. A depth-stencil attachment that disagrees with its colour attachment
	// on samples is a render-pass validation failure, not a wrong pixel.
	const int32 SampleCount = GetReplaySampleCount();

	if (StencilRT.IsValid() && StencilRT->GetSizeXY() == ViewSize && int32(StencilRT->GetDesc().NumSamples) == SampleCount)
	{
		return;
	}

	// NO ShaderResource FLAG and no resolve target: nothing samples this and nothing reads it
	// after the pass. Same argument as MSAART's, one step further -- MSAART is at least a
	// resolve SOURCE, this is neither. DepthStencilTargetable is the whole requirement.
	//
	// The clear binding is what BeginRenderPass's Clear action uses, so stencil 0 here is the
	// same 0 FVaCuusClipMaskState::BeginPass() assumes. Depth 0 matches the projection's pinned
	// clip z of 0.5 in no particular way and does not need to: every depth-stencil state this
	// pass binds has bEnableDepthWrite = false and CF_Always, so the depth plane is inert.
	const FRHITextureCreateDesc Desc =
		FRHITextureCreateDesc::Create2D(TEXT("VaCuusClipMaskStencil"), ViewSize, PF_DepthStencil)
			.SetFlags(ETextureCreateFlags::DepthStencilTargetable)
			.SetClearValue(FClearValueBinding::DepthZero)
			.SetNumSamples(uint8(SampleCount))
			// Only ever a depth-stencil attachment, so DSVWrite is both its initial and its only
			// state -- the same "no second state to be in" argument MSAART's declaration makes.
			.SetInitialState(ERHIAccess::DSVWrite);
	StencilRT = RHICmdList.CreateTexture(Desc);

	if (!bLoggedStencilTarget)
	{
		bLoggedStencilTarget = true;

		// Announced because it is an allocation a buyer did not ask for by name: it appears the
		// first time a document clips under a transform or a border-radius, and the whole point
		// of the lazy allocation is that a document which does neither never sees this line.
		UE_LOG(LogVaCuus, Log,
			TEXT("VaCuus clip mask on: this view records a clip mask, so the replay pass now carries a ")
			TEXT("%dx-sampled stencil at %dx%d (+%.2f MiB per view). Nothing samples or stores it."),
			SampleCount, ViewSize.X, ViewSize.Y, double(GetClipMaskStencilBytes()) / (1024.0 * 1024.0));
	}
}

uint64 FVaCuusReplayRenderer::GetClipMaskStencilBytes() const
{
	if (!StencilRT.IsValid())
	{
		return 0;
	}

	// GPixelFormats rather than a literal 4: PF_DepthStencil is D24S8 on some platforms and
	// D32_S8X24 (8 bytes) on others, and the buyer-facing number in perf-guide.md has to be the
	// one the platform actually allocates.
	const FIntPoint Size = StencilRT->GetSizeXY();
	const uint64 BytesPerSample = uint64(GPixelFormats[PF_DepthStencil].BlockBytes);
	return uint64(Size.X) * uint64(Size.Y) * BytesPerSample * uint64(StencilRT->GetDesc().NumSamples);
}

void FVaCuusReplayRenderer::BeginAsyncTextureUploads(FRHICommandListImmediate& RHICmdList, FVaCuusCommandBuffer& Buffer)
{
	check(IsInRenderingThread());

	const int32 ThresholdBytes = CVarVaCuusAsyncTextureUploadBytes.GetValueOnRenderThread();
	if (ThresholdBytes <= 0 || Buffer.NewTextures.Num() == 0)
	{
		return;
	}

	// The same idempotence rule ShouldConsume() applies, applied one phase earlier: this runs at
	// graph-build time and the pass that consumes the buffer runs later, so a buffer that was
	// already consumed (a second paint of one UI frame) must not have its payloads taken again.
	// Deliberately NOT ShouldConsume() itself -- that one ensure()s on a backwards generation and
	// would fire here as well as at the real consumption point, doubling one report into two.
	if (Buffer.Generation <= LastConsumedGeneration)
	{
		return;
	}

	for (TPair<FVaCuusTextureHandle, FVaCuusTextureData>& Pair : Buffer.NewTextures)
	{
		FVaCuusTextureData& Data = Pair.Value;
		if (Data.RGBA.Num() < ThresholdBytes || !VaCuusReplay::IsWellFormedPayload(Data))
		{
			// A malformed payload is left for UploadNewResources: that is where the ensure that
			// names the handle and the byte count lives, and it should fire once, in one place.
			continue;
		}

		// CREATED HERE, ON THE RENDER THREAD, and that is the half that must stay synchronous:
		// every draw the replay pass records later binds this FRHITexture*, so it has to exist
		// before the pass is built. It is also the cheap half -- VaCuus.Render.Upload.Cost
		// measures CreateTexture at 0.248 ms for the 137 MB case against 42.674 ms for the
		// UpdateTexture2D that follows it, i.e. under 1% of the cost this bead is about.
		FTextureRHIRef Texture = RHICmdList.CreateTexture(VaCuusReplay::MakeUITextureDesc(Data.Size));

		// Queued BEFORE the task is launched, deliberately: the queue is what fixes this list's
		// POSITION in the immediate stream, and it must be taken while the render thread is still
		// the only writer. The engine's own order (SkeletalMeshUpdater.cpp:437-440: new
		// FRHICommandList, then QueueAsyncCommandListSubmit, then record from tasks).
		FRHICommandList* UploadCmdList = new FRHICommandList(FRHIGPUMask::All());
		RHICmdList.QueueAsyncCommandListSubmit(UploadCmdList);

		// FOREGROUND PRIORITY, WHICH IS THE EXACT OPPOSITE OF THE DECODE'S CHOICE, and the
		// difference is not taste: nothing waits for a decode (see
		// FVaCuusRecordingRenderInterface::LoadTexture's BackgroundHigh argument), while the RHI
		// thread DOES wait for this task -- it cannot replay the list until FinishRecording()
		// (RHICommandList.h:4448-4453). A background task here would be a latency-critical
		// dependency parked on the pool the engine lets be preempted. High rather than Normal for
		// the same reason: this is the tail of a frame's submission.
		//
		// A WORKERLESS PROCESS (-onethread, or any run where the scheduler never started) IS THE
		// ONE CASE THAT COULD DEADLOCK -- the RHI thread waits for FinishRecording() and nothing
		// would ever run the task -- AND IT CANNOT HAPPEN, which is why there is no cvar guard
		// for it. FScheduler::LaunchInternal branches on ActiveWorkers, and with none it EXECUTES
		// THE TASK INLINE on the launching thread (Scheduler.cpp:533, :622-630). So the upload
		// degrades to exactly the pre-akj.6.25 behaviour -- a memcpy on the render thread -- and
		// the list is finished before this line returns.
		UE::Tasks::Launch(
			UE_SOURCE_LOCATION,
			[UploadCmdList, Texture, Payload = MoveTemp(Data.RGBA), Size = Data.Size]() mutable
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(VaCuusTextureUpload);

				// THE MEMCPY THIS BEAD EXISTS TO MOVE. On Vulkan this acquires a staging buffer
				// and copies the payload into it on THIS thread, then enqueues the buffer->image
				// copy (VulkanTexture.cpp:1664-1696); the enqueued half rides the command list
				// below. Payload is moved in, so it outlives the command buffer that carried it.
				const FUpdateTextureRegion2D Region(0, 0, 0, 0, uint32(Size.X), uint32(Size.Y));
				UploadCmdList->UpdateTexture2D(Texture, 0, Region, uint32(Size.X) * 4u, Payload.GetData());

				// Last statement, and the RHI thread is blocked until it runs: "Must be called as
				// the last command in a parallel rendering task. It is not safe to continue using
				// the command list after FinishRecording() has been called"
				// (RHICommandList.h:511-519). The list frees itself once replayed.
				UploadCmdList->FinishRecording();
			},
			UE::Tasks::ETaskPriority::High);

		// PARKED, NOT INSTALLED -- see PendingAsyncTextures for the buffer-ordering bug that
		// installing straight into Textures here caused. UploadNewResources of this same buffer
		// does the install, which is still before any draw that could sample it.
		PendingAsyncTextures.Add(Pair.Key, MoveTemp(Texture));
		NumAsyncTextureUploads.fetch_add(1, std::memory_order_release);
	}
}

void FVaCuusReplayRenderer::UploadNewResources(FRHICommandList& RHICmdList, const FVaCuusCommandBuffer& Buffer)
{
	for (const TPair<FVaCuusGeometryHandle, FVaCuusGeometryData>& Pair : Buffer.NewGeometry)
	{
		const FVaCuusGeometryData& Data = Pair.Value;
		FGeometry& Geo = Geometry.Add(Pair.Key);
		if (Data.Vertices.Num() == 0 || Data.Indices.Num() == 0)
		{
			// Keep the (empty) entry so draws resolve the handle and no-op.
			continue;
		}

		Geo.VB = UE::RHIResourceUtils::CreateVertexBufferFromArray<FVaCuusVertex>(
			RHICmdList, TEXT("VaCuusVB"), EBufferUsageFlags::Static, MakeConstArrayView(Data.Vertices));
		// int32 elements -> stride 4 -> 32-bit index buffer, matching the
		// memcpy'd Rml index stream.
		Geo.IB = UE::RHIResourceUtils::CreateIndexBufferFromArray<int32>(
			RHICmdList, TEXT("VaCuusIB"), EBufferUsageFlags::Static, MakeConstArrayView(Data.Indices));
		Geo.NumVertices = Data.Vertices.Num();
		Geo.NumIndices = Data.Indices.Num();
	}

	for (const TPair<FVaCuusTextureHandle, FVaCuusTextureData>& Pair : Buffer.NewTextures)
	{
		// THE ASYNC HANDOVER, and it happens BEFORE the payload check because
		// BeginAsyncTextureUploads MOVED this payload into its task: what is left in NewTextures
		// is a zero-byte entry the ensure below would report as a corrupt buffer. Installing the
		// ref HERE rather than at graph-build time is what keeps two buffers carrying the same
		// handle in order -- see PendingAsyncTextures.
		if (FTextureRHIRef* Parked = PendingAsyncTextures.Find(Pair.Key))
		{
			Textures.Add(Pair.Key, MoveTemp(*Parked));
			PendingAsyncTextures.Remove(Pair.Key);
			continue;
		}

		const FVaCuusTextureData& Data = Pair.Value;
		if (!ensureMsgf(VaCuusReplay::IsWellFormedPayload(Data), TEXT("Texture %llu payload mismatch: %dx%d, %d bytes"),
				Pair.Key, Data.Size.X, Data.Size.Y, Data.RGBA.Num()))
		{
			continue;
		}

		FTextureRHIRef Texture = RHICmdList.CreateTexture(VaCuusReplay::MakeUITextureDesc(Data.Size));

		const FUpdateTextureRegion2D Region(0, 0, 0, 0, uint32(Data.Size.X), uint32(Data.Size.Y));
		RHICmdList.UpdateTexture2D(Texture, 0, Region, uint32(Data.Size.X) * 4u, Data.RGBA.GetData());

		Textures.Add(Pair.Key, MoveTemp(Texture));
		NumSyncTextureUploads.fetch_add(1, std::memory_order_release);
	}

	// Shaders: the "upload" is the map insert — see the member's declaration. Add on an
	// existing key is the swap, same argument as NewTextures' re-Add (cannot actually
	// happen for shaders — handles are never recycled and a desc is immutable once
	// compiled — but the map does not need to care).
	for (const TPair<FVaCuusShaderHandle, FVaCuusShaderDesc>& Pair : Buffer.NewShaders)
	{
		Shaders.Add(Pair.Key, Pair.Value);
	}
}

namespace VaCuusReplayPrivate
{
/**
 * THE SECOND PSO AXIS OF THE REPLAY PASS (bead VaCuus-4ik). Depth-stencil state and colour write
 * mask move together and only together, so they are ONE enum rather than two: every value here
 * is a (stencil behaviour, colour behaviour) pair that actually occurs, and no other pair does.
 *
 * Unmasked is the pre-4ik state and the ONLY value a document without a clip mask ever reaches,
 * so the common case is still one pipeline bound once for the whole pass.
 *
 * FILE SCOPE RATHER THAN INSIDE ReplayCommands because the material tier needs the same states:
 * it binds a full pipeline of its own and would otherwise be the one draw path in the frame that
 * ignores the mask.
 */
enum class EBoundDS : uint8
{
	None,        // nothing bound yet
	Unmasked,    // stencil test off, colour blended: an ordinary draw
	Masked,      // stencil test EQUAL <ref>, no stencil write, colour blended
	MaskReplace, // writing the mask: stencil <- <ref> where covered, NO colour
	MaskAdd      // writing the mask: stencil += 1 where covered, NO colour
};

/**
 * The one place each of those states is spelled out. Shared by the replay pass's own binds and
 * by the material tier's, so the two cannot drift into disagreeing about what "masked" means.
 *
 * bEnableBackFaceStencil stays FALSE on all three stencil states even though the rasterizer is
 * CM_None and both windings reach the ROP: with it false the RHI MIRRORS the front-face ops onto
 * the back face rather than leaving the back face at its defaults (VulkanState.cpp:302-316 does
 * exactly that assignment). Setting it true would mean writing every op twice for no behavioural
 * difference.
 *
 * bEnableDepthWrite is false and the depth test CF_Always in every one of them, which is what
 * lets the depth plane stay inert next to MakePixelToClipMatrix's pinned clip z of 0.5.
 */
FRHIDepthStencilState* DepthStencilStateFor(EBoundDS Which)
{
	switch (Which)
	{
		case EBoundDS::Masked:
			// Read the mask, never disturb it: the 0x00 WRITE mask is what makes a masked draw
			// safe to run between two mask builds.
			return TStaticDepthStencilState<false, CF_Always, true, CF_Equal, SO_Keep, SO_Keep, SO_Keep, false, CF_Always,
				SO_Keep, SO_Keep, SO_Keep, 0xFF, 0x00>::GetRHI();

		case EBoundDS::MaskReplace:
			return TStaticDepthStencilState<false, CF_Always, true, CF_Always, SO_Replace, SO_Replace, SO_Replace, false,
				CF_Always, SO_Keep, SO_Keep, SO_Keep, 0xFF, 0xFF>::GetRHI();

		case EBoundDS::MaskAdd:
			// SATURATED, not plain SO_Increment: plain increment WRAPS 255 -> 0, which would turn
			// an over-nested mask from "clips everything" into "clips a stale region". Saturation
			// keeps that failure monotone, and FVaCuusClipMaskState::Step ensures long before it.
			return TStaticDepthStencilState<false, CF_Always, true, CF_Always, SO_SaturatedIncrement, SO_SaturatedIncrement,
				SO_SaturatedIncrement, false, CF_Always, SO_Keep, SO_Keep, SO_Keep, 0xFF, 0xFF>::GetRHI();

		default:
			// Unmasked (and None, which no bind ever asks for): byte-for-byte the state this pass
			// bound before the clip-mask work, so a document that never masks is untouched.
			return TStaticDepthStencilState<false, CF_Always>::GetRHI();
	}
}
} // namespace VaCuusReplayPrivate

void FVaCuusReplayRenderer::ReplayCommands(FRHICommandList& RHICmdList, const FVaCuusCommandBuffer& Buffer)
{
	const FIntPoint RTSize = Buffer.ViewSize;
	const FMatrix44f Projection = VaCuusReplay::MakePixelToClipMatrix(RTSize);
	int32 NumDrawCalls = 0;

	/** Mask builds, kept apart from NumDrawCalls: they write stencil, never colour. */
	int32 NumClipMaskDraws = 0;

	// TOptionalShaderMapRef, NOT TShaderMapRef: the hard ref checkf()s on a missing
	// shader (GlobalShader.h:201) and under the automation suite's -nullrhi no global
	// shader is ever compiled, so the first test that drove a replay took the whole
	// suite down (the M5 Task 5 spike's recorded observation; it dodged the problem by
	// driving its material draw below Replay -- the world sink cannot, its replay IS
	// the arrival path). On any real RHI the guard is dead code: an incomplete global
	// shader map is fatal at engine startup long before a frame records. The early
	// return is BEFORE the RTV transition, so the SRVMask-outside-Replay invariant
	// holds on the path that skips.
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TOptionalShaderMapRef<FVaCuusUIVS> VertexShader(ShaderMap);
	TOptionalShaderMapRef<FVaCuusUIPS> PixelShader(ShaderMap);
	TOptionalShaderMapRef<FVaCuusGradientPS> GradientShader(ShaderMap);
	if (!VertexShader.IsValid() || !PixelShader.IsValid() || !GradientShader.IsValid())
	{
		UE_LOG(LogVaCuus, Verbose, TEXT("Replay skipped draw pass: global shaders unavailable (null RHI)"));
		return;
	}

	// THE MSAA FORK (bead VaCuus-3tg), and it is the whole of it: with the knob on, the draws
	// below rasterize into the multisampled companion and the pass's STORE ACTION resolves them
	// into OutputRT at EndRenderPass. Nothing between these two lines changes -- same viewport,
	// same scissor arithmetic, same projection, same PSOs (every BindPipeline here builds from
	// ApplyCachedRenderTargets, which reads the sample count off the bound RT), and the same
	// single-sampled OutputRT for everything downstream.
	//
	// The DESTINATION is the only resource whose state moves. OutputRT goes SRVMask -> RTV
	// without the knob and SRVMask -> ResolveDst with it (the access RDG itself asserts for a
	// resolve target, RHIValidationContext.h:1078); MSAART is created in RTV and stays there.
	const bool bResolve = MSAART.IsValid();
	FRHITexture* ColorTarget = bResolve ? MSAART.GetReference() : OutputRT.GetReference();
	const ERHIAccess DestAccess = bResolve ? ERHIAccess::ResolveDst : ERHIAccess::RTV;

	// THE CLIP-MASK FORK (bead VaCuus-4ik), and it is paid for only by documents that use one.
	// The scan is what makes the stencil allocation lazy; EnsureStencilTarget must run HERE,
	// before BeginRenderPass, because creating a texture inside a render pass is illegal.
	const bool bClipMask = VaCuusReplay::BufferUsesClipMask(Buffer);
	if (bClipMask)
	{
		EnsureStencilTarget(RHICmdList, RTSize);
	}

	RHICmdList.Transition(FRHITransitionInfo(OutputRT, ERHIAccess::SRVMask, DestAccess));

	FRHIRenderPassInfo RPInfo(ColorTarget,
		bResolve ? ERenderTargetActions::Clear_Resolve : ERenderTargetActions::Clear_Store,
		bResolve ? OutputRT.GetReference() : nullptr);

	// ASSIGNED RATHER THAN PASSED TO A CONSTRUCTOR, and that is forced rather than stylistic:
	// every FRHIRenderPassInfo overload that takes both a colour resolve and a depth target
	// asserts `check(!ResolveColorRT || ResolveColorRT->IsMultisampled())`
	// (RHIResources.h:5526) -- which is the wrong texture to test, since a resolve DESTINATION
	// is single-sampled by definition, and our OutputRT would trip it. The fields are public and
	// the three-argument overload above already left them at "no depth", so filling them in
	// afterwards is the same render pass with none of the false assertion.
	if (bClipMask && StencilRT.IsValid())
	{
		RPInfo.DepthStencilRenderTarget.DepthStencilTarget = StencilRT.GetReference();
		RPInfo.DepthStencilRenderTarget.ResolveTarget = nullptr;

		// CLEAR AT ENTRY, DISCARD AT EXIT. The clear is what FVaCuusClipMaskState::BeginPass()
		// assumes (stencil 0 everywhere, so its first Set can use value 1), and it is free --
		// a tile-based GPU folds a clear-on-load into the pass and never touches memory for it.
		// DontStore is what keeps this attachment invisible downstream: nothing reads the
		// stencil after EndRenderPass, so there is no store and no resolve.
		RPInfo.DepthStencilRenderTarget.Action = EDepthStencilTargetActions::ClearDepthStencil_DontStoreDepthStencil;

		// DepthWrite_StencilWrite, not DepthNop_StencilWrite, even though no state below writes
		// depth: Nop asks the RHI to leave that plane's layout alone, which then needs
		// RPInfo.NopAccess set correctly for the RHIs that track planes separately
		// (RHIResources.h:5374-5375). Declaring both writable is the permissive answer, costs one
		// clear of a plane we discard anyway, and cannot be got subtly wrong.
		RPInfo.DepthStencilRenderTarget.ExclusiveDepthStencil = FExclusiveDepthStencil::DepthWrite_StencilWrite;
	}

	RHICmdList.BeginRenderPass(RPInfo, TEXT("VaCuusReplay"));
	{
		RHICmdList.SetViewport(0.0f, 0.0f, 0.0f, float(RTSize.X), float(RTSize.Y), 1.0f);

		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
		// Premultiplied-alpha blend for both color and alpha, so the RT
		// accumulates a premultiplied image ready for the Task 8 composite.
		GraphicsPSOInit.BlendState =
			TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI();
		GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GVaCuusVertexDeclaration.VertexDeclarationRHI;
		GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;

		// THE MID-PASS PSO SWITCH (M5 spec §2(e)) — DrawShader is the first command that
		// changes pipelines inside this pass. Between UI and Gradient only the PIXEL
		// SHADER differs: blend, rasterizer, depth-stencil, vertex declaration and VS are
		// shared, so a switch is one PSO-cache hit, not a new state vector. Bound LAZILY
		// on demand: a geometry-only buffer (every pre-M5 document) binds the UI pipeline
		// once and never switches, and N consecutive DrawShaders cost one switch, not N.
		// Scissor and viewport survive the switch — they are command-list state, not PSO
		// state (the glass draw's own pattern: set once, draw through PSO binds,
		// VaCuusSlateElement.cpp:556-584).
		//
		// Material (M5 Task 5b) is the odd one out: a material draw is a FULL pipeline
		// (its VS differs too) bound inside DrawMaterial_RenderThread, so its enum value
		// here means "something else is bound now" — it never binds through this lambda,
		// it only forces the next UI/Gradient draw to rebind. Same-material runs are
		// deduplicated by the material path's own memo (FPassState::BoundMaterialPS).
		enum class EBoundPS : uint8
		{
			None,
			UI,
			Gradient,
			Material
		};

		using EBoundDS = VaCuusReplayPrivate::EBoundDS;

		// The material path's per-pass state: one lazy view UB + the bound-material memo.
		VaCuusMaterialDraw::FPassState MaterialPassState;

		// The stencil value protocol, and the enable flag EnableClipMask toggles. BeginPass()
		// matches the render pass's own stencil clear to 0 just above.
		VaCuusReplay::FVaCuusClipMaskState ClipMask;
		ClipMask.BeginPass();
		bool bClipMaskEnabled = false;

		EBoundPS BoundPS = EBoundPS::None;
		EBoundDS BoundDS = EBoundDS::None;

		// int64 so "never set" is distinguishable from ref 0, which is a legal ref.
		int64 BoundStencilRef = -1;

		const auto BindPipeline = [&RHICmdList, &GraphicsPSOInit, &BoundPS, &BoundDS, &BoundStencilRef, &PixelShader,
									  &GradientShader, &MaterialPassState](EBoundPS WantedPS, EBoundDS WantedDS, uint32 WantedRef)
		{
			check(WantedPS != EBoundPS::Material); // material pipelines bind in DrawMaterial_RenderThread
			check(WantedDS != EBoundDS::None);

			if (BoundPS != WantedPS || BoundDS != WantedDS)
			{
				BoundPS = WantedPS;
				BoundDS = WantedDS;
				GraphicsPSOInit.BoundShaderState.PixelShaderRHI =
					(WantedPS == EBoundPS::Gradient) ? GradientShader.GetPixelShader() : PixelShader.GetPixelShader();

				GraphicsPSOInit.DepthStencilState = VaCuusReplayPrivate::DepthStencilStateFor(WantedDS);

				// A mask build must not touch a pixel. CW_NONE rather than a discarding shader
				// because the mask geometry IS ordinary compiled geometry -- RmlUi hands it over
				// through CompileGeometry like any other mesh (RenderManager.cpp:197-212) -- so it
				// draws with the ordinary UI shaders and the colour is simply masked off at the ROP.
				const bool bWritingMask = (WantedDS == EBoundDS::MaskReplace || WantedDS == EBoundDS::MaskAdd);
				GraphicsPSOInit.BlendState = bWritingMask
					? TStaticBlendState<CW_NONE>::GetRHI()
					: TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_One,
						  BF_InverseSourceAlpha>::GetRHI();

				SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, WantedRef);
				BoundStencilRef = int64(WantedRef);

				// The memos must invalidate each other: a material draw after this bind
				// is under OUR pipeline now, whatever material was bound before it.
				MaterialPassState.BoundMaterialPS = nullptr;
				MaterialPassState.BoundStencilRef = -1;
				return;
			}

			// Same pipeline, different reference value -- the common case inside one document,
			// since every mask level in a frame gets its own value. The ref is COMMAND-LIST state,
			// not PSO state, so this costs no pipeline switch (RHICommandList.h:3786-3796).
			if (BoundStencilRef != int64(WantedRef))
			{
				BoundStencilRef = int64(WantedRef);
				RHICmdList.SetStencilRef(WantedRef);
			}
		};

		// What an ordinary (non-mask-building) draw wants right now. Three spellings of one answer:
		// the enum for BindPipeline's memo, the RHI object for the material tier's own PSO, and
		// the reference value both bind. The ref is 0 with masking off, which is the value the
		// unmasked state ignores anyway -- keeping it constant stops a mask level left over from
		// earlier in the frame provoking pointless SetStencilRef calls between unmasked draws.
		const auto ContentDS = [&bClipMaskEnabled]() { return bClipMaskEnabled ? EBoundDS::Masked : EBoundDS::Unmasked; };
		const auto ContentDSState = [&ContentDS]() { return VaCuusReplayPrivate::DepthStencilStateFor(ContentDS()); };
		const auto ContentRef = [&bClipMaskEnabled, &ClipMask]() -> uint32 { return bClipMaskEnabled ? ClipMask.GetTestValue() : 0u; };

		// SetTransform state, already in UE row-vector convention (the recorder
		// memcpy's Rml's column-major matrix, which lands as the transpose).
		FMatrix44f CurrentTransform = FMatrix44f::Identity;

		for (const FVaCuusCommand& Command : Buffer.Commands)
		{
			switch (Command.Type)
			{
				case EVaCuusCommandType::DrawGeometry:
				{
					const FGeometry* Geo = Geometry.Find(Command.Geometry);
					if (!ensureMsgf(Geo, TEXT("Draw references unknown geometry handle %llu"), Command.Geometry))
					{
						break;
					}
					if (Geo->NumIndices < 3)
					{
						break; // Empty/degenerate geometry: nothing to draw.
					}

					BindPipeline(EBoundPS::UI, ContentDS(), ContentRef());

					// Row-vector composition, left to right = application order:
					// translate (pixels) -> Rml transform -> pixel-to-clip ortho.
					// Matches RmlUi backends, which compute
					// projection * user_transform * (position + translation)
					// in their column-vector convention.
					FMatrix44f Translate = FMatrix44f::Identity;
					Translate.M[3][0] = Command.Translation.X;
					Translate.M[3][1] = Command.Translation.Y;

					FVaCuusUIShaderParameters Parameters;
					Parameters.Projection = Translate * CurrentTransform * Projection;

					// PS contract: UITexture/UISampler must be live descriptors
					// even for untextured draws (Vulkan). GWhiteTexture is the
					// dummy; bUseTexture=0 skips the sample's contribution.
					FRHITexture* TextureRHI = GWhiteTexture->TextureRHI;
					uint32 bUseTexture = 0;
					if (Command.Texture != 0)
					{
						if (const FTextureRHIRef* Found = Textures.Find(Command.Texture))
						{
							TextureRHI = *Found;
							bUseTexture = 1;
						}
						else
						{
							ensureMsgf(false, TEXT("Draw references unknown texture handle %llu"), Command.Texture);
						}
					}
					Parameters.UITexture = TextureRHI;
					Parameters.UISampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
					Parameters.bUseTexture = bUseTexture;

					// One shared parameter struct, bound per stage: each stage
					// only picks up what survives in its parameter map (VS:
					// Projection; PS: texture path). 5.8 non-RDG path verified
					// in Task 6: scratch parameters + batched set per shader.
					{
						FRHIBatchedShaderParameters& BatchedParameters = RHICmdList.GetScratchShaderParameters();
						SetShaderParameters(BatchedParameters, VertexShader, Parameters);
						RHICmdList.SetBatchedShaderParameters(VertexShader.GetVertexShader(), BatchedParameters);
					}
					{
						FRHIBatchedShaderParameters& BatchedParameters = RHICmdList.GetScratchShaderParameters();
						SetShaderParameters(BatchedParameters, PixelShader, Parameters);
						RHICmdList.SetBatchedShaderParameters(PixelShader.GetPixelShader(), BatchedParameters);
					}

					RHICmdList.SetStreamSource(0, Geo->VB, 0);
					RHICmdList.DrawIndexedPrimitive(Geo->IB,
						/*BaseVertexIndex=*/0, /*FirstInstance=*/0, uint32(Geo->NumVertices),
						/*StartIndex=*/0, uint32(Geo->NumIndices / 3), /*NumInstances=*/1);
					++NumDrawCalls;
					break;
				}

				case EVaCuusCommandType::DrawShader:
				{
					// M5 decorators stage 1 (spec §2(e)): the recorded RenderShader — fill
					// Command.Geometry with the compiled gradient/builtin in Command.Shader.
					// Command.Texture is recorded but unused: no RmlUi shader decorator
					// passes one (geometry.Render(offset, {}, shader),
					// DecoratorShader.cpp:61-65 and the gradient equivalents), and the
					// gradient PS samples nothing.
					const FVaCuusShaderDesc* Desc = Shaders.Find(Command.Shader);
					if (!ensureMsgf(Desc, TEXT("DrawShader references unknown shader handle %llu"), Command.Shader))
					{
						break;
					}
					const FGeometry* Geo = Geometry.Find(Command.Geometry);
					if (!ensureMsgf(Geo, TEXT("DrawShader references unknown geometry handle %llu"), Command.Geometry))
					{
						break;
					}
					if (Geo->NumIndices < 3)
					{
						break;
					}

					// THE MATERIAL TIER (M5 Task 5b): recorded RmlUi geometry filled by an
					// MD_UI material — the spike's mechanism as a recorded command instead
					// of an injection, so recorded scissor state and z-order apply exactly
					// like any other draw. Same matrix math as every case here. The
					// material path binds its own full PSO (its VS differs too), so a
					// DRAWN material moves the memo to Material — the next UI/gradient
					// draw rebinds, and that rebind nulls the material path's own memo
					// (the handshake in BindPipeline). A SKIPPED draw (unresolved id,
					// pair-less walk) touched no pipeline and moves neither memo.
					if (Desc->Kind == EVaCuusShaderKind::Material)
					{
						FMatrix44f Translate = FMatrix44f::Identity;
						Translate.M[3][0] = Command.Translation.X;
						Translate.M[3][1] = Command.Translation.Y;

						const bool bDrawn = VaCuusMaterialDraw::DrawMaterial_RenderThread(RHICmdList, MaterialPassState,
							RTSize, Translate * CurrentTransform * Projection, Desc->MaterialId, Desc->BuiltinKey,
							Geo->VB, Geo->IB, Geo->NumVertices, Geo->NumIndices, ContentDSState(), ContentRef());
						if (bDrawn)
						{
							BoundPS = EBoundPS::Material;

							// The material's own PSO is bound now, so OUR depth-stencil memo is
							// stale too — not just the pixel shader one. Both have to say "unknown"
							// or the next UI draw at the same mask level would skip its rebind and
							// draw under the material's pipeline.
							BoundDS = EBoundDS::None;
							BoundStencilRef = -1;
							++NumDrawCalls;
						}
						break;
					}

					// The dictionary -> uniform conversion is the reference backend's
					// (RmlUi_Renderer_GL3.cpp:1646-1674): P/V per kind, stops as resolved.
					// The three Max/IsNearlyZero guards are ours — the reference divides by
					// whatever arrives, and a degenerate paint box would put NaN in the RT.
					// Memzero, not member-by-member: a shader parameter struct's default
					// constructor is EMPTY (INTERNAL_SHADER_PARAMETER_STRUCT_BEGIN's `{}`
					// suffix, ShaderParameterMacros.h:1330-1334, :1412), and the stop arrays
					// beyond NumStops would otherwise upload stack garbage as uniforms.
					FVaCuusGradientPS::FParameters PSParameters;
					FMemory::Memzero(PSParameters);
					PSParameters.bRepeating = Desc->bRepeating;
					PSParameters.BuiltinDimensions = FVector2f(FMath::Max(Desc->Dimensions.X, 1.0f), FMath::Max(Desc->Dimensions.Y, 1.0f));

					switch (Desc->Kind)
					{
						case EVaCuusShaderKind::LinearGradient:
							PSParameters.GradientMode = 0;
							PSParameters.GradientP = Desc->P0;
							PSParameters.GradientV = Desc->P1 - Desc->P0;
							if (PSParameters.GradientV.IsNearlyZero())
							{
								// Zero-length gradient line: dot(V,V) divides in the PS.
								// Any direction paints the whole box with an edge stop.
								PSParameters.GradientV = FVector2f(1.0f, 0.0f);
							}
							break;

						case EVaCuusShaderKind::RadialGradient:
							PSParameters.GradientMode = 1;
							PSParameters.GradientP = Desc->Center;
							// The reference's 2d curvature, 1/radius per axis.
							PSParameters.GradientV = FVector2f(
								1.0f / FMath::Max(Desc->Radius.X, 0.01f), 1.0f / FMath::Max(Desc->Radius.Y, 0.01f));
							break;

						case EVaCuusShaderKind::ConicGradient:
							PSParameters.GradientMode = 2;
							PSParameters.GradientP = Desc->Center;
							PSParameters.GradientV = FVector2f(FMath::Cos(Desc->Angle), FMath::Sin(Desc->Angle));
							break;

						case EVaCuusShaderKind::Builtin:
						{
							// Known-valid by the recorder's registry check; INDEX_NONE here
							// would mean the registry changed between record and replay,
							// which a static map cannot do. Clamped to the glass-panel mode
							// under the ensure so even that impossibility draws something
							// deterministic rather than reading mode garbage.
							const int32 Mode = VaCuusBuiltinShaders::FindMode(Desc->BuiltinKey);
							ensureMsgf(Mode != INDEX_NONE, TEXT("DrawShader carries unregistered builtin '%s'"), *Desc->BuiltinKey);
							PSParameters.GradientMode = uint32(FMath::Max(Mode, 3));
							break;
						}
					}

					const int32 NumStops = FMath::Min(Desc->Stops.Num(), VaCuusMaxGradientStops);
					PSParameters.NumStops = NumStops;
					for (int32 StopIndex = 0; StopIndex < NumStops; ++StopIndex)
					{
						PSParameters.StopColors[StopIndex] = Desc->Stops[StopIndex].Color;
						// Four positions per register — the PS reads [i>>2][i&3].
						PSParameters.StopPositions[StopIndex / 4][StopIndex % 4] = Desc->Stops[StopIndex].Position;
					}

					BindPipeline(EBoundPS::Gradient, ContentDS(), ContentRef());

					// Same VS, same matrix math as DrawGeometry above.
					FMatrix44f Translate = FMatrix44f::Identity;
					Translate.M[3][0] = Command.Translation.X;
					Translate.M[3][1] = Command.Translation.Y;

					FVaCuusUIShaderParameters VSParameters;
					VSParameters.Projection = Translate * CurrentTransform * Projection;
					VSParameters.UITexture = GWhiteTexture->TextureRHI;
					VSParameters.UISampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
					VSParameters.bUseTexture = 0;

					{
						FRHIBatchedShaderParameters& BatchedParameters = RHICmdList.GetScratchShaderParameters();
						SetShaderParameters(BatchedParameters, VertexShader, VSParameters);
						RHICmdList.SetBatchedShaderParameters(VertexShader.GetVertexShader(), BatchedParameters);
					}
					{
						FRHIBatchedShaderParameters& BatchedParameters = RHICmdList.GetScratchShaderParameters();
						SetShaderParameters(BatchedParameters, GradientShader, PSParameters);
						RHICmdList.SetBatchedShaderParameters(GradientShader.GetPixelShader(), BatchedParameters);
					}

					RHICmdList.SetStreamSource(0, Geo->VB, 0);
					RHICmdList.DrawIndexedPrimitive(Geo->IB,
						/*BaseVertexIndex=*/0, /*FirstInstance=*/0, uint32(Geo->NumVertices),
						/*StartIndex=*/0, uint32(Geo->NumIndices / 3), /*NumInstances=*/1);
					++NumDrawCalls;
					break;
				}

				case EVaCuusCommandType::SetScissor:
				{
					// Rml scissor rects are y-down window pixels, same space as
					// the RT (verified against the recorder's Rectanglei
					// conversion; visual check in Task 9). Clamp: RHIs reject
					// rects outside the RT.
					const int32 MinX = FMath::Clamp(Command.Scissor.Min.X, 0, RTSize.X);
					const int32 MinY = FMath::Clamp(Command.Scissor.Min.Y, 0, RTSize.Y);
					const int32 MaxX = FMath::Clamp(Command.Scissor.Max.X, MinX, RTSize.X);
					const int32 MaxY = FMath::Clamp(Command.Scissor.Max.Y, MinY, RTSize.Y);
					RHICmdList.SetScissorRect(true, uint32(MinX), uint32(MinY), uint32(MaxX), uint32(MaxY));
					break;
				}

				case EVaCuusCommandType::DisableScissor:
				{
					RHICmdList.SetScissorRect(false, 0, 0, 0, 0);
					break;
				}

				case EVaCuusCommandType::SetTransform:
				{
					CurrentTransform = Command.Transform;
					break;
				}

				case EVaCuusCommandType::PushLayer:
				case EVaCuusCommandType::PopLayer:
				case EVaCuusCommandType::CompositeLayers:
				{
					// PASS-THROUGH IN v1, DELIBERATELY (M5 spec §2(d)) — glass is not a
					// replay effect. Replay runs only on publish and the idle gate makes
					// publishes ~never on a static HUD, so a backdrop baked here would
					// freeze over the moving scene; the M5 Task 3 pipeline instead distills
					// these commands from the BUFFER into a glass list the Slate element
					// composites per engine frame (backdrop-glass.md §5, design A).
					//
					// Skipping cannot misplace any draw: a backdrop-only sequence issues no
					// geometry between its PushLayer and PopLayer — only the two composites
					// (ElementEffects.cpp:256-281). For element `filter:`/`mask-image:`
					// content (the Exit-stage stack, ElementEffects.cpp:283-315), draws
					// recorded "into" the pushed layer land directly in the base RT and the
					// filtered composite is skipped, i.e. the element renders unfiltered —
					// the pre-M5 behavior for those properties, kept for v1.
					break;
				}

				case EVaCuusCommandType::EnableClipMask:
				{
					// The flag only steers WHICH depth-stencil state the next content draw binds
					// (ContentDS above). It deliberately does not touch the stencil buffer:
					// RenderManager::ApplyClipMask emits enable-true immediately before rebuilding
					// the mask and enable-false when the list empties (RenderManager.cpp:156-176),
					// so a disable is "stop testing", never "erase" -- and erasing would throw away
					// values FVaCuusClipMaskState has already accounted for.
					bClipMaskEnabled = (Command.bClipMaskEnable != 0);
					break;
				}

				case EVaCuusCommandType::RenderToClipMask:
				{
					// THE STENCIL PASS (bead VaCuus-4ik). Before this existed, both commands fell
					// through to a comment saying the mask "needs a stencil pass this RT does not
					// carry yet" -- and because ElementUtilities.cpp:174-175 turns SCISSOR clipping
					// off unconditionally whenever a transform is active on the clipping chain, and
					// :162-169 emits this mask in its place, dropping it meant a transformed
					// ancestor silently disabled ALL clipping in its subtree. Same sentence for a
					// border-radius on a clip container, which takes the mask path with no
					// transform involved at all.
					if (!StencilRT.IsValid())
					{
						// Only reachable if the pre-pass scan and this walk disagree, which they
						// cannot -- BufferUsesClipMask tests for exactly this command type. Guarded
						// anyway because the alternative is a stencil op against no attachment.
						ensureMsgf(false, TEXT("RenderToClipMask with no stencil target: the pre-pass scan missed it"));
						break;
					}

					const FGeometry* Geo = Geometry.Find(Command.Geometry);
					if (!ensureMsgf(Geo, TEXT("RenderToClipMask references unknown geometry handle %llu"), Command.Geometry))
					{
						break;
					}
					if (Geo->NumIndices < 3)
					{
						// A degenerate mask shape must NOT be treated as "no mask": RmlUi already
						// skipped the push for a null geometry (ElementUtilities.cpp:167-168), so
						// arriving here with one means an empty clip region, and letting the draws
						// through unmasked is the very bug this case fixes. Stepping the protocol
						// without drawing leaves the new TestValue matching nothing, i.e. clipped.
						ClipMask.Step(Command.ClipMaskOp);
						break;
					}

					const VaCuusReplay::FVaCuusClipMaskStep Step = ClipMask.Step(Command.ClipMaskOp);

					if (Step.bNeedsClear)
					{
						// A full-target stencil clear MID-PASS. DrawClearQuad with colour and depth
						// off binds exactly the state this needs (CW_NONE + stencil SO_Replace at
						// the ref, ClearQuad.cpp:44-63), and it is the engine's own way to do this
						// inside a render pass. It also binds its own PSO and its own vertex
						// declaration, so both memos below have to be dropped.
						//
						// NOT REACHED BY THE COMMON CASE: only SetInverse (box-shadow) and the
						// 224th Set in one pass ask for it -- see FVaCuusClipMaskState.
						//
						// The scissor is still whatever the recorded state left, exactly as in the
						// reference backend, where glClear is subject to the scissor test too. That
						// is sound because the scissor also bounds every draw that could read a
						// stencil value outside it.
						DrawClearQuad(RHICmdList, /*bClearColor=*/false, FLinearColor::Black, /*bClearDepth=*/false, 0.0f,
							/*bClearStencil=*/true, Step.ClearValue);
						BoundPS = EBoundPS::None;
						BoundDS = EBoundDS::None;
						BoundStencilRef = -1;
						MaterialPassState.BoundMaterialPS = nullptr;
						MaterialPassState.BoundStencilRef = -1;
					}

					// The ref carries the WRITE value here, not the test value: SO_Replace writes
					// the bound reference. For MaskAdd it is unused by the op and only has to be
					// something -- Step.WriteValue is the post-increment value, which keeps the
					// bound ref monotone with the tests that follow.
					BindPipeline(EBoundPS::UI, Step.bReplace ? EBoundDS::MaskReplace : EBoundDS::MaskAdd, Step.WriteValue);

					// SAME MATRIX MATH AS AN ORDINARY DRAW, and CurrentTransform is load-bearing
					// rather than incidental: RenderManager::ApplyClipMask calls SetTransform with
					// each clip element's OWN transform before its RenderToClipMask and restores the
					// caller's afterwards (RenderManager.cpp:164-175), so the recorded stream
					// already interleaves SetTransform commands and this reads whichever one applies.
					// That is the whole reason a transformed clip box masks the right pixels.
					FMatrix44f Translate = FMatrix44f::Identity;
					Translate.M[3][0] = Command.Translation.X;
					Translate.M[3][1] = Command.Translation.Y;

					FVaCuusUIShaderParameters Parameters;
					Parameters.Projection = Translate * CurrentTransform * Projection;
					Parameters.UITexture = GWhiteTexture->TextureRHI;
					Parameters.UISampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
					Parameters.bUseTexture = 0;

					{
						FRHIBatchedShaderParameters& BatchedParameters = RHICmdList.GetScratchShaderParameters();
						SetShaderParameters(BatchedParameters, VertexShader, Parameters);
						RHICmdList.SetBatchedShaderParameters(VertexShader.GetVertexShader(), BatchedParameters);
					}
					{
						FRHIBatchedShaderParameters& BatchedParameters = RHICmdList.GetScratchShaderParameters();
						SetShaderParameters(BatchedParameters, PixelShader, Parameters);
						RHICmdList.SetBatchedShaderParameters(PixelShader.GetPixelShader(), BatchedParameters);
					}

					RHICmdList.SetStreamSource(0, Geo->VB, 0);
					RHICmdList.DrawIndexedPrimitive(Geo->IB,
						/*BaseVertexIndex=*/0, /*FirstInstance=*/0, uint32(Geo->NumVertices),
						/*StartIndex=*/0, uint32(Geo->NumIndices / 3), /*NumInstances=*/1);

					// NOT counted in NumDrawCalls: that number feeds the VaCuus.Render.DrawCalls
					// stat, which perf-guide.md documents as the count of draws that PRODUCE
					// pixels. A mask build writes no colour. VaCuus.Render.ClipMaskDraws is its
					// own counter for the same reason.
					++NumClipMaskDraws;
					break;
				}
			}
		}

		// The spike's post-replay injection point stood here until Task 5b. Gone, not
		// moved: materials are recorded DrawShader commands now (Kind=Material above),
		// so they replay where the document put them — with recorded scissor and
		// z-order — instead of always painting over the whole frame's content.
	}
	RHICmdList.EndRenderPass();

	RHICmdList.Transition(FRHITransitionInfo(OutputRT, DestAccess, ERHIAccess::SRVMask));

	// SET, not INC, and no longer "one replay per frame so these read as per-frame". Since the
	// M2 Task 12 idle gate there are frames with NO replay at all, and what saves these two
	// numbers from latching stale values is that both are declared
	// DECLARE_DWORD_COUNTER_STAT_EXTERN (VaCuusStats.h:76-77), i.e. EStatFlags::ClearEveryFrame
	// (Stats/Stats.h:180-182). So `stat vacuus` honestly reads 0 draw calls / 0 commands on a
	// withheld frame rather than the last replay's figures.
	//
	// WHAT THAT LOOKS LIKE ON SCREEN, so nobody files it as a bug: on an idle UI the displayed
	// numbers flicker between 0 and the real per-frame count, because the frames in between
	// genuinely did not replay anything -- the composite is still running from the render
	// target. Zero here means "nothing changed", not "nothing is drawn". The steady per-second
	// view of the same thing is vacuus.M1HUD.PerfLog's published/skipped line.
	SET_DWORD_STAT(STAT_VaCuusDrawCalls, NumDrawCalls);
	SET_DWORD_STAT(STAT_VaCuusCommands, Buffer.Commands.Num());
	SET_DWORD_STAT(STAT_VaCuusClipMaskDraws, NumClipMaskDraws);
	FVaCuusPerfLog::AddDraws(NumDrawCalls);
}
