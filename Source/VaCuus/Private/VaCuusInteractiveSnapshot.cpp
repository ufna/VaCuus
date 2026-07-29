// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusInteractiveSnapshot.h"

#include "VaCuusUIThread.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/ElementUtilities.h>

namespace VaCuusInteractiveSnapshot
{
/**
 * Marker attributes. LOWERCASE IS LOAD-BEARING: RmlUi's XML parser lowercases
 * every attribute name as it reads it (XMLParser.cpp:136,167), so a
 * `vacuus-PassThrough` in a document arrives here as `vacuus-passthrough` and a
 * mixed-case lookup would never match.
 *
 * Plain attributes rather than `data-*` names on purpose:
 * ElementUtilities::ApplyDataViewsControllers parses any `data-<type>-<modifier>`
 * attribute (ElementUtilities.cpp:407-424) and would quietly hand
 * type_name="vacuus" to Factory::InstanceDataView. Harmless, but it puts our
 * markers inside someone else's grammar.
 */
static const char* const GPassthroughAttribute = "vacuus-passthrough";
static const char* const GInteractiveAttribute = "vacuus-interactive";

/**
 * The tag half of the interactive predicate (see the header for the whole rule).
 * These are exactly the tags RmlUi ships interactive behaviour for, plus `a`,
 * which is not a built-in element but is the universal "clickable" convention in
 * RML documents. Tags are compared lowercase because the XML parser lowercases
 * them before instancing (XMLParser.cpp:61,74).
 */
static bool IsKnownInteractiveTag(const Rml::String& Tag)
{
	return Tag == "button" || Tag == "input" || Tag == "select" || Tag == "textarea" || Tag == "a";
}

/** Window-space AABB of an element's box area, snapped outwards to whole pixels. */
static FIntRect ToPixelRect(Rml::Vector2f Position, Rml::Vector2f Size)
{
	return FIntRect(
		FMath::FloorToInt(Position.x), FMath::FloorToInt(Position.y),
		FMath::CeilToInt(Position.x + Size.x), FMath::CeilToInt(Position.y + Size.y));
}

/**
 * One depth-first walk over a context's element tree.
 *
 * The clip rectangle is carried DOWN as a parameter rather than looked up per
 * element: ElementUtilities::GetClippingRegion() walks every ancestor on each call
 * (ElementUtilities.cpp:128-150, plus GetClientWidth/GetScrollWidth per level),
 * which would turn this O(N) sweep into O(N*depth) for an identical answer.
 */
struct FSnapshotWalk
{
	explicit FSnapshotWalk(FVaCuusInteractiveSnapshot& InOut)
		: Out(InOut)
	{
	}

	void Visit(Rml::Element* Element, const FIntRect& Clip);

