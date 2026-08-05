# Supported RCSS matrix -- generated, keyed to the vendored RmlUi

**Vendored RmlUi SHA:** `0ae381e00d7426762bb5ed897973366358b16642` (`Source/ThirdParty/RmlUi/VENDORED_SHA.txt`)

**GENERATED FILE -- do not edit the tables by hand.** Regenerate after any RmlUi
bump with `python3 Tools/gen_rcss_matrix.py` (run from the plugin root). The
property and shorthand tables are parsed mechanically from
`StyleSheetSpecification::RegisterDefaultProperties` (StyleSheetSpecification.cpp:248-438
in this vendored tree); the decorator/filter/font-effect names from the
`Factory::RegisterDefault*Instancers` registrations in Factory.cpp. The Notes
column is hand-maintained inside the generator script (plugin-side truth with
source cites) and survives regeneration.

The value grammar per property is its parser chain: `keyword: a, b` accepts
those keywords; `length`, `length_percent`, `number`, `color`, `string` etc.
are the RCSS value parsers registered in RegisterDefaultParsers (same file).
A property with multiple parsers accepts any of them, tried in order.

## Properties (99)

| Property | Default | Inherited | Affects layout | Accepted values | Notes |
|---|---|---|---|---|---|
| `margin-top` | `0px` | no | yes | keyword: auto; length_percent |  |
| `margin-right` | `0px` | no | yes | keyword: auto; length_percent |  |
| `margin-bottom` | `0px` | no | yes | keyword: auto; length_percent |  |
| `margin-left` | `0px` | no | yes | keyword: auto; length_percent |  |
| `padding-top` | `0px` | no | yes | length_percent |  |
| `padding-right` | `0px` | no | yes | length_percent |  |
| `padding-bottom` | `0px` | no | yes | length_percent |  |
| `padding-left` | `0px` | no | yes | length_percent |  |
| `border-top-width` | `0px` | no | yes | length |  |
| `border-right-width` | `0px` | no | yes | length |  |
| `border-bottom-width` | `0px` | no | yes | length |  |
| `border-left-width` | `0px` | no | yes | length |  |
| `border-top-color` | `black` | no | no | color |  |
| `border-right-color` | `black` | no | no | color |  |
| `border-bottom-color` | `black` | no | no | color |  |
| `border-left-color` | `black` | no | no | color |  |
| `border-top-left-radius` | `0px` | no | no | length |  |
| `border-top-right-radius` | `0px` | no | no | length |  |
| `border-bottom-right-radius` | `0px` | no | no | length |  |
| `border-bottom-left-radius` | `0px` | no | no | length |  |
| `display` | `inline` | no | yes | keyword: none, block, inline, inline-block, flow-root, flex, inline-flex, table, inline-table, table-row, table-row-group, table-column, table-column-group, table-cell | No CSS Grid in RmlUi (flex-first is the market bar, arch spec 1); the keyword list here is exhaustive. |
| `position` | `static` | no | yes | keyword: static, relative, absolute, fixed | absolute resolves against the nearest positioned ancestor; there is no browser-style default-positioned root chain past the document (Content/DevUI/M5Hud/vacuus-base.rcss:29-31). |
| `top` | `auto` | no | no | keyword: auto; length_percent |  |
| `right` | `auto` | no | no | keyword: auto; length_percent |  |
| `bottom` | `auto` | no | no | keyword: auto; length_percent |  |
| `left` | `auto` | no | no | keyword: auto; length_percent |  |
| `float` | `none` | no | yes | keyword: none, left, right |  |
| `clear` | `none` | no | yes | keyword: none, left, right, both |  |
| `box-sizing` | `content-box` | no | yes | keyword: content-box, border-box |  |
| `z-index` | `auto` | no | no | keyword: auto; number |  |
| `width` | `auto` | no | yes | keyword: auto; length_percent |  |
| `min-width` | `0px` | no | yes | length_percent |  |
| `max-width` | `none` | no | yes | keyword: none; length_percent |  |
| `height` | `auto` | no | yes | keyword: auto; length_percent |  |
| `min-height` | `0px` | no | yes | length_percent |  |
| `max-height` | `none` | no | yes | keyword: none; length_percent |  |
| `line-height` | `1.2` | yes | yes | number_length_percent |  |
| `vertical-align` | `baseline` | no | yes | keyword: baseline, middle, sub, super, text-top, text-bottom, top, center, bottom; length_percent |  |
| `overflow-x` | `visible` | no | yes | keyword: visible, hidden, auto, scroll |  |
| `overflow-y` | `visible` | no | yes | keyword: visible, hidden, auto, scroll |  |
| `clip` | `auto` | no | no | keyword: auto, none, always; number |  |
| `visibility` | `visible` | no | no | keyword: visible, hidden |  |
| `text-overflow` | `clip` | no | no | keyword: clip, ellipsis; string |  |
| `background-color` | `transparent` | no | no | color |  |
| `color` | `white` | yes | no | color |  |
| `caret-color` | `auto` | yes | no | keyword: auto; color |  |
| `image-color` | `white` | no | no | color |  |
| `opacity` | `1` | yes | no | number |  |
| `font-family` | `` (empty) | yes | yes | string |  |
| `font-style` | `normal` | yes | yes | keyword: normal, italic |  |
| `font-weight` | `normal` | yes | yes | keyword: normal=400, bold=700; number |  |
| `font-size` | `12px` | yes | yes | length; length_percent |  |
| `font-kerning` | `auto` | yes | yes | keyword: auto, normal, none |  |
| `letter-spacing` | `normal` | yes | yes | keyword: normal; length |  |
| `text-align` | `left` | yes | yes | keyword: left, right, center, justify |  |
| `text-decoration` | `none` | yes | no | keyword: none, underline, overline, line-through |  |
| `text-transform` | `none` | yes | yes | keyword: none, capitalize, uppercase, lowercase |  |
| `white-space` | `normal` | yes | yes | keyword: normal, pre, nowrap, pre-wrap, pre-line |  |
| `word-break` | `normal` | yes | yes | keyword: normal, break-all, break-word |  |
| `row-gap` | `0px` | no | yes | length_percent |  |
| `column-gap` | `0px` | no | yes | length_percent |  |
| `cursor` | `` (empty) | yes | no | string |  |
| `drag` | `none` | no | no | keyword: none, drag, drag-drop, block, clone |  |
| `tab-index` | `none` | no | no | keyword: none, auto |  |
| `focus` | `auto` | yes | no | keyword: none, auto |  |
| `nav-up` | `none` | no | no | keyword: none, auto, horizontal, vertical, tree-order; string | The nav-* family is NOT inherited: spatial-nav rules must sit on the focusable element itself, and targets use `#id` syntax (rmlui-input.md:190-207, :450). |
| `nav-right` | `none` | no | no | keyword: none, auto, horizontal, vertical, tree-order; string | See nav-up. |
| `nav-down` | `none` | no | no | keyword: none, auto, horizontal, vertical, tree-order; string | See nav-up. |
| `nav-left` | `none` | no | no | keyword: none, auto, horizontal, vertical, tree-order; string | See nav-up. |
| `scrollbar-margin` | `0` | no | no | length |  |
| `overscroll-behavior` | `auto` | no | no | keyword: auto, contain |  |
| `pointer-events` | `auto` | yes | no | keyword: none, auto | Inherited, but there is no subtree pruning -- `none` on a parent still walks children during hit tests (docs/research/m2-api-notes/rmlui-input.md:198). |
| `perspective` | `none` | no | no | keyword: none; length |  |
| `perspective-origin-x` | `50%` | no | no | keyword: left, center, right; length_percent |  |
| `perspective-origin-y` | `50%` | no | no | keyword: top, center, bottom; length_percent |  |
| `transform` | `none` | no | no | transform |  |
| `transform-origin-x` | `50%` | no | no | keyword: left, center, right; length_percent |  |
| `transform-origin-y` | `50%` | no | no | keyword: top, center, bottom; length_percent |  |
| `transform-origin-z` | `0` | no | no | length |  |
| `transition` | `none` | no | no | transition | THERE IS NO `ease` FAMILY. The complete tween table is eleven families -- back bounce circular cubic elastic exponential linear quadratic quartic quintic sine -- each with -in/-out/-in-out (PropertyParserAnimation.cpp:27-77). `ease-in-out` is not in it, and an unrecognized token drops the WHOLE declaration with one generic "Syntax error parsing property declaration" (:240 misses the map, :267 fails the duration sscanf, :301-315 returns false). Use `cubic-in-out` for CSS's `ease-in-out`. Keywords must be LOWERCASE here: `transition` does not lowercase its token (:240) while `animation` does (:133). gotchas.md #3. |
| `animation` | `none` | no | no | animation | Same tween table as `transition` (PropertyParserAnimation.cpp:27-77), but this parser LOWERCASES each token before the lookup (:133), so `Cubic-Out` works here and silently kills a `transition`. Keyframe values may contain `var()` (Element.cpp:2784-2798). |
| `decorator` | `` (empty) | no | no | decorator | Renders through the recorder; `shader(...)` values resolve builtin names then registered UMaterial style keys (see the decorators section below). |
| `mask-image` | `` (empty) | no | no | decorator | PARSES BUT DOES NOT MASK in v1 -- masking needs the mask layer captured as a filter (ElementEffects.cpp:306 -> RenderInterface::SaveLayerAsMaskImage) and the replayer has no layer render targets. It is REFUSED, once per view, with a Warning (bead VaCuus-iuv). The element renders UNMASKED **and the mask artwork is drawn over it**, because the layer the decorators were drawn into is not a real render target -- verified on screen, not inferred. Substitute: bake the alpha into the image asset and use `decorator: image`/`ninepatch`, or clip with `overflow: hidden` plus `border-radius`. |
| `font-effect` | `` (empty) | yes | no | font_effect | Glyph generation for effects (glow/outline) is the measured spike class -- ~4.2 ms on the reference HUD's FIRST Record, UI thread, before first publish (perf-guide.md, Exp-GLYPH-WARMUP). Budget it at load, not per frame. |
| `filter` | `` (empty) | no | no | filter: filter | v1 compiles `blur` only; the other nine types are refused (one Warning per type, effect dropped per element -- VaCuusRecordingRenderInterface.cpp:866-907). Per-element filter blur is not a shipped v1 surface (arch spec 5, M5 amendment): the verified blur consumer is backdrop-filter. |
| `backdrop-filter` | `` (empty) | no | no | filter | `backdrop-filter: blur(...)` is the shipped glass path -- distilled at record time and re-blurred every engine frame at composite time (arch spec 5, M5 amendment). Non-blur backdrop filters are refused like element filters. |
| `box-shadow` | `none` | no | no | box_shadow | DOES NOT RENDER in v1 -- the shadow needs the current layer captured to a texture (GeometryBoxShadow.cpp:235 -> RenderInterface::SaveLayerAsTexture) and the replayer has no layer render targets. It is REFUSED, once per view, with a Warning naming the property and the substitute; the element then renders its normal background and border with the shadow dropped (bead VaCuus-u0q; VaCuus patch #3 to the vendored RmlUi is what makes the failure harmless -- before it the element rendered as an opaque WHITE rectangle and republished every frame). Substitute: `decorator: ninepatch(...)` with a pre-blurred shadow image, or `font-effect: glow` for text. Also NOT animatable even where it renders: RmlUi refuses the key at animation start with a Warning (ElementAnimation.cpp:640-648); `vacuus lint` flags it at authoring time (Web/packages/cli/lib/lint.mjs:68-99). |
| `fill-image` | `` (empty) | no | no | string |  |
| `align-content` | `stretch` | no | yes | keyword: flex-start, flex-end, center, space-between, space-around, space-evenly, stretch |  |
| `align-items` | `stretch` | no | yes | keyword: flex-start, flex-end, center, baseline, stretch |  |
| `align-self` | `auto` | no | yes | keyword: auto, flex-start, flex-end, center, baseline, stretch |  |
| `flex-basis` | `auto` | no | yes | keyword: auto; length_percent |  |
| `flex-direction` | `row` | no | yes | keyword: row, row-reverse, column, column-reverse |  |
| `flex-grow` | `0` | no | yes | number |  |
| `flex-shrink` | `1` | no | yes | number |  |
| `flex-wrap` | `nowrap` | no | yes | keyword: nowrap, wrap, wrap-reverse |  |
| `justify-content` | `flex-start` | no | yes | keyword: flex-start, flex-end, center, space-between, space-around, space-evenly |  |
| `-rmlui-language` | `` (empty) | yes | yes | string |  |
| `-rmlui-direction` | `auto` | yes | yes | keyword: auto, ltr, rtl |  |

## Shorthands (20)

| Shorthand | Expands to | Expansion type | Notes |
|---|---|---|---|
| `margin` | margin-top, margin-right, margin-bottom, margin-left | Box |  |
| `padding` | padding-top, padding-right, padding-bottom, padding-left | Box |  |
| `border-width` | border-top-width, border-right-width, border-bottom-width, border-left-width | Box |  |
| `border-color` | border-top-color, border-right-color, border-bottom-color, border-left-color | Box |  |
| `border-top` | border-top-width, border-top-color | FallThrough |  |
| `border-right` | border-right-width, border-right-color | FallThrough |  |
| `border-bottom` | border-bottom-width, border-bottom-color | FallThrough |  |
| `border-left` | border-left-width, border-left-color | FallThrough |  |
| `border` | border-top, border-right, border-bottom, border-left | RecursiveRepeat |  |
| `border-radius` | border-top-left-radius, border-top-right-radius, border-bottom-right-radius, border-bottom-left-radius | Box |  |
| `inset` | top, right, bottom, left | Box |  |
| `overflow` | overflow-x, overflow-y | Replicate |  |
| `background` | background-color | FallThrough |  |
| `font` | font-style, font-weight, font-size, font-family | FallThrough |  |
| `gap` | row-gap, column-gap | Replicate |  |
| `nav` | nav-up, nav-right, nav-down, nav-left | Box | Shorthand for the nav-* family; the non-inheritance note on nav-up applies. |
| `perspective-origin` | perspective-origin-x, perspective-origin-y | FallThrough |  |
| `transform-origin` | transform-origin-x, transform-origin-y, transform-origin-z | FallThrough |  |
| `flex` | flex-grow, flex-shrink, flex-basis | Flex |  |
| `flex-flow` | flex-direction, flex-wrap | FallThrough |  |

## Decorators (16 registered names)

Registered in Factory.cpp (line numbers in this vendored tree). All render
through the record/replay path. `shader(<name-or-key>)` resolves builtin
names first (`glass-panel` ships), then UMaterial style keys registered by the
game (`UVaCuusStyleSet` -- the M5 material-decorator tier; unknown keys are
refused once with a Warning naming both halves of what would have worked).

The six gradient decorators are **antialiased in screen space**, which is a
deliberate deviation from RmlUi's reference backend. A hard colour break --
two stops at the same position, which is how a segmented dial ring or a
hazard hatch is written -- is rendered as a one-pixel ramp instead of a step,
so a diagonal break does not staircase. The width comes from the analytic
screen-space derivative of the gradient parameter, so it is one pixel at any
size, scale or transform; a stop pair that is already more than a pixel apart
is left bit-for-bit alone, and a smooth gradient is unchanged. A repeating
gradient gets the same treatment at its period edge as at its interior stops,
and once its bands shrink past about two pixels it fades to the period's mean
colour rather than aliasing (VaCuusGradient.usf).

This covers the gradient FILL only. Element outlines -- notably `border-radius`
arcs -- are tessellated geometry drawn into a single-sampled render target and
are not antialiased.

| Decorator | Factory.cpp line |
|---|---|
| `text` | :196 |
| `tiled-horizontal` | :197 |
| `tiled-vertical` | :198 |
| `tiled-box` | :199 |
| `image` | :200 |
| `ninepatch` | :201 |
| `shader` | :202 |
| `gradient` | :204 |
| `horizontal-gradient` | :205 |
| `vertical-gradient` | :206 |
| `linear-gradient` | :208 |
| `repeating-linear-gradient` | :209 |
| `radial-gradient` | :210 |
| `repeating-radial-gradient` | :211 |
| `conic-gradient` | :212 |
| `repeating-conic-gradient` | :213 |

## Filters (10 registered names) -- blur-only in v1

RmlUi registers ten filter instancers; the VaCuus recorder compiles exactly
ONE -- `blur`. Every other type is refused with one Warning per type and the
effect dropped per element (VaCuusRecordingRenderInterface.cpp:866-907;
returning handle 0 is RmlUi's own safe-refusal contract, and RmlUi then warns
per element, ElementEffects.cpp:161-165). The shipped, verified blur consumer
is `backdrop-filter` (glass); per-element `filter:` blur is a v1.x item (arch
spec 5, M5 amendment).

`box-shadow`'s blur never reaches this table at all: the shadow is refused a
step earlier, at the layer capture it is built on (see the `box-shadow` row
above and `Layer capture` below), so its CompileFilter("blur") call is never
made.

| Filter | Factory.cpp line | v1 status |
|---|---|---|
| `hue-rotate` | :216 | refused (one Warning per type; effect dropped per element) |
| `brightness` | :217 | refused (one Warning per type; effect dropped per element) |
| `contrast` | :218 | refused (one Warning per type; effect dropped per element) |
| `grayscale` | :219 | refused (one Warning per type; effect dropped per element) |
| `invert` | :220 | refused (one Warning per type; effect dropped per element) |
| `opacity` | :221 | refused (one Warning per type; effect dropped per element) |
| `saturate` | :222 | refused (one Warning per type; effect dropped per element) |
| `sepia` | :223 | refused (one Warning per type; effect dropped per element) |
| `blur` | :225 | compiled; shipped consumer is `backdrop-filter` (glass) |
| `drop-shadow` | :226 | refused (one Warning per type; effect dropped per element) |

## Font effects (4 registered names)

All four render. Effect-glyph generation is the measured load-spike class:
~4.2 ms on the reference HUD's first Record (UI thread, before first publish,
never the game thread) -- see perf-guide.md, Exp-GLYPH-WARMUP, before shipping
effect-heavy styles.

