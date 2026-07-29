// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

class FVaCuusRenderModule : public IModuleInterface
{
public:
	//~ Begin IModuleInterface
	virtual void StartupModule() override
	{
		// Map the plugin shader directory before global shader compilation kicks
		// in — the module loads at PostConfigInit for exactly this reason.
		const FString ShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("VaCuus"))->GetBaseDir(), TEXT("Shaders"));
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/VaCuus"), ShaderDir);
	}
	//~ End IModuleInterface
};

IMPLEMENT_MODULE(FVaCuusRenderModule, VaCuusRender)
