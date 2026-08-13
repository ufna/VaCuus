# Fab listing — copy, technical fields, and the decisions behind them

Drafted 2026-08-12 for bead VaCuus-jne; updated 2026-08-13 for the 1.0 release. Voice
matches the shipped docs: every number below traces to `docs/buyer/perf-guide.md`, the
README, or the descriptor — nothing is promised that a buyer cannot re-run. Assets live
in `docs/fab/gallery/`.

---

## Short description

> Build your game's UI in HTML and CSS, rendered entirely off the game thread. Screens
> are plain-text files with live reload, data binds straight from your UPROPERTYs — and
> the whole surface is one an AI coding agent can read, write and verify.

---

## Long description

VaCuus renders your game's interface from HTML and CSS documents — menus, HUDs and
in-world panels — on its own UI thread. Layout, styling and draw-command recording all
happen away from the game thread, which only enqueues input and reads back a snapshot:
at the plugin's own 1,732-node reference HUD, that costs the game thread about 0.01 ms
per frame. Measured, not estimated — the perf guide with the full budget table ships in
the package, and every number in it is re-runnable on your machine.

**Plain text, live reload.** A screen is an `.rml` document plus `.rcss` styles — files
on disk, not editor assets. There is no graph to wire and no recompile: the editor
watches your `Content/DevUI` and reloads a changed document during PIE, and
`vacuus.ReloadUI` does the same at runtime. Changes diff, review and merge like the rest
of your source.

**Built for AI agents.** Because the whole UI surface is text, an agent can read and
write it directly — and everything an agent needs ships in the box: `AGENTS.md` at the
package root (agents find it on their own), a full agent guide, and a support matrix of
the style language generated from the exact engine in the package, so the agent checks
ground truth instead of guessing from web habits. The failure mode this design answers:
an agent cannot see the screen — so parse errors reach the Unreal log with file and line
in every configuration including Shipping, screens render headless at 1920×1080 for
pixel checks, and the 227-test automation suite ships in the package, source and
fixtures included. Onboarding is one line in your project's own CLAUDE.md / AGENTS.md.

**Engine-native data.** Data models bind straight from your `UPROPERTY` fields — no
mirror struct, no serialization layer. Two host classes, both usable from Blueprint and
C++: a UMG widget for screen space and a world component for panels on quads in the
world. JavaScript (QuickJS, vendored) is optional; a TypeScript/Preact workflow sits on
top when you want it.

**Why not a WebView?** Embedding a browser buys the whole web platform at a game's
expense: hundreds of megabytes of payload and RAM, several extra processes, added input
latency across the process boundary, and a permanent security-update treadmill. VaCuus
keeps the part game UI actually uses — documents, stylesheet-driven animation,
data-driven updates — in a small in-process core measured in single-digit megabytes,
running on its own thread. If you need to render the actual web (arbitrary sites, video,
WebGL), a browser is the right tool and this plugin is not one.

**What it is not.** Not a browser: no `<a href>` navigation, no `fetch`, no CSS Grid —
navigation is your host calling `LoadDocument`, layout is flexbox and absolute
positioning. The style language is RCSS, a deliberate subset with its own additions, and
the generated support matrix in the docs is its exact boundary. The full source ships in
the package; a Fab install arrives with Epic-built binaries in your engine, so
Blueprint-only projects work out of the box.

**Status — 1.0, released.** UE 5.6, 5.7 and 5.8, built and packaged from one source
tree. Every shipping platform is supported: Windows, macOS, Linux, Android and iOS all
build, cook and render, and mobile input is wired — touch-drag scrolling with native
inertia and the platform on-screen keyboard for text fields (verified by hand on an
iPhone 11 Pro), with full IME on desktop. Consoles on request. The API is stable:
breaking changes are reserved for major versions and carry release notes. The docs
still state every known limitation by name — read them before you buy at
https://vacuus.ufna.dev

Documentation: https://vacuus.ufna.dev · Issues and questions:
https://github.com/ufna/VaCuus/issues

---

## Technical information (Fab form fields)

**Features:**
- UI runs on a dedicated thread: game thread pays ~0.01 ms/frame even at the 1,732-node
  reference HUD (measured; budget table ships in the package)
