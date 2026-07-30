// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "GenericPlatform/ICursor.h"
#include "Misc/EnumClassFlags.h"

namespace Rml
{
class Context;
}

/**
 * What one reported rect is, beyond "the UI wants pointer events here".
 *
 * A PARALLEL BYTE ARRAY, NOT A FIELD ON A STRUCT (controller decision D11). The
 * rects are scanned linearly on the game thread for every pointer event, and an
 * FIntRect is exactly 16 bytes -- four per cache line. Folding a uint8 into the
 * element would pad the struct to 20 (or 32 once anything else joins it) and make
 * the common query, "is this point covered at all", touch bytes it does not need.
 * Two arrays that grow together keep the hot scan dense and let a rarer question
 * ("...and is it focusable?") index the same position.
 */
enum class EVaCuusRectFlags : uint8
{
	None = 0,

	/**
	 * Set on EVERY reported rect -- it is the reason the rect exists at all (see the
	 * interactive predicate on FVaCuusInteractiveSnapshot). Kept explicit rather than
	 * implied so the flags byte is self-describing and never zero, which is a cheap
	 * invariant to assert on.
	 */
	Interactive = 1 << 0,

	/**
	 * A click here would move RmlUi's focus onto this element, by RmlUi's own rule:
	 * visible, computed `focus` != none on the element AND on every ancestor, and
	 * computed `tab-index: auto` (ElementDocument.cpp's CanFocusElement, 30-43).
	 *
	 * WHY IT IS PER RECT AND NOT PER VIEW: this is the fix for a real UX bug found in
	 * Task 6. bWantsKeyboardFocus below describes whether a focusable element ALREADY
	 * holds focus, which is a fact about the previous published frame -- so the click
	 * that first focuses a text field could not know it had, and typing needed a second
	 * click. Focusability is a property of the GEOMETRY, which is a frame old but
	 * correct, so asking "is the rect under this point focusable" answers on the FIRST
	 * click.
	 *
	 * FOR TASK 9 (IME), WHICH NEEDS BOTH HALVES AND ONLY ONE OF THEM IS HERE. This flag
	 * answers "would a click at this point focus something that takes text", which is
	 * what `OnMouseButtonDown` needs in order to activate the platform IME context on the
	 * SAME click that focuses the field -- add a `TextInput` bit alongside this one (six
	 * are free) rather than a parallel array. But activation also has to be undone, and
	 * "is a text control focused RIGHT NOW" is a view-level fact about UI-thread state,
	 * not about geometry: it belongs next to bWantsKeyboardFocus, whose staleness it
	 * shares. Deriving either from the other is the mistake D11 was fixing.
	 */
	Focusable = 1 << 1,

	/**
	 * A click here would put the CARET in a text field: the element is `<textarea>`, or
	 * `<input>` whose `type` is text or password (controller decision D14a).
	 *
	 * THE SET IS RmlUi'S OWN, not a guess. ElementFormControlInput dispatches on the
	 * `type` attribute and instances InputTypeText for exactly "text" and "password" --
	 * "radio", "checkbox", "range", "submit" and "button" get their own non-text types,
	 * and an unrecognised or missing type falls back to "text"
	 * (ElementFormControlInput.cpp:95-118). InputTypeText is the only one that owns a
	 * WidgetTextInput, i.e. the only one that has a caret, a selection, and a
	 * Rml::TextInputContext for an IME to talk to.
	 *
	 * WHY IT IS A SEPARATE BIT FROM Focusable. Every text field is focusable, but the
	 * converse is loudly false: a button, a checkbox, a `tab-index: auto` div are all
	 * Focusable and none of them takes text. Activating the platform IME context on a
	 * click over any of those would tell the OS to open a candidate window over a
	 * checkbox.
	 *
	 * WHY IT IS NEEDED AT ALL, given bTextInputFocused below exists: the same D11
	 * asymmetry as Focusable. This is GEOMETRY (one frame old but already correct before
	 * anyone clicks), so `OnMouseButtonDown` can activate the IME context on the click
	 * that focuses the field; bTextInputFocused is UI-THREAD STATE, and therefore behind
	 * the click that changed it by one frame. Waiting for it loses the first composition.
	 */
	TextInput = 1 << 2
};

