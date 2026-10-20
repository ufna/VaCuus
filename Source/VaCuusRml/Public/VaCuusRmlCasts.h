// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

// For DLLEXPORT/DLLIMPORT behind VACUUSRML_API and for int32 -- the same one-header
// footprint VaCuusScriptDocument.h established for this module's public surface.
#include "HAL/Platform.h"

namespace Rml
{
class Element;
class ElementFormControl;
}	 // namespace Rml

/*
 * Cross-module cast helpers keep the RmlUi-dependent operations in one module.
 * Before 6.3, inline class identifiers could diverge across UE's separately
 * loaded libraries (bead VaCuus-akj.22). RmlUi 6.3 defines its RTTI methods
 * out of line: Traits.h:87-107, instantiated in Element.cpp:70 and the other
 * element implementation files. The original identity defect is fixed upstream.
 *
 * Retain these helpers as the existing VaCuus seam: callers need neither the
 * concrete form-control headers nor knowledge of the two separate selection
 * APIs. VaCuus.Rml.CrossModuleCast checks a real input through this boundary.
 */

/**
 * This element as a form control, or null when it is not one -- the license for
 * GetValue()/IsDisabled() on it. Virtual calls through the returned pointer are
 * ordinary vtable dispatch and safe from any module; only the CAST needed to
 * come home. UI thread, like every RmlUi call.
 */
VACUUSRML_API Rml::ElementFormControl* VaCuusCastFormControl(Rml::Element& Element);

/**
 * This element's selection as RmlUi CHARACTER offsets, or false if it has none.
 *
 * The WHOLE operation lives here, not just a cast, because it takes TWO casts:
 * GetSelection is NOT on Rml::ElementFormControl -- it exists separately on
 * ElementFormControlInput (ElementFormControlInput.h:47) and
 * ElementFormControlTextArea (ElementFormControlTextArea.h:72), because only
 * those two own a WidgetTextInput. The values come back as character offsets:
 * WidgetTextInput stores bytes and converts on the way out
 * (WidgetTextInput.cpp:360-369). A collapsed selection reports begin == end
 * (the caret); false means "this element has no selection API at all".
 * UI thread.
 */
VACUUSRML_API bool VaCuusGetFormControlSelection(Rml::Element& Element, int32& OutBegin, int32& OutEnd);
