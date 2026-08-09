#!/usr/bin/env bash
# Tools/fab_package.sh — produce the folder that goes to Fab, and refuse to produce it if
# anything about the result is wrong. Bead VaCuus-avu.
#
#   bash Tools/fab_package.sh [--sha <rev>] [--out <dir>] [--engine <dir>]
#                             [--fast] [--zip] [--keep-clone]
#
# WHY A SCRIPT AND NOT A RECIPE IN A DOC. Every ingredient already existed -- RunUAT
# BuildPlugin applies Config/FilterPlugin.ini, Tools/fab_scan.sh is the no-executables
# gate, Tools/fab_inventory.sh is the enumerated inventory -- and every packaging run so
# far still went wrong somewhere a human had to notice. The 2026-08-02 artifact was built
# from a work tree carrying an UNCOMMITTED include fix, so a package rebuilt from canonical
# would have FAILED -StrictIncludes; the buyer docs did not ship at all for three
# milestones because a filter rule was missing and nothing asserted it. Both are classes of
# mistake a script can make impossible and prose cannot.
#
# WHAT IT GUARANTEES, and each one is a refusal rather than a warning:
#   1. the package is built from a CLEAN CLONE at a NAMED SHA, never from a work tree;
#   2. LFS content is real, not ~130-byte pointers;
#   3. the descriptor a buyer reads carries no placeholder;
#   4. the package passes fab_scan.sh AND fab_inventory.sh before it exists at the output
#      path -- a failed run leaves nothing behind that could be mistaken for a good one.
#
# WHAT IT ADDS to BuildPlugin's output: the root AGENTS.md, materialised from
# docs/buyer/agents-root.md. See the FilterPlugin comment for why it is not shipped in
# place, and docs/buyer/ai-guide.md for what it is.
set -uo pipefail

# ---------------------------------------------------------------- arguments and defaults
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SHA="HEAD"
OUT=""
ENGINE="${UE_ROOT:-/w/Unreal/UnrealEngine}"
FAST=0
ZIP=0
KEEP_CLONE=0

die() { echo "fab_package: $*" >&2; exit 1; }
step() { echo; echo "=== $* ==="; }

while [ $# -gt 0 ]; do
	case "$1" in
		--sha)        SHA="${2:?--sha needs a value}"; shift 2 ;;
		--out)        OUT="${2:?--out needs a value}"; shift 2 ;;
		--engine)     ENGINE="${2:?--engine needs a value}"; shift 2 ;;
		--fast)       FAST=1; shift ;;
		--zip)        ZIP=1; shift ;;
		--keep-clone) KEEP_CLONE=1; shift ;;
		-h|--help)    sed -n '2,30p' "${BASH_SOURCE[0]}"; exit 0 ;;
		*)            die "unknown argument: $1" ;;
	esac
done

[ -d "$ENGINE/Engine/Build/BatchFiles" ] || die "no engine at $ENGINE (set --engine or UE_ROOT)"
RUNUAT="$ENGINE/Engine/Build/BatchFiles/RunUAT.sh"
[ -x "$RUNUAT" ] || die "no RunUAT.sh at $RUNUAT"

RESOLVED_SHA="$(git -C "$REPO" rev-parse --verify "$SHA^{commit}" 2>/dev/null)" \
	|| die "not a commit in $REPO: $SHA"
SHORT_SHA="${RESOLVED_SHA:0:7}"
[ -n "$OUT" ] || OUT="/tmp/vacuus-fab-$SHORT_SHA"

# THE OUTPUT PATH IS NEVER OVERWRITTEN IN PLACE. BuildPlugin merges into an existing
# directory rather than replacing it, so a re-run over a previous package can leave files
# that no longer pass the filter -- and both gates would then be scanning a mixture of two
# packages while reporting on one.
[ -e "$OUT" ] && die "output already exists: $OUT (remove it, or pass a different --out)"

CLONE="$(mktemp -d "${TMPDIR:-/tmp}/vacuus-fab-clone-XXXXXX")" || die "mktemp failed"
cleanup() { [ "$KEEP_CLONE" = 1 ] || rm -rf "$CLONE"; }
trap cleanup EXIT

echo "fab_package"
echo "  repo    $REPO"
echo "  commit  $RESOLVED_SHA"
echo "  engine  $ENGINE"
echo "  out     $OUT"
echo "  mode    $([ "$FAST" = 1 ] && echo 'FAST (filter only, no compile)' || echo 'full (-StrictIncludes, compiles)')"

