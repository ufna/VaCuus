# E-P experiment observations — ground truth (2026-08-01)

**Setup:** stock `preact@10.29.7` + `preact/hooks` from npm, bundled with esbuild 0.28.1
(node v26.5.0), `npx esbuild <probe>.js --bundle --format=iife`. Probes ran inside the M4 DOM
rig (`VaCuusJsDomTestRig.h`) on a real UI thread, real `Rml::Context`, real document; every
line below was read out of `globalThis.EPLOG` via the rig's Eval channel and is quoted
verbatim from the automation log. Instrumentation was JS-side only — prototype monkeypatches
and a `Proxy` membrane over the mount container — so **no temporary C++ logging existed and
none needed removing**. Two rounds: against the M4 facade as merged ("pre"), and against the
Task 1 facade ("post"). The committed re-runnable subset lives in
`Content/DevUI/Tests/fixture-dom.js` / `fixture-counter.js` (provenance headers inside) driven
by `VaCuus.Js.Preact.DomContract` / `.CounterTextPath`.

## E-P1 — logging-proxy cold mount

Pre-facade, membrane over the empty `#mount`, `render(h('div',{id:'a'}, h('span',{id:'b'})))`:

```
mount GET __k -> undefined
mount SET __k = {obj}
mount GET namespaceURI -> undefined
mount GET firstChild -> undefined
mount GET firstChild -> undefined
E-P1a THREW: TypeError: not a function
```

- **The container surface preact reads on first render is exactly**: the `__k` expando
  (works today), `namespaceURI`, `firstChild`, then `insertBefore` — nothing else.
