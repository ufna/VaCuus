# 2d6demo — client-facing demo project (design)

Date: 2026-08-04
Status: approved by owner
Delivers: a standalone UE C++ project that consumes VaCuus **exactly as an external
buyer receives it**, and builds a three-screen animated sci-fi UI from the client's
Photoshop comps on top of a live 3D scene.

Source comps: `/w/111/2d6_ui/UI_Design_Configuration.psd`,
`/w/111/2d6_ui/UI_Design_Turrets.psd` (canvas 4586×2580 each).

---

## 1. What this demo has to prove

Three things, to three different readers:

1. **To the client** — that their comp renders as a living UI: animation, hover
   response, data that moves, screens that transition.
2. **To an evaluating engineer** — that the delivery path works: take the Fab
   package, drop it into a fresh C++ project, author UI in TSX, ship.
3. **To the owner** — that the plugin's headline economics are real. The demo carries
   a `demo2d6.Calm` toggle that stops ambient motion; `vacuus.M1HUD.PerfLog 1` then
   shows the HUD go to **0 publishes** while the glass keeps sampling the moving scene.

Spectacle is a stated requirement and is **on by default**. The calm mode is a
demonstration switch, not the default posture.

---

## 2. Constraints established by research

Every row was read in the source named. These are hard boundaries on the UI, not
preferences.

### 2.1 Refused or broken CSS

| Feature | What actually happens | Cite |
|---|---|---|
| `box-shadow` | renders an **opaque white rectangle** over the element's own background, and re-runs its texture callback every frame (permanent resource traffic → forced publish) | `GeometryBoxShadow.cpp:235` needs `SaveLayerAsTexture`, unimplemented per `VaCuusCommandBuffer.h:455-462`; white quad at `BoxShadowCache.cpp:56-59` |
| `mask-image` | parses, then **silently does nothing** — element renders unmasked, artwork discarded, no warning | `ElementEffects.cpp:296-311`, `SaveLayerAsMaskImage` also unimplemented |
| `filter:` anything but `blur` | handle 0, one latched warning, effect dropped | `VaCuusRecordingRenderInterface.cpp:866-892` |
| `ease` / `ease-in-out` / `linear` / `cubic-bezier()` | not keywords → falls to the property-name branch → **the whole `transition` declaration is dropped** | `PropertyParserAnimation.cpp:27-77`, `:311-315` |
| `calc()`, `min()`, `max()`, `clamp()` | no parser | — |
| CSS Grid, `position: sticky`, `order` | not registered | — |
| `::before`, `::after`, any pseudo-element | do not exist | `StyleSheetFactory.cpp:15-27` |
| `!important` | not tokenized | — |
| `rgba()` alpha | is **0–255**, not 0–1 | `PropertyParserColour.cpp:277-286` |
| bare attribute selector `[x]{}` | ~2 ms per style resolution | gotchas.md #6 |
| `transition` keyword lookup | **not** lowercased (`animation` is) — `Cubic-Out` works in one and fails in the other | `PropertyParserAnimation.cpp:133` vs `:240` |

### 2.2 Available and load-bearing

