// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusCommandBuffer.h"

/**
 * One glass panel distilled from a published buffer's backdrop sequence (M5 spec §2(a)):
 * everything the Slate element needs to re-create `backdrop-filter: blur()` against the
 * live scene once per ENGINE frame, long after the buffer that recorded it is gone.
 *
 * All coordinates are VIEW-space pixels of the buffer's ViewSize — window coordinates are
 * never pre-baked (spec §2(a)): the element maps regions, mask vertices and sigma through
 * the live DestRect/ElementsOffset transform every engine frame, which is what keeps the
 * list correct across a PIE viewport whose DestRect.Min is nonzero and across resizes.
 */
struct FVaCuusGlassEntry
{
	/**
	 * The backdrop READ region: the scissor active at the grab composite, i.e. the border
	 * box extended by the blur's ink overflow (3*max(sigma,1) per side —
	 * ThirdParty/RmlUi/Source/Core/ElementEffects.cpp:257-266, FilterBlur.cpp:22-27).
	 * The downsample samples the scene here so the blur has real neighbours at the edge.
	 */
	FIntRect SampleRegion;

	/**
	 * The backdrop WRITE region: the scissor active at the write-back composite — the
	 * element's border box (ElementUtilities.cpp:178-186). The glass draw is scissored to
	 * it, which is what clips a square panel and bounds a rounded one.
	 */
	FIntRect DrawRegion;

	/**
	 * Combined gaussian sigma in VIEW pixels. Multiple blur() entries on one
	 * backdrop-filter compose as consecutive gaussians, and a gaussian of a gaussian is a
	 * gaussian at sqrt(s1^2 + s2^2) — combined here so the pipeline runs ONE separable
	 * blur per entry instead of a chain.
	 */
	float Sigma = 0.0f;

	/**
	 * The panel's own opacity: the glass draw scales its coverage by it, so the blur
	 * fades with the panel instead of standing at full strength while the panel fades in
	 * over it. Read off the WRITE-BACK composite's BackdropOpacity filter (RmlUi patch #22,
	 * ElementEffects.cpp RenderStage::Enter); 1 when that composite carries none, which is
	 * every panel at full opacity. Never 0: RmlUi records no backdrop sequence at all for a
	 * fully transparent element, and the distiller drops one that says 0 regardless.
	 */
	float Opacity = 1.0f;

	/**
	 * The rounded mask: a COPY of the clip-mask geometry (the recorded
	 * RenderToClipMask(Set) between the two composites), shared with the distiller's
	 * cross-buffer map, in the clip element's OWN untransformed space. Null = square
	 * corners (scissor-only clipping) -- the element draws a plain quad over DrawRegion
	 * instead.
	 *
	 * THE LIST OWNS ITS COPY (via this shared ref): the buffer the vertices arrived in is
	 * recycled after replay, and the map entry may be retired by a later buffer's
	 * ReleasedGeometry while this list still draws -- the ref keeps the payload alive until
	 * the next wholesale replacement drops it. Staying a SHARED ref rather than a per-entry
	 * copy is also what lets the element's draw-buffer cache key on it (VaCuusSlateElement.h,
	 * FGlassDraw::SourceGeometry).
	 */
	TSharedPtr<const FVaCuusGeometryData> MaskGeometry;

	/** RenderToClipMask's Translation: the mask's border-box offset in view space. */
	FVector2f MaskTranslation = FVector2f::ZeroVector;

	/**
	 * The clip element's OWN transform at the mask draw. RenderManager::ApplyClipMask calls
	 * SetTransform with each clip element's transform before its RenderToClipMask and
	 * restores the caller's afterwards (ThirdParty/RmlUi/Source/Core/RenderManager.cpp:164-175),
	 * and a child of a transformed ancestor carries the accumulated matrix
	 * (ThirdParty/RmlUi/Source/Core/Element.cpp:3003-3018) -- so this is how a HUD panel
	 * scaled by a `transform: scale()` on its root reaches the glass draw at all. Identity
	 * for the overwhelmingly common untransformed panel.
	 *
	 * NOT applied to the vertices. It is folded into the draw matrix by
	 * VaCuusMakeGlassMaskMatrix, which keeps MaskGeometry a shared ref and leaves the
	 * perspective divide to the GPU, where it is exact.
	 */
	FMatrix44f MaskTransform = FMatrix44f::Identity;

