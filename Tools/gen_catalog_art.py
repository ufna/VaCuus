#!/usr/bin/env python3
"""
Generates the catalogue demo's art and documents (VaCuus-dqs.4).

    python3 Tools/gen_catalog_art.py            # 200 icons, the shipped default
    python3 Tools/gen_catalog_art.py --count 40 # a smaller run

WHY GENERATED AND NOT COMMITTED. The demo needs N *different* image files -- that is the whole
point, since RmlUi dedups by SOURCE and N elements sharing one `src` already cost one texture.
Two hundred real icons would be tens of megabytes of art with no reviewable content, in a
repository whose .gitattributes argues at length against carrying binaries it does not need.
So the script is the artefact and its output is gitignored; anyone can reproduce it in a
second, and the numbers it produces are reproducible with it.

WHAT IT EMITS, all under Content/DevUI/Catalog/:

    loose/icon_NNN.png   N icons at the size VaCuus-dqs opens with, 213x276
    atlas.png            the same N icons packed into ONE grid image
    atlas.rcss           an @spritesheet naming every cell, plus one rule per icon
    ../catalog_loose.rml one <img src> per icon
    ../catalog_atlas.rml one sprite-backed <div> per icon

THE TWO DOCUMENTS DRAW THE SAME GRID AT THE SAME SIZE. They differ in exactly one thing --
where the pixels come from -- so vacuus.TextureStats and the perf log's draw counter are
comparing the thing under test and nothing else.

WHAT THE TWO ARMS MEASURED, 2026-08-13, 200 icons at 1280x720, headless -game, each arm its
own run (vacuus.Catalog* + vacuus.TextureStats + vacuus.M1HUD.PerfLog 1):

    arm      textures   logical bytes   draws per replayed frame
    loose        200      44.85 MiB              201.0
    atlas          1      46.87 MiB              201.0

BOTH RESULTS GO AGAINST THE REFLEX, and they are why this demo exists instead of a paragraph
asserting that atlases are better.

  1. THE ATLAS COSTS MORE *LOGICAL* BYTES AND FAR FEWER REAL ONES -- and the second half was
     measured later, by VaCuus.Render.Texture.PlatformSize, after this table had already been
     written and quoted. Read both.

     On the census's own logical arithmetic the atlas is 2 MiB WORSE: every icon is the same
     size, so the loose arm packs perfectly by definition while the atlas carries the slack of
     its last partial row (200 icons into a 19-wide grid is 11 rows of capacity 209).

     BUT THE RHI DOES NOT ALLOCATE LOGICAL BYTES. Asked what it would really reserve
     (RHICalcTexturePlatformSize, the engine's own "true measure"), this desktop Vulkan
     answers 344,064 B for a 213x276 RGBA8 against 235,152 logical -- +46.32%, on a 1024-byte
     alignment -- while the 4047x3036 atlas pays +1.21%. So:

         200 loose icons   44.85 MiB logical    65.62 MiB actually allocated
         one atlas         46.87 MiB logical    47.44 MiB actually allocated

     THE ATLAS IS 18.19 MiB CHEAPER IN REAL MEMORY. The logical comparison above is not
     wrong, it is just not the number a platform charges for -- and the gap is worst exactly
     where UI art lives, because a narrow texture wastes a larger fraction of every padded
     row. The census reports LOGICAL bytes by construction and cannot see any of this.

  2. DRAW CALLS ARE IDENTICAL: 201 either way, i.e. one per element plus the background. The
     classic atlas win -- fewer state changes because neighbours share a texture -- DOES NOT
     EXIST in VaCuus today, because nothing merges adjacent draws that share a texture. RmlUi
     emits one geometry per element and the replayer replays the recorded buffer 1:1.

  SO AN ATLAS BUYS REAL MEMORY AND LIFETIME, BUT NOT DRAWS. 18 MiB on this screen, one
  texture to release or evict instead of 200, one decode at load instead of 200 -- and not one
  draw call fewer, because nothing here merges draws that share a texture (VaCuus-dqs.5).

THE ICONS ARE DELIBERATELY NOT NOISE. Random pixels would be incompressible and would make the
PNGs (and any future atlas compression) unrepresentative of real icon art. Each is a flat
plate, a hue-rotated emblem whose vertex count varies with the index, and two readout bars --
cheap to generate, compressible like real UI art, and visibly distinct so a wrong sprite
rectangle is obvious in a screenshot rather than plausible.
"""