- **`document.createElementNS` is preact's ONLY element-creation call** (src/diff/index.js:465
  — the default XHTML namespace branch included) and the M4 facade did not have it: the cold
  mount **cannot proceed at all**. This is a **sixteenth gap the spec's §2(j) table missed**
  (invalidation-protocol item #1; G1–G15 assumed `createElement` was the entry). Landed
  facade-side: `createElementNS(ns, tag)` = `createElement(tag)`, namespace ignored.
- With the call shimmed (`createElementNS -> createElement`, logged), the mount completed:
  depth-first construction, every placement via `insertBefore(node, null)` — **preact never
  calls `appendChild`**; unmount goes through `parentNode.removeChild` (src/util.js:24-25).
- **`replaceChild` was never called and never probed** on any E-P run. G5 is dropped —
  first render does not need it.
- `contains`/`ownerDocument` were never probed either; G14 stays unlanded.

`render(h('div'), document.body)` — remember `document.body === document` on this facade:

```
E-P1b THREW: TypeError: cannot read property '__k' of undefined
```

- Preact special-cases `parentDom == document` into `document.documentElement`
  (src/render.js:16-18), which the facade does not define ⇒ **mounting into
  `document`/`document.body` fails by design**. The CLI template (Task 8) must mandate a
  dedicated empty mount element. Not aliased in Task 1 — an alias to the document object
  would re-enter the same collapse one level down.

Mount into NON-empty `#prefilled` (`<div id="pre1" class="pre"/>` inside), pre-facade:

```
prefilled GET firstChild -> undefined
SHIM createElementNS("http://www.w3.org/1999/xhtml", "div")
insertBefore(<div#f>, null) on <div#prefilled>
E-P1d: render returned; prefilled.innerRML = <div class="pre" id="pre1" /><div id="f" />
```

- With `firstChild` undefined the excess-children scan never arms: pre-existing RML content
  is silently left in place and the app appends after it.

Post-facade the same mount **changes behavior — the scan arms**:

```
prefilled GET firstChild -> <div#pre1>
prefilled GET childNodes -> {obj}
removeAttribute("class") on <div#pre1>
E-P1d: render returned; prefilled.innerRML = <div id="f" />
```

- `firstChild` → `childNodes` snapshot → `localName` match (`'div' == 'div'`) → the
  pre-existing div is **adopted**: preact walks `dom.attributes` to build oldProps
  (src/diff/index.js:497-503), diffs them away (`removeAttribute("class")`, id rewritten)
  and reuses the node. **`attributes` is therefore load-bearing the moment G2+G3+localName
  exist** — another member absent from the gap table (invalidation item #2). Observed
  directly by deleting the landed getter and re-mounting:

```
E-P1e THREW: TypeError: cannot read property 'length' of undefined
```

  Landed: `attributes` as a `{name, value}` snapshot array.
- Post-facade the unshimmed E-P1a cold mount runs natively end to end:
  `mount.innerRML = <div id="a"><span id="b" /></div>`, container reads unchanged
  (`__k`, `namespaceURI`, `firstChild` — now `null`, `insertBefore`).

## E-P2 — event-name case + the class path

```
'onclick' in <button>: false
'className' in <button>: false
'class' in <button>: false
addEventListener("Click", fn, false) on <button#btn>
setAttribute("className", "xcls") on <button#btn>
mounted: btn=<button#btn> getAttribute(class)=null getAttribute(className)="xcls" classList.contains(xcls)=false
setAttribute("class", "ycls") on <button#btn2>
mounted: btn2=<button#btn2> getAttribute(class)="ycls" classList.contains(ycls)=true
dispatch lowercase 'click': []
dispatch 'Click' (preact's registered case?): ["handler fired: type=Click target=<button#btn>"]
```

- **Event case (G9 confirmed, fork-side):** `onClick` registers type **`"Click"`** — preact
  lowercases only when `'onclick' in dom` (src/diff/props.js:84-87), and no `on*` properties
  exist on the wrappers. RmlUi's native `click` never fires that listener (observed: empty),
  while a synthetic `Click` dispatch does. **Task 8's fork must lowercase the sliced event
  name** (or pin `options`); the facade keeps no ~30-dummy-`on*` shim.
- **`className` — the spec's §2(j) claim is WRONG for preact 10.29.7** (invalidation item #3,
  the loud one): "`className` is likely a no-op (Preact routes to `setAttribute('class')`)"
  does not hold — the `className → class` rename exists **only inside the SVG-namespace
  branch** (src/diff/props.js:113-118). On a non-SVG element the write lands as
  **`setAttribute("className", …)`**: a literal `className` attribute that no RCSS selector
  ever matches; `getAttribute('class')` stays null; the class is **silently lost**.
  Spec errata required. Fork options for Task 8: rename `className`→`class` in the fork's
  `setProperty`, or have the facade define a `className` accessor (making `'className' in
  dom` true flips preact onto the property path). **Until then: author `class=`, which works
  end to end** (observed: applied immediately, `classList.contains` true).

## E-P3 — text-update path (post-G1)

```
textContent own-descriptor on element proto: ABSENT
createTextNode("count:")
createTextNode(0)
mounted: ctr.innerRML = "count:0"
after bump 1: {"log":[...,"data = 1 (was \"0\")"],"rml":"count:1"}
after bump 2: {"log":[...,"data = 2 (was \"1\")"],"rml":"count:2"}
```

- **Create-once-then-`data`**: the mount creates each text node exactly once (the number
  child arrives RAW — `createTextNode(0)`, not `"0"`; the facade's string coercion handles
  it), and every subsequent update is a bare `.data =` write (src/diff/index.js:484-485).
  No `createTextNode` after mount, ever.
- **`textContent` is NOT on preact's path** — absent from the prototype and never missed.
  Per the plan's condition it is **not landed**.
- The `setState` → `Promise.resolve().then` debounce commits inside the same UI frame's job
  drain (the M4 pump order): the Eval after each bump already reads the updated tree.

## E-P4 — style writes

Pre-facade (`style={{backgroundColor:'red', width:100, '--x':'1'}}`):

```
style.backgroundColor = "red" -> SetProperty returned false
style.width = "100px" -> SetProperty returned true
style.setProperty(--x, "1") -> true
readback background-color = "#00000000"
```

Post-facade:

```
style.backgroundColor = "red" -> SetProperty returned true
style.width = "100px" -> SetProperty returned true
style.setProperty(--x, "1") -> true
readback background-color = "#ff0000"
readback backgroundColor  = "#ff0000"
readback width            = "100px"
readback --x              = "1"
```

- **camelCase→kebab is facade-side and landed** (all three proxy ops map, so the camel
  spelling reads back too). Custom properties arrive through `setProperty('--x', …)`
  already-kebab (src/diff/props.js:16-17) and pass verbatim.
- **Bare numbers never reach the facade**: preact appends `px` itself
  (src/diff/props.js:23 — `width: 100` arrived as `"100px"`). Per the plan's condition the
  facade appends **nothing**; a bare number that does arrive (hand-written JS) fails RmlUi's
  parse with its own loud `Syntax error parsing inline property declaration` warning — that
  refusal is pinned by `VaCuus.Js.Dom.StyleCamelCase`.

## E-P5 — keyed list reversal

Pre-facade (no `nextSibling`), `[1,2,3,4]` → `[4,3,2,1]`:

```
insertBefore(<div#row4>, <div#row1>) on <div#list>
insertBefore(<div#row3>, null) on <div#list>
insertBefore(<div#row1>, null) on <div#list>
after reversal: row4,row2,row3,row1
```

Post-facade (G2 landed), same bundle:

```
insertBefore(<div#row4>, <div#row1>) on <div#list>
insertBefore(<div#row3>, <div#row1>) on <div#list>
after reversal: row4,row3,row2,row1
```

- **`nextSibling` is load-bearing for the insertion anchors** (src/diff/children.js:374-376):
  without it the anchor walk reads `undefined`, placements degrade to append-at-end, and the
  order **corrupts** — the predicted symptom, observed exactly. With it, the reversal is two
  minimal moves and the order is correct. This is the keyed test's
  seen-to-fail evidence; `VaCuus.Js.Preact.DomContract` pins the green half permanently.

## E-P6 / E-P7

Not run in Task 1 — they gate fork/CLI behavior (desync observability, reload re-mount) and
run in Task 8 against `@vacuus/preact` per the plan (8.2).

## Invalidation summary (spec §2(j) errata needed)

1. **`createElementNS` missing from the gap table** — it is preact's *only* element-creation
   call; without it nothing mounts. Landed facade-side.
2. **`attributes` missing from the gap table** — load-bearing for adoption the moment
   traversal lands. Landed facade-side (snapshot array).
3. **The `className` claim is inverted for preact 10.29.7** — no `className`→`class` rename
   outside SVG; the write lands as a dead `className` attribute and the class is silently
   lost. Fork-side fix (Task 8) + spec errata.
4. Confirmations, not contradictions: G1/G2/G3/G4/localName/G7 as tabled; `replaceChild`
   (G5) and `textContent` unneeded — both dropped with their observations recorded above;
   event case G9 confirmed fork-side; mount-into-`document` impossible (dedicated mount
   element is a CLI-template requirement).

## The brace-injection restore run (Task 1.3)

`VaCuus.Js.Dom.BraceInjection` green half: `createTextNode('{{Health}}')` under a live
`data-model="hud"` scope with `Health` bound = literal `{{Health}}` rendered, no `data-text`
attribute, still literal after real `Context::Update` frames. Red mechanism half (committed):
`Factory::InstanceElementText` with the identical string → `data-text` tagged → DataViewText
constructed at insertion (Element.cpp:2162) → evaluates to **`100`**, the bound model value —
user data became an expression.

The restore run — the facade's own `createTextNode` temporarily routed through the parser
entry (`InstanceElementText` into a scratch parent, child stolen), rebuilt, test re-run —
failed exactly as the injection predicts, verbatim:

```
Error: Expected 'green: createTextNode('{{Health}}') is never scanned' to be "{{Health}}|{{Health}}|null", but it was "{{Health}}|{{Health}}|".
Error: Expected 'green: still literal after Context::Update ran' to be "{{Health}}|{{Health}}", but it was "100|100".
Error: Expected 'green probe: the raw element holds the literal string' to be "{{Health}}", but it was "100".
Error: Expected 'green probe: no data-text attribute anywhere' to be false.
```

(the first line's trailing `|` is the `data-text` attribute reading back as an empty string
instead of null — the scanner's tag, present). Bypass restored; the suite is green.
