#!/usr/bin/env bash
# Tools/fab_inventory.sh — the enumerated inventory checklist for a
# `RunUAT BuildPlugin -Package=<dir>` output (M6 spec §2(e), plan 6.3). Committed so
# the dry-run doc's "34/34 PASS" is reproducible, not a prose claim (M6 review
# round 1): every expectation is a line here, and the count is whatever this script
# prints.
#
#   bash Tools/fab_inventory.sh <package-dir>
#
# Non-existence rows are as load-bearing as existence rows: each `absent` encodes a
# FilterPlugin decision or a repo-only guarantee whose silent failure would ship the
# wrong tree. Reasons live beside the rows; the fuller story is
# docs/research/m6-api-notes/buildplugin-fab-dryrun.md §§1,7.
set -u

if [ $# -ne 1 ] || [ ! -d "$1" ]; then
	echo "usage: $0 <package-dir>" >&2
	exit 3
fi
PKG="$1"
pass=0; fail=0
ok()  { echo "PASS  $1"; pass=$((pass+1)); }
bad() { echo "FAIL  $1"; fail=$((fail+1)); }

present() { [ -e "$PKG/$1" ] && ok "present: $1" || bad "MISSING: $1"; }
absent()  { [ ! -e "$PKG/$1" ] && ok "absent:  $1" || bad "PRESENT (should not be): $1"; }

# FilterPlugin honored — Web rides, source-only
present "Web/package.json"
present "Web/package-lock.json"
present "Web/README.md"
present "Web/smoke.mjs"
present "Web/packages/preact-vacuus/PREACT-LICENSE"
present "Web/packages/cli/bin/vacuus.mjs"
present "Web/apps/demo-hud"
absent  "Web/node_modules"

# Backends struck from the vendored tree; the compiled subtree intact
absent  "Source/ThirdParty/RmlUi/Backends"
present "Source/ThirdParty/RmlUi/Source/Core"
present "Source/ThirdParty/RmlUi/Include/RmlUi/Core"

# Licenses — every disclosure entry's in-tree pointer ships
present "Source/ThirdParty/RmlUi/LICENSE.txt"
present "Source/ThirdParty/RmlUi/Include/RmlUi/Core/Containers/LICENSE.txt"
present "Source/ThirdParty/RmlUi/Source/Debugger/LICENSE.txt"
present "Source/ThirdParty/quickjs-ng/LICENSE"
present "Content/DevUI/fonts/OFL.txt"
present "Content/DevUI/fonts/LatoLatin-Regular.ttf"

# Content rides whole (incl. the marker-only bundle asset)
present "Content/Bundles/DevUIBundle.uasset"
present "Content/DevUI/RefHud"
present "Content/DevUI/M5Hud/hud_bundle.js"

# Repo-only trees can never ship
absent  "Tools"
absent  "docs"
absent  ".git"
absent  ".beads"
absent  "CLAUDE.md"
absent  "AGENTS.md"

# Ships on purpose: it keeps a buyer's `npm install` output (the esbuild ELF) out of
# THEIR version control — the exact hazard class the fab_scan polices.
present "Web/.gitignore"

# Default-filter conventions: root Tests/ excluded (none exists), module test SOURCE ships
present "Source/VaCuus/Private/Tests"
absent  "Tests"

# The dry-run finding: the Fab source-only shape — no binaries, no UHT intermediates
absent  "Binaries"
absent  "Intermediate"
present "VaCuus.uplugin"
present "Resources"
present "Shaders"

echo "----"
echo "pass=$pass fail=$fail"
[ "$fail" = 0 ]
