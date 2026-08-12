// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "SVaCuusWidget.h"
#include "VaCuus.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusInputEvent.h"
#include "VaCuusInteractiveSnapshot.h"
#include "VaCuusRmlCasts.h"
#include "VaCuusSlateElement.h"
#include "VaCuusSubsystem.h"
#include "VaCuusTestDocumentHost.h"
#include "VaCuusUIThread.h"
#include "VaCuusView.h"
#include "VaCuusViewStatus.h"

#include "Engine/GameInstance.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Layout/Geometry.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Input/IVirtualKeyboardEntry.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusVirtualKeyboardTest
{
/** The text-entry probe, cut down to the two observations this test reads. */
class FProbeHost final : public FVaCuusTestDocumentHost
{
public:
	FProbeHost()
		: FVaCuusTestDocumentHost(TEXT("vacuus_vkb_view"), "vacuus://vkb.rml", Rml::FocusFlag::Document)
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

		Context->Update();

		FVaCuusInteractiveSnapshot& Snapshot = Status->GetSnapshotWriteBuffer();
		BuildVaCuusInteractiveSnapshot(*Context, ViewSize, ++SnapshotGeneration, Snapshot);
		Status->PublishSnapshot();

		Rml::Element* const Focus = Context->GetFocusElement();
		FocusId = Focus ? FString(UTF8_TO_TCHAR(Focus->GetId().c_str())) : FString();

		// READ OFF THE ELEMENT, ON THE UI THREAD. The published shadow is our own bookkeeping;
		// the element's value is RmlUi's, and it is the only thing that proves a push landed.
		FieldValue.Reset();
		if (RmlDocument)
		{
			if (Rml::Element* Field = RmlDocument->GetElementById("field"))
			{
				if (Rml::ElementFormControl* Control = VaCuusCastFormControl(*Field))
				{
					FieldValue = UTF8_TO_TCHAR(Control->GetValue().c_str());
				}
			}
		}

		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

	FString FocusId;
	FString FieldValue;

private:
	uint64 SnapshotGeneration = 0;
};

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
 * 400x300. Four controls, chosen so every answer IVirtualKeyboardEntry can give has a element
 * that produces it and at least one that does not:
 *
 *   #field  (200,150)-(350,180)  <input type="text" placeholder=...>  default type, hint text
 *   #secret (200,200)-(350,230)  <input type="password">              Keyboard_Password
 *   #area   (20, 80)-(170,130)   <textarea>                           IsMultilineEntry
 *   #btn    (20, 20)-(120, 60)   <button>                             no keyboard at all
 */
static const TCHAR* GDocument = TEXT(R"(<rml>
<head>
<style>
body { display: block; width: 100%; height: 100%; font-family: LatoLatin; font-size: 14px; }
div, button, textarea { display: block; position: absolute; }
#btn    { left: 20px;  top: 20px;  width: 100px; height: 40px; tab-index: auto; }
#area   { left: 20px;  top: 80px;  width: 150px; height: 50px; }
#field  { position: absolute; left: 200px; top: 150px; width: 150px; height: 30px; }
#secret { position: absolute; left: 200px; top: 200px; width: 150px; height: 30px; }
</style>
</head>
<body>
	<button id="btn"/>
	<textarea id="area"></textarea>
	<input id="field" type="text" placeholder="Player name"/>
	<input id="secret" type="password"/>
</body>
</rml>)");

static const FIntPoint GFieldPoint(260, 165);
static const FIntPoint GSecretPoint(260, 215);
static const FIntPoint GAreaPoint(90, 105);
static const FIntPoint GButtonPoint(70, 40);

static FPointerEvent MakePointerEvent(FIntPoint Position, const TSet<FKey>& PressedButtons, const FKey& EffectingButton)
{
	const FVector2D Screen(Position.X, Position.Y);
	return FPointerEvent(FSlateApplicationBase::CursorPointerIndex, Screen, Screen, PressedButtons, EffectingButton,
		/*WheelDelta=*/0.0f, FModifierKeysState());
}
}	 // namespace VaCuusVirtualKeyboardTest

