// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace Rml
{
class Element;
}

namespace VaCuusNodeCount
{
/**
 * The Exp-REF-COUNT observable's STATED method (spec M6 2(g)): the reference
 * HUD's "~1,750 nodes" is a marketing claim made checkable only if what is
 * counted is pinned. This walk counts, from Root inclusive:
 *
 *  - every Rml::Element reached recursively -- which counts TEXT nodes too,
 *    because RmlUi's text nodes ARE elements (ElementText : Element,
 *    ElementText.h:9), one per non-whitespace text run (whitespace-only runs
 *    are never constructed, Factory.cpp:339-342);
 *  - RmlUi-generated scrollbars AS ENCOUNTERED: the walk asks for non-DOM
 *    children (GetNumChildren(true), Element.h:433-438 -- they are the tail of
 *    the children array, Element.cpp:1147-1150);
 *  - EXCLUDING any element carrying the data-for attribute and its whole
 *    subtree: that element is the hidden clone template (display:none,
 *    DataViewDefault.cpp:474), never painted; the generated rows it clones
 *    drop the attribute (:486-489) and are counted normally.
 *
 * UI thread only (checked): the tree is the UI thread's, and the count is
 * meaningful only between that thread's own frames.
 */
int32 CountNodes(Rml::Element* Root);
}	 // namespace VaCuusNodeCount