import argparse
import colorsys
import math
import os
from PIL import Image, ImageDraw

# The size the epic opens with, so every number this demo prints is comparable with
# VaCuus-dqs's own arithmetic: 213 x 276 x 4 = 235,152 bytes per icon.
ICON_W, ICON_H = 213, 276

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.dirname(HERE)
DEVUI = os.path.join(PLUGIN, "Content", "DevUI")
CATALOG = os.path.join(DEVUI, "Catalog")


def make_icon(index, count):
    """One icon: flat plate, polygon emblem, two bars. Distinct per index by hue and vertex count."""
    hue = (index / max(count, 1)) % 1.0
    r, g, b = colorsys.hsv_to_rgb(hue, 0.55, 0.85)
    accent = (int(r * 255), int(g * 255), int(b * 255), 255)
    plate = (24, 24, 28, 255)

    img = Image.new("RGBA", (ICON_W, ICON_H), plate)
    d = ImageDraw.Draw(img)
    d.rectangle([2, 2, ICON_W - 3, ICON_H - 3], outline=(58, 58, 56, 255), width=2)

    # The emblem: 3..10 vertices, so neighbouring icons never share a silhouette.
    sides = 3 + (index % 8)
    cx, cy, rad = ICON_W / 2, ICON_H * 0.40, min(ICON_W, ICON_H) * 0.26
    pts = []
    for i in range(sides):
        a = -math.pi / 2 + (2 * math.pi * i / sides)
        pts.append((cx + rad * math.cos(a), cy + rad * math.sin(a)))
    d.polygon(pts, fill=accent, outline=(240, 240, 235, 255))

    # Two readout bars, filled by index so the bottom of the icon differs too.
    for row in range(2):
        y = int(ICON_H * (0.72 + row * 0.11))
        d.rectangle([18, y, ICON_W - 19, y + 14], outline=(80, 80, 78, 255))
        frac = ((index * (row + 3)) % 100) / 100.0
        d.rectangle([20, y + 2, 20 + int((ICON_W - 42) * frac), y + 12], fill=accent)

    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=200)
    ap.add_argument("--cards", type=int, default=0,
                    help="Also emit catalog_nodes.rml with this many image-free cards (VaCuus-wgq).")
    args = ap.parse_args()
    count = args.count

    loose_dir = os.path.join(CATALOG, "loose")
    os.makedirs(loose_dir, exist_ok=True)

    icons = [make_icon(i, count) for i in range(count)]
    for i, img in enumerate(icons):
        img.save(os.path.join(loose_dir, f"icon_{i:03d}.png"), optimize=True)

    # A GRID PACK, not a rectangle packer, and the demo is honest about the difference in its
    # own README note: every icon here is the same size, so a grid IS the optimal pack and the
    # atlas carries only the slack of its last partial row. Real icon sets are ragged and would
    # need a real packer; that is a different experiment from the one this demo runs.
    cols = max(1, int(math.floor(4096 / ICON_W)))
    rows = int(math.ceil(count / cols))
    atlas_w, atlas_h = cols * ICON_W, rows * ICON_H
    atlas = Image.new("RGBA", (atlas_w, atlas_h), (0, 0, 0, 0))
    for i, img in enumerate(icons):
        atlas.paste(img, ((i % cols) * ICON_W, (i // cols) * ICON_H))
    atlas.save(os.path.join(CATALOG, "atlas.png"), optimize=True)

    with open(os.path.join(CATALOG, "atlas.rcss"), "w") as f:
        f.write("/* GENERATED by Tools/gen_catalog_art.py -- do not edit. See VaCuus-dqs.4. */\n")
        # `atlas.png`, NOT `Catalog/atlas.png`, and the difference cost a run. A LINKED
        # stylesheet resolves its paths relative to THE SHEET, where an INLINE <style> block
        # resolves them relative to the DOCUMENT -- so the spelling that is right in
        # spritesheet_spike.rml (inline, at the DevUI root) is wrong here. Getting it wrong
        # probes Catalog/Catalog/atlas.png and the census reads 0, which looks exactly like
        # "sprites do not work".
        f.write("@spritesheet catalog\n{\n\tsrc: atlas.png;\n\n")
        for i in range(count):
            x, y = (i % cols) * ICON_W, (i // cols) * ICON_H
            f.write(f"\ticon-{i:03d}: {x}px {y}px {ICON_W}px {ICON_H}px;\n")
        f.write("}\n\n")
        for i in range(count):
            f.write(f".icon-{i:03d} {{ decorator: image(icon-{i:03d}); }}\n")

    # The two documents. Same grid, same cell size, same everything except where pixels come
    # from -- see the module header.
    shared_style = """
		body { width: 100%; height: 100%; background-color: #101014; font-family: LatoLatin; }
		.grid { display: flex; flex-wrap: wrap; width: 100%; }
		.cell { display: block; width: 71px; height: 92px; margin: 2px; }
		#census {
			display: block; position: absolute; left: 0px; top: 0px;
			width: 100%; height: 22px; padding: 4px 8px;
			background-color: #000000c8; color: #efcc27;
			font-size: 14px;
		}
"""
    # THE CENSUS ON SCREEN, which is what this bead asked for and what makes the difference
    # legible to someone who is not reading a log. It reads vacuus.textureStats() -- the
    # published snapshot, so it costs no render-thread round trip -- twice a second.
    #
    # IT CHANGES THE THING IT MEASURES, and says so on screen rather than quietly: this is the
    # only text in either document, so it is what pulls in the font atlas, and a font atlas is
    # a texture. The strip therefore prints "incl. 1 font atlas" beside the count, and the
    # text-free runs recorded in this file's header are the clean figures.
    census_script = """
	<script>
		const readout = document.getElementById('census');
		const label = %s;
		function tick() {
			const s = vacuus.textureStats();
			const mib = (s.bytes / 1048576).toFixed(2);
			readout.innerRML = label + ' \u2014 ' + s.count + ' texture(s), ' + mib +
				' MiB logical (incl. 1 font atlas, which this readout is what pulls in)';
		}
		tick();
		setInterval(tick, 500);
	</script>
"""

    with open(os.path.join(DEVUI, "catalog_loose.rml"), "w") as f:
        cells = "\n".join(
            f'\t<img class="cell" src="Catalog/loose/icon_{i:03d}.png"/>' for i in range(count))
        f.write(f"""<rml>
<head>
	<title>catalogue — {count} loose files</title>
	<!-- GENERATED by Tools/gen_catalog_art.py. VaCuus-dqs.4: the control arm. {count} DIFFERENT
	     source files, so RmlUi's dedup-by-source cannot collapse them and this really is
	     {count} textures. Toggle with vacuus.CatalogLoose; measure with vacuus.TextureStats. -->
	<style>{shared_style}</style>{census_script % ("'LOOSE: %d files'" % count)}
</head>
<body>
<div id="census"/>
<div class="grid">
{cells}
</div>
</body>
</rml>
""")

    with open(os.path.join(DEVUI, "catalog_atlas.rml"), "w") as f:
        cells = "\n".join(f'\t<div class="cell icon-{i:03d}"/>' for i in range(count))
        f.write(f"""<rml>
<head>
	<title>catalogue — {count} sprites, one atlas</title>
	<!-- GENERATED by Tools/gen_catalog_art.py. VaCuus-dqs.4: the atlas arm. The same {count}
	     icons, drawn as sprite rectangles of ONE file. Toggle with vacuus.CatalogAtlas. -->
	<link type="text/rcss" href="Catalog/atlas.rcss"/>
	<style>{shared_style}</style>{census_script % ("'ATLAS: 1 file, %d sprites'" % count)}
</head>
<body>
<div id="census"/>
<div class="grid">
{cells}
</div>
</body>
</rml>
""")

    # VaCuus-wgq's measurement vehicle, and it deliberately carries NO IMAGES. That bead is
    # about per-frame CPU scaling with NODE COUNT -- Context::Update recurses into every child,
    # Context::Render walks the whole stacking context with no viewport culling, and the
    # interactive-region snapshot is a full-tree walk by construction. Putting art in it would
    # add texture memory and upload traffic to a measurement that is not about either.
    #
    # FIVE NODES PER CARD is the bead's own assumption about a store row, made literal so the
    # arithmetic it estimated with can be checked rather than trusted.
    if args.cards > 0:
        card = ('\t<div class="card">'
                '<div class="thumb"/><div class="title"/><div class="sub"/>'
                '<div class="price"/></div>')
        cards = "\n".join(card for _ in range(args.cards))
        with open(os.path.join(DEVUI, "catalog_nodes.rml"), "w") as f:
            f.write(f"""<rml>
<head>
	<title>{args.cards} cards, {args.cards * 5} nodes</title>
	<!-- GENERATED by Tools/gen_catalog_art.py --cards N. VaCuus-wgq: node-count scaling with no
	     images and no text, so Update/Render/snapshot cost is the only thing being measured.
	     Toggle with vacuus.CatalogNodes; measure with vacuus.M1HUD.PerfLog 1. -->
	<style>
		body {{ width: 100%; height: 100%; background-color: #101014; }}
		.grid {{ display: flex; flex-wrap: wrap; width: 100%; }}
		.card {{ display: block; width: 71px; height: 92px; margin: 2px; background-color: #1b1b1f; }}
		.thumb {{ display: block; width: 67px; height: 60px; margin: 2px; background-color: #2a2a30; }}
		.title {{ display: block; width: 55px; height: 6px; margin: 3px; background-color: #3a3a42; }}
		.sub {{ display: block; width: 40px; height: 5px; margin: 3px; background-color: #33333a; }}
		.price {{ display: block; width: 24px; height: 5px; margin: 3px; background-color: #4a4a52; }}
	</style>
</head>
<body>
<div class="grid">
{cards}
</div>
</body>
</rml>
""")
        print(f"  nodes : {args.cards} cards x 5 = {args.cards * 5} nodes -> catalog_nodes.rml")

        # THE RICH ARM. Same five ELEMENTS per card, same grid, same sizes -- what changes is
        # what those elements CONTAIN: a gradient decorator on the thumb and real text in the
        # three label boxes. That is the 4x this bead is hunting.
        #
        # IT IS NOT NODE-FOR-NODE WITH THE PLAIN ARM, and pretending otherwise would be the
        # measurement's own lie: RmlUi materialises an ElementText child for every text run, so
        # a card with three labels is 5 elements + 3 text nodes = 8. The honest x-axis is
        # therefore CARDS, with the node composition of each arm stated. A version that kept
        # node parity by deleting the text would be measuring nothing anyone ships.
        rich_card = ('\t<div class="card">'
                     '<div class="thumb"/>'
                     '<div class="title">ITEM {i:04d}</div>'
                     '<div class="sub">TIER III</div>'
                     '<div class="price">1200</div></div>')
        rich_cards = "\n".join(rich_card.format(i=i) for i in range(args.cards))
        with open(os.path.join(DEVUI, "catalog_nodes_rich.rml"), "w") as f:
            f.write(f"""<rml>
<head>
	<title>{args.cards} rich cards</title>
	<!-- GENERATED by Tools/gen_catalog_art.py --cards N. VaCuus-wgq: the same {args.cards} cards
	     as catalog_nodes.rml, with a gradient decorator and three text runs each. Toggle with
	     vacuus.CatalogNodesRich and compare against vacuus.CatalogNodes: the structure is
	     identical, so the difference is what a node CONTAINS. -->
	<style>
		body {{ width: 100%; height: 100%; background-color: #101014; font-family: LatoLatin; }}
		.grid {{ display: flex; flex-wrap: wrap; width: 100%; }}
		.card {{ display: block; width: 71px; height: 92px; margin: 2px; background-color: #1b1b1f; }}
		.thumb {{
			display: block; width: 67px; height: 60px; margin: 2px;
			decorator: linear-gradient(160deg, #2a2a30, #4a4a52);
		}}
		.title {{ display: block; font-size: 8px; color: #d8d8d6; margin-left: 3px; }}
		.sub {{ display: block; font-size: 7px; color: #8b8b88; margin-left: 3px; }}
		.price {{ display: block; font-size: 7px; color: #efcc27; margin-left: 3px; }}
	</style>
</head>
<body>
<div class="grid">
{rich_cards}
</div>
</body>
</rml>
""")
        print(f"  rich  : {args.cards} cards x (5 elements + 3 text) -> catalog_nodes_rich.rml")

    loose_bytes = count * ICON_W * ICON_H * 4
    atlas_bytes = atlas_w * atlas_h * 4
    print(f"{count} icons at {ICON_W}x{ICON_H}")
    print(f"  loose : {count} textures, {loose_bytes / 1048576:.2f} MiB logical")
    print(f"  atlas : 1 texture  {atlas_w}x{atlas_h}, {atlas_bytes / 1048576:.2f} MiB logical")
    print(f"  disk  : loose {sum(os.path.getsize(os.path.join(loose_dir, n)) for n in os.listdir(loose_dir)) / 1048576:.2f} MiB, "
          f"atlas {os.path.getsize(os.path.join(CATALOG, 'atlas.png')) / 1048576:.2f} MiB")


if __name__ == "__main__":
    main()
