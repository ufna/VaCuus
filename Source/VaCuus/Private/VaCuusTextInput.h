// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusInteractiveSnapshot.h"

// Complete, not forward-declared: FVaCuusImeSurface below is held BY VALUE, and it is
// declared next to the view's own IME facade because that facade is the only thing that
// ever produces one.
#include "VaCuusView.h"

#include "GenericPlatform/ITextInputMethodSystem.h"
#include "Templates/SharedPointer.h"

class FVaCuusTextInputMethodContext;
class UVaCuusView;
struct FVaCuusInputEvent;

namespace Rml
{
class Context;
class Element;
class TextInputHandler;
}

/**
 * THE IME SEAM, both halves of it.
 *
 * The problem in one paragraph: the platform's text-input method system is a
 * SYNCHRONOUS PULL API on the GAME thread (`ITextInputMethodContext`, 14 pure virtuals,
 * every one of them "answer me now"), while the text it is asking about lives in RmlUi on
 * the VaCuus UI thread, which no game-thread call may touch. So the two halves never speak
 * directly: the UI thread publishes a shadow copy of the focused field
 * (FVaCuusTextFieldState, inside the per-view snapshot) and the game-thread context answers
 * every read from it; every write is a generation-stamped input event the UI thread applies
 * and may drop. Controller decision D15.
 *
 * WHAT LIVES WHERE:
 *
 *   UI thread (this file's `VaCuusTextInput` namespace)
 *     - one process-wide `Rml::TextInputHandler`, installed with Rml::SetTextInputHandler()
 *       before Rml::Initialise(); it caches the raw non-owning `Rml::TextInputContext*` the
 *       library hands out on focus and nulls it on destroy.
 *     - FillTextFieldState(): reads this view's focused control and publishes the shadow.
 *     - ApplyMutation(): applies one queued mutation, or drops it.
 *
 *   Game thread
 *     - FVaCuusTextInputMethodContext: the 14 virtuals (defined entirely in the .cpp).
 *     - FVaCuusImeHandler: registers/activates/deactivates/unregisters it and owns the
 *       change notifier. Held by UVaCuusView, driven by the Slate host.
 *
 * WORLD-SPACE SURFACES HAVE NO IME (controller decision D17) -- see FVaCuusImeSurface in
 * VaCuusView.h for why, and what a world-space host is expected to do instead.
 */