ENUM_CLASS_FLAGS(EVaCuusRectFlags)

/**
 * THE IME SHADOW STATE: everything `ITextInputMethodContext`'s pull-style virtuals have
 * to answer, as a plain value type the game thread already owns (controller decision
 * D15).
 *
 * WHY IT MUST BE A SHADOW AND NOT A QUERY. All 14 of ITextInputMethodContext's methods
 * are SYNCHRONOUS PULLS made from the platform message pump on the GAME thread
 * (ITextInputMethodSystem.h:14-139) -- the OS asks "how long is your text", "what is
 * selected", "where is the caret" and expects an answer before it returns. The answers
 * live in RmlUi, on the VaCuus UI thread, which no game-thread call may touch (spec §4).
 * Blocking on the UI thread to answer would hand the OS's input pump a stall; so the UI
 * thread publishes this instead, once per frame, inside the snapshot, and the context
 * answers from it. Only MUTATIONS travel the other way, as queued input events stamped
 * with Generation. Epic's own out-of-process precedent does the same thing for the same
 * reason (FCEFTextInputMethodContext keeps its own CompositionSequence rather than
 * querying the renderer, CEFTextInputMethodContext.cpp:89-139).
 *
 * INDEX SPACE: **UTF-16 units into Value**, i.e. plain FString indices -- because that is
 * what the engine's side of the boundary means by "code point index" (Slate answers
 * GetTextLength with FString::Len(), SlateEditableTextLayout.cpp), and what TSF's ACP
 * offsets are on Windows. RmlUi's public offsets are UTF-8 CHARACTER offsets, a third
 * space; the conversion happens exactly twice, both in VaCuusTextInput.cpp -- on publish
 * (RmlUi character offset -> UTF-16 index) and on enqueue (UTF-16 index -> RmlUi
 * character offset). Both are pure functions of Value, so both sides agree by
 * construction.
 *
 * ORDERING DISCIPLINE, and it is short because the triple buffer does the hard part:
 *
 *  1. The UI thread fills this whole struct and only THEN swaps the buffer
 *     (FVaCuusViewStatus::PublishSnapshot), so the game thread can never observe a
 *     half-filled state and no per-field release ordering is needed. Every field below is
 *     therefore consistent with every other field IN THE SAME publish -- that is the one
 *     guarantee the IME context relies on (a selection range that does not fit the value
 *     would make TSF compute a negative length).
 *  2. It is filled AFTER `Context::Update()` and after the caret latch is sampled, in
 *     that order: Update() is what leaves the element boxes clean, and ActivateKeyboard
 *     can fire from inside it.
 *  3. Generation is an IDENTITY token, not a freshness token (the snapshot's own
 *     Generation is freshness). It moves ONLY when the platform's active field changes --
 *     activate, deactivate, destroy -- so a queued mutation whose stamp no longer matches
 *     is a mutation for a field that is gone, and is dropped rather than half-applied.
 *     This is load-bearing: WidgetTextInput::SetSelectionRange EARLY-RETURNS when the
 *     element is not focused (WidgetTextInput.cpp:330-331), so without the stamp a stale
 *     selection would be silently swallowed while the SetText next to it still landed.
 *  4. Every field is a snapshot of the PREVIOUS UI frame. The IME therefore reasons about
 *     text that is up to one frame old; the mutation path is what keeps that safe, since
 *     a mutation the UI thread rejects never changes anything.
 */
struct FVaCuusTextFieldState
{
	/**
	 * Identity of the platform's currently active text field; 0 means "none".
	 *
	 * PROCESS-WIDE, not per view, and that is RmlUi's constraint rather than a choice:
	 * `Rml::TextInputHandler`'s three callbacks carry only a `TextInputContext*` and no
	 * context or element (TextInputHandler.h:24-32), so "which field is the IME talking
	 * to" is a single global fact. It matches what the platform means anyway -- TSF has
	 * one active context at a time, and so does Slate's own IME.
	 */
	uint64 Generation = 0;

	/** The focused control's whole value, UTF-16. The index space for everything below. */
	FString Value;

	/** Selection as half-open UTF-16 indices into Value; equal means a bare caret. */
	int32 SelectionBegin = 0;
	int32 SelectionEnd = 0;

