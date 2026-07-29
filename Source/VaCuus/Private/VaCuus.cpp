// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuus.h"

#include "VaCuusDefines.h"

#define LOCTEXT_NAMESPACE "FVaCuusModule"

DEFINE_LOG_CATEGORY(LogVaCuus);

void FVaCuusModule::StartupModule()
{
	UE_LOG(LogVaCuus, Log, TEXT("VaCuus runtime module started"));
}

void FVaCuusModule::ShutdownModule()
{
	UE_LOG(LogVaCuus, Log, TEXT("VaCuus runtime module shut down"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVaCuusModule, VaCuus)
