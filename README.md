VaCuus
======

HTML/CSS user interface for Unreal Engine, rendered **off the game thread**.

You author screens as `.rml`/`.rcss` documents — the web languages, not a UMG graph — and
the plugin runs them on its own UI thread: layout, styling and draw-command recording all
happen away from the game thread, which only enqueues input and reads back a snapshot.
Optional JavaScript (QuickJS) and a TypeScript/Preact workflow sit on top. Rendering goes
through the engine's RHI into a persistent render target that Slate composites, so a
document can be a full-screen HUD or a panel on a quad in the world.

> **Status: released, 1.0. Engines: UE 5.6, 5.7 and 5.8.** All three are built and
> tested from this one source tree — editor and packaged-game targets, plus the
> 227-test automation suite that ships with the plugin and that you can run yourself
> (below). Every shipping platform is supported: Windows, macOS, Linux, Android and
> iOS; consoles on request. A package is stamped with the engine that built it, so take
> the download that matches yours (see [`docs/buyer/setup.md`](docs/buyer/setup.md)).
> The API is stable — breaking changes are reserved for major versions and carry
> release notes.

## Read these, in this order

| Page | What it answers |
| --- | --- |
| [`docs/buyer/setup.md`](docs/buyer/setup.md) | **Start here.** What is in the package, what your project needs, first document, and how UI ships in a cooked build. |
| [`docs/buyer/gotchas.md`](docs/buyer/gotchas.md) | Every recorded surprise, numbered. Read before your first authoring day — most of them cost someone a debugging session already. |
| [`docs/buyer/rcss-matrix.md`](docs/buyer/rcss-matrix.md) | The exact supported RCSS surface, generated from the vendored RmlUi this plugin ships. RCSS is not CSS; this page is the difference. |
| [`docs/buyer/perf-guide.md`](docs/buyer/perf-guide.md) | The budgets, the measured numbers, and the idioms that keep a HUD inside them. |
| [`docs/buyer/ai-guide.md`](docs/buyer/ai-guide.md) | **If an AI agent writes your UI.** The whole interface is plain text, which suits agents well — but most mistakes here produce no error and a screen that renders, and an agent cannot see the screen. This page is what to brief it with. `AGENTS.md` beside this file is the short version an agent finds on its own. |

**Using Claude Code, Codex, Cursor or Copilot?** One line in your project's own
`CLAUDE.md` / `AGENTS.md` is enough to point it at the right pages:

```markdown
UI is VaCuus (HTML/CSS off the game thread). Before touching anything under
Content/DevUI, read Plugins/VaCuus/docs/buyer/ai-guide.md and
Plugins/VaCuus/docs/buyer/rcss-matrix.md.
```

## Install, in one paragraph

**From Fab**: the launcher installs the plugin **into the engine, not into your project**
— Epic builds the binaries for code plugins, so nothing compiles on your machine and a
**Blueprint-only project works**. Enabling it is the whole install. **From source** (this
repository): your project must be a C++ project — a Blueprint-only project has no
toolchain to compile it with (converting takes two minutes: Tools → New C++ Class →
None). Drop the folder into `<YourProject>/Plugins/VaCuus`, so that `VaCuus.uplugin`
lands at `<YourProject>/Plugins/VaCuus/VaCuus.uplugin`, and the first editor launch
compiles the modules once. Either way, enable it in `Edit → Plugins` or in your
`.uproject`:

```json
"Plugins": [
    {
        "Name": "VaCuus",
        "Enabled": true
    }
]
```

The package root *is* the plugin folder — there is no extra directory level to strip.
`setup.md` §0 has the full version of this, including the two ways a shipped copy differs
from the repository.

**Verify it works**: the console knows `vacuus.M2Demo` — run it and an interactive demo
document appears. `Automation RunTests VaCuus` runs the suite; the test source and its
fixtures ship on purpose.

**A symlink instead of a copy works at runtime but breaks the build**: Unreal Build
Accelerator's file detours see the symlinked path and the real path as two different files
and abort renaming an object file (`cross-process rename-while-open`). To keep the plugin
outside the project, use `AdditionalPluginDirectories` in your `.uproject` instead — and
point it at a **parent** of the plugin directory, never at the plugin directory itself.

## What is in the package

```
VaCuus.uplugin              Plugin descriptor
Content/DevUI/              The RML/RCSS documents the plugin ships, demos and test fixtures
Resources/Icon128.png       Icon shown in the editor plugin browser
Shaders/                    The UI shaders, including the scene-colour backdrop path
Source/ThirdParty/RmlUi/    Vendored RmlUi 6.x — bit-exact upstream plus recorded patches
Source/ThirdParty/quickjs-ng/  Vendored QuickJS-ng, the JavaScript runtime
Source/VaCuusRml/           The vendored library, built as a module (Runtime, Default)
Source/VaCuus/              Runtime core: the UI thread, VFS, subsystem, views (Runtime, Default)
Source/VaCuusRender/        Record -> RHI replay -> Slate composite, UMG widget (Runtime, PostConfigInit)
Source/VaCuusJs/            The JavaScript host and the DOM facade (Runtime, Default)
Source/VaCuusEditor/        Live reload and the bundle factory (Editor, PostEngineInit)
Web/                        Optional TypeScript/Preact workflow, source-only (npm-install it yourself)
docs/buyer/                 The four pages above
```

There is no `Binaries/` directory in the package and that is Fab's rule for sellers of
UE-facing code: what we upload is source. The binaries a Fab install carries are Epic's
build of that source, and they land in the engine — a source install compiles them on
your machine instead.

Every RmlUi call happens on the UI thread; the game thread only enqueues commands and reads
a published interactive-region snapshot. Runtime code logs under the `LogVaCuus` category
declared in `Source/VaCuus/Public/VaCuusDefines.h` — that is the category to raise to
`Verbose` when something is quiet.

## Third-party components

Both are vendored in-tree with their licenses beside them, and both are MIT:

- **RmlUi 6.x** — `Source/ThirdParty/RmlUi/LICENSE.txt`. The document, layout and styling
  engine. Patches applied to it are recorded rather than silent; see `VENDORED_TAG.txt`.
- **QuickJS-ng** — `Source/ThirdParty/quickjs-ng/LICENSE`. The JavaScript runtime, with its
  symbols deliberately kept out of the module's export table.

The plugin's own source is licensed under the **Business Source License 1.1**
([`LICENSE.md`](LICENSE.md)): free to read, modify and use outside production, free in
production for noncommercial purposes, and each version converts to MIT four years after
release. **Commercial production use needs a purchased license** — on Fab (under the Fab
EULA) or directly from the author; [`COMMERCIAL.md`](COMMERCIAL.md) is the plain-language
version. A copy bought on a store is governed by that store's terms. Contributions
require the CLA ([`CLA.md`](CLA.md)).

## Support

Issues and questions: <https://github.com/ufna/VaCuus/issues>.

---

*Working on the plugin itself rather than with it?* The host-project layout, the
two-working-trees hazard and the arrangements that do and do not remove the duplication are
in [`docs/dev/host-project.md`](docs/dev/host-project.md), which is repo-only.
