# Gotchas — what will surprise you, why, and what to do

Every entry is a recorded finding from building the plugin's own demos and reference
HUD — none is speculative. Format: symptom → cause (with the source that proves it) →
what to do. Perf-shaped entries cross-reference `perf-guide.md`; RCSS surface questions
go to `rcss-matrix.md`.

## Authoring: styles and layout

**1. Your first document collapses into one line.**
Cause: RmlUi ships NO user-agent stylesheet — `div` defaults to `display: inline`,
margins are 0 (`Content/DevUI/M5Hud/vacuus-base.rcss:4-8` documents this exactly).
Do: link `vacuus-base.rcss` FIRST in every document (the CLI template ships it:
`Web/packages/cli/template/vacuus-base.rcss`), then your own sheets after it.

**2. `box-shadow` does not render at all — and it is not just the transition.**
Symptom: the shadow never appears. The element itself is fine: it renders its normal
background and border, exactly as if you had not written the property. One Warning per
view names the property and the substitute.
Cause: RmlUi builds a box-shadow by rendering it into an off-screen layer and asking the
renderer to hand that layer back as a texture — `SaveLayerAsTexture`
(`Source/ThirdParty/RmlUi/Source/Core/GeometryBoxShadow.cpp:235`). VaCuus has no layer
render targets: `PushLayer`/`CompositeLayers`/`PopLayer` are recorded and then skipped at
replay, so there is no layer to capture. Implementing the capture alone would not help —
the shadow's blur is a filtered composite and its shape is cut with a clip mask, and
neither of those is applied at replay either, so it would come out as a hard-edged
rectangle painted over the element. Refusing is the honest answer for v1.
Do: use `decorator: ninepatch(...)` with a pre-blurred shadow image (the standard game-UI
substitute, and cheaper), or `font-effect: glow` for text.
**Also, separately: a box-shadow TRANSITION does nothing even where shadows render** —
RmlUi refuses the key at animation start with a Warning, not an error
(`Source/ThirdParty/RmlUi/Source/Core/ElementAnimation.cpp:640-648`), and `vacuus lint`
catches it at authoring time (`Web/packages/cli/lib/lint.mjs:68-99`).
*If you are reading an older copy of these docs that said only "not animatable": that was
wrong about the bigger half. Fixed 2026-08-05 (bead VaCuus-u0q), which also fixed the
plugin — until then a shadowed element rendered as an opaque WHITE rectangle over its own
background and border, and forced a published frame on every single tick.*

**2b. `mask-image` parses, does not mask, and paints its artwork over your element.**
Symptom: the element is unmasked, and whatever you used as the mask (a gradient, an image)
is visible on top of it. One Warning per view.
Cause: the same wall as #2, one door along. RmlUi draws the mask decorators into a pushed
layer and asks for that layer back as a filter — `SaveLayerAsMaskImage`
(`Source/ThirdParty/RmlUi/Source/Core/ElementEffects.cpp:306`). With no real layer, the
mask artwork's draws land in the frame like any other geometry, and the composite runs
with no mask filter. Unlike #2 there is nothing to make harmless short of implementing the
capture, so the warning is the whole fix for v1 (bead `VaCuus-iuv`).
Do: bake the alpha into the image asset and use `decorator: image` or `ninepatch`, or clip
with `overflow: hidden` plus `border-radius`.

**3. `transition: opacity 0.3s ease-in-out;` drops the ENTIRE declaration — there is no
`ease` family in RmlUi.**
Cause: the complete tween keyword table is eleven families —
`back bounce circular cubic elastic exponential linear quadratic quartic quintic sine`,
each with `-in`, `-out` and `-in-out`
(`Source/ThirdParty/RmlUi/Source/Core/PropertyParserAnimation.cpp:27-77`). No `ease`,
no `ease-in`, no `ease-out`, no `ease-in-out`. A token that is not in that map is not a
tween, so `ParseTransition` tries the other two shapes: the `%fs` sscanf for a duration
fails (`:267`), and the property-name branch gets null from both `GetShorthand` and
`GetProperty`, which returns false (`:301-315`). One false return discards the whole
`transition` property — not just the bad token — and RCSS reports it as a single generic
line:

