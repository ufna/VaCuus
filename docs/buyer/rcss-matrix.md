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
| `position` | `static` | no | yes | keyword: static, relative, absolute, fixed | absolute resolves against the nearest positioned ancestor; there is no browser-style default-positioned root chain past the document (Content/DevUI/M5Hud/vacuus-base.rcss:15-17). |
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
| `transition` | `none` | no | no | transition |  |
| `animation` | `none` | no | no | animation |  |
| `decorator` | `` (empty) | no | no | decorator | Renders through the recorder; `shader(...)` values resolve builtin names then registered UMaterial style keys (see the decorators section below). |
| `mask-image` | `` (empty) | no | no | decorator |  |
| `font-effect` | `` (empty) | yes | no | font_effect | Glyph generation for effects (glow/outline) is the measured spike class -- ~4.2 ms on the reference HUD's FIRST Record, UI thread, before first publish (perf-guide.md, Exp-GLYPH-WARMUP). Budget it at load, not per frame. |
| `filter` | `` (empty) | no | no | filter: filter | v1 compiles `blur` only; the other nine types are refused (one Warning per type, effect dropped per element -- VaCuusRecordingRenderInterface.cpp:778-801). Per-element filter blur is not a shipped v1 surface (arch spec 5, M5 amendment): the verified blur consumer is backdrop-filter. |
| `backdrop-filter` | `` (empty) | no | no | filter | `backdrop-filter: blur(...)` is the shipped glass path -- distilled at record time and re-blurred every engine frame at composite time (arch spec 5, M5 amendment). Non-blur backdrop filters are refused like element filters. |
| `box-shadow` | `none` | no | no | box_shadow | NOT animatable: RmlUi refuses the key at animation start with a Warning (ElementAnimation.cpp:640-648); `vacuus lint` flags it at authoring time (Web/packages/cli/lib/lint.mjs:68-99). |
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
effect dropped per element (VaCuusRecordingRenderInterface.cpp:778-801;
returning handle 0 is RmlUi's own safe-refusal contract, and RmlUi then warns
per element, ElementEffects.cpp:153-165). The shipped, verified blur consumer
is `backdrop-filter` (glass); per-element filter/box-shadow blur is a v1.x
item (arch spec 5, M5 amendment).

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

## At-rules

Dispatched in StyleSheetParser.cpp (`at_rule_identifier`, :786-798 in this
tree): `@keyframes`, `@decorator`, `@spritesheet`, `@media`. `@media` supports
the query grammar parsed at :590-670 (width/height/resolution/theme et al.).

## What is NOT here

No CSS Grid (flex-first; arch spec 1 non-goals). No UA stylesheet -- link
`vacuus-base.rcss` first (gotchas.md #1). Selector support, `data-*` binding
attributes and element tags are RmlUi-documented surface, not RCSS properties,
and are out of this matrix's scope.