| Font effect | Factory.cpp line |
|---|---|
| `blur` | :229 |
| `glow` | :230 |
| `outline` | :231 |
| `shadow` | :232 |

## Layer capture -- the two properties v1 refuses outright

Rml::RenderInterface has 21 virtuals and the VaCuus recorder now overrides all
21, but two of them are REFUSALS rather than recordings -- `SaveLayerAsTexture`
and `SaveLayerAsMaskImage` (RenderInterface.h:112-116). Both mean "hand me the
current layer back", and this renderer has no layer to hand back: PushLayer,
CompositeLayers and PopLayer are recorded and then skipped at replay, so every
draw between a push and a pop lands directly in the base render target. Glass
(`backdrop-filter`) does not need them -- it is distilled from the buffer and
composited per engine frame -- which is why it ships and these do not.

Each is refused with **one Warning per view**, latched, naming the property and
its substitute. A document with two hundred shadowed elements logs one line.

| Property | Reaches | v1 behaviour |
|---|---|---|
| `box-shadow` | `SaveLayerAsTexture` | Shadow dropped; the element renders its **normal background and border**. No per-frame cost. |
| `mask-image` | `SaveLayerAsMaskImage` | Element renders **unmasked**, and the mask artwork is **drawn over it**. |

Both are pinned by automation: `VaCuus.Render.LayerCapture.Refused` and
`VaCuus.Render.LayerCapture.RestyleChurn`.