| Feature | Note | Cite |
|---|---|---|
| `@keyframes` + `animation` | `from`/`to`/`n%` only; duration must be > 0; unknown name = silent skip | `StyleSheetParser.cpp:403-460`, `Element.cpp:2779-2780` |
| easing families | `back bounce circular cubic elastic exponential linear quadratic quartic quintic sine`, each `-in`/`-out`/`-in-out` | `Tween.h:10` |
| animatable units | `NUMBER_LENGTH_PERCENT ANGLE COLOUR TRANSFORM KEYWORD DECORATOR FILTER` — **decorators and filters interpolate** | `ElementAnimation.cpp:640-648`, `:271-362` |
| full 3D `transform` | translate/scale/rotate/skew/matrix, 3d variants, `perspective` | `PropertyParserTransform.cpp:46-150` |
| decorators (16) | `linear/radial/conic/repeating-*-gradient`, `image`, `tiled-*`, `ninepatch`, `text`, `shader` | `Factory.cpp:196-213` |
| `backdrop-filter: blur()` | composite-time, **outside the publish gate** — 0.011 ms/frame, 0 of 3363 frames published | `VaCuusSlateElement.cpp:179-195`, perf-guide.md:124-128 |
| `font-effect: glow/outline/shadow/blur` | the only working glow; cost is **load-time** (~4.2 ms first Record), not per-frame | `FontEffectGlow.cpp:122-127`, perf-guide.md:114-120 |
| `var()` / `--custom-properties` | fully implemented, inherit, work inside `@keyframes`, writable from JS — **undocumented in the buyer docs** | `ElementStyle.cpp:162-243`, `PropertySpecification.cpp:232-247` |
| flexbox | complete, incl. `gap` and `space-evenly` | `FlexFormattingContext.cpp` |
| `@font-face` | parser dispatches it (`font-family`, `src`, `font-weight`, `font-style`, `fallback-face`, `face-index`) — **used nowhere in the repo; verify first** | `StyleSheetParser.cpp:855`, `:525-564` |
| `@spritesheet` | parsed, consumed by `<img sprite=…>` and tiled decorators — also unused in-repo | `StyleSheetParser.cpp:798-833` |
| `vacuus-interactive` attribute | marks a plain `<div>` as hit-testable in the interactive-region snapshot | `VaCuusInteractiveSnapshot.h:283-303` |

### 2.3 Cost model

- **Publish gate**: a recorded frame is withheld only if *all* of — gate enabled,
  `Generation > 0`, content hash unchanged, **no resource traffic**, no live material
  decorator (`VaCuusRecordingRenderInterface.cpp:1474-1485`). Any running animation
  defeats it. Reference HUD (1,732 nodes, 908 draws) publishes 100% and costs
  **1.077 ms UI thread / 0.403 ms render thread / 0.012 ms game thread** on the
  reference machine; the whole animated HUD costs ~5% of frame rate.
- **Cheap motion**: `transform` and `opacity` — no relayout, no geometry rebuild.
  64 blips: transform pump 0.235 ms vs `left`+`top` 0.379 ms.
- **Expensive**: background/border/`border-radius`/`background-color` changes
  unconditionally `Release` + `MakeGeometry` (`ElementBackgroundBorder.cpp:131-137`)
  → resource traffic → forced publish. **Animated text is the worst**: a recoloured or
  rewritten string never mesh-compares equal, so geometry is released and recompiled
  every frame (`ElementText.cpp:425-428`, `Vertex.h:20-23`). Image `src` swaps per
  frame are worse still.
- **Material decorators** (`shader(<key>)`) force a publish per engine frame while live
  (`:1455-1495`). Use sparingly.
- **Never write a property unconditionally** — `ElementStyle::SetProperty` dirties
  regardless of whether the value changed.
- **JS facade ops**: style set 2.4 µs, `getElementById` 0.09 µs. Model reads are cheap
  individually and expensive in bulk; gate them (§8.3).

### 2.4 Assets

- **No SVG anywhere.** `Source/SVG` was deleted at vendoring
  (`VENDORED_TAG.txt:18`); `Include/RmlUi/SVG/ElementSVG.h` is an orphan header with
  no implementation and no instancer. Upstream's SVG plugin links **lunasvg**
  (`Source/Core/CMakeLists.txt:414`), a third external library the plugin declines to
  ship. And the texture loader would refuse it regardless: `DetectImageFormat` returns
  `Invalid` → null handle (`VaCuusRecordingRenderInterface.cpp:295-300`).
- **Accepted raster formats: PNG, JPEG, four-component UEJPEG.** A deliberate
  whitelist — BMP/DDS/ICO `check()`-crash on a decode **worker**, TGA silently swaps
  red and blue (`:298-380`).
- **Paths are document-relative**; a leading `/` is the only spelling that reaches a
  sibling directory (`SystemInterface.cpp:52-83`).
- **VFS roots**: `<Plugin>/Content/DevUI` first, then `<Project>/Content/DevUI`. A
  project file **cannot shadow** a same-named plugin file (`VaCuusContentPaths.h:26-38`).
