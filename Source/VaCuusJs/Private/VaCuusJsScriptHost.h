// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusScriptHost.h"

#include "Templates/UniquePtr.h"

class FVaCuusJsRuntime;

/**
 * The IVaCuusScriptHost implementation (M4): the UI thread's factory-built
 * gateway to quickjs. Created in FVaCuusUIThread::Init() via the factory
 * FVaCuusJsModule::StartupModule registers, destroyed at the top of Exit() --
 * see the interface for the full timing contract.
 *
 * TASK 2 SHAPE: this host owns the process's FVaCuusJsRuntime and the frame
 * hooks that reach it; per-view JSContexts, timers/rAF/the job drain
 * (FVaCuusJsViewContext) are M4 Task 3, documents Task 6. The runtime is
 * created LAZILY, at the first call that actually needs JS (spec 2(e)) -- until
 * a view runs a script, a JS-enabled build pays one virtual call per frame
 * phase and allocates nothing.
 */
class FVaCuusJsScriptHost final : public IVaCuusScriptHost
{
public:
	FVaCuusJsScriptHost();
	virtual ~FVaCuusJsScriptHost() override;

	//~ Begin IVaCuusScriptHost
	virtual void OnViewAdded(uint32 ViewId) override;
	virtual void OnViewRemoved(uint32 ViewId) override;
	virtual void OnDocumentReady(uint32 ViewId, Rml::ElementDocument* Document) override;
	virtual void OnDocumentClosing(uint32 ViewId) override;
	virtual void PumpFrame(double NowSeconds) override;
	virtual void CollectGarbage(const TCHAR* Reason) override;
	virtual void ExecuteScript(uint32 ViewId, const FString& Source, const FString& SourceName) override;
	virtual void OnInlineFrameEntry() override;
	virtual void Shutdown() override;
	//~ End IVaCuusScriptHost

private:
	/**
	 * The process's one runtime (spec 2(e)), lazy: null until the first script
	 * needs it. Task 2 wires no such caller, so in this task's production
	 * configuration it stays null for the host's whole life and every frame hook
	 * below is a null-check.
	 */
	TUniquePtr<FVaCuusJsRuntime> Runtime;
};