```
Warning: [Rml] Syntax error parsing property declaration 'transition: opacity 0.3s ease-in-out;' in 2d6/components.rcss: 16.
```

Measured in a running engine with the two spellings side by side in one sheet:
`ease-in-out` produced that warning and no transition; `cubic-in-out` was silent and
applied.
Do: **`cubic-in-out` is the closest thing to CSS's `ease-in-out`** — that is the direct
substitution when you port a browser stylesheet. And write tween keywords **lowercase**:
`animation` lowercases each token before the keyword lookup (`:133`) and `transition`
does **not** (`:240`), so `Cubic-Out` animates fine and silently kills a transition. The
canonical property-duration-tween order still holds; the lint pass is the backstop for
`box-shadow` only, not for this.

**3b. `transition: opacity var(--t-fast) cubic-in-out;` — the other silent way to lose a
whole transition. FIXED 2026-08-06 in the vendored tree; read this if you build against
stock RmlUi or are wondering why an older build never animated.**
Symptom: identical to #3 — the element snaps — but with **no warning at all**, not even the
generic syntax-error line #3 gives you. Nothing in the log, at any level, in a debug build.
Cause: a value containing `var()` is stored **unparsed**, as
`Property{value, Unit::VAR_EXPRESSION}`
(`Source/ThirdParty/RmlUi/Source/Core/PropertySpecification.cpp:260-267`), and resolved at
**compute** time. That late resolution is exactly why `var()` works for every other
property — and `transition` is the one property read *before* it, deliberately:
`ElementStyle::TransitionPropertyChanges` takes the local property "to intercept property
changes even before the computed values are ready" (`ElementStyle.cpp:388`) and then dropped
anything that was not already a parsed transition list. A raw string is not, so the whole
declaration vanished.

It is not exotic, it is the *recommended* idiom: RCSS has no `calc()`, so `var()` is the
only theming layer `rcss-matrix.md` can point you at, and timing tokens in custom
properties are the first thing anyone does with it. In the plugin's own 2d6 demo it killed
**~30 transitions across four component batches and three screens**, through a
screenshot-reviewed component catalogue, and went unnoticed for days.

**`animation` was never affected** — its own value may contain `var()` and always could,
because it is read through `ComputedValues::animation()`, which resolves
(`ComputedValues.cpp:8-16`). So did `var()` inside `@keyframes` values. Only `transition`.

