// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusTextInput.h"

#include "VaCuusDefines.h"
#include "VaCuusInputEvent.h"
#include "VaCuusUIThread.h"
#include "VaCuusView.h"

// Types.h FIRST, and this is not include hygiene for its own sake: TextInputHandler.h has
// ZERO #includes of its own yet uses RMLUICORE_API and NonCopyMoveable, so it only compiles
// when something has already pulled Header.h and Traits.h in. The Win32 backend gets away
// with it purely by alphabetical ordering.
#include <RmlUi/Core/Types.h>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/TextInputContext.h>
#include <RmlUi/Core/TextInputHandler.h>

namespace VaCuusTextInput
{
namespace
{
/**
 * The one RmlUi-side handler for the whole process.
 *
 * MIRRORS TextInputMethodEditor_Win32 (RmlUi_Platform_Win32.h:73-118), which is RmlUi's own
 * complete IME state machine, with the WM_IME_* driver swapped for the engine's
 * ITextInputMethodContext driver -- and mirrors
 * FSlateEditableTextLayout::FTextInputMethodContext::KillContext for lifetime: the
 * `Rml::TextInputContext*` below is RAW AND NON-OWNING, its lifetime ends at OnDestroy()
 * (~WidgetTextInputContext, WidgetTextInput.cpp:80-83, fired when the element is removed),
 * and the engine meanwhile holds a TSharedRef to our game-thread context that outlives it.
 * So OnDestroy nulls the pointer and every consumer early-outs on null.
 *
 * UI THREAD ONLY. Every callback arrives from inside Context::Update or one of the
 * Context::Process* calls the input drain makes (WidgetTextInput::ProcessEvent on
 * EventId::Focus / EventId::Blur, WidgetTextInput.cpp:670-690), all of which are UI-thread
 * by construction.
 */
class FVaCuusRmlTextInputHandler final : public Rml::TextInputHandler
{
public:
	//~ Begin Rml::TextInputHandler
	virtual void OnActivate(Rml::TextInputContext* InContext) override
	{
		if (InContext == nullptr)
		{
			return;
		}

		// A re-activation of the SAME field still bumps the generation: RmlUi fires Focus
		// again when a document is hidden and shown, and the platform will have been
		// deactivated in between, so any mutation queued against the old activation is stale.
		ActiveContext = InContext;
		CompositionBegin = 0;
		CompositionEnd = 0;
		++Generation;

		UE_LOG(LogVaCuus, Verbose, TEXT("IME: RmlUi activated a text field (field generation %llu)"), Generation);
	}

	virtual void OnDeactivate(Rml::TextInputContext* InContext) override
	{
		// Guarded on identity: RmlUi can blur a field that was never the active one (a
		// document with two inputs, focus moving between them, the events interleaving with
		// our own queue), and clearing on that would drop a live field's pointer.
		if (InContext != ActiveContext || ActiveContext == nullptr)
		{
			return;
		}

		Clear();
		UE_LOG(LogVaCuus, Verbose, TEXT("IME: RmlUi deactivated the text field (field generation %llu)"), Generation);
	}

	virtual void OnDestroy(Rml::TextInputContext* InContext) override
	{
		if (InContext != ActiveContext || ActiveContext == nullptr)
		{
			return;
		}

		// THE KillContext MOMENT: the object behind ActiveContext is being destructed right
		// now, so nulling here is what keeps every later ApplyMutation from dereferencing it.
		Clear();
		UE_LOG(LogVaCuus, Verbose, TEXT("IME: RmlUi destroyed the text field's context (field generation %llu)"), Generation);
	}
	//~ End Rml::TextInputHandler

	Rml::TextInputContext* GetActiveContext() const { return ActiveContext; }
	uint64 GetGeneration() const { return Generation; }

	void SetAppliedCompositionRange(int32 InBegin, int32 InEnd)
	{
		CompositionBegin = InBegin;
		CompositionEnd = InEnd;
	}

	void GetAppliedCompositionRange(int32& OutBegin, int32& OutEnd) const
	{
		OutBegin = CompositionBegin;
		OutEnd = CompositionEnd;
	}

private:
	void Clear()
	{
		ActiveContext = nullptr;
		CompositionBegin = 0;
		CompositionEnd = 0;
		++Generation;
	}

	/** Raw, non-owning, UI thread only. Null between fields and after OnDestroy. */
	Rml::TextInputContext* ActiveContext = nullptr;

	/**
	 * The composing span this handler last pushed into RmlUi, in RmlUi CHARACTER offsets.
	 *
	 * TRACKED RATHER THAN READ BACK, because it cannot be read back: `Rml::TextInputContext`
	 * has a SetCompositionRange and no getter, and the only getter that exists
	 * (WidgetTextInput::GetCompositionRange, which returns raw BYTE offsets) is on an internal
	 * class the public API never exposes. That absence is also why VaCuus never crosses the
	 * byte-offset index space at all -- see the conversion note in the header.
	 */
	int32 CompositionBegin = 0;
	int32 CompositionEnd = 0;

