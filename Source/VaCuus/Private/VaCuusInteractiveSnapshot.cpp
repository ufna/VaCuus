// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusInteractiveSnapshot.h"

#include "VaCuusTextInput.h"
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

/**
 * Which of the four direction keys would RmlUi actually move focus for, asked of the
 * element its arrow branch reads the `nav-*` property from.
 *
 * THE GATE IS TWO TESTS, AND ONLY THE FIRST ONE IS ABOUT PRESENCE.
 * ElementDocument::ProcessDefaultAction resolves a focus node, then wraps everything that
 * could move focus in `if (const Property* nav_property =
 * focus_node->GetLocalProperty(property_id))` (ElementDocument.cpp:628-638) -- so a missing
 * declaration is not a default, it is a dead branch. But a PRESENT declaration is not an
 * answer either: FindNextNavigationElement reads the VALUE and returns nullptr, moving
 * nothing, for `nav: none` (ElementDocument.cpp:785) and for `horizontal`/`vertical` asked
 * about the orthogonal axis (:787-794). Testing presence alone is what made
 * `body { nav: vertical; }` -- the idiomatic vertical menu -- consume the player's Left and
 * Right and move nothing, which is the exact failure the published flag exists to prevent.
 *
 * LOCAL, not computed, because that is what RmlUi reads: all four properties are
 * registered with inherited=false (StyleSheetSpecification.cpp:378-381), so an
 * ancestor's `nav` does not reach a descendant and GetComputedValues would answer a
 * different question. GetLocalProperty sees inline styles plus the element's own
 * matched definition (ElementStyle::GetLocalProperty).
 *
 * WHAT IS DELIBERATELY NOT PREDICTED -- the cases where a bit may still be set for a press
 * that moves nothing, stated because the alternative is a comment that claims more than the
 * code does:
 *
 *  1. THE STRING FORM (`nav-right: "#some-id"`). RmlUi returns nullptr for a value with no
 *     leading '#' (ElementDocument.cpp:760-766) and for an id that names nothing (:770-774),
 *     and both are cheap to state but not cheap to KNOW: resolution goes through
 *     ElementUtilities::GetElementById, a breadth-first walk of the whole document with a
 *     heap-allocated queue per call (ElementUtilities.cpp:25-48). Covering them would put
 *     four full document walks per frame into a snapshot whose entire design is O(1) per
 *     element. They are also a different KIND of error from the keywords above: `nav:
 *     vertical` is a CORRECT document that presence-testing mispredicted, while both string
 *     failures are authoring bugs RmlUi already reports loudly, in the log, at the moment of
 *     the press. So a string is taken at its word -- the author declared an edge.
 *  2. Even a resolved target can refuse: `next->Focus(true)` returns false when the target
 *     has `focus: none` (Element.cpp:1179-1184), which is the same walk away.
 *  3. Any other unit is answered NO, which matches RmlUi's own `default: break;`
 *     fall-through to `return nullptr` (ElementDocument.cpp:819-821).
 *
 * The caller gates the whole mask on bTabEntersFocus. That is exact for every keyword --
 * they all end in FindNextTabElement (:795, :798-799), which needs something focusable to
 * land on -- and conservative for the string form, whose target needs only `focus` != none
 * and not `tab-index: auto`. Conservative there means losing a press rather than the
 * player's input, which is the direction this decision errs in everywhere else.
 */
