# TASK A — @vacuus/preact contract: Preact's DOM needs vs the M4 facade

## 1. The facade surface as it exists today (exhaustive, from the code)

### Element prototype (both wrapper classes; the document prototype chains to it — VaCuusJsDom.cpp:349-356)

Installed in `InstallDomPrototypes`, VaCuusJsDom.cpp:313-335:

| Member | Kind | Backing | Notes |
|---|---|---|---|
| `appendChild(child)` | method | `Element::AppendChild` via ElementPtr recovery | returns the same child wrapper (VaCuusJsDom.cpp:565); cycle guard returns null instead of throwing (:519-525); cross-context child refused as null (:507-512) |
| `insertBefore(child, ref)` | method | `Element::InsertBefore` | **null/dead/absent ref appends** — RmlUi's own fallback, matches DOM for null ref; a non-child ref appends where DOM throws NotFoundError (VaCuusJsDom.cpp:548-556; Element.cpp:1372-1418) |
| `removeChild(child)` | method | `Element::RemoveChild`, ElementPtr adopted into child's wrapper | child stays alive detached, identity preserved (VaCuusJsDom.cpp:568-601) |
| `remove()` | method | reset Owned / `Parent->RemoveChild` discard | destroys the subtree (VaCuusJsDom.cpp:603-635) |
| `querySelector`, `querySelectorAll`, `closest` | methods | RmlUi equivalents | documented deviations: self never matches; closest starts at parent (VaCuusJsDom.cpp:670-684) |
| `getAttribute`/`setAttribute`/`removeAttribute` | methods | Variant map + synchronous `OnAttributeChange` (VaCuusJsDom.cpp:735-764; Element.inl:15-23) | `setAttribute("class", …)` takes effect immediately via `meta->style.SetClassNames` (Element.cpp:1713-1714) |
| `id` | get/set | `GetId`/`SetId` (VaCuusJsDom.cpp:793-794, :834-836) | |
| `tagName` | get | `GetTagName()` verbatim = **lowercase** (VaCuusJsDom.cpp:796-800) | deviation from DOM's uppercase; equals DOM `localName` semantics |
| `innerRML` | get/set | `GetInnerRML`/`SetInnerRML` | set destroys all children synchronously; their wrappers go dead before the setter returns (VaCuusJsDom.cpp:838-845; Element.cpp:1167) |
| `parentNode` | get | `GetParentNode`, identity-cached wrap (VaCuusJsDom.cpp:854-872) | |
| `children` | get | fresh snapshot array per call, DOM children only, **`#text` children filtered out** (VaCuusJsDom.cpp:874-915, filter at :902) | |
| `classList` | get | fresh 4-method object per access: `add/remove/toggle/contains` over `SetClass`/`IsClassSet` — **never the class attribute** (VaCuusJsDom.cpp:917-1010); one token per call, no variadic (:986-987) | `el.classList !== el.classList` (:921-923) |
| `style` | get | fresh JS Proxy per access; get→`GetProperty(name)->ToString()`, set→`SetProperty(name, value)`, delete/`removeProperty`→`RemoveProperty` (VaCuusJsDom.cpp:104-134, :1046-1125) | name passed **verbatim** to RmlUi — no camelCase mapping |
| `addEventListener(type, fn, capture?/options)` | method | registry-owned `FVaCuusJsEventListener`, real capture flag; options object honored for `capture` only (VaCuusJsEvents.cpp:684-743, :89-109); DOM duplicate rule (:729-732); non-function listener is a TypeError (:700-704) | |
| `removeEventListener` | method | exact (element, type, fnPtr, capture) match or no-op (VaCuusJsEvents.cpp:745-797) | |
| `dispatchEvent(type, params?)` | method | `Element::DispatchEvent`; params object → Dictionary, bool/number/string only (VaCuusJsEvents.cpp:799-866) | |

### Document prototype (VaCuusJsDom.cpp:337-341)

| Member | Notes |
|---|---|
| `createElement(tag)` | tag **lowercased** before `Factory::InstanceElement(nullptr, tag, tag, {})`; fresh node detached, wrapper owns the ElementPtr (VaCuusJsDom.cpp:378-430) |
| `getElementById(id)` | whole-tree semantics (VaCuusJsDom.cpp:432-458) |
| `body` (get) | **`document.body === document`** — RmlUi's ElementDocument *is* the body (VaCuusJsDom.cpp:460-479) |

### Event object (per dispatch, VaCuusJsEvents.cpp:180-287)