	/** Never resets; see FVaCuusTextFieldState::Generation for what it is a token for. */
	uint64 Generation = 0;
};

FVaCuusRmlTextInputHandler& GetHandler()
{
	// Function-local static: initialized exactly once even if two threads race the first
	// call, and it must outlive Rml::Shutdown() because RmlUi keeps the raw pointer we gave
	// it until then (Core.cpp:189 nulls the slot, it does not own the object).
	static FVaCuusRmlTextInputHandler Handler;
	return Handler;
}

/** UTF-8 character offset into an Rml::String -> the UTF-16 index naming the same position. */
int32 Utf8CharacterOffsetToUtf16Index(const Rml::String& Utf8Value, int32 CharacterOffset)
{
	if (CharacterOffset <= 0)
	{
		return 0;
	}

	// RmlUi's own converter, so a malformed or clamped offset behaves exactly as it does
	// inside the library rather than as our own reimplementation would.
	const int32 ByteOffset = Rml::StringUtilities::ConvertCharacterOffsetToByteOffset(Utf8Value, CharacterOffset);
	if (ByteOffset <= 0)
	{
		return 0;
	}

	// The prefix's UTF-16 length IS the index: decoding [0, ByteOffset) is the only way to
	// count surrogate pairs correctly, and it is also what makes this exact rather than an
	// "assume BMP" approximation. Costs one conversion per publish, and only while a field
	// is focused.
	const Rml::String Prefix(Utf8Value, 0, SIZE_T(ByteOffset));
	return FString(UTF8_TO_TCHAR(Prefix.c_str())).Len();
}

/**
 * This element's selection as RmlUi CHARACTER offsets, or false if it has none.
 *
 * `GetSelection` is NOT on Rml::ElementFormControl -- it exists separately on
 * ElementFormControlInput (ElementFormControlInput.h:47) and ElementFormControlTextArea
 * (ElementFormControlTextArea.h:72), because only those two own a WidgetTextInput. Hence two
 * casts rather than one. The values come back as character offsets: WidgetTextInput stores
 * bytes and converts on the way out (WidgetTextInput.cpp:365-369).
 */
bool GetSelectionCharacterRange(Rml::Element& Element, int32& OutBegin, int32& OutEnd)
{
	int Begin = 0;
	int End = 0;

	if (Rml::ElementFormControlInput* const Input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(&Element))
	{
		Input->GetSelection(&Begin, &End, nullptr);
	}
	else if (Rml::ElementFormControlTextArea* const TextArea =
				 rmlui_dynamic_cast<Rml::ElementFormControlTextArea*>(&Element))
	{
		TextArea->GetSelection(&Begin, &End, nullptr);
	}
	else
	{
		return false;
	}

	OutBegin = int32(Begin);
	OutEnd = int32(End);
	return true;
}

/** The element's border box in view pixels; the same two paths the snapshot DFS takes. */
FIntRect GetElementViewRect(Rml::Element& Element)
{
	Rml::Vector2f Position;
	Rml::Vector2f Size;

	if (Element.GetTransformState() != nullptr)
	{
		// GetAbsoluteOffset() is UNTRANSFORMED, so a transformed element has to go the
		// expensive way and have its corners projected (ElementUtilities.cpp:235-275).
		Rml::Rectanglef Bounds;
		if (!Rml::ElementUtilities::GetBoundingBox(Bounds, &Element, Rml::BoxArea::Border))
		{
			return FIntRect(0, 0, 0, 0);
		}

		Position = Bounds.Position();
		Size = Bounds.Size();
	}
	else
	{
		Position = Element.GetAbsoluteOffset(Rml::BoxArea::Border);
		Size = Element.GetBox().GetSize(Rml::BoxArea::Border);
	}

	return FIntRect(FMath::FloorToInt(Position.x), FMath::FloorToInt(Position.y),
		FMath::CeilToInt(Position.x + Size.x), FMath::CeilToInt(Position.y + Size.y));
}
}	 // namespace

Rml::TextInputHandler& GetRmlTextInputHandler()
{
	return GetHandler();
}

uint64 GetActiveFieldGeneration()
{
	check(FVaCuusUIThread::IsInUIThread());
	return GetHandler().GetGeneration();
}

bool IsTextInputElement(const Rml::Element& Element)
{
	// Tags are lowercased by the parser before instancing (XMLParser.cpp:136,167), so a
	// straight compare is right here -- unlike attribute NAMES, which pass through verbatim.
	const Rml::String& Tag = Element.GetTagName();
	if (Tag == "textarea")
	{
		return true;
	}
	if (Tag != "input")
	{
		return false;
	}

	// RmlUi's own dispatch: anything it does not recognise falls back to "text"
	// (ElementFormControlInput.cpp:95-118), so the DEFAULT for a bare <input> is a text
	// field and the NEGATIVE list -- not a positive one -- is what matches the library. A
	// positive list would silently miss every plain <input>, which is the common case.
	const Rml::String Type = Element.GetAttribute<Rml::String>("type", Rml::String("text"));
	return Type != "radio" && Type != "checkbox" && Type != "range" && Type != "submit" && Type != "button";
}

bool FillTextFieldState(Rml::Context& Context, FVaCuusTextFieldState& OutState)
{
	check(FVaCuusUIThread::IsInUIThread());

	// Reset first and unconditionally: every early return below must leave the state saying
	// "no field", or a view that just lost its field would keep publishing the old text.
	OutState.Reset();

	Rml::Element* const Focus = Context.GetFocusElement();
	if (Focus == nullptr || !IsTextInputElement(*Focus))
	{
		return false;
	}

	Rml::ElementFormControl* const Control = rmlui_dynamic_cast<Rml::ElementFormControl*>(Focus);
	if (Control == nullptr)
	{
		// An element that looks like an <input> by tag but is not a form control: an author
		// can register their own element under that tag name. Not a text field.
		return false;
	}

	// READ FROM THIS VIEW'S OWN FOCUSED ELEMENT, not from the process-wide handler, and that
	// is the point: `Rml::TextInputContext` exposes no way to read the text or its length at
	// all (TextInputContext.h has no GetText/GetLength), so an embedder must either shadow
	// only the composition sequence the way CEF does, or recover the element and ask it. The
	// second is both more correct AND exact per view -- with several views up, each publishes
	// its own field rather than whichever one the global handler activated last.
	const Rml::String Value = Control->GetValue();
	OutState.Value = UTF8_TO_TCHAR(Value.c_str());

	// INDEX-SPACE CONVERSION SITE 1: RmlUi character offsets -> engine UTF-16 indices.
	int32 SelectionBeginChars = 0;
	int32 SelectionEndChars = 0;
	if (GetSelectionCharacterRange(*Focus, SelectionBeginChars, SelectionEndChars))
	{
		OutState.SelectionBegin = Utf8CharacterOffsetToUtf16Index(Value, SelectionBeginChars);
		OutState.SelectionEnd = Utf8CharacterOffsetToUtf16Index(Value, SelectionEndChars);
	}
	else
	{
		// No selection API: park the caret at the end, which is where RmlUi puts it on focus.
		OutState.SelectionBegin = OutState.Value.Len();
		OutState.SelectionEnd = OutState.SelectionBegin;
	}

	// INDEX-SPACE CONVERSION SITE 2: the composing span we last applied, same direction.
	int32 CompositionBeginChars = 0;
	int32 CompositionEndChars = 0;
	GetHandler().GetAppliedCompositionRange(CompositionBeginChars, CompositionEndChars);
	if (CompositionEndChars > CompositionBeginChars)
	{
		OutState.CompositionBegin = Utf8CharacterOffsetToUtf16Index(Value, CompositionBeginChars);
		OutState.CompositionEnd = Utf8CharacterOffsetToUtf16Index(Value, CompositionEndChars);
	}

	// RmlUi has no IsReadOnly(): `disabled` is an ElementFormControl-level query (it walks the
	// element and its ancestors, ElementFormControl.h:42 -- NOT on Rml::Element) and `readonly`
	// is a plain attribute InputTypeText honours by refusing edits. Both mean "the IME must not
	// compose here", so both map onto the one flag TSF asks about.
	OutState.bReadOnly = Control->IsDisabled() || Focus->HasAttribute("readonly");

	OutState.BoundingBox = GetElementViewRect(*Focus);

	// LAST, and after Update(): the caret latch is written from inside RmlUi -- from
	// ShowCursor(true), which fires during layout as well as during input dispatch -- so
	// sampling it at the end of the frame is what gets this frame's value.
	//
	// The serial is deliberately NOT compared here. The gate that matters is the one already
	// passed above ("MY context has a focused text control"), which is stronger than a serial
	// window: a view with no field publishes no caret at all. The residual is the pathological
	// case RmlUi's API cannot express -- two views each holding a focused text field, where
	// ActivateKeyboard carries no context and the last writer wins. Documented on
	// GetVaCuusLatchedCaret; harmless because the platform has one active IME context anyway.
	uint64 CaretSerial = 0;
	const FVaCuusCaretLatch Caret = GetVaCuusLatchedCaret(CaretSerial);
	if (Caret.bActive)
	{
		OutState.CaretPosition = Caret.Position;
		OutState.CaretLineHeight = Caret.LineHeight;
		OutState.bCaretValid = true;
	}

	// Stamped last, so a reader that sees a non-zero generation sees a filled state.
	OutState.Generation = GetHandler().GetGeneration();
	return true;
}

void ApplyMutation(Rml::Context& Context, uint32 ViewId, const FVaCuusInputEvent& Event)
{
	check(FVaCuusUIThread::IsInUIThread());

	FVaCuusRmlTextInputHandler& Handler = GetHandler();
	Rml::TextInputContext* const Target = Handler.GetActiveContext();

	if (Target == nullptr)
	{
		UE_LOG(LogVaCuus, Verbose,
			TEXT("View %u: IME mutation dropped -- no active RmlUi text field (the element was blurred or removed ")
			TEXT("while the mutation was in the queue)"),
			ViewId);
		return;
	}

	if (Event.FieldGeneration != Handler.GetGeneration())
	{
		// The reason the stamp exists. WidgetTextInput::SetSelectionRange early-returns when
		// the element is not focused (WidgetTextInput.cpp:330-331), so without this a stale
		// pair of mutations would half-apply: the SetText lands, the SetSelectionRange next to
		// it is silently swallowed, and the caret ends up somewhere the OS does not believe.
		UE_LOG(LogVaCuus, Verbose,
			TEXT("View %u: IME mutation dropped -- stamped field generation %llu but the active field is %llu"),
			ViewId, Event.FieldGeneration, Handler.GetGeneration());
		return;
	}

	// The active context is process-wide (RmlUi's callbacks carry no context), so confirm the
	// ROUTED view is the one holding a focused text control before touching it. Without this
	// a mutation routed to view A could be applied to view B's field.
	Rml::Element* const Focus = Context.GetFocusElement();
	if (Focus == nullptr || !IsTextInputElement(*Focus))
	{
		UE_LOG(LogVaCuus, Verbose,
			TEXT("View %u: IME mutation dropped -- this view has no focused text control, so the active field ")
			TEXT("belongs to another view"),
			ViewId);
		return;
	}

	switch (Event.Kind)
	{
		case EVaCuusInputEventKind::ImeSetTextInRange:
		{
			// The Rml::String is a NAMED LOCAL on purpose: StringView is non-owning
			// (StringUtilities.h:127), so handing it a temporary's data would be a
			// use-after-free the moment the argument list ends.
			const Rml::String Utf8Text(TCHAR_TO_UTF8(*Event.Text));
			Target->SetText(Rml::StringView(Utf8Text), Event.RangeBegin, Event.RangeEnd);

			// SetText does NOT move the caret (TextInputContext.h:44-49 replaces the range and
			// nothing else), and TSF expects it after the inserted text -- the same place
			// WidgetTextInputContext::SetCursorPosition would put it.
			const int32 CaretChars =
				Event.RangeBegin + int32(Rml::StringUtilities::LengthUTF8(Rml::StringView(Utf8Text)));
			Target->SetCursorPosition(CaretChars);
			break;
		}

		case EVaCuusInputEventKind::ImeSetSelectionRange:
			Target->SetSelectionRange(Event.RangeBegin, Event.RangeEnd);

			// RmlUi's selection is a range plus a separate cursor, and SetSelectionRange leaves
			// the cursor at `end` unconditionally (WidgetTextInput.cpp:334-336). TSF's
			// ECaretPosition::Beginning means the caret is at the other end -- e.g. after a
			// shift-left selection -- so it has to be pushed back explicitly or a subsequent
			// extend-selection grows the wrong way.
			Target->SetCursorPosition(Event.bCaretAtRangeBeginning ? Event.RangeBegin : Event.RangeEnd);
			break;

		case EVaCuusInputEventKind::ImeSetCompositionRange:
			Target->SetCompositionRange(Event.RangeBegin, Event.RangeEnd);

			// Remembered because it cannot be read back through the public interface, and
			// because CommitComposition below needs a live range to act on at all.
			Handler.SetAppliedCompositionRange(Event.RangeBegin, Event.RangeEnd);
			break;

		case EVaCuusInputEventKind::ImeCommitComposition:
		{
			// THE RANGE IS RE-ASSERTED FIRST, AND IT IS NOT REDUNDANT: CommitComposition reads
			// RmlUi's own composition range and TAKES NO ACTION when it is [0, 0]
			// (WidgetTextInput.cpp:128-131). A commit that follows an empty or already-cleared
			// range would therefore silently drop the composed text.
			if (Event.RangeEnd > Event.RangeBegin)
			{
				Target->SetCompositionRange(Event.RangeBegin, Event.RangeEnd);
			}

			const Rml::String Utf8Text(TCHAR_TO_UTF8(*Event.Text));

			// CommitComposition rather than SetText, and this is the whole reason the two kinds
			// are distinct: only the commit path respects the element's maxlength
			// (WidgetTextInput.cpp:134-150), and the commit is exactly when it has to hold.
			Target->CommitComposition(Rml::StringView(Utf8Text));

			// Clear the underline; the composed text is ordinary content now.
			Target->SetCompositionRange(0, 0);
			Handler.SetAppliedCompositionRange(0, 0);

			const int32 CaretChars =
				Event.RangeBegin + int32(Rml::StringUtilities::LengthUTF8(Rml::StringView(Utf8Text)));
			Target->SetCursorPosition(CaretChars);
			break;
		}

		default:
			checkNoEntry();
			break;
	}
}

int32 Utf16IndexToCharacterOffset(const FString& Text, int32 Utf16Index)
{
	const int32 Clamped = FMath::Clamp(Utf16Index, 0, Text.Len());

	// One code point per unit EXCEPT a trailing surrogate, so counting the units that are not
	// low surrogates is the whole conversion. An unpaired low surrogate (which Slate can
	// deliver) is skipped rather than counted, which is the same choice UTF8CHAR conversion
	// makes: it is not a character on its own.
	int32 Offset = 0;
	for (int32 Index = 0; Index < Clamped; ++Index)
	{
		const uint32 Unit = uint32(Text[Index]);
		if (Unit < 0xDC00 || Unit > 0xDFFF)
		{
			++Offset;
		}
	}

	return Offset;
}

int32 CharacterOffsetToUtf16Index(const FString& Text, int32 CharacterOffset)
{
	if (CharacterOffset <= 0)
	{
		return 0;
	}

	int32 Remaining = CharacterOffset;
	for (int32 Index = 0; Index < Text.Len(); ++Index)
	{
		const uint32 Unit = uint32(Text[Index]);
		if (Unit < 0xDC00 || Unit > 0xDFFF)
		{
			if (Remaining == 0)
			{
				return Index;
			}
			--Remaining;
		}
	}

	// Past the end: clamp. TSF does ask for offsets one past the last character (that is
	// where a caret at the end lives), so this is the normal case rather than an error.
	return Text.Len();
}

bool ComputeMinimalChangedRange(
	const FString& Old, const FString& New, int32& OutBeginIndex, int32& OutOldLength, int32& OutNewLength)
{
	if (Old.Equals(New, ESearchCase::CaseSensitive))
	{
		// Skipping a degenerate notify is the caller's whole reason for asking.
		return false;
	}

	const int32 OldLen = Old.Len();
	const int32 NewLen = New.Len();

	int32 Prefix = 0;
	while (Prefix < OldLen && Prefix < NewLen && Old[Prefix] == New[Prefix])
	{
		++Prefix;
	}

	int32 Suffix = 0;
	while (Suffix < OldLen - Prefix && Suffix < NewLen - Prefix && Old[OldLen - Suffix - 1] == New[NewLen - Suffix - 1])
	{
		++Suffix;
	}

	OutBeginIndex = Prefix;
	OutOldLength = OldLen - Prefix - Suffix;
	OutNewLength = NewLen - Prefix - Suffix;
	return true;
}
}	 // namespace VaCuusTextInput

