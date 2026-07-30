// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "SVaCuusWidget.h"
#include "VaCuus.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusInputEvent.h"
#include "VaCuusInteractiveSnapshot.h"
#include "VaCuusSlateElement.h"
#include "VaCuusSubsystem.h"
#include "VaCuusUIThread.h"
#include "VaCuusView.h"
#include "VaCuusViewStatus.h"

#include "Engine/GameInstance.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/ITextInputMethodSystem.h"
#include "HAL/PlatformProcess.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Layout/Geometry.h"
#include "UObject/StrongObjectPtr.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusTextEntryTest
{
/**
 * A document host that reports the focused field's live value and id.
 *
 * Same shape and same thread hand-off as VaCuusInputRoutingTest's and
 * VaCuusSlateRoutingTest's probes: plain members written on the UI thread, read on the
 * test thread only after WaitForFrameCount() has seen the frame counter advance (which
 * the UI thread stores with release ordering after RunFrame() returns).
 *
 * It builds the real snapshot -- BuildVaCuusInteractiveSnapshot -- because that is what
 * publishes both halves of controller decision D14 and the whole of D15's shadow state.
 * A hand-written snapshot would test nothing.
 */
class FProbeHost final : public IVaCuusDocumentHost
{
public:
	virtual bool Initialize(uint32 InViewId, const TSharedRef<FVaCuusViewStatus>& InStatus) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		ViewId = InViewId;
		Status = InStatus;
		ContextName = FString::Printf(TEXT("vacuus_text_entry_view_%u"), ViewId);

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
		// Not exercised: this test only ever loads from memory.
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
			Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(*RmlSource)), "vacuus://text_entry.rml");
		if (!NewDocument)
		{
			Report(LoadSerial, /*bSuccess=*/false);
			return;
		}

		CloseDocument();
		RmlDocument = NewDocument;

		// FocusFlag::Document, exactly like the production host: without it nothing inside the
		// document can ever be reached by a key, and controller decision D9 relies on a
		// document-only focus not counting as "the UI wants the keyboard".
		RmlDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document);
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

		Rml::Element* const Focus = Context->GetFocusElement();
		FocusId = Focus ? FString(UTF8_TO_TCHAR(Focus->GetId().c_str())) : FString();

		FieldValue.Reset();
		if (RmlDocument)
		{
			if (Rml::Element* Field = RmlDocument->GetElementById("field"))
			{
				if (Rml::ElementFormControl* Control = rmlui_dynamic_cast<Rml::ElementFormControl*>(Field))
				{
					FieldValue = UTF8_TO_TCHAR(Control->GetValue().c_str());
				}
			}
		}

		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

	//~ Post-frame observations; see the class comment for why plain members are safe.
	FString FocusId;
	FString FieldValue;

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

/**
 * 400x300. Four elements chosen so the D14a flag has something to be right AND wrong about:
 *
 *   #field (200,150)-(350,180)  <input type="text">      -> TextInput AND Focusable
 *   #bare  (200,200)-(350,230)  <input> with no `type`   -> TextInput (RmlUi defaults to text)
 *   #check (200,250)-(230,280)  <input type="checkbox">  -> Focusable, NOT TextInput
 *   #btn   (20,20)-(120,60)     <button>                 -> Focusable, NOT TextInput
 *
 * #bare and #check are the two halves of the negative-list rule the flag is built on: RmlUi
 * instances InputTypeText for anything it does not recognise, so a bare <input> IS a text field,
 * while the five named non-text types are not (ElementFormControlInput.cpp:95-118). Getting that
 * backwards would either miss every plain <input> or open an IME over a checkbox.
 */
