// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuus.h"

#include "VaCuusDefines.h"
#include "VaCuusEngine.h"
#include "VaCuusUIThread.h"

#include "HAL/PlatformProcess.h"

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
	// Ordered teardown, and the reason ForceShutdownAll() below is only ever a
	// diagnostic now: the thread we own is the one that booted RmlUi, so stopping
	// it runs the paired Shutdown() on the owner thread while VaCuusRml is still
	// loaded and every document host is still alive.
	StopUIThread();

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
		// on purpose: the thread that took the stray reference may already be gone,
		// and this path is an error report either way.
		Engine->ForceShutdownAll();
	}

	Engine.Reset();

	UE_LOG(LogVaCuus, Log, TEXT("VaCuus runtime module shut down"));
}

FVaCuusUIThread* FVaCuusModule::GetOrStartUIThread()
{
	check(IsInGameThread());

	if (UIThread.IsValid())
	{
		return UIThread.Get();
	}

	check(Engine.IsValid());
	TUniquePtr<FVaCuusUIThread> NewThread = MakeUnique<FVaCuusUIThread>(*Engine);
	if (NewThread->Start())
	{
		UIThread = MoveTemp(NewThread);
		return UIThread.Get();
	}

	// Start() fails for two very different reasons. No multithreading is a
	// configuration (commandlets, -nothreading) and the frames simply move onto the
	// game thread; anything else -- RmlUi already owned elsewhere, a boot failure --
	// is not something an inline retry would fix.
	if (!FPlatformProcess::SupportsMultithreading() && NewThread->StartInline())
	{
		UIThread = MoveTemp(NewThread);
		return UIThread.Get();
	}

	UE_LOG(LogVaCuus, Error, TEXT("The VaCuus UI thread is unavailable; no views can be created"));
	return nullptr;
}

void FVaCuusModule::StopUIThread()
{
	if (!UIThread.IsValid())
	{
		return;
	}

	check(IsInGameThread());

	// The destructor requests the stop, joins, and the worker's Exit() shuts every
	// view and RmlUi itself down — all on the UI thread, which is the only thread
	// allowed to. In inline mode it runs that same teardown right here.
	UIThread.Reset();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVaCuusModule, VaCuus)