- **No public C++ font-loading API.** `Rml::LoadFontFace` is called only from
  `VaCuusEngine.cpp:138-153` (LatoLatin) and the plugin's own lobby demo. A consumer
  project's only route to its own faces is `@font-face` in RCSS — hence §11 R1.

---

## 3. Project layout and plugin delivery

```
/w/Unreal/Demo2d6/                       # own git repo
  Demo2d6.uproject                       # C++ project — mandatory: the plugin ships
                                         # source-only, a BP-only project never compiles it
  Source/
    Demo2d6.Target.cs, Demo2d6Editor.Target.cs
    Demo2d6/
      Demo2d6.Build.cs                   # + "VaCuus", "VaCuusRender"
      Demo2d6.h/.cpp                     # IMPLEMENT_PRIMARY_GAME_MODULE
      Demo2d6Types.h                     # the USTRUCT model tree (§7.1)
      Demo2d6SimSubsystem.h/.cpp         # simulation + model push (§7.2)
      Demo2d6HudActor.h/.cpp             # owns the UVaCuusWidget, routes events (§7.3)
      Demo2d6GameMode.h/.cpp
      Demo2d6Cheats.cpp                  # console commands (§7.4)
  Plugins/VaCuus/                        # ← BuildPlugin -Package output, verbatim
  Content/
    DevUI/2d6/                           # rml, rcss, bundle, img/, fonts/
    Scene/                               # materials, textures, level
    Maps/Demo2d6.umap
  Web/apps/2d6/                          # TSX source (§8)
  README.md                              # the client's runbook
```

**The plugin goes in as SOURCE, not binaries.** A binary drop under a project's
`Plugins/` is recompiled anyway: `RulesCompiler.cs:402` passes
`bReadOnly: Unreal.IsProjectInstalled()` (false on a normal engine), so
`UEBuildBinary.cpp:268`'s precompiled early-out never fires — measured 236 build
actions and ~10 minutes, with the 210 MiB download overwritten. Only an
`Engine/Plugins/Marketplace` install on a Launcher engine accepts delivered binaries.

Production command (from a clean clone; never against the working tree):

```bash
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/RunUAT.sh BuildPlugin \
  -Plugin=<clean-clone>/VaCuus.uplugin \
  -Package=/w/Unreal/Demo2d6/Plugins/VaCuus \
  -TargetPlatforms=Linux -StrictIncludes
bash /w/Unreal/VaCuus/Tools/fab_scan.sh      /w/Unreal/Demo2d6/Plugins/VaCuus
bash /w/Unreal/VaCuus/Tools/fab_inventory.sh /w/Unreal/Demo2d6/Plugins/VaCuus
```

`BuildPlugin` clears `bEnabledByDefault`, so `Demo2d6.uproject` must list the plugin
explicitly — which also converts a dropped-plugin situation from silence into a named
build failure.

---

## 4. The 3D scene

The comps show the UI over rendered ship art. We reproduce that as a **live scene**, so
`backdrop-filter` has something to sample and the panels read as glass rather than as
flat overlays.

- Extract the ship/asteroid/haze artwork from the PSDs at full resolution as PNG,
  import as textures, place on **unlit translucent planes at three depths**.
- Two dressings, one level:
  - **Lobby** — cold, near-black, ship broadside (from `Configuration.psd` `step 1`).
  - **Combat** — warm sepia dust, ship at range, debris (from `Turrets.psd`).
- Slow camera dolly + parallax between planes + a light dust particle layer. Enough
  motion that the blur behind a panel visibly changes; cheap enough to be irrelevant.
- Screen changes blend the camera between framings, timed to the UI transition (§6.3).

Deliberately **not** modelling a ship from primitives: it would not match the comp, and
it is work that demonstrates nothing about the UI plugin.

---

## 5. Design system

### 5.1 Scale

