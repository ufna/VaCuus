// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "SVaCuusWidget.h"
#include "VaCuus.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusInteractiveSnapshot.h"
#include "VaCuusCommandBuffer.h"
#include "VaCuusRecordingRenderInterface.h"
#include "VaCuusReplayRenderer.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "VaCuusSlateElement.h"
#include "VaCuusSubsystem.h"
#include "VaCuusTestDocumentHost.h"
#include "VaCuusUIThread.h"
#include "VaCuusView.h"
#include "VaCuusViewStatus.h"

#include "Engine/GameInstance.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "HAL/PlatformProcess.h"
#include "Input/Events.h"
#include "Layout/Geometry.h"
#include "Layout/WidgetPath.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SWindow.h"

#include <RmlUi/Core.h>

#include <atomic>

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusMultiTouchRoutingTest
{
/**
 * Counts what RmlUi actually delivered to the document: releases, mouseouts on the button, and clicks with the element
 * they landed on. Those are the only observables that tell two fingers apart -- the widget's
 * FReply says what Slate was asked, not what RmlUi did with the finger.
 *
 * Same hand-off rule as VaCuusTouchInputTest's probe: plain members written on the UI thread,
 * read on the test thread only after RunFrames() has seen the frame counter advance.
 */
// A virtual green image makes document-relative resolution observable without disk IO.
// The production recorder still generates/uploads the texture and records every draw.
class FRelativeImageRecorder final : public FVaCuusRecordingRenderInterface
{
public:
	virtual Rml::TextureHandle LoadTexture(Rml::Vector2i& OutDimensions, const Rml::String& Source) override
	{
		if (Source.find("marker.png") == Rml::String::npos)
		{
			return FVaCuusRecordingRenderInterface::LoadTexture(OutDimensions, Source);
		}
		BadRequests += Source != "Tests/marker.png";
		OutDimensions = Rml::Vector2i(1, 1);
		const Rml::byte Green[] = {0, 255, 0, 255};
		return GenerateTexture(Rml::Span<const Rml::byte>(Green, 4), OutDimensions);
	}
	int32 BadRequests = 0;
};

class FProbeHost final
	: public FVaCuusTestDocumentHost
	, public Rml::EventListener
{
public:
	explicit FProbeHost(bool bInRecord = false)
		: FVaCuusTestDocumentHost(TEXT("vacuus_multitouch_routing_view"), bInRecord ? "Tests/clone-source.rml" : "vacuus://multitouch_routing.rml", Rml::FocusFlag::Auto)
		, bRecord(bInRecord)
	{
	}

	virtual void SetVisible(bool bVisible) override
	{
		check(FVaCuusUIThread::IsInUIThread());
		if (RmlDocument)
		{
			bVisible ? RmlDocument->Show() : RmlDocument->Hide();
		}
	}

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		if (Recorder)
		{
			Recorder->BeginFrame(ViewSize);
		}
		switch (Action.exchange(EAction::None, std::memory_order_acq_rel))
		{
			case EAction::RemoveSecond:
				if (Rml::Element* Source = RmlDocument ? RmlDocument->GetElementById("btn2") : nullptr)
				{
					Rml::ElementPtr Removed = Source->GetParentNode()->RemoveChild(Source);
				}
				break;
			case EAction::Close:
				CloseDocument();
				break;
			default:
				break;
		}
		Context->Update();

		FVaCuusInteractiveSnapshot& Snapshot = Status->GetSnapshotWriteBuffer();
		BuildVaCuusInteractiveSnapshot(*Context, ViewSize, ++SnapshotGeneration, Snapshot);
		Status->PublishSnapshot();
		if (Recorder)
		{
			Context->Render();
			if (TUniquePtr<FVaCuusCommandBuffer> Frame = Recorder->EndFrameAndPublish())
			{
				for (const auto& Pair : Frame->NewGeometry)
				{
					Geometry.Add(Pair.Key, Pair.Value);
				}
				for (const auto& Pair : Frame->NewTextures)
				{
					Textures.Add(Pair.Key, Pair.Value);
				}
				BadImageRequests = Recorder->BadRequests;
				GhostBounds.Reset();
				for (const FVaCuusCommand& Command : Frame->Commands)
				{
					const FVaCuusGeometryData* Mesh = Geometry.Find(Command.Geometry);
					if (Command.Type != EVaCuusCommandType::DrawGeometry || !Mesh || Mesh->Vertices.IsEmpty())
					{
						continue;
					}
					FBox2D Box(ForceInit);
					bool bBlue = true;
					for (const FVaCuusVertex& Vertex : Mesh->Vertices)
					{
						// Recorder vertices carry RmlUi RGBA bytes, not FColor's BGRA fields
						// (VaCuusRecordingRenderInterface.cpp:26-34).
						uint8 RGBA[4];
						FMemory::Memcpy(RGBA, &Vertex.Color, sizeof(RGBA));
						bBlue &= RGBA[0] == 0 && RGBA[1] == 0 && RGBA[2] == 255 && RGBA[3] == 255;
						Box += FVector2D(Vertex.Position + Command.Translation);
					}
					if (bBlue)
					{
						GhostBounds.Add(Box);
					}
				}
				ReplayFrame = MakeUnique<FVaCuusCommandBuffer>(*Frame);
				ReplayFrame->NewGeometry = Geometry;
				ReplayFrame->ReleasedGeometry.Reset();
				ReplayFrame->NewTextures = Textures;
				ReplayFrame->ReleasedTextures.Reset();
				for (const FVaCuusTextureHandle Handle : Frame->ReleasedTextures)
				{
					Textures.Remove(Handle);
				}
				for (const FVaCuusGeometryHandle Handle : Frame->ReleasedGeometry)
				{
					Geometry.Remove(Handle);
				}
			}
		}

		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

	//~ Begin Rml::EventListener
	virtual void ProcessEvent(Rml::Event& Event) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		Rml::Element* const Target = Event.GetTargetElement();
		const FString TargetId = Target ? FString(UTF8_TO_TCHAR(Target->GetId().c_str())) : FString();

		switch (Event.GetId())
		{
			case Rml::EventId::Dragstart:
				if (RemoveOnDragStart.exchange(false, std::memory_order_acq_rel))
				{
					Rml::Element* Source = static_cast<Rml::Element*>(Event.GetParameter<void*>("drag_element", nullptr));
					if (Source && Source->GetParentNode())
					{
						Rml::ElementPtr Removed = Source->GetParentNode()->RemoveChild(Source);
					}
				}
				break;
			case Rml::EventId::Mouseup:
				++NumMouseUp;
				break;
			case Rml::EventId::Click:
				++NumClick;
				LastClickId = TargetId;
				break;
			case Rml::EventId::Mouseout:
				// Mouseout is sent once per element in the old hover chain, each targeting that element, and
				// bubbles to the document. Only the button is counted; the other elements in the chain are not.
				if (TargetId == TEXT("btn"))
				{
					++NumMouseOutOnButton;
				}
				break;
			default:
				break;
		}
	}
	//~ End Rml::EventListener

	//~ Post-frame observations; see the class comment.
	int32 NumMouseUp = 0;
	int32 NumClick = 0;
	FString LastClickId;
	int32 NumMouseOutOnButton = 0;

	enum class EAction { None, RemoveSecond, Close };
	std::atomic<EAction> Action{EAction::None};
	std::atomic<bool> RemoveOnDragStart{false};
	TArray<FBox2D> GhostBounds;
	int32 BadImageRequests = 0;
	TUniquePtr<FVaCuusCommandBuffer> ReplayFrame;

protected:
	virtual Rml::RenderInterface* CreateRenderInterface() override
	{
		if (bRecord)
		{
			Recorder = MakeUnique<FRelativeImageRecorder>();
		}
		return Recorder.Get();
	}

	virtual void OnDocumentAdopted() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		RmlDocument->AddEventListener(Rml::EventId::Mouseup, this);
		RmlDocument->AddEventListener(Rml::EventId::Dragstart, this);
		RmlDocument->AddEventListener(Rml::EventId::Click, this);
		RmlDocument->AddEventListener(Rml::EventId::Mouseout, this);
	}

	virtual void OnDocumentClosing() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		RmlDocument->RemoveEventListener(Rml::EventId::Mouseup, this);
		RmlDocument->RemoveEventListener(Rml::EventId::Dragstart, this);
		RmlDocument->RemoveEventListener(Rml::EventId::Click, this);
		RmlDocument->RemoveEventListener(Rml::EventId::Mouseout, this);
	}

private:
	bool bRecord = false;
	TUniquePtr<FRelativeImageRecorder> Recorder;
	TMap<FVaCuusTextureHandle, FVaCuusTextureData> Textures;
	TMap<FVaCuusGeometryHandle, FVaCuusGeometryData> Geometry;
	uint64 SnapshotGeneration = 0;
};

/** One UI frame at a time; the wake event coalesces, so N triggers are not N frames. */
static bool RunFrames(FVaCuusUIThread& UIThread, int32 NumFrames)
{
	for (int32 Index = 0; Index < NumFrames; ++Index)
	{
		const uint64 Before = UIThread.GetFrameCount();
		UIThread.Trigger();
		if (!UIThread.WaitForFrameCount(Before + 1, 5.0))
		{
			return false;
		}
	}

	return true;
}