## At-rules

Dispatched in StyleSheetParser.cpp on `at_rule_identifier` (:783-869 in this
tree): **`@keyframes`** (:786), **`@decorator`** (:790), **`@spritesheet`**
(:798), **`@media`** (:835), **`@font-face`** (:855). Anything else logs
"Invalid at-rule identifier" (:869). `@media` supports the query grammar
parsed by ParseMediaFeatureMap (:567-684 -- width/height/resolution/theme et al.).

### `@font-face` -- and its `src` is ROOT-relative, unlike every other path

There is NO public C++ font-loading API (`Rml::LoadFontFace` is called only
inside the plugin, VaCuusEngine.cpp:138-153), so `@font-face` is the ONE route
for a project shipping its own faces. It works.

```css
@font-face {
	font-family: "Michroma";                  /* REQUIRED, a quoted string */
	src: myapp/fonts/Michroma-Regular.ttf;    /* REQUIRED. ROOT-relative. Bare path, NO url() */
	font-weight: normal;                     /* all | normal | bold | <number>; default `all` */
	font-style: normal;                      /* normal | italic; default normal */
	-rmlui-fallback-face: false;             /* RmlUi extension; default false */
	-rmlui-face-index: 0;                    /* RmlUi extension, for collections; default 0 */
}
```

