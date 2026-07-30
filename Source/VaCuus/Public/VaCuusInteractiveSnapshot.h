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
	Focusable = 1 << 1
};

ENUM_CLASS_FLAGS(EVaCuusRectFlags)

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
		// Index-based rather than ranged: the flags live in a parallel array, and the
		// index is the only thing that ties the two together.
		const int32 NumRects = FMath::Min(InteractiveRects.Num(), RectFlags.Num());
		for (int32 Index = 0; Index < NumRects; ++Index)
		{
			if (InteractiveRects[Index].Contains(Point) &&
				EnumHasAnyFlags(RectFlags[Index], EVaCuusRectFlags::Focusable))
			{
				return true;
			}
		}

		return false;
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