Plain data props `type`, `target`, `currentTarget` (both through the identity cache, so `ev.target === document.getElementById(...)` holds — :192-210), `eventPhase` (DOM numbering 1/2/3, :212-231), `params` (all convertible RmlUi parameters, :235-245), DOM-ish aliases `mouseX/mouseY/button/keyIdentifier/text/ctrlKey/shiftKey/altKey/metaKey` (:258-284). Methods: `stopPropagation`, `stopImmediatePropagation`, `preventDefault` — **preventDefault maps to StopPropagation**, RmlUi has no separate default-action veto (:321-330). Inside a handler `this` is the currentTarget (:636).

### Globals (VaCuusJsViewContext.cpp:159-222; VaCuusJsHostApi.cpp:91-109)

`document` (null until bound — :175), `console.log/info/warn/error`, `setTimeout`/`setInterval`/`clearTimeout`/`clearInterval` (no extra-arg forwarding — :528-530), `requestAnimationFrame`/`cancelAnimationFrame` (:216-219), `vacuus` {`onUnload`, `log`, `emit`, `model`, `stats`, `view` getter}. `Promise` and `queueMicrotask` are QuickJS intrinsics, deliberately not shadowed (VaCuusJsViewContext.cpp:29-34). **There is no `window`.**

### Not present anywhere

`createTextNode`, `nodeType`, `nodeValue`/`data`, `textContent`, `childNodes`, `firstChild`/`lastChild`, `nextSibling`/`previousSibling`, `replaceChild`, `contains`, `ownerDocument`, `className`, `localName`/`nodeName`, `innerHTML`, `cloneNode`, any `on*` properties on wrappers.

## 2. The undom-class minimal-DOM contract

[inference: reconstructed from undom's published API shape as the arch spec's named precedent — "undom contract; precedent: OneJS/Unity" (2026-07-29-vacuus-architecture-design.md:261-263) and "JS facade conformance (subset of undom tests)" (:349). To be verified against the vendored preact when the CLI lands.]

What Preact's renderer path touches on a minimal DOM: `document.createElement(type)`, `document.createTextNode(text)`; node ops `appendChild`, `insertBefore(node, refOrNull)`, `removeChild`, `remove()`; traversal `parentNode`, `childNodes`, `firstChild`, `nextSibling`; discrimination `nodeType` (1/3/9) and `localName`-vs-vnode-type comparison; `setAttribute`/`removeAttribute`; `addEventListener`/`removeEventListener` with one persistent proxy per (node, type) plus a `_listeners` **expando on the node**; text updates via `node.data = value` (nodeValue alias); a writable `style` object; `dangerouslySetInnerHTML` → `innerHTML`. Scheduling: `Promise.resolve().then` (debounceRendering default), `requestAnimationFrame` + `setTimeout` fallback for effect flushing, `options.requestAnimationFrame`/`options.debounceRendering` as override points.

Expandos work today: wrappers are ordinary `JS_NewObjectClass` objects; Preact's `_listeners`/`__k` root property on `document` land as plain JS properties, and the identity cache (VaCuusJsDom.cpp:195-200) is exactly what keeps them attached to the "same node" across lookups.

## 3. GAP TABLE