	/**
	 * The composing span RmlUi was last asked to underline, as half-open UTF-16 indices.
	 * [0, 0) means "not composing" -- RmlUi's own "clear it" value
	 * (WidgetTextInput::SetCompositionRange(0, 0)).
	 *
	 * PUBLISHED FOR OBSERVABILITY, NOT AS THE SOURCE OF TRUTH: the authoritative
	 * composition range is the game-thread context's own member, because TSF drives it and
	 * `IsComposing()` must answer without consulting anything. This is what the UI thread
	 * actually applied, which is the only way a test can tell the two apart.
	 */
	int32 CompositionBegin = 0;
	int32 CompositionEnd = 0;

	/**
	 * Caret position and line height in VIEW PIXELS, straight from
	 * `SystemInterface::ActivateKeyboard(caret_position, line_height)`.
	 *
	 * THE ONLY PER-CARET SIGNAL RmlUi GIVES AN EMBEDDER. `Rml::TextInputContext` has no
	 * caret API at all -- its `GetBoundingBox` is the element's whole BORDER box
	 * (WidgetTextInput.cpp:85-88) -- so without this the OS candidate window would be
	 * pinned to the field's top-left instead of following the caret. RmlUi emits it from
	 * WidgetTextInput::SetKeyboardActive, i.e. on every ShowCursor(true): focus, every
	 * arrow key, every click and every edit (WidgetTextInput.cpp:1159-1175).
	 */
	FVector2f CaretPosition = FVector2f::ZeroVector;
	float CaretLineHeight = 0.0f;

	/** False until ActivateKeyboard has been seen for this field; GetTextBounds falls back. */
	bool bCaretValid = false;

	/** The focused control's border box in view pixels; the GetTextBounds fallback. */
	FIntRect BoundingBox = FIntRect(0, 0, 0, 0);

	/** RmlUi: the element carries `readonly`, or is disabled. */
	bool bReadOnly = false;

	void Reset()
	{
		Generation = 0;
		Value.Reset();
		SelectionBegin = 0;
		SelectionEnd = 0;
		CompositionBegin = 0;
		CompositionEnd = 0;
		CaretPosition = FVector2f::ZeroVector;
		CaretLineHeight = 0.0f;
		bCaretValid = false;
		BoundingBox = FIntRect(0, 0, 0, 0);
		bReadOnly = false;
	}
};