/**
 * The 14 pure virtuals, answered from the shadow state and never from RmlUi.
 *
 * WHAT IT DOES NOT DO, said plainly because the alternative is a hang: it never blocks, never
 * waits on the UI thread and never calls into RmlUi. Every read comes out of the snapshot
 * UVaCuusView already caches (one UI frame old, at most), and every write is a queued input
 * event stamped with the field generation.
 *
 * THE ONE VIRTUAL THAT CANNOT BE ANSWERED HONESTLY is GetCharacterIndexFromPoint -- see its
 * implementation. It returns INDEX_NONE always.
 *
 * Private inheritance of the overrides mirrors FCEFTextInputMethodContext: only IsComposing()
 * has a caller outside the platform system.
 */
class FVaCuusTextInputMethodContext final : public ITextInputMethodContext
{
public:
	explicit FVaCuusTextInputMethodContext(FVaCuusImeHandler& InOwner)
		: Owner(&InOwner)
	{
	}

	/** The KillContext moment: after this every override answers as "empty and read-only". */
	void KillContext()
	{
		Owner = nullptr;
		bIsComposing = false;
		State.Reset();
	}

	/** Called once per game frame by the handler with the newest published shadow state. */
	void SetShadowState(const FVaCuusTextFieldState& InState) { State = InState; }

