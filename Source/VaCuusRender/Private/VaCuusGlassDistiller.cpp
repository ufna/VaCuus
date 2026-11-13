// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusGlassDistiller.h"

#include "VaCuusDefines.h"
#include "VaCuusReplayRenderer.h"

namespace VaCuusGlassPrivate
{
/** A grab composite (0 -> temp, [blur...]) waiting for its write-back (temp -> 0, []). */
struct FPendingGrab
{
	FIntRect SampleRegion;
	float Sigma = 0.0f;
};

/** One RenderToClipMask recorded while the mask is enabled. */
struct FMaskDraw
{
	EVaCuusClipMaskOp Op = EVaCuusClipMaskOp::Set;
	FVaCuusGeometryHandle Geometry = 0;
	FVector2f Translation = FVector2f::ZeroVector;

	/** The parse state's CurrentTransform at this draw — see the Set case in Distill(). */
	FMatrix44f Transform = FMatrix44f::Identity;
};


/**
 * The transformed axis-aligned bounds of a mask, in view pixels. False for empty geometry.
 *
 * The perspective divide happens HERE, on the CPU, unlike the Set mask's — a rectangle has
 * no w to carry, so there is nowhere downstream to defer it to. Harmless: an affine
 * transform leaves w at 1 and this is a no-op for it.
 */
bool ComputeMaskBounds(const FVaCuusGeometryData& Geometry, const FVector2f& Translation, const FMatrix44f& Transform, FIntRect& OutBounds)
{
	if (Geometry.Vertices.Num() == 0)
	{
		return false;
	}

	FVector2f Min(TNumericLimits<float>::Max(), TNumericLimits<float>::Max());
	FVector2f Max(TNumericLimits<float>::Lowest(), TNumericLimits<float>::Lowest());
	for (const FVaCuusVertex& Vertex : Geometry.Vertices)
	{
		const FVector4f Homogeneous = Transform.TransformFVector4(
			FVector4f(Vertex.Position.X + Translation.X, Vertex.Position.Y + Translation.Y, 0.0f, 1.0f));
		const float InvW = (Homogeneous.W != 0.0f) ? (1.0f / Homogeneous.W) : 1.0f;
		const FVector2f Position(Homogeneous.X * InvW, Homogeneous.Y * InvW);
		Min.X = FMath::Min(Min.X, Position.X);
		Min.Y = FMath::Min(Min.Y, Position.Y);
		Max.X = FMath::Max(Max.X, Position.X);
		Max.Y = FMath::Max(Max.Y, Position.Y);
	}

	// Rounded OUTWARD: this rect clips DrawRegion, so it must never eat a pixel the mask
	// actually covers. Erring inward would trim the panel's own edge.
	OutBounds = FIntRect(FMath::FloorToInt(Min.X), FMath::FloorToInt(Min.Y), FMath::CeilToInt(Max.X), FMath::CeilToInt(Max.Y));
	return true;
}
} // namespace VaCuusGlassPrivate