/**
 * 400x300. Two buttons, one above the other, with a gap between them that holds nothing. The gap is
 * the pass-through spot: a finger that moves onto it is still a finger, but no element answers for it.
 */
static const TCHAR* GDocument = TEXT(R"(<rml>
<head>
<style>
body { display: block; width: 100%; height: 100%; }
button { display: block; position: absolute; }
#btn  { left: 20px; top: 20px;  width: 100px; height: 40px; tab-index: auto; }
#btn2 { left: 20px; top: 100px; width: 100px; height: 40px; tab-index: auto; }
</style>
</head>
<body>
	<button id="btn"/>
	<button id="btn2"/>
</body>
</rml>)");

static const FIntPoint GViewSize(400, 300);

static const FVector2D GButtonPoint(70.0, 40.0);
static const FVector2D GButton2Point(70.0, 120.0);
static const FVector2D GGapPoint(70.0, 80.0);

/** Empty space, well away from both buttons and the gap. */
static const FVector2D GFarPoint(300.0, 250.0);

/**
 * A TOUCH event, as VaCuusTouchInputTest builds one: the touch constructor sets bIsTouchEvent, puts
 * the finger on its own PointerIndex, and bakes {LeftMouseButton} into PressedButtons for press and
 * release alike.
 */
static FPointerEvent MakeTouchEvent(
	const FVector2D& Position, const FVector2D& LastPosition, int32 FingerIndex = 0, int32 SlateUserIndex = 0)
{
	return FPointerEvent(static_cast<uint32>(SlateUserIndex), static_cast<uint32>(FingerIndex), Position, LastPosition,
		/*InForce=*/1.0f, /*bPressLeftMouseButton=*/true, /*bInIsForceChanged=*/false, /*bInIsFirstMove=*/false,
		FModifierKeysState());
}

/**
 * A MOUSE event on the cursor pointer. A press names the button that went down in both PressedButtons and
 * EffectingButton; a move passes no buttons, and a release passes the buttons still held.
 */
static FPointerEvent MakeMouseEvent(const FVector2D& Position, const TSet<FKey>& PressedButtons, const FKey& EffectingButton)
{
	return FPointerEvent(/*InUserIndex=*/0, FSlateApplicationBase::CursorPointerIndex, Position, Position, PressedButtons,
		EffectingButton, /*InWheelDelta=*/0.0f, FModifierKeysState());
}

/** A view pixel as the screen position Slate would report for it, when the widget sits at the path's origin. */
static FVector2D ToScreen(const FGeometry& WidgetGeometry, const FVector2D& ViewPixel)
{
	return FVector2D(WidgetGeometry.LocalToAbsolute(ViewPixel / WidgetGeometry.Scale));
}

/** Everything a test needs: the UI thread, the probe, and a widget on a live view. */
struct FRig
{
	FVaCuusUIThread* UIThread = nullptr;
	FProbeHost* Host = nullptr;
	TStrongObjectPtr<UGameInstance> GameInstance;
	TStrongObjectPtr<UVaCuusSubsystem> Subsystem;
	TStrongObjectPtr<UVaCuusView> View;
	TSharedPtr<SVaCuusWidget> Widget;
	FGeometry Geometry;
};

/**
 * Starts the UI thread, loads the fixture and builds the widget, then stops before the first input
 * assertion: every test begins from the same published snapshot. A view handle wired by hand, as in
 * VaCuusTouchInputTest, so there is no PIE world.
 */
static bool BringUp(FAutomationTestBase& Test, FRig& Rig, bool bTakesKeyboardFocus = true, bool bRecordClones = false, bool bImage = false)
{
	if (!Test.TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	FVaCuusModule& Module = FVaCuusModule::Get();
	Rig.UIThread = Module.GetOrStartUIThread();
	if (!Test.TestNotNull(TEXT("UI thread"), Rig.UIThread))
	{
		return false;
	}

	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();

	TUniquePtr<FProbeHost> OwnedHost = MakeUnique<FProbeHost>(bRecordClones);
	Rig.Host = OwnedHost.Get();

	const uint32 ViewId = Rig.UIThread->AllocateViewId();
	Rig.UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), GViewSize, Status);
	FString Source = bRecordClones
		? FString(GDocument).Replace(TEXT("button {"), TEXT("button { background-color: #ff0000; drag: clone;"))
			.Replace(TEXT("</style>"), TEXT("button:drag { background-color: #0000ff; } </style>"))
		: FString(GDocument);
	if (bImage)
	{
		Source.ReplaceInline(TEXT("<button id=\"btn2\"/>"),
			TEXT("<button id=\"btn2\"><img src=\"marker.png\" style=\"display:block;position:absolute;left:40px;top:10px;width:20px;height:20px;\"/></button>"));
	}
	Rig.UIThread->EnqueueLoadDocumentFromMemory(ViewId, Source, /*LoadSerial=*/1);

	Rig.GameInstance = TStrongObjectPtr<UGameInstance>(NewObject<UGameInstance>());
	Rig.Subsystem = TStrongObjectPtr<UVaCuusSubsystem>(NewObject<UVaCuusSubsystem>(Rig.GameInstance.Get()));
	Rig.View = TStrongObjectPtr<UVaCuusView>(NewObject<UVaCuusView>(Rig.Subsystem.Get()));
	Rig.View->InitializeView(Rig.Subsystem.Get(), ViewId, Status, GViewSize);

	const TSharedRef<FVaCuusSlateElement> Element = MakeShared<FVaCuusSlateElement>();
	Rig.Widget = SNew(SVaCuusWidget, Rig.View.Get(), Element, bTakesKeyboardFocus);

	Rig.Geometry = FGeometry::MakeRoot(FVector2f(GViewSize.X, GViewSize.Y), FSlateLayoutTransform());

	if (!Test.TestTrue(TEXT("UI frames ran"), RunFrames(*Rig.UIThread, 2)))
	{
		return false;
	}
	if (!Test.TestTrue(TEXT("Document loaded"),
			Status->LoadCompletedSerial.load(std::memory_order_acquire) == 1 &&
				Status->LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded)))
	{
		return false;
	}

	// The widget answers from the view's cached snapshot, which only PollStatus() refreshes. The
	// document never changes, so one call covers every test in this file.
	Rig.View->PollStatus();
	return true;
}

/** Flushes the queue, detaches the widget and stops the UI thread. Safe on a rig that never came up. */
static void TearDown(FRig& Rig)
{
	if (Rig.UIThread == nullptr)
	{
		return;
	}

	RunFrames(*Rig.UIThread, 1);
	if (Rig.Widget.IsValid())
	{
		Rig.Widget->DetachView();
	}
	FVaCuusModule::Get().StopUIThread();
}
static TArray<FColor> ReplayPixels(const FVaCuusCommandBuffer& Frame)
{
	TArray<FColor> Pixels;
	ENQUEUE_RENDER_COMMAND(VaCuusTouchClonesReadback)
	([&Pixels, &Frame](FRHICommandListImmediate& RHICmdList)
	{
		FVaCuusReplayRenderer Replayer;
		Replayer.Replay(RHICmdList, Frame);
		if (FRHITexture* Target = Replayer.GetOutputRT())
		{
			RHICmdList.Transition(FRHITransitionInfo(Target, ERHIAccess::SRVMask, ERHIAccess::CopySrc));
			FRHIGPUTextureReadback Readback(TEXT("VaCuusTouchClones"));
			Readback.EnqueueCopy(RHICmdList, Target);
			RHICmdList.SubmitAndBlockUntilGPUIdle();
			int32 Pitch = 0;
			if (Readback.IsReady())
			{
				if (const void* Data = Readback.Lock(Pitch))
				{
					Pixels.SetNumUninitialized(Frame.ViewSize.X * Frame.ViewSize.Y);
					for (int32 Y = 0; Y < Frame.ViewSize.Y; ++Y)
					{
						FMemory::Memcpy(Pixels.GetData() + Y * Frame.ViewSize.X,
							static_cast<const FColor*>(Data) + int64(Y) * Pitch, Frame.ViewSize.X * sizeof(FColor));
					}
					Readback.Unlock();
				}
			}
		}
		Replayer.ReleaseResources();
	});
	FlushRenderingCommands();
	return Pixels;
}
}	 // namespace VaCuusMultiTouchRoutingTest

