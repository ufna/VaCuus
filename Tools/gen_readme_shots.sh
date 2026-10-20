#!/usr/bin/env bash
# Makes the README's raster images out of the site's headless captures (VaCuus-gmv.2).
#
#     bash Tools/gen_readme_shots.sh                          # needs TINYPNG_API_KEY in .env
#     ENV_FILE=/path/to/.env bash Tools/gen_readme_shots.sh   # from a worktree, which has none
#
# Writes docs/marketing/readme/{refhud,m2demo,world,glass,deco,vectors}.webp.
#
# WHY WEBP. README.md links its images by relative path, so they are fetched out of the
# repository on every view. *.png is a Git LFS pattern (.gitattributes) and LFS objects are
# billed per download; *.webp is not, so these stay ordinary small blobs -- 1.1 MB of PNG
# became 0.21 MB (measured 2026-10-12). Every published raster goes through TinyPNG
# (docs/dev/site-images.md), and TinyPNG's convert step is what produces the WebP, so there
# is one pipeline, not two.
#
# WHY CROPS. A README table cell is ~445 px wide on github.com: the full 1920x1080 frame shrank
# the UI in m2demo and glass to texture. Each crop keeps every panel of its demo plus enough
# level round it to read as "in a game". The hero (refhud) stays a full frame. The feature
# rows link to the uncropped PNGs in site/public/shots/.
#
# SOURCES are site/public/shots/*.png, the 1920x1080 headless renders the site serves.
# Re-capture those, then re-run this. Boxes are WxH+X+Y in source pixels.
set -euo pipefail
cd "$(dirname "$0")/.."

# shellcheck disable=SC1090
source "${ENV_FILE:-.env}"
: "${TINYPNG_API_KEY:?TINYPNG_API_KEY is not set -- see docs/dev/site-images.md}"

SRC=site/public/shots
OUT=docs/marketing/readme
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT"

shot() { # name crop-box ('' = full frame)
  local png="$TMP/$1.png" loc
  if [[ -n "$2" ]]; then
    magick "$SRC/$1.png" -crop "$2" +repage -strip "$png"
  else
    magick "$SRC/$1.png" -strip "$png"
  fi
  loc=$(curl -sS -D - -o /dev/null --user "api:$TINYPNG_API_KEY" \
        --data-binary @"$png" https://api.tinify.com/shrink \
        | grep -i '^location:' | tr -d '\r' | awk '{print $2}')
  curl -sS --fail --user "api:$TINYPNG_API_KEY" -H 'Content-Type: application/json' \
       -d '{"convert":{"type":"image/webp"}}' "$loc" -o "$OUT/$1.webp"
  echo "$OUT/$1.webp  $(magick identify -format '%wx%h' "$OUT/$1.webp")  $(stat -c %s "$OUT/$1.webp") bytes"
}

shot refhud ''                 # the hero: the whole reference HUD
shot m2demo 1000x562+0+0       # all four interaction panels; they end near x 942, y 472
shot world  1240x698+380+90    # the quad's panel cluster, x ~600..1390, y ~130..775
shot glass  1560x690+90+200    # the three glass panels, x 120..1622, y 320..655
shot deco   1780x790+90+130    # the six decorator panels, x 120..1842, y 160..893

shot vectors 1600x650+0+0    # vector demo header and all four panels