static EVaCuusNavDirection LocalNavDirections(Rml::Element& Element)
{
	struct FNavAxis
	{
		Rml::PropertyId Property;
		EVaCuusNavDirection Direction;
		bool bHorizontal;
	};

	static constexpr FNavAxis Axes[] = {
		{Rml::PropertyId::NavUp, EVaCuusNavDirection::Up, false},
		{Rml::PropertyId::NavDown, EVaCuusNavDirection::Down, false},
		{Rml::PropertyId::NavLeft, EVaCuusNavDirection::Left, true},
		{Rml::PropertyId::NavRight, EVaCuusNavDirection::Right, true},
	};

	EVaCuusNavDirection Directions = EVaCuusNavDirection::None;

	for (const FNavAxis& Axis : Axes)
	{
		const Rml::Property* Property = Element.GetLocalProperty(Axis.Property);
		if (Property == nullptr)
		{
			continue;
		}

		if (Property->unit == Rml::Unit::KEYWORD)
		{
			// `Property->value.Get<int>()` rather than Property->Get<int>(): that is the
			// read FindNextNavigationElement itself performs (ElementDocument.cpp:783), and
			// Property::Get goes through the numeric accessor, which is a different question.
			switch (static_cast<Rml::Style::Nav>(Property->value.Get<int>()))
			{
				case Rml::Style::Nav::None:
					continue;

				case Rml::Style::Nav::Horizontal:
					if (!Axis.bHorizontal)
					{
						continue;
					}
					break;

				case Rml::Style::Nav::Vertical:
					if (Axis.bHorizontal)
					{
						continue;
					}
					break;

				// Both reach FindNextTabElement from a document focus node
				// (ElementDocument.cpp:795 and :798-799), so both are answered by the
				// bTabEntersFocus gate the caller applies.
				case Rml::Style::Nav::Auto:
				case Rml::Style::Nav::TreeOrder:
					break;
			}
		}
		else if (Property->unit != Rml::Unit::STRING)
		{
			continue;
		}

		Directions |= Axis.Direction;
	}

	return Directions;
}

/**
 * RmlUi's own three-valued focusability answer, spelled out here because
 * ElementDocument.cpp's sits in an anonymous namespace (:25-28) and cannot be called.
 * Line-for-line with CanFocusElement (:30-44); the ORDER matters, because
 * `focus: none` and invisibility both prune the SUBTREE while a non-auto tab-index
 * only rejects the element itself.
 */
enum class ECanFocus : uint8
{
	Yes,
	No,
	NoAndNoChildren
};

static ECanFocus CanFocusElement(Rml::Element& Element)
{
	if (!Element.IsVisible())
	{
		return ECanFocus::NoAndNoChildren;
	}

	const Rml::ComputedValues& Computed = Element.GetComputedValues();

	if (Computed.focus() == Rml::Style::Focus::None)
	{
		return ECanFocus::NoAndNoChildren;
	}

	if (Computed.tab_index() == Rml::Style::TabIndex::Auto)
	{
		return ECanFocus::Yes;
	}

	return ECanFocus::No;
}

static bool HasTabTargetAmongChildren(Rml::Element& Parent);

/** Mirrors ElementDocument::SearchFocusSubtree (:728-737), reduced to "is there one". */
static bool IsTabTargetOrContainsOne(Rml::Element& Element)
{
	switch (CanFocusElement(Element))
	{
		case ECanFocus::Yes:
			return true;

		case ECanFocus::NoAndNoChildren:
			return false;

		case ECanFocus::No:
			break;
	}

	return HasTabTargetAmongChildren(Element);
}

/**
 * Mirrors ElementDocument::SearchFocusSubtreeChildren (:739-750).
 *
 * GetNumChildren()'s DEFAULT -- include_non_dom_elements = FALSE -- and that is the whole
 * reason this walk is separate from the snapshot DFS below, which passes true because
 * scrollbars and form-control internals genuinely take clicks. Tab cannot reach them:
 * :741 asks for the DOM count, so the search never indexes past the DOM children and never
 * enters a non-DOM subtree at all. Nothing in RmlUi's own code puts `tab-index: auto` on a
 * non-DOM element (ElementFormControl.cpp:8 is the only setter and its subclasses are
 * parsed from RML), but tab-index is an ordinary registered property
 * (StyleSheetSpecification.cpp:375) and non-DOM elements are style-matched like any other,
 * so one line of author RCSS -- `scrollbarvertical { tab-index: auto; }` -- is enough to
 * make the difference real. Mirroring the default makes it unreachable instead of unlikely.
 *
 * DIRECTION IS DROPPED, unlike :744's forward/backward index, because the question here is
 * EXISTENCE and both directions visit the same set. Tab and Shift+Tab therefore get the
 * same answer, which is correct: they differ in which target they land on, never in whether
 * there is one.
 */
