// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"
#include "Templates/UniquePtr.h"

class FVaCuusLiveReload;
class FVaCuusStructRecompileGuard;

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
	 * nothing outside needs the object. (vacuus.ReloadUI is registered by the RUNTIME module
	 * these days -- VaCuusSubsystem.cpp, bead VaCuus-akj.6.18 -- not here.)
	 */
	TUniquePtr<FVaCuusLiveReload> LiveReload;

	/**
	 * The Blueprint-struct-recompile listener (VaCuus-akj.16). OWNING IT IS BEING
	 * SUBSCRIBED: INotifyOnStructChanged registers itself in its constructor and
	 * unregisters in its destructor (ListenerManager.h:25-32), so this pointer's lifetime
	 * IS the subscription -- created in StartupModule, reset in ShutdownModule while the
	 * UnrealEd manager it must unregister from is still up.
	 */
	TUniquePtr<FVaCuusStructRecompileGuard> StructRecompileGuard;
};