	/** Called by the handler when the surface moved; the two coordinate bases live here. */
	void SetSurface(const FVaCuusImeSurface& InSurface) { Surface = InSurface; }

	const FVaCuusTextFieldState& GetShadowState() const { return State; }

	/**
	 * Abandons a composition without committing it -- the teardown path.
	 *
	 * Clears RmlUi's underline (an empty range) and drops our own composing flag, but does
	 * NOT commit: aborting means the composed text was never accepted. Must run BEFORE
	 * DeactivateContext, or the IME calls EndComposition() on a dying owner (the CEF comment
	 * at CEFImeHandler.cpp:92).
	 */
	void AbortComposition()
	{
		if (!bIsComposing)
		{
			return;
		}

		bIsComposing = false;
		QueueCompositionRange(0, 0);
		CompositionBeginIndex = 0;
		CompositionEndIndex = 0;
	}

	//~ Begin ITextInputMethodContext
	virtual bool IsComposing() override { return bIsComposing; }

	virtual bool IsReadOnly() override { return Owner == nullptr || State.bReadOnly; }

	virtual uint32 GetTextLength() override { return uint32(State.Value.Len()); }

	virtual void GetSelectionRange(uint32& OutBeginIndex, uint32& OutLength, ECaretPosition& OutCaretPosition) override
	{
		const int32 Begin = FMath::Clamp(State.SelectionBegin, 0, State.Value.Len());
		const int32 End = FMath::Clamp(State.SelectionEnd, Begin, State.Value.Len());

		OutBeginIndex = uint32(Begin);
		OutLength = uint32(End - Begin);

		// RmlUi keeps a selection anchor and a cursor but exposes only the ordered pair
		// (WidgetTextInput::GetSelection normalises), so which END the caret is at is not
		// recoverable. Ending is the answer that is right for every forward selection and for
		// every bare caret, i.e. for everything except a backwards drag -- and being wrong
		// there costs a shift-arrow extending from the far end, not corruption.
		OutCaretPosition = ECaretPosition::Ending;
	}

