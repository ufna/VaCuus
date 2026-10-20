# Third-party notices

VaCuus itself is licensed to you under the terms you bought it on. This page covers the
third-party software **inside the package**, and it is a summary — the authoritative text
of each licence is the file named in its row, which ships beside the code it covers.

The components below use MIT, BSD, FreeType License, Unicode License V3, SIL OFL 1.1
and public-domain grants. Keep the listed copyright and licence notices with the
code, data and assets when redistributing them; their full texts ship in the package.

## Vendored source — compiled into the plugin's modules

| Component | Licence | Full text in the package |
| --- | --- | --- |
| **RmlUi** — the HTML/CSS engine, 6.3, vendored at upstream `ba95ffe` | MIT © 2008–2014 CodePoint Ltd, Shift Technology Ltd and contributors; © 2019–2026 The RmlUi Team and contributors | `Source/ThirdParty/RmlUi/LICENSE.txt` |
| ↳ RmlUi's own bundled libraries (`robin_hood` 3.11.5 unordered map & set, `itlib` 1.12.3) | MIT, each stated separately | `Source/ThirdParty/RmlUi/Include/RmlUi/Core/Containers/LICENSE.txt` |
| ↳ RmlUi Debugger font assets (*Courier Prime Code*, regular and italic) | SIL Open Font License 1.1 | `Source/ThirdParty/RmlUi/Source/Debugger/LICENSE.txt` |
| **quickjs-ng 0.17.0** — the JavaScript engine | MIT © 2017–2026 Fabrice Bellard; © 2017–2024 Charlie Gordon; © 2023–2026 Ben Noordhuis; © 2023–2026 Saúl Ibarra Corretgé | `Source/ThirdParty/quickjs-ng/LICENSE` |
| ↳ Unicode data tables used by quickjs-ng | Unicode License V3 © 1991–2026 Unicode, Inc. | `Source/ThirdParty/quickjs-ng/libunicode-table.h` (notice at the top) |
| **LunaSVG 3.5.0** — SVG rendering | MIT | `Source/ThirdParty/lunasvg/LICENSE` |
| ↳ **PlutoVG 1.3.3**, including embedded stb image/font code | MIT and the grants in each stb header | `Source/ThirdParty/lunasvg/plutovg/LICENSE`; notices in `source/plutovg-stb-*.h` |
| ↳ FreeType-derived rasterizer and stroker | FreeType License | `Source/ThirdParty/lunasvg/plutovg/FTL.TXT` and notices in the `plutovg-ft-*` sources |
| **rlottie** — Lottie rendering, pinned at upstream `ea06d2f` | MIT, with imported components under their own notices | `Source/ThirdParty/rlottie/COPYING` and `Source/ThirdParty/rlottie/licenses/` |
| ↳ rlottie vector routines, rasterizer, image decoder and JSON parser | MIT, BSD and FreeType License; embedded zip/miniz grants remain in their headers | `licenses/COPYING.MIT`, `COPYING.SKIA`, `COPYING.FTL`, `COPYING.PIX`, `COPYING.STB`, `COPYING.RPD` under `Source/ThirdParty/rlottie/`; notices in `src/lottie/zip/` |

The vector rasterizers contain software based in part on the work of the FreeType
Team. These are selected, renamed rasterizer/stroker routines; font loading still
links the engine's FreeType2 library as described below. LunaSVG and rlottie are
modified for static embedding and VaCuus resource loading; their `VENDORED_TAG.txt`
files describe each local change.

**RmlUi is modified.** Local patches are applied on top of the vendored commit; each
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
| **Preact 11.0.1** — re-exported by the `preact-vacuus` package | MIT © 2015–present Jason Miller | `Web/packages/preact-vacuus/PREACT-LICENSE` |

The `Web/` tree ships as source with no `node_modules`; anything you install to build it is
yours to account for, not ours.

## Linked from the engine, not shipped here

**FreeType2**, **zlib** and **libPNG** are used by the font engine, and they come from
Unreal Engine's own third-party tree via `AddEngineThirdPartyPrivateStaticDependencies`
(`Source/VaCuusRml/VaCuusRml.Build.cs`). No copy of them is in this package, and their
notices are Epic's, already present in your engine installation.
