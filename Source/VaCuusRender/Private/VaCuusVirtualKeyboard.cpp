// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusVirtualKeyboard.h"

#include "VaCuus.h"
#include "VaCuusInputEvent.h"
#include "VaCuusView.h"

TSharedRef<FVaCuusVirtualKeyboardEntry> FVaCuusVirtualKeyboardEntry::Create(UVaCuusView& InView)
{
	return MakeShareable(new FVaCuusVirtualKeyboardEntry(InView));
}

FVaCuusVirtualKeyboardEntry::FVaCuusVirtualKeyboardEntry(UVaCuusView& InView)
	: View(&InView)
{
	check(IsInGameThread());
}

void FVaCuusVirtualKeyboardEntry::SetShadowState(const FVaCuusTextFieldState& InState)
{
	check(IsInGameThread());

	State = InState;

	// The echo window closes as soon as the field agrees with what we pushed -- OR as soon as
	// the field's IDENTITY changes, because a value from the previous field must never be
	// reported as this one's. Both conditions, not just the first: a push that was DROPPED by
	// the UI thread (stale stamp, element gone) would otherwise hold PushedValue forever and
	// keep lying to the OS about a field it never reached.
	if (bHasPushedValue && (State.Value.Equals(PushedValue, ESearchCase::CaseSensitive) || State.Generation == 0))
	{
		bHasPushedValue = false;
		PushedValue.Reset();
	}
}

void FVaCuusVirtualKeyboardEntry::Shutdown()
{
	check(IsInGameThread());

	View.Reset();
	State.Reset();
	PushedValue.Reset();
	bHasPushedValue = false;
}

void FVaCuusVirtualKeyboardEntry::SetTextFromVirtualKeyboard(const FText& InNewText, ETextEntryType TextEntryType)
{
	check(IsInGameThread());

	UVaCuusView* const ViewPtr = View.Get();
	if (ViewPtr == nullptr || State.Generation == 0)
	{
		// After Shutdown(), or with no field focused: the platform can and does deliver a final
		// result after the field is gone (Android's dismiss path pushes the buffer one last time,
		// AndroidJNI.cpp:1267-1269), and there is nowhere to send it.
		UE_LOG(LogVaCuus, Verbose,
			TEXT("Virtual keyboard: a value arrived with no live field to put it in; dropped"));
		return;
	}

	// EVERY ENTRY TYPE IS APPLIED, INCLUDING TextEntryCanceled, and that is deliberate rather
	// than an oversight. The three types are Canceled, Accepted and Updated
	// (IVirtualKeyboardEntry.h:21-29), and Android reaches the Canceled one by DISMISSING the
	// dialog -- which it does with `update == JNI_TRUE` and the current contents, i.e. it means
	// "the player backed out of the dialog", not "discard what they typed"
	// (AndroidJNI.cpp:1223-1230, :1267-1269). Slate's own reference implementation makes no
	// distinction either: SVirtualKeyboardEntry::SetTextFromVirtualKeyboard ignores the
	// parameter entirely (SVirtualKeyboardEntry.cpp:54-72). A "revert on cancel" would need an
	// original value to revert TO, and the only honest one -- the document's -- may have been
	// changed by the document itself while the dialog was up.
	const FString NewValue = InNewText.ToString();

	PushedValue = NewValue;
	bHasPushedValue = true;

	// STAMPED WITH THE SHADOW'S GENERATION, like every IME mutation: a whole-value push into a
	// field that has since been blurred would not corrupt an offset, it would overwrite a
	// different field's entire contents. VaCuusTextInput::ApplyMutation drops it on mismatch.
	ViewPtr->SendInput(FVaCuusInputEvent::VirtualKeyboardValue(State.Generation, NewValue));

	UE_LOG(LogVaCuus, Verbose, TEXT("Virtual keyboard: pushed %d characters into field generation %llu (entry type %d)"),
		NewValue.Len(), State.Generation, int32(TextEntryType));
}