- HTML/CSS authoring (`.rml`/`.rcss`, vendored RmlUi 6.x) with live reload in PIE and at
  runtime — no editor asset, no recompile
- Data binding straight from `UPROPERTY` fields, Blueprint- and C++-friendly
- Screen-space UMG widget and in-world panel component
- Optional JavaScript (vendored QuickJS-ng) and a TypeScript/Preact workflow
- Ships for AI-agent workflows: `AGENTS.md` at package root, agent guide, generated RCSS
  support matrix, log-first diagnostics, headless render recipe
- 227-test automation suite ships in the package (source and fixtures, on purpose)
- Localization support incl. live language switch; IME on desktop platforms
- Cross-platform from one source tree: Windows, macOS, Linux, Android and iOS
  supported; consoles on request

**Code modules:**
- `VaCuusRml` (Runtime) — vendored RmlUi 6.x built as a module
- `VaCuus` (Runtime) — UI thread, VFS, subsystem, views, input, IME
- `VaCuusJs` (Runtime) — JavaScript host and DOM facade
- `VaCuusRender` (Runtime) — command recording, RHI replay, Slate composite, UMG widget
- `VaCuusEditor` (Editor) — live reload, bundle factory

**Number of C++ classes:** 10 UCLASSes (the Blueprint-facing surface); the C++ API
beneath is larger.
**Number of Blueprints:** 0 — the plugin is code plus plain-text UI documents.
**Supported engine versions:** 5.6, 5.7, 5.8
**Supported development platforms:** Win64, Mac, Linux
**Supported target build platforms:** Win64, Mac, Linux, Android, iOS
**Documentation:** https://vacuus.ufna.dev
**Example/demo content:** yes — `Content/DevUI` ships interactive demos
  (`vacuus.M2Demo`, `vacuus.RefHud`, decorator and glass galleries) plus the test
  fixtures.
**Important/additional notes:** Third-party components are vendored in-tree with their
MIT licenses beside them: RmlUi 6.x and QuickJS-ng. Full source included.

**Category:** Code Plugins → User Interface (HUD & UI)
**Suggested tags** (Fab allows a limited set — priority order): `ui`, `hud`, `html`,
`css`, `menu`, `user interface`, `data binding`, `javascript`, `typescript`,
`ai`, `middleware`, `performance`

---

## Decisions (settled) and the one thing left open

1. **Engine versions — decided 2026-08-13: the listing claims 5.6, 5.7 and 5.8.**
   Runtime is tested on all three from one source tree (owner runs, 2026-08).
   **Pre-publish gate:** bead VaCuus-93v is still open — `BuildPlugin`, the packaging
   step, has only ever run on 5.8, so the 5.6 and 5.7 *packages* are not yet proven
   end-to-end. Run those two legs (procedure: `docs/passport/2026-08-vacuus-shim1.md`)
   and close 93v before the page goes live; the copy above is written for the state
   after that run.
2. **Release presentation — decided 2026-08-13: 1.0, not beta.** `VersionName "1.0"`
   and `IsBetaVersion: false` in `VaCuus.uplugin`, so the buyer's plugin browser shows
   no pre-release warning, and the status paragraph says the API is stable. Known
   limitations stay documented by name rather than implied by a beta flag (bead
   VaCuus-cob covers what the manual matrix still cannot show).
3. **License — decided 2026-08-12: BUSL 1.1.** Public source under Business Source
   License 1.1 (`LICENSE.md`): non-production use free for everyone, noncommercial
   production free (Additional Use Grant), commercial production = purchased license,
   each version → MIT after four years. Fab channel is covered by the Fab EULA;
   direct sales use `COMMERCIAL.md`. Contributions gated by `CLA.md`. **Price** remains
   the owner's open call. Listing copy may say: "Full source available — free for
   noncommercial use; commercial use via this listing or a direct license."
4. **The MarketplaceURL loop.** After the listing exists: set the Fab product URL in
   `VaCuus.uplugin` → `MarketplaceURL`, re-run `Tools/fab_package.sh` so the shipped
   descriptor carries it (the script WARNs on the empty field today, deliberately).