	virtual void SetSelectionRange(const uint32 InBeginIndex, const uint32 InLength, const ECaretPosition InCaretPosition) override
	{
		QueueSelectionRange(
			int32(InBeginIndex), int32(InBeginIndex + InLength), InCaretPosition == ECaretPosition::Beginning);
	}

	virtual void GetTextInRange(const uint32 InBeginIndex, const uint32 InLength, FString& OutString) override
	{
		const int32 Begin = FMath::Clamp(int32(InBeginIndex), 0, State.Value.Len());
		const int32 Length = FMath::Clamp(int32(InLength), 0, State.Value.Len() - Begin);
		OutString = State.Value.Mid(Begin, Length);
	}

	virtual void SetTextInRange(const uint32 InBeginIndex, const uint32 InLength, const FString& InString) override
	{
		QueueText(EVaCuusInputEventKind::ImeSetTextInRange, int32(InBeginIndex), int32(InBeginIndex + InLength), InString);
	}

	virtual int32 GetCharacterIndexFromPoint(const FVector2D& InPoint) override
	{
		// HONESTLY UNANSWERABLE AT RmlUi 0ae381e, so it says so rather than guessing.
		//
		// There is no public hit test from a point to a character index in a text field:
		// Rml::TextInputContext has no such method, ElementText exposes only GetLines() with
		// per-line positions (ElementText.h:59) and ElementUtilities offers GetStringWidth
		// (ElementUtilities.h:58) -- from which a correct answer would need us to
		// re-implement RmlUi's own line breaking and per-glyph advance, on the game thread,
		// against a value that is a frame old. CEF degrades by scanning cached per-character
		// rects its renderer pushes over (CEFTextInputMethodContext.cpp:141-157); RmlUi
		// publishes no such rects.
		//
		// INDEX_NONE is the interface's documented "none found" answer
		// (ITextInputMethodSystem.h:88). What it costs is IME reconversion and clicking
		// inside a candidate window to reposition the caret -- both optional TSF features.
		// What it does not cost is composition, commit, or caret placement.
		return INDEX_NONE;
	}

	virtual bool GetTextBounds(const uint32 InBeginIndex, const uint32 InLength, FVector2D& OutPosition, FVector2D& OutSize) override
	{
		// Note the inverted return: TRUE means "the range is drawn CLIPPED", i.e. "this is not
		// a usable rect for you" (ITextInputMethodSystem.h:99, and CEF returns false for the
		// good case, CEFTextInputMethodContext.cpp:208).
		if (!State.bCaretValid || Surface.ViewPixelSize.X <= 0 || Surface.ViewPixelSize.Y <= 0)
		{
			GetScreenBounds(OutPosition, OutSize);
			return true;
		}

		// Only the CARET is known, not an arbitrary range: RmlUi's single spatial signal is
		// ActivateKeyboard(caret_position, line_height). For a non-empty range the caret rect
		// is still the right anchor for a candidate window -- it is where the composition is
		// happening -- so it is returned, and `true` tells the caller not to treat it as an
		// exact measurement of those characters.
		const bool bIsRange = InLength > 0;

		OutPosition = ViewToAbsolute(FVector2D(State.CaretPosition));
		OutSize = FVector2D(
			FMath::Max(1.0, double(ViewToAbsoluteScale().X)), double(State.CaretLineHeight) * ViewToAbsoluteScale().Y);
		return bIsRange;
	}

	virtual void GetScreenBounds(FVector2D& OutPosition, FVector2D& OutSize) override
	{
		// Verbatim the Slate precedent (FSlateEditableTextLayout::FTextInputMethodContext::
		// GetScreenBounds, SlateEditableTextLayout.cpp:4222-4233), including the explicit
		// FVector2D wrap the FVector2f geometry members need.
		OutPosition = Surface.AbsolutePosition;
		OutSize = Surface.AbsoluteSize;
	}

	virtual TSharedPtr<FGenericWindow> GetWindow() override { return Surface.NativeWindow; }

	virtual void BeginComposition() override
	{
		bIsComposing = true;

		// The composition starts where the caret is, which is the shadow selection -- TSF
		// composes over the current selection when there is one. Captured now because the
		// range TSF sends to UpdateCompositionRange is relative to the whole text, and the
		// commit needs to know which span to replace even if the selection has moved since.
		CompositionBeginIndex = FMath::Clamp(State.SelectionBegin, 0, State.Value.Len());
		CompositionEndIndex = FMath::Clamp(State.SelectionEnd, CompositionBeginIndex, State.Value.Len());
	}

	virtual void UpdateCompositionRange(const int32 InBeginIndex, const uint32 InLength) override
	{
		if (!bIsComposing)
		{
			// TSF should not send this outside a composition; if it does, honouring it would
			// underline text nobody is composing.
			UE_LOG(LogVaCuus, Verbose, TEXT("IME: UpdateCompositionRange outside a composition; ignored"));
			return;
		}

		CompositionBeginIndex = FMath::Clamp(InBeginIndex, 0, State.Value.Len());
		CompositionEndIndex = FMath::Clamp(InBeginIndex + int32(InLength), CompositionBeginIndex, State.Value.Len());
		QueueCompositionRange(CompositionBeginIndex, CompositionEndIndex);
	}