static bool HasTabTargetAmongChildren(Rml::Element& Parent)
{
	const int32 NumChildren = Parent.GetNumChildren();
	for (int32 Index = 0; Index < NumChildren; ++Index)
	{
		if (Rml::Element* Child = Parent.GetChild(Index))
		{
			if (IsTabTargetOrContainsOne(*Child))
			{
				return true;
			}
		}
	}

	return false;
}

/**
 * Would a TAB press move focus INTO this document, given that the DOCUMENT ELEMENT is what
 * holds focus right now? (The caller checks that; it is bTabEntersFocus's other
 * precondition.)
 *
 * WHY THIS COLLAPSES TO ONE CALL. FindNextTabElement(document, forward, wrap_around=true --
 * ElementDocument.h:104) with current_element == document takes the first branch
 * unconditionally (`forward || current_element == document`, :679-681) and searches the
 * document's children; the upward walk is then skipped entirely because its condition is
 * `while (child != document)` (:691); and the wrap-around tail cannot add anything, since
 * its self-focus test is guarded by `current_element != document` (:721) and its fallback
 * is the very same SearchFocusSubtreeChildren(document, forward) call (:725). So the whole
 * function reduces to this, and the DOCUMENT ITSELF IS NOT A CANDIDATE -- which is right,
 * because it is where focus already is.
 *
 * PER DOCUMENT, NOT PER CONTEXT, because that is FindNextTabElement's own scope: it takes
 * `document = current_element->GetOwnerDocument()` (:675) and every branch stops there. A
 * focusable in a SECOND document is not somewhere Tab can go from this one.
 *
 * A SEPARATE TRAVERSAL FROM THE SNAPSHOT DFS, DELIBERATELY, and this is the fix for bead
 * VaCuus-akj.6.36. The DFS answers a POINTER-COVERAGE question and prunes for
 * pointer-coverage reasons -- `vacuus-passthrough` opts a subtree out of taking clicks, an
 * empty child clip means no descendant can be hit -- neither of which RmlUi's tab search
 * has ever heard of. Accumulating focusability inside that walk therefore made
 * bTabEntersFocus a strict UNDER-approximation: a document whose only focusables sat in a
 * pass-through wrapper, or in a collapsed `overflow:hidden; height:0` container, reported
 * "Tab cannot enter" while RmlUi entered fine, and the entering press was then consumed by
 * the game AND moved UI focus. Two questions, two walks.
 *
 * COST: at most one extra traversal per FRAME, not per document, and only in the state the
 * flag is read in -- a document is shown and nothing inside it holds focus. It early-outs
 * on the first target, which in a HUD is usually the first button.
 */
