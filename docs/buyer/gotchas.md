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

**2. A box-shadow transition silently does nothing.**
Cause: RmlUi refuses the key at animation start — "Box shadows do not support
animations or transitions", a Warning, not an error
(`Source/ThirdParty/RmlUi/Source/Core/ElementAnimation.cpp:640-648`).
Do: animate opacity/transform on a wrapper or swap decorators; `vacuus lint` catches
it at authoring time (`Web/packages/cli/lib/lint.mjs:68-99`).

**3. A transition that works in a browser parses to nothing here.**
Cause: tween parsing is strict — `transition: opacity 0.3s ease-in-out;` works, loose
shorthand orderings browsers forgive do not (`vacuus-base.rcss:13-14`; arch spec §13's
gotcha risk row).
Do: keep the canonical property-duration-tween order; the lint pass is the backstop.

**4. `position: absolute` lands somewhere unexpected.**
Cause: it resolves against the nearest ancestor with `position: relative|absolute` —
there is no browser-style default-positioned root chain past the document
(`vacuus-base.rcss:15-17`).
Do: put `position: relative` on the container you mean to anchor to.

**5. Text renders nothing; the log repeats "No font face defined".**
Cause: there is no default font (`vacuus-base.rcss:18-19`), and the message repeats
per layout pass.
Do: register a font and name it in your base sheet (the plugin ships LatoLatin with
its OFL license, `Content/DevUI/fonts/`).

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

**12. A `<script src>` or rcss link 404s with the directory doubled.**
Cause: `src` is DOCUMENT-relative — the head handler joins the path against the
document's own URL (`Content/DevUI/M5Hud/m5_hud.rml:13-18`, citing
SystemInterface::JoinPath via XMLNodeHandlerHead). From `M5Hud/` the bare name is
correct; `M5Hud/hud_bundle.js` doubles the directory and skips the script with one
named Error.
Do: write paths relative to the document, and read the Error's resolved path when a
load is skipped.

**13. There is no CSS Grid.**
Cause: RmlUi is flex-first (research :76; arch spec §1 non-goals — the same market
bar as Gameface).
Do: flex layouts; the CLI templates are flex-first. Grid is a candidate upstream
contribution, not a v1 promise.

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

**20. Group stats can kill the EDITOR binary on bleeding-edge glibc (Linux dev loop only).**
Symptom: your editor or `-game` session exits instantly and silently — no
callstack, no crash dialog, exit status 127 — the moment you enable a stat
GROUP (`stat vacuus`, `stat slate`, `stat scenerendering`).
Scope, measured rather than assumed (each a separate 60-second run): the
modular editor binary dies on a group stat **even with VaCuus disabled**;
`stat fps` is unaffected (it draws its own counter and never master-enables
collection); and a **packaged monolithic build survives `stat vacuus`** — no
dlopen, no static-TLS pressure. So your shipped game is not affected, and
this is not a plugin defect; it is the development binary on a
rolling-release glibc (seen on Arch, glibc 2.43, 2026-08). Under gdb the
death is an ld.so TLS allocation failure on the first stat message after
master-enable; the root cause is still open at the time of writing.
Do: measure VaCuus with its own instrument — `vacuus.M1HUD.PerfLog 1` (all
scopes, publish/skip ratios, per-window means and p99, in every
configuration including packaged Shipping via `-VaCuusPerfLog`). It is what
the performance passport was measured with. If you meet the silent-127 death
on a rolling-release distro, that is the signature — and it will not follow
you into a packaged build.
