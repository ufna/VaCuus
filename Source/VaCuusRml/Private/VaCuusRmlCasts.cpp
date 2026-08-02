// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusRmlCasts.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>

// The rmlui_dynamic_cast calls below are the ONLY legal ones outside the vendored
// tree, and only because this file compiles INTO VaCuusRml.so -- the header's
// whole argument. Do not add a cast helper anywhere else.

Rml::ElementFormControl* VaCuusCastFormControl(Rml::Element& Element)
{
	return rmlui_dynamic_cast<Rml::ElementFormControl*>(&Element);
}

bool VaCuusGetFormControlSelection(Rml::Element& Element, int32& OutBegin, int32& OutEnd)
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
