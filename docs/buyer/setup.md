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
`vacuus-base.rcss` first (gotchas.md #1 — there is no UA stylesheet), then pick a host.
**Two classes host a document, and each works from Blueprint and from C++:**

- **`UVaCuusWidget`** — "VaCuus View" in the palette, `VaCuusUMGWidget.h`. Screen space.
  Drop it into any UMG tree and set its `Document` to `MyHud/myhud.rml`
  (`bAutoLoadDocument` loads it when the widget is built).
- **`UVaCuusWorldComponent`** — "VaCuus World Panel", `VaCuusWorldComponent.h`. A `DrawSize`
  pixel panel on a quad in the world; same `Document` / `bAutoLoadDocument` properties.

Either way you get a `UVaCuusView*` from `GetView()`, and that is what you feed and drive:
`LoadDocument`, `Close`, the data-model bindings, the input and status surface. **Bind data
models BEFORE LoadDocument** (gotchas.md #9).

### From C++

Add both runtime modules to your module's `.Build.cs` —

```csharp
PublicDependencyModuleNames.AddRange(new string[] { "VaCuus", "VaCuusRender" });
```

— then `#include "VaCuusUMGWidget.h"` (or `"VaCuusWorldComponent.h"`) and `"VaCuusView.h"`.
`UVaCuusWidget` is not UMG-only: `UWidget::TakeWidget()` builds the view and hands back a
plain `SWidget`, so it drops into a raw Slate tree or straight onto the viewport.

```cpp
UGameViewportClient* Viewport = GameInstance->GetGameViewportClient();

UVaCuusWidget* Hud = NewObject<UVaCuusWidget>(GameInstance);
Hud->DocumentPath = TEXT("MyHud/myhud.rml");

// TakeWidget() is what creates the view and queues the load.
TSharedRef<SWidget> SlateWidget = Hud->TakeWidget();
Viewport->AddViewportWidgetContent(SlateWidget, /*ZOrder=*/100);

UVaCuusView* View = Hud->GetView();   // bind models, read status, drive it

// Teardown, in this order: out of the tree first, then release.
// ReleaseSlateResources() is the call that drops mouse capture, hands Slate's
// navigation config back and retires the view.
Viewport->RemoveViewportWidgetContent(SlateWidget);
Hud->ReleaseSlateResources(/*bReleaseChildren=*/true);
```

Keep a reference to the `UVaCuusWidget` (a `UPROPERTY`, or `TStrongObjectPtr` if it lives
outside a widget tree) — nothing else roots it. The plugin ships this exact sequence as a
runnable reference: `vacuus.UMGDemo` (`Source/VaCuusRender/Private/VaCuusUMGWidget.cpp`,
namespace `VaCuusUMGDemo`).

### What is and is not a C++ entry point

| Route | Blueprint | C++ |
|---|---|---|
| `UVaCuusWidget` — screen | yes | **yes** |
| `UVaCuusWorldComponent` — world panel | yes | **yes** |
| `UVaCuusView` — load, bind, drive, status | yes | **yes** |
| `UVaCuusSubsystem::CreateView(host, size)` | no | **only with your own host** — see below |
| the render backend (recorder, replayer, Slate element, frame sinks) | no | no — internal |

`UVaCuusSubsystem::CreateView` is an **extension seam, not the way to show a document**.
Releases up to 0.1 advertised it as "the code route"; it never was one, because the argument
is an `IVaCuusDocumentHost` and the plugin's own host publishes into a render-backend frame
sink that is not part of the supported surface. Call it only when you are supplying your own
`IVaCuusDocumentHost` (`VaCuusDocumentHost.h`) — a headless, offscreen or test view. That is
supported and needs nothing linked: the interface is pure virtual. To put pixels on screen,
use one of the two host classes above.

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
  (read the passport's "The re-baseline" before planning a 1,700-node always-animated HUD).