/**
 * Two fingers on two buttons are two gestures: each is handled, each asks Slate for its own capture,
 * and the widget counts both as outstanding.
 *
 * WHY THE REPLIES' CAPTORS AND NOT JUST IsEventHandled(). Before this change the second press answered
 * Handled and asked for nothing, because the one-bit mouse capture was already held by the first
 * finger. Slate then routes the second finger's moves by hit test, so a drag that leaves its button
 * is lost. FReply::GetMouseCaptor() is what Slate is told, and it is the only place that is visible.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchTwoFingersTest, "VaCuus.Input.MultiTouchRouting.TwoFingersBothHandledAndCaptured",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusMultiTouchTwoFingersTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchRoutingTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no worker thread to drive"));
		return true;
	}

	FRig Rig;
	ON_SCOPE_EXIT
	{
		TearDown(Rig);
	};
	if (!BringUp(*this, Rig))
	{
		return false;
	}

	SVaCuusWidget* const Widget = Rig.Widget.Get();

	const FReply First = Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButtonPoint, GButtonPoint, /*FingerIndex=*/0));
	TestTrue(TEXT("finger 0 on a button is handled"), First.IsEventHandled());
	TestSamePtr(TEXT("...and asks Slate to capture it"), First.GetMouseCaptor().Get(), static_cast<SWidget*>(Widget));

	const FReply Second = Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButton2Point, GButton2Point, /*FingerIndex=*/1));
	TestTrue(TEXT("finger 1 on the other button is handled"), Second.IsEventHandled());
	TestSamePtr(TEXT("...and asks Slate to capture it for its own pointer"), Second.GetMouseCaptor().Get(), static_cast<SWidget*>(Widget));

	TestEqual(TEXT("both fingers are outstanding"), Widget->GetNumActiveTouches_Debug(), 2);
	TestFalse(TEXT("fingers never take the mouse flag: this widget is not its holder"),
		SVaCuusWidget::GetMouseCaptureHolder_Debug().Get() == Widget);

	Widget->OnTouchEnded(Rig.Geometry, MakeTouchEvent(GButtonPoint, GButtonPoint, /*FingerIndex=*/0));
	Widget->OnTouchEnded(Rig.Geometry, MakeTouchEvent(GButton2Point, GButton2Point, /*FingerIndex=*/1));
	TestEqual(TEXT("...and both are ended"), Widget->GetNumActiveTouches_Debug(), 0);
	TestFalse(TEXT("...and no capture is tracked any more"), Widget->IsTrackingMouseCapture_Debug());

	return true;
}

/**
 * A finger that is captured keeps its moves even off every interactive rect, while a second finger
 * is down. The gap is the control: the snapshot must NOT cover it, or the Handled answer proves nothing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchSecondMoveTest, "VaCuus.Input.MultiTouchRouting.SecondFingerMoveOutsideStillHandled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusMultiTouchSecondMoveTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchRoutingTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no worker thread to drive"));
		return true;
	}

	FRig Rig;
	ON_SCOPE_EXIT
	{
		TearDown(Rig);
	};
	if (!BringUp(*this, Rig))
	{
		return false;
	}

	SVaCuusWidget* const Widget = Rig.Widget.Get();

	Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButtonPoint, GButtonPoint, /*FingerIndex=*/0));
	Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButton2Point, GButton2Point, /*FingerIndex=*/1));

	TestFalse(TEXT("control: the gap is not interactive"), Rig.View->GetSnapshot().Contains(FIntPoint(70, 80)));
	TestTrue(TEXT("finger 1 moves off every button onto the gap, and is still handled"),
		Widget->OnTouchMoved(Rig.Geometry, MakeTouchEvent(GGapPoint, GButton2Point, /*FingerIndex=*/1)).IsEventHandled());

	Widget->OnTouchEnded(Rig.Geometry, MakeTouchEvent(GGapPoint, GGapPoint, /*FingerIndex=*/1));
	Widget->OnTouchEnded(Rig.Geometry, MakeTouchEvent(GButtonPoint, GButtonPoint, /*FingerIndex=*/0));
	TestEqual(TEXT("...and both are ended"), Widget->GetNumActiveTouches_Debug(), 0);

	return true;
}

/**
 * Lifting the first finger ends only the first finger. The second still clicks its own button, and
 * the engine's own release leaves the second finger's capture in place.
 *
 * THE ENGINE HALF (per-finger capture, Slate capture release). The handler-level rows can say what this
 * widget does with a finger; they cannot say what Slate releases when one lifts. So the last block
 * routes the fingers through a real window and asks Slate's captor map. Source: a touch end
 * releases only its own PointerIdx (SlateUser.cpp:1284-1290, the ReleaseCapture at :1290); Slate does
 * not release the other fingers' captures.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchFirstEndTest, "VaCuus.Input.MultiTouchRouting.FirstFingerEndDoesNotCancelSecond",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusMultiTouchFirstEndTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchRoutingTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no worker thread to drive"));
		return true;
	}

	FRig Rig;
	ON_SCOPE_EXIT
	{
		TearDown(Rig);
	};
	if (!BringUp(*this, Rig))
	{
		return false;
	}

	SVaCuusWidget* const Widget = Rig.Widget.Get();

	Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButtonPoint, GButtonPoint, /*FingerIndex=*/0));
	Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButton2Point, GButton2Point, /*FingerIndex=*/1));
	if (!TestTrue(TEXT("UI frame ran after both presses"), RunFrames(*Rig.UIThread, 1)))
	{
		return false;
	}

	const FReply FirstUp = Widget->OnTouchEnded(Rig.Geometry, MakeTouchEvent(GButtonPoint, GButtonPoint, /*FingerIndex=*/0));
	TestTrue(TEXT("finger 0's release is ours"), FirstUp.IsEventHandled());
	TestTrue(TEXT("...and asks Slate to release that pointer's capture"), FirstUp.ShouldReleaseMouse());
	TestEqual(TEXT("ending finger 0 leaves finger 1 outstanding"), Widget->GetNumActiveTouches_Debug(), 1);
	if (!TestTrue(TEXT("UI frame ran after the first release"), RunFrames(*Rig.UIThread, 1)))
	{
		return false;
	}

	const int32 ClicksBeforeSecond = Rig.Host->NumClick;
	Widget->OnTouchEnded(Rig.Geometry, MakeTouchEvent(GButton2Point, GButton2Point, /*FingerIndex=*/1));
	if (!TestTrue(TEXT("UI frame ran after the second release"), RunFrames(*Rig.UIThread, 1)))
	{
		return false;
	}
	TestEqual(TEXT("finger 1's tap on the second button still clicks"), Rig.Host->NumClick, ClicksBeforeSecond + 1);
	TestEqual(TEXT("...and the click names that button"), Rig.Host->LastClickId, FString(TEXT("btn2")));
	TestEqual(TEXT("...and nothing is left outstanding"), Widget->GetNumActiveTouches_Debug(), 0);

	// The engine half, through a real window. Only this widget's own handlers and Slate's captor map
	// are involved: the presses and releases are the ones FSlateApplication would route.
	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Skipped the engine half: no FSlateApplication, so there is no captor map to read"));
		return true;
	}

	{
		FSlateApplication& Slate = FSlateApplication::Get();
		const int32 UserIndex = static_cast<int32>(MakeTouchEvent(FVector2D::ZeroVector, FVector2D::ZeroVector).GetUserIndex());
		const TSharedPtr<SWidget> EditorFocus = Slate.GetUserFocusedWidget(UserIndex);

		// The grant at SlateApplication.cpp:3494 needs the app active, or this flag letting inactive device
		// input through. Set here so the routed check does not depend on which window has focus; restored
		// after the window below has its destruction requested, because this scope exit is declared first.
		const bool bPrev = Slate.GetHandleDeviceInputWhenApplicationNotActive();
		Slate.SetHandleDeviceInputWhenApplicationNotActive(true);
		ON_SCOPE_EXIT
		{
			Slate.SetHandleDeviceInputWhenApplicationNotActive(bPrev);
		};

		TSharedRef<SWindow> Window = SNew(SWindow).ClientSize(FVector2D(GViewSize.X, GViewSize.Y)).Content()
		[
			SNew(SBox).WidthOverride(static_cast<float>(GViewSize.X)).HeightOverride(static_cast<float>(GViewSize.Y))
			[
				Rig.Widget.ToSharedRef()
			]
		];

		// Detached before the window's destruction is requested: the destruction is only requested here, and
		// a widget that outlives its view must not reach the next test.
		ON_SCOPE_EXIT
		{
			Widget->ReleaseOwnPointerCapture(TEXT("the routing check is over"));
			Widget->DetachView();
			if (EditorFocus.IsValid())
			{
				Slate.SetUserFocus(UserIndex, EditorFocus, EFocusCause::SetDirectly);
			}
			else
			{
				Slate.ClearUserFocus(UserIndex, EFocusCause::SetDirectly);
			}
			Slate.RequestDestroyWindow(Window);
		};

		Slate.AddWindow(Window, /*bShowImmediately=*/false);
		Window->SlatePrepass();

		FWidgetPath FoundPath;
		if (!TestTrue(TEXT("the widget is reachable from its window"), Slate.FindPathToWidget(Rig.Widget.ToSharedRef(), FoundPath)))
		{
			return false;
		}

		// Rebuilt the way the hit-test grid builds one: routing reads a pointer position for every widget on it.
		TArray<FWidgetAndPointer> WidgetsAndPointers;
		for (int32 Index = 0; Index < FoundPath.Widgets.Num(); ++Index)
		{
			WidgetsAndPointers.Emplace(FoundPath.Widgets[Index]);
		}
		const FWidgetPath WidgetPath(WidgetsAndPointers);
		const FGeometry WidgetGeometry = WidgetPath.Widgets.Last().Geometry;

		const FVector2D Finger0Screen = ToScreen(WidgetGeometry, GButtonPoint);
		const FVector2D Finger1Screen = ToScreen(WidgetGeometry, GButton2Point);

		Slate.RoutePointerDownEvent(WidgetPath, MakeTouchEvent(Finger0Screen, Finger0Screen, /*FingerIndex=*/0));
		TestTrue(TEXT("routed: Slate records finger 0's capture for pointer 0"), Widget->HasMouseCaptureByUser(UserIndex, 0));

		Slate.RoutePointerDownEvent(WidgetPath, MakeTouchEvent(Finger1Screen, Finger1Screen, /*FingerIndex=*/1));
		TestTrue(TEXT("routed: finger 1's capture is recorded for pointer 1"), Widget->HasMouseCaptureByUser(UserIndex, 1));
		TestTrue(TEXT("routed: and finger 0's is still there"), Widget->HasMouseCaptureByUser(UserIndex, 0));

		Slate.RoutePointerUpEvent(WidgetPath, MakeTouchEvent(Finger0Screen, Finger0Screen, /*FingerIndex=*/0));
		TestFalse(TEXT("routed: lifting finger 0 releases pointer 0 only"), Widget->HasMouseCaptureByUser(UserIndex, 0));
		TestTrue(TEXT("routed: finger 1 keeps its capture through that release"), Widget->HasMouseCaptureByUser(UserIndex, 1));
		TestEqual(TEXT("routed: one finger is still outstanding"), Widget->GetNumActiveTouches_Debug(), 1);

		Slate.RoutePointerUpEvent(WidgetPath, MakeTouchEvent(Finger1Screen, Finger1Screen, /*FingerIndex=*/1));
		TestFalse(TEXT("routed: lifting finger 1 releases it"), Widget->HasMouseCaptureByUser(UserIndex, 1));
		TestEqual(TEXT("routed: nothing is left outstanding"), Widget->GetNumActiveTouches_Debug(), 0);

		// Route both orders through Slate: its touch enter/leave synthesis must not move the
		// primary's hover (SlateApplication.cpp:5466-5473, :5584-5587).
		Slate.RoutePointerDownEvent(WidgetPath, MakeTouchEvent(Finger0Screen, Finger0Screen, /*FingerIndex=*/0));
		if (!TestTrue(TEXT("routed hover: primary press processed"), RunFrames(*Rig.UIThread, 1)))
		{
			return false;
		}
		const int32 OutsBeforeSecondary = Rig.Host->NumMouseOutOnButton;
		Slate.RoutePointerDownEvent(WidgetPath, MakeTouchEvent(Finger1Screen, Finger1Screen, /*FingerIndex=*/1));
		if (!TestTrue(TEXT("routed hover: secondary press processed"), RunFrames(*Rig.UIThread, 1)))
		{
			return false;
		}
		TestEqual(TEXT("a secondary touch enter leaves the primary hovered"), Rig.Host->NumMouseOutOnButton, OutsBeforeSecondary);

		Slate.RoutePointerUpEvent(WidgetPath, MakeTouchEvent(Finger1Screen, Finger1Screen, /*FingerIndex=*/1));
		if (!TestTrue(TEXT("routed hover: secondary release processed"), RunFrames(*Rig.UIThread, 1)))
		{
			return false;
		}
		TestEqual(TEXT("a secondary touch leave leaves the primary hovered"), Rig.Host->NumMouseOutOnButton, OutsBeforeSecondary);
		TestTrue(TEXT("routed: primary still owns pointer 0 after secondary lifts"), Widget->HasMouseCaptureByUser(UserIndex, 0));
		const int32 ClicksBeforePrimary = Rig.Host->NumClick;
		Slate.RoutePointerUpEvent(WidgetPath, MakeTouchEvent(Finger0Screen, Finger0Screen, /*FingerIndex=*/0));
		if (!TestTrue(TEXT("routed hover: primary release processed"), RunFrames(*Rig.UIThread, 1)))
		{
			return false;
		}
		TestEqual(TEXT("routed: primary still clicks after secondary lifts"), Rig.Host->NumClick, ClicksBeforePrimary + 1);
		TestEqual(TEXT("routed: that click belongs to the primary's button"), Rig.Host->LastClickId, FString(TEXT("btn")));

		Slate.RoutePointerDownEvent(WidgetPath, MakeTouchEvent(Finger1Screen, Finger1Screen, 1));
		const TSharedPtr<FSlateUser> User = Slate.GetUser(UserIndex);
		if (!TestTrue(TEXT("a second widget can hold another pointer for the same user"),
			User.IsValid() && User->SetPointerCaptor(2, Window, WidgetPath)))
		{
			return false;
		}
		ON_SCOPE_EXIT { User->ReleaseCapture(2); };
		Widget->ReleaseOwnPointerCapture(TEXT("per-pointer teardown test"));
		TestFalse(TEXT("teardown releases this widget's touch"), Widget->HasMouseCaptureByUser(UserIndex, 1));
		TestTrue(TEXT("teardown preserves another widget's pointer for the same user"), User->DoesWidgetHaveCapture(Window, 2));
	}

	return true;
}

