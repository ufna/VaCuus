// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"
#include "Templates/UniquePtr.h"

class FVaCuusEngine;

class FVaCuusModule : public IModuleInterface
{
public:
	/** Out of line: FVaCuusEngine is only forward-declared here. */
	virtual ~FVaCuusModule();

	//~ Begin IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	//~ End IModuleInterface

	/** Loads the module if it isn't loaded yet */
	static FVaCuusModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FVaCuusModule>("VaCuus");
	}

	static bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("VaCuus");
	}

	/** The module-owned engine; alive between StartupModule() and ShutdownModule(). Prefer FVaCuusEngine::Get(). */
	FVaCuusEngine& GetEngine() const
	{
		check(Engine.IsValid());
		return *Engine;
	}

private:
	/**
	 * Owned by the module rather than by a function-local static so it dies in
	 * ShutdownModule() — before C++ static destruction, and before VaCuusRml is
	 * unloaded in a modular build.
	 */
	TUniquePtr<FVaCuusEngine> Engine;
};