Grammar at StyleSheetParser.cpp:294-330 (the property set) and :525-564 (the
block: both required properties checked, then one LoadFontFace per src). `src`
is COMMA-EXPANDED into a list of bare paths -- `url()` is not part of this
grammar at all.

**ROOT-relative, not document-relative.** `src` is passed verbatim with no
`JoinPath` (:561), handed straight to the file interface
(FontEngineDefault/FontProvider.cpp:94) and resolved against the ordered document
roots (Source/VaCuus/Private/VaCuusContentPaths.cpp:92-103). `<link>` and
`<script src>` are the opposite -- document-relative (gotchas.md #12) -- so a
sheet at `Content/DevUI/myapp/app.rcss` links its neighbours bare but must spell
its fonts `myapp/fonts/Face.ttf`.

**Variable fonts render at their default weight for every requested weight**: the
default font engine calls `FT_New_Face` without setting a variation axis. Ship
static instances, one file per weight.

## Custom properties and `var()` -- the only theming layer

Fully implemented in this vendored tree, and worth knowing because **there is no
`calc()`** (see below): `var()` is the whole of RCSS's computed-value story.

```css
body { --accent: #38BDF8; --pad: 12px; }
#panel { color: var(--accent); padding: var(--pad); }
#panel.alt { color: var(--missing, #F87171); }   /* fallback after the comma */
```

- **Declared** like any property whose name starts with `--`; the value is stored
  unparsed as `Unit::STRING`, or as `Unit::VAR_EXPRESSION` when it itself contains
  a `var()` (PropertySpecification.cpp:233-248). A normal property whose value
  contains `var()` is likewise stored as VAR_EXPRESSION and resolved at compute
  time, not parse time (:260-300).
- **Substituted** with fallbacks (`var(--x, <fallback>)`, nested parens counted)
  and with CYCLE DETECTION that logs an Error naming the variable and the element
  (ElementStyle.cpp:162-243).
- **Inherited**: an undefined name walks up the parent chain
  (ElementStyle.cpp:115-134), so `body { --accent: … }` themes the document.
- **Writable from JS** through the facade's `Element::SetProperty`, which routes
  `--*` names to `SetCustomProperty` (Element.cpp:590-614, the loop at :605-608) -- so a
  one write, not a stylesheet swap.
- **Works inside `@keyframes`**: keyframe properties are resolved through the same
  substitution path (Element.cpp:2784-2798).

Unknown variable with no fallback is an Error and the declaration is dropped, so
`LogVaCuus: Error: [Rml] Invalid substitution, variable '--x' not defined` is the
line to grep for (gotchas.md #14).

## What is NOT here

No CSS Grid (flex-first; arch spec 1 non-goals). **No `calc()`** -- there is no
such parser anywhere in the vendored Core, so lengths cannot be computed and
`var()` above is the only indirection RCSS offers. No UA stylesheet -- link
`vacuus-base.rcss` first (gotchas.md #1). Note also that `transform` on a clipping
chain silently disables clipping in v1 (gotchas.md #8a) -- relevant because
`transform: scale()` on a root is the usual substitute for the missing `calc()`.
Selector support, `data-*` binding attributes and element tags are
RmlUi-documented surface, not RCSS properties, and are out of this matrix's scope.
