// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "SVaCuusWidget.h"
#include "VaCuus.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusInteractiveSnapshot.h"
#include "VaCuusSlateElement.h"
#include "VaCuusSubsystem.h"
#include "Engine/GameInstance.h"
#include "VaCuusUIThread.h"
#include "VaCuusView.h"
#include "VaCuusViewStatus.h"

#include "HAL/PlatformProcess.h"
#include "InputCoreTypes.h"
#include "Input/Events.h"
#include "Layout/Geometry.h"
#include "UObject/StrongObjectPtr.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusSlateRoutingTest
{
/**
 * A document host that reports the RmlUi state the widget's events produced.
 *
 * Same shape as VaCuusInputRoutingTest's probe, plus the focused field's value as a
 * UTF-8 HEX STRING. Hex on purpose: the surrogate assertion below is about an exact
 * code point above the BMP, and comparing hex bytes cannot be confused by an
 * FString round-trip re-splitting the character back into a surrogate pair.
 *
 * THREAD HAND-OFF: the observations are plain members written on the UI thread and
 * read on the test thread. Sound because the test only reads them after
 * WaitForFrameCount() has seen the frame counter advance, and the UI thread stores
 * that counter with release ordering after RunFrame() returns.
 */
class FProbeHost final : public IVaCuusDocumentHost
{
public:
	virtual bool Initialize(uint32 InViewId, const TSharedRef<FVaCuusViewStatus>& InStatus) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		ViewId = InViewId;
		Status = InStatus;
		ContextName = FString::Printf(TEXT("vacuus_slate_routing_view_%u"), ViewId);

		Context = Rml::CreateContext(Rml::String(TCHAR_TO_UTF8(*ContextName)), Rml::Vector2i(1, 1));
		return Context != nullptr;
	}

	virtual void Shutdown() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		if (RmlDocument)
		{
			RmlDocument->Close();
			RmlDocument = nullptr;
		}
		if (Context)
		{
			Rml::RemoveContext(Rml::String(TCHAR_TO_UTF8(*ContextName)));
			Context = nullptr;
		}
	}

	virtual void SetViewSize(FIntPoint InViewSize) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		if (InViewSize == ViewSize || InViewSize.X <= 0 || InViewSize.Y <= 0)
		{
			return;
		}

		ViewSize = InViewSize;
		if (Context)
		{
			Context->SetDimensions(Rml::Vector2i(ViewSize.X, ViewSize.Y));
		}
	}

	virtual void LoadDocumentFromFile(const FString& VfsPath, uint64 LoadSerial) override
	{
		Report(LoadSerial, /*bSuccess=*/false);
	}

	virtual void LoadDocumentFromMemory(const FString& RmlSource, uint64 LoadSerial) override
	{
		check(FVaCuusUIThread::IsInUIThread());
		if (!Context)
		{
			Report(LoadSerial, /*bSuccess=*/false);
			return;
		}

		Rml::ElementDocument* NewDocument =
			Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(*RmlSource)), "vacuus://slate_routing.rml");
		if (!NewDocument)
		{
			Report(LoadSerial, /*bSuccess=*/false);
			return;
		}

		CloseDocument();
		RmlDocument = NewDocument;
		RmlDocument->Show();
		Report(LoadSerial, /*bSuccess=*/true);
	}

	virtual void CloseDocument() override
	{
		check(FVaCuusUIThread::IsInUIThread());
		if (RmlDocument)
		{
			RmlDocument->Close();
			RmlDocument = nullptr;
		}
	}

	virtual void SetVisible(bool bVisible) override
	{
		check(FVaCuusUIThread::IsInUIThread());
		if (RmlDocument)
		{
			bVisible ? RmlDocument->Show() : RmlDocument->Hide();
		}
	}

	virtual bool HasView() const override
	{
		check(FVaCuusUIThread::IsInUIThread());
		return Context != nullptr && RmlDocument != nullptr && ViewSize.X > 0 && ViewSize.Y > 0;
	}

	virtual Rml::Context* GetContext() const override
	{
		check(FVaCuusUIThread::IsInUIThread());
		return Context;
	}

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		Context->Update();

		FVaCuusInteractiveSnapshot& Snapshot = Status->GetSnapshotWriteBuffer();
		BuildVaCuusInteractiveSnapshot(*Context, ViewSize, ++SnapshotGeneration, Snapshot);
		Status->PublishSnapshot();

		Rml::Element* const Hover = Context->GetHoverElement();
		HoverId = Hover ? FString(UTF8_TO_TCHAR(Hover->GetId().c_str())) : FString();
		bHoverIsActive = Hover != nullptr && Hover->IsPseudoClassSet("active");

		Rml::Element* const Focus = Context->GetFocusElement();
		FocusId = Focus ? FString(UTF8_TO_TCHAR(Focus->GetId().c_str())) : FString();

		FieldValueUtf8Hex.Reset();
		if (RmlDocument)
		{
			if (Rml::Element* Field = RmlDocument->GetElementById("field"))
			{
				if (Rml::ElementFormControl* Control = rmlui_dynamic_cast<Rml::ElementFormControl*>(Field))
				{
					const Rml::String Value = Control->GetValue();
					for (const char Byte : Value)
					{
						FieldValueUtf8Hex += FString::Printf(TEXT("%02X"), uint8(Byte));
					}
				}
			}
		}

		Status->FramesPublished.fetch_add(1, std::memory_order_release);
	}

	//~ Post-frame observations; see the class comment for why plain members are safe.
	FString HoverId;
	FString FocusId;
	FString FieldValueUtf8Hex;
	bool bHoverIsActive = false;

