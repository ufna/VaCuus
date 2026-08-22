// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuus.h"

#include "VaCuusBundleMount.h"
#include "VaCuusContentPaths.h"
#include "VaCuusDefines.h"
#include "VaCuusEngine.h"
#include "VaCuusFontRegistry.h"
#include "VaCuusStyleSet.h"
#include "VaCuusTextureRegistry.h"
#include "VaCuusTranslation.h"
#include "VaCuusUIThread.h"

#include "HAL/PlatformProcess.h"

#define LOCTEXT_NAMESPACE "FVaCuusModule"

DEFINE_LOG_CATEGORY(LogVaCuus);

namespace
{
/**
 * How long StopUIThread() waits for the worker to drain the in-band shutdown
 * before hard-stopping it. Generous next to a UI frame (fractions of a
 * millisecond for the M1 HUD) and short enough to be invisible in engine
 * shutdown; the fallback is correct either way, just louder and less tidy.
 */
constexpr double GVaCuusGracefulShutdownSeconds = 0.1;
}	 // namespace

FVaCuusModule::~FVaCuusModule() = default;

void FVaCuusModule::StartupModule()
{
	Engine = MakeUnique<FVaCuusEngine>();

	// Primed HERE, on the game thread, and that is the only reason the call is not
	// simply left to the first document load: it resolves through
	// IPluginManager::FindPlugin(), and the first load happens on the UI thread.
	VaCuusContentPaths::GetDocumentRoots();

	UE_LOG(LogVaCuus, Log, TEXT("VaCuus runtime module started"));
}

void FVaCuusModule::ShutdownModule()
{
	// Ordered teardown, and the reason ForceShutdownAll() below is only ever a
	// diagnostic now: the thread we own is the one that booted RmlUi, so stopping
	// it runs the paired Shutdown() on the owner thread while VaCuusRml is still
	// loaded and every document host is still alive.
	StopUIThread();

	// After the thread (nothing can be recording), before the engine: drops the style
	// registry's roots and its render-thread proxy mirror while the render thread is
	// still around to flush against.
	FVaCuusStyleRegistry::Shutdown_GameThread();
	FVaCuusTextureRegistry::Shutdown_GameThread();

	// The bundle regions die HERE and nowhere earlier (spec M6 section 4's release
	// rule): StopUIThread() above joined the thread whose file interface held the
	// only open span handles, so no read can be in flight -- and a steal-backed
	// record destroyed any sooner could never be rebuilt (the steal is once-only).
	FVaCuusBundleMountTable::DestroyRecords();

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

	if (!Engine.IsValid())
	{
		// Only reachable after ShutdownModule(): the contract callers rely on is
		// "null when the thread is unavailable", not an assert on a teardown race.
		UE_LOG(LogVaCuus, Error,
			TEXT("The VaCuus UI thread cannot start: the module has no engine (already shut down?)"));
		return nullptr;
	}

	TUniquePtr<FVaCuusUIThread> NewThread = MakeUnique<FVaCuusUIThread>(*Engine, ScriptHostFactory);
	if (NewThread->Start())
	{
		UIThread = MoveTemp(NewThread);

		// The register-before-boot half of the style registry (M5 Task 5b): a style set
		// registered before the first view exists must reach the first document's
		// CompileShader. First command in, ahead of any AddView/Load the caller enqueues
		// after this returns — the queue is FIFO from this one producer. The translation
		// table (M5 Task 8) rides the identical argument one line down.
		FVaCuusStyleRegistry::PublishToUIThread(*UIThread);
		FVaCuusTextureRegistry::PublishToUIThread(*UIThread);
		FVaCuusTranslationRegistry::PublishToUIThread(*UIThread);

		// And the font faces (spec 2026-08-09 §3), by the same argument plus one of its own: a
		// RESTARTED thread comes back with a fresh RmlUi whose font provider holds only the
		// shipped Latin face, so without this replay a game's Cyrillic or CJK face is silently
		// gone and every glyph it served becomes U+FFFD.
		FVaCuusFontRegistry::PublishToUIThread(*UIThread);
		return UIThread.Get();
	}

	// Start() fails for two very different reasons. No multithreading is a
	// configuration (commandlets, -nothreading) and the frames simply move onto the
	// game thread; anything else -- RmlUi already owned elsewhere, a boot failure --
	// is not something an inline retry would fix.
	if (!FPlatformProcess::SupportsMultithreading() && NewThread->StartInline())
	{
		UIThread = MoveTemp(NewThread);
		FVaCuusStyleRegistry::PublishToUIThread(*UIThread);
		FVaCuusTextureRegistry::PublishToUIThread(*UIThread);
		FVaCuusTranslationRegistry::PublishToUIThread(*UIThread);

		// And the font faces (spec 2026-08-09 §3), by the same argument plus one of its own: a
		// RESTARTED thread comes back with a fresh RmlUi whose font provider holds only the
		// shipped Latin face, so without this replay a game's Cyrillic or CJK face is silently
		// gone and every glyph it served becomes U+FFFD.
		FVaCuusFontRegistry::PublishToUIThread(*UIThread);
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

	// Graceful first (spec §4's "stop accepting commands -> drain UI thread -> close
	// documents"): the in-band Shutdown command lets the worker close every document
	// and account for anything still queued, on its own thread, before a join can cut
	// it off. The wait is bounded — a worker wedged in a long frame must not hold up
	// module shutdown — and the hard stop below is the fallback, not the plan.
	if (!UIThread->RequestGracefulShutdown(GVaCuusGracefulShutdownSeconds))
	{
		UE_LOG(LogVaCuus, Warning,
			TEXT("The VaCuus UI thread did not process the in-band shutdown within %.0f ms; falling back to a hard stop"),
			GVaCuusGracefulShutdownSeconds * 1000.0);
	}

	// The destructor requests the stop, joins, and the worker's Exit() shuts every
	// view and RmlUi itself down — all on the UI thread, which is the only thread
	// allowed to. In inline mode it runs that same teardown right here.
	UIThread.Reset();
}

void FVaCuusModule::SetScriptHostFactory(FVaCuusScriptHostFactory InFactory)
{
	check(IsInGameThread());

	// The FVaCuusEngine::SetRenderInterface enforcement shape (VaCuusEngine.cpp:230-242):
	// the UI thread snapshots the factory when it is CONSTRUCTED and creates the host in
	// Init(), so a factory set while a thread exists could never affect that thread --
	// accepting it would only arm a surprise for the next boot. UIThread validity is the
	// whole gate: StopUIThread() resets the pointer, after which the factory may change
	// again (test stubs, module unload) and the next GetOrStartUIThread() picks it up.
	if (UIThread.IsValid())
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("SetScriptHostFactory() must be called before the UI thread starts; ignored"));
		return;
	}

	ScriptHostFactory = MoveTemp(InFactory);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVaCuusModule, VaCuus)
