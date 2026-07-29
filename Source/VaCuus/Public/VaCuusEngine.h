// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace Rml
{
class RenderInterface;
}

class FVaCuusSystemInterface;
class FVaCuusFileInterface;
class FVaCuusNullRenderInterface;

/**
 * Owns the RmlUi library lifecycle: installs the system/file/render interfaces,
 * boots the library and loads the default font face.
 *
 * Ref-counted and idempotent: every successful Initialize() must be paired with
 * a Shutdown(); the library is torn down when the last reference is released.
 */
class VACUUS_API FVaCuusEngine
{
public:
	/** Constructed and destroyed by FVaCuusModule; use Get() to reach the instance. */
	FVaCuusEngine();
	~FVaCuusEngine();

	FVaCuusEngine(const FVaCuusEngine&) = delete;
	FVaCuusEngine& operator=(const FVaCuusEngine&) = delete;

	static FVaCuusEngine& Get();

	/** Boots RmlUi if needed and adds a reference. Returns true if the library is up. */
	bool Initialize();

	/** Releases a reference; tears RmlUi down when the last one is gone. */
	void Shutdown();

	/** True while the library is booted. */
	bool IsInitialized() const { return RefCount > 0; }

	/**
	 * Overrides the render interface used at boot. Must be called before the
	 * first Initialize(); ignored (with an error) while the library is up.
	 * When never called, a null render stub is installed for headless use.
	 */
	void SetRenderInterface(Rml::RenderInterface* InRenderInterface);

private:
	/** Number of live Initialize() references. */
	int32 RefCount = 0;

	/** Render interface handed to Rml::SetRenderInterface (not owned unless it's the null stub). */
	Rml::RenderInterface* RenderInterface = nullptr;

	TUniquePtr<FVaCuusSystemInterface> SystemInterface;
	TUniquePtr<FVaCuusFileInterface> FileInterface;
	TUniquePtr<FVaCuusNullRenderInterface> NullRenderInterface;
};
