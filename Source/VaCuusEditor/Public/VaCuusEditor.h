// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"
#include "Templates/UniquePtr.h"

class FVaCuusLiveReload;

class FVaCuusEditorModule : public IModuleInterface
{
public:
	FVaCuusEditorModule();
	virtual ~FVaCuusEditorModule() override;

	//~ Begin IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	//~ End IModuleInterface

	/** Loads the module if it isn't loaded yet */
	static FVaCuusEditorModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FVaCuusEditorModule>("VaCuusEditor");
	}

	static bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("VaCuusEditor");
	}

private:
	/**
	 * The DevUI file watcher; null before StartupModule() and after ShutdownModule().
	 *
	 * OWNED BY THE MODULE rather than by a function-local static so it is destroyed while
	 * the DirectoryWatcher module is still loaded -- the same reason FVaCuusModule owns the
	 * RmlUi engine instead of leaving it to static destruction. Deliberately not exposed:
	 * nothing outside needs the object, and vacuus.ReloadUI reaches the dispatch through
	 * FVaCuusLiveReload::ReloadAllLiveViews(), which is static because it holds no state.
	 */
	TUniquePtr<FVaCuusLiveReload> LiveReload;
};
