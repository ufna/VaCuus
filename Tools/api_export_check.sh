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
# LINUX AND macOS, one script and therefore ONE list (bead VaCuus-akj.26). Win64 has its own
# file, api_export_check_win64.ps1, because `dumpbin /exports` shares no flags with `nm` --
# and its lists are duplicated there, which is a drift risk its own comment names. These two
# do NOT duplicate anything: only the toolchain line below differs, so a class promised on one
# of them is promised on both by construction.
#
# WHAT DIFFERS BETWEEN THE TWO, and both halves matter:
#   ELF (Linux)    modules are libUnrealEditor-<M>.so;    GNU nm, `-D --defined-only --demangle`
#   Mach-O (macOS) modules are libUnrealEditor-<M>.dylib; BSD nm has none of those long flags --
#                  `-g` (global) `-U` (suppress undefined), then pipe through c++filt
#
# THE MACH-O TRAP, and it will mislead anyone who checks by hand (it misled the author on the
# first pass): the two FORBIDDEN classes DO have symbols carrying their names in the dylib --
#   vtable for FVaCuusSlateElement
#   vtable for FVaCuusRmlDocumentHost
#   SVaCuusWidget::Construct(..., TSharedRef<FVaCuusSlateElement> const&)
#   VaCuusSlateView::MakeDocumentHost(TSharedRef<FVaCuusSlateElement> const&)
# -- a vtable and two PARAMETER mentions. None of them is a member, and none of them lets a
# consumer do anything: the class definitions stay in Private/, so nothing outside the module
# can name either type. Matching "<Class>::" is what separates a member from a parameter (a
# parameter demangles as `TSharedRef<FVaCuusSlateElement> const&` -- no `::` after the class
# name) and from a vtable (`vtable for X` -- no `::` at all). A raw grep for the mangled name
# reports 3 and 1 instead of 0 and 0. Demangle FIRST, then match; that is why c++filt is not
# optional on the Mach-O leg.
set -u

PKG="${1:-}"
if [[ -z "${PKG}" ]]; then
	echo "usage: bash Tools/api_export_check.sh <package-dir>" >&2
	exit 2
fi

# Fail closed on anything that is not a BuildPlugin package with binaries: a missing
# directory must never read as "no violations found".
if ! compgen -G "${PKG}/*.uplugin" > /dev/null; then
	echo "ABORT: no *.uplugin at '${PKG}' — that is not a BuildPlugin package" >&2
	exit 2
fi

# Which binary flavour is in the package decides the whole toolchain line. Linux first only
# because it is the dev platform; a package carrying both is checked as Linux, and running the
# script on the Mac copy of that package is how you check the other.
if [[ -d "${PKG}/Binaries/Linux" ]]; then
	PLATFORM="Linux"
	BINDIR="${PKG}/Binaries/Linux"
	LIBEXT="so"
elif [[ -d "${PKG}/Binaries/Mac" ]]; then
	PLATFORM="Mac"
	BINDIR="${PKG}/Binaries/Mac"
	LIBEXT="dylib"
else
	echo "ABORT: neither '${PKG}/Binaries/Linux' nor '${PKG}/Binaries/Mac' exists — this package" >&2
	echo "       carries no binaries this script can read, so there is nothing to check." >&2
	echo "       (Source-only packages are the Fab upload shape; comment out FilterPlugin.ini's" >&2
	echo "       -/Binaries/... to get a binary drop. For Win64 use api_export_check_win64.ps1.)" >&2
	exit 2
fi

if ! command -v nm > /dev/null 2>&1; then
	echo "ABORT: nm not found (binutils on Linux, Xcode command line tools on macOS)" >&2
	exit 2
fi
# Only the Mach-O leg needs it: GNU nm demangles itself, BSD nm cannot.
if [[ "${PLATFORM}" == "Mac" ]] && ! command -v c++filt > /dev/null 2>&1; then
	echo "ABORT: c++filt not found — the Mach-O leg cannot separate a member from a parameter" >&2
	echo "       or a vtable without demangling first. See the header." >&2
	exit 2
fi

echo "== ${PLATFORM} (${BINDIR}) =="

# THE SUPPORTED SURFACE, as "module<TAB>class". Adding a class here is how you promise it;
# see VaCuusRender.Build.cs for what is deliberately absent and why.
REQUIRED=(
	"VaCuusRender	UVaCuusWidget"          # screen host — UMG, or any Slate tree via TakeWidget()
	"VaCuusRender	UVaCuusWorldComponent"  # world host — a panel on a quad
	"VaCuus	UVaCuusView"                    # the handle everything is driven through
	"VaCuus	UVaCuusSubsystem"               # owns views; the CreateView extension seam
	"VaCuus	UVaCuusStyleSet"                # RCSS material decorators (rcss-matrix.md)
	"VaCuusRender	SVaCuusWidget"          # the hand-composition door: SUBCLASSED to route
	                                        # input between stacked views (bead VaCuus-akj.25)
)

# Deliberately NOT exported. Zero members is the pass.
#
# THIS LEG SURVIVED akj.25 UNCHANGED, and that is the point of keeping it: the bead added
# VaCuusSlateView::MakeDocumentHost(), which HANDS BACK an FVaCuusRmlDocumentHost, without
# exporting a single one of its members. The factory is a free function; the host crosses as
# the public IVaCuusDocumentHost interface it implements, and the caller can only call
# through that vtable. If a future edit exports the class itself, the render backend has
# become ABI and this line is what says so.
FORBIDDEN=(
	"VaCuusRender	FVaCuusRmlDocumentHost"
	"VaCuusRender	FVaCuusSlateElement"
)

# Counts exported member symbols of Class:: in one module's shared library. Prints the count
# and nothing else, so it can be used by every leg including the self-test.
#
# "Class::" IS THE MATCHER ON BOTH PLATFORMS and it is doing real work, not cosmetics: an
# exported MEMBER demangles as `... UVaCuusView::Foo(void)` while a class used only as a
# PARAMETER reads as `UVaCuusView *` or `TSharedRef<FVaCuusSlateElement> const&` -- no `::`
# after the name -- and a vtable reads `vtable for X`. See the header for the Mach-O case where
# ignoring this turns two zeros into a 3 and a 1.
count_members() {
	local Module="$1" Class="$2" Lib
	Lib="${BINDIR}/libUnrealEditor-${Module}.${LIBEXT}"
	if [[ ! -f "${Lib}" ]]; then
		echo "MISSING_SO"
		return
	fi
	if [[ "${PLATFORM}" == "Mac" ]]; then
		# BSD nm: -g global, -U suppress undefined. It cannot demangle, hence c++filt.
		nm -gU "${Lib}" 2>/dev/null | c++filt | grep -c -- "${Class}::" || true
	else
		nm -D --defined-only --demangle "${Lib}" 2>/dev/null | grep -c -- "${Class}::" || true
	fi
}

FAILURES=0

echo "== supported surface (must be reachable from a buyer's C++) =="
for Entry in "${REQUIRED[@]}"; do
	Module="${Entry%%	*}"
	Class="${Entry##*	}"
	N="$(count_members "${Module}" "${Class}")"
	if [[ "${N}" == "MISSING_SO" ]]; then
		echo "  FAIL  ${Module}/${Class}: libUnrealEditor-${Module}.${LIBEXT} not in the package"
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
		echo "  FAIL  ${Module}/${Class}: libUnrealEditor-${Module}.${LIBEXT} not in the package"
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