Comp canvas 4586×2580 = 1920×1080 × **2.3887**. All authored dimensions are the PSD
layer bboxes divided by 2.3887, in px, against a 1920×1080 base. Other resolutions get
a single `transform: scale(s)` on the root wrapper, `s = view.height / 1080`, written
from JS on resize only. `calc()` does not exist and `dp == px` here (the plugin never
calls `SetDensityIndependentPixelRatio`), so this is the only cheap route — and
`transform` costs no layout.

### 5.2 Palette

Taken from the designer's own swatch group (hidden layer group `Colors` in
`Turrets.psd`), not eyeballed:

```
--gold           #efcc27      --gold-hot      #ffec1f
--blue           #42718f      --green         #42b27a
--teal           #6f8f7f      --violet        #7a80ba
--red            #d3452d      --orange        #e48979
--surface        rgba(24,24,24,178)        /* alpha is 0-255 here */
--surface-solid  #181818
--stroke         #3a3a38      --stroke-hot    #efcc27
--text           #d8d8d6      --text-dim      #8b8b88
```

All exposed as `--` custom properties on `body`; a theme swap is one
`style.setProperty` call.

### 5.3 Typography

The comps use **Orbitron** (Bold/Medium), **Rajdhani** (Bold/Medium) — both OFL, used
as-is — and **WadikBold** / **AKONY-Bold**, which are not freely licensable. Display
headings substitute **Michroma** (OFL), the closest wide geometric techno face.

| Role | Family | Where |
|---|---|---|
| Display | Michroma | `SECTION`, `HP09 PD TURRET`, `EXTERNAL SUBSYSTEMS`, `DESCRIPTION` |
| UI / labels | Orbitron | nav tabs, panel headers, button captions, gauge labels |
| Data / body | Rajdhani | numbers, stat rows, descriptions, list rows |

Declared via `@font-face` (one rule per face — RCSS `font-family` is a **plain string
parser, not a list**, so a comma-separated fallback is a syntax error that drops the
declaration and leaves the document with no text at all). Faces ship under
`Content/DevUI/2d6/fonts/` with their `OFL.txt`.

### 5.4 Component catalogue (25 types)

Each is one TSX component with typed props plus one RCSS block. This is the
"standardize element types" deliverable.

**Surfaces** — `Panel` (`glass|flat`, optional corner notch, optional bracket corners) ·
`PanelHeader` (`gold|dim`, optional icon) · `ScreenTitle` (code + name, two colours) ·
`Tooltip` (gold title, body, notch) · `ContextMenu` (action list, notch) ·
`DescriptionCard` (title, body, `StatRow[]`, thumbnail)

**Navigation** — `SectionTab` (icon, label, `active`, gold underline) · `TabBar`
(section variant) · `NumberedTab` (`TURRETS /02`) · `SegmentedToggle` ·
`StatusBar` (bottom: buttons, week counter, expedition clock) · `IconRail` (right:
stacked icon buttons, separators, collapse chevron) · `ReturnBar`

**Data readouts** — `DialGauge` (ring + centre number + label) · `VerticalGauge` (heat,
gradient fill, threshold ticks) · `Meter` (horizontal bar, `warn|crit` states) ·
`PipRow` (tier chevrons) · `StatRow` (label ⋯ value) · `Badge` (`B4`, `HP13`,
`NO SLOT`) · `Radar` (rings, sweep, blip pool) · `Slider` (`NORMAL|TACTICAL`, and the
bow/stern divider)

**Collections** — `SlotCard` (`filled|selected|empty|noslot`) · `ItemCard` (turret:
icon, ammo count, ammo bar) · `InventoryGrid` · `MatrixGrid` (small icon grid, attack /
defence turrets) · `CrewList`

Every interactive component that is not `<button>`/`<input>` carries
`vacuus-interactive` so the interactive-region snapshot hit-tests it.

Deliverable alongside the screens: a **catalogue page** (`2d6/catalog.rml`) that renders
every component in every state. It is where states get reviewed and where a regression
is visible in one screenshot.

---

## 6. Animation

### 6.1 Allowed / forbidden

