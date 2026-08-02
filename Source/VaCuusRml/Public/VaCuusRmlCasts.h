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
 * Cross-module rmlui_dynamic_cast, single-homed in VaCuusRml (M6 sweep, bead
 * VaCuus-akj.22) -- the same cure VaCuusScriptDocument.h applied to the M4
 * SIGSEGV, this time applied BEFORE the crash instead of after it.
 *
 * THE MECHANISM. RmlUi's custom RTTI compares the address of a function-local
 * static inside an INLINE member (RMLUI_RTTI_Define*, Traits.h:67-91) through
 * virtual IsClass (rmlui_dynamic_cast, Traits.h:93-105). UBT's Linux modular
 * builds compile with -fvisibility-ms-compat (LinuxToolChain.cs:439) and
 * -fvisibility-inlines-hidden (LinuxToolChain.cs:450); the identity statics
 * still land in every .so that instantiates the inline member -- as WEAK
 * DEFAULT dynamic symbols with GLOB_DAT relocations (readelf on
 * Binaries/Linux/: present in VaCuusRml.so, VaCuus.so, VaCuusRender.so). Which
 * copy a module's GOT binds to is therefore the DYNAMIC LINKER's scope
 * decision, and UE dlopens every module RTLD_LOCAL
 * (UnixPlatformProcess.cpp:109; UE modules are deliberately kept local by the
 * InitializeModule probe, :129-141).
 *
 * THE LOAD-ORDER ACCIDENT, settled by experiment (dlopen probe against the real
 * binaries, M6 research: docs/research/m6-api-notes/p2-sweep.md section 3): a
 * module dlopen'd as its own root binds the identity to ITS OWN copy; modules
 * pulled in as DT_NEEDED of one dlopen root all bind to the ROOT's copy. Ids
 * unify inside one dlopen closure and diverge across separate dlopens. Today's
 * editor works only because VaCuusRender loads at PostConfigInit (VaCuus.uplugin)
 * and its DT_NEEDED closure drags in VaCuusRml AND VaCuus, so all three unify on
 * VaCuusRender's copies -- while VaCuusJs, dlopen'd separately at Default phase,
 * got its own copies, which is exactly what both confirmed M4 cast failures were.
 * One .uplugin phase edit away from every cast below silently answering null,
 * and a monolithic build masks it completely.
 *
 * THE FIX SHAPE: non-inline, exported from THIS module. Both sides of the id
 * compare -- the GetStaticClassIdentifier() argument and the vtable'd IsClass
 * that answers it (RmlUi instances every element inside VaCuusRml.so, so the
 * vtables live here) -- then resolve through VaCuusRml.so's own relocations,
 * sound under EVERY load order. Rejected alternatives, for the record: "export
 * the statics with default visibility" does not work (they already are WEAK
 * DEFAULT and still diverge under RTLD_LOCAL scoping); rewriting the Traits.h
 * macros out-of-line is a diff across 24 vendored classes with permanent
 * upstream-merge burden; `linux_global_symbols` (UnixPlatformProcess.cpp:114-127)
 * would export vendored Rml symbols RTLD_GLOBAL -- a collision hazard the moment
 * any other plugin in a buyer's project vendors RmlUi.
 *
 * THE RULE THIS FILE ENFORCES: no plugin module outside VaCuusRml calls
 * rmlui_dynamic_cast. VaCuusJs already lives by it (membership sets and tag
 * compares, see FVaCuusScriptDocumentInstancer::IsOurs); these helpers are how
 * VaCuus and VaCuusRender live by it. The canary (VaCuus.Rml.CrossModuleCast)
 * asserts the helpers resolve a real <input type="text"> from another module,
 * so a regression of the mechanism fails a named test instead of parking carets
 * at end-of-text.
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