static const TCHAR* GDocument = TEXT(R"(<rml>
<head>
<style>
/* The font is named so the caret RmlUi reports actually tracks the glyphs: with no
   font-family every string measures zero wide and the caret never leaves the field's
   origin, which would make the caret assertions vacuous. It degrades safely -- a project
   without the face still lays out, the caret just stays put. */
body { display: block; width: 100%; height: 100%; font-family: LatoLatin; font-size: 14px; }
div, button { display: block; position: absolute; }
#btn   { left: 20px;  top: 20px;  width: 100px; height: 40px; tab-index: auto; }
#field { position: absolute; left: 200px; top: 150px; width: 150px; height: 30px; }
#bare  { position: absolute; left: 200px; top: 200px; width: 150px; height: 30px; }
#check { position: absolute; left: 200px; top: 250px; width: 30px;  height: 30px; }
</style>
</head>
<body>
	<button id="btn"/>
	<input id="field" type="text"/>
	<input id="bare"/>
	<input id="check" type="checkbox"/>
</body>
</rml>)");

static const FIntPoint GFieldPoint(260, 165);
static const FIntPoint GBarePoint(260, 215);
static const FIntPoint GCheckPoint(210, 265);
static const FIntPoint GButtonPoint(70, 40);

/** A pointer event as FSlateApplication would build one. */
static FPointerEvent MakePointerEvent(FIntPoint Position, const TSet<FKey>& PressedButtons, const FKey& EffectingButton)
{
	const FVector2D Screen(Position.X, Position.Y);
	return FPointerEvent(FSlateApplicationBase::CursorPointerIndex, Screen, Screen, PressedButtons, EffectingButton,
		/*WheelDelta=*/0.0f, FModifierKeysState());
}

/** A character event as Slate delivers one per UTF-16 unit. */
static FCharacterEvent MakeCharacterEvent(TCHAR Character)
{
	return FCharacterEvent(Character, FModifierKeysState(), /*UserIndex=*/0, /*bIsRepeat=*/false);
}
}	 // namespace VaCuusTextEntryTest