/**
 * MOBILE TEXT ENTRY: the on-screen keyboard bridge, end to end (bead VaCuus-ujm).
 *
 * WHAT THIS PROVES. On Android and iOS there is no `ITextInputMethodSystem` at all -- only
 * FWindowsApplication and FMacApplication override it, everything else inherits
 * GenericApplication.h:550's null -- and the OnKeyChar degradation VaCuus.Input.TextEntry
 * exercises delivers NOTHING there, because with no hardware keyboard nothing ever produces a
 * character. Mobile text entry is `IVirtualKeyboardEntry`, a whole-value push interface, and
 * this test drives that interface exactly as the platform does: the widget registers an entry
 * when a field takes focus, the entry answers the four questions the platform asks before it
 * opens a keyboard, `SetTextFromVirtualKeyboard` puts the OS's string into the RmlUi element,
 * and a blur takes the keyboard away again.
 *
 * WHAT THE OBSERVABLE IS, AND WHY IT IS OURS. `FSlateApplication::ShowVirtualKeyboard` is a
 * one-way call (SlateApplication.cpp:4283-4298) with no query counterpart anywhere in the
 * engine, and on this box the platform side of it is `FGenericPlatformTextField`, whose
 * ShowVirtualKeyboard has an empty body (GenericPlatformTextField.h:12). SO NO OS KEYBOARD
 * APPEARS HERE AND NONE IS ASSERTED. What is asserted is our half: that the widget built an
 * entry, handed it over for the right field, kept it correct, and took it back --
 * SVaCuusWidget::GetShownVirtualKeyboardEntry_Debug() and its generation twin. The remaining
 * leg -- a keyboard actually rising on an iPhone -- is device-only.
 *
 * HOW THE MOBILE BRANCH IS REACHED FROM A DESKTOP. The arbitration is the engine's runtime
 * signal, not a `#if`: `FPlatformApplicationMisc::RequiresVirtualKeyboard()`, which is what
 * FSlateEditableTextLayout branches on (SlateEditableTextLayout.cpp:879-893, :939-957). Its
 * generic implementation is `PLATFORM_HAS_TOUCH_MAIN_SCREEN || bAllowVirtualKeyboard`
 * (GenericPlatformApplicationMisc.cpp:110-113) and the second term is the `AllowVirtualKeyboard`
 * cvar (:34-42). Section 1 asserts the DESKTOP default before touching it, which is also the
 * no-regress statement: with the cvar at its default nothing in this feature runs at all.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusVirtualKeyboardTest, "VaCuus.Input.VirtualKeyboard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusVirtualKeyboardTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusVirtualKeyboardTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no worker thread to drive"));
		return true;
	}

	// The same macOS venue accommodation VaCuus.Input.TextEntry carries, and for the same
	// reason: on a platform that HAS an ITextInputMethodSystem, a headless widget has no
	// NSWindow for FMacTextInputMethodSystem::ActivateContext to find, and the Error it logs
	// would fail this test even though nothing here is about the IME. Negative count == silently
	// ignored (AutomationTest.h:1792-1794).
	AddExpectedMessagePlain(TEXT("Activating a context failed when its window"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, /*Occurrences=*/-1);

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

	// 1. THE DESKTOP DEFAULT, ASSERTED BEFORE IT IS CHANGED. This is the no-regress statement
	// for Win64/Mac/Linux in one line: the predicate every site in this feature is gated on is
	// false here, so none of it runs and the ITextInputMethodSystem path is untouched.
	if (!TestFalse(TEXT("A desktop does not use a virtual keyboard as its input method"),
			FPlatformApplicationMisc::RequiresVirtualKeyboard()))
	{
		return false;
	}

	IConsoleVariable* const AllowVirtualKeyboard =
		IConsoleManager::Get().FindConsoleVariable(TEXT("AllowVirtualKeyboard"));
	if (!TestNotNull(TEXT("The engine's AllowVirtualKeyboard cvar exists"), AllowVirtualKeyboard))
	{
		return false;
	}

	AllowVirtualKeyboard->Set(TEXT("1"), ECVF_SetByCode);
	ON_SCOPE_EXIT
	{
		// Restored whatever happens: this is a process-global that decides Slate's own text
		// behaviour, and leaving it on would change every later test in the run.
		AllowVirtualKeyboard->Set(TEXT("0"), ECVF_SetByCode);
	};

	if (!TestTrue(TEXT("...and setting AllowVirtualKeyboard makes the platform report one"),
			FPlatformApplicationMisc::RequiresVirtualKeyboard()))
	{
		return false;
	}

	AddInfo(TEXT("FPlatformApplicationMisc::RequiresVirtualKeyboard() forced true via the engine's AllowVirtualKeyboard ")
			TEXT("cvar. The platform text field on this host is FGenericPlatformTextField, whose ShowVirtualKeyboard is ")
			TEXT("an empty body -- no OS keyboard appears, and none is asserted."));

	const FIntPoint ViewSize(400, 300);
	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();

	TUniquePtr<FProbeHost> OwnedHost = MakeUnique<FProbeHost>();
	FProbeHost* Host = OwnedHost.Get();

	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), ViewSize, Status);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GDocument, /*LoadSerial=*/1);

	TStrongObjectPtr<UGameInstance> GameInstance(NewObject<UGameInstance>());
	TStrongObjectPtr<UVaCuusSubsystem> Subsystem(NewObject<UVaCuusSubsystem>(GameInstance.Get()));
	TStrongObjectPtr<UVaCuusView> View(NewObject<UVaCuusView>(Subsystem.Get()));
	View->InitializeView(Subsystem.Get(), ViewId, Status, ViewSize);

	const TSharedRef<FVaCuusSlateElement> Element = MakeShared<FVaCuusSlateElement>();
	TSharedRef<SVaCuusWidget> Widget = SNew(SVaCuusWidget, View.Get(), Element);

	ON_SCOPE_EXIT
	{
		Widget->DetachView();
	};

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
	Widget->Tick(Geometry, /*InCurrentTime=*/0.0, /*InDeltaTime=*/0.0f);

	TestFalse(TEXT("No keyboard before anything is focused"), Widget->GetShownVirtualKeyboardEntry_Debug().IsValid());

	// The press/release pair, and the reconcile that follows it. Wrapped because every section
	// below is the same three steps: click, let the UI thread see it, let the widget reconcile.
	const TSet<FKey> LeftOnly = {EKeys::LeftMouseButton};
	const TSet<FKey> NoButtons;
	auto ClickAndSettle = [&](FIntPoint Position) -> bool
	{
		Widget->OnMouseButtonDown(Geometry, MakePointerEvent(Position, LeftOnly, EKeys::LeftMouseButton));
		Widget->OnMouseButtonUp(Geometry, MakePointerEvent(Position, NoButtons, EKeys::LeftMouseButton));
		if (!RunFrames(*UIThread, 1))
		{
			return false;
		}
		View->PollStatus();
		Widget->Tick(Geometry, /*InCurrentTime=*/0.0, /*InDeltaTime=*/0.0f);
		return true;
	};

	// 2. FOCUSING A TEXT FIELD REGISTERS AND SHOWS AN ENTRY.
	uint64 FieldGeneration = 0;
	if (!TestTrue(TEXT("UI frame ran after clicking the field"), ClickAndSettle(GFieldPoint)))
	{
		return false;
	}
	if (!TestEqual(TEXT("Clicking the field focuses it in RmlUi"), Host->FocusId, FString(TEXT("field"))))
	{
		return false;
	}

	TSharedPtr<IVirtualKeyboardEntry> Entry = Widget->GetShownVirtualKeyboardEntry_Debug();
	if (!TestValid(TEXT("An IVirtualKeyboardEntry was registered and shown for the focused field"), Entry))
	{
		return false;
	}

	{
		const FVaCuusTextFieldState& Field = View->GetSnapshot().TextField;
		FieldGeneration = Field.Generation;

		TestNotEqual(TEXT("The field carries a non-zero generation"), int64(FieldGeneration), int64(0));
		TestEqual(TEXT("The shown keyboard belongs to THAT field"),
			int64(Widget->GetShownVirtualKeyboardFieldGeneration_Debug()), int64(FieldGeneration));
	}

	// 3. THE FOUR ANSWERS THE PLATFORM PULLS BEFORE IT OPENS A KEYBOARD. Android reads all of
	// these synchronously and ships them into Java (AndroidPlatformTextField.cpp:41-102), so a
	// wrong answer here is a wrong keyboard on the device.
	{
		TestEqual(TEXT("GetText answers from the published shadow"), Entry->GetText().ToString(), FString());
		TestEqual(TEXT("GetHintText is the element's `placeholder`"), Entry->GetHintText().ToString(),
			FString(TEXT("Player name")));
		TestTrue(TEXT("A plain <input type=text> asks for the default keyboard"),
			Entry->GetVirtualKeyboardType() == EKeyboardType::Keyboard_Default);
		TestFalse(TEXT("...and is not a multi-line entry"), Entry->IsMultilineEntry());

		int32 SelectionStart = -1;
		int32 SelectionEnd = -1;
		TestTrue(TEXT("GetSelection answers rather than declining"), Entry->GetSelection(SelectionStart, SelectionEnd));
		TestEqual(TEXT("...with the caret at the start of an empty field"), SelectionStart, 0);
		TestEqual(TEXT("...and nothing selected"), SelectionEnd, 0);
	}

	// 4. THE PUSH: the OS hands over its whole edit buffer and the RmlUi element takes it.
	//
	// THIS IS THE ASSERTION THE FEATURE EXISTS FOR. Everything above is registration; this is
	// the only part that puts a character anywhere. Asserted on Host->FieldValue -- read off
	// Rml::ElementFormControl::GetValue() on the UI thread -- and NOT on the entry's own
	// GetText(), which would pass even with the queue push severed (the echo below is why).
	{
		Entry->SetTextFromVirtualKeyboard(FText::FromString(TEXT("Neo")), ETextEntryType::TextEntryAccepted);

		// THE ECHO WINDOW, asserted before the queue has even drained: iOS re-reads GetText()
		// while its keyboard is open (IOSPlatformTextField.cpp:500-560), and the shadow will not
		// carry the new value for at least another UI frame. Answering the old string there would
		// invite the OS to restore it.
		TestEqual(TEXT("GetText reports the pushed value immediately, before the UI thread has seen it"),
			Entry->GetText().ToString(), FString(TEXT("Neo")));

		if (!TestTrue(TEXT("UI frame ran after the push"), RunFrames(*UIThread, 1)))
		{
			return false;
		}

		TestEqual(TEXT("SetTextFromVirtualKeyboard changes the RmlUi element's value"), Host->FieldValue,
			FString(TEXT("Neo")));

		View->PollStatus();
		Widget->Tick(Geometry, /*InCurrentTime=*/0.0, /*InDeltaTime=*/0.0f);

		TestEqual(TEXT("...and the published shadow catches up"), View->GetSnapshot().TextField.Value,
			FString(TEXT("Neo")));
		TestEqual(TEXT("GetText now answers from the shadow rather than the echo"), Entry->GetText().ToString(),
			FString(TEXT("Neo")));
		TestEqual(TEXT("The keyboard was NOT re-shown for the same field"),
			int64(Widget->GetShownVirtualKeyboardFieldGeneration_Debug()), int64(FieldGeneration));

		// AND A SECOND PUSH OF THE SAME STRING CHANGES NOTHING, which is the echo-suppression
		// branch in ApplyMutation: both backends re-push an unchanged buffer (iOS on every
		// selection move, Android once more on dismiss) and each one that got through would
		// dispatch RmlUi's `change` event and re-enter JS.
		Entry->SetTextFromVirtualKeyboard(FText::FromString(TEXT("Neo")), ETextEntryType::TextEntryUpdated);
		if (!TestTrue(TEXT("UI frame ran after the echo push"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		TestEqual(TEXT("An unchanged push leaves the value alone"), Host->FieldValue, FString(TEXT("Neo")));
	}

	// 5. THE KEYBOARD TYPE IS A PROPERTY OF THE FIELD, and moving between fields moves it.
	// `password` is the only text type RmlUi's <input> dispatch distinguishes
	// (ElementFormControlInput.cpp:105-119), which is why there is one bit and not an enum.
	{
		if (!TestTrue(TEXT("UI frame ran after clicking the password field"), ClickAndSettle(GSecretPoint)))
		{
			return false;
		}
		if (!TestEqual(TEXT("The password field took focus"), Host->FocusId, FString(TEXT("secret"))))
		{
			return false;
		}

		TSharedPtr<IVirtualKeyboardEntry> SecretEntry = Widget->GetShownVirtualKeyboardEntry_Debug();
		if (!TestValid(TEXT("A keyboard is shown for the password field too"), SecretEntry))
		{
			return false;
		}

		TestTrue(TEXT("<input type=password> asks for the obscured keyboard"),
			SecretEntry->GetVirtualKeyboardType() == EKeyboardType::Keyboard_Password);
		TestNotEqual(TEXT("Moving to another field is a new show, not the same one"),
			int64(Widget->GetShownVirtualKeyboardFieldGeneration_Debug()), int64(FieldGeneration));
	}

	// 6. <textarea> IS THE MULTI-LINE ENTRY. Android turns this into TYPE_TEXT_FLAG_MULTI_LINE,
	// i.e. a newline key instead of a "done" key (AndroidPlatformTextField.cpp:98-99).
	{
		if (!TestTrue(TEXT("UI frame ran after clicking the textarea"), ClickAndSettle(GAreaPoint)))
		{
			return false;
		}
		if (!TestEqual(TEXT("The textarea took focus"), Host->FocusId, FString(TEXT("area"))))
		{
			return false;
		}

		TSharedPtr<IVirtualKeyboardEntry> AreaEntry = Widget->GetShownVirtualKeyboardEntry_Debug();
		if (!TestValid(TEXT("A keyboard is shown for the textarea"), AreaEntry))
		{
			return false;
		}
		TestTrue(TEXT("<textarea> reports itself as a multi-line entry"), AreaEntry->IsMultilineEntry());
	}

	// 7. BLUR DISMISSES IT. A keyboard left up over a document with no field focused would cover
	// half a phone's screen and would commit its result into whatever is focused next.
	{
		if (!TestTrue(TEXT("UI frame ran after clicking the button"), ClickAndSettle(GButtonPoint)))
		{
			return false;
		}
		if (!TestEqual(TEXT("The button took RmlUi focus"), Host->FocusId, FString(TEXT("btn"))))
		{
			return false;
		}

		TestFalse(TEXT("No text control is focused any more"), View->GetSnapshot().bTextInputFocused);
		TestFalse(TEXT("...so the keyboard was dismissed"), Widget->GetShownVirtualKeyboardEntry_Debug().IsValid());
		TestEqual(TEXT("...and the shown-field generation is cleared"),
			int64(Widget->GetShownVirtualKeyboardFieldGeneration_Debug()), int64(0));

		// A LATE PUSH FROM THE PLATFORM IS DROPPED, not typed into the button. The keyboard's
		// dismiss animation outlives the dismiss call on both backends, and Android pushes the
		// buffer one final time as it goes (AndroidJNI.cpp:1267-1269); the generation stamp is
		// what makes that harmless.
		Entry->SetTextFromVirtualKeyboard(FText::FromString(TEXT("late")), ETextEntryType::TextEntryCanceled);
		if (!TestTrue(TEXT("UI frame ran after the late push"), RunFrames(*UIThread, 1)))
		{
			return false;
		}
		TestEqual(TEXT("A push stamped for a field that lost focus changes nothing"), Host->FieldValue,
			FString(TEXT("Neo")));
	}

	// 8. THE ARBITRATION, DRIVEN BACKWARDS. With the platform predicate false again -- which is
	// every desktop's steady state -- focusing a field registers no entry at all, and the
	// ITextInputMethodSystem path is the only one left. This is the same assertion section 1
	// makes, but made AFTER the feature has been seen to work, so "nothing happened" cannot be
	// mistaken for "nothing works".
	{
		AllowVirtualKeyboard->Set(TEXT("0"), ECVF_SetByCode);
		if (!TestFalse(TEXT("The platform stops asking for a virtual keyboard"),
				FPlatformApplicationMisc::RequiresVirtualKeyboard()))
		{
			return false;
		}

		if (!TestTrue(TEXT("UI frame ran after refocusing the field"), ClickAndSettle(GFieldPoint)))
		{
			return false;
		}
		if (!TestEqual(TEXT("The field is focused again"), Host->FocusId, FString(TEXT("field"))))
		{
			return false;
		}
		TestTrue(TEXT("A focused text field is reported"), View->GetSnapshot().bTextInputFocused);
		TestFalse(TEXT("...but no virtual-keyboard entry is registered where the input method is not one"),
			Widget->GetShownVirtualKeyboardEntry_Debug().IsValid());

		// The desktop path is still alive on the very same field: the IME context exists and is
		// tracking it. (VaCuus.Input.TextEntry is what exercises it properly; this is only the
		// "the other bridge did not get switched off" half.)
		TestTrue(TEXT("...while the IME bridge is still built for it"), View->GetImeStatus().bHandlerBuilt);
	}

	UIThread->EnqueueRemoveView(ViewId);
	TestTrue(TEXT("A UI frame survives the view removal"), RunFrames(*UIThread, 2));

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