void FVaCuusGlassDistiller::Distill(const FVaCuusCommandBuffer& Buffer)
{
	using namespace VaCuusGlassPrivate;

	// THE WHOLESALE REPLACEMENT, FIRST AND UNCONDITIONAL (spec §2(a); the removal test
	// VaCuus.Render.Glass.Removal). Every published buffer repaints the whole frame, so a
	// buffer with no glass signature means the glass is GONE — an early-out above this
	// line ("nothing glass-shaped here, keep the old list") is precisely the bug that
	// leaves the last panel's blur running forever after removal or document unload.
	Entries.Reset();
	ViewSize = Buffer.ViewSize;
	++ListGeneration;

	// New filters first: a filter compiled and composited in the same buffer must resolve
	// (same rule as the replayer's resources-before-commands ordering).
	for (const TPair<FVaCuusFilterHandle, FVaCuusFilterData>& Pair : Buffer.NewFilters)
	{
		Filters.Add(Pair.Key, Pair.Value);
	}

	// Resolves a composite's filter slice through the cross-buffer map. One line, once, for
	// a handle with no recorded payload: RmlUi cannot produce one (a composite only names
	// live handles), so it is either a recorder bug or a buffer distilled out of publish order.
	const auto ResolveFilter = [this, &Buffer](const FVaCuusCommand& Command, int32 Index) -> const FVaCuusFilterData*
	{
		const FVaCuusFilterHandle Handle = Buffer.CompositeFilters[Command.FilterOffset + Index];
		const FVaCuusFilterData* Data = Filters.Find(Handle);
		if (!Data && !bWarnedUnresolvedFilter)
		{
			bWarnedUnresolvedFilter = true;
			UE_LOG(LogVaCuus, Warning,
				TEXT("Glass distiller: composite references filter %llu with no recorded sigma or opacity; entry dropped"), Handle);
		}
		return Data;
	};

	// The parse state: scissor, transform, clip-mask list and layer stack, tracked
	// exactly as the replayer would apply them. CurrentTransform starts at identity —
	// the same value the replayer assumes at the top of its own command walk
	// (VaCuusReplayRenderer.cpp:1106) before any SetTransform has been seen.
	TOptional<FIntRect> Scissor;
	FMatrix44f CurrentTransform = FMatrix44f::Identity;
	TArray<FMaskDraw, TInlineAllocator<4>> ActiveMasks;
	TArray<FVaCuusLayerHandle, TInlineAllocator<4>> LayerStack;
	TMap<FVaCuusLayerHandle, FPendingGrab> PendingGrabs;

	// The full view is the scissorless default — a body-level backdrop grabs everything.
	const FIntRect FullView(0, 0, Buffer.ViewSize.X, Buffer.ViewSize.Y);

	for (const FVaCuusCommand& Command : Buffer.Commands)
	{
		switch (Command.Type)
		{
			case EVaCuusCommandType::SetScissor:
				Scissor = Command.Scissor;
				break;

			case EVaCuusCommandType::DisableScissor:
				Scissor.Reset();
				break;

			case EVaCuusCommandType::SetTransform:
				CurrentTransform = Command.Transform;
				break;

			case EVaCuusCommandType::EnableClipMask:
				// BOTH edges clear the list: the enable edge is RmlUi's "a new mask list
				// replaces the old one" signal (RenderManager::ApplyClipMask,
				// RenderManager.cpp:156-176), the disable edge ends the mask's scope.
				ActiveMasks.Reset();
				break;

			case EVaCuusCommandType::RenderToClipMask:
			{
				ActiveMasks.Add({Command.ClipMaskOp, Command.Geometry, Command.Translation, CurrentTransform});

				// Feed the cross-buffer map at the reference, from the SAME buffer's
				// NewGeometry — see the map's declaration for why first-reference and
				// compile always share a buffer. Fed for every mask draw, glass or not:
				// ordinary rounded-corner clipping keeps the map warm for the frame a
				// glass panel appears over an already-compiled clip shape.
				if (!MaskGeometry.Contains(Command.Geometry))
				{
					if (const FVaCuusGeometryData* Data = Buffer.NewGeometry.Find(Command.Geometry))
					{
						MaskGeometry.Add(Command.Geometry, MakeShared<const FVaCuusGeometryData>(*Data));
					}
				}
				break;
			}

			case EVaCuusCommandType::PushLayer:
				LayerStack.Push(Command.SourceLayer);
				break;

			case EVaCuusCommandType::PopLayer:
				if (LayerStack.Num() > 0)
				{
					PendingGrabs.Remove(LayerStack.Pop());
				}
				break;

			case EVaCuusCommandType::CompositeLayers:
			{
				if (Command.SourceLayer == 0 && Command.DestLayer != 0)
				{
					// The GRAB: base layer -> temp, carrying the filter list. Resolve every
					// blur sigma through the cross-buffer map; consecutive gaussians combine
					// as sqrt of the sum of squares.
					float SigmaSquared = 0.0f;
					for (int32 Index = 0; Index < Command.FilterCount; ++Index)
					{
						const FVaCuusFilterData* Filter = ResolveFilter(Command, Index);
						if (Filter && Filter->Kind == EVaCuusFilterKind::Blur)
						{
							SigmaSquared += Filter->Sigma * Filter->Sigma;
						}
					}

					// A grab with no resolvable blur produces NO glass entry (spec §2(d)):
					// the refused-filter shape arrives here as FilterCount == 0.
					if (SigmaSquared > 0.0f)
					{
						PendingGrabs.Add(Command.DestLayer, {Scissor.Get(FullView), FMath::Sqrt(SigmaSquared)});
					}
				}
				else if (Command.DestLayer == 0 && Command.SourceLayer != 0)
				{
					// The WRITE-BACK: temp -> base. Only a grab that carried a blur makes
					// this a glass entry. Its filter list is empty at full opacity and
					// carries the panel's opacity otherwise (RmlUi patch #22); several
					// would multiply, as successive opacity filters do. Resolved only for a
					// write-back that closes a grab: an Exit-stage element-filter composite
					// lands on this branch too, and its filters are none of the glass's. A
					// handle that does not resolve drops the entry, as ResolveFilter's warning
					// says: its opacity is unknown, and full strength is exactly the blurred
					// silhouette patch #22 exists to remove.
					const auto WriteBackOpacity = [&ResolveFilter](const FVaCuusCommand& WriteBack)
					{
						float Opacity = 1.0f;
						for (int32 Index = 0; Index < WriteBack.FilterCount; ++Index)
						{
							const FVaCuusFilterData* Filter = ResolveFilter(WriteBack, Index);
							if (!Filter)
							{
								return 0.0f;
							}
							if (Filter->Kind == EVaCuusFilterKind::BackdropOpacity)
							{
								Opacity *= Filter->Opacity;
							}
						}
						return Opacity;
					};

					FPendingGrab Grab;
					const bool bClosesGrab = PendingGrabs.RemoveAndCopyValue(Command.SourceLayer, Grab);
					const float Opacity = bClosesGrab ? WriteBackOpacity(Command) : 0.0f;
					if (bClosesGrab && Opacity > 0.0f)
					{
						FVaCuusGlassEntry& Entry = Entries.AddDefaulted_GetRef();
						Entry.SampleRegion = Grab.SampleRegion;
						Entry.DrawRegion = Scissor.Get(FullView);
						Entry.Sigma = Grab.Sigma;
						Entry.Opacity = Opacity;

						// The rounded mask: the Set draw of the active list (first —
						// ElementUtilities.cpp:163-169 makes the first Set and ancestors
						// Intersect). v1 draws the Set mask only; ancestor ROUNDED clipping
						// over a glass panel is out of scope (spec §11 — root-level
						// elements). Ancestor SQUARE clipping lands in DrawRegion either
						// way: through the scissor when that ancestor is untransformed, and
						// through the Intersect fold below when it is not — a transformed
						// clipping element puts NOTHING in the scissor
						// (ElementUtilities.cpp:174-178).
						if (ActiveMasks.Num() > 0 && ActiveMasks[0].Op == EVaCuusClipMaskOp::Set)
						{
							if (const TSharedPtr<const FVaCuusGeometryData>* Found = MaskGeometry.Find(ActiveMasks[0].Geometry))
							{
								Entry.MaskGeometry = *Found;
								Entry.MaskTranslation = ActiveMasks[0].Translation;

								// The clip element's own transform rides along as a MATRIX
								// rather than being pushed into the vertices. Two things
								// depend on that choice: the geometry stays the map's
								// shared untransformed copy, which is the key the
								// element's draw-buffer cache needs, and the perspective
								// divide stays on the GPU, where it is exact rather than
								// approximated in view space.
								Entry.MaskTransform = ActiveMasks[0].Transform;
							}
							else if (!bWarnedUnresolvedMask)
							{
								// Degrade to square rather than drop: a glass panel with
								// hard corners is a lesser lie than no glass at all.
								bWarnedUnresolvedMask = true;
								UE_LOG(LogVaCuus, Warning,
									TEXT("Glass distiller: clip-mask geometry %llu is not resolvable; drawing the panel square"),
									ActiveMasks[0].Geometry);
							}
						}

						// Every mask after the first is an Intersect (see above). Its exact
						// SHAPE is not drawn — one Set mask stays the drawn geometry — but
						// its BOUNDS belong in DrawRegion, which the glass draw is scissored
						// to. Strictly conservative and so it cannot erase a pixel RmlUi
						// would have painted: the clip is the INTERSECTION of the mask
						// shapes, so every drawn pixel lies inside every mask's bounds.
						for (int32 MaskIndex = 1; MaskIndex < ActiveMasks.Num(); ++MaskIndex)
						{
							const FMaskDraw& Mask = ActiveMasks[MaskIndex];
							const TSharedPtr<const FVaCuusGeometryData>* Found = MaskGeometry.Find(Mask.Geometry);
							FIntRect Bounds;
							if (Found && VaCuusGlassPrivate::ComputeMaskBounds(**Found, Mask.Translation, Mask.Transform, Bounds))
							{
								Entry.DrawRegion.Clip(Bounds);
								++Entry.BoundsOnlyMasks;
							}
						}
					}
				}
				// Layer-to-layer composites (neither side 0) belong to the Exit-stage
				// filter/mask stack the replayer also skips in v1; nothing to distill.
				break;
			}

			default:
				break;
		}
	}

	// Retirement AFTER the parse — the buffer carrying a release may still reference the
	// handle (same-frame compile+use+release; the replayer's deferred-release rule).
	// Entries keep their shared geometry refs alive past retirement by design.
	for (const FVaCuusFilterHandle Handle : Buffer.ReleasedFilters)
	{
		Filters.Remove(Handle);
	}
	for (const FVaCuusGeometryHandle Handle : Buffer.ReleasedGeometry)
	{
		MaskGeometry.Remove(Handle);
	}
}