**Use:** `transform`, `opacity`, `@keyframes`, `transition` with `cubic-*`/`sine-*`/
`quadratic-*`/`back-*` easings, gradient decorators (they interpolate),
`backdrop-filter: blur()`, `font-effect: glow|outline`, `var()`.

**Never:** `box-shadow`, `mask-image`, non-blur `filter`, per-frame text rewrites,
per-frame `src` swaps, `left`/`top` for motion, unconditional property writes.

### 6.2 Ambient (on by default)

Pure `@keyframes`, zero script: a scan highlight travelling across panel headers; gold
pip pulse; radar sweep rotation; breathing outline on the selected slot; a slow
gradient drift inside the heat gauge. Each is `transform`/`opacity`/decorator only.

`demo2d6.Calm 1` removes a class on `body` that gates every ambient rule, so the HUD
falls to zero publishes and `PerfLog` proves it. This is the buyer-facing demonstration
of the idle gate.

### 6.3 Transitions

Screen change = staggered exit (panels `translateY` + fade, 40 ms apart) → camera blend
→ staggered enter, ~450 ms total, `cubic-out`. Screens are separate documents on **one
view**; `LoadDocument` replaces the tree, so the exit animation runs first and the load
is issued on its `animationend`.

### 6.4 Data-driven motion

| Readout | Technique | Why |
|---|---|---|
| heat level | `transform: scaleY` on a gradient-filled child | no layout, no geometry rebuild |
| ammo bars | `transform: scaleX` | same |
| dial gauges | `transform: rotate` on an arc element | same |
| radar blips | rAF pool writing `style.transform` | measured 0.235 ms / 64 blips |
| numbers | text written **only when the value changes**; a 0.6 s count-up on screen entry only | animated text = geometry churn every frame |
| state colours | class toggles against pre-declared rules | avoids per-frame colour writes |

---

## 7. Mock data and simulation

### 7.1 Model structs (`Demo2d6Types.h`)

```
FDemo2d6Ship      Name, Week, ExpeditionTime, Heat, HeatCap, Revision
FDemo2d6Section   Id, Label, Integrity(0..100), Power, Tier, Alerts, Revision
FDemo2d6Turret    Id, Class(Railgun|Ballistic), Mount(Bow|Stern), Ammo, AmmoMax,
                  State(Ready|Reload|Repair|Destroyed), Heat
FDemo2d6Slot      Category(Main|Second|Bay|Shield), State(Filled|Empty|NoSlot), ItemId
FDemo2d6Item      Id, Name, Desc, ReloadTime, Accuracy, Range, Thumb
FDemo2d6Contact   Id, X, Y, Kind
FDemo2d6Notice    Id, Severity, Text
FDemo2d6State     Ship, Sections[7], Turrets[], Slots[], Items[], Contacts[], Notices[],
                  + one int32 Revision per domain
```

`FString` for `BindModel`'s model name, `FName` everywhere after — an `FName` loses
exact case in a cooked build and `data-model` matches byte-for-byte
(`VaCuusView.h:300-311`).

### 7.2 `UDemo2d6SimSubsystem` (`UWorldSubsystem`)

Ticks the state: firing and ammo drain, reload timers, heat rise and dissipation,
section integrity drift and repair, radar contacts entering and leaving, notices
arriving. Pushes with `UpdateModel` once per frame from `FCoreDelegates::OnBeginFrame`
— *before* `UVaCuusSubsystem::Tick` publishes. Bumps only the revisions whose domain
changed.

### 7.3 `ADemo2d6HudActor`

Owns the `UVaCuusWidget` (held in a `UPROPERTY` — nothing else roots it), calls
`BindModel` **before** `LoadDocument` (RmlUi resolves `data-model` exactly once, in
`Element::SetParent`), sets `FInputModeGameAndUI` + `SetShowMouseCursor(true)` (without
it the viewport holds capture and **no input reaches the UI at all**), and keeps the
widget `EVisibility::Visible` (anything else turns the document into a picture).