/**
 * Where a view is interactive, as a fully detached value type.
 *
 * WHY IT EXISTS: Slate demands a synchronous verdict. `OnMouseButtonDown` has to
 * return Handled or Unhandled *now*, and "Unhandled" is what lets a click fall
 * through to the game. The authoritative hit test
 * (`Rml::Context::GetElementAtPoint`) lives on the UI thread and may not be called
 * from the game thread at all -- it rebuilds RmlUi's private stacking-context
 * cache despite being declared const (Context.cpp:1407). So the UI thread
 * publishes this instead: the rectangles it *would* have hit, as plain integers,
 * once per frame, and the game thread answers from them.
 *
 * COVERAGE, NOT IDENTITY: the rect list is a set-union. It says "a click here
 * belongs to the UI"; it does not say which element, and painter order is
 * irrelevant. Everything the game thread needs from it is Contains().
 *
 * PASS-THROUGH IS THE ABSENCE OF COVERAGE, never an occluder. A region opts out
 * (`vacuus-passthrough`) by not being reported, so a pass-through element floating
 * *over* a button does NOT hide the button -- the button still reports its rect and
 * the click is still taken by the UI. RmlUi's public API cannot express the
 * occluder version: `Element::stacking_context` and `BuildLocalStackingContext`
 * are private (Element.h:671-674), so a painter-ordered rect list is not buildable
 * from outside the library.
 *
 * STALE BY ONE UI FRAME, on purpose. The game thread never waits for the UI
 * thread, so the geometry it tests against is up to one UI frame old. A button
 * that moved this frame can eat one click meant for the game (or miss one) --
 * accepted in the spec, and the reason Generation exists: the consumer keeps its
 * last snapshot when nothing new arrived (that is TTripleBuffer's "never block"
 * behaviour), and Generation is the only way to tell a fresh publish from a
 * repeat.
 *
 * PIXELS AND EDGES: view-space pixels, already clipped, and HALF-OPEN --
 * FIntRect::Contains() includes the min edge and excludes the max one, so a
 * 100x40 rect at (20,20) covers x in [20,120). That differs from RmlUi's own
 * inclusive-on-both-edges Rectangle::Contains(); the half-open convention is the
 * right one for pixel coverage (adjacent rects tile without overlapping) and the
 * one-pixel disagreement on the far edge is below what any input event can
 * resolve.
 *
 * THE INTERACTIVE PREDICATE IS A HEURISTIC -- and it is the M2 contract, so it is
 * written down here rather than left in the .cpp. An element is reported when:
 *
 *     computed `pointer-events` != none
 *     AND ( computed `tab-index` == auto
 *        OR its tag is one of button|input|select|textarea|a
 *        OR it carries the plain attribute `vacuus-interactive` )
 *
 * WHY A HEURISTIC AT ALL: what actually makes an element interactive in RmlUi is
 * an attached event listener, and RmlUi exposes no way to ask ("does this element
 * have a click listener?" has no public API at 0ae381e). tab-index/tag/attribute
 * are the observable proxies. `vacuus-interactive` is the escape hatch for
 * anything the proxies miss -- a plain <div> wired up from script, most obviously.
 *
 * The known-interactive tag list includes RmlUi's three non-DOM scrollbar tags
 * (`scrollbarvertical`, `scrollbarhorizontal`, `scrollbarcorner` -- the names
 * ElementScroll instances them under, ElementScroll.cpp:193,213). Controller
 * decision D8: the DFS already walks them, because it passes
 * include_non_dom_elements, but they carry no tab-index and no attribute, so
 * without this a drag on a scrollbar read as pass-through and scrolled the game
 * instead of the list.
 *
 * FOCUSABILITY, BY CONTRAST, IS EXACT. Each reported rect also carries whether a
 * click on it would take RmlUi focus (EVaCuusRectFlags::Focusable), computed with
 * RmlUi's own rule rather than a proxy -- because unlike "does this have a click
 * listener", focusability IS queryable: visible, computed `focus` != none on the
 * element and every ancestor, and computed `tab-index: auto`.
 *
 * KNOWN GAP (not silent): only an element's MAIN box is measured, so fragmented
 * inline content under-reports. RmlUi's own hit test unions all of an element's
 * boxes (Element.cpp:546-565); a HUD's interactive elements are block-level, so
 * this is deferred rather than solved.
 */
struct FVaCuusInteractiveSnapshot
{
	/**
	 * Strictly increasing per view, stamped by the UI thread. The game thread's
	 * read is a "keep the last one" swap that silently returns the previous buffer
	 * when no new frame arrived, so this is how a consumer tells the difference.
	 */
	uint64 Generation = 0;

	/** The view size the rects below were computed against; diagnostics and sanity checks. */
	FIntPoint ViewSize = FIntPoint::ZeroValue;

	/** Clipped, view-space pixels, half-open. Union semantics; order is meaningless. */
	TArray<FIntRect> InteractiveRects;

	/**
	 * One byte per entry in InteractiveRects, same index (controller decision D11).
	 *
	 * INVARIANT: RectFlags.Num() == InteractiveRects.Num(), and no entry is
	 * EVaCuusRectFlags::None. Both arrays are only ever appended to together, by the
	 * one DFS in BuildVaCuusInteractiveSnapshot.
	 */
	TArray<EVaCuusRectFlags> RectFlags;

	/**
	 * Cursor shape the UI wants, for SVaCuusWidget::OnCursorQuery. RmlUi pushes
	 * cursor changes through SystemInterface::SetMouseCursor and only on change, so
	 * the source is a latch (GetVaCuusLatchedMouseCursor) that the host samples right
	 * after its own Context::Update(); the value is carried here rather than queried,
	 * because OnCursorQuery is a const game-thread call that cannot ask the UI thread
	 * anything.
	 */
	EMouseCursor::Type Cursor = EMouseCursor::Default;

