// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusEngine.h"

#include "VaCuus.h"
#include "VaCuusDefines.h"
#include "VaCuusFileInterface.h"
#include "VaCuusSystemInterface.h"

#include "Misc/Paths.h"

#include <RmlUi/Core.h>

/**
 * Render interface stub that satisfies RmlUi without touching any RHI.
 * Hands out dummy non-zero handles so layout and document loading work
 * headlessly (tests, commandlets). Replaced by the real RHI backend later.
 */
class FVaCuusNullRenderInterface : public Rml::RenderInterface
{
public:
	//~ Begin Rml::RenderInterface
	virtual Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> Vertices, Rml::Span<const int> Indices) override
	{
		return Rml::CompiledGeometryHandle(NextHandle++);
	}

	virtual void RenderGeometry(Rml::CompiledGeometryHandle Geometry, Rml::Vector2f Translation, Rml::TextureHandle Texture) override {}
	virtual void ReleaseGeometry(Rml::CompiledGeometryHandle Geometry) override {}

	virtual Rml::TextureHandle LoadTexture(Rml::Vector2i& TextureDimensions, const Rml::String& Source) override
	{
		TextureDimensions = Rml::Vector2i(1, 1);
		return Rml::TextureHandle(NextHandle++);
	}

	virtual Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> Source, Rml::Vector2i SourceDimensions) override
	{
		return Rml::TextureHandle(NextHandle++);
	}

	virtual void ReleaseTexture(Rml::TextureHandle Texture) override {}

	virtual void EnableScissorRegion(bool bEnable) override {}
	virtual void SetScissorRegion(Rml::Rectanglei Region) override {}
	//~ End Rml::RenderInterface

private:
	uintptr_t NextHandle = 1;
};

FVaCuusEngine::FVaCuusEngine() = default;
FVaCuusEngine::~FVaCuusEngine() = default;

FVaCuusEngine& FVaCuusEngine::Get()
{
	// Module-owned: torn down in FVaCuusModule::ShutdownModule(), i.e. before
	// static destruction and before VaCuusRml unloads. Loading on demand keeps
	// early callers (automation tests, other modules) working.
	return FVaCuusModule::Get().GetEngine();
}

bool FVaCuusEngine::Initialize()
{
	if (RefCount > 0)
	{
		++RefCount;
		return true;
	}

	SystemInterface = MakeUnique<FVaCuusSystemInterface>();
	FileInterface = MakeUnique<FVaCuusFileInterface>();

	if (RenderInterface == nullptr)
	{
		NullRenderInterface = MakeUnique<FVaCuusNullRenderInterface>();
		RenderInterface = NullRenderInterface.Get();
	}

	Rml::SetSystemInterface(SystemInterface.Get());
	Rml::SetFileInterface(FileInterface.Get());
	Rml::SetRenderInterface(RenderInterface);

	if (!Rml::Initialise())
	{
		UE_LOG(LogVaCuus, Error, TEXT("Rml::Initialise() failed"));

		// RmlUi caches these as raw static pointers and only clears them in
		// Rml::Shutdown(); null them out before destroying the interfaces so
		// nothing is left dangling after this failure path.
		Rml::SetSystemInterface(nullptr);
		Rml::SetFileInterface(nullptr);
		Rml::SetRenderInterface(nullptr);

		// Drop only our stub; an externally provided interface is kept for a later retry.
		if (RenderInterface == NullRenderInterface.Get())
		{
			RenderInterface = nullptr;
		}
		NullRenderInterface.Reset();
		SystemInterface.Reset();
		FileInterface.Reset();
		return false;
	}

	// Default font face; missing font is a warning, not a boot failure.
	const FString FontDiskPath = FPaths::ProjectContentDir() / TEXT("DevUI/fonts/LatoLatin-Regular.ttf");
	if (FPaths::FileExists(FontDiskPath))
	{
		// Relative path: resolved by FVaCuusFileInterface against <Project>/Content/DevUI/.
		if (!Rml::LoadFontFace("fonts/LatoLatin-Regular.ttf"))
		{
			UE_LOG(LogVaCuus, Warning, TEXT("Failed to load default font face from '%s'"), *FontDiskPath);
		}
	}
	else
	{
		UE_LOG(LogVaCuus, Warning, TEXT("Default font not found at '%s'; text will not be rendered"), *FontDiskPath);
	}

	RefCount = 1;
	UE_LOG(LogVaCuus, Log, TEXT("RmlUi initialized"));
	return true;
}

void FVaCuusEngine::Shutdown()
{
	if (RefCount == 0)
	{
		UE_LOG(LogVaCuus, Warning, TEXT("Shutdown() called without a matching Initialize()"));
		return;
	}

	if (--RefCount > 0)
	{
		return;
	}

	Rml::Shutdown();

	// Interfaces must outlive Rml::Shutdown(); safe to drop them now.
	if (RenderInterface == NullRenderInterface.Get())
	{
		RenderInterface = nullptr;
	}
	NullRenderInterface.Reset();
	SystemInterface.Reset();
	FileInterface.Reset();

	UE_LOG(LogVaCuus, Log, TEXT("RmlUi shut down"));
}

void FVaCuusEngine::SetRenderInterface(Rml::RenderInterface* InRenderInterface)
{
	if (RefCount > 0)
	{
		UE_LOG(LogVaCuus, Error, TEXT("SetRenderInterface() must be called before Initialize(); ignored"));
		return;
	}

	RenderInterface = InRenderInterface;
	NullRenderInterface.Reset();
}