/**
 * OnMouseCaptureLost names the pointer that lost capture (FCaptureLostEvent carries it). Losing pointer
 * 1 cancels finger 1 and nothing else: finger 0 keeps its capture and its RmlUi state. The next phase
 * loses the last finger, which must leave the set and must reach RmlUi as one MouseLeave. The last phase
 * is the other branch of the same callback: losing the cursor pointer cancels every finger that Slate
 * holds nothing for. Here no Slate captor stands behind either finger, so both are cancelled.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchCaptureLostTest, "VaCuus.Input.MultiTouchRouting.CaptureLostForPointer1CancelsOnly1",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusMultiTouchCaptureLostTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchRoutingTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no worker thread to drive"));
		return true;
	}

	FRig Rig;
	ON_SCOPE_EXIT
	{
		TearDown(Rig);
	};
	if (!BringUp(*this, Rig))
	{
		return false;
	}

	SVaCuusWidget* const Widget = Rig.Widget.Get();

	Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButtonPoint, GButtonPoint, /*FingerIndex=*/0));
	Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButton2Point, GButton2Point, /*FingerIndex=*/1));
	TestEqual(TEXT("both fingers are outstanding"), Widget->GetNumActiveTouches_Debug(), 2);
	if (!TestTrue(TEXT("UI frame ran after both presses"), RunFrames(*Rig.UIThread, 1)))
	{
		return false;
	}

	const int32 MouseUpsBefore = Rig.Host->NumMouseUp;

	Widget->OnMouseCaptureLost(FCaptureLostEvent(/*InUserIndex=*/0, /*InPointerIndex=*/1));
	TestEqual(TEXT("losing pointer 1 cancels that finger only"), Widget->GetNumActiveTouches_Debug(), 1);

	TestFalse(TEXT("control: the gap is not interactive"), Rig.View->GetSnapshot().Contains(FIntPoint(70, 80)));
	TestTrue(TEXT("finger 0 keeps its capture: a move onto the gap is still handled"),
		Widget->OnTouchMoved(Rig.Geometry, MakeTouchEvent(GGapPoint, GButtonPoint, /*FingerIndex=*/0)).IsEventHandled());

	if (!TestTrue(TEXT("UI frame ran after the cancel"), RunFrames(*Rig.UIThread, 1)))
	{
		return false;
	}
	TestEqual(TEXT("RmlUi released button 0 for the cancelled finger, once"), Rig.Host->NumMouseUp, MouseUpsBefore + 1);

	Widget->OnTouchEnded(Rig.Geometry, MakeTouchEvent(GGapPoint, GGapPoint, /*FingerIndex=*/0));
	TestEqual(TEXT("...and ending the survivor empties the ledger"), Widget->GetNumActiveTouches_Debug(), 0);
	TestFalse(TEXT("...and no capture is tracked: the lost finger left the set too"), Widget->IsTrackingMouseCapture_Debug());

	// Losing the last finger is the one that must reach RmlUi as a MouseLeave: the widget sends it once the set
	// and the mouse flag are both empty. RmlUi shows that as a mouseout on the element the finger was over.
	Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButtonPoint, GButtonPoint, /*FingerIndex=*/0));
	if (!TestTrue(TEXT("UI frame ran after a lone press"), RunFrames(*Rig.UIThread, 1)))
	{
		return false;
	}
	const int32 MouseOutsBefore = Rig.Host->NumMouseOutOnButton;

	Widget->OnMouseCaptureLost(FCaptureLostEvent(/*InUserIndex=*/0, /*InPointerIndex=*/0));
	if (!TestTrue(TEXT("UI frame ran after the lone capture loss"), RunFrames(*Rig.UIThread, 1)))
	{
		return false;
	}
	TestEqual(TEXT("losing the last finger sends RmlUi a MouseLeave: one mouseout on the button"),
		Rig.Host->NumMouseOutOnButton, MouseOutsBefore + 1);
	TestEqual(TEXT("...and nothing is left outstanding"), Widget->GetNumActiveTouches_Debug(), 0);

	// The cursor pointer is the other branch of the same callback.
	Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButtonPoint, GButtonPoint, /*FingerIndex=*/0));
	Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButton2Point, GButton2Point, /*FingerIndex=*/1));
	Widget->OnMouseButtonDown(Rig.Geometry, MakeMouseEvent(GButton2Point, TSet<FKey>({EKeys::LeftMouseButton}), EKeys::LeftMouseButton));
	Widget->OnMouseCaptureLost(FCaptureLostEvent(/*InUserIndex=*/0, static_cast<int32>(FSlateApplicationBase::CursorPointerIndex)));
	TestFalse(TEXT("losing the cursor also clears an unrouted mouse grant"), SVaCuusWidget::GetMouseCaptureHolder_Debug().Get() == Widget);
	TestEqual(TEXT("losing the cursor pointer leaves both fingers alone"), Widget->GetNumActiveTouches_Debug(), 2);
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication& Slate = FSlateApplication::Get();
		const bool bWasActive = Slate.IsActive();
		ON_SCOPE_EXIT { Slate.OnApplicationActivationChanged(bWasActive); };
		Slate.OnApplicationActivationChanged(true);
		const int32 ClicksBeforeCancel = Rig.Host->NumClick;
		Slate.OnApplicationActivationChanged(false);
		TestEqual(TEXT("app deactivation cancels all fingers, including unrouted grants"), Widget->GetNumActiveTouches_Debug(), 0);
		TestTrue(TEXT("deactivation reached RmlUi"), RunFrames(*Rig.UIThread, 1));
		TestEqual(TEXT("deactivation never turns a held touch into a click"), Rig.Host->NumClick, ClicksBeforeCancel);
	}

	return true;
}

