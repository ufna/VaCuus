// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusInteractiveSnapshot.h"

#include "VaCuusUIThread.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/ElementUtilities.h>

namespace VaCuusInteractiveSnapshot
{
/**
 * Marker attributes.
 *
 * ATTRIBUTE NAMES ARE CASE-SENSITIVE IN RmlUi, so the match below is not. Only
 * *tag* names get lowercased while parsing (StringUtilities::ToLower on the
 * element name, XMLParser.cpp:136,167); attribute names pass through verbatim --
 * BaseXMLParser::ReadAttributes stores `attributes[attribute] = ...` with the
 * author's exact spelling (BaseXMLParser.cpp:306-345) -- and
 * Element::HasAttribute is a plain map find (Element.cpp:861-864). A lowercase-only
 * lookup would therefore miss `vacuus-PassThrough` completely, and silently
 * ignoring an opt-OUT is the worst possible failure here: it eats the very clicks
 * the author asked us to let through to the game.
 *
 * So lowercase is the authoring CONVENTION (what the docs and tests use), while
 * the lookup accepts any casing. The cost is one pass over the element's own
 * attribute map -- zero to a handful of entries -- where a length compare rejects
 * nearly every name before a single character is examined. That is cheaper than
 * what it replaces: both marker names are 18 characters, past libstdc++'s 15-char
 * small-string limit, so each `HasAttribute("vacuus-...")` built a heap-allocated
 * temporary Rml::String per element per frame.
 *
 * Plain attributes rather than `data-*` names on purpose:
 * ElementUtilities::ApplyDataViewsControllers parses any `data-<type>-<modifier>`
 * attribute (ElementUtilities.cpp:407-424) and would quietly hand
 * type_name="vacuus" to Factory::InstanceDataView. Harmless, but it puts our
 * markers inside someone else's grammar.
 */
static constexpr char GPassthroughAttribute[] = "vacuus-passthrough";
static constexpr char GInteractiveAttribute[] = "vacuus-interactive";

/** ASCII case-insensitive compare against a lowercase literal; length-gated first. */
template <SIZE_T Length>
static bool EqualsIgnoreCaseAscii(const Rml::String& Name, const char (&Lowercase)[Length])
{
	constexpr SIZE_T NumChars = Length - 1;
	if (Name.size() != NumChars)
	{
		return false;
	}

	for (SIZE_T Index = 0; Index < NumChars; ++Index)
	{
		const char Character = Name[Index];
		const char Folded = (Character >= 'A' && Character <= 'Z') ? char(Character - 'A' + 'a') : Character;
		if (Folded != Lowercase[Index])
		{
			return false;
		}
	}

	return true;
}

/** Which VaCuus markers this element carries, in one pass over its attributes. */
struct FMarkerAttributes
{
	bool bPassthrough = false;
	bool bInteractive = false;
};

static FMarkerAttributes ReadMarkerAttributes(const Rml::Element& Element)
{
	FMarkerAttributes Markers;

	// `auto` on purpose: ElementAttributes is a config-dependent alias whose
	// value_type is std::pair<String, Variant> for itlib::flat_map but
	// std::pair<const String, Variant> for the std::unordered_map configuration --
	// naming either one explicitly would silently copy every entry in the other.
	for (const auto& Attribute : Element.GetAttributes())
	{
		if (EqualsIgnoreCaseAscii(Attribute.first, GPassthroughAttribute))
		{
			Markers.bPassthrough = true;
		}
		else if (EqualsIgnoreCaseAscii(Attribute.first, GInteractiveAttribute))
		{
			Markers.bInteractive = true;
		}
	}

	return Markers;
}

/**
 * The tag half of the interactive predicate (see the header for the whole rule).
 * These are exactly the tags RmlUi ships interactive behaviour for, plus `a`,
 * which is not a built-in element but is the universal "clickable" convention in
 * RML documents. Tags really are lowercase here: unlike attribute names, element
 * names ARE lowercased by the parser before instancing (XMLParser.cpp:136,167).
 *
 * The three scrollbar tags are controller decision D8. They are the names
 * ElementScroll instances its non-DOM children under (ElementScroll.cpp:193,213),
 * they have no tab-index and carry no attribute, and the DFS already walks them --
 * so before this a drag on a scrollbar was reported as pass-through and scrolled
 * the game instead of the list under the cursor.
 */
static bool IsKnownInteractiveTag(const Rml::String& Tag)
{
	return Tag == "button" || Tag == "input" || Tag == "select" || Tag == "textarea" || Tag == "a" ||
		Tag == "scrollbarvertical" || Tag == "scrollbarhorizontal" || Tag == "scrollbarcorner";
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

	/**
	 * bFocusBlocked mirrors RmlUi's CanFocus::NoAndNoChildren: `focus: none` prunes an
	 * element AND its whole subtree from tab and spatial navigation
	 * (ElementDocument.cpp:34-38), so it has to be carried down rather than tested per
	 * element. Unlike pointer-events -- which RmlUi does re-evaluate per element after
	 * descending -- this one really is inherited-by-pruning.
	 */
	void Visit(Rml::Element* Element, const FIntRect& Clip, bool bFocusBlocked);

	FVaCuusInteractiveSnapshot& Out;
	int32 NumElementsVisited = 0;
};

void FSnapshotWalk::Visit(Rml::Element* Element, const FIntRect& Clip, bool bFocusBlocked)
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

