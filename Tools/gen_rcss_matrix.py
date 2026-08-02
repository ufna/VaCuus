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

What is hand-maintained: the ANNOTATIONS dict below (plugin-side truth --
what VaCuus refuses or treats specially). Each annotation cites its source;
keep the cites open-and-checked when editing (project convention).

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
    "effect dropped per element -- VaCuusRecordingRenderInterface.cpp:778-801). Per-element "
    "filter blur is not a shipped v1 surface (arch spec 5, M5 amendment): the verified blur "
    "consumer is backdrop-filter.",
    "backdrop-filter": "`backdrop-filter: blur(...)` is the shipped glass path -- distilled at record "
    "time and re-blurred every engine frame at composite time (arch spec 5, M5 amendment). "
    "Non-blur backdrop filters are refused like element filters.",
    "box-shadow": "NOT animatable: RmlUi refuses the key at animation start with a Warning "
    "(ElementAnimation.cpp:640-648); `vacuus lint` flags it at authoring time "
    "(Web/packages/cli/lib/lint.mjs:68-99).",
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
    "display": "No CSS Grid in RmlUi (flex-first is the market bar, arch spec 1); the keyword "
    "list here is exhaustive.",
    "position": "absolute resolves against the nearest positioned ancestor; there is no "
    "browser-style default-positioned root chain past the document "
    "(Content/DevUI/M5Hud/vacuus-base.rcss:15-17).",
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
    w.append("| Decorator | Factory.cpp line |")
    w.append("|---|---|")
    for name, line in decorators:
        w.append(f"| `{name}` | :{line} |")
    w.append("")
    w.append(f"## Filters ({len(filters)} registered names) -- blur-only in v1")
    w.append("")
    w.append("RmlUi registers ten filter instancers; the VaCuus recorder compiles exactly")
    w.append("ONE -- `blur`. Every other type is refused with one Warning per type and the")
    w.append("effect dropped per element (VaCuusRecordingRenderInterface.cpp:778-801;")
    w.append("returning handle 0 is RmlUi's own safe-refusal contract, and RmlUi then warns")
    w.append("per element, ElementEffects.cpp:153-165). The shipped, verified blur consumer")
    w.append("is `backdrop-filter` (glass); per-element filter/box-shadow blur is a v1.x")
    w.append("item (arch spec 5, M5 amendment).")
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
    w.append("## At-rules")
    w.append("")
    w.append("Dispatched in StyleSheetParser.cpp (`at_rule_identifier`, :786-798 in this")
    w.append("tree): `@keyframes`, `@decorator`, `@spritesheet`, `@media`. `@media` supports")
    w.append("the query grammar parsed at :590-670 (width/height/resolution/theme et al.).")
    w.append("")
    w.append("## What is NOT here")
    w.append("")
    w.append("No CSS Grid (flex-first; arch spec 1 non-goals). No UA stylesheet -- link")
    w.append("`vacuus-base.rcss` first (gotchas.md #1). Selector support, `data-*` binding")
    w.append("attributes and element tags are RmlUi-documented surface, not RCSS properties,")
    w.append("and are out of this matrix's scope.")
    w.append("")
    OUT.write_text("\n".join(w))
    print(f"wrote {OUT} ({len(props)} properties, {len(shorthands)} shorthands, "
          f"{len(decorators)} decorators, {len(filters)} filters, {len(font_effects)} font effects)")


if __name__ == "__main__":
    main()
