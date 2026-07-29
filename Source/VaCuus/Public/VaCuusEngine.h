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
 *
 * THREAD AFFINITY -- the owner-thread contract:
 * The instance is reachable from any thread (Get() only touches the module
 * manager) and IsInitialized() may be read from anywhere, but the *mutating*
 * calls are single-threaded by contract. The thread that takes the reference
 * count from 0 to 1 becomes the owner and is recorded; every later
 * Initialize()/Shutdown()/SetRenderInterface() check()s that it is on that same
 * thread, and the record is cleared when the count returns to 0.
 *
 * In M2 the owner is normally the VaCuus UI thread, because
 * FVaCuusUIThread::Init() is what boots the library. Automation tests are the
 * other legitimate owner: they initialize on the test thread and own RmlUi for
 * their duration. Anything else -- a stray Initialize() from the game thread
 * while the UI thread holds a reference -- is a bug that would corrupt RmlUi's
 * global state silently, so it fails loudly instead.
 */
class VACUUS_API FVaCuusEngine
{
public:
	/** Constructed and destroyed by FVaCuusModule; use Get() to reach the instance. */
	FVaCuusEngine();
	~FVaCuusEngine();

	FVaCuusEngine(const FVaCuusEngine&) = delete;
	FVaCuusEngine& operator=(const FVaCuusEngine&) = delete;

	/** Safe from any thread; the calls below are not (see the owner-thread contract). */
	static FVaCuusEngine& Get();

	/**
	 * Boots RmlUi if needed and adds a reference. Returns true if the library is up.
	 * The first caller becomes the owner thread; later callers must be on it.
	 */
	bool Initialize();

	/** Releases a reference; tears RmlUi down when the last one is gone. Owner thread. */
	void Shutdown();

	/** True while the library is booted. Safe from any thread. */
	bool IsInitialized() const { return RefCount > 0; }

	/**
	 * Overrides the render interface used at boot. Must be called before the
	 * first Initialize(); ignored (with an error) while the library is up.
	 * When never called, a null render stub is installed for headless use.
	 * Owner thread, when there is one.
	 */
	void SetRenderInterface(Rml::RenderInterface* InRenderInterface);

	/**
	 * Last-resort teardown for FVaCuusModule::ShutdownModule(): balances every
	 * outstanding reference regardless of which thread owns them, because leaving
	 * RmlUi up past module unload is worse than tearing it down from the wrong
	 * thread. Always an error path -- the caller logs it. Never call this to
	 * balance your own Initialize().
	 */
	void ForceShutdownAll();

private:
	/** Asserts the owner-thread contract; no-op while nobody owns the library. */
	void CheckOwnerThread(const TCHAR* Operation) const;

	/** Rml::Shutdown() + interface release + owner reset; assumes RefCount already hit 0. */
	void TearDownLibrary();

	/** Number of live Initialize() references. */
	int32 RefCount = 0;

	/** Thread that took RefCount 0 -> 1, or 0 while the library is down. */
	uint32 OwnerThreadId = 0;

	/** Render interface handed to Rml::SetRenderInterface (not owned unless it's the null stub). */
	Rml::RenderInterface* RenderInterface = nullptr;

	TUniquePtr<FVaCuusSystemInterface> SystemInterface;
	TUniquePtr<FVaCuusFileInterface> FileInterface;
	TUniquePtr<FVaCuusNullRenderInterface> NullRenderInterface;
};
