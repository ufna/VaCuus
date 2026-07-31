// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusJs.h"

DEFINE_LOG_CATEGORY(LogVaCuusJS);

void FVaCuusJsModule::StartupModule()
{
	// Nothing to boot yet: the JSRuntime is created lazily on the UI thread by
	// the script host (M4 Task 2), never at module load.
	UE_LOG(LogVaCuusJS, Log, TEXT("VaCuusJs module started (quickjs-ng %hs vendored, see Source/ThirdParty/quickjs-ng/VENDORED_TAG.txt)"),
		"v0.15.1");
}

void FVaCuusJsModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FVaCuusJsModule, VaCuusJs)
