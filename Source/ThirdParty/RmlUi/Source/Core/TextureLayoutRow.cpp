#include "TextureLayoutRow.h"
#include "TextureLayout.h"

namespace Rml {

TextureLayoutRow::TextureLayoutRow()
{
	height = 0;
}

TextureLayoutRow::~TextureLayoutRow() {}

int TextureLayoutRow::Generate(TextureLayout& layout, int max_width, int y)
{
	// VaCuus patch #15 follow-up (VENDORED_TAG.txt): the gutter is two texels, not one. A sub-pixel glyph quad is one
	// column wider than its bitmap and its u starts up to a texel early (FontFaceLayer::GenerateGeometry), so under a
	// transform a bilinear sample at either edge reaches two columns out -- one transparent column let it read the
	// neighbouring glyph.
	int width = 2;
	int first_unplaced_index = 0;
	int placed_rectangles = 0;

	while (width < max_width)
	{
		// Find the first unplaced rectangle we can fit.
		int index;
		for (index = first_unplaced_index; index < layout.GetNumRectangles(); ++index)
		{
			TextureLayoutRectangle& rectangle = layout.GetRectangle(index);
			if (!rectangle.IsPlaced())
			{
				if (width + rectangle.GetDimensions().x + 2 <= max_width) // VaCuus patch #15 follow-up: two-texel gutter
					break;
			}
		}

		if (index == layout.GetNumRectangles())
			return placed_rectangles;

		TextureLayoutRectangle& rectangle = layout.GetRectangle(index);

		// Increment the row height if necessary.
		height = Math::Max(height, rectangle.GetDimensions().y);

		// Add this glyph onto our list and mark it as placed.
		rectangles.push_back(&rectangle);
		rectangle.Place(layout.GetNumTextures(), Vector2i(width, y));
		++placed_rectangles;

		// Increment our width. An extra pixel is added on so the rectangles aren't pushed up
		// against each other. This will avoid filtering artifacts.
		if (rectangle.GetDimensions().x > 0)
			width += rectangle.GetDimensions().x + 2; // VaCuus patch #15 follow-up: two-texel gutter

		first_unplaced_index = index + 1;
	}

	return placed_rectangles;
}

void TextureLayoutRow::Allocate(byte* texture_data, int stride)
{
	for (size_t i = 0; i < rectangles.size(); ++i)
		rectangles[i]->Allocate(texture_data, stride);
}

int TextureLayoutRow::GetHeight() const
{
	return height;
}

void TextureLayoutRow::Unplace()
{
	for (size_t i = 0; i < rectangles.size(); ++i)
		rectangles[i]->Unplace();
}

} // namespace Rml