# ------------------------------------------------------------------ 1. clean clone at SHA
#
# A CLONE AND NOT AN rsync OF THE WORK TREE, and this is finding C1 of the buyer-install
# simulation made structural: the 2026-08-02 package was produced from a tree with an
# uncommitted fix, so its green result described a tree that did not exist in canonical.
# `git clone` can only ever materialise committed content, which is the property we want --
# so an uncommitted fix now fails the package instead of silently blessing it.
step "1. clean clone at $SHORT_SHA"
git clone --quiet "$REPO" "$CLONE/VaCuus" || die "clone failed"
git -C "$CLONE/VaCuus" checkout --quiet --detach "$RESOLVED_SHA" || die "checkout failed"

if git -C "$CLONE/VaCuus" lfs env >/dev/null 2>&1; then
	git -C "$CLONE/VaCuus" lfs pull >/dev/null 2>&1 || die "git lfs pull failed"
else
	echo "  note: git-lfs not configured here; the pointer check below is what catches it"
fi

# The .git directory must not ride into BuildPlugin's input: it is not filtered by
# FilterPlugin (which describes the plugin's own tree) and it carries every branch.
rm -rf "$CLONE/VaCuus/.git" "$CLONE/VaCuus/.beads"
echo "  materialised $(find "$CLONE/VaCuus" -type f | wc -l) files"

# --------------------------------------------------------- 2. the clone is fit to package
step "2. preconditions on the clone"
fail=0
check() { if eval "$2"; then echo "  PASS  $1"; else echo "  FAIL  $1"; fail=1; fi; }

# LFS POINTERS ARE THE SILENT ONE. A checkout that never smudged packages ~130-byte text
# files with .uasset names; the plugin then boots, lays out correctly and draws nothing,
# with three warning lines per asset as the only evidence (the same shape that cost the
# demo project its 61 images). fab_scan has an LFS_POINTER class for the package, but
# catching it HERE names the cause instead of the symptom.
#
# BOTH CHECKS SKIP Tools/scan-fixture*, and not as a convenience. Those trees are
# fab_scan.sh's self-test corpus: one committed plant per violation class, including a
# literal LFS pointer and a literal node_modules directory, and fab_scan ABORTS unless it
# finds exactly them. They are supposed to look wrong. They also never reach a package --
# no filter rule includes Tools/ at all -- so a precondition that failed on them would be
# refusing to package the repository's own gate.
FIXTURES=(-path "$CLONE/VaCuus/Tools/scan-fixture*" -prune -o)

POINTERS="$(find "$CLONE/VaCuus" "${FIXTURES[@]}" -type f -print 2>/dev/null |
	xargs -r grep -l --binary-files=text '^version https://git-lfs' 2>/dev/null | head -5)"
check "no LFS pointers in the clone" '[ -z "$POINTERS" ]'
[ -n "$POINTERS" ] && printf '        %s\n' $POINTERS

check "no node_modules" \
	'[ -z "$(find "$CLONE/VaCuus" "${FIXTURES[@]}" -type d -name node_modules -print -quit)" ]'
check "no Binaries"          '[ ! -d "$CLONE/VaCuus/Binaries" ]'
check "no Intermediate"      '[ ! -d "$CLONE/VaCuus/Intermediate" ]'
check "descriptor present"   '[ -f "$CLONE/VaCuus/VaCuus.uplugin" ]'
check "agents-root source"   '[ -f "$CLONE/VaCuus/docs/buyer/agents-root.md" ]'
check "third-party notices"  '[ -f "$CLONE/VaCuus/THIRD-PARTY-NOTICES.md" ]'

# THE DESCRIPTOR IS WHAT A BUYER READS IN Edit -> Plugins, and it shipped for three
# milestones saying "VaCuus plugin for Unreal Engine." A placeholder there is not a
# cosmetic defect; it is the product's one-line pitch, blank, on the storefront page and in
# every buyer's editor. So it fails the package rather than warning about it.
DESC="$(sed -n 's/.*"Description"[[:space:]]*:[[:space:]]*"\(.*\)".*/\1/p' "$CLONE/VaCuus/VaCuus.uplugin")"
check "descriptor Description is not the placeholder" \
	'[ -n "$DESC" ] && [ "$DESC" != "VaCuus plugin for Unreal Engine." ]'
check "descriptor DocsURL is set" \
	'grep -q "\"DocsURL\"[[:space:]]*:[[:space:]]*\"http" "$CLONE/VaCuus/VaCuus.uplugin"'

# A WARNING AND NOT A REFUSAL: MarketplaceURL can only be the Fab product page, and that
# URL does not exist until the listing does. Anything else there sends the editor's
# "Marketplace" button somewhere wrong, which is worse than empty.
grep -q '"MarketplaceURL"[[:space:]]*:[[:space:]]*""' "$CLONE/VaCuus/VaCuus.uplugin" \
	&& echo "  WARN  MarketplaceURL is empty -- fill it once the Fab listing exists"