	// Both markers in one pass over this element's attributes; see the comment on
	// the marker names for why the match is case-insensitive.
	const FMarkerAttributes Markers = ReadMarkerAttributes(*Element);

	// The opt-out: `vacuus-passthrough` removes this element AND its subtree from
	// the snapshot, so the region reads as "not covered" and clicks reach the game.
	// A subtree prune (not a per-element skip) because that is what an author means
	// by marking a region pass-through; the inverse opt-back-in (`vacuus-capture`
	// on a descendant) is deliberately not implemented in M2 -- no use for it yet,
	// and it would make the rule two-sided for no gain.
	if (Markers.bPassthrough)
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
			Markers.bInteractive);

	// `focus: none` blocks this element and everything under it (see Visit's comment).
	// Evaluated before the flag below so an element that blocks focus is not itself
	// reported focusable.
	bFocusBlocked = bFocusBlocked || Computed.focus() == Rml::Style::Focus::None;

	// RmlUi's own focusability rule, not a proxy for it: visible (already true -- an
	// invisible element returned above), no `focus: none` anywhere up the chain, and
	// computed tab-index auto. Same three tests as ElementDocument's private
	// CanFocusElement (ElementDocument.cpp:30-43), which is what Tab, the arrow keys and
	// a mouse press all resolve focus through.
	const bool bFocusable = !bFocusBlocked && Computed.tab_index() == Rml::Style::TabIndex::Auto;

	if (bInteractive && Rect.Area() > 0)
	{
		// Both arrays, always together: the index is the only thing that pairs a rect
		// with its flags (see FVaCuusInteractiveSnapshot::RectFlags).
		Out.InteractiveRects.Add(Rect);
		Out.RectFlags.Add(EVaCuusRectFlags::Interactive | (bFocusable ? EVaCuusRectFlags::Focusable : EVaCuusRectFlags::None));
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
			Visit(Child, ChildClip, bFocusBlocked);
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
	// happens (Context.cpp:533-537).
	Rml::Element* const Focus = Context.GetFocusElement();
	bool bFocusIsRealElement = Focus != nullptr && Focus != Context.GetRootElement();

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
			// Controller decision D9: a document element holding focus is not a reason to
			// take Slate's keyboard focus away from the game. Show() focuses the document
			// itself whenever nothing inside it carries `autofocus` (FocusFlag::Auto), so
			// without this test every shown document would claim the keyboard. Compared
			// against the documents we are already iterating rather than cast or
			// tag-matched -- this is the exact set of document elements in the context.
			if (Document == Focus)
			{
				bFocusIsRealElement = false;
			}

			// bFocusBlocked starts false: `focus` defaults to auto and is inherited
			// (StyleSheetSpecification.cpp:375), so a document is focusable until an
			// author says otherwise -- and RmlUi relies on that, since Show() focuses the
			// document element itself.
			Walk.Visit(Document, ViewRect, /*bFocusBlocked=*/false);
		}
	}

	OutSnapshot.bWantsKeyboardFocus = bFocusIsRealElement;

	FVaCuusSnapshotBuildStats Stats;
	Stats.NumDocuments = NumDocuments;
	Stats.NumElementsVisited = Walk.NumElementsVisited;
	return Stats;
}