	virtual void EndComposition() override
	{
		if (!bIsComposing)
		{
			return;
		}

		// The composed text is whatever is in the field's composing span RIGHT NOW: TSF has
		// been writing it through SetTextInRange all along, so the commit re-asserts that span
		// rather than carrying its own string. Taken from the shadow value, which is why the
		// clamps above matter.
		const int32 Begin = FMath::Clamp(CompositionBeginIndex, 0, State.Value.Len());
		const int32 End = FMath::Clamp(CompositionEndIndex, Begin, State.Value.Len());
		const FString Composed = State.Value.Mid(Begin, End - Begin);

		// bIsComposing is cleared BEFORE the queue call so the notifier suppression in the
		// handler stops applying from here on -- but note the queued event is still stamped
		// with the same generation, so ordering against the mutations TSF already sent holds.
		bIsComposing = false;

		QueueText(EVaCuusInputEventKind::ImeCommitComposition, Begin, End, Composed);

		CompositionBeginIndex = 0;
		CompositionEndIndex = 0;
	}
	//~ End ITextInputMethodContext

private:
	/** Nulled by KillContext(); every queue call early-outs on it. */
	FVaCuusImeHandler* Owner = nullptr;

	/** The published shadow, refreshed once per game frame. */
	FVaCuusTextFieldState State;

	/** Where the surface is; the only coordinate basis this class has. */
	FVaCuusImeSurface Surface;

	bool bIsComposing = false;

	/** The composing span in UTF-16 indices, owned here because TSF drives it. */
	int32 CompositionBeginIndex = 0;
	int32 CompositionEndIndex = 0;

	/** View pixels -> Slate absolute units, per axis. See FVaCuusImeSurface. */
	FVector2D ViewToAbsoluteScale() const
	{
		if (Surface.ViewPixelSize.X <= 0 || Surface.ViewPixelSize.Y <= 0)
		{
			return FVector2D(1.0, 1.0);
		}

		return FVector2D(
			Surface.AbsoluteSize.X / double(Surface.ViewPixelSize.X), Surface.AbsoluteSize.Y / double(Surface.ViewPixelSize.Y));
	}

	FVector2D ViewToAbsolute(const FVector2D& ViewPixel) const
	{
		const FVector2D Scale = ViewToAbsoluteScale();
		return Surface.AbsolutePosition + FVector2D(ViewPixel.X * Scale.X, ViewPixel.Y * Scale.Y);
	}

	//~ The three queue helpers. All of them convert the engine's UTF-16 indices into RmlUi
	//~ character offsets HERE, on the game thread, against the shadow value the OS's indices
	//~ came from -- see FVaCuusInputEvent::RangeBegin for why the UI thread is the wrong place.

	void QueueSelectionRange(int32 Utf16Begin, int32 Utf16End, bool bCaretAtBeginning);
	void QueueText(EVaCuusInputEventKind Kind, int32 Utf16Begin, int32 Utf16End, const FString& Text);
	void QueueCompositionRange(int32 Utf16Begin, int32 Utf16End);
};

//~ The queue helpers are out of line because they need UVaCuusView's full definition, which
//~ in turn holds a TSharedPtr<FVaCuusImeHandler> -- the include cycle only closes here.

void FVaCuusTextInputMethodContext::QueueSelectionRange(int32 Utf16Begin, int32 Utf16End, bool bCaretAtBeginning)
{
	if (Owner == nullptr)
	{
		return;
	}

	// INDEX-SPACE CONVERSION SITE 3 (and 4): engine UTF-16 -> RmlUi character offsets.
	const int32 BeginChars = VaCuusTextInput::Utf16IndexToCharacterOffset(State.Value, Utf16Begin);
	const int32 EndChars = VaCuusTextInput::Utf16IndexToCharacterOffset(State.Value, Utf16End);

	Owner->QueueImeEvent(
		FVaCuusInputEvent::ImeSetSelectionRange(State.Generation, BeginChars, EndChars, bCaretAtBeginning));
}

void FVaCuusTextInputMethodContext::QueueText(
	EVaCuusInputEventKind Kind, int32 Utf16Begin, int32 Utf16End, const FString& Text)
{
	if (Owner == nullptr)
	{
		return;
	}

	const int32 BeginChars = VaCuusTextInput::Utf16IndexToCharacterOffset(State.Value, Utf16Begin);
	const int32 EndChars = VaCuusTextInput::Utf16IndexToCharacterOffset(State.Value, Utf16End);

	if (Kind == EVaCuusInputEventKind::ImeCommitComposition)
	{
		Owner->QueueImeEvent(FVaCuusInputEvent::ImeCommitComposition(State.Generation, BeginChars, EndChars, Text));
	}
	else
	{
		Owner->QueueImeEvent(FVaCuusInputEvent::ImeSetTextInRange(State.Generation, BeginChars, EndChars, Text));
	}
}

void FVaCuusTextInputMethodContext::QueueCompositionRange(int32 Utf16Begin, int32 Utf16End)
{
	if (Owner == nullptr)
	{
		return;
	}

	const int32 BeginChars = VaCuusTextInput::Utf16IndexToCharacterOffset(State.Value, Utf16Begin);
	const int32 EndChars = VaCuusTextInput::Utf16IndexToCharacterOffset(State.Value, Utf16End);

	Owner->QueueImeEvent(FVaCuusInputEvent::ImeSetCompositionRange(State.Generation, BeginChars, EndChars));
}

TSharedRef<FVaCuusImeHandler> FVaCuusImeHandler::Create(UVaCuusView& InView)
{
	// Private ctor + static Create, like both engine references: the platform system takes
	// the context by TSharedRef, so nothing here may ever exist on the stack.
	return MakeShareable(new FVaCuusImeHandler(InView));
}

FVaCuusImeHandler::FVaCuusImeHandler(UVaCuusView& InView)
	: View(&InView)
{
	check(IsInGameThread());
}

FVaCuusImeHandler::~FVaCuusImeHandler()
{
	// The safety net, not the intended path -- see the class comment on D18. If this is the
	// first teardown the view leaked a detach somewhere, and Shutdown() says so by doing the
	// work here (frames late) instead of not at all.
	Shutdown();
}

void FVaCuusImeHandler::QueueImeEvent(const FVaCuusInputEvent& Event)
{
	check(IsInGameThread());

	// Silently dropped after Shutdown(): the platform can and does call into a context it has
	// not finished letting go of, and there is nowhere left to send the event.
	if (View != nullptr)
	{
		View->SendInput(Event);
	}
}