	/**
	 * Intersect clip masks applied as BOUNDS ONLY: their transformed axis-aligned box was
	 * intersected into DrawRegion, their shape was not drawn. Zero for the ordinary panel.
	 *
	 * WHY THIS NUMBER EXISTS. Without a transform an ancestor's clip reaches the entry
	 * through the scissor, so DrawRegion already carries it. With one it does not:
	 * GetClippingRegion sets disable_scissor_clipping for a transformed clipping element and
	 * expresses that ancestor ONLY as an Intersect mask
	 * (ThirdParty/RmlUi/Source/Core/ElementUtilities.cpp:162-178). Folding the bounds
	 * restores parity, so a transform does not change what is supported. What parity does
	 * NOT restore is an ancestor's corner ROUNDING, and this counter is how that remaining
	 * gap is asserted rather than merely believed.
	 */
	int32 BoundsOnlyMasks = 0;
};

/**
 * Distills the Task 2 glass signature out of each PUBLISHED buffer into the persistent
 * glass list the Slate element composites from every engine frame (M5 spec §2(a),
 * backdrop-glass.md §5 design A).
 *
 * The signature (verified against ElementEffects.cpp:268-307 by
 * VaCuus.Render.Glass.BackdropSequence): scissor(border box + ink overflow) → PushLayer →
 * CompositeLayers(0 → temp, [blur...]) → scissor(border box) [+ clip-mask geometry for
 * border-radius] → CompositeLayers(temp → 0, [] or [backdrop-opacity]) → PopLayer.
 *
 * WHERE THIS RUNS, AND WHY (the Task 3 placement decision): on the Slate element, called
 * once per ARRIVING published buffer from SetPendingBuffer_RenderThread — never from
 * Draw_RenderThread. Publishes are the gated, ~zero-when-idle event; engine frames are
 * not, so parsing here keeps the per-engine-frame glass path down to reads of a list that
 * already exists. Element-side rather than recorder-side because the failure mode the
 * spec's removal test guards ("a distiller that early-outs on glass-free buffers leaves
 * the last panel's blur running forever", spec §2(a)) lives at the point where the OLD
 * list could survive — and that point is wherever the list persists, which is here. A
 * recorder-side distiller would still need an element-side adoption step, i.e. the same
 * bug surface one hop removed, plus a buffer-borne list every publish. The struct itself
 * is thread-agnostic data-in/data-out (single-writer, like the recorder) so unit tests
 * drive it directly with recorder-produced buffers, no render thread required.
 *
 * THE WHOLESALE-REPLACEMENT INVARIANT (spec §2(a), the removal test): every call rebuilds
 * Entries from the given buffer alone — the FIRST statement of Distill() is the Reset().
 * There is no early-out above it, and must never be: each published buffer repaints the
 * whole frame (the replayer's own drain contract), so a buffer without a glass signature
 * MEANS the glass is gone.
 *
 * The two cross-buffer maps live here because their feed and retirement ride the same
 * per-published-buffer call: filter handle → sigma or opacity (compiled in one buffer, referenced by
 * composites in later ones; filters are cross-frame resources) and mask geometry handle →
 * payload copy (the clip geometry is compiled once and referenced by every subsequent
 * glass frame — cross-buffer exactly like draws). Both retire on the buffer's Released*
 * arrays, AFTER the commands are parsed (the buffer carrying a release may still
 * reference the handle — the replayer's deferred-release rule, applied to parsing).
 */
class FVaCuusGlassDistiller
{
public:
	/**
	 * Wholesale-rebuilds Entries from the buffer and updates the cross-buffer maps.
	 * Call once per published buffer, in publish order (the maps assume it, exactly as
	 * the replayer's resource maps do).
	 */
	void Distill(const FVaCuusCommandBuffer& Buffer);

	/** Teardown: list and maps. Pairs with the replayer's ReleaseResources — a fresh recorder restarts handles at 1, so stale maps would mis-resolve. */
	void Reset();

	/** The current glass list — replaced wholesale by every Distill call. */
	const TArray<FVaCuusGlassEntry>& GetEntries() const { return Entries; }

	/** ViewSize of the buffer the list came from: the denominator of the element's DestRect mapping. */
	FIntPoint GetViewSize() const { return ViewSize; }

	/** Bumped by every Distill: the element rebuilds its per-entry draw buffers only when this moved. */
	uint64 GetListGeneration() const { return ListGeneration; }

private:
	TArray<FVaCuusGlassEntry> Entries;
	FIntPoint ViewSize = FIntPoint::ZeroValue;
	uint64 ListGeneration = 0;

	/**
	 * Compiled filters by handle — blur sigmas for the grab, panel opacities for the
	 * write-back; fed from NewFilters, retired on ReleasedFilters.
	 */
	TMap<FVaCuusFilterHandle, FVaCuusFilterData> Filters;

