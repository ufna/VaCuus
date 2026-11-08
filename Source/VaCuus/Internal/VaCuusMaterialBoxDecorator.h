// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/NumericValue.h>
#include <RmlUi/Core/RenderBox.h>

/**
 * decorator: material-box(<key>, <image-size>, <margin> [, box | border])
 *
 * Slate-style Box / Border for a style-set material: a grid whose UV0 is sliced
 * as FSlateElementBatcher::AddBoxElements slices it (ElementBatcher.cpp:821-874). Border repeats
 * its edges before material vertex evaluation. <image-size> is FSlateBrush::ImageSize (`dp` follows the dp
 * ratio); <margin> is FSlateBrush::Margin as fractions 0..1, CSS order. Margin 0 is DrawAs=Image.
 */
namespace VaCuusMaterialBox
{
struct FParams
{
	Rml::String Key;
	Rml::NumericValue ImageWidth;
	Rml::NumericValue ImageHeight;
	float Margin[4] = {0.f, 0.f, 0.f, 0.f}; // top, right, bottom, left
	bool bBorder = false;
};

/** Builds a fresh mesh; optional PaintBox supplies RmlUi corner radii. Border repeats edges; excessive tile counts leave it empty. */
VACUUS_API void BuildMesh(const FParams& Params, Rml::Vector2f Offset, Rml::Vector2f Size, float W, float H,
	Rml::ColourbPremultiplied Colour, Rml::Mesh& OutMesh, const Rml::RenderBox* PaintBox = nullptr);

/** vacuus.MaterialBox: 0 draws a plain 0..1 quad (corners stretch) instead of the grid. */
VACUUS_API bool IsEnabled();

/** FVaCuusEngine only: right after Rml::Initialise() / right after Rml::Shutdown(). */
void Register();
void Unregister();
} // namespace VaCuusMaterialBox