void FVaCuusGlassDistiller::Reset()
{
	Entries.Reset();
	Filters.Empty();
	MaskGeometry.Empty();
	ViewSize = FIntPoint::ZeroValue;
	++ListGeneration;
}

FIntRect FVaCuusGlassMapping::MapRect(const FIntRect& ViewRect) const
{
	FIntRect Mapped(
		FIntPoint(FMath::RoundToInt(float(ViewRect.Min.X) * Scale.X + Offset.X), FMath::RoundToInt(float(ViewRect.Min.Y) * Scale.Y + Offset.Y)),
		FIntPoint(FMath::RoundToInt(float(ViewRect.Max.X) * Scale.X + Offset.X), FMath::RoundToInt(float(ViewRect.Max.Y) * Scale.Y + Offset.Y)));
	Mapped.Clip(ClampRect);
	return Mapped;
}

FVaCuusGlassMapping VaCuusMakeGlassMapping(
	const FIntRect& DestRect, const FVector2f& ElementsOffset, FIntPoint ViewSize, const FIntRect& SceneViewRect, FIntPoint OutputExtent)
{
	FVaCuusGlassMapping Mapping;

	// The same convention as the existing UI composite (VaCuusSlateElement.cpp:179-182):
	// DestRect is window-space, the elements texture may host the window at an offset.
	Mapping.Offset = FVector2f(float(DestRect.Min.X) + ElementsOffset.X, float(DestRect.Min.Y) + ElementsOffset.Y);
	Mapping.Scale = FVector2f(
		ViewSize.X > 0 ? float(DestRect.Width()) / float(ViewSize.X) : 1.0f,
		ViewSize.Y > 0 ? float(DestRect.Height()) / float(ViewSize.Y) : 1.0f);

	// SceneViewRect bounds the scene within the elements texture (populated from the
	// window's ViewportRect, SlateRHIRenderer.cpp:1728 -> :1034) — the PIE clamp the spec
	// names; intersected with the physical extent so a degenerate SceneViewRect cannot
	// widen anything.
	Mapping.ClampRect = FIntRect(0, 0, OutputExtent.X, OutputExtent.Y);
	if (SceneViewRect.Area() > 0)
	{
		Mapping.ClampRect.Clip(SceneViewRect);
	}

	return Mapping;
}