[ "$fail" = 0 ] || die "the clone is not fit to package (see FAIL rows above)"

# ------------------------------------------------------------------------ 3. BuildPlugin
#
# -StrictIncludes is -NoPCH -NoSharedPCH -DisableUnity (BuildPluginCommand.Automation.cs:
# 133-137). It is not optional strictness: Fab recompiles this source in an environment we
# do not control, and the first run that used it found seven missing includes that PCH and
# unity had been silently supplying. --fast skips compilation entirely and exercises only
# the FILTER, which is the right loop when the thing under test is FilterPlugin.ini.
step "3. RunUAT BuildPlugin"
UAT_ARGS=(BuildPlugin "-Plugin=$CLONE/VaCuus/VaCuus.uplugin" "-Package=$OUT")
if [ "$FAST" = 1 ]; then
	UAT_ARGS+=(-NoHostPlatform -NoTargetPlatforms)
else
	UAT_ARGS+=("-TargetPlatforms=$(uname -s | sed 's/Darwin/Mac/;s/Linux/Linux/')" -StrictIncludes)
fi
echo "  $RUNUAT ${UAT_ARGS[*]}"
if ! "$RUNUAT" "${UAT_ARGS[@]}"; then
	rm -rf "$OUT"
	die "BuildPlugin failed; nothing left at $OUT"
fi

# ------------------------------------------------- 4. the root files BuildPlugin cannot add
#
# AGENTS.md is the front door for a buyer's coding agent, and it must sit at the package
# ROOT to be found -- but the repository root already has an AGENTS.md about DEVELOPING
# this plugin, and the two cannot occupy one path. So the buyer's copy is authored at
# docs/buyer/agents-root.md, excluded from the filter there, and written here.
step "4. materialise the root AGENTS.md"
# Strip the authoring note: it explains this mechanism to us and means nothing to a buyer.
sed '/^<!--$/,/^-->$/d' "$CLONE/VaCuus/docs/buyer/agents-root.md" > "$OUT/AGENTS.md" \
	|| die "could not write $OUT/AGENTS.md"
head -1 "$OUT/AGENTS.md" | grep -q '^# VaCuus' \
	|| die "AGENTS.md did not come out as expected (comment strip removed too much?)"
echo "  wrote AGENTS.md ($(wc -l < "$OUT/AGENTS.md") lines)"

# ------------------------------------------------------------------------- 5. provenance
#
# The re-run contract of bead VaCuus-n3w asks for the SHA an artifact was built from to be
# recorded WITH the artifact. BEFORE the gates and not after, so that the gates scan every
# file that ships -- a manifest written afterwards would be the one file in the package
# that no scan had ever looked at. It records provenance only: whether the gates passed is
# the gates' output to state, not this file's to claim about itself.
step "5. manifest"
{
	echo "VaCuus Fab package"
	echo "commit:        $RESOLVED_SHA"
	echo "built:         $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo "engine:        $ENGINE"
	echo "mode:          $([ "$FAST" = 1 ] && echo 'fast (filter only)' || echo 'full (-StrictIncludes)')"
	echo "files:         $(find "$OUT" -type f | wc -l)"
} > "$OUT/PACKAGE-MANIFEST.txt"
sed 's/^/  /' "$OUT/PACKAGE-MANIFEST.txt"

# ------------------------------------------------------------------------- 6. the gates
#
# BOTH, AND BEFORE ANYTHING IS BUILT AGAINST THE PACKAGE. fab_inventory's own header is
# explicit that dropping a package into a project and building writes Binaries/ and
# Intermediate/ INTO it, after which the inventory reports fail=4 and the scan ~253
# violations -- all of them build output, none of them package defects.
step "6. gates"
GATES_OK=1
bash "$REPO/Tools/fab_scan.sh" "$OUT" || GATES_OK=0
echo
bash "$REPO/Tools/fab_inventory.sh" "$OUT" || GATES_OK=0
if [ "$GATES_OK" != 1 ]; then
	echo
	echo "fab_package: a gate failed. The package is left at $OUT for inspection and is" >&2
	echo "             NOT fit to upload. Fix, delete it, and re-run." >&2
	exit 1
fi

if [ "$ZIP" = 1 ]; then
	step "7. zip"
	( cd "$(dirname "$OUT")" && zip -qr "$(basename "$OUT").zip" "$(basename "$OUT")" ) \
		|| die "zip failed"
	echo "  $OUT.zip ($(du -h "$OUT.zip" | cut -f1))"
fi

echo
echo "fab_package: OK -- $OUT"