/**
 * TEXT ENTRY AND THE IME SEAM, end to end and through the real queue (Task 9).
 *
 * WHAT THIS PROVES, and it is deliberately two different things:
 *
 *  1. THE DEGRADED PATH IS THE TESTED PATH (controller decision D16). Typing goes
 *     SVaCuusWidget::OnKeyChar -> EVaCuusInputEventKind::TextInput -> the shared input queue ->
 *     Rml::Context::ProcessTextInput -> the element's value, with no platform IME involved at
 *     all. That is the only text path that exists on Linux, where FLinuxApplication never
 *     overrides GenericApplication::GetTextInputMethodSystem() -- Epic's own CEF IME handler is
 *     compiled out here for the same reason (CEFImeHandler.h:7). The absence is asserted, not
 *     assumed, and logged.
 *
 *  2. THE FULL PATH IS BUILT AND CORRECT ANYWAY. The game-thread half -- the shadow state, the
 *     14 ITextInputMethodContext answers, the index-space conversions, the generation-stamped
 *     mutation queue and the composition round trip -- is exercised here even though nothing
 *     platform-side will call it on this machine, because the context is deliberately built
 *     with or without an ITextInputMethodSystem (only REGISTRATION needs one). Windows
 *     validation of the platform hand-off itself is an M6 matrix item; what cannot be faked
 *     here is a TSF environment, and this test does not pretend to.
 *
 * WHAT IS SYNTHETIC: the FPointerEvents, FCharacterEvents and FGeometry are real engine types
 * carrying the values FSlateApplication itself supplies, and the widget, view, queue, UI thread,
 * RmlUi context and document are all real. What is NOT exercised is FSlateApplication's
 * hit-test grid -- a headless run has no window -- so the handlers are invoked directly, exactly
 * as VaCuus.Input.SlateRouting does. The live half is covered by vacuus.M1HUD.TypeShot.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusTextEntryTest, "VaCuus.Input.TextEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusTextEntryTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusTextEntryTest;

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

	// A view handle wired by hand, same as VaCuus.Input.SlateRouting: the handle reaches the UI
	// thread through FVaCuusModule, so no PIE world is needed -- but UVaCuusSubsystem declares
	// ClassWithin=UGameInstance, so it still needs a (never-initialized) game instance outer.
	TStrongObjectPtr<UGameInstance> GameInstance(NewObject<UGameInstance>());
	TStrongObjectPtr<UVaCuusSubsystem> Subsystem(NewObject<UVaCuusSubsystem>(GameInstance.Get()));
	TStrongObjectPtr<UVaCuusView> View(NewObject<UVaCuusView>(Subsystem.Get()));
	View->InitializeView(Subsystem.Get(), ViewId, Status, ViewSize);

	const TSharedRef<FVaCuusSlateElement> Element = MakeShared<FVaCuusSlateElement>();
	TSharedRef<SVaCuusWidget> Widget = SNew(SVaCuusWidget, View.Get(), Element);

	ON_SCOPE_EXIT
	{
		// D18 in the test's own teardown: the widget hands the IME context back before anything
		// else drops it, exactly as the production teardown sites do.
		Widget->DetachView();
	};

	// Absolute origin (0,0) and scale 1, so screen-space positions ARE view pixels and every
	// coordinate below can be read straight against the document's RCSS. It also makes the
	// view-pixel -> Slate-absolute mapping the identity, which is what lets the caret rect
	// assertions below be about the value RmlUi reported rather than about a transform.
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

	View->PollStatus();

	// 1. CONTROLLER DECISION D14a -- THE `TextInput` RECT FLAG. The whole point of a separate
	// bit: everything here is Focusable, and only the text fields take text.
	{
		const FVaCuusInteractiveSnapshot& Snapshot = View->GetSnapshot();

		if (!TestTrue(TEXT("The <input type=text> rect is published"), Snapshot.Contains(GFieldPoint)))
		{
			return false;
		}

		TestTrue(TEXT("<input type=text> carries the TextInput flag"), Snapshot.IsTextInputAt(GFieldPoint));
		TestTrue(TEXT("A bare <input> with no type carries it too (RmlUi defaults to text)"),
			Snapshot.IsTextInputAt(GBarePoint));

		TestTrue(TEXT("<button> is focusable"), Snapshot.IsFocusableAt(GButtonPoint));
		TestFalse(TEXT("...but does NOT carry the TextInput flag"), Snapshot.IsTextInputAt(GButtonPoint));

		TestTrue(TEXT("<input type=checkbox> is reported interactive"), Snapshot.Contains(GCheckPoint));
		TestFalse(TEXT("...and does NOT carry the TextInput flag"), Snapshot.IsTextInputAt(GCheckPoint));

		// D14b's negative half: a document-only focus is not a focused text control.
		TestFalse(TEXT("No text control is focused before any click"), Snapshot.bTextInputFocused);
		TestEqual(TEXT("...so the shadow field state is empty"), int64(Snapshot.TextField.Generation), int64(0));
	}

	// 2. CONTROLLER DECISION D16 -- THE PLATFORM IME IS ABSENT HERE, asserted and logged. The
	// first Tick is what publishes the surface and therefore builds the handler.
	Widget->Tick(Geometry, /*InCurrentTime=*/0.0, /*InDeltaTime=*/0.0f);

	const UVaCuusView::FImeStatus ImeStatus = View->GetImeStatus();
	if (!TestTrue(TEXT("The first Tick built the IME bridge"), ImeStatus.bHandlerBuilt))
	{
		return false;
	}

	const bool bPlatformImeAbsent = ImeStatus.bPlatformImeAbsent;
	AddInfo(FString::Printf(
		TEXT("Platform ITextInputMethodSystem: %s. FSlateApplication::GetTextInputMethodSystem() returns %s, so the ")
		TEXT("tested text path is OnKeyChar -> ProcessTextInput and composition is unavailable."),
		bPlatformImeAbsent ? TEXT("ABSENT") : TEXT("present"),
		bPlatformImeAbsent ? TEXT("null") : TEXT("a system")));

	// Asserted against Slate directly rather than trusted: the handler's answer must be the
	// platform's answer, not its own idea of it.
	TestEqual(TEXT("The handler's platform verdict matches FSlateApplication"), bPlatformImeAbsent,
		FSlateApplication::Get().GetTextInputMethodSystem() == nullptr);
	TestEqual(TEXT("A context is only REGISTERED where a platform system exists"), ImeStatus.bRegistered,
		!bPlatformImeAbsent);

	// The context exists either way -- that is what makes the rest of this test possible on a
	// platform with no IME (see UVaCuusView::GetImeContextForTesting).
	ITextInputMethodContext* const ImeContext = View->GetImeContextForTesting();
	if (!TestNotNull(TEXT("The IME context is built even with no platform IME system"), ImeContext))
	{
		return false;
	}
	TestFalse(TEXT("IsComposing() is false outside a composition"), ImeContext->IsComposing());

	// 3. TYPING THROUGH THE REAL PATH: click the field, then OnKeyChar. Both go through the
	// widget's own handlers and the shared input queue -- nothing is injected downstream.
	{
		const TSet<FKey> LeftOnly = {EKeys::LeftMouseButton};
		const TSet<FKey> NoButtons;

		const FReply Down =
			Widget->OnMouseButtonDown(Geometry, MakePointerEvent(GFieldPoint, LeftOnly, EKeys::LeftMouseButton));
		TestTrue(TEXT("A press on the field is Handled"), Down.IsEventHandled());
		TestTrue(TEXT("...and requests Slate focus (D11)"), Down.ShouldSetUserFocus());
		Widget->OnMouseButtonUp(Geometry, MakePointerEvent(GFieldPoint, NoButtons, EKeys::LeftMouseButton));

		if (!TestTrue(TEXT("UI frame ran after the click"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		if (!TestEqual(TEXT("Clicking the field focuses it in RmlUi"), Host->FocusId, FString(TEXT("field"))))
		{
			return false;
		}

		// The widget needs the cached snapshot to say the UI wants the keyboard before OnKeyChar
		// will answer Handled; the value still reaches the field either way, because OnKeyChar
		// queues unconditionally (the FReply is the only thing the snapshot decides).
		View->PollStatus();

		const FReply CharReply = Widget->OnKeyChar(Geometry, MakeCharacterEvent(TCHAR('V')));
		TestTrue(TEXT("OnKeyChar is Handled while a focusable element holds RmlUi focus"), CharReply.IsEventHandled());
		Widget->OnKeyChar(Geometry, MakeCharacterEvent(TCHAR('a')));

		if (!TestTrue(TEXT("UI frame ran after typing"), RunFrames(*UIThread, 1)))
		{
			return false;
		}

		// THE DEGRADATION, PROVEN: two OnKeyChar calls and no platform IME anywhere in the chain
		// put two characters in the element.
		TestEqual(TEXT("Typing through OnKeyChar changes the element's value"), Host->FieldValue, FString(TEXT("Va")));
	}

	// 4. THE PUBLISHED SHADOW STATE (D15) after those edits.
	View->PollStatus();
	uint64 FieldGeneration = 0;
	{
		const FVaCuusInteractiveSnapshot& Snapshot = View->GetSnapshot();

		if (!TestTrue(TEXT("A focused text control is reported (D14b)"), Snapshot.bTextInputFocused))
		{
			return false;
		}

		const FVaCuusTextFieldState& Field = Snapshot.TextField;
		FieldGeneration = Field.Generation;

		TestNotEqual(TEXT("The field state carries a non-zero generation"), int64(Field.Generation), int64(0));
		TestEqual(TEXT("The shadow value is what was typed"), Field.Value, FString(TEXT("Va")));
		TestEqual(TEXT("The caret sits after the last character"), Field.SelectionBegin, 2);
		TestEqual(TEXT("...with nothing selected"), Field.SelectionEnd, 2);
		TestEqual(TEXT("Nothing is being composed"), Field.CompositionBegin, 0);
		TestEqual(TEXT("...at either end"), Field.CompositionEnd, 0);
		TestFalse(TEXT("The field is not read-only"), Field.bReadOnly);

		// THE CARET CAME FROM SystemInterface::ActivateKeyboard, which is the only per-caret
		// signal RmlUi offers -- Rml::TextInputContext::GetBoundingBox is the element's whole
		// border box. If FVaCuusSystemInterface stopped overriding it, this is what would fail.
		TestTrue(TEXT("A caret position was latched from ActivateKeyboard"), Field.bCaretValid);
		TestTrue(TEXT("...with a positive line height"), Field.CaretLineHeight > 0.0f);
		TestTrue(TEXT("...inside the field's own box"), Field.BoundingBox.Contains(FIntPoint(
			FMath::FloorToInt(Field.CaretPosition.X), FMath::FloorToInt(Field.CaretPosition.Y))));

		AddInfo(FString::Printf(TEXT("Caret latched at view px (%.1f, %.1f), line height %.1f; field box %s"),
			Field.CaretPosition.X, Field.CaretPosition.Y, Field.CaretLineHeight, *Field.BoundingBox.ToString()));
	}

	// 5. THE 14 VIRTUALS, ANSWERED FROM THAT SHADOW -- synchronously, on this thread, with the
	// UI thread parked. Nothing here may block, and nothing here calls RmlUi.
	{
		TestEqual(TEXT("GetTextLength answers from the shadow"), int64(ImeContext->GetTextLength()), int64(2));

		FString InRange;
		ImeContext->GetTextInRange(0, 2, InRange);
		TestEqual(TEXT("GetTextInRange returns the whole value"), InRange, FString(TEXT("Va")));

		ImeContext->GetTextInRange(1, 1, InRange);
		TestEqual(TEXT("...and a sub-range"), InRange, FString(TEXT("a")));

		// Out of range is clamped rather than crashing: TSF asks about indices that were valid
		// one frame ago, and the shadow is by construction one frame old.
		ImeContext->GetTextInRange(5, 10, InRange);
		TestEqual(TEXT("An out-of-range request is clamped to empty"), InRange, FString());

		uint32 Begin = 0;
		uint32 Length = 0;
		ITextInputMethodContext::ECaretPosition CaretPosition = ITextInputMethodContext::ECaretPosition::Beginning;
		ImeContext->GetSelectionRange(Begin, Length, CaretPosition);
		TestEqual(TEXT("GetSelectionRange reports the caret index"), int64(Begin), int64(2));
		TestEqual(TEXT("...with a zero-length selection"), int64(Length), int64(0));
		TestTrue(TEXT("...and the caret at the ending"), CaretPosition == ITextInputMethodContext::ECaretPosition::Ending);

		TestFalse(TEXT("IsReadOnly is false for a plain input"), ImeContext->IsReadOnly());

		// THE ONE VIRTUAL THAT CANNOT BE ANSWERED HONESTLY, asserted so the code and its own
		// documentation cannot drift: RmlUi has no public point -> character-index hit test at
		// 0ae381e, and INDEX_NONE is the interface's documented "none found".
		TestEqual(TEXT("GetCharacterIndexFromPoint degrades to INDEX_NONE"),
			ImeContext->GetCharacterIndexFromPoint(FVector2D(260.0, 165.0)), int32(INDEX_NONE));

		// GetScreenBounds is the widget's absolute rect, and with the identity geometry above
		// that is the whole 400x300 view.
		FVector2D ScreenPosition = FVector2D::ZeroVector;
		FVector2D ScreenSize = FVector2D::ZeroVector;
		ImeContext->GetScreenBounds(ScreenPosition, ScreenSize);
		TestEqual(TEXT("GetScreenBounds starts at the widget's absolute origin"), ScreenPosition, FVector2D::ZeroVector);
		TestEqual(TEXT("...and spans the view"), ScreenSize, FVector2D(ViewSize.X, ViewSize.Y));

		// GetTextBounds for a BARE CARET returns false == "not clipped", i.e. a usable rect.
		FVector2D CaretPos = FVector2D::ZeroVector;
		FVector2D CaretSize = FVector2D::ZeroVector;
		const bool bClipped = ImeContext->GetTextBounds(2, 0, CaretPos, CaretSize);
		TestFalse(TEXT("GetTextBounds reports a caret as NOT clipped"), bClipped);
		TestTrue(TEXT("...with a positive height"), CaretSize.Y > 0.0);
		AddInfo(FString::Printf(TEXT("GetTextBounds caret rect: pos (%.1f, %.1f) size (%.1f, %.1f) in Slate absolute px"),
			CaretPos.X, CaretPos.Y, CaretSize.X, CaretSize.Y));

		// A non-empty RANGE is answered `true` (clipped): only the caret is known, so the rect is
		// an anchor rather than a measurement of those characters. Stated in code, not just in a
		// comment.
		FVector2D RangePos = FVector2D::ZeroVector;
		FVector2D RangeSize = FVector2D::ZeroVector;
		TestTrue(TEXT("GetTextBounds for a real range admits it is not an exact measurement"),
			ImeContext->GetTextBounds(0, 2, RangePos, RangeSize));

		TestTrue(TEXT("GetWindow() is null in a headless run"), !ImeContext->GetWindow().IsValid());
	}

	// 6. A STALE MUTATION IS DROPPED. Pushed straight onto the real queue so the drop happens
	// where it has to -- on the UI thread, in VaCuusTextInput::ApplyMutation.
	{
		const uint64 StaleGeneration = FieldGeneration + 1000;
		UIThread->EnqueueInput(ViewId,
			FVaCuusInputEvent::ImeSetTextInRange(StaleGeneration, /*RangeBegin=*/0, /*RangeEnd=*/2, TEXT("ZZ")));

		if (!TestTrue(TEXT("UI frame ran after the stale mutation"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		TestEqual(TEXT("A mutation stamped with a stale field generation is dropped"), Host->FieldValue,
			FString(TEXT("Va")));

		// THE CONTROL, and it is what makes the assertion above mean anything: the same mutation
		// with the CURRENT stamp must land. Otherwise "dropped" could just mean "the whole path
		// is dead".
		UIThread->EnqueueInput(ViewId,
			FVaCuusInputEvent::ImeSetTextInRange(FieldGeneration, /*RangeBegin=*/0, /*RangeEnd=*/2, TEXT("Ok")));

		if (!TestTrue(TEXT("UI frame ran after the current mutation"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		TestEqual(TEXT("The SAME mutation with the current stamp is applied"), Host->FieldValue, FString(TEXT("Ok")));
	}

	// 7. THE COMPOSITION ROUND TRIP, driven exactly as the platform would drive it: Begin,
	// UpdateCompositionRange, End. The queue and RmlUi are real; only TSF is stood in for.
	View->PollStatus();
	{
		const uint64 CurrentGeneration = View->GetSnapshot().TextField.Generation;

		ImeContext->BeginComposition();
		TestTrue(TEXT("BeginComposition makes IsComposing true"), ImeContext->IsComposing());

		ImeContext->UpdateCompositionRange(/*InBeginIndex=*/0, /*InLength=*/2);
		if (!TestTrue(TEXT("UI frame ran after the composition range"), RunFrames(*UIThread, 1)))
		{
			return false;
		}

		View->PollStatus();
		{
			const FVaCuusTextFieldState& Field = View->GetSnapshot().TextField;
			TestEqual(TEXT("The composing span reaches RmlUi and comes back published"), Field.CompositionBegin, 0);
			TestEqual(TEXT("...to the right end"), Field.CompositionEnd, 2);
			TestEqual(TEXT("...without changing the field's identity"), int64(Field.Generation),
				int64(CurrentGeneration));
		}

		ImeContext->EndComposition();
		TestFalse(TEXT("EndComposition clears IsComposing"), ImeContext->IsComposing());

		if (!TestTrue(TEXT("UI frame ran after the commit"), RunFrames(*UIThread, 1)))
		{
			return false;
		}

		// The commit replaced [0,2) with what was already there, so the value is unchanged --
		// which is the correct outcome and the one that proves the RANGE arithmetic: an off-by-one
		// in either index space would duplicate or truncate a character here.
		TestEqual(TEXT("The commit leaves the composed text in place"), Host->FieldValue, FString(TEXT("Ok")));

		View->PollStatus();
		{
			const FVaCuusTextFieldState& Field = View->GetSnapshot().TextField;
			TestEqual(TEXT("The composing span is cleared after the commit"), Field.CompositionBegin, 0);
			TestEqual(TEXT("...at both ends"), Field.CompositionEnd, 0);
			TestEqual(TEXT("The value survives the round trip"), Field.Value, FString(TEXT("Ok")));
		}
	}

	// 8. BLURRING THE FIELD RETRACTS EVERYTHING. Clicking the button moves RmlUi focus off the
	// text control, so the view must stop publishing a field -- otherwise the platform would
	// keep composing into text nobody is editing.
	{
		const TSet<FKey> LeftOnly = {EKeys::LeftMouseButton};
		const TSet<FKey> NoButtons;

		Widget->OnMouseButtonDown(Geometry, MakePointerEvent(GButtonPoint, LeftOnly, EKeys::LeftMouseButton));
		Widget->OnMouseButtonUp(Geometry, MakePointerEvent(GButtonPoint, NoButtons, EKeys::LeftMouseButton));

		if (!TestTrue(TEXT("UI frame ran after clicking the button"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		TestEqual(TEXT("The button took RmlUi focus"), Host->FocusId, FString(TEXT("btn")));

		View->PollStatus();
		const FVaCuusInteractiveSnapshot& Snapshot = View->GetSnapshot();
		TestTrue(TEXT("The view still wants the keyboard (a button holds focus)"), Snapshot.bWantsKeyboardFocus);
		TestFalse(TEXT("...but no TEXT control does (D14b is strictly narrower)"), Snapshot.bTextInputFocused);
		TestEqual(TEXT("...so the shadow field state is retracted"), int64(Snapshot.TextField.Generation), int64(0));
		TestEqual(TEXT("...including its value"), Snapshot.TextField.Value, FString());

		// And a mutation for the field that is gone is dropped rather than typed into the button.
		UIThread->EnqueueInput(ViewId,
			FVaCuusInputEvent::ImeSetTextInRange(FieldGeneration, /*RangeBegin=*/0, /*RangeEnd=*/2, TEXT("XX")));
		if (!TestTrue(TEXT("UI frame ran after the orphaned mutation"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		TestEqual(TEXT("A mutation for a blurred field changes nothing"), Host->FieldValue, FString(TEXT("Ok")));
	}

	// 9. REGRESSION: AN ABANDONED COMPOSITION IS CLEARED IN RmlUi, NOT LEFT BEHIND.
	//
	// THE BUG THIS GUARDS. When RmlUi's OWN focus leaves a composing field -- Tab, a script
	// Blur(), a Task-10 document swap -- nothing engine-side calls EndComposition. The cleanup
	// used to be queued from FVaCuusImeHandler::DeactivateContext -> AbortComposition, stamped
	// with the shadow generation; but by then the snapshot carries the RESET field state
	// (Generation 0) while the UI-side handler has already bumped its generation past 0 in the
	// same UI frame, so ApplyMutation dropped that cleanup EVERY time -- it could not, even in
	// principle, carry a matching stamp. RmlUi's Blur handler does not reset
	// ime_composition_begin/end_index either, and FormatText draws the composing underline
	// whenever end > begin regardless of whether any OS is still composing, so a phantom
	// underline survived until the value happened to change. The fix clears it directly on the
	// UI thread in FVaCuusRmlTextInputHandler::OnDeactivate, where the live
	// Rml::TextInputContext* is already in hand.
	//
	// HOW IT IS ASSERTED, AND WHY INDIRECTLY. RmlUi exposes NO public getter for a field's
	// composition range: TextInputContext has SetCompositionRange and no counterpart, and the
	// only getter that exists (WidgetTextInput::GetCompositionRange) is on an internal class no
	// public header reaches. Asserting the PUBLISHED FVaCuusTextFieldState::CompositionBegin/End
	// would prove nothing -- those come from our own handler's bookkeeping, which
	// Clear() already zeroed on deactivate even with the bug present.
	//
	// So the probe is BEHAVIOURAL, and it reads RmlUi's internal range through the one public
	// door that depends on it: CommitComposition TAKES NO ACTION when the composition range is
	// [0, 0] (WidgetTextInput.cpp:128-131). Committing with an EMPTY passed range -- which
	// ApplyMutation deliberately does not re-assert -- therefore does exactly one of two things:
	//
	//   bug present:  the internal range is still (0, 2) -> the commit replaces those bytes ->
	//                 the value becomes "!!"
	//   bug fixed:    the internal range is (0, 0)       -> the commit no-ops ->
	//                 the value stays "Ok"
	//
	// Nothing between setting the range and the probe touches the value, which matters because
	// OnValueAttributeChanged is the one other thing that resets the range
	// (WidgetTextInput.cpp:265-268) and would otherwise mask the bug.
	{
		const TSet<FKey> LeftOnly = {EKeys::LeftMouseButton};
		const TSet<FKey> NoButtons;
		const FVaCuusModifierState NoModifiers;

		// Back onto the field, which still holds "Ok".
		Widget->OnMouseButtonDown(Geometry, MakePointerEvent(GFieldPoint, LeftOnly, EKeys::LeftMouseButton));
		Widget->OnMouseButtonUp(Geometry, MakePointerEvent(GFieldPoint, NoButtons, EKeys::LeftMouseButton));
		if (!TestTrue(TEXT("UI frame ran after refocusing the field"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		if (!TestEqual(TEXT("The field is focused again"), Host->FocusId, FString(TEXT("field"))))
		{
			return false;
		}

		View->PollStatus();

		// Start a composition and give it a real span, so there is something to abandon.
		ImeContext->BeginComposition();
		ImeContext->UpdateCompositionRange(/*InBeginIndex=*/0, /*InLength=*/2);
		if (!TestTrue(TEXT("UI frame ran after starting the composition"), RunFrames(*UIThread, 1)))
		{
			return false;
		}

		View->PollStatus();
		if (!TestEqual(TEXT("The composing span reached RmlUi before the blur"),
				View->GetSnapshot().TextField.CompositionEnd, 2))
		{
			return false;
		}

		// THE ABANDONMENT: Tab moves RmlUi's focus to the next field with no EndComposition
		// anywhere. This is the exact path the bug lived on.
		UIThread->EnqueueInput(ViewId, FVaCuusInputEvent::KeyEvent(/*bDown=*/true, EKeys::Tab, NoModifiers));
		if (!TestTrue(TEXT("UI frame ran after Tab"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		TestNotEqual(TEXT("Tab moved RmlUi focus off the composing field"), Host->FocusId, FString(TEXT("field")));

		// The engine-facing contract is deliberately UNTOUCHED by the RmlUi-side cleanup: TSF
		// owns this flag and only EndComposition/AbortComposition may clear it. If the fix ever
		// starts reaching across into the game-thread context, this is what catches it.
		TestTrue(TEXT("The engine-side composition is still open (nothing called EndComposition)"),
			ImeContext->IsComposing());

		// Back onto the field for the probe. Focus changes no value, so RmlUi's internal
		// composition range is whatever the blur left it as -- which is the thing under test.
		Widget->OnMouseButtonDown(Geometry, MakePointerEvent(GFieldPoint, LeftOnly, EKeys::LeftMouseButton));
		Widget->OnMouseButtonUp(Geometry, MakePointerEvent(GFieldPoint, NoButtons, EKeys::LeftMouseButton));
		if (!TestTrue(TEXT("UI frame ran after refocusing for the probe"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		if (!TestEqual(TEXT("The field is focused for the probe"), Host->FocusId, FString(TEXT("field"))))
		{
			return false;
		}
		TestEqual(TEXT("Refocusing did not change the value"), Host->FieldValue, FString(TEXT("Ok")));

		View->PollStatus();
		const uint64 ProbeGeneration = View->GetSnapshot().TextField.Generation;

		// An EMPTY range, so ApplyMutation does not re-assert one and CommitComposition is left
		// reading RmlUi's own. See the block comment for the two outcomes.
		UIThread->EnqueueInput(ViewId,
			FVaCuusInputEvent::ImeCommitComposition(ProbeGeneration, /*RangeBegin=*/0, /*RangeEnd=*/0, TEXT("!!")));
		if (!TestTrue(TEXT("UI frame ran after the probe commit"), RunFrames(*UIThread, 1)))
		{
			return false;
		}

		TestEqual(
			TEXT("An abandoned composition was cleared in RmlUi on blur, so a commit against an empty range no-ops"),
			Host->FieldValue, FString(TEXT("Ok")));
	}

	UIThread->EnqueueRemoveView(ViewId);
	TestTrue(TEXT("A UI frame survives the view removal"), RunFrames(*UIThread, 2));

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