void FVaCuusVirtualKeyboardEntry::SetSelectionFromVirtualKeyboard(int InSelStart, int InSelEnd)
{
	check(IsInGameThread());

	// DECLINED, WITH THE SAME ANSWER SLATE'S OWN REFERENCE IMPLEMENTATION GIVES:
	// SVirtualKeyboardEntry::SetSelectionFromVirtualKeyboard has an empty body and says
	// "Nothing to do for widgets without a cursor" (SVirtualKeyboardEntry.cpp:74-78). We DO have
	// a cursor, so the reason here is a different one and worth stating plainly rather than
	// borrowing theirs.
	//
	// THE INDEX SPACE IS NOT SPECIFIED. `IVirtualKeyboardEntry` documents neither of these two
	// ints (IVirtualKeyboardEntry.h:66), the only caller that produces them is iOS
	// (IOSPlatformTextField.cpp:538) and RmlUi's selection API is in UTF-8 CHARACTER offsets.
	// If those spaces differ -- and they differ for every non-BMP character -- applying the
	// values would silently drop the caret in the wrong place, in exactly the text (emoji, CJK)
	// where a mobile keyboard is most likely to be used. Guessing is worse than declining: the
	// whole-value push above already leaves the caret after the pushed text, which is where the
	// OS's own caret is at that moment.
	//
	// WHAT IT COSTS, so it is not discovered on a device: on iOS, moving the caret with the OS
	// keyboard's own controls does not move RmlUi's caret until the next value push. Resolving
	// it needs a device to establish what the two ints actually are.
	UE_LOG(LogVaCuus, Verbose,
		TEXT("Virtual keyboard: selection [%d, %d) from the platform ignored -- its index space is unspecified"),
		InSelStart, InSelEnd);
}

FText FVaCuusVirtualKeyboardEntry::GetText() const
{
	// The echo first: this is what the platform must see between our push and the UI thread
	// republishing the field. See PushedValue.
	return FText::FromString(bHasPushedValue ? PushedValue : State.Value);
}

bool FVaCuusVirtualKeyboardEntry::GetSelection(int& OutSelStart, int& OutSelEnd)
{
	// ANSWERED, unlike the setter, because a read cannot corrupt anything: the worst a
	// UTF-16-vs-character mismatch can do here is put the OS's initial caret one position out
	// in a field containing non-BMP text, and the alternative -- `return false`, which is what
	// SVirtualKeyboardEntry does (SVirtualKeyboardEntry.h:114-117) -- puts it at position zero
	// in every field, every time.
	OutSelStart = State.SelectionBegin;
	OutSelEnd = State.SelectionEnd;
	return true;
}

FText FVaCuusVirtualKeyboardEntry::GetHintText() const
{
	// The element's `placeholder`, which on Android's default route is the LABEL of the modal
	// input dialog (AndroidPlatformTextField.cpp:102) -- the only thing on screen telling the
	// player what they are typing into, since the dialog covers the document.
	return FText::FromString(State.HintText);
}

EKeyboardType FVaCuusVirtualKeyboardEntry::GetVirtualKeyboardType() const
{
	// TWO OF THE SIX, BECAUSE RmlUi EXPOSES TWO. EKeyboardType also has Number, Web, Email and
	// AlphaNumeric (IVirtualKeyboardEntry.h:11-19), and Android maps each to a real Java
	// InputType (AndroidPlatformTextField.cpp:44-64) -- but RmlUi's <input> dispatch recognises
	// only text, password, radio, checkbox, range, submit and button
	// (ElementFormControlInput.cpp:105-119), and everything after `password` in that list
	// instances a control with no caret at all. There is no RmlUi attribute that selects a
	// numeric or email keyboard, so there is nothing to map from; inventing one would mean
	// inventing the attribute too.
	return State.bPassword ? EKeyboardType::Keyboard_Password : EKeyboardType::Keyboard_Default;
}

FVirtualKeyboardOptions FVaCuusVirtualKeyboardEntry::GetVirtualKeyboardOptions() const
{
	// The default: autocorrect off. FVirtualKeyboardOptions carries exactly one field today
	// (IVirtualKeyboardEntry.h:31-47) and RmlUi has no attribute to drive it from, so this is
	// the whole of it -- and the platform still gets the last word, since Android ANDs our
	// answer with the project's own Input setting
	// (IPlatformTextField::ShouldUseVirtualKeyboardAutocorrect, AndroidPlatformTextField.cpp:67).
	return FVirtualKeyboardOptions();
}

bool FVaCuusVirtualKeyboardEntry::IsMultilineEntry() const
{
	// <textarea>. Android turns this into TYPE_TEXT_FLAG_MULTI_LINE on the integrated keyboard
	// (AndroidPlatformTextField.cpp:98-99), which is what puts a newline key on it instead of
	// a "done" key.
	return State.bMultiline;
}