	/**
	 * Clip-mask geometry payload copies by handle, fed lazily: the first RenderToClipMask
	 * that references a handle copies it out of that buffer's NewGeometry. Sound because
	 * clip-mask geometry is structurally DEDICATED in RmlUi — GetClipGeometry keeps it
	 * under its own BackgroundType::Clip* slot, generated only for masking
	 * (ElementBackgroundBorder.cpp:55-77), and RenderManager defers the compile to first
	 * use (RenderManager.cpp:205-206) — so a mask handle's first reference and its
	 * NewGeometry entry arrive in the SAME buffer, and later buffers hit this map.
	 * Retired on ReleasedGeometry.
	 */
	TMap<FVaCuusGeometryHandle, TSharedPtr<const FVaCuusGeometryData>> MaskGeometry;

	/** Latched: an unresolvable mask/filter handle is a contract break worth exactly one line each. */
	bool bWarnedUnresolvedMask = false;
	bool bWarnedUnresolvedFilter = false;
};

/**
 * The per-engine-frame coordinate mapping (M5 spec §2(a)): view space → the Slate
 * elements texture. Pure data so the PIE-shaped case (DestRect.Min != 0) is a unit test.
 *
 *   offset = DestRect.Min + ElementsOffset
 *   scale  = DestRect.Size / ViewSize   (applied to regions, mask vertices AND sigma)
 *   every mapped rect clamped to SceneViewRect ∩ the output extent
 */
struct FVaCuusGlassMapping
{
	FVector2f Scale = FVector2f(1.0f, 1.0f);
	FVector2f Offset = FVector2f::ZeroVector;

	/** SceneViewRect ∩ (0,0,OutputExtent); every mapped rect is clipped into it. */
	FIntRect ClampRect;

	/** View-space rect → clamped output-space rect (empty if fully clipped). */
	FIntRect MapRect(const FIntRect& ViewRect) const;

	/** View-space sigma → output-space sigma, per axis (X for the horizontal pass, Y for the vertical). */
	FVector2f MapSigma(float ViewSigma) const { return FVector2f(ViewSigma * Scale.X, ViewSigma * Scale.Y); }
};

FVaCuusGlassMapping VaCuusMakeGlassMapping(
	const FIntRect& DestRect, const FVector2f& ElementsOffset, FIntPoint ViewSize, const FIntRect& SceneViewRect, FIntPoint OutputExtent);

/**
 * The mask's draw matrix: mask vertices -> clip space, in ROW-VECTOR order
 *
 *   translate(MaskTranslation) * MaskTransform * (mapping scale, offset) * pixel-to-clip
 *
 * the same composition the replayer's stencil pass draws the very same mask with
 * (VaCuusReplayRenderer.cpp:1525), with the perspective divide left to the GPU.
 *
 * WHY THE DIVIDE CAN BE LEFT THERE, which is what makes this exact rather than an
 * approximation for a projective MaskTransform: everything after MaskTransform is affine
 * with fourth column (0,0,0,1), so w survives untouched to the GPU's divide and the divide
 * commutes with those matrices. And the z a 3D transform produces never reaches the output
 * -- MakePixelToClipMatrix pins clip z at 0.5*w and gives z no path into x or y
 * (VaCuusReplayRenderer.cpp:214-224) -- so a rotated-in-3D panel cannot drift outside the
 * clip planes.
 *
 * Pure data in, data out: VaCuus.Render.Glass.MaskMatrix drives it with no engine at all,
 * and the Slate element uses this same function, so the shader and the test cannot drift.
 */
FMatrix44f VaCuusMakeGlassMaskMatrix(const FVaCuusGlassEntry& Entry, const FVaCuusGlassMapping& Mapping, FIntPoint OutputExtent);

/**
 * Can vertex/index buffers built from (SourceGeometry, SourceQuad) be KEPT for Entry?
 *
 * The element rebuilds its per-entry draw buffers whenever the glass list is replaced, and
 * the list is replaced by every published buffer -- so without this, a clock ticking in the
 * corner of the HUD recreates an untouched glass panel's buffers, on every publish, forever.
 *
 * THE TRANSFORM IS DELIBERATELY NOT PART OF THE KEY. It never touches the vertices: it is
 * folded into the draw matrix per engine frame by VaCuusMakeGlassMaskMatrix. So a panel
 * being animated by `transform` re-uploads nothing at all, which is the whole reason the
 * matrix is carried instead of baked.
 */
bool VaCuusGlassDrawMatchesEntry(const TSharedPtr<const FVaCuusGeometryData>& SourceGeometry, const FIntRect& SourceQuad, const FVaCuusGlassEntry& Entry);