	/**
	 * True when a REAL focusable element inside a document HOLDS RmlUi focus right now
	 * -- neither the context root nor a document element itself (controller decision
	 * D9). This is what the KEY handlers answer Slate from: keys are consumed only
	 * while the UI actually owns the keyboard.
	 *
	 * WHY EXCLUDE THE DOCUMENT ELEMENT: ElementDocument::Show() focuses the document
	 * itself (that is what makes Tab/arrow/Enter run at all, since
	 * ProcessDefaultAction lives on the document), so "a document is up" would
	 * otherwise read as "the UI wants the keyboard" and every loaded HUD would take the
	 * keyboard away from the game.
	 *
	 * NOT DERIVABLE FROM RectFlags, and the distinction is the whole of D11: a
	 * Focusable rect says "a click HERE WOULD take focus" (geometry, one frame old but
	 * correct), while this says "something focusable HAS focus" (UI-thread state, one
	 * frame old and therefore behind the click that changed it). Different questions
	 * with different staleness -- which is why the click path uses IsFocusableAt() and
	 * the key path uses this.
	 */
	bool bWantsKeyboardFocus = false;

	/**
	 * Would a TAB press move RmlUi's focus INTO this view's document from where it is
	 * now? (Task 14 acceptance decision A1.)
	 *
	 * WHAT PROBLEM THIS SOLVES. bWantsKeyboardFocus above is "something focusable HAS
	 * focus", and the key handlers used it alone -- so the press that ENTERS the UI was
	 * answered Unhandled and reached the game as well as the UI. A player who opens a
	 * pad-driven menu and presses a direction got both: the menu highlight moved AND the
	 * character stepped. This is the fact that closes that gap, and it is published rather
	 * than guessed because the game thread cannot ask RmlUi anything.
	 *
	 * EXACTLY RmlUi's OWN PRECONDITIONS, both of them:
	 *
	 *  1. A DOCUMENT ELEMENT holds focus. Context::ProcessKeyDown dispatches to `focus`,
	 *     or to the context ROOT when there is none (Context.cpp:534-537); the root is not
	 *     an ElementDocument, so ElementDocument::ProcessDefaultAction -- where every Tab,
	 *     arrow and Return handler lives -- never runs and no key can move focus at all.
	 *  2. At least one element in the context passes RmlUi's focusability test, because
	 *     FindNextTabElement has to have somewhere to land (ElementDocument.cpp:581-591
	 *     calls it; SearchFocusSubtree gates every candidate on CanFocusElement,
	 *     :728-733, and CanFocusElement is :30-43).
	 *
	 * NOT DERIVABLE FROM RectFlags: a focusable element with a zero-area or fully clipped
	 * box is tabbable but is not REPORTED (the rect list gates on Area() > 0), so counting
	 * Focusable rects would answer "no" for a document Tab can enter perfectly well.
	 *
	 * MEANINGLESS WHILE bWantsKeyboardFocus IS TRUE, and it is false then by construction:
	 * if something focusable holds focus, the document does not, so (1) fails. The two are
	 * therefore never both true, and the key path can simply OR them.
	 */
	bool bTabEntersFocus = false;

	/**
	 * The same question for a DIRECTION key (arrows and the DPad), which needs one thing
	 * Tab does not.
	 *
	 * WHY IT IS A SEPARATE BOOL. Tab is handled unconditionally
	 * (ElementDocument.cpp:581-591 goes straight to FindNextTabElement), but the arrow
	 * branch first reads a `nav-*` property off the focus node with GetLocalProperty and
	 * does NOTHING AT ALL when there is none -- every line that could move focus sits
	 * inside `if (const Property* nav_property = ...)` (ElementDocument.cpp:628-638). With
	 * nothing focusable focused the focus node resolves to the document itself (:625), so
	 * the property has to be on the DOCUMENT element -- which is what `body { nav: auto; }`
	 * is for, and why every VaCuus document carries it.
	 *
	 * So a document with focusables but no `nav` on its root can be entered by Tab and NOT
	 * by an arrow. Collapsing the two into one bool would make the widget consume a
	 * direction key that provably moves nothing -- swallowing the player's input to
	 * achieve exactly nothing, which is the worse of the two errors this decision is
	 * choosing between.
	 *
	 * Implies bTabEntersFocus (same two preconditions plus the property).
	 */
	bool bDirectionEntersFocus = false;

