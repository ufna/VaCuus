// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusInteractiveSnapshot.h"

#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtr.h"
#include "Widgets/Input/IVirtualKeyboardEntry.h"

class UVaCuusView;

/**
 * THE MOBILE HALF OF TEXT ENTRY: one RmlUi text field, dressed as an `IVirtualKeyboardEntry`
 * so the platform's on-screen keyboard can edit it.
 *
 * WHY A SECOND TEXT-INPUT BRIDGE EXISTS AT ALL, next to the IME one in VaCuusTextInput.h.
 * There is no ITextInputMethodSystem on a phone -- only FWindowsApplication and
 * FMacApplication override GenericApplication::GetTextInputMethodSystem(), everything else
 * inherits the `return NULL` at GenericApplication.h:550 -- so on Android and iOS the entire
 * IME path is inert AND the OnKeyChar degradation it falls back to produces nothing either,
 * because with no hardware keyboard nothing ever generates a character. Mobile text entry is
 * a different interface, not a reduced one, and this is that interface.
 *
 * WHOSE ARBITRATION, AND IT IS THE ENGINE'S, NOT OURS. Slate's own editable text asks
 * `FPlatformApplicationMisc::RequiresVirtualKeyboard()` and takes one branch or the other --
 * ShowVirtualKeyboard on true, EnableTextInputMethodContext on false
 * (SlateEditableTextLayout.cpp:879-893, and the mirror-image pair in HandleFocusLost at
 * :939-957). SVaCuusWidget asks the same question in the same shape, so there is no
 * `#if PLATFORM_ANDROID` anywhere in this feature and no possibility of both paths driving
 * one field: the answer is a RUNTIME one (`PLATFORM_HAS_TOUCH_MAIN_SCREEN || bAllowVirtualKeyboard`,
 * GenericPlatformApplicationMisc.cpp:110-113), which is also what makes it testable on a
 * desktop -- the `AllowVirtualKeyboard` cvar (:34-42) flips it.
 *
 * THE INTERFACE IS WHOLE-VALUE, WHICH IS THE WHOLE DESIGN. `SetTextFromVirtualKeyboard`
 * delivers the complete contents of the OS's own edit buffer (IVirtualKeyboardEntry.h:64);
 * `GetText`/`GetHintText`/`GetVirtualKeyboardType`/`IsMultilineEntry` are pulled back
 * synchronously, on the GAME thread, by the platform text field before it crosses into Java
 * or Objective-C (AndroidPlatformTextField.cpp:41-102). So this object answers every read
 * from the published shadow -- exactly the discipline FVaCuusTextInputMethodContext follows
 * for the 14 IME virtuals -- and turns every write into one generation-stamped queue event.
 * It never touches RmlUi and never blocks.
 *
 * WHY IT LIVES IN VaCuusRender AND NOT NEXT TO ITS IME SIBLING. `IVirtualKeyboardEntry` and
 * `FSlateApplication::ShowVirtualKeyboard` are both in the Slate module, and the VaCuus module
 * deliberately depends only on ApplicationCore. The IME case got to keep its bridge in VaCuus
 * because only the LOOKUP was Slate-shaped -- the interface itself is ApplicationCore -- so the
 * host could pass a pointer across (FVaCuusImeSurface::TextInputMethodSystem). That trick does
 * not extend here: the interface, the entry and the show call are all Slate. So the whole
 * bridge sits on the Slate side of the same seam, and reaches the view through exactly two
 * public doors it already had -- the published snapshot and UVaCuusView::SendInput.
 *
 * Game thread only. Every platform caller marshals: Android through an explicit
 * FFunctionGraphTask onto ENamedThreads::GameThread (AndroidJNI.cpp:1234-1245), iOS through
 * its own dispatch, and the engine's reference implementation asserts the same
 * (SVirtualKeyboardEntry::SetTextFromVirtualKeyboard, SVirtualKeyboardEntry.cpp:54).
 */
class FVaCuusVirtualKeyboardEntry final : public IVirtualKeyboardEntry, public TSharedFromThis<FVaCuusVirtualKeyboardEntry>
{
public:
	/**
	 * InView must outlive the entry minus its Shutdown(); the hosting SVaCuusWidget owns both
	 * ends of that.
	 *
	 * Private ctor + static Create, like both IME references and for the same reason:
	 * FSlateApplication::ShowVirtualKeyboard takes a TSharedPtr and the platform keeps a
	 * TWeakPtr to it for the lifetime of the keyboard (AndroidJNI.cpp:1213-1220), so nothing
	 * here may ever exist on the stack.
	 */
	static TSharedRef<FVaCuusVirtualKeyboardEntry> Create(UVaCuusView& InView);

	/** Refreshed once per game frame from the newest published snapshot. */
	void SetShadowState(const FVaCuusTextFieldState& InState);

	/** The shadow as last set, for the reconciler's field-identity edge and for tests. */
	const FVaCuusTextFieldState& GetShadowState() const { return State; }

	/**
	 * The KillContext moment (SlateEditableTextLayout.h:600-604): after this every getter
	 * answers empty and every setter is a no-op.
	 *
	 * NEEDED EVEN THOUGH THE VIEW POINTER IS WEAK. The platform holds this object by TWeakPtr
	 * and can call into it after the widget has been torn out of the tree; a retained shadow
	 * would keep GetText() handing the OS the contents of a field that is gone, which is
	 * precisely what a keyboard left on screen during a level transition would then commit
	 * back into whatever is focused next.
	 */
	void Shutdown();

	//~ Begin IVirtualKeyboardEntry
	virtual void SetTextFromVirtualKeyboard(const FText& InNewText, ETextEntryType TextEntryType) override;
	virtual void SetSelectionFromVirtualKeyboard(int InSelStart, int InSelEnd) override;
	virtual FText GetText() const override;
	virtual bool GetSelection(int& OutSelStart, int& OutSelEnd) override;
	virtual FText GetHintText() const override;
	virtual EKeyboardType GetVirtualKeyboardType() const override;
	virtual FVirtualKeyboardOptions GetVirtualKeyboardOptions() const override;
	virtual bool IsMultilineEntry() const override;
	//~ End IVirtualKeyboardEntry

private:
	explicit FVaCuusVirtualKeyboardEntry(UVaCuusView& InView);

	/**
	 * The view, or null after Shutdown().
	 *
	 * WEAK RATHER THAN RAW, unlike FVaCuusImeHandler's back-pointer, and the difference is
	 * real: that handler is OWNED BY the view, so "the view died without retiring me" cannot
	 * happen. This one is owned by the widget and outlives the widget in the platform's hands,
	 * so the view genuinely can be garbage collected underneath it.
	 */
	TWeakObjectPtr<UVaCuusView> View;

	/** The published shadow; every read below is answered from it and from nowhere else. */
	FVaCuusTextFieldState State;

	/**
	 * The value we last pushed into the queue, held until the shadow catches up.
	 *
	 * NOT AN OPTIMISATION -- IT CLOSES A REAL ECHO WINDOW. The shadow is republished by the UI
	 * thread a frame or more after a push lands, and iOS re-reads GetText() while the keyboard
	 * is open (IOSPlatformTextField.cpp:500-560). Without this the OS would read back the text
	 * as it was BEFORE its own edit and could reasonably decide to restore it, undoing the
	 * keystroke. Cleared the moment the shadow reports the same string, so it can never mask a
	 * change the document made itself.
	 */
	FString PushedValue;
	bool bHasPushedValue = false;
};
