// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Logging/LogMacros.h"
#include "Modules/ModuleManager.h"

VACUUSJS_API DECLARE_LOG_CATEGORY_EXTERN(LogVaCuusJS, Log, All);

/**
 * The VaCuus JavaScript module: vendored quickjs-ng (Source/ThirdParty/quickjs-ng,
 * pinned + patched per VENDORED_TAG.txt) behind the M4 script-host seam.
 *
 * quickjs never crosses this module's boundary: the vendored headers sit on the
 * PRIVATE include path and patch #1 strips JS_* symbol visibility, so consumers
 * reach scripting only through IVaCuusScriptHost (declared in the VaCuus core,
 * implemented here from M4 Task 2 on). Task 1 is the skeleton -- the vendored
 * library, this module shell, and the vendoring smoke test.
 */
class FVaCuusJsModule : public IModuleInterface
{
public:
	//~ Begin IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	//~ End IModuleInterface
};