	/**
	 * True when a TEXT control -- one with a caret, a selection and a
	 * Rml::TextInputContext -- holds RmlUi focus right now (controller decision D14b).
	 *
	 * THE SECOND HALF OF D14, AND NOT DERIVABLE FROM THE FIRST. EVaCuusRectFlags::TextInput
	 * says "a click HERE WOULD put the caret in a field" (geometry, already correct before
	 * the click); this says "a field HAS the caret" (UI-thread state, one frame behind the
	 * click that changed it). The IME needs both and for opposite reasons: the flag decides
	 * when to ACTIVATE the platform context (on the first click, or the first composition is
	 * lost), this decides when to DEACTIVATE it (once nothing takes text any more, or the OS
	 * keeps a candidate window open over a game that has moved on).
	 *
	 * STRICTLY NARROWER THAN bWantsKeyboardFocus: a focused button makes the view want the
	 * keyboard but takes no text.
	 *
	 * COMPUTED FROM THIS VIEW'S OWN CONTEXT, so unlike FVaCuusTextFieldState::Generation it
	 * is exact per view even with several views up.
	 */
	bool bTextInputFocused = false;

	/**
	 * What the platform IME needs to know about the focused text field (D15). Meaningful
	 * only while bTextInputFocused is true; reset (Generation 0) otherwise, so a view with
	 * no field never publishes another view's text.
	 */
	FVaCuusTextFieldState TextField;

	/**
	 * Is this point covered by any interactive region? Linear scan -- tens of rects
	 * in practice, and the cost is a few nanoseconds per rect with no branching
	 * worth optimising. If a document ever produces hundreds, the cheap fix is a
	 * cached union-bounds early-out (see rmlui-input.md's pattern), not a tree.
	 */
	bool Contains(FIntPoint Point) const
	{
		for (const FIntRect& Rect : InteractiveRects)
		{
			if (Rect.Contains(Point))
			{
				return true;
			}
		}

		return false;
	}

	/**
	 * Would a click at this point take keyboard focus? One pass, same scan as
	 * Contains().
	 *
	 * PERMISSIVE ON OVERLAP, on purpose: rects are a union with no painter order (see
	 * the class comment), so when a focusable button's rect and its non-focusable
	 * parent panel's rect both cover the point, the answer is yes. That matches what
	 * RmlUi will actually do -- its hit test walks front to back and lands on the
	 * button -- and it errs the same way the rest of this type errs, towards "the UI
	 * takes it".
	 */
	bool IsFocusableAt(FIntPoint Point) const
	{
		return HasFlagAt(Point, EVaCuusRectFlags::Focusable);
	}

	/**
	 * Would a click at this point put the caret in a text field (controller decision
	 * D14a)? Same scan, same permissive-on-overlap rule as IsFocusableAt().
	 *
	 * This is what activates the platform IME context on the FIRST click. Erring towards
	 * "yes" costs an IME context activated over a field the click missed -- which the next
	 * published snapshot corrects, because bTextInputFocused will say no; erring towards
	 * "no" costs the player's first composition, silently.
	 */
	bool IsTextInputAt(FIntPoint Point) const
	{
		return HasFlagAt(Point, EVaCuusRectFlags::TextInput);
	}

	/**
	 * Back to "nothing is interactive", keeping both arrays' allocations. The
	 * publisher rotates through three of these forever, so reusing the arrays is
	 * what makes a steady-state UI frame allocation-free.
	 */
	void Reset()
	{
		Generation = 0;
		ViewSize = FIntPoint::ZeroValue;
		InteractiveRects.Reset();
		RectFlags.Reset();
		Cursor = EMouseCursor::Default;
		bWantsKeyboardFocus = false;
		bTabEntersFocus = false;
		bDirectionEntersFocus = false;
		bTextInputFocused = false;

		// Keeps Value's allocation, like the rect arrays above: the publisher rotates
		// through three of these forever and a focused field is re-published every frame.
		TextField.Reset();
	}

private:
	/** Shared body of IsFocusableAt/IsTextInputAt; one pass over the parallel arrays. */
	bool HasFlagAt(FIntPoint Point, EVaCuusRectFlags Flag) const
	{
		// Index-based rather than ranged: the flags live in a parallel array, and the
		// index is the only thing that ties the two together.
		const int32 NumRects = FMath::Min(InteractiveRects.Num(), RectFlags.Num());
		for (int32 Index = 0; Index < NumRects; ++Index)
		{
			if (InteractiveRects[Index].Contains(Point) && EnumHasAnyFlags(RectFlags[Index], Flag))
			{
				return true;
			}
		}

		return false;
	}
};

