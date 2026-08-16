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
#
# RUN THE GATES *BEFORE* ANY PROJECT IS BUILT AGAINST THE PACKAGE, and against a package
# produced from a CLEAN CLONE. Both halves are load-bearing:
#   - Dropping the package into a project's Plugins/ and building writes Binaries/ and
#     Intermediate/ INTO the package directory. After that this script reports fail=4 and
#     Tools/fab_scan.sh reports ~253 violations — every one of them build output, not a
#     package defect. Measured 2026-08-04.
#   - Building from a DIRTY tree lets untracked files ride the broad `/Web/...` filter
#     rule into the package. That is how the old `present Web/package-lock.json` row ever
#     passed for a file that has never been tracked (see the `absent` row below).
# If you must re-verify after a build, re-package first.
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
present "Web/README.md"
present "Web/smoke.mjs"
present "Web/packages/preact-vacuus/PREACT-LICENSE"
present "Web/packages/cli/bin/vacuus.mjs"
present "Web/apps/demo-hud"
absent  "Web/node_modules"
# ...and NOT the lockfile. It has never been tracked in any branch
# (`git log --all -- Web/package-lock.json` is empty) and Web/.gitignore:6 excludes it,
# so from a CLEAN CLONE it cannot exist. This row used to assert `present` and could
# only ever have passed against a dev tree where a stray local `npm install` left an
# untracked lockfile that the broad `/Web/...` filter rule then swept into the package —
# i.e. green on a dirty tree and red on a correct one, the wrong way round for a gate
# guarding a Fab submission (bead jn1). Asserting `absent` is what makes the row
# reproducible AND catches the dirty-tree leak it was previously hiding.
absent  "Web/package-lock.json"

# Backends struck from the vendored tree; the compiled subtree intact
absent  "Source/ThirdParty/RmlUi/Backends"
present "Source/ThirdParty/RmlUi/Source/Core"
present "Source/ThirdParty/RmlUi/Include/RmlUi/Core"

# THE RE-VENDOR SCRIPTS DO NOT SHIP -- the first thing Fab's technical review failed the
# 1.0 package on ("the plugin will not be compiled until executables are removed",
# 2026-08-16, bead VaCuus-1hy). M6 had decided to ship them as source with the exec bit
# stripped and fab_scan.sh whitelisted their shebangs, so BOTH gates blessed the exact
# shape a reviewer refused; these rows are that reversal's observable, and the whitelist
# entries are gone so the scan now reports either file if a filter change lets it back in.
absent  "Source/VaCuusJs/gen_relays.sh"
absent  "Source/VaCuusRml/gen_relays.sh"

# ...and the FILTER ITSELF ships, the third failure of that review: the rules that declare
# docs/ and Web/ deliberate never reached the reviewer, because BuildPlugin's default
# filter has no /Config/... rule (BuildPluginCommand.Automation.cs:459-472). The copy in
# the package is inert -- BuildPlugin reads the source tree's -- and exists to be read.
present "Config/FilterPlugin.ini"

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
absent  ".git"
absent  ".beads"
absent  "CLAUDE.md"
# AGENTS.md USED TO BE AN `absent` ROW HERE, and it was right until 2026-08-09: the only
# AGENTS.md that existed was the repository's, about developing the plugin. The package now
# carries a DIFFERENT file of that name for the buyer's coding agent (bead VaCuus-avu), so
# the question changed from "is it here" to "which one is it" -- asserted below, by content.

# THE BUYER DOCUMENTATION SHIPS, and this is the positive form of what an `absent "docs"`
# row used to assert here. FilterPlugin.ini gained `/README.md` and `/docs/buyer/...` at
# c1394d6 (2026-08-03, "the package stops being a plugin nobody can be told how to use")
# precisely because the buyer-install simulation had graded GREEN only by reading these
# pages out of the DEV TREE. This script was last touched a day earlier (28fd5b9,
# 2026-08-02), so its `absent "docs"` row went stale on the spot and failed every correct
# package from then on (bead jn1). Every one of these four pages is cross-referenced BY
# NAME from the others — "gotchas.md #18" is a dead pointer without the file — so a
# missing page is a real delivery defect, not a tidiness one.
present "README.md"
present "docs/buyer/setup.md"
present "docs/buyer/gotchas.md"
present "docs/buyer/perf-guide.md"
present "docs/buyer/rcss-matrix.md"

# ...but the handoff is INTERNAL: it enumerates the owner's unrun hardware matrix, machine
# hostnames and bead ids. FilterPlugin.ini:70-76 excludes it with a later-wins rule; this
# row is that decision's observable.
absent  "docs/buyer/owner-handoff.md"
# Nothing outside docs/buyer/ ships — the research notes, specs, passport and plans are
# repo-only. (Checked as a directory: `-e` on docs/research is enough.)
absent  "docs/research"
absent  "docs/passport"
absent  "docs/superpowers"
absent  "docs/dev"

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

# EVERY MODULE DECLARES ITS PLATFORMS -- the second Fab failure (bead VaCuus-1hy: "all
# listed modules need their own PlatformAllowList or PlatformDenyList key with values").
# Asserted on the PACKAGED descriptor and not the repository's, because BuildPlugin
# rewrites it (BuildPluginCommand.Automation.cs:433-445) and the key survives only if
# ModuleDescriptor.Write emits it (ModuleDescriptor.cs:432-440) -- which is the half a
# repo-side check could not see. One LoadingPhase per module is the module count.
MODULES="$(grep -c '"LoadingPhase"' "$PKG/VaCuus.uplugin" 2>/dev/null || echo 0)"
ALLOWED="$(grep -c '"PlatformAllowList"' "$PKG/VaCuus.uplugin" 2>/dev/null || echo 0)"
if [ "$MODULES" -gt 0 ] && [ "$ALLOWED" = "$MODULES" ]; then
	ok "every module in the packaged descriptor has PlatformAllowList ($ALLOWED/$MODULES)"
else
	bad "PlatformAllowList on $ALLOWED of $MODULES modules in the packaged descriptor"
fi

# ---------------------------------------------------------------- the buyer's front door
# Bead VaCuus-avu. Tools/fab_package.sh writes AGENTS.md into the package root from
# docs/buyer/agents-root.md; a package produced by RunUAT ALONE will not have it, and that
# is the point of asserting it here rather than trusting the step ran.
present "AGENTS.md"
present "docs/buyer/ai-guide.md"
present "THIRD-PARTY-NOTICES.md"
present "PACKAGE-MANIFEST.txt"

# ...and it must be the BUYER's file, not the repository's. The two are different documents
# with the same name: ours is about developing the plugin (beads, the two-tree build), and
# handing that to a buyer's coding agent would point it at a workflow that does not exist on
# its machine. A grep for a phrase only the buyer copy contains is what tells them apart --
# a `present` row cannot, because both would satisfy it.
if grep -q "instructions for AI coding agents" "$PKG/AGENTS.md" 2>/dev/null &&
   ! grep -q "beads" "$PKG/AGENTS.md" 2>/dev/null; then
	ok "AGENTS.md is the buyer copy, not the repository's dev one"
else
	bad "AGENTS.md is NOT the buyer copy (dev instructions would reach the buyer)"
fi

# The source of that file must NOT also ship: two front doors, one of them buried, and the
# same text in the package twice.
absent  "docs/buyer/agents-root.md"

echo "----"
echo "pass=$pass fail=$fail"
[ "$fail" = 0 ]
