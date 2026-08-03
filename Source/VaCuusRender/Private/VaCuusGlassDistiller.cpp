// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusGlassDistiller.h"

#include "VaCuusDefines.h"

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
};
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
		FilterSigmas.Add(Pair.Key, Pair.Value.Sigma);
	}

	// The parse state: scissor, clip-mask list and layer stack, tracked exactly as the
	// replayer would apply them.
	TOptional<FIntRect> Scissor;
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

			case EVaCuusCommandType::EnableClipMask:
				// BOTH edges clear the list: the enable edge is RmlUi's "a new mask list
				// replaces the old one" signal (RenderManager::ApplyClipMask,
				// RenderManager.cpp:156-176), the disable edge ends the mask's scope.
				ActiveMasks.Reset();
				break;

			case EVaCuusCommandType::RenderToClipMask:
			{
				ActiveMasks.Add({Command.ClipMaskOp, Command.Geometry, Command.Translation});

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
						const FVaCuusFilterHandle Handle = Buffer.CompositeFilters[Command.FilterOffset + Index];
						if (const float* Sigma = FilterSigmas.Find(Handle))
						{
							SigmaSquared += (*Sigma) * (*Sigma);
						}
						else if (!bWarnedUnresolvedFilter)
						{
							// One line, once: RmlUi cannot produce this (a composite only
							// names live handles), so it is either a recorder bug or a
							// buffer distilled out of publish order.
							bWarnedUnresolvedFilter = true;
							UE_LOG(LogVaCuus, Warning,
								TEXT("Glass distiller: composite references filter %llu with no recorded sigma; entry dropped"), Handle);
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
					// this a glass entry.
					FPendingGrab Grab;
					if (PendingGrabs.RemoveAndCopyValue(Command.SourceLayer, Grab))
					{
						FVaCuusGlassEntry& Entry = Entries.AddDefaulted_GetRef();
						Entry.SampleRegion = Grab.SampleRegion;
						Entry.DrawRegion = Scissor.Get(FullView);
						Entry.Sigma = Grab.Sigma;

						// The rounded mask: the Set draw of the active list (first —
						// ElementUtilities.cpp:165-169 makes the first Set and ancestors
						// Intersect). v1 draws the Set mask only; ancestor ROUNDED clipping
						// over a glass panel is out of scope (spec §11 — root-level
						// elements), while ancestor square clipping still lands via
						// DrawRegion's scissor.
						if (ActiveMasks.Num() > 0 && ActiveMasks[0].Op == EVaCuusClipMaskOp::Set)
						{
							if (const TSharedPtr<const FVaCuusGeometryData>* Found = MaskGeometry.Find(ActiveMasks[0].Geometry))
							{
								Entry.MaskGeometry = *Found;
								Entry.MaskTranslation = ActiveMasks[0].Translation;
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
		FilterSigmas.Remove(Handle);
	}
	for (const FVaCuusGeometryHandle Handle : Buffer.ReleasedGeometry)
	{
		MaskGeometry.Remove(Handle);
	}
}

void FVaCuusGlassDistiller::Reset()
{
	Entries.Reset();
	FilterSigmas.Empty();
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