namespace VaCuusTextInput
{
/**
 * The process-wide handler, installed by FVaCuusEngine::Initialize() with
 * `Rml::SetTextInputHandler()` BEFORE `Rml::Initialise()`.
 *
 * BEFORE, not after, and it matters: Rml::Initialise() installs a default no-op handler if
 * none is set (Core.cpp:121-125) and Rml::Shutdown() nulls whatever is there
 * (Core.cpp:189), so the install has to be repeated for every boot and has to happen while
 * the slot is still empty -- otherwise the library allocates a default nobody wants.
 *
 * PROCESS-WIDE rather than per context (`Rml::CreateContext`'s 4th argument), matching the
 * system interface and the cursor latch: RmlUi's callbacks carry no context, so a per-context
 * handler would not actually let anyone attribute an activation any better -- the caret
 * arrives through the one global SystemInterface regardless. See
 * FVaCuusTextFieldState::Generation.
 *
 * Returns a reference with static storage duration; safe to install once per boot.
 */
Rml::TextInputHandler& GetRmlTextInputHandler();

/**
 * Identity of the field the platform IME is currently attached to, or 0 for none.
 *
 * Moves on activate, deactivate and destroy and nothing else -- it is the token every
 * queued mutation is stamped with. UI thread.
 */
uint64 GetActiveFieldGeneration();

/**
 * Would a click on this element put a caret in a text field (controller decision D14a)?
 *
 * `<textarea>`, or `<input>` whose `type` resolves to text or password -- RmlUi's own
 * dispatch set (ElementFormControlInput.cpp:95-118), which is exactly the set that owns a
 * WidgetTextInput and therefore a caret and a Rml::TextInputContext. UI thread.
 */
bool IsTextInputElement(const Rml::Element& Element);

/**
 * Publishes the shadow state for one view, from that view's OWN focused control.
 *
 * Returns true when a text control holds focus in this context, which is what
 * FVaCuusInteractiveSnapshot::bTextInputFocused reports (D14b). On false OutState is left
 * reset, so a view with no field never republishes another view's text.
 *
 * Must run on the UI thread AFTER Context::Update() -- it reads element boxes, and the
 * caret latch it samples can be written from inside Update().
 */
bool FillTextFieldState(Rml::Context& Context, FVaCuusTextFieldState& OutState);

/**
 * Applies one queued IME mutation to the routed view's context, or drops it and says why.
 *
 * DROPS on any of: no active Rml::TextInputContext (the element died, or focus left before
 * the queue drained), a stale generation stamp, or this view not being the one holding a
 * focused text control. All three are ordinary -- the queue is at least a frame behind the
 * focus changes it describes -- which is why they are Verbose rather than Warning.
 */
void ApplyMutation(Rml::Context& Context, uint32 ViewId, const FVaCuusInputEvent& Event);

//~ THE INDEX-SPACE BOUNDARY, and the only place it is crossed. Three spaces exist: RmlUi
//~ UTF-8 CHARACTER offsets (its whole public API), RmlUi UTF-8 BYTE offsets (its internals,
//~ e.g. WidgetTextInput::GetCompositionRange) and engine UTF-16 FString indices (what
//~ ITextInputMethodContext means by "code point index", matching Slate's own
//~ GetTextLength == FString::Len()). VaCuus never touches the byte space -- nothing on the
//~ public Rml::TextInputContext interface speaks it -- so exactly two conversions exist, and
//~ both are pure functions of the same string the other side used.

/** UTF-16 index into Text -> the RmlUi character offset naming the same position. */
int32 Utf16IndexToCharacterOffset(const FString& Text, int32 Utf16Index);

/** RmlUi character offset -> the UTF-16 index into Text naming the same position. */
int32 CharacterOffsetToUtf16Index(const FString& Text, int32 CharacterOffset);

/**
 * Smallest (BeginIndex, OldLength, NewLength) triple describing Old -> New, in UTF-16
 * indices. False when the strings are equal, so the caller can skip a degenerate notify.
 *
 * COPIED, NOT LINKED: the engine's identical helper is file-static and unexported
 * (SlateEditableTextLayoutIME::ComputeMinimalChangedRange, SlateEditableTextLayout.cpp:46-73).
 * It is what turns "the element's value changed" into the half-open changed span TSF's
 * NotifyTextChanged expects.
 */
bool ComputeMinimalChangedRange(
	const FString& Old, const FString& New, int32& OutBeginIndex, int32& OutOldLength, int32& OutNewLength);
}	 // namespace VaCuusTextInput