void FVaCuusImeHandler::UpdateSurface(const FVaCuusImeSurface& InSurface)
{
	check(IsInGameThread());

	if (View == nullptr)
	{
		return;
	}

	// LOGGED ONCE, AND LOUDLY, because it is the whole reason Task 9's tested behaviour is
	// the degradation (controller decision D16): FLinuxApplication never overrides
	// GenericApplication::GetTextInputMethodSystem(), so it returns null, and Epic's own CEF
	// IME handler is compiled out on Linux for the same reason (CEFImeHandler.h:7). Without a
	// line here the platform difference is invisible and "IME does nothing" looks like a bug.
	static bool bLoggedPlatformSupport = false;
	if (!bLoggedPlatformSupport)
	{
		bLoggedPlatformSupport = true;
		if (InSurface.TextInputMethodSystem == nullptr)
		{
			UE_LOG(LogVaCuus, Warning,
				TEXT("IME: this platform exposes no ITextInputMethodSystem (GetTextInputMethodSystem() returned null), ")
				TEXT("so composition is unavailable and text entry degrades to OnKeyChar -> Rml::Context::ProcessTextInput. ")
				TEXT("Only FWindowsApplication and FMacApplication implement it; FLinuxApplication does not."));
		}
		else
		{
			UE_LOG(LogVaCuus, Log, TEXT("IME: platform ITextInputMethodSystem present; composition is available"));
		}
	}

	const bool bSystemChanged = TextInputMethodSystem != InSurface.TextInputMethodSystem;
	TextInputMethodSystem = InSurface.TextInputMethodSystem;

	const bool bMoved = !PreviousSurface.AbsolutePosition.Equals(InSurface.AbsolutePosition) ||
		!PreviousSurface.AbsoluteSize.Equals(InSurface.AbsoluteSize) ||
		PreviousSurface.ViewPixelSize != InSurface.ViewPixelSize || PreviousSurface.NativeWindow != InSurface.NativeWindow;

	const bool bLostHostFocus = bHostHasFocus && !InSurface.bHostHasFocus;

	PreviousSurface = InSurface;
	bHostHasFocus = InSurface.bHostHasFocus;

	// THE CONTEXT IS BUILT ON EVERY PLATFORM, whether or not there is a platform system to hand
	// it to -- see GetContextForTesting() for why that split is the honest one. Everything below
	// that needs the system is individually gated on it, and on Linux they all become no-ops
	// while the context itself stays live and correct.
	EnsureContext();
	Context->SetSurface(InSurface);

	// EDGE-TRIGGERED, and here rather than in the per-frame reconcile: losing Slate focus means
	// keys stop arriving AT ALL, so the OS must be told immediately -- waiting for the next
	// snapshot poll would leave a candidate window floating over a game that has taken the
	// keyboard back. The activation hint is dropped too, or the reconcile would revive a context
	// nothing can type into.
	if (bLostHostFocus)
	{
		bActivationHintPending = false;
		DeactivateContext();
	}

	// A moved or resized surface is a layout change as far as TSF is concerned: it is what makes
	// the candidate window follow a dragged window or a resized viewport. Suppressed while
	// composing for the same reason as the text notifications below.
	if ((bMoved || bSystemChanged) && ChangeNotifier.IsValid() && !Context->IsComposing())
	{
		ChangeNotifier->NotifyLayoutChanged(ITextInputMethodChangeNotifier::ELayoutChangeType::Changed);
	}
}

void FVaCuusImeHandler::NotifyTextInputClicked(uint64 SnapshotGeneration)
{
	check(IsInGameThread());

	// NOT gated on the platform system: the hint bookkeeping is what the reconcile below reads,
	// and keeping it correct on a platform with no IME is what makes the degraded path behave
	// identically apart from the hand-off nobody can make.
	if (View == nullptr)
	{
		return;
	}

	// D14a: activate NOW, on the click that will focus the field, not on the snapshot that
	// will confirm it. The hint outranks bTextInputFocused until a STRICTLY NEWER snapshot
	// arrives, so the frame-old "no field focused" that is still cached cannot cancel it.
	bActivationHintPending = true;
	ActivationHintSnapshotGeneration = SnapshotGeneration;

	// Forced: the FReply that grants Slate focus has not been processed yet, so bHostHasFocus
	// is still false on the very click this exists to serve. See ActivateContext.
	ActivateContext(/*bForce=*/true);
}

void FVaCuusImeHandler::OnSnapshotRefreshed(const FVaCuusInteractiveSnapshot& Snapshot)
{
	check(IsInGameThread());

	if (View == nullptr || !Context.IsValid())
	{
		// No context yet, which means no host has ever supplied a surface: a headless view, or
		// one whose Slate widget has not ticked. Nothing to shadow and nothing to notify.
		return;
	}

	// THE SHADOW FIRST, THE NOTIFICATIONS SECOND, and the order is load-bearing: TSF reacts to
	// a notification by immediately pulling GetTextLength/GetTextInRange/GetSelectionRange back
	// out of this same object, synchronously, before the notify call returns. Notifying against
	// the old shadow would hand it the state it already had.
	Context->SetShadowState(Snapshot.TextField);

	// The activation hint is consumed by the first snapshot that is newer than the click that
	// set it -- older ones simply cannot know about the field yet.
	if (bActivationHintPending && Snapshot.Generation > ActivationHintSnapshotGeneration)
	{
		bActivationHintPending = false;
	}

	const bool bWantActive = Snapshot.bTextInputFocused || bActivationHintPending;
	if (bWantActive)
	{
		ActivateContext(/*bForce=*/bActivationHintPending);
	}
	else
	{
		DeactivateContext();
	}

	if (!ChangeNotifier.IsValid())
	{
		return;
	}

	// A NEW FIELD IS A NEW LAYOUT, not a text edit: the indices restart, so reporting the
	// difference between two different fields' values as a change would make TSF splice one
	// into the other.
	if (Snapshot.TextField.Generation != LastNotifiedFieldGeneration)
	{
		LastNotifiedFieldGeneration = Snapshot.TextField.Generation;
		LastNotifiedValue = Snapshot.TextField.Value;
		LastNotifiedSelectionBegin = Snapshot.TextField.SelectionBegin;
		LastNotifiedSelectionEnd = Snapshot.TextField.SelectionEnd;

		if (!Context->IsComposing())
		{
			ChangeNotifier->NotifyLayoutChanged(ITextInputMethodChangeNotifier::ELayoutChangeType::Changed);
		}
		return;
	}

	// NEVER WHILE COMPOSING, and 5.8.1 spells out why (SlateEditableTextLayout.cpp:3265-3292):
	// TSF already owns the edits it is making, so reporting them back as external changes
	// double-counts indices and corrupts its position model. Note this suppresses the final
	// commit too -- deliberately, because EndComposition runs its transaction while the flag
	// is still conceptually set on the platform's side.
	if (Context->IsComposing())
	{
		LastNotifiedValue = Snapshot.TextField.Value;
		LastNotifiedSelectionBegin = Snapshot.TextField.SelectionBegin;
		LastNotifiedSelectionEnd = Snapshot.TextField.SelectionEnd;
		return;
	}

	int32 ChangeBegin = 0;
	int32 OldLength = 0;
	int32 NewLength = 0;
	if (VaCuusTextInput::ComputeMinimalChangedRange(
			LastNotifiedValue, Snapshot.TextField.Value, ChangeBegin, OldLength, NewLength))
	{
		ChangeNotifier->NotifyTextChanged(uint32(ChangeBegin), uint32(OldLength), uint32(NewLength));
		LastNotifiedValue = Snapshot.TextField.Value;
	}

	if (LastNotifiedSelectionBegin != Snapshot.TextField.SelectionBegin ||
		LastNotifiedSelectionEnd != Snapshot.TextField.SelectionEnd)
	{
		LastNotifiedSelectionBegin = Snapshot.TextField.SelectionBegin;
		LastNotifiedSelectionEnd = Snapshot.TextField.SelectionEnd;
		ChangeNotifier->NotifySelectionChanged();
	}
}

