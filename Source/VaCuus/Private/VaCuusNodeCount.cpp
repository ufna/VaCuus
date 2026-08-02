// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusNodeCount.h"

#include "VaCuusUIThread.h"

#include <RmlUi/Core/Element.h>

namespace VaCuusNodeCountPrivate
{
static int32 CountRecursive(Rml::Element* Element)
{
	// The hidden data-for clone template: excluded with its subtree (the
	// header's method statement carries the citations).
	if (Element == nullptr || Element->HasAttribute("data-for"))
	{
		return 0;
	}

	int32 Count = 1;
	const int NumChildren = Element->GetNumChildren(/*include_non_dom_elements=*/true);
	for (int Index = 0; Index < NumChildren; ++Index)
	{
		Count += CountRecursive(Element->GetChild(Index));
	}
	return Count;
}
}	 // namespace VaCuusNodeCountPrivate

int32 VaCuusNodeCount::CountNodes(Rml::Element* Root)
{
	check(FVaCuusUIThread::IsInUIThread());
	return VaCuusNodeCountPrivate::CountRecursive(Root);
}
