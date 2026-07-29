// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

class FVaCuusModule : public IModuleInterface
{
public:
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
};
