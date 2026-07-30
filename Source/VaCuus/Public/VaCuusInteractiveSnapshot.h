// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "GenericPlatform/ICursor.h"

namespace Rml
{
class Context;
}

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
	 * Cursor shape the UI wants, for SVaCuusWidget::OnCursorQuery. RmlUi pushes
	 * cursor changes through SystemInterface::SetMouseCursor and only on change, so
	 * the source is a latch (GetVaCuusLatchedMouseCursor) that the host samples right
	 * after its own Context::Update(); the value is carried here rather than queried,
	 * because OnCursorQuery is a const game-thread call that cannot ask the UI thread
	 * anything.
	 */
	EMouseCursor::Type Cursor = EMouseCursor::Default;

	/**
	 * True when a REAL focusable element inside a document holds RmlUi focus --
	 * neither the context root nor a document element itself (controller decision
	 * D9). SVaCuusWidget takes Slate user focus on a click only when this is set.
	 *
	 * WHY EXCLUDE THE DOCUMENT ELEMENT: ElementDocument::Show() focuses the document
	 * itself when nothing inside it carries `autofocus` (FocusFlag::Auto), so "a
	 * document is up" would otherwise read as "the UI wants the keyboard" and every
	 * click anywhere on an interactive rect would steal focus from the game.
	 *
	 * CONSEQUENCE, and it is real: this describes the focus state of the PREVIOUS
	 * published frame, so the click that first focuses a text field cannot know it did
	 * -- the widget only takes Slate focus on the click AFTER that. Fine for buttons
	 * (RmlUi handles those entirely UI-side), a wart for typing, and the reason Task 9
	 * will want per-rect focusability in the snapshot rather than one view-wide bool.
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
	 * Back to "nothing is interactive", keeping InteractiveRects' allocation. The
	 * publisher rotates through three of these forever, so reusing the arrays is
	 * what makes a steady-state UI frame allocation-free.
	 */
	void Reset()
	{
		Generation = 0;
		ViewSize = FIntPoint::ZeroValue;
		InteractiveRects.Reset();
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
