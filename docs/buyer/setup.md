# Setup — install, first document, and shipping the bundle

Three stops: get the plugin running (minutes), author against the loose-file dev
loop, then wire the one asset that ships your UI. The shipping section is the part
that is NOT like other plugins — read the mount predicate table and the
cook-inclusion rule before your first packaged build.

## 0. What is in the box — read this before you install

There are two ways this plugin arrives, and they install into **different places**:

**From [Fab](https://fab.com/s/6571fd1716eb) — the normal way.** Epic builds the binaries
for a code plugin itself, and the launcher installs the result **into the engine, not into
your project**. Nothing compiles on your machine, so a **Blueprint-only project works**:
enable the plugin (step 2 below) and that is the whole install. The full source is in the
package regardless — Fab's rule for UE-facing code is that sellers upload source, and what
we upload carries no `Binaries/` directory at all; the binaries a Fab install lands with
are Epic's build of that source for your engine version.

**From source** — a release archive from
[GitHub Releases](https://github.com/ufna/VaCuus/releases) dropped in by hand, or this
repository. This route goes into `<Project>/Plugins/VaCuus` and **does** compile on your
machine, so it needs a C++ project:

> If you are reading this inside such an archive, check that you took the right one before
> you install: the archive is named for the engine it was built on
> (`VaCuus-<version>-UE5.6.zip`, `-UE5.7.zip`, `-UE5.8.zip`), and the stamp is per
> package, not per plugin — a 5.6 package on a 5.8 engine loads with a warning even though
> the source is identical (the engine note further down this section). Releases is the only
> place these archives are published; the same page carries `SHA256SUMS.txt`.

> A Blueprint-only project has no build target and no toolchain, so nothing compiles the
> four runtime modules and the plugin cannot load. Converting is a two-minute job and
> permanent: in the editor, **Tools → New C++ Class → None → Create Class**. That adds
> `Source/` and a target, and from then on the project builds plugins like any other C++
> project.

You also need the platform toolchain the engine already requires to compile anything:
Visual Studio 2022 with the C++ workload on Win64, Xcode on macOS, the bundled clang
toolchain on Linux. First launch after dropping the plugin in takes a few minutes — that
is the plugin compiling, once.

Two more properties of the shipped copy, both of which differ from the repository and
neither of which the engine tells you about:

- **It is not enabled by default.** `RunUAT BuildPlugin` rewrites the descriptor it
  packages: `bEnabledByDefault` is cleared and `bInstalled` is set to true
  (`BuildPluginCommand.Automation.cs:434-445`). So step 2 below is not optional
  housekeeping — without it the plugin sits in your `Plugins/` folder doing nothing.
- **It is stamped with an engine version.** The same rewrite writes `EngineVersion` from
  the engine that built the package. A different engine version will warn on load; that is
  a compatibility marker, not a hard block.

  **Which engines this plugin supports: 5.6, 5.7 and 5.8.** One source tree serves all
  three — built (editor and packaged-game targets) and tested, on Linux. The plugin
  itself needed no changes for 5.7; a project created on an older engine may need its
  `Target.cs` brought to the engine's current defaults
  (`DefaultBuildSettings = BuildSettingsVersion.V6;
  IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;`) — that is a project-side
  setting, not a plugin one. Take the package stamped with your engine: the stamp is
  per-package, not per-plugin, so a 5.6 download on 5.8 warns even though the source
  compiles on both.

  One thing that does NOT travel between engines, if you ever move content between
  projects: `.uasset` files are forward-compatible only. Anything saved by a 5.8 editor is
  refused outright by 5.6 — silently, with no version line in the log; the asset simply
  never appears. The plugin's own content is authored on the oldest supported engine for
  exactly this reason.

## 1. Install and enable

1. Install: a Fab install already put the plugin into the engine — there is nothing to
   copy. A source install goes into `<Project>/Plugins/VaCuus`.
2. Enable it — Editor → Plugins is simplest. By hand, the `.uproject` entry needs the two
   fields the editor would write, or every editor start shows "Project file is out of date"
   (the editor compares both against the plugin descriptor,
   `GameProjectUtils.cpp:946-976`):

   ```json
   "Plugins": [
     {
       "Name": "VaCuus",
       "Enabled": true,
       "MarketplaceURL": "https://fab.com/s/6571fd1716eb",
       "SupportedTargetPlatforms": ["Win64", "Mac", "Linux", "Android", "IOS"]
     }
   ]
   ```

   Copy both values from `VaCuus.uplugin` rather than from this page — they follow the
   release. Clicking the toast's **Update** once writes the same thing. Runtime modules: `VaCuus`,
   `VaCuusRender`, `VaCuusJs`, `VaCuusRml`; `VaCuusEditor` is editor-only (live reload,
   bundle factory).
3. Verify: the console knows `vacuus.M2Demo`. Run it — an interactive demo document
   appears. `Automation RunTests VaCuus` runs the full shipped suite (test source
   ships on purpose, with its fixtures).

### Platforms — and what refusal looks like

`VaCuus.uplugin` declares `"SupportedTargetPlatforms": ["Win64", "Mac", "Linux",
"Android", "IOS"]`.

**Android and iOS are supported.** Android: the arm64 Vulkan (ES3_1) pipeline compiles,
cooks, packages, boots and renders (first proven 2026-08-11; the NDK r27c header fix it
took is recorded as vendored patch #6 in `Source/ThirdParty/RmlUi/VENDORED_TAG.txt`).
iOS: zero plugin source changes to build, a full cook passes (every VaCuus global
shader cross-compiles to Metal ES3_1), and the signed build runs on hardware — verified
2026-08-12 on an iPhone 11 Pro (iOS 26.0.1, UE 5.8.1): the 1,732-node reference HUD
rendered over the live scene, driven by its JavaScript (quickjs-ng needs no JIT and no
entitlement), with the device log confirming the UI thread is off the game thread on
the phone too. Building, signing and packaging for either platform is standard Unreal —
nothing plugin-specific to set up.

Mobile input is wired end to end, verified by hand on the iPhone: a tap clicks (once —
Slate's touch-to-mouse fallback is answered, not duplicated), a finger drag scrolls
with RmlUi's own inertia and cancels the click as a scroll should, and focusing a text
field summons the platform's on-screen keyboard, with typed text landing in the
document and a tap outside dismissing it. World panels take taps too. The finer edges
still recorded in the tracker rather than hidden: the click-vs-scroll threshold is not
yet DPI-scaled on very dense screens, multi-finger gestures share one capture, and an
iPad with a hardware keyboard types without IME composition.

Unreal's own refusal for an unsupported platform is quiet, so here is what you will
actually see:

- **A game target for an unsupported platform silently drops the plugin.** UBT emits
  `Ignoring plugin 'VaCuus' … due to unsupported target platform` at `-VeryVerbose`
  only — a normal build prints nothing, exits 0, and produces a binary with no VaCuus
  modules. The `.uplugin` is not staged, `/VaCuus/` content is not cooked, and your UI
  is simply absent at runtime with no error. **If a build for a platform outside the
  declared list comes back with no UI
  and no message, this is why.**
- **Step 2 above protects you from that.** Listing the plugin in your `.uproject`
  turns the silence into a hard build failure that names the plugin:
  `VaCuus.uplugin is referenced via <YourProject>.uproject with a mismatched
  'SupportedTargetPlatforms' field`. Read that as "VaCuus does not support the
  platform you are targeting" — the engine's suggested fix (relaunch the editor to
  update references) is aimed at a different problem and will not help here.
- **If one of your C++ modules depends on a VaCuus module**, UBT compiles part of the
  plugin even after dropping it — you would get whatever your dependency chain reaches
  and nothing else, with no renderer and no staged descriptor. The plugin fails that
  build itself instead, with the platform named, from `VaCuusRml.Build.cs`.
- **Authoring is unaffected.** `bIncludePluginsForTargetPlatforms` defaults to editor
  targets, so the editor loads the plugin normally on Win64/Mac/Linux and only *game*
  targets for other platforms refuse.

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

### Binding and input rules — read these before your first model

Every rule below once cost a real integration an hour or more, because breaking it renders
something plausible rather than failing. Each now logs, but knowing the rule is cheaper.

1. **A model field binds only if it is exposed** — `BlueprintReadOnly`/`BlueprintReadWrite`,
   or `VisibleAnywhere`/`EditAnywhere` (`CPF_BlueprintVisible | CPF_Edit`,
   `Source/VaCuus/Private/VaCuusModelLayout.cpp:747`). A plain `UPROPERTY()` is skipped
   quietly, because most structs have members a UI should not see; when **no** field
   survives, the model's Warning names every member it skipped. One knock-on: UE's Python
   binding strips the `b` from bool names, so a now-exposed `bCountdown` and `Countdown`
   collide there (PythonScriptPlugin `PyGenUtil.cpp:1957-1961`) — rename one.
2. **No container inside an array element.** In `TArray<FCard>`, a `TArray` (or `TMap`,
   `TSet`) member of `FCard` is not bound: change tracking is one bit per top-level array,
   so an inner list's edits would be invisible. The rest of the row binds, and one Warning
   names the array and the member. To show per-row lists, flatten them into one top-level
   array whose entries carry their row's index, and filter with `data-if` inside the row
   loop. An `FText` anywhere in a row still refuses the whole array — project it to
   `FString` on the game side.
3. **Reserved names, and aliases.** `t` is the live translation variable in every model
   (localization.md §3); a field named `t` is refused. A `data-for` alias is unreachable when
   it equals `t` **or any top-level field of the same model** — RmlUi resolves top-level
   variables first — and is logged as an Error naming the element and the document. Give
   aliases names no field has: `card`, `trait`, `row`.
4. **A click writes a request; the game decides.** `data-event-click="Pick = 2"` does not
   change the UI's copy of the model. The write is delivered to the game thread as
   `UVaCuusView::OnModelWrite` (model, path, value) and the control snaps back until the
   game pushes a new value (`Source/VaCuus/Public/VaCuusView.h:128-135`). **A write equal to
   the field's current value is swallowed** (the echo rule,
   `Source/VaCuus/Private/VaCuusWriteRouter.h:65-79`), so a request field must return to an
   idle value — 0, -1, "" — once the game has acted, or the second press of the same button
   never arrives. `Content/DevUI/m4_demo.rml` writes `Ammo = Ammo - 1` for this reason.
5. **What takes a press away from the game.** A press over an element goes to the UI, not
   the game, when the element is a `button`/`input`/`select`/`textarea`/`a`, has
   `tab-index: auto`, carries the `vacuus-interactive` attribute, or has a press handler in
   its markup — `data-event-click` (or `dblclick`, `mousedown`, `mouseup`, `mousescroll`,
   `drag*`) or an inline `onclick`-style handler. Hover handlers do not count, and
   `vacuus-passthrough` gives a whole subtree's presses back to the game. A div given a
   listener from script with `addEventListener` is invisible to all of this: mark it
   `vacuus-interactive`. A passive overlay — a comparison card over an FPS view — should
   carry none of these, or it eats the shot.
6. **`dp` follows the DPI curve; `px` does not.** A screen view is laid out in physical
   pixels, and `dp` is scaled by Slate's geometry scale — your project's DPI curve, as UMG
   uses — so dp-authored UI keeps its size from 1080p to 4K, while `px` stays one physical
   pixel. `vacuus.DpFollowsDpiScale 0` restores dp == px (the behaviour of 1.0.3 and
   earlier). World panels keep dp == px: their `DrawSize` is their pixel size.

### From C++

Add both runtime modules to your module's `.Build.cs` — **and `UMG`** —

```csharp
PublicDependencyModuleNames.AddRange(new string[] { "VaCuus", "VaCuusRender", "UMG" });
```

`UMG` is not optional and its omission does not show up as a missing header. `UVaCuusWidget`
derives from `UWidget`, so calling `TakeWidget()` below needs UMG on YOUR module's link line,
and UBT does not put it there for you: a public dependency propagates **include paths**
transitively, not libraries — the recursive link gather is gated on
`bIsModuleBinaryAStaticLibrary`, and in a modular editor build every module is its own shared
library (`UnrealBuildTool/Configuration/UEBuildModule.cs:950-958`). So the code compiles and
then dies at link: `ld.lld: error: undefined symbol: UWidget::TakeWidget()` with clang, an
`LNK2019: unresolved external symbol` naming `UWidget::TakeWidget` with MSVC. Naming UMG
yourself is the fix.

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

**If this is the first UI your project has ever shown, that snippet is not yet enough:** the
game viewport holds the pointer capture by default, so the document comes up looking correct
and receives no input at all, silently. One `SetInputMode(FInputModeGameAndUI)` on the local
player controller fixes it — gotchas.md #23 has the mechanism and the code. A widget dropped
into an existing UMG tree inherits whatever input mode the game already set and needs
nothing.

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
| `SVaCuusWidget` + `VaCuusSlateView.h` — hand-composed stack | no | **yes** — see below |
| `UVaCuusSubsystem::CreateView(host, size)` | no | **only with your own host** — see below |
| the render backend (recorder, replayer, Slate element, frame sinks) | no | no — internal |

`UVaCuusSubsystem::CreateView` is an **extension seam, not the way to show a document**.
Releases up to 0.1 advertised it as "the code route"; it never was one, because the argument
is an `IVaCuusDocumentHost` and the plugin's own host publishes into a render-backend frame
sink that is not part of the supported surface. Call it only when you are supplying your own
`IVaCuusDocumentHost` (`VaCuusDocumentHost.h`) — a headless, offscreen or test view. That is
supported and needs nothing linked: the interface is pure virtual. To put pixels on screen,
use one of the two host classes above.

### Stacking views: when `UVaCuusWidget` is not enough

`UVaCuusWidget` composes exactly **one** view into **one** widget. That is the right shape for
almost everything, and the wrong shape for a UI that stacks views — a persistent chrome layer
over a swapped content layer, the ordinary shooter-lobby arrangement.

**Stacked `UVaCuusWidget`s do not compose, and the reason is Slate, not this plugin.** Slate
delivers a pointer event to the topmost hit-testable widget, and an `Unhandled` reply from it
goes to the **game** — never down to a covered sibling. So the lower view never hears a click
the upper one declined. The fix is one widget that owns the input and routes between the views
itself, which means subclassing:

```cpp
#include "SVaCuusWidget.h"      // subclass this
#include "VaCuusSlateView.h"    // build a view's render side by hand

TSharedRef<FVaCuusSlateElement> Element = VaCuusSlateView::MakeElement();
UVaCuusView* View = Subsystem->CreateView(VaCuusSlateView::MakeDocumentHost(Element), Size);
TSharedRef<SVaCuusWidget> Widget = SNew(SVaCuusWidget, View, Element);
Viewport->AddViewportWidgetContent(Widget, /*ZOrder=*/100);
```

`FVaCuusSlateElement` is an **opaque handle** — declared, never defined for you. Hold it, pass
it to the two calls above, drop it at teardown; there is nothing else to call on it. That is
deliberate: the type carries the glass distiller, the replay renderer and the engine-version
compatibility seam, and none of those are a promise this plugin is willing to freeze.

**Override the input virtuals and chain to `Super`.** Each handler in `SVaCuusWidget` queues
the event for the UI thread *and* answers Slate from the view's published snapshot; skipping
`Super` drops both halves. A router typically calls `Super::OnMouseButtonDown`, returns its
reply when it is `Handled`, and otherwise forwards the point to the lower view itself.

**Teardown order is the part that bites**, and it is the same rule `UVaCuusWidget` follows
internally: call `ReleaseOwnPointerCapture()` and `DetachView()` **before** the view is
destroyed, then drop the widget, then the element. Slate's captor is a weak widget path that
never notices the leaf died, so a widget dropped mid-drag trips an ensure inside
`FSlateApplication`.

Your module must list `VaCuusRender`, `VaCuus` and **`SlateCore`** itself — subclassing
`SVaCuusWidget` references `SWidget` symbols directly, and a dependency's dependency is not
on your link line (see the `UMG` note above; the mechanism is identical).

The shipped worked example is the lobby demo in the **VaCuusDemo project** (not in the
plugin): `Source/TP_ThirdPerson/VaCuus/VaCuusLobbyDemo.cpp`, a chrome view over a swapped
content view with one `SVaCuusLobbyRouterWidget` between them. It lives outside the plugin on
purpose — it compiles against this page and nothing else, which is what keeps the page honest.

Edit the `.rcss` while PIE runs: the watcher reloads the document in place. That
loop — plus `vacuus dev` from `Web/` if you author in TSX — is the whole dev story.

## 3. Shipping: the bundle

Cooked builds do not ship loose UI files (see the staging gate below). The shipping
path is `UVaCuusBundle`: one asset that packs your whole DevUI tree into a single
payload at cook time and serves it through the same VFS paths, so nothing about your
documents changes.

**What goes in.** Every file of a packed kind under your project's `Content/DevUI` and
under every other enabled plugin's `Content/DevUI`, minus `Tests/` folders. From
**VaCuus's own** `Content/DevUI`, only what your documents can depend on: its
stylesheets (`M5Hud/vacuus-base.rcss` among them) and its fonts (the default face, which a
Shipping build loads from the bundle and nowhere else). The plugin's demo documents,
scripts and images stay out — before this, a ten-file UI cooked into a 41-file bundle. The
cook log line counts what was left out (`… N plugin demo file(s) excluded`). A project that
does want the demos in its package — the plugin's own test hosts boot them in Shipping —
sets `bPackPluginDemos=True` under `[VaCuus]` (step 2 below). A project document that has
the same path as a plugin demo now ships, where it used to be shadowed by the demo.

**Memory-mapped or resident is the engine's call, not a platform law we wrote.** The
loader maps the payload where `FPlatformProperties::SupportsMemoryMappedFiles()` is
true and hands us one resident buffer where it is false. Of the platforms this plugin
supports, **Win64 answers true; Linux and macOS answer false**, because neither
declares the property and both inherit the generic `false`. The VFS reads a span
either way, so the branch changes footprint and nothing else — and it is not a
platform test: a platform that *can* map can still end up resident if the mapping is
refused. Do not infer one from the other. The mount log line names which branch it
actually took, and that line is the one to read:

```
LogVaCuus: Mounted bundle '<name>': 24 entries, 461881 bytes, resident buffer (...), hash <hex>
```

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
   ; bPackPluginDemos=True   ; only to ship VaCuus's own demo documents as well
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

   **Measured on 5.8 only.** 5.6 has no `CookIncrementalDefaultIncremental` key in its
   `BaseEditor.ini` at all, so its incremental-cook behaviour is a different question and
   nobody here has run the experiment on it. The opt-in line above is harmless either way
   — without incremental cooking the bundle just repacks every cook.

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

**Your own DevUI files and the loose-file leg — one config line, and only for
non-Shipping packages.** The bundle packer walks every document root
(`VaCuusContentPaths::GetDocumentRoots()`), so everything under
`<Project>/Content/DevUI` is inside the bundle and Shipping needs nothing from you. The
loose copies are a different story: the plugin's staging globs are anchored at
`$(PluginDir)/Content/DevUI` (`Source/VaCuus/VaCuus.Build.cs`), which is the plugin's own
tree and not yours — nothing in the plugin can stage files from your project. So a
packaged **Development** build stages the plugin's demo documents and none of yours, and
`vacuus.Bundle.Enable 0` in that build finds an empty loose root. If you want that A/B leg,
add your directory yourself:

```ini
[/Script/UnrealEd.ProjectPackagingSettings]
+DirectoriesToAlwaysStageAsUFS=(Path="DevUI")
```

The path is relative to your project's `Content` directory
(`ProjectPackagingSettings.h:585-591`, resolved at
`CopyBuildToStagingDirectory.Automation.cs:837`), so `DevUI` means
`<Project>/Content/DevUI`.

**The staging gate that makes Shipping bundle-only:** the plugin's loose-file
staging rules gate on configuration — `Source/VaCuus/VaCuus.Build.cs:138-146` adds
the DevUI `RuntimeDependencies` globs only when
`Target.Configuration != Shipping`. Development packages stage both (so you can
still A/B); Shipping ships the bundle or nothing. **Verify it served:** every view's
teardown logs `N open(s) served by mounted bundles, M by loose roots` — in Shipping,
M must be 0, and the plugin's own packaged acceptance gates assert exactly that.

## 4. Verify it yourself — the headless recipe

`Automation RunTests VaCuus` was mentioned twice above without saying how to *run* it
without a person sitting at the editor. Here is the whole pipeline, and then the four
things about it that will each cost you an afternoon the first time.

**The suite** — no RHI needed, so it runs on a build agent, and it exits by itself with a
status (0 when every test passed, non-zero otherwise):

```bash
<Engine>/Binaries/<Platform>/UnrealEditor-Cmd <Project>.uproject \
  -unattended -nullrhi -nosplash -ExecCmds="Automation RunTests VaCuus; Quit"
```

The `;` there is not a slip — see 1 and 4 below.

**A visual run** — a real frame, offscreen, at a resolution you chose:

```bash
<Engine>/Binaries/<Platform>/UnrealEditor <Project>.uproject \
  -game -RenderOffscreen -ForceRes -resx=1920 -resy=1080 \
  -ExecCmds="vacuus.RefHud, vacuus.M1HUD.AutoShot 10,"
```

Screenshots land in `Saved/Screenshots/<Platform>/`.

**A click, by element id** — append to the same command list, e.g.
`-ExecCmds="<your command>, vacuus.Click ready-btn 1.0,"`. `vacuus.Click <element-id>
[delaySeconds] [viewId]` asks the UI thread where the element is after its next layout,
presses and releases at its centre through Slate's real routing, and logs who took it:
`…the press was taken by THE UI` or `…by THE GAME`. It aims at the element, not at
coordinates, so it survives `data-if` lines that move buttons and window resizes. A
missing id, or an element that is hidden or has no area, is an Error that says which.

**1. `-ExecCmds` splits on COMMAS, not semicolons.** `ParseExecCommands.cpp:27` is the
split, and single-quoted commas are the documented escape (`:11-14`). A recipe of ordinary
console commands written with `;` is not rejected — it is parsed as ONE command, which does
not exist, and no "not recognized" line appears anywhere. **The value also swallows every
argument after it**: `ParseExecCmdsFromCommandLine` passes `bShouldStopOnSeparator=false`
(`ParseExecCommands.cpp:63`), so anything to its right becomes part of the last command.
Put `-ExecCmds` last and **end a console-command list with a comma**, as the visual recipe
does.

**`Automation` is the exception, and the suite recipe relies on it.** The `Automation`
command splits ITS OWN argument on `;` (`AutomationCommandline.cpp:582`), so
`Automation RunTests VaCuus; Quit` is one `-ExecCmds` command whose two halves the
automation handler runs in order — and that `Quit` is the handler's own subcommand
(`:726`), not the console one. See 4.

**2. `-RenderOffscreen` alone does not give you the resolution you asked for.**
`-resx`/`-resy` are clamped to the monitor's usable size unless `-ForceRes` is present
(`GameEngine.cpp:413-419`), and offscreen there is no monitor to measure — you get a
small default and every pixel assertion you make is about the wrong frame.

**3. Read results from `Saved/Logs/<Project>.log`, not from stdout.** Other engine
processes (the trace server in particular) fork and interleave, and the tail of stdout is
routinely clobbered. The log file is not.

**4. Quit through `Automation`, not beside it.** A console `Quit` as its own `-ExecCmds`
entry (`"Automation RunTests VaCuus, Quit"`) is dispatched at frame 0, before the tests
have even started, and is deferred; after an automation session it typically never fires,
so the process sits idle until something kills it. `Automation`'s own `Quit` is queued
behind the run and exits when the run completes, setting the exit code from the results
(`AutomationCommandline.cpp:490-504`, which also prints `**** TEST COMPLETE. EXIT CODE: N
****`). Measured on the plugin's own suite, same test, same machine: `"…; Quit"` exited by
itself in 23 s with exit code 0; `"…, Quit"` finished the test in 5 s and then idled until
a watchdog killed it at 150 s. If you ever do have to kill a stuck editor, kill it **by PID**.
Do not `pkill -f` a pattern taken from the command line — that pattern also matches the
shell that launched it, and on a build agent it will match the job.

None of this is VaCuus-specific; it is how the engine behaves. It is here because the
plugin's own acceptance runs are driven exactly this way, and because a buyer who cannot
reproduce the pipeline cannot check any claim in `perf-guide.md` against their own machine.

## 5. What to read next

- `gotchas.md` — before your first authoring day; every entry is a recorded finding.
- `rcss-matrix.md` — the exact supported RCSS surface, generated from the vendored
  RmlUi this plugin ships.
- `perf-guide.md` — the budgets, the measured numbers, and the design idioms
  (read the passport's "The re-baseline" before planning a 1,700-node always-animated HUD).

SVG images and Lottie animations are described in [vector assets](vector-assets.md),
including relative resource paths, cooked bundles and CPU rasterization cost.
