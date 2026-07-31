// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusJs.h"

#include "VaCuus.h"
#include "VaCuusJsScriptHost.h"

DEFINE_LOG_CATEGORY(LogVaCuusJS);

void FVaCuusJsModule::StartupModule()
{
	// The seam registration (M4 spec 3.1): from here on, every UI thread boot
	// creates an FVaCuusJsScriptHost -- unless vacuus.Js.Enable is 0 at that boot.
	// Load order guarantees this precedes any boot: VaCuus is listed before this
	// module in the .uplugin (same Default phase), and the thread itself starts
	// lazily at the first view request, long after module startup. The factory is
	// stateless; it runs on the UI thread, inside FVaCuusUIThread::Init().
	FVaCuusModule::Get().SetScriptHostFactory(
		[]() -> TUniquePtr<IVaCuusScriptHost> { return MakeUnique<FVaCuusJsScriptHost>(); });

	UE_LOG(LogVaCuusJS, Log, TEXT("VaCuusJs module started (quickjs-ng %hs vendored, see Source/ThirdParty/quickjs-ng/VENDORED_TAG.txt)"),
		"v0.15.1");
}

void FVaCuusJsModule::ShutdownModule()
{
	// SHUTDOWN ORDER IS THE INVERSE OF VaCuusRender'S PROBLEM. Render loads at
	// PostConfigInit and therefore shuts down AFTER VaCuus, so its host objects are
	// destroyed by a still-loaded module; this module loads after VaCuus in the same
	// phase and shuts down BEFORE it (reverse load order -- the VaCuus.cpp
	// ShutdownModule comment walks the same ordering), while the UI thread may still
	// be live and holding a script host whose vtable lives in THIS module. So: stop
	// the thread ourselves -- idempotent, and FVaCuusModule::ShutdownModule repeats
	// it harmlessly -- which destroys the host on the UI thread, then clear the
	// factory (a TFunction whose code also lives here) so no later boot can call
	// into an unloading module. The clear is only legal once no thread exists, which
	// the stop just guaranteed.
	if (FVaCuusModule* VaCuus = FVaCuusModule::GetPtr())
	{
		VaCuus->StopUIThread();
		VaCuus->SetScriptHostFactory(nullptr);
	}
}

IMPLEMENT_MODULE(FVaCuusJsModule, VaCuusJs)
