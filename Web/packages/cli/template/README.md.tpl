# __APP_NAME__

A VaCuus TSX app, created by `vacuus create`. Preact (via `@vacuus/preact`) over
the VaCuus DOM facade, rendered fully off the game thread.

## Workflow

```bash
# From the app directory (npm install once, in the Web/ workspace root):
node ../../packages/cli/bin/vacuus.mjs dev --app .    # watch + rebuild into Content/DevUI/__APP_DIR__/
node ../../packages/cli/bin/vacuus.mjs build --app .  # one-shot build + provenance
node ../../packages/cli/bin/vacuus.mjs lint src/*.tsx *.rcss
```

With the editor running, `vacuus dev` + the M2 live-reload watcher give you
save-to-screen: every successful rebuild reloads the document (a **full
re-mount** — module top level re-runs; component state is lost by design, game
state lives in data models).

**THE ONE TRAP (read this now, not after an hour of confusion):** a TSX build
error writes **no bundle**, so **no reload fires** and the engine silently keeps
the **last good UI**. The failure exists only in the `vacuus dev` terminal.

## Web-dev gotchas (the arch-spec §13 list)

- **No UA stylesheet** — without `vacuus-base.rcss` linked first, `div` is
  inline and your layout collapses to one line.
- **box-shadow is not animatable** — RmlUi refuses it at animation start;
  `vacuus lint` catches it at authoring time.
- **Strict tween parsing** — `transition: opacity 0.3s ease-in-out;` works;
  loose browser-tolerated orderings do not.
- **position: absolute** resolves against the nearest positioned ancestor; give
  panels `position: relative` deliberately.
- **classList never writes the class attribute** — on preact-owned nodes drive
  classes through props, and don't write `[class…]` attribute selectors.
- **Localization**: route user-visible strings through `vacuus.translate`.
  Gettext-style keys (the English text, `{token}`s included) degrade to identity
  until the game pushes a table.
- **`e.preventDefault()` also stops bubbling** (maps to StopPropagation).
- **Errors in engine logs show bundle positions** — paste the stack into
  `vacuus symbolicate --bundle <built bundle>` for TSX positions.
