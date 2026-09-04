#!/usr/bin/env python3
"""
gen_rcss_matrix.py -- generates docs/buyer/rcss-matrix.md from the vendored
RmlUi sources (M6 Task 7, spec 3.4: the matrix is GENERATED, keyed to the
vendored SHA, so an RmlUi bump regenerates it instead of rotting it).

Run from the plugin root:

    python3 Tools/gen_rcss_matrix.py

What it parses (mechanically -- no hand-copied rows):
  1. StyleSheetSpecification::RegisterDefaultProperties in
     Source/ThirdParty/RmlUi/Source/Core/StyleSheetSpecification.cpp --
     every RegisterProperty(PropertyId::X, "name", "default", inherited,
     forces_layout) with its .AddParser chain, and every RegisterShorthand.
  2. Factory::RegisterDefault*Instancers registration lines in
     Source/ThirdParty/RmlUi/Source/Core/Factory.cpp -- decorator, filter
     and font-effect names.

What is hand-maintained (and therefore what an RmlUi bump must be re-checked
against by hand -- regenerating does NOT revalidate these):
  - the ANNOTATIONS dict below (plugin-side truth -- what VaCuus refuses or
    treats specially);
  - the At-rules section, including the @font-face grammar and its
    root-relative src rule;
  - the "Custom properties and var()" section;
  - the "What is NOT here" section.
Each carries its source cites; keep the cites open-and-checked when editing
(project convention). The at-rule list in particular was a hardcoded string
that had gone stale against StyleSheetParser.cpp -- it omitted @font-face,
the ONE font route a buyer has (bead 9r2).

Invoked with `bash`-less plain python3; writes docs/buyer/rcss-matrix.md.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPEC_CPP = ROOT / "Source/ThirdParty/RmlUi/Source/Core/StyleSheetSpecification.cpp"
FACTORY_CPP = ROOT / "Source/ThirdParty/RmlUi/Source/Core/Factory.cpp"
SHA_FILE = ROOT / "Source/ThirdParty/RmlUi/VENDORED_SHA.txt"
OUT = ROOT / "docs/buyer/rcss-matrix.md"

# Plugin-side truth per property/name. HAND-MAINTAINED; every cite opened when
# written (2026-08-02). Rendered into the Notes column / section notes.
ANNOTATIONS = {
    "decorator": "Renders through the recorder; `shader(...)` values resolve builtin names "
    "then registered UMaterial style keys (see the decorators section below).",
    "filter": "v1 compiles `blur` only; the other nine types are refused (one Warning per type, "
    "effect dropped per element -- VaCuusRecordingRenderInterface.cpp:866-907). Per-element "
    "filter blur is not a shipped v1 surface (arch spec 5, M5 amendment): the verified blur "
    "consumer is backdrop-filter.",
    "backdrop-filter": "`backdrop-filter: blur(...)` is the shipped glass path -- distilled at record "
    "time and re-blurred every engine frame at composite time (arch spec 5, M5 amendment). "
    "Non-blur backdrop filters are refused like element filters.",
    "box-shadow": "DOES NOT RENDER in v1 -- the shadow needs the current layer captured to a "
    "texture (GeometryBoxShadow.cpp:235 -> RenderInterface::SaveLayerAsTexture) and the replayer "
    "has no layer render targets. It is REFUSED, once per view, with a Warning naming the property "
    "and the substitute; the element then renders its normal background and border with the shadow "
    "dropped (bead VaCuus-u0q; VaCuus patch #3 to the vendored RmlUi is what makes the failure "
    "harmless -- before it the element rendered as an opaque WHITE rectangle and republished every "
    "frame). Substitute: `decorator: ninepatch(...)` with a pre-blurred shadow image, or "
    "`font-effect: glow` for text. Also NOT animatable even where it renders: RmlUi refuses the key "
    "at animation start with a Warning (ElementAnimation.cpp:640-648); `vacuus lint` flags it at "
    "authoring time (Web/packages/cli/lib/lint.mjs:68-99).",
    "mask-image": "PARSES BUT DOES NOT MASK in v1 -- masking needs the mask layer captured as a "
    "filter (ElementEffects.cpp:306 -> RenderInterface::SaveLayerAsMaskImage) and the replayer has "
    "no layer render targets. It is REFUSED, once per view, with a Warning (bead VaCuus-iuv). The "
    "element renders UNMASKED **and the mask artwork is drawn over it**, because the layer the "
    "decorators were drawn into is not a real render target -- verified on screen, not inferred. "
    "Substitute: bake the alpha into the image asset and use `decorator: image`/`ninepatch`, or "
    "clip with `overflow: hidden` plus `border-radius`.",
    "font-effect": "Glyph generation for effects (glow/outline) is the measured spike class -- "
    "~4.2 ms on the reference HUD's FIRST Record, UI thread, before first publish "
    "(perf-guide.md, Exp-GLYPH-WARMUP). Budget it at load, not per frame.",
    "pointer-events": "Inherited, but there is no subtree pruning -- `none` on a parent still walks "
    "children during hit tests (docs/research/m2-api-notes/rmlui-input.md:198).",
    "nav-up": "The nav-* family is NOT inherited: spatial-nav rules must sit on the focusable "
    "element itself, and targets use `#id` syntax (rmlui-input.md:190-207, :450).",
    "nav-down": "See nav-up.",
    "nav-left": "See nav-up.",
    "nav-right": "See nav-up.",
    "nav": "Shorthand for the nav-* family; the non-inheritance note on nav-up applies.",
    "transition": "THERE IS NO `ease` FAMILY. The complete tween table is eleven families -- "
    "back bounce circular cubic elastic exponential linear quadratic quartic quintic sine -- "
    "each with -in/-out/-in-out (PropertyParserAnimation.cpp:27-77). `ease-in-out` is not in it, "
    "and an unrecognized token drops the WHOLE declaration with one generic \"Syntax error parsing "
    "property declaration\" (:240 misses the map, :267 fails the duration sscanf, :301-315 returns "
    "false). Use `cubic-in-out` for CSS's `ease-in-out`. Keywords must be LOWERCASE here: "
    "`transition` does not lowercase its token (:240) while `animation` does (:133). gotchas.md #3. "
    "Timing tokens MAY come from `var()`, but only because the vendored tree is patched for it "
    "(VENDORED_TAG.txt patch #4, bead VaCuus-6gj): upstream reads this property before computed "
    "values exist (ElementStyle.cpp:388) and drops the whole declaration with no diagnostic at all. "
    "gotchas.md #3b.",
    "animation": "Same tween table as `transition` (PropertyParserAnimation.cpp:27-77), but this "
    "parser LOWERCASES each token before the lookup (:133), so `Cubic-Out` works here and silently "
    "kills a `transition`. Keyframe values may contain `var()` (Element.cpp:2784-2798), and so may "
    "this property's own value -- it is read through ComputedValues::animation(), which resolves "
    "variables (ComputedValues.cpp:8-16), so unlike `transition` it never needed a patch.",
    "display": "No CSS Grid in RmlUi (flex-first is the market bar, arch spec 1); the keyword "
    "list here is exhaustive.",
    "drag": "Only `drag-drop` and `clone` send dragover/dragout/dragdrop/dragmove -- plain `drag` "
    "sends dragstart/drag/dragend and nothing else (Context.cpp:692 sets drag_verbose for exactly "
    "those two), and `block` stops an ancestor from becoming the drag source. `clone` ships a "
    "ghost under the cursor styleable via `:drag` -- the ONE drag pseudo-class, set on the clone "
    "only (Context.cpp:1504); sources and targets get no pseudo-class at all. Shipped reference: "
    "vacuus.DragDemo + gotchas.md #24; proven by VaCuus.Js.DragDrop.",
    "position": "absolute resolves against the nearest positioned ancestor; there is no "
    "browser-style default-positioned root chain past the document "
    "(Content/DevUI/M5Hud/vacuus-base.rcss:29-31).",
}


def parse_properties(text):
    """Yield (name, default, inherited, forces_layout, parsers) from the
    RegisterDefaultProperties function body."""
    m = re.search(
        r"^void StyleSheetSpecification::RegisterDefaultProperties\(\)\s*\n\{(.*?)^\}",
        text,
        re.S | re.M,
    )
    if not m:
        sys.exit("RegisterDefaultProperties not found -- did the vendored layout change?")
    body = m.group(1)
    props, shorthands = [], []
    # Statements end with ';'. Chains span lines; normalize whitespace per statement.
    for stmt in body.split(";"):
        stmt = " ".join(stmt.split())
        pm = re.search(
            r'RegisterProperty\(PropertyId::\w+, "([^"]+)", "([^"]*)", (true|false), (true|false)\)(.*)',
            stmt,
        )
        if pm:
            name, default, inherited, layout, chain = pm.groups()
            parsers = []
            for am in re.finditer(r'\.AddParser\("([^"]+)"(?:, "([^"]*)")?\)', chain):
                ptype, keywords = am.groups()
                parsers.append(f"{ptype}: {keywords}" if keywords else ptype)
            props.append((name, default, inherited == "true", layout == "true", parsers))
            continue
        sm = re.search(
            r'RegisterShorthand\(ShorthandId::\w+, "([^"]+)", "([^"]+)", ShorthandType::(\w+)\)',
            stmt,
        )
        if sm:
            shorthands.append(sm.groups())
    return props, shorthands


def parse_instancers(text, kind):
    """Yield (name, line_no) for Register<kind>Instancer("name", ...) default
    registrations (the ones inside RegisterDefaultInstancers, i.e. with a
    string literal first argument)."""
    out = []
    for i, line in enumerate(text.splitlines(), 1):
        m = re.search(rf'Register{kind}Instancer\("([^"]+)"', line)
        if m:
            out.append((m.group(1), i))
    return out


def func_lines(text, signature):
    """1-based [start, end] line numbers of a top-level function body."""
    lines = text.splitlines()
    start = next(i for i, l in enumerate(lines, 1) if signature in l)
    end = next(i for i, l in enumerate(lines[start:], start + 1) if l == "}")
    return start, end


def esc(s):
    return s.replace("|", "\\|")


def main():
    spec = SPEC_CPP.read_text()
    factory = FACTORY_CPP.read_text()
    sha = SHA_FILE.read_text().strip()
    props, shorthands = parse_properties(spec)
    decorators = parse_instancers(factory, "Decorator")
    filters = parse_instancers(factory, "Filter")
    font_effects = parse_instancers(factory, "FontEffect")
    fstart, fend = func_lines(spec, "void StyleSheetSpecification::RegisterDefaultProperties()")

    w = []
    w.append("# Supported RCSS matrix -- generated, keyed to the vendored RmlUi")
    w.append("")
    w.append(f"**Vendored RmlUi SHA:** `{sha}` (`Source/ThirdParty/RmlUi/VENDORED_SHA.txt`)")
    w.append("")
    w.append("**GENERATED FILE -- do not edit the tables by hand.** Regenerate after any RmlUi")
    w.append("bump with `python3 Tools/gen_rcss_matrix.py` (run from the plugin root). The")
    w.append("property and shorthand tables are parsed mechanically from")
    w.append(f"`StyleSheetSpecification::RegisterDefaultProperties` (StyleSheetSpecification.cpp:{fstart}-{fend}");
    w.append("in this vendored tree); the decorator/filter/font-effect names from the")
    w.append("`Factory::RegisterDefault*Instancers` registrations in Factory.cpp. The Notes")
    w.append("column is hand-maintained inside the generator script (plugin-side truth with")
    w.append("source cites) and survives regeneration.")
    w.append("")
    w.append("The value grammar per property is its parser chain: `keyword: a, b` accepts")
    w.append("those keywords; `length`, `length_percent`, `number`, `color`, `string` etc.")
    w.append("are the RCSS value parsers registered in RegisterDefaultParsers (same file).")
    w.append("A property with multiple parsers accepts any of them, tried in order.")
    w.append("")
    w.append(f"## Properties ({len(props)})")
    w.append("")
    w.append("| Property | Default | Inherited | Affects layout | Accepted values | Notes |")
    w.append("|---|---|---|---|---|---|")
    for name, default, inh, layout, parsers in props:
        note = ANNOTATIONS.get(name, "")
        w.append(
            f"| `{name}` | `{esc(default)}`{'' if default else ' (empty)'} | {'yes' if inh else 'no'} "
            f"| {'yes' if layout else 'no'} | {esc('; '.join(parsers)) or '--'} | {esc(note)} |"
        )
    w.append("")
    w.append(f"## Shorthands ({len(shorthands)})")
    w.append("")
    w.append("| Shorthand | Expands to | Expansion type | Notes |")
    w.append("|---|---|---|---|")
    for name, expansion, stype, in shorthands:
        note = ANNOTATIONS.get(name, "")
        w.append(f"| `{name}` | {esc(expansion)} | {stype} | {esc(note)} |")
    w.append("")
    w.append(f"## Decorators ({len(decorators)} registered names)")
    w.append("")
    w.append("Registered in Factory.cpp (line numbers in this vendored tree). All render")
    w.append("through the record/replay path. `shader(<name-or-key>)` resolves builtin")
    w.append("names first (`glass-panel` ships), then UMaterial style keys registered by the")
    w.append("game (`UVaCuusStyleSet` -- the M5 material-decorator tier; unknown keys are")
    w.append("refused once with a Warning naming both halves of what would have worked).")
    w.append("")
    w.append("A style-key material sees Slate's texture-coordinate slots, so a material")
    w.append("authored for UMG ports unchanged: `GetUserInterfaceUV`'s Tiling (slot 2) and")
    w.append("Pixel Size (slot 3) carry the values Slate would hand it -- (1, 1) and the")
    w.append("decorator's paint box in whole pixels -- and its three UV outputs are the paint")
    w.append("box's own 0..1 (VaCuusMaterial.usf). Customized UVs run in the material's vertex")
    w.append("stage here as they do under Slate, and a material that declares any reads the")
    w.append("raw channels at the pixel stage, exactly as Slate's own UI shader hands them.")
    w.append("")
    w.append("The six gradient decorators are **antialiased in screen space**, which is a")
    w.append("deliberate deviation from RmlUi's reference backend. A hard colour break --")
    w.append("two stops at the same position, which is how a segmented dial ring or a")
    w.append("hazard hatch is written -- is rendered as a one-pixel ramp instead of a step,")
    w.append("so a diagonal break does not staircase. The width comes from the analytic")
    w.append("screen-space derivative of the gradient parameter, so it is one pixel at any")
    w.append("size, scale or transform; a stop pair that is already more than a pixel apart")
    w.append("is left bit-for-bit alone, and a smooth gradient is unchanged. A repeating")
    w.append("gradient gets the same treatment at its period edge as at its interior stops,")
    w.append("and once its bands shrink past about two pixels it fades to the period's mean")
    w.append("colour rather than aliasing (VaCuusGradient.usf).")
    w.append("")
    # Restored into the GENERATOR on 2026-08-06 (bead VaCuus-6gj): commit 00497b7 wrote
    # these two paragraphs into docs/buyer/rcss-matrix.md by hand, and the next
    # regeneration -- this one -- silently reverted them, which is the failure mode this
    # whole script exists to prevent. Edit here, never the markdown.
    w.append("This covers the gradient FILL only. Element outlines -- notably `border-radius`")
    w.append("arcs -- are tessellated geometry, and geometry is antialiased by a separate,")
    w.append("OPT-IN knob: `vacuus.ViewSampleCount` (1 by default, 2/4/8) multisamples the")
    w.append("per-view render target. Off, a curve's outline is a hard staircase -- a")
    w.append("206x206 `border-radius` disc measures exactly **0** partially covered pixels,")
    w.append("because a single-sampled rasterizer produces a step function; at 4x it measures")
    w.append("432, and text, axis-aligned edges and gradient fills come back byte-identical.")
    w.append("The knob costs one extra multisampled target per view (`N x 7.91 MiB` at 1080p,")
    w.append("on top of the RT it resolves into), which is why it is off: see")
    w.append("`perf-guide.md`'s cost table and `docs/research/proofs/3tg-view-msaa`.")
    w.append("")
    w.append("`border-radius` takes a LENGTH ONLY, not a percentage -- all four longhands")
    w.append("register the `\"length\"` parser and nothing else")
    w.append("(StyleSheetSpecification.cpp:300-303), so `border-radius: 50%` does not parse")
    w.append("and the corner stays square. Spell a circle as half its own side")
    w.append("(`width: 64px; border-radius: 32px`).")
    w.append("")
    w.append("| Decorator | Factory.cpp line |")
    w.append("|---|---|")
    for name, line in decorators:
        w.append(f"| `{name}` | :{line} |")
    w.append("")
    w.append(f"## Filters ({len(filters)} registered names) -- blur-only in v1")
    w.append("")
    w.append("RmlUi registers ten filter instancers; the VaCuus recorder compiles exactly")
    w.append("ONE -- `blur`. Every other type is refused with one Warning per type and the")
    w.append("effect dropped per element (VaCuusRecordingRenderInterface.cpp:866-907;")
    w.append("returning handle 0 is RmlUi's own safe-refusal contract, and RmlUi then warns")
    w.append("per element, ElementEffects.cpp:161-165). The shipped, verified blur consumer")
    w.append("is `backdrop-filter` (glass); per-element `filter:` blur is a v1.x item (arch")
    w.append("spec 5, M5 amendment).")
    w.append("")
    w.append("`box-shadow`'s blur never reaches this table at all: the shadow is refused a")
    w.append("step earlier, at the layer capture it is built on (see the `box-shadow` row")
    w.append("above and `Layer capture` below), so its CompileFilter(\"blur\") call is never")
    w.append("made.")
    w.append("")
    w.append("| Filter | Factory.cpp line | v1 status |")
    w.append("|---|---|---|")
    for name, line in filters:
        status = "compiled; shipped consumer is `backdrop-filter` (glass)" if name == "blur" else "refused (one Warning per type; effect dropped per element)"
        w.append(f"| `{name}` | :{line} | {status} |")
    w.append("")
    w.append(f"## Font effects ({len(font_effects)} registered names)")
    w.append("")
    w.append("All four render. Effect-glyph generation is the measured load-spike class:")
    w.append("~4.2 ms on the reference HUD's first Record (UI thread, before first publish,")
    w.append("never the game thread) -- see perf-guide.md, Exp-GLYPH-WARMUP, before shipping")
    w.append("effect-heavy styles.")
    w.append("")
    w.append("| Font effect | Factory.cpp line |")
    w.append("|---|---|")
    for name, line in font_effects:
        w.append(f"| `{name}` | :{line} |")
    w.append("")
    w.append("## Layer capture -- the two properties v1 refuses outright")
    w.append("")
    w.append("Rml::RenderInterface has 21 virtuals and the VaCuus recorder now overrides all")
    w.append("21, but two of them are REFUSALS rather than recordings -- `SaveLayerAsTexture`")
    w.append("and `SaveLayerAsMaskImage` (RenderInterface.h:112-116). Both mean \"hand me the")
    w.append("current layer back\", and this renderer has no layer to hand back: PushLayer,")
    w.append("CompositeLayers and PopLayer are recorded and then skipped at replay, so every")
    w.append("draw between a push and a pop lands directly in the base render target. Glass")
    w.append("(`backdrop-filter`) does not need them -- it is distilled from the buffer and")
    w.append("composited per engine frame -- which is why it ships and these do not.")
    w.append("")
    w.append("Each is refused with **one Warning per view**, latched, naming the property and")
    w.append("its substitute. A document with two hundred shadowed elements logs one line.")
    w.append("")
    w.append("| Property | Reaches | v1 behaviour |")
    w.append("|---|---|---|")
    w.append("| `box-shadow` | `SaveLayerAsTexture` | Shadow dropped; the element renders its "
             "**normal background and border**. No per-frame cost. |")
    w.append("| `mask-image` | `SaveLayerAsMaskImage` | Element renders **unmasked**, and the "
             "mask artwork is **drawn over it**. |")
    w.append("")
    w.append("Both are pinned by automation: `VaCuus.Render.LayerCapture.Refused` and")
    w.append("`VaCuus.Render.LayerCapture.RestyleChurn`.")
    w.append("")
    w.append("## At-rules")
    w.append("")
    w.append("Dispatched in StyleSheetParser.cpp on `at_rule_identifier` (:783-869 in this")
    w.append("tree): **`@keyframes`** (:786), **`@decorator`** (:790), **`@spritesheet`**")
    w.append("(:798), **`@media`** (:835), **`@font-face`** (:855). Anything else logs")
    w.append("\"Invalid at-rule identifier\" (:869). `@media` supports the query grammar")
    w.append("parsed by ParseMediaFeatureMap (:567-684 -- width/height/resolution/theme et al.).")
    w.append("")
    w.append("### `@font-face` -- and its `src` is ROOT-relative, unlike every other path")
    w.append("")
    w.append("There is NO public C++ font-loading API (`Rml::LoadFontFace` is called only")
    w.append("inside the plugin, VaCuusEngine.cpp:138-153), so `@font-face` is the ONE route")
    w.append("for a project shipping its own faces. It works.")
    w.append("")
    w.append("```css")
    w.append("@font-face {")
    w.append("\tfont-family: \"Michroma\";                  /* REQUIRED, a quoted string */")
    w.append("\tsrc: myapp/fonts/Michroma-Regular.ttf;    /* REQUIRED. ROOT-relative. Bare path, NO url() */")
    w.append("\tfont-weight: normal;                     /* all | normal | bold | <number>; default `all` */")
    w.append("\tfont-style: normal;                      /* normal | italic; default normal */")
    w.append("\t-rmlui-fallback-face: false;             /* RmlUi extension; default false */")
    w.append("\t-rmlui-face-index: 0;                    /* RmlUi extension, for collections; default 0 */")
    w.append("}")
    w.append("```")
    w.append("")
    w.append("Grammar at StyleSheetParser.cpp:294-330 (the property set) and :525-564 (the")
    w.append("block: both required properties checked, then one LoadFontFace per src). `src`")
    w.append("is COMMA-EXPANDED into a list of bare paths -- `url()` is not part of this")
    w.append("grammar at all.")
    w.append("")
    w.append("**ROOT-relative, not document-relative.** `src` is passed verbatim with no")
    w.append("`JoinPath` (:561), handed straight to the file interface")
    w.append("(FontEngineDefault/FontProvider.cpp:94) and resolved against the ordered document")
    w.append("roots (Source/VaCuus/Private/VaCuusContentPaths.cpp:92-103). `<link>` and")
    w.append("`<script src>` are the opposite -- document-relative (gotchas.md #12) -- so a")
    w.append("sheet at `Content/DevUI/myapp/app.rcss` links its neighbours bare but must spell")
    w.append("its fonts `myapp/fonts/Face.ttf`.")
    w.append("")
    w.append("**Variable fonts render at their default weight for every requested weight**: the")
    w.append("default font engine calls `FT_New_Face` without setting a variation axis. Ship")
    w.append("static instances, one file per weight.")
    w.append("")
    w.append("## Custom properties and `var()` -- the only theming layer")
    w.append("")
    w.append("Fully implemented in this vendored tree, and worth knowing because **there is no")
    w.append("`calc()`** (see below): `var()` is the whole of RCSS's computed-value story.")
    w.append("")
    w.append("```css")
    w.append("body { --accent: #38BDF8; --pad: 12px; }")
    w.append("#panel { color: var(--accent); padding: var(--pad); }")
    w.append("#panel.alt { color: var(--missing, #F87171); }   /* fallback after the comma */")
    w.append("```")
    w.append("")
    w.append("- **Declared** like any property whose name starts with `--`; the value is stored")
    w.append("  unparsed as `Unit::STRING`, or as `Unit::VAR_EXPRESSION` when it itself contains")
    w.append("  a `var()` (PropertySpecification.cpp:233-248). A normal property whose value")
    w.append("  contains `var()` is likewise stored as VAR_EXPRESSION and resolved at compute")
    w.append("  time, not parse time (:260-300).")
    w.append("- **Substituted** with fallbacks (`var(--x, <fallback>)`, nested parens counted)")
    w.append("  and with CYCLE DETECTION that logs an Error naming the variable and the element")
    w.append("  (ElementStyle.cpp:162-243).")
    w.append("- **Inherited**: an undefined name walks up the parent chain")
    w.append("  (ElementStyle.cpp:115-134), so `body { --accent: … }` themes the document.")
    w.append("- **Writable from JS** through the facade's `Element::SetProperty`, which routes")
    w.append("  `--*` names to `SetCustomProperty` (Element.cpp:590-614, the loop at :605-608) -- so a")
    w.append("  one write, not a stylesheet swap.")
    w.append("- **Works inside `@keyframes`**: keyframe properties are resolved through the same")
    w.append("  substitution path (Element.cpp:2784-2798).")
    w.append("- **Works in `transition` and `animation` timing** -- `transition` only because the")
    w.append("  vendored tree is patched for it. Resolution happens at COMPUTE time, and those two")
    w.append("  are the only properties read before that: upstream's `transition` read")
    w.append("  (ElementStyle.cpp:388) saw the unresolved string and dropped the entire declaration")
    w.append("  with no warning, no error and no log line, so a theme that put its durations in")
    w.append("  custom properties -- the obvious thing to do, and what this section recommends --")
    w.append("  simply never animated. `animation` was never affected (ComputedValues.cpp:8-16).")
    w.append("  VENDORED_TAG.txt patch #4, bead VaCuus-6gj, pinned by")
    w.append("  VaCuus.Core.Style.TransitionVariable; gotchas.md #3b.")
    w.append("")
    w.append("Unknown variable with no fallback is an Error and the declaration is dropped, so")
    w.append("`LogVaCuus: Error: [Rml] Invalid substitution, variable '--x' not defined` is the")
    w.append("line to grep for (gotchas.md #14).")
    w.append("")
    w.append("## What is NOT here")
    w.append("")
    w.append("No CSS Grid (flex-first; arch spec 1 non-goals). **No `calc()`** -- there is no")
    w.append("such parser anywhere in the vendored Core, so lengths cannot be computed and")
    w.append("`var()` above is the only indirection RCSS offers. No UA stylesheet -- link")
    w.append("`vacuus-base.rcss` first (gotchas.md #1). `transform: scale()` on a root is the")
    w.append("usual substitute for the missing `calc()`, and it DOES now clip correctly -- that")
    w.append("cost every scroll container in the document until the clip-mask stencil pass")
    w.append("landed, so check gotchas.md #8a for what it costs in memory before adopting it.")
    w.append("Selector support, `data-*` binding attributes and element tags are")
    w.append("RmlUi-documented surface, not RCSS properties, and are out of this matrix's scope.")
    w.append("")
    OUT.write_text("\n".join(w))
    print(f"wrote {OUT} ({len(props)} properties, {len(shorthands)} shorthands, "
          f"{len(decorators)} decorators, {len(filters)} filters, {len(font_effects)} font effects)")


if __name__ == "__main__":
    main()
