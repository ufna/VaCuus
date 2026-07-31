// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaCuusScriptHost.h"

#include "Modules/ModuleManager.h"
#include "Templates/UniquePtr.h"

class FVaCuusEngine;
class FVaCuusUIThread;

/**
 * The VaCuus runtime module. Owns the two things whose lifetime must bracket
 * everything else: the RmlUi library wrapper (FVaCuusEngine) and the
 * process-wide UI thread that is the library's only legal caller.
 *
 * WHY BOTH LIVE HERE: RmlUi's interfaces, its `initialised` flag and its context
 * registry are process-global statics, so there can be exactly one library
 * instance and exactly one thread owning it. Putting the thread next to the
 * engine is what makes teardown ordered -- ShutdownModule() stops the thread
 * (whose Exit() pairs the engine's Initialize()) and only then destroys the
 * engine, so FVaCuusEngine::ForceShutdownAll() is a diagnostic rather than the
 * expected path.
 */
class FVaCuusModule : public IModuleInterface
{
public:
	/** Out of line: FVaCuusEngine and FVaCuusUIThread are only forward-declared here. */
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

	/** The loaded module, or null. Use this on teardown paths, where Get() would reload it. */
	static FVaCuusModule* GetPtr()
	{
		return FModuleManager::GetModulePtr<FVaCuusModule>("VaCuus");
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

	/**
	 * The process-wide UI thread, started on first use. Game thread only.
	 *
	 * Returns null when the thread could not be brought up at all: RmlUi is owned
	 * by another thread (an automation test), or there is no multithreading AND the
	 * inline fallback failed to boot too. Callers must handle null by not creating
	 * the view.
	 */
	VACUUS_API FVaCuusUIThread* GetOrStartUIThread();

	/** The UI thread if one is running, without starting one. Safe from any thread. */
	FVaCuusUIThread* GetUIThread() const { return UIThread.Get(); }

	/** Stops and joins the UI thread (its Exit() tears RmlUi down). Game thread only. */
	VACUUS_API void StopUIThread();

	/**
	 * Registers (or, with nullptr, clears) the factory the next UI thread boot uses
	 * to build its script host (M4). FVaCuusJsModule::StartupModule is the one
	 * production caller; the factory runs ON the UI thread, in Init().
	 *
	 * Must be called while no UI thread exists; refused (with an error) otherwise --
	 * the FVaCuusEngine::SetRenderInterface shape, and for the same reason: the
	 * consumer snapshots it at boot, so a later change would silently apply to a
	 * FUTURE boot while looking like it applied to this one. Game thread only, like
	 * every other mutator of UIThread.
	 */
	VACUUS_API void SetScriptHostFactory(FVaCuusScriptHostFactory InFactory);

private:
	/**
	 * Owned by the module rather than by a function-local static so it dies in
	 * ShutdownModule() — before C++ static destruction, and before VaCuusRml is
	 * unloaded in a modular build.
	 */
	TUniquePtr<FVaCuusEngine> Engine;

	/**
	 * One per process (see the class comment). Created lazily by the first view
	 * request rather than in StartupModule(): a session that never shows UI should
	 * not pay for a thread or for RmlUi's boot.
	 */
	TUniquePtr<FVaCuusUIThread> UIThread;

	/**
	 * Handed to each UI thread at construction and snapshotted there: the thread
	 * cannot read it from here itself, because its Init() runs off the game thread
	 * and FModuleManager refuses module lookups there (see FVaCuusUIThread's
	 * constructor comment on why Engine is a reference).
	 */
	FVaCuusScriptHostFactory ScriptHostFactory;
};
