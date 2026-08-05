#include "../../Include/RmlUi/Core/CallbackTexture.h"
#include "../../Include/RmlUi/Core/Texture.h"
#include "RenderManagerAccess.h"

namespace Rml {

void CallbackTexture::Release()
{
	if (resource_handle != StableVectorIndex::Invalid)
	{
		RenderManagerAccess::ReleaseResource(render_manager, *this);
		Clear();
	}
}

Rml::CallbackTexture::operator Texture() const
{
	return Texture(render_manager, resource_handle);
}

CallbackTextureInterface::CallbackTextureInterface(RenderManager& render_manager, RenderInterface& render_interface, TextureHandle& texture_handle,
	Vector2i& dimensions) : render_manager(render_manager), render_interface(render_interface), texture_handle(texture_handle), dimensions(dimensions)
{}

bool CallbackTextureInterface::GenerateTexture(Span<const byte> source, Vector2i new_dimensions) const
{
	if (texture_handle)
	{
		RMLUI_ERRORMSG("Texture already set");
		return false;
	}
	texture_handle = render_interface.GenerateTexture(source, new_dimensions);
	if (texture_handle)
		dimensions = new_dimensions;
	return texture_handle != TextureHandle{};
}

// VaCuus patch #3 (VENDORED_TAG.txt): returns bool; see CallbackTexture.h for why. Every early-out already
// left texture_handle at zero, so the only change in behaviour is that the caller can now SEE that.
bool CallbackTextureInterface::SaveLayerAsTexture() const
{
	if (texture_handle)
	{
		RMLUI_ERRORMSG("Texture already set");
		return false;
	}

	const Rectanglei region = render_manager.GetScissorRegion();
	if (!region.Valid())
	{
		RMLUI_ERRORMSG("Save layer as texture requires a scissor region to be set first");
		return false;
	}

	// A render interface that does not implement layer capture returns the base class's zero
	// (RenderInterface.cpp:37-40) -- an optional virtual, so this is a supported configuration
	// rather than an error, and it is the one the bool return exists to report.
	texture_handle = render_interface.SaveLayerAsTexture();
	if (!texture_handle)
		return false;

	dimensions = region.Size();
	return true;
}

void CallbackTextureInterface::SetTextureHandle(TextureHandle handle, Vector2i new_dimensions) const
{
	if (texture_handle)
	{
		RMLUI_ERRORMSG("Texture already set");
		return;
	}

	texture_handle = handle;
	dimensions = new_dimensions;
}

RenderManager& CallbackTextureInterface::GetRenderManager() const
{
	return render_manager;
}

CallbackTextureSource::CallbackTextureSource(CallbackTextureFunction&& callback) : callback(std::move(callback)) {}

CallbackTextureSource::CallbackTextureSource(CallbackTextureSource&& other) noexcept :
	callback(std::move(other.callback)), textures(std::move(other.textures))
{}

CallbackTextureSource& CallbackTextureSource::operator=(CallbackTextureSource&& other) noexcept
{
	callback = std::move(other.callback);
	textures = std::move(other.textures);
	return *this;
}

Texture CallbackTextureSource::GetTexture(RenderManager& render_manager) const
{
	CallbackTexture& texture = textures[&render_manager];
	if (!texture)
		texture = render_manager.MakeCallbackTexture(callback);
	return Texture(texture);
}

} // namespace Rml