void FVaCuusImeHandler::EnsureContext()
{
	check(IsInGameThread());

	if (!Context.IsValid())
	{
		Context = MakeShared<FVaCuusTextInputMethodContext>(*this);
		Context->SetSurface(PreviousSurface);
	}

	// Registration is the ONLY part that needs the platform. Retried on every call rather than
	// only at creation, because the system can appear later: a widget built outside a window
	// (or before Slate finished coming up) has no system at first and gets one when it is
	// re-parented into a real one.
	if (TextInputMethodSystem != nullptr && !bRegistered)
	{
		ChangeNotifier = TextInputMethodSystem->RegisterContext(Context.ToSharedRef());
		bRegistered = true;

		if (ChangeNotifier.IsValid())
		{
			ChangeNotifier->NotifyLayoutChanged(ITextInputMethodChangeNotifier::ELayoutChangeType::Created);
		}

		UE_LOG(LogVaCuus, Verbose, TEXT("IME: registered a text input context with the platform"));
	}
}

void FVaCuusImeHandler::ActivateContext(bool bForce)
{
	check(IsInGameThread());

	EnsureContext();

	if (TextInputMethodSystem == nullptr)
	{
		// The degraded path (D16): the context exists and stays up to date, but there is nobody
		// to activate it with. Typing still works -- it goes through OnKeyChar.
		return;
	}

	// The host must hold Slate focus for the platform context to be meaningful: keys travel
	// the focus path, so activating while another widget owns focus would point the OS at a
	// field no keystroke can reach. bForce is the one exception, and it is the D14a click.
	if (!bHostHasFocus && !bForce)
	{
		return;
	}

	const TSharedRef<FVaCuusTextInputMethodContext> ContextRef = Context.ToSharedRef();
	if (!TextInputMethodSystem->IsActiveContext(ContextRef))
	{
		TextInputMethodSystem->ActivateContext(ContextRef);
		UE_LOG(LogVaCuus, Verbose, TEXT("IME: activated the text input context"));
	}
}

void FVaCuusImeHandler::DeactivateContext()
{
	check(IsInGameThread());

	if (TextInputMethodSystem == nullptr || !Context.IsValid())
	{
		return;
	}

	const TSharedRef<FVaCuusTextInputMethodContext> ContextRef = Context.ToSharedRef();
	if (!TextInputMethodSystem->IsActiveContext(ContextRef))
	{
		return;
	}

	// ABORT BEFORE DEACTIVATING, never after: an IME left mid-composition will call
	// EndComposition() on a context whose owner is going away (CEFImeHandler.cpp:92), and
	// CancelComposition is how the platform is told to stop rather than commit.
	if (ContextRef->IsComposing())
	{
		ContextRef->AbortComposition();
		if (ChangeNotifier.IsValid())
		{
			ChangeNotifier->CancelComposition();
		}
	}

	TextInputMethodSystem->DeactivateContext(ContextRef);
	UE_LOG(LogVaCuus, Verbose, TEXT("IME: deactivated the text input context"));
}

void FVaCuusImeHandler::Shutdown()
{
	check(IsInGameThread());

	if (View == nullptr && !Context.IsValid())
	{
		// Already retired; idempotent by contract (several teardown sites call this).
		return;
	}

	if (Context.IsValid())
	{
		DeactivateContext();

		if (TextInputMethodSystem != nullptr)
		{
			// UNREGISTER, not just deactivate: the system holds the context by TSharedRef, so
			// skipping this keeps our object -- and through it this handler's back-pointer --
			// alive inside the platform for the rest of the session.
			TextInputMethodSystem->UnregisterContext(Context.ToSharedRef());
		}

		// The KillContext pattern: null the back-pointer so any override the platform still
		// manages to call answers as an empty read-only field instead of reaching a dead owner.
		Context->KillContext();
		Context.Reset();
	}

	ChangeNotifier.Reset();
	TextInputMethodSystem = nullptr;
	bRegistered = false;
	bActivationHintPending = false;
	bHostHasFocus = false;
	View = nullptr;
}

bool FVaCuusImeHandler::IsContextActive() const
{
	return TextInputMethodSystem != nullptr && Context.IsValid() &&
		TextInputMethodSystem->IsActiveContext(Context.ToSharedRef());
}

ITextInputMethodContext* FVaCuusImeHandler::GetContextForTesting() const
{
	return Context.Get();
}
