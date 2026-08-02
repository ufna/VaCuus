# Setup — install, first document, and shipping the bundle

Three stops: get the plugin running (minutes), author against the loose-file dev
loop, then wire the one asset that ships your UI. The shipping section is the part
that is NOT like other plugins — read the mount predicate table and the
cook-inclusion rule before your first packaged build.

## 1. Install and enable

1. Drop the plugin into `<Project>/Plugins/VaCuus` (Fab install does the same).
2. Enable it (Editor → Plugins, or `"Plugins": [{"Name": "VaCuus", "Enabled": true}]`
   in the `.uproject`). Runtime modules: `VaCuus`, `VaCuusRender`, `VaCuusJs`,
   `VaCuusRml`; `VaCuusEditor` is editor-only (live reload, bundle factory).
3. Verify: the console knows `vacuus.M2Demo`. Run it — an interactive demo document
   appears. `Automation RunTests VaCuus` runs the full shipped suite (test source
   ships on purpose, with its fixtures).

## 2. First document

UI files live as loose files during development. The VFS resolves relative paths
against two ordered roots, **plugin first** (`Source/VaCuus/Public/VaCuusContentPaths.h`
— the order is a decision, documented there):

1. `<Plugin>/Content/DevUI` — canonical, ships the demos; the live-reload watcher
   watches it.
2. `<Project>/Content/DevUI` — YOUR documents. An extension point, not an override
   point: a project file cannot shadow a same-named plugin file.

Make `Content/DevUI/MyHud/myhud.rml` + `.rcss` in your project, link
`vacuus-base.rcss` first (gotchas.md #1 — there is no UA stylesheet), then either:

- **UMG:** add the "VaCuus View" widget (`UVaCuusWidget`) to any UMG tree, set its
  `Document` to `MyHud/myhud.rml` (`bAutoLoadDocument` loads it at construction), or
- **Code:** `UVaCuusSubsystem::CreateView(...)` then `View->LoadDocument("MyHud/myhud.rml")`
  — bind data models BEFORE LoadDocument (gotchas.md #9).

Edit the `.rcss` while PIE runs: the watcher reloads the document in place. That
loop — plus `vacuus dev` from `Web/` if you author in TSX — is the whole dev story.

## 3. Shipping: the bundle

Cooked builds do not ship loose UI files (see the staging gate below). The shipping
path is `UVaCuusBundle`: one asset that packs your whole DevUI tree into a single
payload at cook time — memory-mapped on Win64, resident buffer on Linux/macOS — and
serves it through the same VFS paths, so nothing about your documents changes.

**One-time wiring (three config lines + one asset):**

1. Create the bundle asset (editor: the VaCuusBundle factory; headless:
   `vacuus.Bundle.CreateAsset`). It saves as a marker — payload and index serialize
   only into cooks, so the editor asset stays diffable and tiny.
2. `Config/DefaultGame.ini`:

   ```ini
   [/Script/UnrealEd.ProjectPackagingSettings]
   +DirectoriesToAlwaysCook=(Path="/Game/Bundles")   ; wherever the asset lives

   [VaCuus]
   BundleAssetPath=/Game/Bundles/MyUIBundle.MyUIBundle
   ```

   **The cook-inclusion rule, and why it is loud:** `BundleAssetPath` is a config
   soft path — the cooker never sees it, so nothing cooks the bundle unless you
   hard-reference it or list its directory in `DirectoriesToAlwaysCook`. Forget
   this and the cooked build logs one Error naming the path at first mount
   (`VaCuusSubsystem.cpp` Initialize — the first place that can notice).

3. Optional but recommended, `Config/DefaultEditor.ini`:

   ```ini
   [CookSettings]
   +IncrementalClassAllowList=/Script/VaCuus.VaCuusBundle
   ```

   The erratum truth behind this line (found by running, not reading —
   `docs/research/m6-api-notes/bundle-cook-experiments.md`): incremental cooking is
   NOT default-on in stock 5.8 (`BaseEditor.ini:393` overrides the code default),
   and its skipping is class-allowlisted with project plugins outside the stock
   list. Without the opt-in the bundle simply REPACKS on every cook — safe, never
   stale, just never skipped. With it (and ZenStore on, the 5.8 default), a loose
   UI file edit recooks exactly the bundle package and an unchanged tree skips it —
   the tree-hash cook dependency was verified against edit, add AND delete. The one
   configuration that goes stale is in gotchas.md #19.

**When the bundle mounts — the predicate table:**

| Build | Auto-mount? | What serves UI files |
|---|---|---|
| Editor / PIE | no | loose roots (live reload works); `vacuus.Bundle.Enable 1` packs the loose tree on demand for PIE parity — mounted content then SHADOWS edits, loudly (gotchas.md #18) |
| Uncooked `-game` | no | loose roots; `Bundle.Enable 1` refuses (no editor data to pack from) |
| Cooked Development | yes — the config-listed bundle at game-instance init | bundle first, loose staged files as fallback |
| Cooked Shipping | yes | bundle ONLY — no loose UI files exist in the build |

`vacuus.Bundle.Enable 0` (e.g. via `-dpcvars`) suppresses the cooked auto-mount for
loose-vs-bundle A/B runs. `vacuus.DumpBundle` prints any mounted bundle's index,
provenance and content hash.

**The staging gate that makes Shipping bundle-only:** the plugin's loose-file
staging rules gate on configuration — `Source/VaCuus/VaCuus.Build.cs:138-146` adds
the DevUI `RuntimeDependencies` globs only when
`Target.Configuration != Shipping`. Development packages stage both (so you can
still A/B); Shipping ships the bundle or nothing. **Verify it served:** every view's
teardown logs `N open(s) served by mounted bundles, M by loose roots` — in Shipping,
M must be 0, and the plugin's own packaged acceptance gates assert exactly that.

## 4. What to read next

- `gotchas.md` — before your first authoring day; every entry is a recorded finding.
- `rcss-matrix.md` — the exact supported RCSS surface, generated from the vendored
  RmlUi this plugin ships.
- `perf-guide.md` — the budgets, the measured numbers, and the design idioms
  (read "The breach" before planning a 1,700-node always-animated HUD).