Do: **nothing, on this plugin** — the vendored tree resolves it now
(`Source/ThirdParty/RmlUi/VENDORED_TAG.txt`, patch #4; bead `VaCuus-6gj`), pinned by
`VaCuus.Core.Style.TransitionVariable`, which reports `expected opacity 1.0000, got 0.2500`
the moment that patch goes missing. On stock RmlUi, spell transition timings as literals.
Either way #3 still applies to the tween keyword, and the two traps stack: a `var()` that
substitutes to a value containing `ease-in-out` fails at #3 instead, which at least logs.

**4. `position: absolute` lands somewhere unexpected.**
Cause: it resolves against the nearest ancestor with `position: relative|absolute` —
there is no browser-style default-positioned root chain past the document
(`vacuus-base.rcss:29-31`).
Do: put `position: relative` on the container you mean to anchor to.

**5. Text renders nothing; the log repeats "No font face defined" — and your own
`@font-face` `src` resolves against the wrong directory.**
Cause: there is no default font (`vacuus-base.rcss:32-36`), and the message repeats per
layout pass. The plugin loads LatoLatin for you and nothing else. **There is no public
C++ font-loading API** — `Rml::LoadFontFace` is called only inside the plugin
(`Source/VaCuus/Private/VaCuusEngine.cpp:138-153`) — so a project shipping its own faces
has exactly one route, and it works: `@font-face` in RCSS.

**Its `src` is ROOT-relative, which is the opposite of every other path in the system.**
`<link>` and `<script src>` are document-relative (#12); `@font-face`'s `src` is passed
verbatim with no `JoinPath` (`StyleSheetParser.cpp:561`) and handed straight to the file
interface (`FontEngineDefault/FontProvider.cpp:94`), which resolves it against the
ordered document roots (`Source/VaCuus/Private/VaCuusContentPaths.cpp:92-103`). So a
sheet at `Content/DevUI/myapp/app.rcss` links its neighbours bare but must spell its
fonts from the ROOT:

```css
@font-face {
	font-family: "Michroma";       /* required, a quoted string */
	src: myapp/fonts/Michroma-Regular.ttf;   /* ROOT-relative; bare path, NO url() */
	font-weight: normal;           /* optional: all | normal | bold | <number>; default all */
	font-style: normal;            /* optional: normal | italic */
}
```

The grammar is `StyleSheetParser.cpp:294-330` (properties) and `:525-564` (the block):
`font-family` and `src` are required, `src` is a **comma-expanded list of bare paths —
`url()` is not part of it**, and `-rmlui-fallback-face` and `-rmlui-face-index` are the
two RmlUi extensions. The control run that settles the resolution rule: from a
subdirectory sheet, a bare `Michroma-Regular.ttf` gave
`Failed to open file 'Michroma-Regular.ttf' (resolved to …/Content/DevUI/Michroma-Regular.ttf)`.

**Variable fonts render at their default weight for every weight you ask for.** The
default font engine calls `FT_New_Face` and never sets a variation axis, so one variable
file cannot serve `font-weight: 400` and `700` differently — you get the default
instance twice. Ship **static instances**, one file per weight.
Do: put your faces under your project's `Content/DevUI/`, declare them with root-relative
`src`, and name the family in your sheet. The plugin's own LatoLatin ships with its OFL
license under `Content/DevUI/fonts/`.

**6. Style resolution cost jumps after adding one selector.**
Cause: a bare attribute selector (`[disabled] { … }` with no element/class anchor) is
matched against every element on every resolution — measured at ~2 ms on the research
workload (`docs/research/2026-07-29-webui-middleware.md:94`). The facade-specific
half: JS `classList` writes never write the class ATTRIBUTE, so `[class…]` selectors
lie under classList-driven state (`Web/packages/cli/lib/lint.mjs:33-57`).
Do: anchor attribute selectors (`button[disabled]`); the lint rule flags bare ones.

**7. The first frame with `font-effect: glow` takes milliseconds.**
Cause: effect-glyph generation is the measured spike class — up to 32.5 ms on large
glyph sets (research :94-95), ~4.2 ms on the reference HUD's first Record
(`docs/passport/2026-08-vacuus-perf-passport.md`, Exp-GLYPH-WARMUP).
Do: nothing lands on the game thread — the spike is UI-thread, before first publish
(arch spec §9's warm-up: the load IS the warm-up). Budget effect-heavy styles at
document load, not per frame; see perf-guide.md.

**8. A layout-thrashing document costs 5–16 ms per frame.**
Cause: measured RmlUi pathology on documents that force full relayout every frame
(research :94-95).
Do: animate `transform` and opacity (no layout), not `left/top/width`; see the blip
idiom in perf-guide.md.

**8a. FIXED — clipping under a `transform` (and under `border-radius`) works. It costs one
stencil buffer per view, and only for views that use it.**
This entry used to be a named v1 limitation: a `transform` anywhere on the clipping chain
silently disabled **all** clipping beneath it, and `overflow: hidden|auto|scroll` clipped
nothing at all. That is no longer true, and the note is kept rather than deleted because
the shape of the old failure is worth knowing if you are reading older material.

What was happening: RmlUi turns the scissor off whenever a transform is active on the
clipping chain — `if (transform) disable_scissor_clipping = true;`, unconditional
(`Source/ThirdParty/RmlUi/Source/Core/ElementUtilities.cpp:174-175`), because a transformed
element's geometry may project anywhere and a screen-space rectangle can no longer describe
it. The replacement it emits instead is a **clip mask** (`:162-169`, whenever
`has_border_radius || (transform && has_clipping_content)`), and the replayer used to skip
the two clip-mask commands. So the original clipping was switched off and the replacement
never landed.

**One correction to the old entry, because it overstated half of itself.** It used to say the
same sentence applied to `border-radius` on a clip container with no transform. It did not.
Three lines below the one everyone was citing, RmlUi says: *"If we only have border-radius then
we add this element to the scissor region as well as the clip mask… However, when we have a
transform, the element cannot be added to the scissor region"* (`:171-175`). So a **rounded**
clip container kept its scissor and still clipped rectangularly — only the four corner **arcs**
went unclipped, measured at 11.7 differing pixels on the plugin's own demo. Cosmetic, not a
screenful. Only `transform` disabled clipping outright.

The replayer now attaches a stencil target to the replay pass and honours both commands
(`Source/VaCuusRender/Private/VaCuusReplayRenderer.cpp`, `EnableClipMask` /
`RenderToClipMask`). `transform: scale()` on a root wrapper — the cheap way to author
against a fixed 1920×1080 surface here, because there is no `calc()` and `dp == px` — no
longer costs you every scroll container in the document.

**What it costs, since it is not free.** The stencil is allocated **lazily**: a view whose
document never takes the mask path never allocates one, and `stat vacuus`'s *Clip Mask
Draws* reads 0 for such a document. A view that does take it pays one depth-stencil target
at the view's extent and the view's `vacuus.ViewSampleCount`, permanently:

| `vacuus.ViewSampleCount` | stencil, per view at 1920×1080 |
|---|---|
| 1 (default) | 7.91 MiB |
| 2 | 15.82 MiB |
| 4 | 31.64 MiB |
| 8 | 63.28 MiB |

Same table as the MSAA companion target's, for the same reason: same extent, same sample
count, same bytes per sample. The two add up if you run both. See perf-guide.md.

**Two things still worth knowing.**
- A **rounded** clip container takes the mask path even with no transform at all
  (`has_border_radius ||` above), so `border-radius` on a scroll container is what most
  often turns the allocation on.
- The perf advice has not changed and was never really about clipping: for long lists,
  deleting rows still beats clipping them, because a clipped scrollback still records every
  row into the command stream ("Smaller standing DOM", perf-guide.md). Clipping now
  *works*; it was never free.

**8b. `opacity` does NOT establish a group. A child that sets its own `opacity` escapes its
ancestor's completely.**
Symptom: you fade a panel out and its contents stay. Worst case, and the one that cost real
time: a plate at `opacity: 0` with 18 icons at `opacity: 0.9` inside it photographs as
**eighteen lit glyphs floating on nothing**. It reads as a z-order or decorator bug, because
the one property you would suspect is the one you already set to zero.
Cause: in CSS, `opacity` creates a group — the subtree is composited to its own buffer and
that buffer is then faded, so a child at `opacity: 1` inside a parent at `0.5` renders at
0.5. RmlUi has no such buffer. `opacity` is a plain **inherited** property
(`Source/ThirdParty/RmlUi/Source/Core/StyleSheetSpecification.cpp:351`,
`inherited = true`) that each element multiplies into its own colours at paint time —
background and borders at `ElementBackgroundBorder.cpp:166-173`, text at
`ElementText.cpp:371-374`, every gradient and shader decorator at `DecoratorGradient.cpp:140`,
`:265`, `:434`, `:631` and `DecoratorShader.cpp:42`. Inheritance means *copy*, not
*compose*: `ComputeValues` copies the parent's inherited block wholesale
(`ComputedValues.h:394`, `inherited = parent.inherited`) and a local declaration then
**overwrites** it (`ElementStyle.cpp:1247`, `values.opacity(p->Get<float>())`). Nothing
anywhere multiplies the two together.
Do, and the right answer differs by what the child is:
- **Do not redeclare `opacity` on a descendant** if any ancestor animates or toggles its
  own. Inherit it — that is the case RmlUi gets right, and it is free.
- **Images, `<progress>`, and `image`/`tiled`/`ninepatch` decorators**: use `image-color`
  for the child's own fade. It is multiplied **by** the inherited opacity rather than
  replacing it (`Elements/ElementImage.cpp:178`, `Elements/ElementProgress.cpp:222`,
  `DecoratorTiled.cpp:80`, `DecoratorNinePatch.cpp:40` — all
  `image_color().ToPremultiplied(computed.opacity())`). This is what the 2d6 demo's fix
  used.
- **Text and solid fills**: fade with the alpha channel of `color` /
  `background-color` instead of with `opacity`, for the same reason — `ToPremultiplied`
  computes `alpha * opacity` (`Include/RmlUi/Core/Colour.h:89-98`), so an alpha byte is the
  element's own and composes with what it inherited instead of replacing it.
- If you genuinely need group semantics, give the group a **single** opacity and keep every
  descendant silent about it. There is no way to nest two.

## Data binding and JS

**9. Your data model binds to nothing, one Error at load time.**
Cause: `data-model="x"` is resolved EXACTLY ONCE, in `Element::SetParent`, when the
body is parented into the context (`Content/DevUI/m3_demo.rml:7-14` documents it with
the RmlUi cite: Element.cpp:2202-2219). A model created after document load attaches
to nothing.
Do: bind models BEFORE `LoadDocument`; the command queue being FIFO from one producer
is what makes that ordering hold across the thread boundary.

**10. Your `data-for` list renders one extra invisible row — or styling misses rows.**
Cause: the element carrying `data-for` is a hidden clone TEMPLATE, not the first row —
`DataViewFor::Initialize` sets `display: none` on it and every generated row is a
clone inserted before it (`m3_demo.rml:71-79`, citing DataViewDefault.cpp:474, :523).
Do: hang row styling off the template's own class list; never expect the template
element itself to render.

**11. Writing `{{Health}}` from JS shows literal braces.**
Cause: the brace-injection contract — a text node written through the facade renders
literally, never as a binding (M5 spec §7:298-301; the facade test proves both
directions).
Do: this is a security property, not a bug. Bindings come from the document; JS
writes are data. Route through `innerRML` only when you mean markup.

**11b. A style your JS wrote inline disappears when the component framework re-renders
that element — and a change gate that outlives the element makes it permanent.**
Cause: OBSERVED, mechanism deliberately not asserted. In the 2d6 demo a `applyScale()`
wrote `transform: scale(1.3333)` onto `#stage` and gated later writes on a `data-scale`
ATTRIBUTE it wrote beside it. On two screens the router changed screen in the same frame
and re-rendered the subtree under `#stage`: the JS log shows the transform WAS written
(`stage: view 2560x1440 -> scale(1.3333)`, frame 1), the measured tab-bar card edges are
the 1920-space ones in a 2560-wide view, and the attribute survived while the inline style
did not — so the gate then suppressed every rewrite and the document stayed unscaled in the
corner of the view for the rest of the session. Preact's `diffProps` should not touch a
`style` prop that neither vnode carries, so a definition-dirty path in RmlUi is the other
candidate; nobody has chased it, and this entry does not pretend otherwise.
Do: two rules, and the second is the load-bearing one.
(a) Put JS-owned properties on an element your component tree never renders into. The
`ElementDocument` is the natural one — `document.style` is reachable from JS and no
framework owns it.
(b) **Never gate a write on state whose lifetime differs from the thing it gates.** A
module-scope variable outlives re-renders correctly; an attribute or class on the same
element you are writing to does not. The symptom is silence — no warning, no error, one
missing property, forever — which is why it costs a session to find rather than a minute.

**12. A `<script src>` or rcss link 404s with the directory doubled.**
Cause: `src` is DOCUMENT-relative — the head handler joins the path against the
document's own URL (`Content/DevUI/M5Hud/m5_hud.rml:13-18`, citing
SystemInterface::JoinPath via XMLNodeHandlerHead). From `M5Hud/` the bare name is
correct; `M5Hud/hud_bundle.js` doubles the directory and skips the script with one
named Error.
Do: write paths relative to the document, and read the Error's resolved path when a
load is skipped. **`@font-face`'s `src` is the one exception — it is ROOT-relative
(#5).** The CLI scaffold gets this right: `vacuus create` emits the bare bundle name.

**13. There is no CSS Grid.**
Cause: RmlUi is flex-first (research :76; arch spec §1 non-goals — the same market
bar as Gameface).
Do: flex layouts; the CLI templates are flex-first. Grid is a candidate upstream
contribution, not a v1 promise.

**13a. `Atomics` does not exist on Windows — and `SharedArrayBuffer` does, so the
obvious feature test lies.**
Measured on Win64 2026-08-03: `typeof Atomics` is `"undefined"` while
`typeof SharedArrayBuffer` is `"function"`. On Linux and macOS both are present.
Cause: the vendored quickjs-ng guards its whole atomics feature on
`!__STDC_NO_ATOMICS__` (`quickjs.c:73`), and MSVC defines `__STDC_NO_ATOMICS__`
unless `/experimental:c11atomics` is passed — which VaCuus deliberately does not pass,
rather than opt a shipped module into an experimental compiler switch
(`VaCuusJs.Build.cs` carries the decision). `SharedArrayBuffer` is not behind that
guard, which is why the two come apart.
Do: **feature-detect `Atomics` itself, never `SharedArrayBuffer` as a proxy** —
`if (typeof Atomics !== 'undefined')`. The proxy test passes on Windows and then
`Atomics.load` throws a `TypeError`, so a script written that way works everywhere
you develop and fails on the platform most of your buyers ship to. If you need
cross-thread coordination, note that VaCuus already runs every document on one
process-wide UI thread, so a JS-visible atomic is rarely the tool you want.

## Engine, cook and packaging

**14. Shipping builds never assert on RmlUi contract violations — but the log still names them.**
Cause: RmlUi's asserts compile out of shipped configs; its Error/Warning log lines do
not (commit b08bd34; the routing is
`Source/VaCuus/Private/VaCuusSystemInterface.cpp` — RmlUi log → `LogVaCuus`).
Do: treat `LogVaCuus: Error: [Rml] …` in any build as the assert you didn't get.
Zero such lines is an acceptance gate the plugin's own demos hold themselves to.

**15. Restarting PIE does not give you a clean stylesheet slate.**
Cause: RmlUi's StyleSheet/Template caches are process-global and outlive PIE
(bd memory `rmlui-caches-outlive-pie-2026-07-30` — found the hard way in M2, where a
cache bug survived review because the caches expose no observable).
Do: edit-and-watch (live reload invalidates properly) or `vacuus.ReloadUI`; do not
expect a PIE restart alone to drop cached styles in the same editor process.

**16. An identifier that differs only by case works uncooked and breaks cooked.**
Cause: FName case-collision — in cooked builds the first registration wins and later
same-spelled-differently names silently take its casing (bd memory
`fname-cooked-first-registration-wins`). Bundle paths dodge this by construction:
they are normalized lowercase (`Source/VaCuus/Public/VaCuusBundle.h`,
`NormalizePath` — the one definition).
Do: treat UI paths and model names as case-insensitive-unique; never distinguish two
identities by case alone.

**17. The standalone binary exits within seconds on uncooked content — no log, exit 1.**
Cause: a non-editor target has no compiled global shader library and cannot build one
from uncooked data (bead akj.6.17, reproduced in the M6 sweep — the failure writes no
project log at all, `docs/research/m6-api-notes/p2-sweep.md` §4). This is stock UE
behavior, not a plugin defect.
Do: the supported matrix is — uncooked content → `UnrealEditor -game`; the standalone
game binary → cooked/staged builds only. Every recipe in the plugin's docs already
follows this.

**18. You edit a file in PIE, the reload fires, and the screen shows the old bytes.**
Cause: a mounted bundle shadows the loose tree — the VFS serves the PACKED copy of
anything the bundle contains, and live reload never applies to bundle-served content
(the watcher watches loose roots only). The trap is loud, not silent: the watcher
logs one Warning per shadowed file naming the bundle
(`Source/VaCuusEditor/Private/VaCuusLiveReload.cpp:472-497`).
Do: what the Warning says — `vacuus.Bundle.Enable 0` unmounts (loose files serve
again); `vacuus.Bundle.Enable 1` re-packs the tree with your edit in it.

**19. `-legacyiterative` with ZenStore off ships a stale bundle, silently.**
Cause: the legacy iterative cook cannot store `FCookDependency` data — the cooker
says so itself and falls back — so a tree edit (or worse, a deletion) does not repack
the bundle; the cooked bundle still contains the deleted file
(`docs/research/m6-api-notes/bundle-cook-experiments.md`, Exp-COOK-FILEDEP ZenStore
OFF: "Keeping 586. Recooking 0." with a deleted file still packed). This is the ONE
stale-bundle configuration found; everything else either recooks correctly
(ZenStore on, incremental — exactly the bundle package recooks on a tree edit) or
recooks everything (ZenStore off, default full cook — slow but correct).
Do: with `bUseZenStore=False`, do not pass `-legacyiterative` on a project that cooks
UI bundles. The safe default full cook is what you get without it.

**20. A silent `exit 127` when you enable a stat GROUP means a STALE module binary, not a
stat bug.**
Symptom: your editor or `-game` session exits instantly — no callstack, no crash dialog,
exit status 127 — the moment you enable a stat GROUP (`stat vacuus`, `stat slate`,
`stat scenerendering`), while `stat fps` is fine (it draws its own counter and never
master-enables collection).
Cause, and it is neither this plugin nor your machine's libc: in a
**built-from-source engine tree**, a module `.so` left over from an older build can import
symbols the current `libUnrealEditor-Core.so` no longer exports. The dynamic linker binds
lazily, so nothing fails at load — the process dies at the first *call* into that module,
and enabling a stat group is a reliable way to make that call happen, because engine
modules tick `SCOPE_CYCLE_COUNTER` scopes and a master-enable is what first sends those
messages. On the machine this was diagnosed on (2026-08-03) it was
`libUnrealEditor-XMPP.so` from four months earlier, importing `FLLMScope`'s constructor and
destructor, which today's Core exports only as `FLLMScopeDynamic`.
Do: **read stderr, not the log.** The `.log` file ends mid-line and shows nothing, but the
process prints one line to the terminal that names the culprit exactly:

```
symbol lookup error: .../libUnrealEditor-XMPP.so: undefined symbol: _ZN9FLLMScopeD1Ev
```

Compare that `.so`'s date with `libUnrealEditor-Core.so`; if it is older, delete it and
rebuild. Deleting it was enough here — the module was not part of the current target at
all, only left behind in the manifest. **An Installed (Launcher) engine cannot hit this**,
because you never rebuild Core underneath its modules; it is a from-source dev-loop
hazard, and a packaged build is immune by construction.
Do also: measure VaCuus with its own instrument regardless — `vacuus.M1HUD.PerfLog 1` (all
scopes, publish/skip ratios, per-window means and p99, in every configuration including
packaged Shipping via `-VaCuusPerfLog`). It is what the performance passport was measured
with, and it does not depend on the stats system being enabled at all.
