# @vacuus/preact

Stock `preact@10.29.7` (npm-fetched at install time — never committed) plus an
**options-based adapter** (`src/index.js`) that pins the three divergences the E-P
experiments observed between preact and the VaCuus DOM facade. Import this package
instead of `preact`; it re-exports all of `preact` and `preact/hooks`.

The full rationale, each patch's mechanism with `file:line` cites, and the
deliberate spec deviation ("patched preact" shipped as adapter, not fork) live in
the header of [`src/index.js`](src/index.js). The observed behavior that forced
each patch is recorded in `docs/research/m5-api-notes/ep-observations.md`.

## What a component author must know (facade deviations)

- **`e.preventDefault()` also stops bubbling** — RmlUi has no separate
  default-action veto; the facade maps it onto `StopPropagation`
  (VaCuusJsEvents.cpp:321-330).
- **`onFocus`/`onBlur`, not `onFocusIn`/`onFocusOut`** — RmlUi fires
  `focus`/`blur` only.
- **Render into a dedicated empty mount element** — mounting into `document` /
  `document.body` throws by construction (E-P1b); pre-existing children of a
  non-empty mount get *adopted* and re-propped (E-P1d). The CLI template ships the
  mount div.
- **`dangerouslySetInnerHTML` is an RML re-parse** — `{{…}}` inside the string is
  a live data expression to RmlUi's scanner (Factory.cpp:343-392). Never feed it
  user data; text children take the scanner-bypassed `createTextNode` path and
  render braces literally.
- **A `setState` from a listener RmlUi fires inside `Context::Update`** (hover
  chains, animation events) commits one UI frame later; input-event handlers
  commit in the same frame (preact-contract.md §6).
- **Reload is re-mount**: a document reload destroys the whole JS context and
  re-runs the bundle from module scope. All component/hook state is lost by
  design; continuity lives in M3 data models (`vacuus.model`).
