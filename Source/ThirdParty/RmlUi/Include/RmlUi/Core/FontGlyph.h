#pragma once

#include "Types.h"

namespace Rml {

/**
    Metrics and bitmap data for a single glyph within a font face.
 */

class RMLUICORE_API FontGlyph {
public:
	/// The distance from the cursor (positioned vertically on the baseline) to the top-left corner of this glyph's bitmap.
	Vector2i bearing;
	/// The glyph's advance; this is how far the cursor will be moved along after rendering this character.
	int advance = 0;
	/// VaCuus patch #15 (VENDORED_TAG.txt): the same advance in fractional pixels, unrounded. The default font
	/// engine moves its pen by this one, so a run of glyphs does not collect a rounding error per glyph.
	float advance_exact = 0.f;

	/// Bitmap data defining this glyph. The dimensions and format of the data is given below. This will be nullptr if the glyph has no bitmap data.
	const byte* bitmap_data = nullptr;

	Vector2i bitmap_dimensions;
	ColorFormat color_format = ColorFormat::A8;

	// bitmap_data may point to this member or another font glyph data.
	UniquePtr<byte[]> bitmap_owned_data;

	// Create a copy with its bitmap data owned by another glyph.
	FontGlyph WeakCopy() const
	{
		FontGlyph glyph;
		glyph.bearing = bearing;
		glyph.advance = advance;
		glyph.advance_exact = advance_exact; // VaCuus patch #15 (VENDORED_TAG.txt): fallback-font glyphs are weak copies.
		glyph.bitmap_data = bitmap_data;
		glyph.bitmap_dimensions = bitmap_dimensions;
		glyph.color_format = color_format;
		return glyph;
	}
};

// VaCuus patch #15 follow-up (VENDORED_TAG.txt): WeakCopy copies FontGlyph member by member, so a member added without a
// line there is silently dropped from every fallback-face glyph -- the loss VaCuus.Font.Subpixel.FallbackAdvance exists for.
// A brace list longer than the member count is ill-formed, so this stops compiling when FontGlyph gains a member: carry
// the member in WeakCopy, then update the count.
namespace FontGlyphMemberCount {
	struct AnyMember {
		template <typename T>
		operator T() const;
	};
	template <typename G>
	concept TakesSeven = requires { G{AnyMember{}, AnyMember{}, AnyMember{}, AnyMember{}, AnyMember{}, AnyMember{}, AnyMember{}}; };
	template <typename G>
	concept TakesEight = requires { G{AnyMember{}, AnyMember{}, AnyMember{}, AnyMember{}, AnyMember{}, AnyMember{}, AnyMember{}, AnyMember{}}; };
	static_assert(TakesSeven<FontGlyph> && !TakesEight<FontGlyph>, "FontGlyph's member count changed: carry the member in WeakCopy, then update this.");
} // namespace FontGlyphMemberCount

using FontGlyphMap = UnorderedMap<Character, FontGlyph>;

} // namespace Rml
