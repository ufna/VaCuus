// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusM1Harness.h"

#include "VaCuusDefines.h"
#include "VaCuusEngine.h"
#include "VaCuusRecordingRenderInterface.h"
#include "VaCuusSlateElement.h"
#include "VaCuusStats.h"

#include "HAL/IConsoleManager.h"
#include "RenderingThread.h"
#include "UnrealClient.h"

#include <RmlUi/Core.h>

// Debug helper for headless verification: request a UI-inclusive screenshot
// after N drawn HUD frames (0 = off). Set BEFORE toggling vacuus.M1HUD on,
// e.g. -ExecCmds="vacuus.M1HUD.AutoShot 10, vacuus.M1HUD".
static TAutoConsoleVariable<int32> CVarVaCuusM1HUDAutoShot(
	TEXT("vacuus.M1HUD.AutoShot"),
	0,
	TEXT("If > 0, request a screenshot (with UI) once this many M1 HUD frames have been drawn."));

FVaCuusM1Harness::FVaCuusM1Harness(const TSharedRef<FVaCuusSlateElement>& InElement)
	: Element(InElement)
{
}

FVaCuusM1Harness::~FVaCuusM1Harness()
{
	Shutdown();
}

bool FVaCuusM1Harness::Boot(FIntPoint InitialViewSize, EDocumentSource Source, const FString& InDocument)
{
	check(IsInGameThread());

	FVaCuusEngine& Engine = FVaCuusEngine::Get();
	if (Engine.IsInitialized())
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("M1 harness requires an uninitialized RmlUi: the recorder must be installed before Initialize()"));
		return false;
	}

	Recorder = MakeUnique<FVaCuusRecordingRenderInterface>();
	Engine.SetRenderInterface(Recorder.Get());
	if (!Engine.Initialize())
	{
		Engine.SetRenderInterface(nullptr);
		Recorder.Reset();
		return false;
	}

	Context = Rml::CreateContext("VaCuusM1",
		Rml::Vector2i(FMath::Max(InitialViewSize.X, 1), FMath::Max(InitialViewSize.Y, 1)));
	if (!Context)
	{
		UE_LOG(LogVaCuus, Error, TEXT("M1 harness failed to create the Rml context"));
		Shutdown();
		return false;
	}

	if (Source == EDocumentSource::VfsPath)
	{
		// Goes through Rml::GetFileInterface() (FVaCuusFileInterface): relative
		// paths — including the document's own <link>/<img> references — resolve
		// against <Project>/Content/DevUI.
		Document = Context->LoadDocument(Rml::String(TCHAR_TO_UTF8(*InDocument)));
	}
	else
	{
		Document = Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(*InDocument)), "m1://hud.rml");
	}

	if (!Document)
	{
		UE_LOG(LogVaCuus, Error, TEXT("M1 harness failed to load the %s document"),
			Source == EDocumentSource::VfsPath ? *FString::Printf(TEXT("VFS ('%s')"), *InDocument) : TEXT("inline test"));
		Shutdown();
		return false;
	}
	Document->Show();

	UE_LOG(LogVaCuus, Log, TEXT("M1 harness booted (%dx%d)"), InitialViewSize.X, InitialViewSize.Y);
	return true;
}

void FVaCuusM1Harness::DrawFrame(FIntPoint ViewSize)
{
	check(IsInGameThread());

	if (!Context || ViewSize.X <= 0 || ViewSize.Y <= 0)
	{
		return;
	}

	if (ViewSize != LastLoggedViewSize)
	{
		// Measurement evidence (Task 10): the size every recorded frame is laid
		// out and replayed at, straight from the widget geometry.
		LastLoggedViewSize = ViewSize;
		UE_LOG(LogVaCuus, Log, TEXT("M1 HUD view size now %dx%d"), ViewSize.X, ViewSize.Y);
	}

	Recorder->BeginFrame(ViewSize);
	Context->SetDimensions(Rml::Vector2i(ViewSize.X, ViewSize.Y));

	{
		VACUUS_PERF_SCOPE(Update);
		Context->Update();
	}

	TUniquePtr<FVaCuusCommandBuffer> Buffer;
	{
		VACUUS_PERF_SCOPE(Record);
		Context->Render();
		Buffer = Recorder->EndFrameAndPublish();
	}

	ENQUEUE_RENDER_COMMAND(VaCuusPublishUIFrame)(
		[LocalElement = Element, Buf = MoveTemp(Buffer)](FRHICommandListImmediate& RHICmdList) mutable
		{
			LocalElement->SetPendingBuffer_RenderThread(RHICmdList, MoveTemp(Buf));
		});

	++FrameCount;
	const int32 AutoShotFrame = CVarVaCuusM1HUDAutoShot.GetValueOnGameThread();
	if (!bAutoShotDone && AutoShotFrame > 0 && FrameCount >= AutoShotFrame)
	{
		bAutoShotDone = true;
		UE_LOG(LogVaCuus, Log, TEXT("M1 HUD auto-screenshot after %d frames"), FrameCount);
		FScreenshotRequest::RequestScreenshot(/*bInShowUI=*/true);
	}

	FVaCuusPerfLog::TickLog();
}

void FVaCuusM1Harness::Shutdown()
{
	check(IsInGameThread());

	if (Document)
	{
		// Queues the document unload; RmlUi processes it during RemoveContext.
		Document->Close();
		Document = nullptr;
	}

	if (Context)
	{
		Rml::RemoveContext("VaCuusM1");
		Context = nullptr;
	}

	if (Recorder)
	{
		FVaCuusEngine& Engine = FVaCuusEngine::Get();
		if (Engine.IsInitialized())
		{
			// Rml::Shutdown() releases remaining geometry/textures through the
			// recorder, so the recorder must still be alive here. That trailing
			// release traffic lands in a pending buffer that is never published
			// — dropped with the recorder below, together with the replayer's
			// mirror resources (the documented M1 teardown pattern).
			Engine.Shutdown();
		}

		// Don't leave FVaCuusEngine holding a raw pointer to the dead recorder.
		Engine.SetRenderInterface(nullptr);
		Recorder.Reset();
	}
}
