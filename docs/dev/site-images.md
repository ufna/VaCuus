# Site & marketplace images: compress before publishing

Every raster image we publish — site screenshots (`site/public/shots/`), Fab gallery
(`docs/fab/gallery/`), anything else that ships to users — **must go through the TinyPNG
API before it is committed**. Raw 1920×1080 editor captures weigh 1.6–2.4 MB each; after
TinyPNG they are ~300–480 KB (−79% across the five site shots, 2026-08-13) with the same
resolution, quantized to an 8-bit palette. PNG stays PNG, so no reference changes.

## API key

The key lives in `.env` at the repo root (gitignored — **never commit it**):

```
TINYPNG_API_KEY=...
```

Free tier is 500 compressions/month per key. Get a key at https://tinypng.com/developers.

## How to compress

One file, in place:

```bash
source .env
loc=$(curl -sS -D - -o /dev/null --user "api:$TINYPNG_API_KEY" \
      --data-binary @shot.png https://api.tinify.com/shrink \
      | grep -i '^location:' | tr -d '\r' | awk '{print $2}')
curl -sS --user "api:$TINYPNG_API_KEY" "$loc" -o shot.png
```

The upload returns the compressed result's URL in the `Location` header; the second
request downloads it. Already-compressed files are safe to re-run (TinyPNG is roughly
idempotent) but each pass costs quota — check `ls -l` first: a 1920×1080 shot already
under ~500 KB has been done.

## Why this matters

The shots are LFS objects served by GitHub Pages; before compression the landing page
pulled 9.6 MB of PNGs and screenshots visibly crawled in. Compression is a publish-time
step, not a build-time one — the site workflow deploys whatever bytes are committed.
