// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusEditor.h"

#include "VaCuusDefines.h"

#define LOCTEXT_NAMESPACE "FVaCuusEditorModule"

void FVaCuusEditorModule::StartupModule()
{
	UE_LOG(LogVaCuus, Log, TEXT("VaCuus editor module started"));
}

void FVaCuusEditorModule::ShutdownModule()
{
	UE_LOG(LogVaCuus, Log, TEXT("VaCuus editor module shut down"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVaCuusEditorModule, VaCuusEditor)