/**
 * A second finger that lands on empty space, while the first holds a button, is Unhandled and takes no
 * capture. The first finger's capture survives that press, and the pass-through finger is still ledgered.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchPassThroughTest, "VaCuus.Input.MultiTouchRouting.TouchOnEmptyAreaUnhandledWhileOtherHeld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusMultiTouchPassThroughTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchRoutingTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no worker thread to drive"));
		return true;
	}

	FRig Rig;
	ON_SCOPE_EXIT
	{
		TearDown(Rig);
	};
	if (!BringUp(*this, Rig))
	{
		return false;
	}

	SVaCuusWidget* const Widget = Rig.Widget.Get();

	const FReply Button = Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButtonPoint, GButtonPoint, /*FingerIndex=*/0));
	TestTrue(TEXT("finger 0 on a button is handled"), Button.IsEventHandled());

	const FReply Gap = Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GGapPoint, GGapPoint, /*FingerIndex=*/1));
	TestFalse(TEXT("finger 1 on empty space, with finger 0 held, is Unhandled"), Gap.IsEventHandled());
	TestFalse(TEXT("...and asks Slate for no capture"), Gap.GetMouseCaptor().IsValid());
	TestEqual(TEXT("...but RmlUi was told about it, so it is still outstanding"), Widget->GetNumActiveTouches_Debug(), 2);

	TestFalse(TEXT("control: the far point is not interactive"), Rig.View->GetSnapshot().Contains(FIntPoint(300, 250)));
	TestFalse(TEXT("finger 1, which holds no capture, moving onto empty space is Unhandled"),
		Widget->OnTouchMoved(Rig.Geometry, MakeTouchEvent(GFarPoint, GGapPoint, /*FingerIndex=*/1)).IsEventHandled());
	TestTrue(TEXT("finger 0 keeps its capture past finger 1's press: a move off every rect is handled"),
		Widget->OnTouchMoved(Rig.Geometry, MakeTouchEvent(GFarPoint, GButtonPoint, /*FingerIndex=*/0)).IsEventHandled());

	TestFalse(TEXT("finger 1's release is pass-through too"),
		Widget->OnTouchEnded(Rig.Geometry, MakeTouchEvent(GFarPoint, GFarPoint, /*FingerIndex=*/1)).IsEventHandled());
	TestTrue(TEXT("finger 0's release is ours"),
		Widget->OnTouchEnded(Rig.Geometry, MakeTouchEvent(GFarPoint, GFarPoint, /*FingerIndex=*/0)).IsEventHandled());
	TestEqual(TEXT("...and nothing is left outstanding"), Widget->GetNumActiveTouches_Debug(), 0);

	return true;
}

/**
 * A finger that starts on empty space is the game's from its press, and stays the game's wherever it goes. It
 * never captured, so a move onto a button and a lift on it are pass-through too: the press decided who hears
 * the rest of the gesture.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchCrossTest, "VaCuus.Input.MultiTouchRouting.PassThroughFingerCrossesButton",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusMultiTouchCrossTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchRoutingTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no worker thread to drive"));
		return true;
	}

	FRig Rig;
	ON_SCOPE_EXIT
	{
		TearDown(Rig);
	};
	if (!BringUp(*this, Rig))
	{
		return false;
	}

	SVaCuusWidget* const Widget = Rig.Widget.Get();
	const int32 Baseline = Widget->GetNumActiveTouches_Debug();

	const FReply Press = Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GGapPoint, GGapPoint, /*FingerIndex=*/0));
	TestFalse(TEXT("the press on the gap is pass-through"), Press.IsEventHandled());
	TestEqual(TEXT("...but RmlUi was told about the finger"), Widget->GetNumActiveTouches_Debug(), Baseline + 1);

	TestFalse(TEXT("the finger moves onto a button: OnTouchMoved is Unhandled"),
		Widget->OnTouchMoved(Rig.Geometry, MakeTouchEvent(GButtonPoint, GGapPoint, /*FingerIndex=*/0)).IsEventHandled());
	TestFalse(TEXT("...and its mouse fallback is Unhandled too"),
		Widget->OnMouseMove(Rig.Geometry, MakeTouchEvent(GButtonPoint, GGapPoint, /*FingerIndex=*/0)).IsEventHandled());

	TestFalse(TEXT("the finger lifts on the button: OnTouchEnded is Unhandled"),
		Widget->OnTouchEnded(Rig.Geometry, MakeTouchEvent(GButtonPoint, GButtonPoint, /*FingerIndex=*/0)).IsEventHandled());
	TestFalse(TEXT("...and its mouse fallback is Unhandled too"),
		Widget->OnMouseButtonUp(Rig.Geometry, MakeTouchEvent(GButtonPoint, GButtonPoint, /*FingerIndex=*/0)).IsEventHandled());

	TestEqual(TEXT("nothing is left outstanding"), Widget->GetNumActiveTouches_Debug(), Baseline);

	return true;
}

/**
 * A view that takes no keyboard focus. Slate gives the keyboard to the leaf-most widget under a press that
 * supports it, and this view always did, so a press on empty space used to take the keyboard from the game.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchNoFocusTest, "VaCuus.Input.MultiTouchRouting.NoFocusWhenDisabled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusMultiTouchNoFocusTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchRoutingTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no worker thread to drive"));
		return true;
	}
	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Skipped: needs an FSlateApplication to route the press through"));
		return true;
	}

	FRig Rig;
	ON_SCOPE_EXIT
	{
		TearDown(Rig);
	};
	if (!BringUp(*this, Rig, /*bTakesKeyboardFocus=*/false))
	{
		return false;
	}

	SVaCuusWidget* const Widget = Rig.Widget.Get();
	FSlateApplication& Slate = FSlateApplication::Get();
	const int32 UserIndex = 0;
	const TSharedPtr<SWidget> EditorFocus = Slate.GetUserFocusedWidget(UserIndex);

	// Declared before the window's scope exit, so the flag is restored after the window's destruction is requested.
	const bool bPrev = Slate.GetHandleDeviceInputWhenApplicationNotActive();
	Slate.SetHandleDeviceInputWhenApplicationNotActive(true);
	ON_SCOPE_EXIT
	{
		Slate.SetHandleDeviceInputWhenApplicationNotActive(bPrev);
	};

	TSharedRef<SWindow> Window = SNew(SWindow).ClientSize(FVector2D(GViewSize.X, GViewSize.Y)).Content()
	[
		SNew(SBox).WidthOverride(static_cast<float>(GViewSize.X)).HeightOverride(static_cast<float>(GViewSize.Y))
		[
			Rig.Widget.ToSharedRef()
		]
	];
	ON_SCOPE_EXIT
	{
		Widget->ReleaseOwnPointerCapture(TEXT("the focus check is over"));
		Widget->DetachView();
		if (EditorFocus.IsValid())
		{
			Slate.SetUserFocus(UserIndex, EditorFocus, EFocusCause::SetDirectly);
		}
		else
		{
			Slate.ClearUserFocus(UserIndex, EFocusCause::SetDirectly);
		}
		Slate.RequestDestroyWindow(Window);
	};

	Slate.AddWindow(Window, /*bShowImmediately=*/false);
	Window->SlatePrepass();

	FWidgetPath FoundPath;
	if (!TestTrue(TEXT("the widget is reachable from its window"), Slate.FindPathToWidget(Rig.Widget.ToSharedRef(), FoundPath)))
	{
		return false;
	}
	TArray<FWidgetAndPointer> WidgetsAndPointers;
	for (int32 Index = 0; Index < FoundPath.Widgets.Num(); ++Index)
	{
		WidgetsAndPointers.Emplace(FoundPath.Widgets[Index]);
	}
	const FWidgetPath WidgetPath(WidgetsAndPointers);
	const FGeometry WidgetGeometry = WidgetPath.Widgets.Last().Geometry;

	const TSet<FKey> NoButtons;
	const TSet<FKey> LeftDown({EKeys::LeftMouseButton});
	const FVector2D FarScreen = ToScreen(WidgetGeometry, GFarPoint);

	TestFalse(TEXT("control: the far point is not interactive"), Rig.View->GetSnapshot().Contains(FIntPoint(300, 250)));
	TestFalse(TEXT("the view does not take keyboard focus"), Widget->SupportsKeyboardFocus());

	Slate.ClearUserFocus(UserIndex, EFocusCause::SetDirectly);
	Slate.RoutePointerDownEvent(WidgetPath, MakeMouseEvent(FarScreen, LeftDown, EKeys::LeftMouseButton));
	Slate.RoutePointerUpEvent(WidgetPath, MakeMouseEvent(FarScreen, NoButtons, EKeys::LeftMouseButton));

	TestFalse(TEXT("a press on empty space does not make the view the keyboard's owner"),
		Slate.GetUserFocusedWidget(UserIndex).Get() == Widget);

	return true;
}