FMatrix44f VaCuusMakeGlassMaskMatrix(const FVaCuusGlassEntry& Entry, const FVaCuusGlassMapping& Mapping, FIntPoint OutputExtent)
{
	// Row-vector composition, left to right = application order: the mask's border-box
	// offset in view pixels, the clip element's own transform, the live DestRect mapping
	// into output pixels, then the ortho. See the declaration for why the perspective
	// divide is safe to leave to the GPU at the end of this chain.
	FMatrix44f MaskToView = FMatrix44f::Identity;
	MaskToView.M[3][0] = Entry.MaskTranslation.X;
	MaskToView.M[3][1] = Entry.MaskTranslation.Y;

	FMatrix44f ViewToOutput = FMatrix44f::Identity;
	ViewToOutput.M[0][0] = Mapping.Scale.X;
	ViewToOutput.M[1][1] = Mapping.Scale.Y;
	ViewToOutput.M[3][0] = Mapping.Offset.X;
	ViewToOutput.M[3][1] = Mapping.Offset.Y;

	return MaskToView * Entry.MaskTransform * ViewToOutput * VaCuusReplay::MakePixelToClipMatrix(OutputExtent);
}

bool VaCuusGlassDrawMatchesEntry(const TSharedPtr<const FVaCuusGeometryData>& SourceGeometry, const FIntRect& SourceQuad, const FVaCuusGlassEntry& Entry)
{
	// Identity by SHARED POINTER rather than by raw address: holding the reference is what
	// makes the comparison mean "the same payload", where a bare pointer would also match a
	// freed payload whose allocation had been handed out again.
	if (SourceGeometry != Entry.MaskGeometry)
	{
		return false;
	}

	// A square entry has no geometry, so the quad generated from its DrawRegion IS its whole
	// identity. A rounded entry's DrawRegion may move freely without touching its vertices --
	// the draw is scissored to that region, not built from it.
	return Entry.MaskGeometry.IsValid() || SourceQuad == Entry.DrawRegion;
}