static bool DoesTabEnterDocument(Rml::ElementDocument& Document)
{
	return HasTabTargetAmongChildren(Document);
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
	 * (ElementDocument.cpp:37-38), so it has to be carried down rather than tested per
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

	// Same three tests as ElementDocument's private CanFocusElement
	// (ElementDocument.cpp:30-44): visible (already true -- an invisible element returned
	// above), no `focus: none` anywhere up the chain, and computed tab-index auto.
	//
	// PER RECT, AND ONLY PER RECT. This is the flag on a reported rectangle, answering
	// "would a click here land on something focusable"; it is NOT where bTabEntersFocus
	// comes from, and deriving that from these would be wrong in both directions -- a
	// tabbable element with a clipped or zero-area box is never reported at all, and this
	// walk descends into non-DOM children that Tab provably cannot reach. That question has
	// its own traversal, DoesTabEnterDocument() above.
	const bool bFocusable = !bFocusBlocked && Computed.tab_index() == Rml::Style::TabIndex::Auto;

	if (bInteractive && Rect.Area() > 0)
	{
		// Controller decision D14a. Asked only for rects that are actually being reported, and
		// only after the cheap tests above have passed: the answer is two string compares on a
		// tag plus at most one attribute lookup, which is not worth paying for a <div>.
		//
		// NOT GATED ON bFocusable, even though every text field is focusable: the two flags
		// answer different questions and a caller that wants both asks for both. Gating would
		// make TextInput silently imply Focusable and hide the day RmlUi lets one exist without
		// the other (a `focus: none` input, say -- which is unfocusable AND still takes text
		// from a script-driven caret).
		const bool bTextInput = VaCuusTextInput::IsTextInputElement(*Element);

		// Both arrays, always together: the index is the only thing that pairs a rect
		// with its flags (see FVaCuusInteractiveSnapshot::RectFlags).
		Out.InteractiveRects.Add(Rect);
		Out.RectFlags.Add(EVaCuusRectFlags::Interactive |
			(bFocusable ? EVaCuusRectFlags::Focusable : EVaCuusRectFlags::None) |
			(bTextInput ? EVaCuusRectFlags::TextInput : EVaCuusRectFlags::None));
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

	// The document element that holds focus, if focus is on a document at all. Both entry
	// flags below need it: it is the element ProcessDefaultAction would run on, the element
	// whose local `nav-*` the arrow branch would read, and the only subtree
	// FindNextTabElement searches.
	Rml::ElementDocument* FocusedDocument = nullptr;

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
				FocusedDocument = Document;
			}

			// bFocusBlocked starts false: `focus` defaults to auto and is inherited
			// (StyleSheetSpecification.cpp:376), so a document is focusable until an
			// author says otherwise -- and RmlUi relies on that, since Show() focuses the
			// document element itself.
			Walk.Visit(Document, ViewRect, /*bFocusBlocked=*/false);
		}
	}

	OutSnapshot.bWantsKeyboardFocus = bFocusIsRealElement;

	// Task 14 acceptance decision A1: can a navigation key ENTER this document's focus
	// from where focus is now? See the two flags' comments for why they are separate and
	// why neither is derivable from the rects.
	//
	// A DOCUMENT MUST HOLD FOCUS, not merely exist. If focus is null or on the context
	// root, ProcessKeyDown dispatches to the root (Context.cpp:534-537), whose
	// ProcessDefaultAction is Element's and has no navigation in it -- so no key moves
	// focus and consuming one would be a lie. If focus is on something INSIDE a document,
	// bWantsKeyboardFocus is already true and these are irrelevant (and false: the loop
	// above only sets FocusedDocument when the focus IS a document element).
	// Asked here, after the DFS, rather than accumulated inside it: the DFS prunes for
	// pointer-coverage reasons RmlUi's tab search does not share. See DoesTabEnterDocument().
	OutSnapshot.bTabEntersFocus = FocusedDocument != nullptr && DoesTabEnterDocument(*FocusedDocument);
	OutSnapshot.DirectionsEnteringFocus =
		OutSnapshot.bTabEntersFocus ? LocalNavDirections(*FocusedDocument) : EVaCuusNavDirection::None;

	// Controller decision D14b, and the second half of the IME contract: whether a control
	// that takes TEXT holds focus right now, plus everything the platform IME will pull about
	// it (D15). Read from THIS context, so it is exact per view -- unlike the caret and the
	// field generation, which RmlUi only offers process-wide.
	//
	// AFTER the walk rather than inside it, because it is a property of the focus and not of
	// any rect, and because it reads the element's live value and selection -- work that must
	// happen once per frame, not once per element.
	OutSnapshot.bTextInputFocused = VaCuusTextInput::FillTextFieldState(Context, OutSnapshot.TextField);

	FVaCuusSnapshotBuildStats Stats;
	Stats.NumDocuments = NumDocuments;
	Stats.NumElementsVisited = Walk.NumElementsVisited;
	return Stats;
}