/**
 * The mouse and a finger on the same widget are two gestures, and neither takes the other's capture. A
 * finger's capture must not make a mouse move over empty space ours, and a finger that comes and goes while
 * the mouse holds a drag must leave the drag's capture where it was.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchCoexistTest, "VaCuus.Input.MultiTouchRouting.MouseAndFingerCoexist",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusMultiTouchCoexistTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchRoutingTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no worker thread to drive"));
		return true;
	}
	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Skipped: needs an FSlateApplication to route the mouse and the fingers through"));
		return true;
	}

	FRig Rig;
	ON_SCOPE_EXIT
	{
		TearDown(Rig);
	};
	if (!BringUp(*this, Rig))
	{
		return false;
	}

	SVaCuusWidget* const Widget = Rig.Widget.Get();
	FSlateApplication& Slate = FSlateApplication::Get();
	const int32 UserIndex = 0;
	const int32 Cursor = static_cast<int32>(FSlateApplicationBase::CursorPointerIndex);
	const TSharedPtr<SWidget> EditorFocus = Slate.GetUserFocusedWidget(UserIndex);

	// Declared before the window's scope exit, so the flag is restored after the window's destruction is requested.
	const bool bPrev = Slate.GetHandleDeviceInputWhenApplicationNotActive();
	Slate.SetHandleDeviceInputWhenApplicationNotActive(true);
	ON_SCOPE_EXIT
	{
		Slate.SetHandleDeviceInputWhenApplicationNotActive(bPrev);
	};

	TSharedRef<SWindow> Window = SNew(SWindow).ClientSize(FVector2D(GViewSize.X, GViewSize.Y)).Content()
	[
		SNew(SBox).WidthOverride(static_cast<float>(GViewSize.X)).HeightOverride(static_cast<float>(GViewSize.Y))
		[
			Rig.Widget.ToSharedRef()
		]
	];
	ON_SCOPE_EXIT
	{
		Widget->ReleaseOwnPointerCapture(TEXT("the routing check is over"));
		Widget->DetachView();
		if (EditorFocus.IsValid())
		{
			Slate.SetUserFocus(UserIndex, EditorFocus, EFocusCause::SetDirectly);
		}
		else
		{
			Slate.ClearUserFocus(UserIndex, EFocusCause::SetDirectly);
		}
		Slate.RequestDestroyWindow(Window);
	};

	Slate.AddWindow(Window, /*bShowImmediately=*/false);
	Window->SlatePrepass();

	FWidgetPath FoundPath;
	if (!TestTrue(TEXT("the widget is reachable from its window"), Slate.FindPathToWidget(Rig.Widget.ToSharedRef(), FoundPath)))
	{
		return false;
	}
	TArray<FWidgetAndPointer> WidgetsAndPointers;
	for (int32 Index = 0; Index < FoundPath.Widgets.Num(); ++Index)
	{
		WidgetsAndPointers.Emplace(FoundPath.Widgets[Index]);
	}
	const FWidgetPath WidgetPath(WidgetsAndPointers);
	const FGeometry WidgetGeometry = WidgetPath.Widgets.Last().Geometry;

	const FVector2D Finger0Screen = ToScreen(WidgetGeometry, GButtonPoint);
	const FVector2D Finger1Screen = ToScreen(WidgetGeometry, GButton2Point);
	const FVector2D GapScreen = ToScreen(WidgetGeometry, GGapPoint);

	const TSet<FKey> NoButtons;
	const TSet<FKey> LeftDown({EKeys::LeftMouseButton});

	// A: a finger holds its capture; a mouse move over empty space must not adopt that capture.
	Slate.RoutePointerDownEvent(WidgetPath, MakeTouchEvent(Finger0Screen, Finger0Screen, /*FingerIndex=*/0));
	TestTrue(TEXT("A: finger 0 holds its own capture"), Widget->HasMouseCaptureByUser(UserIndex, 0));

	// A repeat press reaches AnswerPointerDown through OnMouseButtonDown: SendMouseInput drops touch events,
	// so only the reply half runs. The entry is current and Slate holds the captor, so the press asks for nothing.
	const FReply Repeat = Widget->OnMouseButtonDown(WidgetGeometry, MakeTouchEvent(Finger0Screen, Finger0Screen, /*FingerIndex=*/0));
	TestFalse(TEXT("A: a repeat press while Slate holds the captor asks for nothing"), Repeat.GetMouseCaptor().IsValid());

	TestFalse(TEXT("control: the gap is not interactive"), Rig.View->GetSnapshot().Contains(FIntPoint(70, 80)));

	const FReply Move = Widget->OnMouseMove(WidgetGeometry, MakeMouseEvent(GapScreen, NoButtons, FKey()));
	TestFalse(TEXT("A: a mouse move onto the gap, while only a finger holds capture, is Unhandled"), Move.IsEventHandled());
	TestFalse(TEXT("A: ...and the finger's capture was not adopted into the mouse flag"),
		SVaCuusWidget::GetMouseCaptureHolder_Debug().Get() == Widget);

	Slate.RoutePointerUpEvent(WidgetPath, MakeTouchEvent(Finger0Screen, Finger0Screen, /*FingerIndex=*/0));
	TestFalse(TEXT("A: lifting finger 0 leaves nothing tracked"), Widget->IsTrackingMouseCapture_Debug());

	// B: the mouse holds a drag; a finger comes and goes on the other button; the drag keeps its capture.
	Slate.RoutePointerDownEvent(WidgetPath, MakeMouseEvent(Finger0Screen, LeftDown, EKeys::LeftMouseButton));
	TestTrue(TEXT("B: Slate records the mouse's capture on the cursor pointer"), Widget->HasMouseCaptureByUser(UserIndex, Cursor));
	TestTrue(TEXT("B: the widget is the mouse-capture holder"), SVaCuusWidget::GetMouseCaptureHolder_Debug().Get() == Widget);

	Slate.RoutePointerDownEvent(WidgetPath, MakeTouchEvent(Finger1Screen, Finger1Screen, /*FingerIndex=*/1));
	Slate.RoutePointerUpEvent(WidgetPath, MakeTouchEvent(Finger1Screen, Finger1Screen, /*FingerIndex=*/1));
	TestTrue(TEXT("B: after a finger's down and up, Slate still routes the cursor pointer to us"),
		Widget->HasMouseCaptureByUser(UserIndex, Cursor));
	TestTrue(TEXT("B: ...and the widget is still the mouse-capture holder"), SVaCuusWidget::GetMouseCaptureHolder_Debug().Get() == Widget);

	Slate.RoutePointerUpEvent(WidgetPath, MakeMouseEvent(Finger0Screen, NoButtons, EKeys::LeftMouseButton));
	TestFalse(TEXT("B: the mouse's own release hands the capture back"), Widget->IsTrackingMouseCapture_Debug());
	TestFalse(TEXT("B: ...and Slate no longer holds the cursor pointer for us"), Widget->HasMouseCaptureByUser(UserIndex, Cursor));

	return true;
}

/**
 * A finger whose press reply never reached Slate keeps its set entry with no captor behind it. Its next
 * press must ask Slate for the capture again: the entry alone must not swallow the request. Handler-level,
 * because the point is the widget's own bookkeeping; the routed tests cover the grant itself.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchRepressTest, "VaCuus.Input.MultiTouchRouting.RepressAfterUnroutedPressAsksForCapture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusMultiTouchRepressTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchRoutingTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no worker thread to drive"));
		return true;
	}
	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Skipped: the repress asks Slate whether it holds a captor, so it needs an FSlateApplication"));
		return true;
	}

	FRig Rig;
	ON_SCOPE_EXIT
	{
		TearDown(Rig);
	};
	if (!BringUp(*this, Rig))
	{
		return false;
	}

	SVaCuusWidget* const Widget = Rig.Widget.Get();

	const FReply First = Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButtonPoint, GButtonPoint, /*FingerIndex=*/0));
	TestTrue(TEXT("the first press of finger 0 asks for its capture"), First.GetMouseCaptor().IsValid());
	const TSharedPtr<FSlateUser> SlateUser = FSlateApplication::Get().GetUser(/*UserIndex=*/0);
	TestFalse(TEXT("control: that reply was never routed, so Slate holds no captor for pointer 0"),
		SlateUser.IsValid() && SlateUser->HasCapture(/*PointerIndex=*/0));

	const FReply Again = Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButtonPoint, GButtonPoint, /*FingerIndex=*/0));
	TestTrue(TEXT("a repress of finger 0 asks for its capture again"), Again.GetMouseCaptor().IsValid());

	Widget->OnTouchEnded(Rig.Geometry, MakeTouchEvent(GButtonPoint, GButtonPoint, /*FingerIndex=*/0));
	TestEqual(TEXT("ending finger 0 leaves nothing outstanding"), Widget->GetNumActiveTouches_Debug(), 0);

	return true;
}