Subscribes `OnJsEvent` through a `UFUNCTION` adapter: `nav`, `select_slot`,
`turret_action`, `toggle_power`. Each mutates the simulation; the change reaches the UI
on the next `UpdateModel`. One-way data flow with an explicit ask.

### 7.4 Console commands

`demo2d6.Heat <0..100>` · `demo2d6.Damage <section> <amount>` · `demo2d6.Wave` (spawn
contacts) · `demo2d6.Notice <text>` · `demo2d6.Calm <0|1>` · `demo2d6.Screen <lobby|section|turrets>`.

---

## 8. TSX application

### 8.1 Layout

```
Web/apps/2d6/
  package.json            { entry: src/main.tsx, outDir: 2d6, bundleName: 2d6_bundle.js }
  src/
    main.tsx              module-scope render into an empty #mount div
    tokens.ts             typed mirror of the RCSS custom properties
    model.ts              revision-gated model access (§8.3)
    components/           the 25 catalogue components
    screens/              Lobby.tsx, Section.tsx, Turrets.tsx, Catalog.tsx
```

Built with the **plugin's** CLI against the project's content tree:

```bash
cd /w/Unreal/Demo2d6/Plugins/VaCuus/Web && npm install
node packages/cli/bin/vacuus.mjs build \
  --app /w/Unreal/Demo2d6/Web/apps/2d6 \
  --devui-dir /w/Unreal/Demo2d6/Content/DevUI
```

This is the buyer's documented loop, so the demo exercises it.

### 8.2 Known CLI defect to work around

`Web/packages/cli/template/app.rml.tpl:8` emits
`<script src="__APP_DIR__/__APP_NAME___bundle.js">`, and `src` is document-relative, so
from `Content/DevUI/2d6/2d6.rml` it resolves to `2d6/2d6/2d6_bundle.js` — one Error,
styling with no behaviour. Our documents write the bare filename.

### 8.3 Revision-gated model access

`vacuus.model(name)` mints a fresh object per call and has **no subscription** — the
only read path is polling. Reading ~200 leaves every frame is avoidable waste, so
`model.ts` reads the per-domain `Revision` integers in one rAF and re-reads a domain's
leaves only when its revision moved. Components subscribe per domain.

### 8.4 Facade rules the components must respect

- `style` refuses bare numbers — write `"100px"`.
- `classList` never touches the `class` attribute; `[class…]` selectors would lie.
- `preventDefault()` also stops propagation.
- `querySelector` never matches self; `closest` starts at the parent.
- `document.body === document`; mount into a dedicated empty `<div>`.
- No `focusin`/`focusout`; use `onFocus`/`onBlur`.
- Never feed user data to `dangerouslySetInnerHTML` — `innerRML` is an RML re-parse and
  `{{…}}` becomes a data expression. Text children are safe.
- No C++ tree surgery under the preact mount — it kills every wrapper silently.

---

## 9. Asset extraction

From the PSDs, via `psd-tools` (layer-tree aware, respects group structure and
visibility), export to PNG at 2× the 1080p size then downsample:

- section icons (7), rail icons (~8), action icons (~13), category icons (4)
- turret / weapon glyphs (railgun, ballistic, attack, defence)
- slot thumbnails (main turret, second turret, bay, shield emitter — several each)
- ship silhouettes (3, for the AKIRO overview strip)
- scene plates (ship broadside, ship at range, asteroids, dust)

Individual PNGs, not a `@spritesheet`: upload cost is one-time
(256² = 0.071 ms) and the sprite path is unexercised in the repo. Rings, dials, notches
and bracket corners are **not** exported — they are drawn with gradient decorators and
borders, which costs no texture at all.

---

## 10. Screens

### 10.1 Lobby (`Configuration.psd` `step 1`)

Top `TabBar` of 7 `SectionTab`s (HABITAT · SCIENCE · PROPULSION · REACTOR · SALVAGE ·
GRAVI ENG · HANGAR) with the active tab gold-filled and underlined; ship hero over the
live scene; right `IconRail`; bottom `StatusBar` (NOTICE / MENU / MAP / pause, week
counter, expedition clock). Clicking any section tab → Section screen for that section.

