// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuus.h"

#include "VaCuusDefines.h"
#include "VaCuusEngine.h"

#define LOCTEXT_NAMESPACE "FVaCuusModule"

DEFINE_LOG_CATEGORY(LogVaCuus);

FVaCuusModule::~FVaCuusModule() = default;

void FVaCuusModule::StartupModule()
{
	Engine = MakeUnique<FVaCuusEngine>();

	UE_LOG(LogVaCuus, Log, TEXT("VaCuus runtime module started"));
}

void FVaCuusModule::ShutdownModule()
{
	// The engine dies here rather than at static destruction time: by then
	// FModuleManager has already unloaded VaCuusRml, and any RmlUi call made
	// from a late destructor would land in an unloaded module.
	//
	// Note this runs *before* VaCuusRender::ShutdownModule() (reverse load
	// order — VaCuusRender is PostConfigInit), so a live RmlUi user may still
	// be out there; force the teardown instead of leaking the library.
	if (Engine.IsValid() && Engine->IsInitialized())
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("VaCuus module is shutting down while RmlUi is still initialized: an Initialize() went unpaired. Forcing teardown"));

		// Balance every outstanding reference so Rml::Shutdown() actually runs and
		// the interfaces are released while this module still exists. Owner-agnostic
		// on purpose: the thread that booted RmlUi (in M2 usually the UI thread) may
		// already be gone, and this path is an error report either way.
		Engine->ForceShutdownAll();
	}

	Engine.Reset();

	UE_LOG(LogVaCuus, Log, TEXT("VaCuus runtime module shut down"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVaCuusModule, VaCuus)
