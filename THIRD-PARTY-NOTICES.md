# Third-party notices

VaCuus itself is licensed to you under the terms you bought it on. This page covers the
third-party software **inside the package**, and it is a summary — the authoritative text
of each licence is the file named in its row, which ships beside the code it covers.

Nothing here restricts shipping a game that uses this plugin. Every licence below is MIT
or SIL OFL 1.1; all of them require that the copyright notice and licence text travel with
redistributed **source**, which is why those files are in the package and why they should
stay there if you vendor the plugin into your own tree.

## Vendored source — compiled into the plugin's modules

| Component | Licence | Full text in the package |
| --- | --- | --- |
| **RmlUi** — the HTML/CSS engine, vendored at upstream `0ae381e` | MIT © 2008–2014 CodePoint Ltd, Shift Technology Ltd and contributors; © 2019–2026 The RmlUi Team and contributors | `Source/ThirdParty/RmlUi/LICENSE.txt` |
| ↳ RmlUi's own bundled libraries (`robin_hood` unordered map & set, `itlib`) | MIT, each stated separately | `Source/ThirdParty/RmlUi/Include/RmlUi/Core/Containers/LICENSE.txt` |
| ↳ RmlUi Debugger font assets (*Courier Prime Code*, regular and italic) | SIL Open Font License 1.1 | `Source/ThirdParty/RmlUi/Source/Debugger/LICENSE.txt` |
| **quickjs-ng** — the JavaScript engine | MIT © 2017–2026 Fabrice Bellard; © 2017–2024 Charlie Gordon; © 2023–2026 Ben Noordhuis; © 2023–2026 Saúl Ibarra Corretgé | `Source/ThirdParty/quickjs-ng/LICENSE` |

**RmlUi is modified.** Four local patches are applied on top of the vendored commit; each
is described, with its reason and its named regression test, in
`Source/ThirdParty/RmlUi/VENDORED_TAG.txt`. MIT permits the modification and requires only
that the notice above travel with it, which it does. If you re-vendor, read that file
first — a patch silently lost re-introduces a defect that is invisible on screen.

## Assets

| Asset | Licence | Full text in the package |
| --- | --- | --- |
| **Lato** (`LatoLatin-Regular.ttf`) — the fallback font the plugin loads when a document names no face | SIL Open Font License 1.1 © 2011–2015 tyPoland Lukasz Dziedzic, Reserved Font Name "Lato" | `Content/DevUI/fonts/OFL.txt` |

The rest of the demo content under `Content/` is VaCuus's own.

## Web workflow (optional, `Web/`)

| Component | Licence | Full text in the package |
| --- | --- | --- |
| **Preact** — re-exported by the `preact-vacuus` package | MIT © 2015–present Jason Miller | `Web/packages/preact-vacuus/PREACT-LICENSE` |

The `Web/` tree ships as source with no `node_modules`; anything you install to build it is
yours to account for, not ours.

## Linked from the engine, not shipped here

**FreeType2**, **zlib** and **libPNG** are used by the font engine, and they come from
Unreal Engine's own third-party tree via `AddEngineThirdPartyPrivateStaticDependencies`
(`Source/VaCuusRml/VaCuusRml.Build.cs`). No copy of them is in this package, and their
notices are Epic's, already present in your engine installation.
