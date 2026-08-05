#include "ElementBackgroundBorder.h"
#include "../../Include/RmlUi/Core/Box.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Context.h"
#include "../../Include/RmlUi/Core/DecorationTypes.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/MeshUtilities.h"
#include "../../Include/RmlUi/Core/Profiling.h"
#include "../../Include/RmlUi/Core/RenderManager.h"
#include "../../Include/RmlUi/Core/Texture.h" // VaCuus patch #3: Texture::GetDimensions, the box-shadow capture probe in GenerateGeometry
#include "BoxShadowCache.h"
#include "GeometryBoxShadow.h"

namespace Rml {

ElementBackgroundBorder::ElementBackgroundBorder() {}

void ElementBackgroundBorder::Render(Element* element)
{
	if (background_dirty || border_dirty)
	{
		for (auto& background : backgrounds)
		{
			if (background.first != BackgroundType::BackgroundBorder)
				background.second.geometry.Release();
		}

		GenerateGeometry(element);

		background_dirty = false;
		border_dirty = false;
	}

	if (Background* shadow = GetBackground(BackgroundType::BoxShadowAndBackgroundBorder))
	{
		const Vector2f offset = element->GetAbsoluteOffset(BoxArea::Border);
		shadow->box_shadow_and_background_border->geometry.Render(offset, shadow->box_shadow_and_background_border->texture);
	}
	else if (Background* background = GetBackground(BackgroundType::BackgroundBorder))
	{
		const Vector2f offset = element->GetAbsoluteOffset(BoxArea::Border);
		background->geometry.Render(offset);
	}
}

void ElementBackgroundBorder::DirtyBackground()
{
	background_dirty = true;
}

void ElementBackgroundBorder::DirtyBorder()
{
	border_dirty = true;
}

Geometry* ElementBackgroundBorder::GetClipGeometry(Element* element, BoxArea clip_area)
{
	BackgroundType type = {};
	switch (clip_area)
	{
	case Rml::BoxArea::Border: type = BackgroundType::ClipBorder; break;
	case Rml::BoxArea::Padding: type = BackgroundType::ClipPadding; break;
	case Rml::BoxArea::Content: type = BackgroundType::ClipContent; break;
	default: RMLUI_ERROR; return nullptr;
	}

	RenderManager* render_manager = element->GetRenderManager();
	Geometry& geometry = GetOrCreateBackground(type).geometry;
	if (render_manager && !geometry)
	{
		Mesh mesh = geometry.Release(Geometry::ReleaseMode::ClearMesh);
		MeshUtilities::GenerateBackground(mesh, element->GetRenderBox(clip_area), ColourbPremultiplied(255));
		geometry = render_manager->MakeGeometry(std::move(mesh));
	}

	return &geometry;
}

ElementBackgroundBorder::Background* ElementBackgroundBorder::GetBackground(BackgroundType type)
{
	auto it = backgrounds.find(type);
	if (it != backgrounds.end())
		return &it->second;
	return nullptr;
}

ElementBackgroundBorder::Background& ElementBackgroundBorder::GetOrCreateBackground(BackgroundType type)
{
	auto it = backgrounds.find(type);
	if (it != backgrounds.end())
		return it->second;

	Background& background = backgrounds[type];
	return background;
}

void ElementBackgroundBorder::EraseBackground(BackgroundType type)
{
	backgrounds.erase(type);
}

void ElementBackgroundBorder::GenerateGeometry(Element* element)
{
	RMLUI_ZoneScoped;
	RenderManager* render_manager = element->GetRenderManager();
	if (!render_manager)
		return;

	const ComputedValues& computed = element->GetComputedValues();
	const bool has_box_shadow = computed.has_box_shadow();

	// VaCuus patch #3 (VENDORED_TAG.txt): a shadow that was asked for and refused, parked below on the
	// ordinary background so the CACHE ENTRY OUTLIVES THIS CALL. That is not bookkeeping -- see the
	// refusal branch for what dropping it costs.
	SharedPtr<BoxShadowRenderable> refused_shadow;

	if (has_box_shadow)
	{
		SharedPtr<BoxShadowRenderable> shadow_renderable = BoxShadowCache::GetHandle(element, computed);

		// VaCuus patch #3 (VENDORED_TAG.txt). Upstream commits to the shadow path here unconditionally.
		// When the render interface leaves SaveLayerAsTexture at its optional default the shadow texture
		// can never be produced, and committing anyway is DESTRUCTIVE rather than merely degraded: the
		// normal background and border are erased on the line below and never generated, while
		// Render() draws BoxShadowRenderable::geometry -- a premultiplied WHITE quad covering the border
		// box extended by 1.5*blur+spread (BoxShadowCache.cpp:56-59) -- with the null texture handle. The
		// element's own background and border are replaced by an opaque white rectangle.
		//
		// Reading the dimensions is what forces the texture callback (Texture::GetDimensions ->
		// CallbackTextureDatabase::EnsureLoaded, TextureDatabase.cpp:36-39), so this probe is the callback's
		// first and, with the GeometryBoxShadow.cpp hunk latching load_failed, ONLY run for this cache entry.
		// It costs nothing on a backend that implements capture: the callback has to run this frame anyway,
		// two lines later at Render(), and it restores the render state it borrows (GeometryBoxShadow.cpp:238).
		//
		// Falling THROUGH rather than rendering BoxShadowRenderable::background_border_geometry is deliberate.
		// That cached mesh looks like the right substitute but is not: the cache key bakes opacity into the
		// shadow quad's vertex alpha instead of the background colour (BoxShadowCache.cpp:57-58, whose
		// GetHandle passes ToPremultiplied() with NO opacity at :85-91), so at opacity < 1 it renders fully
		// opaque. The normal path below premultiplies by opacity (:131-133) and is exactly right.
		const Texture shadow_texture = shadow_renderable ? Texture(shadow_renderable->texture) : Texture();
		if (shadow_texture.GetDimensions().x > 0)
		{
			// The box shadow geometry also includes the element's background and border, thus we can skip the normal background generation.
			EraseBackground(BackgroundType::BackgroundBorder);
			Background& shadow_background = GetOrCreateBackground(BackgroundType::BoxShadowAndBackgroundBorder);
			shadow_background.box_shadow_and_background_border = std::move(shadow_renderable);
			return;
		}

		// KEEPING THE REFUSED RENDERABLE ALIVE IS WHAT MAKES THE LATCH WORK, and dropping it here
		// was a real bug in the first version of this patch -- caught by
		// VaCuus.Render.LayerCapture.RestyleChurn, which reported one refusal PER RESTYLED FRAME.
		// BoxShadowCache holds its entries by WeakPtr and erases the entry from the renderable's
		// destructor (BoxShadowCache.cpp:13, :65-77), so letting this SharedPtr die at the end of
		// the scope destroys the cache entry -- and with it the CallbackTexture whose load_failed
		// flag the GeometryBoxShadow.cpp hunk just set. The next dirty of the background would then
		// mint a fresh entry and run the whole callback again: a layer push, a blur compiled and
		// released, geometry made and released, per restyle. Parked on the ordinary background below,
		// the entry lives exactly as long as the element still asks for this shadow, and
		// EnsureLoaded short-circuits on load_failed forever after (TextureDatabase.cpp:50).
		refused_shadow = std::move(shadow_renderable);
	}

	EraseBackground(BackgroundType::BoxShadowAndBackgroundBorder);

	const float opacity = computed.opacity();
	ColourbPremultiplied background_color = computed.background_color().ToPremultiplied(opacity);
	Array<ColourbPremultiplied, 4> border_colors = {
		computed.border_top_color().ToPremultiplied(opacity),
		computed.border_right_color().ToPremultiplied(opacity),
		computed.border_bottom_color().ToPremultiplied(opacity),
		computed.border_left_color().ToPremultiplied(opacity),
	};

	Background& background = GetOrCreateBackground(BackgroundType::BackgroundBorder);

	// VaCuus patch #3: the refused shadow's only owner. Null in every other case, including the one
	// where the element stops carrying box-shadow at all -- which is what then frees the cache entry.
	background.box_shadow_and_background_border = std::move(refused_shadow);

	Geometry& geometry = background.geometry;
	Mesh mesh = geometry.Release(Geometry::ReleaseMode::ClearMesh);

	for (int i = 0; i < element->GetNumBoxes(); i++)
		MeshUtilities::GenerateBackgroundBorder(mesh, element->GetRenderBox(BoxArea::Padding, i), background_color, border_colors.data());

	geometry = render_manager->MakeGeometry(std::move(mesh));
}

} // namespace Rml