private:
	void Report(uint64 LoadSerial, bool bSuccess)
	{
		if (Status.IsValid() && LoadSerial != 0)
		{
			Status->LoadResult.store(
				static_cast<uint8>(bSuccess ? EVaCuusLoadResult::Succeeded : EVaCuusLoadResult::Failed),
				std::memory_order_relaxed);
			Status->LoadCompletedSerial.store(LoadSerial, std::memory_order_release);
		}
	}

	TSharedPtr<FVaCuusViewStatus> Status;
	FString ContextName;
	Rml::Context* Context = nullptr;
	Rml::ElementDocument* RmlDocument = nullptr;
	FIntPoint ViewSize = FIntPoint::ZeroValue;
	uint32 ViewId = 0;
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

/** A button to press and a text field to type into. 400x300 view. */
static const TCHAR* GDocument = TEXT(R"(<rml>
<head>
<style>
body { display: block; width: 100%; height: 100%; }
div, button { display: block; position: absolute; }
#btn   { left: 20px;  top: 20px;  width: 100px; height: 40px; tab-index: auto; }
#field { position: absolute; left: 200px; top: 150px; width: 150px; height: 30px; }
</style>
</head>
<body>
	<button id="btn"/>
	<input id="field" type="text"/>
</body>
</rml>)");

static const FVector2D GButtonPoint(70.0, 40.0);
static const FVector2D GFieldPoint(260.0, 165.0);

/** Somewhere the document has nothing: the pass-through case. */
static const FVector2D GEmptyPoint(350.0, 280.0);

/**
 * A pointer event as FSlateApplication would build it.
 *
 * PressedButtons is the caller's business precisely because the engine's own
 * semantics differ between down and up: FSlateApplication::OnMouseUp REMOVES the
 * released button before constructing the event (SlateApplication.cpp:6098-6106), so
 * an up event's set is the post-release state. Passing it explicitly is what lets the
 * test reproduce a two-button release faithfully instead of assuming.
 */
static FPointerEvent MakePointerEvent(const FVector2D& Position, const TSet<FKey>& PressedButtons,
	const FKey& EffectingButton, float WheelDelta = 0.0f)
{
	return FPointerEvent(
		FSlateApplicationBase::CursorPointerIndex,
		Position,
		Position,
		PressedButtons,
		EffectingButton,
		WheelDelta,
		FModifierKeysState());
}
}	 // namespace VaCuusSlateRoutingTest

