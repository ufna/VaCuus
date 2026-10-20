# Marketing art: what exists, where it comes from, how to re-make it

Repo-only. Every image below is derived from something in this repository, and every one that
states a fact says where the fact lives — the rule the epic (VaCuus-gmv) set after the 227-test
badge outlived the number it showed.

## Brand — `docs/marketing/brand/`

| File | What | Made by |
| --- | --- | --- |
| `vacuus-wordmark-dark.svg` | "VaCuus" for dark backgrounds — ink `#e7edf5`, C `#ffb43c` | `Tools/gen_brand_svg.py` |
| `vacuus-wordmark-light.svg` | the same for light backgrounds — ink `#14181d`, C `#d98c14` | `Tools/gen_brand_svg.py` |
| `vacuus-mark.svg` | square "Vc" mark on the favicons' dark tile | `Tools/gen_brand_svg.py` |

All three are Archivo outlines, shaped with HarfBuzz from `site/public/fonts/archivo-latin.woff2`
at the settings the site's CSS uses for the live wordmark (`.vc-wordmark`: wght 800,
font-stretch 116%, letter-spacing −0.035em). The mark is set at wght 900 / wdth 75 to match the
favicons it was measured from. Why outlines rather than `<text>`: an `<img>`-loaded SVG cannot
fetch a web font, so live text would render in the viewer's fallback sans. The script's header
has the rest.

```bash
pip install fonttools brotli uharfbuzz     # once, in any venv
python3 Tools/gen_brand_svg.py
```

**Exported from the mark:** `Resources/Icon128.png`, the icon Edit → Plugins shows (it ships in
the package; replaced the older cyan "Va" circle on 2026-10-12):

```bash
rsvg-convert -w 128 -h 128 docs/marketing/brand/vacuus-mark.svg -o Resources/Icon128.png
# then TinyPNG, docs/dev/site-images.md
```

The site favicons (`site/public/favicon-*.png`, `apple-touch-icon.png`) are this same design,
rendered earlier from live text; they have not been re-exported from the SVG.

The site's `site/public/og.jpg` uses the shared thumbnail layout with the current test count
from `site/.vitepress/theme/Home.vue`. Render it with `Tools/gen_site_card.mjs`, then resize
and crop it and compress it through TinyPNG as required by `docs/dev/site-images.md`:

```bash
# Serve the repository root on port 8765; Playwright and Chromium must be installed.
node Tools/gen_site_card.mjs /tmp/vacuus-site-card.png
magick /tmp/vacuus-site-card.png -filter Lanczos -resize 1200x675 -crop 1200x630+0+22 +repage -quality 88 /tmp/vacuus-site-card.jpg
# TinyPNG shrink the JPEG into site/public/og.jpg before committing.
```

`PLAYWRIGHT_MODULE` and `CHROMIUM_EXECUTABLE` can select an existing local installation.
`CARD_ORIGIN` can select a different local server. The generator changes only the numeric
badge in the rendered DOM, so the Fab listing's source retains its own published count.

## README images — `docs/marketing/readme/`

| File | What | Made by |
| --- | --- | --- |
| `threads.svg` | one UI frame across the three threads | hand-written; restates CLAUDE.md "Architecture Overview" |
| `refhud.webp` | the hero, full frame | `Tools/gen_readme_shots.sh` |
| `m2demo.webp`, `world.webp`, `glass.webp`, `deco.webp`, `vectors.webp` | feature-row crops | `Tools/gen_readme_shots.sh` |

Sources are the site's 1920×1080 headless captures in `site/public/shots/`; the feature rows
link to those full frames. The crops exist because a README table cell is ~445 px wide: the
full frame shrank the UI in `m2demo` and `glass` to texture. Crop boxes are in the script.

**Relative paths, WebP.** README.md links every image by relative path, so it renders on any
branch with no deploy step. Rasters are WebP rather than PNG because `*.png` is a Git LFS
pattern here and LFS bills each download — and a README image is downloaded on every view;
`*.webp` stays an ordinary blob (all five together: 0.21 MB). The cost is in the package:
README.md ships there (`Config/FilterPlugin.ini`, `/README.md`) and `docs/marketing/` does not
(only `docs/buyer/` ships, `Tools/fab_inventory.sh`), so the packaged copy shows alt text.

The hero caption's "1,732 live nodes" and the "Measured, not promised" table are quoted from
`docs/buyer/perf-guide.md` (the budget table and the idle-gate section) and change only when
that page does. The status blockquote is the release-time edit, as before; the release badge
reads the latest GitHub release by itself, and the Unreal Engine badge carries the engine list,
so a new engine means editing the badge too.

## GitHub social preview — `docs/marketing/social-preview.png`

1280×640, the size GitHub asks for (`og.jpg` is 1200×630 and would be cropped). Source:
`docs/marketing/src/social-preview.html`, sharing `docs/fab/src/base.css` with the Fab art.
It carries no test count on purpose — it is uploaded by hand, so no release step would ever
re-render it; the engine range is the one claim that dates.

```bash
# from the repository root (base.css loads fonts from /site/public/fonts/)
python3 -m http.server 8765 --bind 127.0.0.1 &
# Playwright, chromium at 1280x640, deviceScaleFactor 1, then TinyPNG (docs/dev/site-images.md):
#   page.goto('http://127.0.0.1:8765/docs/marketing/src/social-preview.html')
#   page.screenshot({ path: 'docs/marketing/social-preview.png' })
```

Upload: Settings → General → Social preview → Edit → Upload an image. There is no API for it.

## Repository metadata

Set with `gh repo edit`, not stored anywhere in the tree — so this is the record of what it
should say:

- **Description:** Game UI in HTML/CSS for Unreal Engine, rendered off the game thread —
  RmlUi + QuickJS on a dedicated UI thread. Windows, macOS, Linux, Android and iOS.
- **Website:** https://vacuus.ufna.dev/
- **Topics:** unreal-engine, ue5, unreal-engine-plugin, ue5-plugin, game-ui, hud, user-interface,
  webui, html, css, rmlui, quickjs, gamedev, game-development, cpp, multithreading, ai-coding

## Capture recipes after the separator cleanup

Use a 1920×1080 offscreen game viewport with `-ForceRes`. Screen demos use
`vacuus.M2Demo`, `vacuus.M5Glass`, `vacuus.M5Deco`, `vacuus.RefHud` or
`vacuus.VectorDemo`, followed by `vacuus.M1HUD.AutoShot 30`. The world capture
uses `vacuus.M5World 0 1024 1024 0.25, vacuus.WorldDemo.Shot 2`.
Stop each editor normally before building again. Compress the captured PNGs with
TinyPNG before copying them to `site/public/shots`; regenerate the README crops
from those captures. The SVG/Lottie demo is `vectors.png` / `vectors.webp`.