/** What one snapshot build touched. Diagnostics only: the DFS cost is linear in NumElementsVisited. */
struct FVaCuusSnapshotBuildStats
{
	int32 NumDocuments = 0;
	int32 NumElementsVisited = 0;
};

/**
 * Builds the snapshot for one context by walking its documents once.
 *
 * MUST run on the VaCuus UI thread, after `Context::Update()` and before
 * `Context::Render()`: Update() is what leaves every element's absolute offset and
 * box clean, which is the whole reason the walk is O(1) per element instead of a
 * layout pass. Asserted.
 *
 * OutSnapshot is written in place and its array allocation is reused -- pass the
 * publisher's write buffer straight in (FVaCuusViewStatus::GetSnapshotWriteBuffer)
 * rather than building a local and copying.
 *
 * Takes Rml::Context by pointer-compatible reference only; nothing RmlUi-shaped
 * ever reaches the returned snapshot.
 */
VACUUS_API FVaCuusSnapshotBuildStats BuildVaCuusInteractiveSnapshot(
	Rml::Context& Context, FIntPoint ViewSize, uint64 Generation, FVaCuusInteractiveSnapshot& OutSnapshot);

/**
 * The cursor RmlUi last asked for, and a serial that only moves when it did.
 *
 * The source of FVaCuusInteractiveSnapshot::Cursor, and it has to be a latch:
 * RmlUi pushes the cursor name through SystemInterface::SetMouseCursor from inside
 * Context::Update, only when the name changed (Context.cpp:1315-1327), and offers
 * nothing to query. There is one system interface for the whole process but a
 * cursor per context, so a host must sample this IMMEDIATELY after its own
 * Update() and adopt the value only when the serial moved -- that is what
 * attributes a change to the view that caused it. Sample it later, or without the
 * serial test, and N views inherit each other's cursor.
 *
 * UI thread only (asserted): it is written from inside RmlUi.
 */
VACUUS_API EMouseCursor::Type GetVaCuusLatchedMouseCursor(uint64& OutSerial);

/** Where RmlUi last said the text caret is, in view pixels, plus its line height. */
struct FVaCuusCaretLatch
{
	FVector2f Position = FVector2f::ZeroVector;
	float LineHeight = 0.0f;

	/** False after DeactivateKeyboard: no field is taking keys, so there is no caret. */
	bool bActive = false;
};

/**
 * The caret RmlUi last asked the platform keyboard to follow, and a serial that only
 * moves when it did.
 *
 * A LATCH FOR EXACTLY THE SAME REASON AS THE CURSOR ONE ABOVE, and with the same
 * process-wide/per-context mismatch: `SystemInterface::ActivateKeyboard(caret_position,
 * line_height)` is the ONLY per-caret spatial signal RmlUi offers an embedder
 * (SystemInterface.h:59), it is PUSH-only, and the system interface is one object for the
 * whole library while a caret belongs to one context. RmlUi calls it from
 * WidgetTextInput::SetKeyboardActive on every ShowCursor(true) -- focus, arrow keys,
 * clicks, edits (WidgetTextInput.cpp:1159-1175) -- which happens both inside
 * `Context::Update()` and inside the `Context::Process*` calls the input drain makes.
 *
 * So a host adopts the value only when the serial moved AND its own context has a text
 * control focused; a view with no focused field publishes no caret. Position is in
 * ABSOLUTE RMLUI CONTEXT PIXELS (element absolute offset - scroll + cursor position,
 * WidgetTextInput.cpp:1613-1617), which is the same space as the view's own pixels and as
 * the interactive rects.
 *
 * UI thread only (asserted): it is written from inside RmlUi.
 */
VACUUS_API FVaCuusCaretLatch GetVaCuusLatchedCaret(uint64& OutSerial);