	FVaCuusInteractiveSnapshot& Out;
	int32 NumElementsVisited = 0;
};

void FSnapshotWalk::Visit(Rml::Element* Element, const FIntRect& Clip)
{
	++NumElementsVisited;

	// Prunes the whole subtree, which is exactly what RmlUi does: both
	// Element::AddToStackingContext (Element.cpp:2390) and the hit test bail on an
	// invisible element, and `visible` is a cached flag == (display != None &&
	// visibility == Visible) recomputed on property change -- so this is a field
	// read, not a style query. Document->Hide() lands here too.
	if (!Element->IsVisible())
	{
		return;
	}

	// The opt-out: `vacuus-passthrough` removes this element AND its subtree from
	// the snapshot, so the region reads as "not covered" and clicks reach the game.
	// A subtree prune (not a per-element skip) because that is what an author means
	// by marking a region pass-through; the inverse opt-back-in (`vacuus-capture`
	// on a descendant) is deliberately not implemented in M2 -- no use for it yet,
	// and it would make the rule two-sided for no gain.
	if (Element->HasAttribute(GPassthroughAttribute))
	{
		return;
	}

	// GetAbsoluteOffset() is UNTRANSFORMED (Element.cpp:359-362): under a `transform`
	// it reports where the element would have been, and RmlUi's own hit test
	// compensates by inverse-projecting the query point instead. So a transformed
	// element has to go the expensive way -- GetBoundingBox projects the four corners
	// (ElementUtilities.cpp:235-275) -- and the transform state is the gate, because
	// the cheap path is a cached field read and this is not.
	FIntRect Rect;
	if (Element->GetTransformState() != nullptr)
	{
		Rml::Rectanglef Bounds;
		if (!Rml::ElementUtilities::GetBoundingBox(Bounds, Element, Rml::BoxArea::Border))
		{
			return;
		}

		Rect = ToPixelRect(Bounds.Position(), Bounds.Size());
	}
	else
	{
		// Both reads are O(1) after Context::Update(): absolute_offset is cached and
		// clean (Element.cpp:365-389) and GetBox() is a plain `return main_box;`
		// (Element.cpp:464). That is what makes a per-frame full-tree walk affordable.
		Rect = ToPixelRect(
			Element->GetAbsoluteOffset(Rml::BoxArea::Border), Element->GetBox().GetSize(Rml::BoxArea::Border));
	}

	Rect.Clip(Clip);

	const Rml::ComputedValues& Computed = Element->GetComputedValues();

	// pointer-events is checked PER ELEMENT and never used to prune the subtree.
	// It is an inherited property (StyleSheetSpecification.cpp:386) but RmlUi
	// evaluates it only after descending into children (Context.cpp:1442, after the
	// stacking-context loop), so a `pointer-events: auto` descendant of a `none`
	// parent IS hit -- and must therefore still be reported here.
	const bool bInteractive = Computed.pointer_events() != Rml::Style::PointerEvents::None &&
		(Computed.tab_index() == Rml::Style::TabIndex::Auto || IsKnownInteractiveTag(Element->GetTagName()) ||
			Element->HasAttribute(GInteractiveAttribute));

	if (bInteractive && Rect.Area() > 0)
	{
		Out.InteractiveRects.Add(Rect);
	}

	// Does this element clip its descendants? Same test RmlUi uses to build a
	// scissor region (ElementUtilities.cpp:141).
	const bool bClipsChildren = Computed.overflow_x() != Rml::Style::Overflow::Visible ||
		Computed.overflow_y() != Rml::Style::Overflow::Visible;

	FIntRect ChildClip = Clip;
	if (bClipsChildren)
	{
		// RmlUi clips to the element's CLIP AREA, which defaults to the padding box
		// (Element.cpp:83) and is what GetClippingRegion feeds the scissor
		// (ElementUtilities.cpp:180-183) -- not the border box we reported above. Asking
		// the element for its clip area rather than assuming Padding also honours the
		// custom elements that call SetClipArea().
		const Rml::BoxArea ClipArea = Element->GetClipArea();
		ChildClip = ToPixelRect(Element->GetAbsoluteOffset(ClipArea), Element->GetBox().GetSize(ClipArea));
		ChildClip.Clip(Clip);

		// Every descendant rect is an intersection with this, so an empty clip means
		// the whole subtree is invisible -- the one place a rect test may prune.
		// NOTE what is deliberately NOT done here: rmlui-input.md's pattern also
		// prunes when the clipping element is ITSELF interactive, on the grounds that
		// its descendants' rects are a subset of its own and the union is unchanged.
		// True, and a real win on a long scrollable list -- but it silently defeats
		// `vacuus-passthrough` on any descendant, so it stays off until a rect count
		// actually justifies it.
		if (ChildClip.Area() <= 0)
		{
			return;
		}
	}

	// include_non_dom_elements=true is required, not optional: scrollbars and
	// form-control internals are stored LAST in the children array and GetChild()
	// indexes the full array (Element.cpp:1139-1149), so the default `false` walks
	// right past genuinely interactive children.
	const int32 NumChildren = Element->GetNumChildren(/*include_non_dom_elements=*/true);
	for (int32 Index = 0; Index < NumChildren; ++Index)
	{
		if (Rml::Element* Child = Element->GetChild(Index))
		{
			Visit(Child, ChildClip);
		}
	}
}
}	 // namespace VaCuusInteractiveSnapshot

FVaCuusSnapshotBuildStats BuildVaCuusInteractiveSnapshot(
	Rml::Context& Context, FIntPoint ViewSize, uint64 Generation, FVaCuusInteractiveSnapshot& OutSnapshot)
{
	using namespace VaCuusInteractiveSnapshot;

	// Every RmlUi read below is UI-thread-only, including the ones that look const:
	// GetAbsoluteOffset/GetBox/GetNumChildren are non-const and lazily refresh caches.
	check(FVaCuusUIThread::IsInUIThread());

	// Keeps the arrays' capacity; the publisher hands us one of three buffers it
	// rotates through forever, so steady state must not allocate.
	OutSnapshot.Reset();
	OutSnapshot.Generation = Generation;
	OutSnapshot.ViewSize = ViewSize;

	// Focus, not hover: hover is where the mouse was last seen, which the game thread
	// already knows. What it cannot know is whether keys will land on an element --
	// Context::ProcessKeyDown dispatches to `focus`, or to the context root if there
	// is none, and the root is not a document and has no default action, so nothing
	// happens (Context.cpp:533-537). Root == "no document wants the keyboard".
	Rml::Element* const Focus = Context.GetFocusElement();
	OutSnapshot.bWantsKeyboardFocus = Focus != nullptr && Focus != Context.GetRootElement();

	FSnapshotWalk Walk(OutSnapshot);

	// Front to back: a higher document index is closer to the front
	// (PullDocumentToFront appends to the root's children, Context.cpp:468-481). The
	// order does not change the union, but it costs nothing to produce the rects in
	// the order RmlUi would have hit-tested them.
	const int32 NumDocuments = Context.GetNumDocuments();
	const FIntRect ViewRect(0, 0, ViewSize.X, ViewSize.Y);
	for (int32 Index = NumDocuments - 1; Index >= 0; --Index)
	{
		if (Rml::ElementDocument* Document = Context.GetDocument(Index))
		{
			Walk.Visit(Document, ViewRect);
		}
	}

	FVaCuusSnapshotBuildStats Stats;
	Stats.NumDocuments = NumDocuments;
	Stats.NumElementsVisited = Walk.NumElementsVisited;
	return Stats;
}