/**
 * The game-thread owner of one view's platform IME context: registers it, activates and
 * deactivates it, holds the change notifier, and turns each refreshed snapshot into the
 * notifications TSF expects.
 *
 * SHAPED AFTER FCEFImeHandler (CEFImeHandler.h:39-124), deliberately: CEF is the engine's
 * only other IME client that proxies a text model living somewhere else, so its split --
 * handler owns lifecycle, context owns the 14 answers -- is the shape that already survived
 * contact with TSF.
 *
 * D18: THE TEARDOWN IS `Shutdown()`, NOT THE DESTRUCTOR. `ITextInputMethodSystem` holds the
 * context by TSharedRef, so a context registered on focus keeps the platform pointing at a
 * view whose Slate widget may already be gone. Every teardown site therefore calls
 * Shutdown() explicitly -- SVaCuusWidget::DetachView, UVaCuusWidget::ReleaseSlateResources
 * (through it), UVaCuusView::Invalidate and BeginDestroy -- because the destructor can run
 * frames later, and a live composition would meanwhile call EndComposition() on a dead
 * owner (the CEF comment at CEFImeHandler.cpp:92).
 *
 * NO-OP WHEN THE PLATFORM HAS NO IME SYSTEM, which is the tested path here (controller
 * decision D16). `ITextInputMethodSystem` has exactly two implementations in the engine:
 * FWindowsApplication (WindowsApplication.h:390) and FMacApplication (MacApplication.h:198).
 * Everything else -- FLinuxApplication included -- inherits
 * `GenericApplication::GetTextInputMethodSystem()`'s `return NULL` (GenericApplication.h:550),
 * and Epic's own CEF IME handler is compiled out entirely on Linux for the same reason
 * (CEFImeHandler.h:7).
 *
 * WHAT THAT DEGRADES TO IS NOT THE SAME EVERYWHERE, and an earlier revision of this comment
 * said it was. It claimed, flatly, that with no IME system "typing degrades to
 * SVaCuusWidget::OnKeyChar -> EVaCuusInputEventKind::TextInput -> Context::ProcessTextInput".
 * That is true on a DESKTOP with no IME system (Linux, which is where it was written and
 * where it is tested), and it is false on a phone: with no hardware keyboard nothing ever
 * produces a character, so `OnKeyChar` never fires and the "degradation" delivers nothing at
 * all. There are three text paths, not two, and which of them is live is a property of the
 * platform's INPUT METHOD rather than of this interface's absence:
 *
 *   FPlatformApplicationMisc::RequiresVirtualKeyboard()   what actually types
 *   ---------------------------------------------------  ---------------------------------
 *   false, IME system present   (Win64, Mac)              this file: full composition
 *   false, IME system absent    (Linux; iPad + hardware   OnKeyChar -> ProcessTextInput;
 *                                keyboard)                 no composition
 *   true                        (Android, iPhone, iPad     IVirtualKeyboardEntry --
 *                                with no keyboard)          FVaCuusVirtualKeyboardEntry,
 *                                                           over in VaCuusRender
 *
 * The third row is the one this class cannot serve: the interface it implements does not
 * exist on those platforms, and the interface that does is in the Slate module, which is why
 * its bridge lives beside SVaCuusWidget instead of here. The arbitration between rows is the
 * engine's own -- see SlateEditableTextLayout.cpp:879-893.
 *
 * Game thread only.
 */
class FVaCuusImeHandler : public TSharedFromThis<FVaCuusImeHandler>
{
public:
	/** InView must outlive the handler minus its Shutdown(); UVaCuusView owns both ends. */
	static TSharedRef<FVaCuusImeHandler> Create(UVaCuusView& InView);

	~FVaCuusImeHandler();

	/**
	 * Where the surface is, whose window it is in, and whether the host holds Slate focus.
	 * Called once per game frame by the Slate host; cheap and idempotent when nothing moved.
	 *
	 * A moved surface fires NotifyLayoutChanged(Changed), which is what makes the OS
	 * candidate window follow a HUD that was resized or a viewport that was dragged.
	 */
	void UpdateSurface(const FVaCuusImeSurface& Surface);

	/**
	 * D14a: a press landed on a rect carrying EVaCuusRectFlags::TextInput, so activate the
	 * platform context on THIS click rather than on the snapshot that will confirm it next
	 * frame.
	 *
	 * SnapshotGeneration is the generation the press was answered from; the deactivation
	 * logic will not undo this hint until a STRICTLY NEWER snapshot reports no focused text
	 * control, so the one-frame window in which the snapshot still says "no field" cannot
	 * cancel the activation it is too old to know about.
	 */
	void NotifyTextInputClicked(uint64 SnapshotGeneration);

	/**
	 * Once per game frame, from UVaCuusView::PollStatus() right after the snapshot cache is
	 * refreshed: reconciles activation with bTextInputFocused and turns value/selection
	 * changes into TSF notifications.
	 */
	void OnSnapshotRefreshed(const FVaCuusInteractiveSnapshot& Snapshot);

	/** Deactivate, cancel any composition, unregister, and forget the view. Idempotent. */
	void Shutdown();

	/**
	 * Pushes one IME mutation onto the view's input queue. Called by the context's 14
	 * virtuals; public because the context is a separate class (defined in the .cpp) rather
	 * than a nested one, exactly like CEF's split.
	 *
	 * A no-op after Shutdown(), which is what makes a mutation the OS sends during teardown
	 * harmless instead of a crash.
	 */
	void QueueImeEvent(const FVaCuusInputEvent& Event);

