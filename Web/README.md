# VaCuus Web workspace

TSX over the VaCuus DOM facade (M5 Track P): `@vacuus/preact` (stock preact +
an options adapter — see `packages/preact-vacuus/src/index.js` for the three
patches and why an adapter beat a fork), `@vacuus/cli`
(create/dev/build/symbolicate/lint/manifest), and the apps whose built bundles
are committed under `Content/DevUI/`.

**Source-only** (Fab rules): no `node_modules`, no binaries, no build output in
git — except the `Content/DevUI` bundles, which are COMMITTED WITH PROVENANCE
(`*.provenance.json`: preact version, facade-manifest hash, build command) so
every other machine and CI stay hermetic; the in-engine suite verifies the
provenance and skips-with-a-named-warning when a facade change makes a bundle
stale (spec §2(l)).

## Setup and daily use

```bash
cd Web && npm install          # once per checkout (preact 10.29.7 + esbuild 0.28.1, exact pins)

node packages/cli/bin/vacuus.mjs create myhud            # scaffold Web/apps/myhud + Content/DevUI/myhud
node packages/cli/bin/vacuus.mjs dev --app apps/myhud    # watch -> Content/DevUI/myhud/ (M2 live reload picks it up)
node packages/cli/bin/vacuus.mjs build --app apps/demo-hud   # the committed M5Hud bundle + provenance
node packages/cli/bin/vacuus.mjs lint apps/demo-hud/src/hud.tsx
node packages/cli/bin/vacuus.mjs manifest --check        # typings manifest staleness (smoke runs this)
node smoke.mjs                                           # the whole node-side smoke
```

**THE TSX-ERROR TRAP:** in `vacuus dev`, a failed build writes no bundle, so no
reload fires and the engine silently keeps the last good UI. The failure exists
only in the dev terminal. (The CLI prints this loudly; so do the app READMEs.)

## The pieces

| Path | What |
|---|---|
| `packages/preact-vacuus` | `@vacuus/preact`: preact re-exported through the options adapter (event-name lowercase, className→class, scheduling pinned, innerHTML alias) |
| `packages/cli` | the toolchain; `types/vacuus.d.ts` is the hand-written typed surface, `fixtures/lint/` the linter's fire/silent pairs |
| `apps/demo-hud` | the M5 TSX HUD → `Content/DevUI/M5Hud/hud_bundle.js` (tests: BundleMount, Cost.PumpSteadyTsx; demo: Task 9) |
| `apps/ep-fixture` | the adapter test fixture → `Content/DevUI/Tests/fixture-vacuus-preact.js` (tests: AdapterContract, DesyncObservability, ReloadRemount) |
| `smoke.mjs` | node-side smoke: lint fixtures both ways, manifest freshness + doctored-stale detection, create/build roundtrip, symbolicate |

## The typings manifest pipeline (spec §2(l))

`vacuus.d.ts` → `vacuus manifest` → `Content/DevUI/Tests/vacuus-api-manifest.json`
(committed) → the in-engine `VaCuus.Js.Preact.Conformance` walks the real
prototypes/globals against it **both ways** (typed-but-absent fails,
present-but-untyped fails; the manifest's `excluded` block documents the names
left out by design and why). `smoke.mjs` fails when the committed manifest is
stale against the `.d.ts`. Changing the facade surface therefore requires
touching the `.d.ts`, regenerating the manifest, and rebuilding the committed
bundles — and every step that is skipped has a named failure.
