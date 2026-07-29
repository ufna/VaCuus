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
 * KNOWN GAPS (Task 6 territory, not silent): RmlUi's non-DOM scrollbar children
 * (`scrollbarvertical`/`scrollbarhorizontal`/`scrollbarcorner`) are traversed but
 * match none of the three clauses, so a drag on a scrollbar is not reported as
 * interactive; and only an element's MAIN box is measured, so fragmented inline
 * content under-reports.
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
	 * cursor changes through SystemInterface::SetMouseCursor (and only on change),
	 * so the source has to be latched there -- that is Task 6; until then this
	 * stays Default.
	 */
	EMouseCursor::Type Cursor = EMouseCursor::Default;

	/**
	 * True when something inside a document holds RmlUi focus, i.e. keys sent to
	 * this view will reach an element. Task 6 uses it to decide whether a click on
	 * an interactive rect should also take Slate user focus.
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
