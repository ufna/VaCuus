#!/usr/bin/env bash
# Tools/strict_includes_check.sh — compiles this plugin the way a NON-UNITY build does,
# which is the one configuration the ordinary dev loop here cannot see (bead VaCuus-y1z).
#
#   bash Tools/strict_includes_check.sh <engine-root> <path/to/Host.uproject>
#   UE_ROOT=/w/Unreal/UnrealEngine UE_PROJECT=/w/Unreal/VcHost/VcHost.uproject \
#     bash Tools/strict_includes_check.sh
#
# WHY THIS EXISTS. TWICE an outside contributor has been the first person to compile this
# plugin with unity and the PCH off, and both times they found a real break: PR #2's
# author needed two #include lines at 683d0f0, and PR #5 landed those same two lines
# after vacuus.TexDemo sat broken for three weeks. Neither was caught here. That is not a
# code defect any more — the tree is otherwise clean on both targets — it is that nothing
# on this repository ever compiles the plugin that way. A unity build concatenates
# translation units, so a file that forgot an include silently borrows it from whichever
# neighbour landed in the same blob; a buyer whose project disables unity, or whose module
# ordering differs, gets the error we never saw.
#
# WHAT IT COMPILES, and why it is only three modules. VaCuusRml and VaCuusJs already set
# PCHUsage = NoPCHs and bUseUnity = false in their own Build.cs, so every ordinary build
# already strict-checks them. VaCuus, VaCuusRender and VaCuusEditor use
# UseExplicitOrSharedPCHs with unity on, and are therefore the three this has to force.
# Both targets, because the monolithic game target takes a different code path from the
# editor one (module boundaries, WITH_EDITOR) and has caught things the editor leg did
# not; VaCuusEditor exists only in the editor target.
#
# WHY -Module= AND NOT A Build.cs FLIP. The bead proposed temporarily rewriting the three
# Build.cs files and restoring them afterwards. This passes -DisableUnity -NoPCH
# -NoSharedPCH with -Module=<name> instead, which gets the same compilation without
# touching a tracked file — so an interrupted run, a failed build or a Ctrl-C cannot leave
# the tree modified, and there is no restore path to get wrong. It is also fast for the
# same reason the bead predicted: only our modules' actions are invalidated, the engine's
# stay cached.
#
# THE CHECK MUST BE SEEN TO FAIL BEFORE IT MAY PASS (Tools/api_export_check.sh's rule).
# There is no cheap self-test for a compile gate — the compile IS the test — so the
# demonstration is recorded rather than re-run. Deleting 1eb80f5's two #include lines from
# Source/VaCuusRender/Private/VaCuusRender.cpp, which is the exact break PR #5 fixed after
# it had shipped for three weeks, and running this reports (2026-09-19, Linux, UE 5.8.1):
#
#   VcHostEditor  VaCuusRender  FAILED
#       VaCuusRender.cpp:2634:41: error: member access into incomplete type 'FTexture2DMipMap'
#       VaCuusRender.cpp:2665:5:  error: member access into incomplete type 'FTexture2DMipMap'
#       UObjectGlobals.h:2177:31: error: incomplete type 'UStaticMesh' named in nested name specifier
#       Result: Failed (OtherCompilationError)
#   VcHost        VaCuusRender  FAILED   (the same three, monolithic)
#
# with exit 1 — and the other three legs still ok, so the report points at the module that
# is actually broken rather than going red everywhere. Restored, all five pass. Redo that
# any time this script changes; a gate never seen red is not evidence.
#
# EXIT: 0 every leg compiled, 1 a leg failed (its compiler errors are printed), 2 the
# arguments or the engine could not be resolved.

set -u

ENGINE="${1:-${UE_ROOT:-}}"
PROJECT="${2:-${UE_PROJECT:-}}"

if [[ -z "$ENGINE" || -z "$PROJECT" ]]; then
	echo "usage: bash Tools/strict_includes_check.sh <engine-root> <project.uproject>" >&2
	echo "       (or set UE_ROOT and UE_PROJECT)" >&2
	exit 2
fi

BUILD="$ENGINE/Engine/Build/BatchFiles/Linux/Build.sh"
if [[ ! -x "$BUILD" ]]; then
	echo "ABORT: no Linux Build.sh under '$ENGINE' (looked for $BUILD)" >&2
	exit 2
fi
if [[ ! -f "$PROJECT" ]]; then
	echo "ABORT: no such .uproject: $PROJECT" >&2
	exit 2
fi

PROJECT_NAME="$(basename "$PROJECT" .uproject)"
LOG="$(mktemp -t vacuus-strict-XXXXXX.log)"
trap 'rm -f "$LOG"' EXIT

# target:module pairs. VaCuusEditor is editor-only; the game target is monolithic.
LEGS=(
	"${PROJECT_NAME}Editor:VaCuus"
	"${PROJECT_NAME}Editor:VaCuusRender"
	"${PROJECT_NAME}Editor:VaCuusEditor"
	"${PROJECT_NAME}:VaCuus"
	"${PROJECT_NAME}:VaCuusRender"
)

echo "strict-includes: unity OFF, PCH OFF, ${#LEGS[@]} legs"
echo "  engine  $ENGINE"
echo "  project $PROJECT"
echo

FAILED=0
for LEG in "${LEGS[@]}"; do
	TARGET="${LEG%%:*}"
	MODULE="${LEG##*:}"
	printf '  %-22s %-14s ' "$TARGET" "$MODULE"
	if "$BUILD" "$TARGET" Linux Development -project="$PROJECT" \
		-Module="$MODULE" -DisableUnity -NoPCH -NoSharedPCH >"$LOG" 2>&1; then
		# UBT can exit 0 on "nothing to do"; the Result line is what it actually says.
		if grep -q "^Result: Succeeded" "$LOG"; then
			echo "ok"
		else
			echo "UNCLEAR -- no 'Result: Succeeded' line"
			grep -E "^Result:|error:|Error" "$LOG" | head -20 | sed 's/^/      /'
			FAILED=1
		fi
	else
		echo "FAILED"
		grep -E "error:|^Result:" "$LOG" | head -20 | sed 's/^/      /'
		FAILED=1
	fi
done

echo
if [[ $FAILED -ne 0 ]]; then
	echo "STRICT-INCLUDES: FAILED — a module does not compile without unity or a PCH."
	echo "  That is exactly what reaches a buyer whose project disables unity."
	exit 1
fi
echo "STRICT-INCLUDES: CLEAN — all ${#LEGS[@]} legs compiled with unity and PCH off."
echo "  Record the date in the milestone's passport; the point of this gate is that the"
echo "  leg it certifies ran inside the milestone rather than six weeks earlier."
exit 0
