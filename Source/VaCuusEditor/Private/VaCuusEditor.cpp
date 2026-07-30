// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusEditor.h"

#include "VaCuusDefines.h"
#include "VaCuusLiveReload.h"

#define LOCTEXT_NAMESPACE "FVaCuusEditorModule"

// Out of line, both of them: the header only forward-declares FVaCuusLiveReload, so
// TUniquePtr's destructor needs the complete type here and nowhere else.
FVaCuusEditorModule::FVaCuusEditorModule() = default;
FVaCuusEditorModule::~FVaCuusEditorModule() = default;

void FVaCuusEditorModule::StartupModule()
{
	// PostEngineInit (VaCuus.uplugin), so the plugin manager is populated and the DevUI
	// roots resolve -- and the editor engine that will tick the watcher exists.
	LiveReload = MakeUnique<FVaCuusLiveReload>();
	LiveReload->Start();

	UE_LOG(LogVaCuus, Log, TEXT("VaCuus editor module started"));
}

void FVaCuusEditorModule::ShutdownModule()
{
	// Explicit rather than left to the destructor below: the watches must be handed back
	// while the DirectoryWatcher module is still loaded.
	if (LiveReload.IsValid())
	{
		LiveReload->Shutdown();
		LiveReload.Reset();
	}

	UE_LOG(LogVaCuus, Log, TEXT("VaCuus editor module shut down"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVaCuusEditorModule, VaCuusEditor)