| # | Preact need [inference unless cited] | Facade today | Effort | Notes |
|---|---|---|---|---|
| G1 | `document.createTextNode` + text-node `data`/`nodeValue` setter | **absent**; `children` even filters `#text` (VaCuusJsDom.cpp:902) | **moderate** | Fully buildable — see §4. Same ownership matrix as createElement; no new lifetime semantics. |
| G2 | `nextSibling` (insertion anchors for keyed reorder), `firstChild` (render's initial oldDom), `previousSibling`/`lastChild` | absent | **trivial** | RmlUi has all four (Element.h:419-429); they include `#text` children and exclude only non-DOM extras (Element.cpp:1095-1141) — wrap and go. |
| G3 | `childNodes` (excess-children scan on first render into a non-empty parent) | absent (`children` exists but filters `#text`) | **trivial** | Same snapshot-array pattern as `children` (VaCuusJsDom.cpp:874-915) minus the filter. Keep `children` filtered — that IS the DOM contract for `children`. |
| G4 | `nodeType` (1/3/9) | absent | **trivial** | Discriminators already exist: document = owner-doc self-reference (VaCuusJsDom.cpp:222), text = tag `"#text"` (Element.cpp:1547). |
| G5 | `replaceChild` | absent | **trivial-moderate** | `Element::ReplaceChild` exists and returns the replaced ElementPtr (Element.h:519; Element.cpp:1421-1448) — adopt it into the replaced node's wrapper, move the inserted node's `Owned` in: the removeChild/insertBefore dance combined. |
| G6 | `insertBefore(node, null)` = append | **works** (VaCuusJsDom.cpp:548-556) | none | Non-child ref appends instead of throwing — benign for Preact, whose stale anchor then degrades to append. |
| G7 | `style.camelCase = v` property names | proxy passes names **verbatim**; `backgroundColor` fails RmlUi's kebab-registered parse with a warning, returns false (VaCuusJsDom.cpp:1097-1113; Element.cpp:591-598; StyleSheetSpecification.cpp:356) | **trivial** | One camelCase→kebab transform in `GStyleFactorySource` (VaCuusJsDom.cpp:117-134), skipping `--custom` keys. [inference] Preact also appends `px` to bare numbers for non-dimensionless props — decide facade vs fork; E-P4. |
| G8 | `className` writes | no `className` property. [inference] Preact renames `className`→`class`, then property-vs-attribute check `'class' in dom` → false → **setAttribute('class', …) path, which works** (VaCuusJsDom.cpp:755-758 → Element.cpp:1713-1714) | **likely none** | The M4 classList-never-attribute trap (VaCuusJsDom.cpp:988-996; Element.cpp:258-276) does **not** bite Preact's own diff: it compares old vnode props, never reads `getAttribute('class')` back [inference]. It DOES bite mixed usage: any Preact `class` write wholesale-resets `meta->style` classes, wiping `classList.add()`-applied ones — same as browser className overwrite, but here `getAttribute('class')` also lies after manual `SetClass`. Document: under Preact, class is Preact's; use style/state, not classList, on Preact-owned nodes. E-P2 verifies the path taken. |
| G9 | `on*` prop → event type case: [inference] Preact lowercases the type only if `name.toLowerCase() in dom`; our wrappers have no `on*` properties, so `onClick` registers type `"Click"` — which RmlUi's lowercase `click` never fires | addEventListener itself is fine | **fork-side patch** (that is what @vacuus/preact is for) | Alternative — defining ~30 lowercase `on*` dummy props on the prototype — is worse than a 3-line fork patch. E-P2 settles it. |
| G10 | `dangerouslySetInnerHTML` → `innerHTML` | only `innerRML` | **trivial** | Alias getter/setter, or fork maps the prop to `innerRML`. Remember: an innerRML write kills every child wrapper synchronously (VaCuusJsDom.cpp:838-845) — fine for Preact, which recreates. |
| G11 | `localName`/`nodeName` for vnode-type comparison on DOM reuse | only `tagName` (lowercase — equals localName semantics, VaCuusJsDom.cpp:796-800) | **trivial** | Add `localName` alias; skip uppercase `nodeName`. |
| G12 | `event.target` identity across dispatch | **works** — identity cache (VaCuusJsEvents.cpp:192-210) | none | Stashed events go dead-not-dangling (VaCuusJsEvents.cpp:658-666) — compat's `persist()` no-op pattern is naturally satisfied. |
| G13 | `preventDefault` semantics | maps to StopPropagation (VaCuusJsEvents.cpp:321-330) | none (documented deviation) | A Preact handler calling `e.preventDefault()` also stops bubbling — behavioral difference to document in @vacuus/preact README. |
| G14 | `ownerDocument`, `contains` | absent | **trivial** | `GetOwnerDocument()` wrap; `contains` = the cycle-guard walk (VaCuusJsDom.cpp:519-525) inverted. Only add if E-P1 shows use. |
| G15 | Silent-null failure contract | facade **never throws**: dead handle / refused op → null/undefined (VaCuusJsDom.cpp:3-9) | **structural — by policy, not code** | Preact assumes DOM ops succeed. A dead wrapper in Preact's `_dom` pointers (game code or an innerRML write killed the subtree) means every subsequent insert silently no-ops and the vdom diverges from the tree with **zero diagnostic**. Cheapest observable: a per-context refused-op counter surfaced in the dev overlay + `vacuus.stats()`. E-P6. |

**Nothing on this table is structural in the facade's architecture** — the ownership matrix (VaCuusJsDomHandle.h:20-70) already covers text nodes because they are Elements (§4). The one structural item is G15, and it is a diagnostics decision, not a rework.

## 4. The text-node question — settled from the vendored RmlUi

**RmlUi text nodes are ordinary `Element`s and everything Preact needs exists as public API:**

- `ElementText final : public Element` with `SetText(const String&)` / `const String& GetText()` (ElementText.h:9, :17-19). `SetText` dirties layout automatically when text changes (ElementText.cpp:108-117).
- The tag `"#text"` is a registered instancer like any other: `RegisterElementInstancer("#text", &default_instancers.element_text)` (Factory.cpp:178), a pooled `ElementInstancerText` returning `ElementPtr(new ElementText)` (ElementInstancer.cpp:44-48). RmlUi's own parser creates text children through the *same generic call the facade already uses for createElement*: `Factory::InstanceElement(parent, "#text", "#text", attributes)` (Factory.cpp:394).
- Text children are **reachable and first-class in the tree**: `GetNumChildren()` excludes only non-DOM extras (scrollbars), not text (Element.cpp:1147-1150); `GetFirstChild`/`GetNextSibling`/`GetPreviousSibling`/`GetLastChild` all return them (Element.cpp:1095-1141). Only `querySelector*` (Element.cpp:1547, :1571) and the facade's `children` getter (VaCuusJsDom.cpp:902) skip them.
- Curiosity: `document.createElement("#text")` **already mints an ElementText today** — the facade lowercases the tag (no-op for `#text`) and calls the same Factory path (VaCuusJsDom.cpp:406-414). What's missing is purely the mutation surface: no way to reach `SetText` from JS.

So `createTextNode(text)` is: `Factory::InstanceElement(nullptr, "#text", "#text", {})` → `static_cast<Rml::ElementText*>` → `SetText(text)` → wrap detached-owning, exactly the createElement shape. Two traps to design around:

1. **The cast must NOT be `rmlui_dynamic_cast`** — the custom-RTTI static duplicates across modules on Linux modular builds and answers null for objects whose vtable lives in VaCuusRml.so (the documented Task 4 finding, VaCuusJsDom.cpp:212-221). Discriminate by `GetTagName() == "#text"` then `static_cast`; RmlUi itself relies on the `#text` instancer producing an ElementText derivative and errors otherwise (Factory.cpp:402-407), so under our default-instancer world the tag is a sound discriminator.
2. Do **not** route text through `Factory::InstanceElementText` — that's the *parser's* entry: it drops all-whitespace text, re-parses `<`-containing text as RML, and tags `{expr}` text for data binding (Factory.cpp:330-397). `SetText` takes the string verbatim (escaping is re-applied only on GetRML output, ElementText.cpp:443-446) — which is exactly DOM createTextNode semantics.

`data`/`nodeValue` getter+setter over `GetText`/`SetText`, `nodeType` 3, and keeping such wrappers out of `children` but in `childNodes` completes G1. Effort: **moderate** — one new thunk family plus tests, zero new lifetime machinery.

## 5. The recycle hazard: Preact across live reload

M4's document replace is **destroy-the-whole-JSContext, browser-refresh semantics**: `OnDocumentReady` moves the old context out and resets it before building fresh (VaCuusJsScriptHost.cpp:231-242, :286-294; spec 3.4, 2026-07-31-vacuus-m4-js-tier1.md:198-202). The module cache lives on the JSContext and dies with it, so every module re-executes (VaCuusJsViewContext.cpp:56-63).

Consequence for Preact — **the hazard is smaller than it looks**: Preact's vdom→dom pointers (`_dom`, `_children`, the root `__k` expando on the container) don't dangle across reload because *no JS survives* the reload. The context destructor neuters every listener shell against the still-live JSContext, neuters the wrapper cache, and `JS_FreeContext` finalizes every wrapper (VaCuusJsViewContext.cpp:82-125). The fresh context re-runs the bundle from scratch against the fresh `document` binding (BindDocument, VaCuusJsDom.cpp:281-303).

What re-boot requires, concretely: **the Preact root must be (re-)created by module top-level execution** — the CLI template's entry must call `render(<App/>, mountPoint)` at module scope (or from a captured `<script>`), because that is the only thing the recycle re-runs. All component state, hooks state, and pending effects are lost by design — the continuity story is M3 data models, which survive the reload (spec :201-202). A `vacuus.onUnload` hook (VaCuusJsViewContext.cpp:249-294) is the place a template could serialize transient state via `vacuus.emit` if it wants opt-in state survival.

The *in-session* version of the hazard is the real one and is G15: anything that kills elements while the context lives — an `innerRML` write on a Preact-owned subtree (kills children synchronously, VaCuusJsDom.cpp:838-845), or C++ tree surgery — leaves Preact holding dead wrappers, and the never-throw contract makes the divergence silent.

## 6. Scheduling: Preact's assumptions vs the M4 pump

The UI frame order is `DrainCommands → DrainInput → DataApply → JsPump → record loop (Context::Update + Render per view)` (VaCuusUIThread.cpp:1011-1073). Inside JsPump, per view: **rAF → timers → job drain** (PumpCallbacks phases 1+2, VaCuusJsViewContext.cpp:310-446; then DrainJobs *inside the same view's segment*, VaCuusJsScriptHost.cpp:443-449 — a promise resolved in a rAF callback runs its `.then` this same pump, after this view's timers). Deliberately not browser order (spec 3.5, M4 spec:214-218).

- **debounceRendering** [inference: default `Promise.resolve().then`]: a `setState` from an input event handler (dispatched during DrainInput, before the pump — VaCuusJsEvents.cpp:10-12) commits in the same frame's job drain, *before* `Context::Update` lays it out — one-frame-correct. A `setState` from a listener RmlUi fires **inside `Context::Update`** (hover chains, animation events — the record loop runs after the pump) enqueues a job that waits for the **next** frame's drain: one frame of latency, inherent to the pump placement, worth a line in the @vacuus/preact docs.
- **Job-drain cap**: the render microtask chain shares `vacuus.Js.MaxJobsPerPump` (default 10 000) across all views (VaCuusJsScriptHost.cpp:427-449); a Preact commit is a handful of jobs — no realistic interaction, but a cap hit defers rendering a frame, with the named Error already logged (:461-464).
- **Effect flushing** [inference: Preact's `afterPaint` races `requestAnimationFrame` against a `setTimeout(…, 100)` fallback]: both exist. A rAF registered during the pump lands in `RafPending` and fires next frame (VaCuusJsViewContext.cpp:310-316, :584-591) — so passive effects run at next frame's pump top: browser-equivalent semantics.
- **Override points** [inference: `options.debounceRendering` / `options.requestAnimationFrame` exist for exactly this]: the fork can pin both to the facade explicitly rather than relying on detection.
- `queueMicrotask`/`Promise` are intrinsic and unshadowed (VaCuusJsViewContext.cpp:29-34); timers don't forward extra args (VaCuusJsViewContext.cpp:528-530) — Preact core passes none [inference].

## 7. Named experiments (what only a real preact bundle can settle)

- **E-P1 cold mount**: bundle stock preact via esbuild, `render(<div/>, mount)` into (a) an empty div, (b) the non-empty `document.body`. Log every undefined-property read (a logging Proxy over a wrapper works). Settles: actual first-render use of `firstChild`/`childNodes`/`nodeType`, whether the excess-children scan fires against pre-existing RML content, and whether the CLI template must mandate a dedicated empty mount element.
- **E-P2 event name case + class path**: `<button onClick=… className=…>`; instrument `AddEventListenerThunk` and `AttributeThunk` to log the received type/attribute strings. Settles: whether `onClick` arrives as `"Click"` (fork patch G9) and whether class goes via `setAttribute('class')` (G8 closed as no-op) or a property write.
- **E-P3 text update path**: counter component with a bare `{count}` text child, after G1 lands. Settles: createTextNode-once-then-`.data`-writes vs recreate-per-render, i.e. whether `nodeValue`/`data` alone suffices or element-level `textContent` is also on Preact's path.
- **E-P4 style writes**: `style={{backgroundColor:'red', width:100, '--x':'1'}}`; log `SetProperty` names/values/returns. Settles: camelCase mapping placement (facade proxy vs fork) and the bare-number `px` question.
- **E-P5 keyed reorder**: keyed list, reverse it. Settles: whether `nextSibling` (G2) is load-bearing for anchor computation — expected symptom without it: order corruption that degrades to append-at-end.
- **E-P6 desync observability**: mount a Preact list, kill its parent's children via a C++ `SetInnerRML`, then `setState`. Settles: what the silent-null divergence (G15) actually looks like and whether a refused-op counter + overlay line is worth its cost.
- **E-P7 reload reboot**: stateful hooks app, touch the `.rml` to trigger M2 live reload. Settles: full re-mount happens, `GetNumListenerRefs()` returns to zero after recycle, and no wrapper leaks across the context death (the §5 argument, observed instead of reasoned).