/**
 * The SLATE half of input routing: SVaCuusWidget's own handlers, their FReply
 * bookkeeping, and the chain from a widget handler all the way into Rml::Context.
 *
 * WHY THIS EXISTS SEPARATELY FROM VaCuus.Input.Routing: that test pushes events onto
 * the UI thread's queue directly, so it never executes a single line of
 * SVaCuusWidget. That is exactly how the capture bug this test now guards got in --
 * capture was released when the second-to-last button came up, because the code
 * assumed GetPressedButtons() still contained the button being released.
 *
 * WHAT IS AND IS NOT SYNTHETIC: the FPointerEvents, FCharacterEvents and FGeometry
 * are real engine types carrying the values FSlateApplication itself would supply
 * (see MakePointerEvent), and the widget, view, queue, UI thread, RmlUi context and
 * document are all real. What is NOT exercised is FSlateApplication's hit-test grid
 * and bubble routing -- the handlers are invoked directly rather than through a live
 * window, because a headless automation run has no window to hit-test against. That
 * routing is covered instead by the in-game verification (vacuus.M1HUD.Mouse drives
 * FSlateApplication::ProcessMouseMoveEvent for real).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusSlateRoutingTest, "VaCuus.Input.SlateRouting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusSlateRoutingTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusSlateRoutingTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no worker thread to drive"));
		return true;
	}

	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	FVaCuusModule& Module = FVaCuusModule::Get();
	FVaCuusUIThread* UIThread = Module.GetOrStartUIThread();
	if (!TestNotNull(TEXT("UI thread"), UIThread))
	{
		return false;
	}

	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	const FIntPoint ViewSize(400, 300);
	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();

	TUniquePtr<FProbeHost> OwnedHost = MakeUnique<FProbeHost>();
	FProbeHost* Host = OwnedHost.Get();

	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), ViewSize, Status);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GDocument, /*LoadSerial=*/1);

	// A view handle wired by hand rather than through UVaCuusSubsystem::CreateView():
	// the subsystem reaches the UI thread only through the module
	// (UVaCuusSubsystem::GetUIThread asks FVaCuusModule), so the handle works without a
	// PIE world -- but the subsystem still needs a UGameInstance outer, because
	// UGameInstanceSubsystem declares ClassWithin=UGameInstance and NewObject ensures on
	// a mismatched outer. A bare game instance is enough; it is never initialized.
	// Strong pointers because nothing else roots any of these.
	TStrongObjectPtr<UGameInstance> GameInstance(NewObject<UGameInstance>());
	TStrongObjectPtr<UVaCuusSubsystem> Subsystem(NewObject<UVaCuusSubsystem>(GameInstance.Get()));
	TStrongObjectPtr<UVaCuusView> View(NewObject<UVaCuusView>(Subsystem.Get()));
	View->InitializeView(Subsystem.Get(), ViewId, Status, ViewSize);

	const TSharedRef<FVaCuusSlateElement> Element = MakeShared<FVaCuusSlateElement>();
	TSharedRef<SVaCuusWidget> Widget = SNew(SVaCuusWidget, View.Get(), Element);

	// Absolute origin (0,0) and scale 1, so screen-space positions ARE view pixels and
	// the coordinates below can be read against the document's own RCSS.
	const FGeometry Geometry = FGeometry::MakeRoot(FVector2f(ViewSize.X, ViewSize.Y), FSlateLayoutTransform());

	if (!TestTrue(TEXT("UI frames ran"), RunFrames(*UIThread, 2)))
	{
		return false;
	}
	if (!TestTrue(TEXT("Document loaded"),
			Status->LoadCompletedSerial.load(std::memory_order_acquire) == 1 &&
				Status->LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded)))
	{
		return false;
	}

	// The widget answers from the view's CACHED snapshot, which only PollStatus()
	// refreshes -- the same once-per-frame call UVaCuusSubsystem::Tick makes.
	View->PollStatus();
	if (!TestTrue(TEXT("The cached snapshot covers the button"), View->GetSnapshot().Contains(FIntPoint(70, 40))))
	{
		return false;
	}

	const TSet<FKey> NoButtons;
	const TSet<FKey> LeftOnly = {EKeys::LeftMouseButton};
	const TSet<FKey> RightOnly = {EKeys::RightMouseButton};
	const TSet<FKey> LeftAndRight = {EKeys::LeftMouseButton, EKeys::RightMouseButton};

	// 1. Pass-through: a press where the snapshot reports nothing must be Unhandled,
	// or it never reaches the game, and it must not take capture either.
	{
		const FReply Reply = Widget->OnMouseButtonDown(
			Geometry, MakePointerEvent(GEmptyPoint, LeftOnly, EKeys::LeftMouseButton));
		TestFalse(TEXT("A press outside every interactive rect is Unhandled"), Reply.IsEventHandled());
		TestFalse(TEXT("A pass-through press takes no capture"), Reply.GetMouseCaptor().IsValid());
		TestFalse(TEXT("A pass-through press does not start tracking capture"),
			Widget->IsTrackingMouseCapture_Debug());

		// And the matching release must fall through too: swallowing it would leave the
		// game holding a button down forever.
		const FReply UpReply = Widget->OnMouseButtonUp(
			Geometry, MakePointerEvent(GEmptyPoint, NoButtons, EKeys::LeftMouseButton));
		TestFalse(TEXT("The matching pass-through release is Unhandled"), UpReply.IsEventHandled());
	}

	// 2. THE REGRESSION TEST for the capture bug: Left down, Right down, Left up
	// (still holding Right), Right up.
	{
		const FReply LeftDown = Widget->OnMouseButtonDown(
			Geometry, MakePointerEvent(GButtonPoint, LeftOnly, EKeys::LeftMouseButton));
		TestTrue(TEXT("A press on the button is Handled"), LeftDown.IsEventHandled());
		TestEqual(TEXT("The press captures the mouse to this widget"),
			LeftDown.GetMouseCaptor().Get(), static_cast<SWidget*>(&Widget.Get()));
		TestTrue(TEXT("The widget is tracking capture"), Widget->IsTrackingMouseCapture_Debug());

		// Second button while the first is held: already captured, so no new request.
		const FReply RightDown = Widget->OnMouseButtonDown(
			Geometry, MakePointerEvent(GButtonPoint, LeftAndRight, EKeys::RightMouseButton));
		TestTrue(TEXT("A second press is Handled"), RightDown.IsEventHandled());
		TestFalse(TEXT("A second press does not re-request capture"), RightDown.GetMouseCaptor().IsValid());
		TestTrue(TEXT("The widget still tracks capture"), Widget->IsTrackingMouseCapture_Debug());

		// Releasing Left leaves {Right} in the POST-release set, which is what
		// FSlateApplication::OnMouseUp hands us (SlateApplication.cpp:6098-6106). Capture
		// must survive: this is the exact case a `Num() <= 1` test gets wrong.
		const FReply LeftUp = Widget->OnMouseButtonUp(
			Geometry, MakePointerEvent(GButtonPoint, RightOnly, EKeys::LeftMouseButton));
		TestTrue(TEXT("Releasing one of two buttons is Handled"), LeftUp.IsEventHandled());
		TestFalse(TEXT("Releasing one of two buttons does NOT release capture"), LeftUp.ShouldReleaseMouse());
		TestTrue(TEXT("The widget still tracks capture with a button still down"),
			Widget->IsTrackingMouseCapture_Debug());

		// The last button up: now capture goes back.
		const FReply RightUp = Widget->OnMouseButtonUp(
			Geometry, MakePointerEvent(GButtonPoint, NoButtons, EKeys::RightMouseButton));
		TestTrue(TEXT("Releasing the last button is Handled"), RightUp.IsEventHandled());
		TestTrue(TEXT("Releasing the last button releases capture"), RightUp.ShouldReleaseMouse());
		TestFalse(TEXT("The widget stops tracking capture"), Widget->IsTrackingMouseCapture_Debug());
	}

	// 3. Those presses were real events on a real queue, so RmlUi must have seen them.
	if (!TestTrue(TEXT("UI frame ran after the presses"), RunFrames(*UIThread, 1)))
	{
		return false;
	}
	TestEqual(TEXT("The widget's presses reached RmlUi and focused the button"),
		Host->FocusId, FString(TEXT("btn")));
	TestEqual(TEXT("The widget's move reached RmlUi and hovered the button"),
		Host->HoverId, FString(TEXT("btn")));

	// 4. Capture lost without a button-up (window deactivation, another captor) must
	// clear the flag, or every later move answers Handled and eats the game's camera.
	{
		Widget->OnMouseButtonDown(Geometry, MakePointerEvent(GButtonPoint, LeftOnly, EKeys::LeftMouseButton));
		TestTrue(TEXT("Tracking capture again"), Widget->IsTrackingMouseCapture_Debug());

		Widget->OnMouseCaptureLost(FCaptureLostEvent(0, FSlateApplicationBase::CursorPointerIndex));
		TestFalse(TEXT("Losing capture clears the tracking flag"), Widget->IsTrackingMouseCapture_Debug());

		// And a move over a pass-through point is Unhandled again rather than stuck.
		const FReply Move = Widget->OnMouseMove(Geometry, MakePointerEvent(GEmptyPoint, NoButtons, FKey()));
		TestFalse(TEXT("After losing capture a pass-through move is Unhandled"), Move.IsEventHandled());
	}

	// 5. Typing, and specifically the UTF-16 surrogate pair. Focus the field with a
	// real press pair first.
	Widget->OnMouseButtonDown(Geometry, MakePointerEvent(GFieldPoint, LeftOnly, EKeys::LeftMouseButton));
	Widget->OnMouseButtonUp(Geometry, MakePointerEvent(GFieldPoint, NoButtons, EKeys::LeftMouseButton));
	if (!TestTrue(TEXT("UI frame ran after clicking the field"), RunFrames(*UIThread, 1)))
	{
		return false;
	}
	if (!TestEqual(TEXT("Clicking the field focuses it"), Host->FocusId, FString(TEXT("field"))))
	{
		return false;
	}

	{
		// U+1F600 GRINNING FACE arrives as two OnKeyChar calls, because TCHAR is
		// char16_t here (PLATFORM_TCHAR_IS_CHAR16 for Unix). Only the second one may
		// produce a character, and it must be the joined code point -- not two
		// half-characters, and not a dropped one.
		const FCharacterEvent High(TCHAR(0xD83D), FModifierKeysState(), /*UserIndex=*/0, /*bIsRepeat=*/false);
		const FCharacterEvent Low(TCHAR(0xDE00), FModifierKeysState(), /*UserIndex=*/0, /*bIsRepeat=*/false);

		Widget->OnKeyChar(Geometry, High);
		if (!TestTrue(TEXT("UI frame ran after the high surrogate"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		TestEqual(TEXT("A high surrogate alone produces no text"), Host->FieldValueUtf8Hex, FString());

		Widget->OnKeyChar(Geometry, Low);
		if (!TestTrue(TEXT("UI frame ran after the low surrogate"), RunFrames(*UIThread, 1)))
		{
			return false;
		}

		// U+1F600 in UTF-8 is F0 9F 98 80. Any other value means the pair was joined
		// wrongly, sent twice, or truncated to a single UTF-16 unit.
		AddInfo(FString::Printf(TEXT("Field value after the surrogate pair: %s"), *Host->FieldValueUtf8Hex));
		TestEqual(TEXT("The surrogate pair became one U+1F600 (UTF-8 F09F9880)"),
			Host->FieldValueUtf8Hex, FString(TEXT("F09F9880")));
	}

	{
		// A plain BMP non-ASCII character needs no joining and must not be mangled:
		// U+00E9 LATIN SMALL LETTER E WITH ACUTE is C3 A9 in UTF-8. This is also the
		// case ProcessTextInput(char) would have silently dropped (>127).
		const FCharacterEvent Accented(TCHAR(0x00E9), FModifierKeysState(), /*UserIndex=*/0, /*bIsRepeat=*/false);
		Widget->OnKeyChar(Geometry, Accented);
		if (!TestTrue(TEXT("UI frame ran after the accented character"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		TestEqual(TEXT("A BMP non-ASCII character appends as UTF-8"),
			Host->FieldValueUtf8Hex, FString(TEXT("F09F9880C3A9")));
	}

	{
		// Control characters are dropped: Slate delivers Backspace and friends through
		// OnKeyChar too, and a document's text field must not receive them as text.
		const FCharacterEvent Backspace(TCHAR('\b'), FModifierKeysState(), /*UserIndex=*/0, /*bIsRepeat=*/false);
		Widget->OnKeyChar(Geometry, Backspace);
		if (!TestTrue(TEXT("UI frame ran after the control character"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		TestEqual(TEXT("A C0 control character produces no text"),
			Host->FieldValueUtf8Hex, FString(TEXT("F09F9880C3A9")));
	}

	// 6. Detaching the view makes every handler fall through, which is what a widget
	// outliving its view by a frame during teardown must do.
	Widget->DetachView();
	{
		const FReply Reply = Widget->OnMouseButtonDown(
			Geometry, MakePointerEvent(GButtonPoint, LeftOnly, EKeys::LeftMouseButton));
		TestFalse(TEXT("A detached widget answers Unhandled"), Reply.IsEventHandled());
	}

	UIThread->EnqueueRemoveView(ViewId);
	TestTrue(TEXT("UI frames survive the removal"), RunFrames(*UIThread, 2));

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