/**
 * A finger that is down while the mouse clicks elsewhere is still its own gesture. The mouse's orderly
 * release takes the cursor's capture and no other, so losing the cursor pointer must cancel only the fingers
 * Slate does not hold for this widget. The click reaches its button, the finger survives it,
 * and the finger's own release still reaches RmlUi: nothing was cancelled on the way.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchSurvivesClickTest, "VaCuus.Input.MultiTouchRouting.FingerSurvivesMouseClick",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusMultiTouchSurvivesClickTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchRoutingTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no worker thread to drive"));
		return true;
	}
	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Skipped: needs an FSlateApplication to route the mouse and the fingers through"));
		return true;
	}

	FRig Rig;
	ON_SCOPE_EXIT
	{
		TearDown(Rig);
	};
	if (!BringUp(*this, Rig))
	{
		return false;
	}

	SVaCuusWidget* const Widget = Rig.Widget.Get();
	FSlateApplication& Slate = FSlateApplication::Get();
	const int32 UserIndex = 0;
	const int32 Cursor = static_cast<int32>(FSlateApplicationBase::CursorPointerIndex);
	const TSharedPtr<SWidget> EditorFocus = Slate.GetUserFocusedWidget(UserIndex);

	// Declared before the window's scope exit, so the flag is restored after the window's destruction is requested.
	const bool bPrev = Slate.GetHandleDeviceInputWhenApplicationNotActive();
	Slate.SetHandleDeviceInputWhenApplicationNotActive(true);
	ON_SCOPE_EXIT
	{
		Slate.SetHandleDeviceInputWhenApplicationNotActive(bPrev);
	};

	TSharedRef<SWindow> Window = SNew(SWindow).ClientSize(FVector2D(GViewSize.X, GViewSize.Y)).Content()
	[
		SNew(SBox).WidthOverride(static_cast<float>(GViewSize.X)).HeightOverride(static_cast<float>(GViewSize.Y))
		[
			Rig.Widget.ToSharedRef()
		]
	];
	ON_SCOPE_EXIT
	{
		Widget->ReleaseOwnPointerCapture(TEXT("the routing check is over"));
		Widget->DetachView();
		if (EditorFocus.IsValid())
		{
			Slate.SetUserFocus(UserIndex, EditorFocus, EFocusCause::SetDirectly);
		}
		else
		{
			Slate.ClearUserFocus(UserIndex, EFocusCause::SetDirectly);
		}
		Slate.RequestDestroyWindow(Window);
	};

	Slate.AddWindow(Window, /*bShowImmediately=*/false);
	Window->SlatePrepass();

	FWidgetPath FoundPath;
	if (!TestTrue(TEXT("the widget is reachable from its window"), Slate.FindPathToWidget(Rig.Widget.ToSharedRef(), FoundPath)))
	{
		return false;
	}
	TArray<FWidgetAndPointer> WidgetsAndPointers;
	for (int32 Index = 0; Index < FoundPath.Widgets.Num(); ++Index)
	{
		WidgetsAndPointers.Emplace(FoundPath.Widgets[Index]);
	}
	const FWidgetPath WidgetPath(WidgetsAndPointers);
	const FGeometry WidgetGeometry = WidgetPath.Widgets.Last().Geometry;

	const FVector2D Finger1Screen = ToScreen(WidgetGeometry, GButtonPoint);
	const FVector2D Button2Screen = ToScreen(WidgetGeometry, GButton2Point);

	const TSet<FKey> NoButtons;
	const TSet<FKey> LeftDown({EKeys::LeftMouseButton});

	// Read before any input is routed: the UI thread has not been asked to run a frame since BringUp.
	const int32 ClicksBeforeMouse = Rig.Host->NumClick;
	const int32 MouseUpsBeforeMouse = Rig.Host->NumMouseUp;

	// Finger 1 holds btn; Slate grants it pointer 1.
	Slate.RoutePointerDownEvent(WidgetPath, MakeTouchEvent(Finger1Screen, Finger1Screen, /*FingerIndex=*/1));
	TestTrue(TEXT("finger 1 holds its capture"), Widget->HasMouseCaptureByUser(UserIndex, 1));

	// The mouse clicks btn2 while the finger is down: press, then an orderly release through the routed reply.
	Slate.RoutePointerDownEvent(WidgetPath, MakeMouseEvent(Button2Screen, LeftDown, EKeys::LeftMouseButton));
	Slate.RoutePointerUpEvent(WidgetPath, MakeMouseEvent(Button2Screen, NoButtons, EKeys::LeftMouseButton));
	if (!TestTrue(TEXT("UI frame ran after the mouse click"), RunFrames(*Rig.UIThread, 1)))
	{
		return false;
	}

	TestEqual(TEXT("the mouse click reached btn2: one click"), Rig.Host->NumClick, ClicksBeforeMouse + 1);
	TestEqual(TEXT("...and the click names btn2"), Rig.Host->LastClickId, FString(TEXT("btn2")));
	TestEqual(TEXT("the mouse release is the only mouseup: no finger was cancelled"),
		Rig.Host->NumMouseUp, MouseUpsBeforeMouse + 1);
	TestEqual(TEXT("finger 1 is still tracked: the click did not end it"), Widget->GetNumActiveTouches_Debug(), 1);
	TestTrue(TEXT("Slate still holds finger 1's pointer for the widget"), Widget->HasMouseCaptureByUser(UserIndex, 1));
	TestFalse(TEXT("the cursor pointer is released with the mouse"), Widget->HasMouseCaptureByUser(UserIndex, Cursor));

	// The finger keeps its press independently of the intervening mouse click.
	const int32 ClicksBeforeFinger = Rig.Host->NumClick;
	const int32 MouseUpsBeforeFinger = Rig.Host->NumMouseUp;
	Slate.RoutePointerUpEvent(WidgetPath, MakeTouchEvent(Finger1Screen, Finger1Screen, /*FingerIndex=*/1));
	if (!TestTrue(TEXT("UI frame ran after the finger's release"), RunFrames(*Rig.UIThread, 1)))
	{
		return false;
	}
	TestEqual(TEXT("finger 1's release reaches RmlUi as a mouseup: the finger was still ours to end"),
		Rig.Host->NumMouseUp, MouseUpsBeforeFinger + 1);
	TestEqual(TEXT("the held finger still clicks after the mouse click"), Rig.Host->NumClick, ClicksBeforeFinger + 1);
	TestEqual(TEXT("that click belongs to the finger's button"), Rig.Host->LastClickId, FString(TEXT("btn")));
	TestFalse(TEXT("Slate lets go of finger 1's pointer with the release"), Widget->HasMouseCaptureByUser(UserIndex, 1));
	TestEqual(TEXT("...and nothing is left outstanding"), Widget->GetNumActiveTouches_Debug(), 0);

	const FVector2D GapScreen = ToScreen(WidgetGeometry, GGapPoint);
	Slate.RoutePointerDownEvent(WidgetPath, MakeTouchEvent(GapScreen, GapScreen, 1));
	Slate.RoutePointerDownEvent(WidgetPath, MakeMouseEvent(Button2Screen, LeftDown, EKeys::LeftMouseButton));
	Slate.RoutePointerUpEvent(WidgetPath, MakeMouseEvent(Button2Screen, NoButtons, EKeys::LeftMouseButton));
	TestEqual(TEXT("an orderly mouse click also preserves a pass-through finger"), Widget->GetNumActiveTouches_Debug(), 1);
	Slate.RoutePointerUpEvent(WidgetPath, MakeTouchEvent(GapScreen, GapScreen, 1));
	TestEqual(TEXT("the pass-through finger can still finish"), Widget->GetNumActiveTouches_Debug(), 0);

	return true;
}