	//~ Observables for VaCuus.Input.TextEntry and for diagnostics. All game thread.

	/** True once the context has been handed to the platform system. */
	bool IsRegistered() const { return bRegistered; }

	/** True while the platform system reports OUR context as the active one. */
	bool IsContextActive() const;

	/** True when the platform offers no IME system at all -- the Linux path (D16). */
	bool IsPlatformImeAbsent() const { return TextInputMethodSystem == nullptr; }

	/**
	 * The context itself, for the tests that drive the 14 virtuals directly.
	 *
	 * NON-NULL EVEN WHERE THE PLATFORM HAS NO IME, and that split is deliberate rather than a
	 * test affordance: answering the 14 virtuals needs only the published shadow state and the
	 * surface, both of which exist on every platform. What needs
	 * `ITextInputMethodSystem` is exclusively the HAND-OFF -- register, activate, notify. So on
	 * Linux the whole context is built, kept up to date and observable; it simply has nobody to
	 * hand itself to. That is what makes controller decision D16's "build the full path, test
	 * the degradation" mean something more than "compile the full path".
	 */
	ITextInputMethodContext* GetContextForTesting() const;

private:
	explicit FVaCuusImeHandler(UVaCuusView& InView);

	/**
	 * Creates the context if there is none, and registers it with the platform system if there
	 * is one -- including LATER, if the system only becomes available after the context was
	 * built (a widget re-parented into a real window). Idempotent.
	 */
	void EnsureContext();

	/**
	 * Activates the context. Creates it first if needed; a no-op with no platform system.
	 *
	 * bForce exists for D14a and for one specific reason: the press that focuses a field is
	 * answered with an FReply that grants Slate focus AFTER the handler returns, so at
	 * NotifyTextInputClicked() time bHostHasFocus is still false. Refusing to activate there
	 * would put the activation a frame late, which is the bug D14a exists to avoid. A forced
	 * activation is self-correcting: if the focus grant did not land, the next
	 * OnSnapshotRefreshed sees no focused text control, the hint has expired, and the context
	 * is deactivated again.
	 */
	void ActivateContext(bool bForce);

	/** Aborts a live composition first, then deactivates. Leaves the context registered. */
	void DeactivateContext();

	/**
	 * The view this handler belongs to, or null after Shutdown().
	 *
	 * A RAW BACK-POINTER, nulled on teardown -- the KillContext pattern from
	 * FSlateEditableTextLayout::FTextInputMethodContext (SlateEditableTextLayout.h:600-604).
	 * A TWeakObjectPtr would answer "is it garbage collected", which is the wrong question:
	 * what matters is whether this handler has been retired, and only Shutdown() knows that.
	 */
	UVaCuusView* View = nullptr;

	/**
	 * The platform's system, or null where there is none (Linux). Supplied by the Slate host
	 * rather than looked up, because FSlateApplication::GetTextInputMethodSystem() lives in
	 * the Slate module and this one deliberately does not depend on it -- everything here
	 * speaks ApplicationCore, which is where the whole IME interface lives anyway.
	 */
	ITextInputMethodSystem* TextInputMethodSystem = nullptr;

	/** Registered with the platform system, which holds it by TSharedRef. */
	TSharedPtr<FVaCuusTextInputMethodContext> Context;

	/** Handed back by RegisterContext(); may legitimately be null. */
	TSharedPtr<ITextInputMethodChangeNotifier> ChangeNotifier;

	bool bRegistered = false;

	/** The surface as last supplied; also the coordinate basis handed to a new context. */
	FVaCuusImeSurface PreviousSurface;

	/** Mirror of PreviousSurface.bHostHasFocus, read on every activation decision. */
	bool bHostHasFocus = false;

	/** Set by NotifyTextInputClicked; cleared by the first strictly newer snapshot. */
	bool bActivationHintPending = false;
	uint64 ActivationHintSnapshotGeneration = 0;

	/** Previous frame's shadow value and selection, for the minimal-diff notifications. */
	FString LastNotifiedValue;
	int32 LastNotifiedSelectionBegin = 0;
	int32 LastNotifiedSelectionEnd = 0;
	uint64 LastNotifiedFieldGeneration = 0;
};