### 10.2 Section (`Configuration.psd` `step 2`)

`ScreenTitle` "SECTION"; `SegmentedToggle` EXTERNAL / INTERNAL; a large `Panel`
"EXTERNAL SUBSYSTEMS" holding four category groups (MAIN TURRETS · SECOND TURRETS ·
BAYS · SHIELD EMITTERS), each a `PanelHeader` plus a horizontal strip of `SlotCard`s in
all four states; a floating `ContextMenu` (SELECT / DELETE / REPAIR) on a selected slot;
`ReturnBar` bottom-left; shared chrome. Camera pushes in on the ship.

### 10.3 Turrets HUD (`Turrets.psd`)

Combat HUD over the warm scene:

- top-left: `VerticalGauge` HEAT + a six-row `StatRow` SUBSYSTEMS list
- top-right: AKIRO overview `Panel` (three ship silhouettes) + seven `DialGauge`s with
  `PipRow` and power toggles (HABITAT … HANGAR)
- `NumberedTab` bars (SHIP /01 · TURRETS /02 · BAYS /03 · SUBSYSTEMS /04 · SALVAGE /05)
- two action icon rows
- bottom-left turret grid: RAILGUNS / BALLISTIC sub-tabs, `ItemCard` rows split
  bow/stern by a `Slider` divider, each with ammo count and `Meter`
- `MatrixGrid` attack + defence turrets; `CrewList` (D.C. CREW) with its own heat bar
- `Radar` with rings, sweep and blips, and a NORMAL/TACTICAL `Slider`
- `Tooltip` and `ContextMenu` (REPAIR / RELOAD) on hover/press
- shared `IconRail` + `StatusBar`

---

## 11. Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | **`@font-face` is unexercised in the repo.** Parser support is real (`StyleSheetParser.cpp:855`, `:525-564`) but nothing has ever loaded a face through it, and there is no public C++ fallback API. | Verify in step 1, before any layout work. If it fails: file a bead, and fall back to LatoLatin for the whole comp — a visible fidelity loss the owner must sign off. |
| R2 | Ambient animation publishes 100% of frames. | Measured and acceptable (1.077 ms UI thread, off the game thread, ~5% frame rate on a 1,732-node reference). `demo2d6.Calm` proves the gate still works. Keep node count in the reference band. |
| R3 | The TSX-error trap: a failed `vacuus dev` build writes no bundle, so no reload fires and the engine keeps the last good UI with **no in-engine indication**. | Watch the dev terminal; the README says so in bold. |
| R4 | `-ExecCmds` splits on **commas**, swallows every later argument, and the launcher re-appends `-game` at the end. | Always end the value with a comma. |
| R5 | A running editor holds the `.so`; `pkill -f` kills the calling shell. | Kill by PID only. |
| R6 | `BuildPlugin`'s editor leg rewrites `UnrealEditor.modules` and can resurrect stale platform modules → `exit 127` with the message on **stderr**, not in the log. | Compare `.so` mtimes against `libUnrealEditor-Core.so`; delete stale ones and the manifest. |
| R7 | Comp fidelity depends on art extracted from client PSDs. | Extraction is scripted and re-runnable; every asset traceable to a layer path. |

---

## 12. Out of scope

Bays / Subsystems / Salvage screens (`step 3` item detail, and the other four PSDs);
gameplay; save/load; localization beyond wiring `vacuus.translate`; Windows and macOS
packaging; audio.

---

## 13. Verification

- `@font-face` proof (R1) before layout.
- Component catalogue screenshot, every state.
- Per-screen headless screenshots:
  `-ExecCmds="demo2d6.Screen turrets,vacuus.M1HUD.AutoShot 10,"` — note the trailing
  comma, and that `AutoShot N` fires after `max(N,3)` **recorded** frames.
- `vacuus.M1HUD.PerfLog 1` numbers for ambient-on and `demo2d6.Calm 1`, reported side
  by side.
- Editor **and** monolithic game target both build.
- `fab_scan.sh` + `fab_inventory.sh` clean on the delivered plugin package.