/** The frame goes through RmlUi and the production recorder; :drag is blue, sources are red. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusSecondaryCloneVisualTest, "VaCuus.Input.MultiTouchRouting.SecondaryCloneVisuals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusSecondaryCloneVisualTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchRoutingTest;
	FRig Rig;
	ON_SCOPE_EXIT { TearDown(Rig); };
	if (!BringUp(*this, Rig, true, true))
	{
		return false;
	}
	const FVector2D P1(250, 160), P2(310, 240);
	Rig.Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButtonPoint, GButtonPoint, 0));
	Rig.Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButton2Point, GButton2Point, 1));
	Rig.Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButton2Point, GButton2Point, 2));
	Rig.Widget->OnTouchMoved(Rig.Geometry, MakeTouchEvent(P1, GButton2Point, 1));
	Rig.Widget->OnTouchMoved(Rig.Geometry, MakeTouchEvent(P2, GButton2Point, 2));
	if (!TestTrue(TEXT("two clone drags were recorded"), RunFrames(*Rig.UIThread, 1)))
	{
		return false;
	}
	for (const FBox2D& B : Rig.Host->GhostBounds)
	{
		AddInfo(FString::Printf(TEXT("ghost bounds %.1f,%.1f .. %.1f,%.1f"), B.Min.X, B.Min.Y, B.Max.X, B.Max.Y));
	}
	TestEqual(TEXT("each secondary finger draws its own styled ghost"), Rig.Host->GhostBounds.Num(), 2);
	for (const FVector2D P : {P1, P2})
	{
		TestTrue(TEXT("a ghost keeps the press's grab offset under its own finger"),
			Rig.Host->GhostBounds.ContainsByPredicate([P](const FBox2D& B)
			{
				return B.Min.Equals(P - FVector2D(50, 20), 0.5) && B.Max.Equals(P + FVector2D(50, 20), 0.5);
			}));
	}
	Rig.View->PollStatus();
	TestFalse(TEXT("a ghost never becomes an interactive snapshot region"), Rig.View->GetSnapshot().Contains(FIntPoint(250, 160)));

	Rig.Widget->OnMouseCaptureLost(FCaptureLostEvent(0, 1));
	TestTrue(TEXT("cancel was recorded"), RunFrames(*Rig.UIThread, 1));
	TestEqual(TEXT("cancelling one drag releases only its ghost"), Rig.Host->GhostBounds.Num(), 1);
	Rig.Widget->OnTouchEnded(Rig.Geometry, MakeTouchEvent(P2, P2, 2));
	TestTrue(TEXT("release was recorded"), RunFrames(*Rig.UIThread, 1));
	TestEqual(TEXT("ending the other drag releases its ghost"), Rig.Host->GhostBounds.Num(), 0);
	Rig.Widget->OnTouchEnded(Rig.Geometry, MakeTouchEvent(GButtonPoint, GButtonPoint, 0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusSecondaryCloneLifecycleTest, "VaCuus.Input.MultiTouchRouting.SecondaryCloneLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusSecondaryCloneLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchRoutingTest;
	for (const FProbeHost::EAction Action : {FProbeHost::EAction::RemoveSecond, FProbeHost::EAction::Close})
	{
		FRig Rig;
		ON_SCOPE_EXIT { TearDown(Rig); };
		if (!BringUp(*this, Rig, true, true))
		{
			return false;
		}
		const FVector2D P(250, 160);
		Rig.Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButtonPoint, GButtonPoint, 0));
		Rig.Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButton2Point, GButton2Point, 1));
		Rig.Widget->OnTouchMoved(Rig.Geometry, MakeTouchEvent(P, GButton2Point, 1));
		TestTrue(TEXT("clone was recorded before removal"), RunFrames(*Rig.UIThread, 1));
		TestEqual(TEXT("control: a ghost exists"), Rig.Host->GhostBounds.Num(), 1);
		Rig.Host->Action.store(Action, std::memory_order_release);
		TestTrue(TEXT("removal was recorded"), RunFrames(*Rig.UIThread, 1));
		TestEqual(TEXT("source removal or document close releases its ghost"), Rig.Host->GhostBounds.Num(), 0);
		Rig.Widget->OnTouchEnded(Rig.Geometry, MakeTouchEvent(P, P, 1));
		Rig.Widget->OnTouchEnded(Rig.Geometry, MakeTouchEvent(GButtonPoint, GButtonPoint, 0));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusSecondaryClonePixelsTest, "VaCuus.Render.MultiTouch.SecondaryClonePixels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusSecondaryClonePixelsTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchRoutingTest;
	if (GUsingNullRHI)
	{
		AddInfo(TEXT("SKIPPED under NullRHI: clone pixels need a draw and GPU readback"));
		return true;
	}
	FRig Rig;
	ON_SCOPE_EXIT { TearDown(Rig); };
	if (!BringUp(*this, Rig, true, true))
	{
		return false;
	}
	const FVector2D P1(250, 160), P2(310, 240);
	Rig.Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButtonPoint, GButtonPoint, 0));
	Rig.Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButton2Point, GButton2Point, 1));
	Rig.Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButton2Point, GButton2Point, 2));
	Rig.Widget->OnTouchMoved(Rig.Geometry, MakeTouchEvent(P1, GButton2Point, 1));
	Rig.Widget->OnTouchMoved(Rig.Geometry, MakeTouchEvent(P2, GButton2Point, 2));
	if (!TestTrue(TEXT("clone frame was recorded"), RunFrames(*Rig.UIThread, 1)) ||
		!TestNotNull(TEXT("clone frame was published"), Rig.Host->ReplayFrame.Get()))
	{
		return false;
	}
	const TArray<FColor> Before = ReplayPixels(*Rig.Host->ReplayFrame);
	if (!TestEqual(TEXT("the clone frame read back"), Before.Num(), GViewSize.X * GViewSize.Y))
	{
		return false;
	}
	for (const FIntPoint P : {FIntPoint(250,160), FIntPoint(310,240)})
	{
		const FColor C = Before[P.Y * GViewSize.X + P.X];
		AddInfo(FString::Printf(TEXT("clone pixel %d,%d = (%d,%d,%d,%d)"), P.X, P.Y, C.R, C.G, C.B, C.A));
		TestTrue(TEXT("each finger has an opaque blue :drag copy"), C.B > 252 && C.R < 3 && C.G < 3 && C.A > 252);
	}
	const FColor Source = Before[120 * GViewSize.X + 70];
	TestTrue(TEXT("the original source is still red"), Source.R > 252 && Source.G < 3 && Source.B < 3 && Source.A > 252);
	Rig.Widget->OnMouseCaptureLost(FCaptureLostEvent(0, 1));
	TestTrue(TEXT("cancel frame was recorded"), RunFrames(*Rig.UIThread, 1));
	const TArray<FColor> After = ReplayPixels(*Rig.Host->ReplayFrame);
	if (!TestEqual(TEXT("the cancel frame read back"), After.Num(), GViewSize.X * GViewSize.Y))
	{
		return false;
	}
	TestEqual(TEXT("the cancelled finger's copy is gone"), int32(After[160 * GViewSize.X + 250].A), 0);
	TestTrue(TEXT("the other finger's copy is still blue"), After[240 * GViewSize.X + 310].B > 252);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusCloneRemovedDuringStartTest, "VaCuus.Input.MultiTouchRouting.CloneSourceRemovedDuringStart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusCloneRemovedDuringStartTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchRoutingTest;
	for (const bool bSecondary : {false, true})
	{
		FRig Rig;
		ON_SCOPE_EXIT { TearDown(Rig); };
		if (!BringUp(*this, Rig, true, true))
		{
			return false;
		}
		if (bSecondary)
		{
			Rig.Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButtonPoint, GButtonPoint, 0));
		}
		Rig.Host->RemoveOnDragStart.store(true, std::memory_order_release);
		Rig.Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButton2Point, GButton2Point, 1));
		Rig.Widget->OnTouchMoved(Rig.Geometry, MakeTouchEvent(FVector2D(250,160), GButton2Point, 1));
		TestTrue(TEXT("dragstart removal completed on the UI thread"), RunFrames(*Rig.UIThread, 1));
		TestEqual(TEXT("a removed source never leaves a ghost"), Rig.Host->GhostBounds.Num(), 0);
		Rig.View->PollStatus();
		TestFalse(TEXT("the source really was removed by its dragstart handler"), Rig.View->GetSnapshot().Contains(FIntPoint(70,120)));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusCloneRelativeImagesTest, "VaCuus.Render.MultiTouch.CloneRelativeImages",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusCloneRelativeImagesTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchRoutingTest;
	if (GUsingNullRHI)
	{
		AddInfo(TEXT("SKIPPED under NullRHI: image pixels need GPU readback"));
		return true;
	}
	FRig Rig;
	ON_SCOPE_EXIT { TearDown(Rig); };
	if (!BringUp(*this, Rig, true, true, true))
	{
		return false;
	}
	const FVector2D P(250,160);
	Rig.Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButtonPoint, GButtonPoint, 0));
	Rig.Widget->OnTouchStarted(Rig.Geometry, MakeTouchEvent(GButton2Point, GButton2Point, 1));
	Rig.Widget->OnTouchMoved(Rig.Geometry, MakeTouchEvent(P, GButton2Point, 1));
	if (!TestTrue(TEXT("image clone frame recorded"), RunFrames(*Rig.UIThread, 1)) ||
		!TestNotNull(TEXT("image clone frame published"), Rig.Host->ReplayFrame.Get()))
	{
		return false;
	}
	TestEqual(TEXT("image URLs in the clone retain the source document directory"), Rig.Host->BadImageRequests, 0);
	TestEqual(TEXT("the image has a visible clone parent"), Rig.Host->GhostBounds.Num(), 1);
	const TArray<FColor> Pixels = ReplayPixels(*Rig.Host->ReplayFrame);
	if (!TestEqual(TEXT("image frame read back"), Pixels.Num(), GViewSize.X * GViewSize.Y))
	{
		return false;
	}
	for (const FIntPoint Point : {FIntPoint(70,120), FIntPoint(250,160)})
	{
		const FColor C = Pixels[Point.Y * GViewSize.X + Point.X];
		AddInfo(FString::Printf(TEXT("image pixel %d,%d = (%d,%d,%d,%d)"), Point.X, Point.Y, C.R, C.G, C.B, C.A));
		TestTrue(TEXT("source and cloned image both render the green texture"), C.G > 252 && C.R < 3 && C.B < 3 && C.A > 252);
	}
	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
