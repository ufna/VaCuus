#!/usr/bin/env bash
# Tools/api_export_check.sh — does the DELIVERED BINARY still export the supported C++
# surface? (bead VaCuus-dgl.) Run it over a `RunUAT BuildPlugin -Package=<dir>` output
# that carries Binaries/ (i.e. with FilterPlugin.ini's `-/Binaries/...` rule disabled, or
# on any precompiled drop):
#
#   bash Tools/api_export_check.sh <package-dir>
#
# WHY THIS SCRIPT EXISTS AT ALL — the invariant had no observable, and rotted for four
# milestones. UVaCuusWidget shipped for months with its header in Private/ and no
# VACUUSRENDER_API, and NOTHING in this repository could see it: every consumer (VcHost,
# VaCuusDemo, VaCuusUserDemo) compiles the plugin from source, where Private/ is on the
# include path and symbol visibility is irrelevant. No automation test can see it either
# — an in-process test links against the same module it is testing, so hidden symbols
# resolve fine. The defect is only visible from OUTSIDE the .so, which makes `nm -D` the
# only observable there is, and this script is it.
#
# THE MECHANISM IT GUARDS. On Linux UBT compiles modular targets with
# `-fvisibility-ms-compat` (LinuxToolChain.cs:437-440) = "global TYPES get default
# visibility, global functions and variables get hidden". So a UCLASS with no _API macro
# still REGISTERS — UHT stamps the module's API macro on the generated registrar whatever
# the class says, and the vtable/typeinfo are types — and Blueprint keeps working, while
# every hand-written member is invisible to a linker outside the module. A buyer with
# precompiled binaries can place the widget and cannot link one line of C++ against it.
# That is the exact shape of the shipped bug, and it is why "Blueprint works" is not
# evidence.
#
# THE CHECK MUST BE SEEN TO FAIL BEFORE IT MAY PASS (Tools/fab_scan.sh's rule, same
# reasoning). Every invocation runs three legs:
#   1. REQUIRED  — each supported class must have >= 1 exported member in its module;
#   2. INTERNAL  — FVaCuusRmlDocumentHost must have ZERO, because the CreateView seam
#                  deliberately does not hand buyers a constructible host
#                  (VaCuusRender.Build.cs carries that decision). This leg is a real
#                  invariant, not decoration: it fails if someone exports the render
#                  backend by reflex;
#   3. SELF-TEST — the REQUIRED matcher is run once against a class name that cannot
#                  exist. If that does not come back FAIL, the matcher is broken and the
#                  whole run aborts rather than reporting a green it did not earn.
#
# LINUX ONLY, and it says so instead of pretending: the visibility rule, the mangling and
# `nm -D` are all ELF. A Win64 package needs the same check written against `dumpbin
# /exports`; nobody has needed one yet because Fab ships this plugin as source.
set -u

PKG="${1:-}"
if [[ -z "${PKG}" ]]; then
	echo "usage: bash Tools/api_export_check.sh <package-dir>" >&2
	exit 2
fi

# Fail closed on anything that is not a BuildPlugin package with Linux binaries: a
# missing directory must never read as "no violations found".
BINDIR="${PKG}/Binaries/Linux"
if ! compgen -G "${PKG}/*.uplugin" > /dev/null; then
	echo "ABORT: no *.uplugin at '${PKG}' — that is not a BuildPlugin package" >&2
	exit 2
fi
if [[ ! -d "${BINDIR}" ]]; then
	echo "ABORT: '${BINDIR}' does not exist — this package carries no Linux binaries," >&2
	echo "       so there is nothing to check. (Source-only packages are the Fab upload" >&2
	echo "       shape; comment out FilterPlugin.ini's -/Binaries/... to get a binary drop.)" >&2
	exit 2
fi
if ! command -v nm > /dev/null 2>&1; then
	echo "ABORT: nm not found (binutils)" >&2
	exit 2
fi

# THE SUPPORTED SURFACE, as "module<TAB>class". Adding a class here is how you promise it;
# see VaCuusRender.Build.cs for what is deliberately absent and why.
REQUIRED=(
	"VaCuusRender	UVaCuusWidget"          # screen host — UMG, or any Slate tree via TakeWidget()
	"VaCuusRender	UVaCuusWorldComponent"  # world host — a panel on a quad
	"VaCuus	UVaCuusView"                    # the handle everything is driven through
	"VaCuus	UVaCuusSubsystem"               # owns views; the CreateView extension seam
	"VaCuus	UVaCuusStyleSet"                # RCSS material decorators (rcss-matrix.md)
)

# Deliberately NOT exported. Zero members is the pass.
FORBIDDEN=(
	"VaCuusRender	FVaCuusRmlDocumentHost"
)

# Counts exported member symbols of Class:: in one module's .so. Prints the count; prints
# nothing else, so it can be used by every leg including the self-test.
count_members() {
	local Module="$1" Class="$2" So
	So="${BINDIR}/libUnrealEditor-${Module}.so"
	if [[ ! -f "${So}" ]]; then
		echo "MISSING_SO"
		return
	fi
	nm -D --defined-only --demangle "${So}" 2>/dev/null | grep -c -- "${Class}::" || true
}

FAILURES=0

echo "== supported surface (must be reachable from a buyer's C++) =="
for Entry in "${REQUIRED[@]}"; do
	Module="${Entry%%	*}"
	Class="${Entry##*	}"
	N="$(count_members "${Module}" "${Class}")"
	if [[ "${N}" == "MISSING_SO" ]]; then
		echo "  FAIL  ${Module}/${Class}: libUnrealEditor-${Module}.so not in the package"
		FAILURES=$((FAILURES + 1))
	elif [[ "${N}" -lt 1 ]]; then
		echo "  FAIL  ${Module}/${Class}: 0 exported members — the class registers but cannot be linked"
		FAILURES=$((FAILURES + 1))
	else
		echo "  ok    ${Module}/${Class}: ${N} exported members"
	fi
done

echo "== internals (must stay unreachable) =="
for Entry in "${FORBIDDEN[@]}"; do
	Module="${Entry%%	*}"
	Class="${Entry##*	}"
	N="$(count_members "${Module}" "${Class}")"
	if [[ "${N}" == "MISSING_SO" ]]; then
		echo "  FAIL  ${Module}/${Class}: libUnrealEditor-${Module}.so not in the package"
		FAILURES=$((FAILURES + 1))
	elif [[ "${N}" -gt 0 ]]; then
		echo "  FAIL  ${Module}/${Class}: ${N} exported members — the render backend leaked into the ABI"
		FAILURES=$((FAILURES + 1))
	else
		echo "  ok    ${Module}/${Class}: 0 exported members"
	fi
done

# Leg 3. The matcher is asked a question whose answer must be FAIL. If it answers
# anything else, every "ok" above is unearned.
SENTINEL="$(count_members "VaCuusRender" "UVaCuusThisClassDoesNotExist")"
if [[ "${SENTINEL}" != "0" ]]; then
	echo "ABORT: self-test broken — a class that cannot exist reported '${SENTINEL}' members," >&2
	echo "       so the REQUIRED leg above proved nothing." >&2
	exit 2
fi
echo "== self-test: absent class correctly reports 0 (the FAIL path works) =="

if [[ "${FAILURES}" -gt 0 ]]; then
	echo "RESULT: ${FAILURES} violation(s)."
	exit 1
fi
echo "RESULT: clean — the supported C++ surface survives binary delivery."
exit 0
