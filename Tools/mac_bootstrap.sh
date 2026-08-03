#!/usr/bin/env bash
# Tools/mac_bootstrap.sh — one command bootstraps the macOS/Metal pass, and refuses
# loudly when the machine cannot produce it.
#
#   bash Tools/mac_bootstrap.sh [options]
#
# WHAT THIS IS. `docs/passport/2026-08-vacuus-macos-plan.md` is the authored plan for the
# first execution of VaCuus on Apple silicon and Metal. This script is that plan's
# **automatable half**: the P0 gates (§2), the machine checks (§2 "Ordered checklist"),
# the clone + LFS gate (§2.6–7), the host project (§2.5), the build (block 1), the
# automation suite (blocks 3–4) and the scripted matrix rows (block 5, and the
# window-position-immune half of block 6). It runs those, and it writes down — by name,
# with the reason — everything it did NOT run, because the plan's own rule is that a row
# which cannot run gets its reason recorded, never silently dropped (plan §1).
#
# WHAT IT DELIBERATELY DOES NOT DO. The interactive blocks stay human: row 5 (IME
# composition), row 13 (live reload), row 12's editor-PIE leg, block 6's HoverShot/TypeShot
# coordinates (they need the window origin — plan block 6), blocks 10–11 (packaging and the
# Shipping column) and every "read the screenshot by eye" verdict. The script cannot
# verify Metal *behaviour*; where the plan says a human must look, it prints exactly what
# to look for and where the image is.
#
# HOUSE SHAPE (Tools/fab_scan.sh, Tools/api_export_check.sh): fail-closed, self-explaining,
# every refusal names what is missing and how to fix it. And, like fab_scan, the parsers
# THIS script's verdicts rest on are seen to fail before they may pass: run_self_tests()
# exercises the version comparator, the Build.version reader, the LFS-pointer detector,
# the automation-result counter and the ini editor against fixtures on every invocation,
# and aborts if any of them answers wrong. Those five self-tests are also the only part of
# this script that can be exercised off a Mac, which is why they exist in this shape.
#
# PORTABILITY CONTRACT. Stock macOS ships bash 3.2 at /bin/bash, and this script must run
# there: no associative arrays, no ${var^^}, no mapfile, no `sort -V`, no GNU-only flags.
# BSD userland only — `sed -i` is avoided entirely (awk + mv instead), `grep -P` is never
# used. Nothing is installed: git-lfs is CHECKED, and if it is missing the install command
# is PRINTED, not run.
#
# The one venue correction this script makes to the plan's own recipes: the plan's item (f)
# and CLAUDE.md both say `pgrep -a UnrealEditor`. `-a` is a GNU/Linux pgrep flag and is not
# in BSD pgrep(1) on macOS; the rule it encodes (kill by PID, never `pkill -f`) is honoured
# here with `pgrep -x` + `ps -p`, which exist on both.
set -euo pipefail

SCRIPT_PATH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
SCRIPT_DIR="$(dirname "${SCRIPT_PATH}")"
PLAN_REL="docs/passport/2026-08-vacuus-macos-plan.md"

# ---------------------------------------------------------------------------------------
# Defaults. Every one of them is overridable; see usage().
# ---------------------------------------------------------------------------------------
WORK_DIR="${VACUUS_MAC_WORK_DIR:-$HOME/VaCuusMacPass}"
PROJECT_NAME="VcHost"                       # plan §2.5: named VcHost so every recorded
                                            # command and Saved/Logs/VcHost.log path
                                            # transfers verbatim from the Linux column.
ENGINE_DIR="${VACUUS_UE_ROOT:-}"
PLUGIN_SRC="git@github.com:ufna/VaCuus.git" # plan §2.6: clone, do not copy, do not symlink.
TEMPLATE="TP_Blank"                         # see ensure_project() for why this, not TP_ThirdPerson.
MIN_FREE_GB=40                              # a script default, NOT a figure from the plan;
                                            # see check_disk().
ALLOW_ENGINE_MISMATCH=0
PREFLIGHT_ONLY=0
WITH_SOAKS=0
METAL_DEBUG_LAYER=0
MAX_BUILD_ERRORS=25
EXPECTED_ENGINE="5.8.1"
# Xcode window, quoted from the engine's own Engine/Config/Apple/Apple_SDK.json:3-5; the max
# is inclusive (UEBuildPlatformSDK.cs:328). Plan §2, checklist item 2.
XCODE_MIN="15.2.0"
XCODE_MAX="26.9.0"
XCODE_MAIN="26.1.1"
MACOS_MIN="14.0"     # plan §2 P0: "≥ 14.0 required".
MACOS_SM6_MIN="15.0" # MetalRHI.cpp:255-267, verified: "Only MacOS 15.0+ can use SM6 with MSC".

usage() {
	cat <<-EOF
	usage: bash Tools/mac_bootstrap.sh [options]

	  --work-dir DIR        where the project, logs and results live   (default: $WORK_DIR)
	  --engine DIR          UE 5.8.1 install root                      (default: \$VACUUS_UE_ROOT,
	                        then /Users/Shared/Epic Games/UE_5.8, then any UE_* there)
	  --plugin-src URL|DIR  what to clone into Plugins/VaCuus          (default: $PLUGIN_SRC)
	  --template NAME       engine template for the host project       (default: $TEMPLATE)
	  --min-free-gb N       refuse below this much free disk           (default: $MIN_FREE_GB)
	  --allow-engine-version-mismatch
	                        proceed on an engine that is not $EXPECTED_ENGINE, stamping the
	                        deviation across the report (plan §2: a different version makes
	                        this a different experiment)
	  --preflight-only      run the checks and stop before anything expensive
	  --with-soaks          also run block 7's four Dev perf soaks and dump their PerfLog
	                        lines as RAW MATERIAL (they are not filled cells — see the report)
	  --metal-debug-layer   run the matrix rows with MTL_DEBUG_LAYER=1 (plan §4 risk 9:
	                        "highest value per keystroke"; expect named aborts, that is the point)
	  --max-build-errors N  how many compile errors to quote into the report (default: $MAX_BUILD_ERRORS)
	  -h | --help

	The reasoning behind every check lives in $PLAN_REL;
	section numbers are cited inline by each check.
	EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
		--work-dir) WORK_DIR="$2"; shift 2 ;;
		--engine) ENGINE_DIR="$2"; shift 2 ;;
		--plugin-src) PLUGIN_SRC="$2"; shift 2 ;;
		--template) TEMPLATE="$2"; shift 2 ;;
		--min-free-gb) MIN_FREE_GB="$2"; shift 2 ;;
		--allow-engine-version-mismatch) ALLOW_ENGINE_MISMATCH=1; shift ;;
		--preflight-only) PREFLIGHT_ONLY=1; shift ;;
		--with-soaks) WITH_SOAKS=1; shift ;;
		--metal-debug-layer) METAL_DEBUG_LAYER=1; shift ;;
		--max-build-errors) MAX_BUILD_ERRORS="$2"; shift 2 ;;
		-h|--help) usage; exit 0 ;;
		*) echo "unknown option '$1'" >&2; usage >&2; exit 3 ;;
	esac
done

PROJ_DIR="${WORK_DIR}/${PROJECT_NAME}"
UPROJECT="${PROJ_DIR}/${PROJECT_NAME}.uproject"
PLUGIN_DIR="${PROJ_DIR}/Plugins/VaCuus"
STAMP="$(date -u +%Y%m%d-%H%M%SZ)"
RESULTS_DIR="${WORK_DIR}/Results/${STAMP}"
REPORT="${RESULTS_DIR}/report.md"
CONSOLE_LOG="${RESULTS_DIR}/console.log"
SHOTS_DIR="${RESULTS_DIR}/shots"
LOGS_DIR="${RESULTS_DIR}/logs"

FAILS=0
CHECKS_RUN=0
FINISHED=0
RUN_ISSUES=0
ISSUES_FILE=""
FEATURE_LEVEL_PREDICTION="unknown"
ENGINE_FLAVOUR="unknown"
ENGINE_VERSION="unknown"

# ---------------------------------------------------------------------------------------
# Output. Everything the human sees also lands in the report, because the report is the
# deliverable and a terminal scrollback is not.
# ---------------------------------------------------------------------------------------
say()  { printf '%s\n' "$*" | tee -a "${CONSOLE_LOG}" ; }
rep()  { printf '%s\n' "$*" >> "${REPORT}" ; }
both() { say "$*"; rep "$*"; }
hr()   { say "-------------------------------------------------------------------------------"; }

banner() {
	say ""
	say "==============================================================================="
	say "== $*"
	say "==============================================================================="
}

# A refusal. Names what is missing, how to fix it, and where the reasoning lives.
die() {
	say ""
	say "*** REFUSED: $1"
	[ -n "${2:-}" ] && say "***    fix: $2"
	[ -n "${3:-}" ] && say "***  where: $3"
	rep ""
	rep "## REFUSED"
	rep ""
	rep "**$1**"
	[ -n "${2:-}" ] && rep "- fix: $2"
	[ -n "${3:-}" ] && rep "- where the reasoning lives: $3"
	exit 1
}

check_pass() {
	CHECKS_RUN=$((CHECKS_RUN + 1))
	say "  PASS  $1${2:+ — $2}"
	rep "| PASS | $1 | ${2:-} |"
}

# check_fail <what> <one-line fix> [<citation>] — records, does not exit; preflight()
# refuses at the end so the human sees every problem in one pass, not one per run.
check_fail() {
	CHECKS_RUN=$((CHECKS_RUN + 1))
	FAILS=$((FAILS + 1))
	say "  FAIL  $1"
	say "        fix: $2"
	[ -n "${3:-}" ] && say "        why: $3"
	rep "| **FAIL** | $1 | fix: $2${3:+ (why: $3)} |"
}

# issue <text> — an automated check that answered wrong. Collected so the top of the report
# can state the verdict instead of burying it 400 lines down, and so a run that finishes
# does not read as a run that passed.
issue() {
	RUN_ISSUES=$((RUN_ISSUES + 1))
	printf -- "- %s\n" "$1" >> "${ISSUES_FILE}"
}

# Replaces the placeholder written by write_header with the run's verdict.
finalise_headline() {
	local head="${RESULTS_DIR}/.headline" tmp="${REPORT}.hl"
	{
		if [ "${FINISHED}" != "1" ]; then
			echo "> **RUN INCOMPLETE.** The script refused or aborted before finishing; see the"
			echo "> REFUSED / RUN INCOMPLETE section below. Nothing here is a pass."
			echo ""
		fi
		if [ "${RUN_ISSUES}" -gt 0 ]; then
			echo "> **${RUN_ISSUES} automated check(s) answered wrong:**"
			echo ">"
			LC_ALL=C sed 's/^/> /' "${ISSUES_FILE}" 2>/dev/null || true
		elif [ "${FINISHED}" = "1" ]; then
			echo "> **Every automated check the script can make, passed.** That is a much smaller"
			echo "> claim than \"the macOS column is filled\" — read \"What this script did NOT run\"."
		fi
	} > "${head}"
	awk -v hf="${head}" '{
		if ($0 == "<!-- HEADLINE -->") { while ((getline l < hf) > 0) print l; close(hf) }
		else print
	}' "${REPORT}" > "${tmp}" 2>/dev/null && mv "${tmp}" "${REPORT}"
	rm -f "${head}"
}

on_exit() {
	local rc=$?
	if [ "${FINISHED}" != "1" ]; then
		rep ""
		rep "## RUN INCOMPLETE"
		rep ""
		rep "The script exited (status ${rc}) before finishing. Everything below the last"
		rep "section above was NOT run. Fix the refusal and re-run — the script is"
		rep "idempotent: the clone, the project and the config edits are all reused in place."
		write_not_run_section || true
	fi
	finalise_headline || true
	if [ -f "${REPORT}" ]; then
		if [ "${RUN_ISSUES}" -gt 0 ]; then
			printf '\n%s automated check(s) answered wrong — see the headline at the top of the report.\n' "${RUN_ISSUES}"
		fi
		printf '\nreport: %s\n' "${REPORT}"
	fi
}

# ---------------------------------------------------------------------------------------
# Parsers — each one is a verdict this script rests on, so each one is self-tested below.
# ---------------------------------------------------------------------------------------

# BSD mktemp(1) requires an explicit template ("mktemp" with no argument is a GNU
# extension and fails on stock macOS); GNU mktemp accepts one. These two wrappers are the
# spelling that works on both.
mktmp()  { mktemp "${TMPDIR:-/tmp}/vacuus_mb.XXXXXXXX"; }
mktmpd() { mktemp -d "${TMPDIR:-/tmp}/vacuus_mb.XXXXXXXX"; }

# version_cmp A B -> "lt" | "eq" | "gt". Dotted numeric compare, missing fields are 0.
version_cmp() {
	awk -v a="$1" -v b="$2" 'BEGIN {
		na = split(a, A, "."); nb = split(b, B, ".");
		n = (na > nb ? na : nb);
		for (i = 1; i <= n; i++) {
			x = (i <= na ? A[i] + 0 : 0); y = (i <= nb ? B[i] + 0 : 0);
			if (x < y) { print "lt"; exit }
			if (x > y) { print "gt"; exit }
		}
		print "eq"
	}'
}

version_ge() { [ "$(version_cmp "$1" "$2")" != "lt" ]; }

# engine_version_of <engine-root> -> "5.8.1" | "MISSING". Reads the engine's own
# Engine/Build/Build.version — the plan's rule (§2) is that the version is verified from
# the engine, not from the directory name the Launcher happened to use.
engine_version_of() {
	local bv="$1/Engine/Build/Build.version" maj min pat
	[ -f "${bv}" ] || { echo "MISSING"; return 0; }
	maj="$(LC_ALL=C sed -n 's/.*"MajorVersion"[^0-9]*\([0-9][0-9]*\).*/\1/p' "${bv}" | head -1)"
	min="$(LC_ALL=C sed -n 's/.*"MinorVersion"[^0-9]*\([0-9][0-9]*\).*/\1/p' "${bv}" | head -1)"
	pat="$(LC_ALL=C sed -n 's/.*"PatchVersion"[^0-9]*\([0-9][0-9]*\).*/\1/p' "${bv}" | head -1)"
	if [ -z "${maj}" ] || [ -z "${min}" ] || [ -z "${pat}" ]; then echo "MISSING"; return 0; fi
	echo "${maj}.${min}.${pat}"
}

# is_lfs_pointer <file> — true when the file is a git-lfs pointer instead of its payload.
# This is the shape the plan §2.7 warns about: the 129-byte pointer still EXISTS, so the
# VFS resolves it (VaCuusEngine.cpp:140-141), Rml::LoadFontFace fails on a text file, and
# the code takes the :145-147 Warning path — a silent, textless HUD.
is_lfs_pointer() {
	head -c 200 "$1" 2>/dev/null | LC_ALL=C grep -q 'git-lfs\.github\.com/spec/v1'
}

# count_results <log> -> "<success> <nonsuccess>", read from the log file and never from
# stdout (CLAUDE.md; plan §3 item (g) — an interleaved UnrealTraceServer fork clobbers the
# tail of every run's stdout).
count_results() {
	local log="$1" ok bad
	ok="$(LC_ALL=C grep -c 'Test Completed\. Result={Success}' "${log}" 2>/dev/null || true)"
	bad="$(LC_ALL=C grep 'Test Completed\. Result=' "${log}" 2>/dev/null | LC_ALL=C grep -vc 'Result={Success}' || true)"
	echo "${ok:-0} ${bad:-0}"
}

# ini_set <file> <section> <key> <value> — idempotent single-key assignment. Replaces the
# key inside the section if it is there, inserts it into the section if the section is
# there, appends both if neither is. Re-running the script must not duplicate lines, which
# is what makes step 3 and the row-12 A/B safe to repeat.
ini_set() {
	local file="$1" sec="$2" key="$3" val="$4" tmp
	tmp="${file}.mb.$$"
	[ -f "${file}" ] || : > "${file}"
	# Blank lines are buffered so an inserted key lands at the end of its section's CONTENT,
	# not after the blank line that separates it from the next section. UE would parse it
	# either way; a human reading Config/DefaultEngine.ini afterwards would not.
	awk -v sec="${sec}" -v key="${key}" -v val="${val}" '
		function norm(s) { gsub(/\r/, "", s); gsub(/^[ \t]+|[ \t]+$/, "", s); return s }
		function flush(   i) { for (i = 1; i <= nb; i++) print blanks[i]; nb = 0 }
		# The key goes into a regex, so its ERE metacharacters are escaped first. UE ini
		# keys are identifiers plus dots (r.DefaultBackBufferPixelFormat), but the class is
		# written out so a future key with a bracket cannot silently match the wrong line.
		# "]" must come first inside a POSIX bracket expression.
		BEGIN { insec = 0; done = 0; nb = 0
		        keyre = key; gsub(/[].[*+?()^$|]/, "\\\\&", keyre) }
		{
			line = $0
			if (norm(line) == "") { blanks[++nb] = line; next }
			if (norm(line) ~ /^\[.*\]$/) {
				if (insec && !done) { print key "=" val; done = 1 }
				flush()
				insec = (norm(line) == "[" sec "]") ? 1 : 0
				print line
				next
			}
			flush()
			if (insec && !done && norm(line) ~ ("^" keyre "[ \t]*=")) {
				print key "=" val; done = 1; next
			}
			print line
		}
		END {
			if (insec && !done) { print key "=" val; done = 1 }
			flush()
			if (!done) { print ""; print "[" sec "]"; print key "=" val }
		}
	' "${file}" > "${tmp}"
	mv "${tmp}" "${file}"
}

# ini_add_line <file> <section> <line> — for UE's `+Array=` accumulator keys, where the
# assignment above is wrong. Appends inside the section unless the exact line already
# exists anywhere in the file.
ini_add_line() {
	local file="$1" sec="$2" line="$3" tmp
	[ -f "${file}" ] || : > "${file}"
	if LC_ALL=C grep -Fxq "${line}" "${file}"; then return 0; fi
	tmp="${file}.mb.$$"
	awk -v sec="${sec}" -v newline="${line}" '
		function norm(s) { gsub(/\r/, "", s); gsub(/^[ \t]+|[ \t]+$/, "", s); return s }
		function flush(   i) { for (i = 1; i <= nb; i++) print blanks[i]; nb = 0 }
		BEGIN { insec = 0; done = 0; nb = 0 }
		{
			l = $0
			if (norm(l) == "") { blanks[++nb] = l; next }
			if (norm(l) ~ /^\[.*\]$/) {
				if (insec && !done) { print newline; done = 1 }
				flush()
				insec = (norm(l) == "[" sec "]") ? 1 : 0
				print l; next
			}
			flush()
			print l
		}
		END {
			if (insec && !done) { print newline; done = 1 }
			flush()
			if (!done) { print ""; print "[" sec "]"; print newline }
		}
	' "${file}" > "${tmp}"
	mv "${tmp}" "${file}"
}

# ---------------------------------------------------------------------------------------
# The self-tests. fab_scan.sh's rule, same reasoning: a parser that has never been seen to
# answer wrong is not yet evidence. These run on every invocation and cost milliseconds —
# and they are the only part of this script that can be exercised off a Mac.
# ---------------------------------------------------------------------------------------
selftest_fail() {
	say "SELF-TEST FAILED: $1"
	say "  expected: $2"
	say "  got:      $3"
	exit 2
}

run_self_tests() {
	local t d got want

	# 1. version_cmp — the Xcode window is INCLUSIVE at both ends (Apple_SDK.json:3-5,
	#    UEBuildPlatformSDK.cs:328), so "eq at the max" must not read as out of range.
	got="$(version_cmp 15.1.0 15.2.0)"; [ "${got}" = "lt" ] || selftest_fail "version_cmp 15.1.0 15.2.0" "lt" "${got}"
	got="$(version_cmp 26.9.0 26.9.0)"; [ "${got}" = "eq" ] || selftest_fail "version_cmp 26.9.0 26.9.0" "eq" "${got}"
	got="$(version_cmp 26.10 26.9.0)";  [ "${got}" = "gt" ] || selftest_fail "version_cmp 26.10 26.9.0" "gt" "${got}"
	got="$(version_cmp 26.1.1 15.2.0)"; [ "${got}" = "gt" ] || selftest_fail "version_cmp 26.1.1 15.2.0" "gt" "${got}"
	got="$(version_cmp 14 14.0.0)";     [ "${got}" = "eq" ] || selftest_fail "version_cmp 14 14.0.0" "eq" "${got}"

	# 2. engine_version_of — must read the JSON, and must answer MISSING (never a
	#    vacuous pass) when the file is not there.
	d="$(mktmpd)"; mkdir -p "${d}/Engine/Build"
	cat > "${d}/Engine/Build/Build.version" <<-'EOF'
	{
		"MajorVersion": 5,
		"MinorVersion": 8,
		"PatchVersion": 1,
		"Changelist": 0
	}
	EOF
	got="$(engine_version_of "${d}")"; [ "${got}" = "5.8.1" ] || selftest_fail "engine_version_of" "5.8.1" "${got}"
	got="$(engine_version_of "${d}/nope")"; [ "${got}" = "MISSING" ] || selftest_fail "engine_version_of(absent)" "MISSING" "${got}"
	rm -rf "${d}"

	# 3. is_lfs_pointer — the pointer must be caught and a real payload must not be, or
	#    the LFS gate would either bless a textless HUD or refuse a good checkout.
	d="$(mktmpd)"
	cat > "${d}/pointer" <<-'EOF'
	version https://git-lfs.github.com/spec/v1
	oid sha256:0000000000000000000000000000000000000000000000000000000000000000
	size 12345
	EOF
	printf '\000\001\000\000OTTO binary payload' > "${d}/payload"
	is_lfs_pointer "${d}/pointer" || selftest_fail "is_lfs_pointer(pointer)" "detected" "not detected"
	if is_lfs_pointer "${d}/payload"; then selftest_fail "is_lfs_pointer(payload)" "not detected" "detected"; fi
	rm -rf "${d}"

	# 4. count_results — a log with a failure must never read as clean.
	t="$(mktmp)"
	cat > "${t}" <<-'EOF'
	LogAutomationController: Display: Test Completed. Result={Success} Name={A} Path={VaCuus.A}
	LogAutomationController: Display: Test Completed. Result={Fail} Name={B} Path={VaCuus.B}
	LogAutomationController: Display: Test Completed. Result={Success} Name={C} Path={VaCuus.C}
	LogAutomationController: Display: Sending StopTestSession to X
	EOF
	got="$(count_results "${t}")"; [ "${got}" = "2 1" ] || selftest_fail "count_results" "2 1" "${got}"
	rm -f "${t}"

	# 5. ini_set / ini_add_line — replace in place, insert into an existing section, create
	#    a missing one, and stay idempotent across a re-run (step 3 and row 12 depend on it).
	d="$(mktmpd)"; t="${d}/Test.ini"
	cat > "${t}" <<-'EOF'
	[/Script/EngineSettings.GameMapsSettings]
	GameDefaultMap=/Game/Old/Map
	EditorStartupMap=/Game/Old/Map

	[/Script/Engine.RendererSettings]
	r.AllowStaticLighting=False
	EOF
	ini_set "${t}" "/Script/EngineSettings.GameMapsSettings" "GameDefaultMap" "/Engine/Maps/Templates/Template_Default"
	ini_set "${t}" "/Script/Engine.RendererSettings" "r.DefaultBackBufferPixelFormat" "3"
	ini_set "${t}" "/Script/MacTargetPlatform.XcodeProjectSettings" "bMacSignToRunLocally" "True"
	ini_add_line "${t}" "/Script/UnrealEd.ProjectPackagingSettings" '+DirectoriesToAlwaysCook=(Path="/Game/Bundles")'
	want="$(cat "${t}")"
	# Idempotence: the same four calls again must change nothing at all.
	ini_set "${t}" "/Script/EngineSettings.GameMapsSettings" "GameDefaultMap" "/Engine/Maps/Templates/Template_Default"
	ini_set "${t}" "/Script/Engine.RendererSettings" "r.DefaultBackBufferPixelFormat" "3"
	ini_set "${t}" "/Script/MacTargetPlatform.XcodeProjectSettings" "bMacSignToRunLocally" "True"
	ini_add_line "${t}" "/Script/UnrealEd.ProjectPackagingSettings" '+DirectoriesToAlwaysCook=(Path="/Game/Bundles")'
	got="$(cat "${t}")"
	[ "${got}" = "${want}" ] || selftest_fail "ini_set idempotence" "unchanged file" "file changed on re-run"
	got="$(LC_ALL=C grep -c '^GameDefaultMap=' "${t}")"
	[ "${got}" = "1" ] || selftest_fail "ini_set replaced in place" "1 GameDefaultMap line" "${got}"
	got="$(LC_ALL=C grep '^GameDefaultMap=' "${t}")"
	[ "${got}" = "GameDefaultMap=/Engine/Maps/Templates/Template_Default" ] || selftest_fail "ini_set value" "Template_Default" "${got}"
	got="$(LC_ALL=C grep -c '^EditorStartupMap=/Game/Old/Map' "${t}")"
	[ "${got}" = "1" ] || selftest_fail "ini_set left neighbours alone" "1" "${got}"
	got="$(LC_ALL=C grep -c '^r.AllowStaticLighting=False' "${t}")"
	[ "${got}" = "1" ] || selftest_fail "ini_set left other sections alone" "1" "${got}"
	rm -rf "${d}"

	say "== self-tests OK: the version comparator, the Build.version reader, the LFS-pointer"
	say "   detector, the automation counter and the ini editor were each shown a wrong answer"
	say "   they must not give (an out-of-range version read as in-range, an absent engine read"
	say "   as a version, a pointer read as a payload, a failing suite read as clean, a re-run"
	say "   read as a duplicate line) and rejected it =="
}

# ---------------------------------------------------------------------------------------
# 1. PREFLIGHT — before anything expensive. Every check prints PASS/FAIL and a one-line fix.
# ---------------------------------------------------------------------------------------

check_platform_and_arch() {
	local uname_s uname_m translated chip

	uname_s="$(uname -s)"
	if [ "${uname_s}" != "Darwin" ]; then
		say "  FAIL  this is not macOS (uname -s = ${uname_s})"
		die "mac_bootstrap.sh bootstraps the macOS/Metal pass and must run ON the MacBook." \
		    "copy the repo to the Mac and run it there" \
		    "${PLAN_REL} §1 — the pass exists to fill the macOS Metal columns"
	fi
	check_pass "macOS host (uname -s = Darwin)"

	uname_m="$(uname -m)"
	if [ "${uname_m}" = "arm64" ]; then
		check_pass "Apple silicon (uname -m = arm64)" "plan §2 P0"
	else
		# x86_64 is either a Rosetta shell on Apple silicon (recoverable) or a genuine
		# Intel Mac (not recoverable — see below).
		translated="$(sysctl -n sysctl.proc_translated 2>/dev/null || echo "absent")"
		if [ "${translated}" = "1" ]; then
			say "  FAIL  running under Rosetta (uname -m = x86_64, sysctl.proc_translated = 1)"
			die "this shell is translated; the pass must run natively." \
			    "re-run in a native shell: arch -arm64 /bin/bash ${SCRIPT_PATH}" \
			    "${PLAN_REL} §2 P0 — 'must print 0 (not a Rosetta shell)'"
		fi
		say "  FAIL  Intel Mac (uname -m = x86_64, sysctl.proc_translated = ${translated})"
		rep ""
		rep "### ABORT — Intel Mac"
		rep ""
		rep "UE 5.8 has **removed** Mac rendering on Intel. \`MetalDevice.cpp:202-209\` opens a modal"
		rep "(\"Rendering support for Intel based Mac has been removed in this version of the engine.\")"
		rep "and calls \`RequestExit\`; \`MetalRHI.cpp:408\` re-asserts it as a hard \`check()\`."
		rep "\`GPUFamilyApple7\` is M1 and newer. There is no workaround and no flag."
		rep ""
		rep "**The macOS column cannot be produced on this machine.** It needs a different Mac"
		rep "(Apple silicon). Recorded, not dropped — ${PLAN_REL} §2 P0."
		die "UE 5.8 removed Mac rendering on Intel: MetalDevice.cpp:202-209 opens a modal and calls RequestExit; MetalRHI.cpp:408 re-asserts it as a hard check(). GPUFamilyApple7 is M1 and newer — there is no workaround and no flag. The macOS column cannot be produced on this machine." \
		    "run the pass on an Apple-silicon Mac; nothing else in this script can substitute" \
		    "${PLAN_REL} §2 P0"
	fi

	chip="$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo "unknown")"
	say "        chip: ${chip}"
	rep "| info | CPU | ${chip} |"
}

check_macos_version() {
	local v major sm6_capable chip
	v="$(sw_vers -productVersion 2>/dev/null || echo "0")"
	chip="$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo "unknown")"

	if version_ge "${v}" "${MACOS_MIN}"; then
		check_pass "macOS ${v} (≥ ${MACOS_MIN})" "plan §2 P0"
	else
		check_fail "macOS ${v} is below the ${MACOS_MIN} this pass requires" \
		           "update macOS to ${MACOS_MIN} or newer" \
		           "plan §2 P0"
		return
	fi

	# Feature level. Mac SM6 needs macOS >= 15.0 AND GPUFamilyApple8 (M2+) —
	# MetalRHI.cpp:255-267, verified: "Only MacOS 15.0+ can use SM6 with MSC" and
	# supportsFamily(MTL::GPUFamilyApple8). Otherwise :421-427 logs the fallback Warning
	# and :433-441 pins SP_METAL_SM5.
	sm6_capable=1
	version_ge "${v}" "${MACOS_SM6_MIN}" || sm6_capable=0
	# M1 is GPUFamilyApple7, not Apple8. The two patterns are "Apple M1" exactly (bare M1)
	# and "Apple M1 " followed by Pro/Max/Ultra — written this way so a future "Apple M10"
	# is not swept in by a bare prefix match.
	case "${chip}" in
		*"Apple M1"|*"Apple M1 "*) sm6_capable=0 ;;
	esac
	if [ "${sm6_capable}" = "1" ]; then
		FEATURE_LEVEL_PREDICTION="SM6 (expected)"
	else
		FEATURE_LEVEL_PREDICTION="SM5 (expected — macOS < ${MACOS_SM6_MIN} or an M1-class GPU)"
	fi
	say "        feature level this machine should bind: ${FEATURE_LEVEL_PREDICTION}"
	say "        NOTE: an SM5 run and an SM6 run are DIFFERENT CELLS for matrix rows 8 and 12"
	say "        (plan §2 checklist 11). This is a prediction; the log is the arbiter, and this"
	say "        script greps it for the 'falling back to SM5' Warning after the first real-RHI run."
	rep "| info | feature level (predicted) | ${FEATURE_LEVEL_PREDICTION} — an SM5 run and an SM6 run are different cells for rows 8 and 12 (plan §2.11) |"
}

check_xcode() {
	local dev ver ok
	if ! command -v xcode-select >/dev/null 2>&1; then
		check_fail "xcode-select not found" "install Xcode from the App Store, then launch it once" "plan §2 checklist 2"
		return
	fi
	dev="$(xcode-select -p 2>/dev/null || echo "")"
	if [ -z "${dev}" ]; then
		check_fail "no developer directory selected" \
		           "sudo xcode-select -s /Applications/Xcode.app" "plan §2 checklist 2"
		return
	fi
	case "${dev}" in
		*CommandLineTools*)
			check_fail "the selected developer dir is CommandLineTools (${dev}) — a hard fail, not a warning" \
			           "sudo xcode-select -s /Applications/Xcode.app" \
			           "plan §2 checklist 2: AppleToolChainSettings.cs:155-161 throws a BuildException AND ApplePlatformSDK.cs:102-110 reads \$(xcode-select -p)/../Info.plist, so version detection returns null"
			return ;;
	esac
	check_pass "full Xcode selected" "${dev}"

	if ! command -v xcodebuild >/dev/null 2>&1; then
		check_fail "xcodebuild not found under ${dev}" "sudo xcode-select -s /Applications/Xcode.app" "plan §2 checklist 2"
		return
	fi
	ver="$(xcodebuild -version 2>/dev/null | head -1 | awk '{print $2}')"
	if [ -z "${ver}" ]; then
		check_fail "xcodebuild -version printed no version (usually the licence prompt)" \
		           "sudo xcodebuild -license accept" "plan §2 checklist 2"
		return
	fi
	ok=1
	version_ge "${ver}" "${XCODE_MIN}" || ok=0
	version_ge "${XCODE_MAX}" "${ver}" || ok=0
	if [ "${ok}" = "1" ]; then
		check_pass "Xcode ${ver} inside the engine's window [${XCODE_MIN} … ${XCODE_MAX}] (main ${XCODE_MAIN})" \
		           "Apple_SDK.json:3-5, max inclusive per UEBuildPlatformSDK.cs:328"
	else
		check_fail "Xcode ${ver} is outside the engine's window [${XCODE_MIN} … ${XCODE_MAX}]" \
		           "install an Xcode inside that window (the engine's MainVersion is ${XCODE_MAIN})" \
		           "Apple_SDK.json:3-5; plan §2 checklist 2"
	fi

	if xcodebuild -checkFirstLaunchStatus >/dev/null 2>&1; then
		check_pass "Xcode first-launch/licence state accepted"
	else
		check_fail "Xcode first-launch components are not installed or the licence is not accepted" \
		           "launch Xcode.app once and accept, or: sudo xcodebuild -runFirstLaunch && sudo xcodebuild -license accept" \
		           "plan §2 checklist 2"
	fi
}

check_git_lfs() {
	if ! command -v git >/dev/null 2>&1; then
		check_fail "git not found" "brew install git   (or install the Xcode command line tools)" "plan §2 checklist 3"
		return
	fi
	check_pass "git present" "$(git --version 2>/dev/null)"

	if ! git lfs version >/dev/null 2>&1; then
		check_fail "git-lfs not installed — and its absence fails SILENTLY, as a Warning" \
		           "brew install git-lfs && git lfs install   (this script will NOT install it for you)" \
		           "plan §2 checklist 7: the 129-byte pointer still exists, so ResolveExistingDocument succeeds (VaCuusEngine.cpp:140-141), Rml::LoadFontFace fails on a text file and the code takes the :145-147 Warning path — the HUD renders WITH NO TEXT, ~54k 'No font face defined' warnings per suite run, and rows 1,2,3,4,6,9,11 are silently void"
		return
	fi
	check_pass "git-lfs present" "$(git lfs version 2>/dev/null | head -1)"

	if [ -z "$(git config --get filter.lfs.smudge || true)" ]; then
		check_fail "git-lfs is installed but not initialised (no filter.lfs.smudge)" \
		           "git lfs install" \
		           "plan §2 checklist 7 — without the smudge filter the clone materialises pointers, not payloads, and every demo is textless"
	else
		check_pass "git-lfs initialised (filter.lfs.smudge configured)"
	fi
}

check_engine() {
	local candidates c v found others
	candidates=""
	[ -n "${ENGINE_DIR}" ] && candidates="${ENGINE_DIR}"
	candidates="${candidates}
/Users/Shared/Epic Games/UE_5.8"
	# Any other Launcher install, so the refusal can name what IS there.
	if [ -d "/Users/Shared/Epic Games" ]; then
		for c in "/Users/Shared/Epic Games"/UE_*; do
			[ -d "${c}" ] && candidates="${candidates}
${c}"
		done
	fi

	found=""
	others=""
	while IFS= read -r c; do
		[ -z "${c}" ] && continue
		[ -d "${c}" ] || continue
		v="$(engine_version_of "${c}")"
		[ "${v}" = "MISSING" ] && continue
		if [ "${v}" = "${EXPECTED_ENGINE}" ] && [ -z "${found}" ]; then
			found="${c}"; ENGINE_VERSION="${v}"
		else
			others="${others}
  ${c}  -> ${v}"
		fi
	done <<-EOF
	${candidates}
	EOF

	if [ -z "${found}" ]; then
		# Nothing at 5.8.1. Either refuse (default) or record the deviation loudly. The
		# deviation path is NOT a silent skip: it is counted as a check, printed as a
		# DEVIATION, and stamped across the report — plan §2, "a 5.8.0 or 5.9 install makes
		# this a different experiment".
		if [ "${ALLOW_ENGINE_MISMATCH}" = "1" ] && [ -n "${ENGINE_DIR}" ] && [ -d "${ENGINE_DIR}" ]; then
			ENGINE_VERSION="$(engine_version_of "${ENGINE_DIR}")"
			found="${ENGINE_DIR}"
			CHECKS_RUN=$((CHECKS_RUN + 1))
			say "  DEVI  engine at ${found} is ${ENGINE_VERSION}, not ${EXPECTED_ENGINE}"
			say "        --allow-engine-version-mismatch was passed, so the run PROCEEDS —"
			say "        but this is a DIFFERENT EXPERIMENT and every Method sentence must name it."
			rep "| **DEVIATION** | engine version | ${ENGINE_VERSION}, not ${EXPECTED_ENGINE} — proceeding under \`--allow-engine-version-mismatch\`; every Method sentence in this report must name it (plan §2) |"
		else
			check_fail "no UE ${EXPECTED_ENGINE} install found${others:+; what IS installed:${others}}" \
			           "install UE ${EXPECTED_ENGINE} via the Epic Launcher, or pass --engine /path/to/UE_5.8, or --allow-engine-version-mismatch to record a different version as a deviation" \
			           "plan §2: the version is verified from the engine's own Engine/Build/Build.version, and a different version makes this a different experiment"
			return
		fi
	else
		check_pass "UE ${ENGINE_VERSION} at ${found}" "verified from Engine/Build/Build.version"
	fi

	ENGINE_DIR="${found}"

	if [ -f "${ENGINE_DIR}/Engine/Build/InstalledBuild.txt" ]; then
		ENGINE_FLAVOUR="Launcher binary (Installed build)"
	else
		ENGINE_FLAVOUR="source build"
	fi
	check_pass "engine flavour: ${ENGINE_FLAVOUR}" "plan §2 'The engine decision' — name it in every Method sentence"
	rep "| info | engine | ${ENGINE_VERSION}, ${ENGINE_FLAVOUR}, at \`${ENGINE_DIR}\` |"

	if [ ! -x "${ENGINE_DIR}/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor" ]; then
		check_fail "the Mac editor binary is missing at Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor" \
		           "install the macOS engine (the Launcher's Mac platform), not only the sources" \
		           "plan §3 (a): there is no UnrealEditor-Cmd on Mac; the inner binary is the one to run (CommandletUtils.cs:495)"
	else
		check_pass "Mac editor binary present"
	fi
	if [ ! -f "${ENGINE_DIR}/Engine/Build/BatchFiles/Mac/Build.sh" ]; then
		check_fail "Engine/Build/BatchFiles/Mac/Build.sh missing" "reinstall the engine" "plan block 1"
	else
		check_pass "Mac Build.sh present" "an Installed engine still runs UBT from it (Mac/Build.sh:11 skips only the UBT/SCW self-build; :35 runs UBT)"
	fi
	if [ ! -d "${ENGINE_DIR}/Templates/${TEMPLATE}" ]; then
		check_fail "template ${TEMPLATE} not found under ${ENGINE_DIR}/Templates" \
		           "pass --template <name> naming a template that is installed" \
		           "plan §2 checklist 5"
	else
		check_pass "template ${TEMPLATE} present"
	fi
}

check_disk() {
	local avail_kb avail_gb
	mkdir -p "${WORK_DIR}"
	# -P is POSIX output: one line per filesystem, so a long device name cannot wrap the
	# columns and shift $4. Present in both BSD and GNU df.
	avail_kb="$(df -Pk "${WORK_DIR}" | awk 'NR==2 {print $4}')"
	avail_gb=$((avail_kb / 1024 / 1024))
	if [ "${avail_gb}" -ge "${MIN_FREE_GB}" ]; then
		check_pass "${avail_gb} GB free at ${WORK_DIR} (threshold ${MIN_FREE_GB} GB)"
	else
		check_fail "only ${avail_gb} GB free at ${WORK_DIR}; the threshold is ${MIN_FREE_GB} GB" \
		           "free space, or point --work-dir at a bigger volume, or lower --min-free-gb" \
		           "the threshold is a SCRIPT DEFAULT, not a measured figure from the plan: it covers the project, Intermediate/ and Binaries/ for five plugin modules, plus a DDC-cold Metal shader compile that builds every permutation TWICE (SF_METAL_SM5 + SF_METAL_SM6 — plan block 2)"
	fi
}

# check_no_editor_running [quiet] — `quiet` skips the report row, for the callers that run
# outside preflight's table (a stray "| PASS |" line in the middle of prose breaks it).
check_no_editor_running() {
	local quiet="${1:-}" pids pidlist
	# `pgrep -a` is a GNU flag and does not exist in BSD pgrep(1) on macOS, so the plan's
	# item (f) and CLAUDE.md's recipe are spelled here with `pgrep -x` + `ps -p`, which
	# exist on both. The rule they encode — kill BY PID, never `pkill -f` — is unchanged.
	pids="$(pgrep -x UnrealEditor 2>/dev/null || true)"
	if [ -n "${pids}" ]; then
		pidlist="$(printf '%s' "${pids}" | tr '\n' ',' | sed 's/,$//')"
		say "  FAIL  an UnrealEditor is already running:"
		ps -p "${pidlist}" -o pid=,command= 2>/dev/null | sed 's/^/        /' | tee -a "${CONSOLE_LOG}"
		if [ "${quiet}" = "quiet" ]; then
			say "        kill it BY PID (kill ${pidlist}) — never 'pkill -f UnrealEditor'"
		else
			check_fail "an UnrealEditor process is running and holds the plugin's .dylib" \
			           "kill it BY PID (kill ${pidlist}) — never 'pkill -f UnrealEditor': the pattern matches your own shell wrapper and kills your shell" \
			           "CLAUDE.md dev-loop hazards; plan §3 (f)"
		fi
		return 1
	fi
	[ "${quiet}" = "quiet" ] || check_pass "no UnrealEditor running" "nothing holds the plugin's .dylib"
	return 0
}

preflight() {
	banner "1. PREFLIGHT — nothing expensive happens until every one of these passes"
	rep ""
	rep "## 1. Preflight"
	rep ""
	rep "| result | check | detail |"
	rep "|---|---|---|"

	check_platform_and_arch
	check_macos_version
	check_xcode
	check_git_lfs
	check_engine
	check_disk
	check_no_editor_running || true

	hr
	if [ "${FAILS}" -gt 0 ]; then
		say "PREFLIGHT: ${FAILS} of ${CHECKS_RUN} checks FAILED (listed above, each with its fix)."
		rep ""
		rep "**Preflight refused: ${FAILS} of ${CHECKS_RUN} checks failed.** Nothing after this point ran."
		die "preflight failed ${FAILS} check(s)" \
		    "fix the FAIL lines above and re-run — this script is idempotent and needs no cleanup" \
		    "${PLAN_REL} §2"
	fi
	say "PREFLIGHT: all ${CHECKS_RUN} checks passed."
	rep ""
	rep "All ${CHECKS_RUN} preflight checks passed."
}

# ---------------------------------------------------------------------------------------
# 2. FETCH THE PLUGIN + the LFS gate.
# ---------------------------------------------------------------------------------------
fetch_plugin() {
	banner "2. PLUGIN — clone into Plugins/VaCuus, then the LFS gate"
	rep ""
	rep "## 2. Plugin checkout and the LFS gate"
	rep ""

	mkdir -p "${PROJ_DIR}/Plugins"

	if [ -e "${PLUGIN_DIR}" ]; then
		if [ ! -d "${PLUGIN_DIR}/.git" ]; then
			die "${PLUGIN_DIR} exists but is not a git checkout" \
			    "move or delete it and re-run; the plan requires a CLONE (a copy drags untracked Binaries/ and Intermediate/; a symlink hits the UBA cross-process rename-while-open abort)" \
			    "${PLAN_REL} §2 checklist 6"
		fi
		say "reusing the existing checkout at ${PLUGIN_DIR} (idempotent re-run)"
		rep "- Reused the existing checkout at \`${PLUGIN_DIR}\` (no clobber: a re-run never discards local work)."
	else
		say "cloning ${PLUGIN_SRC} -> ${PLUGIN_DIR}"
		if ! git clone "${PLUGIN_SRC}" "${PLUGIN_DIR}" 2>&1 | tee -a "${CONSOLE_LOG}"; then
			die "git clone of ${PLUGIN_SRC} failed" \
			    "check the SSH key (ssh -T git@github.com), or pass --plugin-src with an https URL or a local path" \
			    "${PLAN_REL} §2 checklist 6"
		fi
		rep "- Cloned \`${PLUGIN_SRC}\` into \`${PLUGIN_DIR}\` (plan §2.6: clone, do not copy, do not symlink)."
	fi

	say "git lfs pull …"
	( cd "${PLUGIN_DIR}" && git lfs pull ) 2>&1 | tee -a "${CONSOLE_LOG}" || \
		die "git lfs pull failed in ${PLUGIN_DIR}" \
		    "check network and LFS access, then re-run" "${PLAN_REL} §2 checklist 7"

	local head_sha
	head_sha="$(cd "${PLUGIN_DIR}" && git rev-parse --short HEAD)"
	say "plugin HEAD: ${head_sha}"
	rep "- Plugin HEAD: \`${head_sha}\`"
	rep ""

	lfs_gate
}

# The plan calls this "the LFS gate — do not skip this one" (§2 checklist 7). A missing
# smudge is a Warning, not an error, so the only honest check is on the bytes.
lfs_gate() {
	local total starred pointers bad_pointers f n
	local fixture="Tools/scan-fixture/planted_lfs_pointer.txt"

	rep "### LFS gate (plan §2 checklist 7)"
	rep ""

	cd "${PLUGIN_DIR}"
	total="$(git lfs ls-files | wc -l | tr -d ' ')"
	starred="$(git lfs ls-files | awk '$2 == "*"' | wc -l | tr -d ' ')"
	pointers="$(git lfs ls-files | awk '$2 == "-" {print $3}')"

	say "git lfs ls-files: ${total} tracked, ${starred} materialised ('*')"
	rep "- \`git lfs ls-files\`: **${total}** tracked, **${starred}** materialised (\`*\`)."

	bad_pointers=""
	for f in ${pointers}; do
		if [ "${f}" = "${fixture}" ]; then
			say "  (permitted) ${f} — the deliberately committed pointer fixture (Tools/fab_scan.sh:22-23,119-126,170)"
			rep "- Permitted pointer: \`${fixture}\` — the deliberately committed scan fixture; fab_scan.sh reporting it is that script's self-test, not a failure."
		else
			bad_pointers="${bad_pointers} ${f}"
		fi
	done
	if [ -n "${bad_pointers}" ]; then
		rep "- **FAIL** — unmaterialised payloads:${bad_pointers}"
		die "these LFS files are still pointers:${bad_pointers}" \
		    "git lfs install && git lfs pull, then re-run" \
		    "${PLAN_REL} §2 checklist 7 — a pointer renders every demo TEXTLESS and voids rows 1,2,3,4,6,9,11 silently"
	fi

	# Bytes, not bookkeeping: the plan's own two spot checks, plus the three .uasset files
	# the plan names as voiding rows 10, 7 and 15 respectively.
	for f in \
		Content/DevUI/fonts/LatoLatin-Regular.ttf \
		Content/M_VaCuusWorldPanel.uasset \
		Content/Spike/M_VaCuusSpike_Translucent.uasset \
		Content/Bundles/DevUIBundle.uasset
	do
		if [ ! -f "${f}" ]; then
			die "${f} is missing from the checkout" "re-clone; the tree is incomplete" "${PLAN_REL} §2 checklist 7"
		fi
		if is_lfs_pointer "${f}"; then
			die "${f} is a git-lfs POINTER, not its payload" \
			    "git lfs install && git lfs pull, then re-run" \
			    "${PLAN_REL} §2 checklist 7"
		fi
		n="$(wc -c < "${f}" | tr -d ' ')"
		say "  ok  ${f}: ${n} bytes — $(file -b "${f}" | cut -c1-60)"
		rep "- \`${f}\`: ${n} bytes, \`$(file -b "${f}" | cut -c1-60)\` — a payload, not a 129-byte pointer."
	done
	cd - >/dev/null

	both "LFS gate: PASS"
	rep ""
}

# ---------------------------------------------------------------------------------------
# 3. HOST PROJECT.
# ---------------------------------------------------------------------------------------
# WHY TP_Blank IS THE DEFAULT, with sources — this is a deviation from "use the Third
# Person template" and it is deliberate:
#   1. The plan (§2 checklist 5) describes the host as "a fresh C++ template project …
#      VcHost is itself a stock template with five source files and nothing the matrix
#      depends on". TP_Blank IS five source files (two .Target.cs, .h/.cpp/.Build.cs);
#      TP_ThirdPerson is forty-plus across four gameplay variants.
#   2. The same line says "Set the default map to something EMPTY … the first Metal shader
#      compile is DDC-cold". TP_ThirdPerson's Lvl_ThirdPerson is a Lumen + Virtual Shadow
#      Map level; TP_Blank has no map at all and we point the project at the engine's own
#      empty template map.
#   3. TP_ThirdPerson cannot be faithfully materialised by a shell copy: its
#      Config/TemplateDefs.ini lists `FoldersToIgnore=Content/ThirdPerson/Character` and
#      `…/Animations` (those directories ship EMPTY) and declares
#      `SharedContentPacks=(MountName="LevelPrototyping"/"Characters"/"Input")`, which only
#      the editor's project wizard installs. Copying it without them yields a map with
#      broken references.
# --template TP_ThirdPerson is still supported and DOES copy the shared content packs from
# Templates/TemplateResources/High/<MountName>/Content — but the map stays heavier and the
# first shader compile stays longer, so the plan's own advice is the default here.
ensure_project() {
	banner "3. HOST PROJECT — ${PROJECT_NAME} from the ${TEMPLATE} template"
	rep ""
	rep "## 3. Host project"
	rep ""

	if [ -f "${UPROJECT}" ]; then
		say "reusing the existing project at ${PROJ_DIR}"
		rep "- Reused the existing project at \`${PROJ_DIR}\`."
		detect_half_built
	else
		create_project_from_template
	fi

	configure_project
	say "project: ${UPROJECT}"
	rep "- **Project path: \`${UPROJECT}\`**"
	rep ""
}

detect_half_built() {
	local has_inter=0 has_bin=0
	[ -d "${PROJ_DIR}/Intermediate/Build" ] && has_inter=1
	if ls "${PROJ_DIR}/Binaries/Mac"/*.dylib >/dev/null 2>&1 || \
	   ls "${PROJ_DIR}/Binaries/Mac"/UnrealEditor-"${PROJECT_NAME}"* >/dev/null 2>&1; then
		has_bin=1
	fi
	if [ "${has_inter}" = "1" ] && [ "${has_bin}" = "0" ]; then
		say "NOTE: this project has Intermediate/Build but no Mac binaries — a previous build did NOT complete."
		say "      This run rebuilds from there. If the earlier failure was a toolchain change, delete"
		say "      ${PROJ_DIR}/Intermediate and ${PROJ_DIR}/Binaries first and re-run."
		rep "- **Half-built project detected** (Intermediate/Build present, no Mac binaries): a previous build did not complete. This run rebuilds; it is not silently reused as if it had succeeded."
	fi
}

create_project_from_template() {
	local tpl_dir="${ENGINE_DIR}/Templates/${TEMPLATE}"
	local tpl_up tpl_low mount packs f rel newname tmp

	say "creating ${PROJECT_NAME} from ${tpl_dir}"
	if [ "${TEMPLATE}" = "TP_Blank" ]; then
		say "  (why TP_Blank: plan §2 checklist 5 — 'a stock template with five source files', and an"
		say "   EMPTY default map because the first Metal shader compile is DDC-cold. The comment above"
		say "   ensure_project() has the full reasoning and the case for --template TP_ThirdPerson.)"
	else
		say "  (--template ${TEMPLATE}: the default is TP_Blank for the reasons in the comment above"
		say "   ensure_project(). The default map is still forced to an empty one — plan §2 checklist 5.)"
	fi

	mkdir -p "${PROJ_DIR}"
	# Copy everything except the wizard's own FoldersToIgnore / FilesToIgnore.
	( cd "${tpl_dir}" && find . \
		-path './Binaries' -prune -o -path './Build' -prune -o \
		-path './Intermediate' -prune -o -path './Saved' -prune -o \
		-path './Media' -prune -o -type f -print ) | while IFS= read -r rel; do
			case "${rel}" in
				./Config/TemplateDefs.ini|./Config/config.ini|./manifest.json|./contents.txt|./Manifest.json) continue ;;
				./${TEMPLATE}.uproject) continue ;;
			esac
			mkdir -p "${PROJ_DIR}/$(dirname "${rel}")"
			cp "${tpl_dir}/${rel}" "${PROJ_DIR}/${rel}"
		done

	# The wizard's rename, done the wizard's way (Config/TemplateDefs.ini
	# FolderRenames + FilenameReplacements + ReplacementsInFiles, extensions cpp/h/ini/cs).
	tpl_up="$(printf '%s' "${TEMPLATE}" | tr '[:lower:]' '[:upper:]')"
	tpl_low="$(printf '%s' "${TEMPLATE}" | tr '[:upper:]' '[:lower:]')"
	local proj_up proj_low
	proj_up="$(printf '%s' "${PROJECT_NAME}" | tr '[:lower:]' '[:upper:]')"
	proj_low="$(printf '%s' "${PROJECT_NAME}" | tr '[:upper:]' '[:lower:]')"

	if [ -d "${PROJ_DIR}/Source/${TEMPLATE}" ]; then
		mv "${PROJ_DIR}/Source/${TEMPLATE}" "${PROJ_DIR}/Source/${PROJECT_NAME}"
	fi
	# Contents first, then names (renaming first would invalidate the find list).
	find "${PROJ_DIR}/Source" "${PROJ_DIR}/Config" -type f \
		\( -name '*.cpp' -o -name '*.h' -o -name '*.ini' -o -name '*.cs' \) 2>/dev/null | while IFS= read -r f; do
			tmp="${f}.mb.$$"
			LC_ALL=C sed -e "s/${tpl_up}/${proj_up}/g" -e "s/${tpl_low}/${proj_low}/g" -e "s/${TEMPLATE}/${PROJECT_NAME}/g" "${f}" > "${tmp}"
			mv "${tmp}" "${f}"
		done
	find "${PROJ_DIR}/Source" "${PROJ_DIR}/Config" -type f -name "*${TEMPLATE}*" 2>/dev/null | while IFS= read -r f; do
			newname="$(dirname "${f}")/$(basename "${f}" | sed -e "s/${TEMPLATE}/${PROJECT_NAME}/g")"
			[ "${f}" = "${newname}" ] || mv "${f}" "${newname}"
		done

	# The wizard also writes the two game-name redirects that keep the template's
	# Blueprints resolving to the renamed module — DefaultTemplateProjectDefs.cpp:50-53.
	# Without these, renaming a template module breaks every Blueprint that references it.
	ini_add_line "${PROJ_DIR}/Config/DefaultEngine.ini" "/Script/Engine.Engine" \
		"+ActiveGameNameRedirects=(OldGameName=\"/Script/${TEMPLATE}\",NewGameName=\"/Script/${PROJECT_NAME}\")"
	ini_add_line "${PROJ_DIR}/Config/DefaultEngine.ini" "/Script/Engine.Engine" \
		"+ActiveGameNameRedirects=(OldGameName=\"${TEMPLATE}\",NewGameName=\"/Script/${PROJECT_NAME}\")"

	# SharedContentPacks: the wizard installs them; a shell copy must too, or the
	# template's map references content that is not there.
	packs="$(LC_ALL=C sed -n 's/^SharedContentPacks=(MountName="\([^"]*\)".*/\1/p' "${tpl_dir}/Config/TemplateDefs.ini" 2>/dev/null || true)"
	for mount in ${packs}; do
		if [ -d "${ENGINE_DIR}/Templates/TemplateResources/High/${mount}/Content" ]; then
			say "  installing shared content pack '${mount}'"
			mkdir -p "${PROJ_DIR}/Content"
			cp -R "${ENGINE_DIR}/Templates/TemplateResources/High/${mount}/Content/." "${PROJ_DIR}/Content/"
		else
			both "  WARNING: shared content pack '${mount}' not found under Templates/TemplateResources/High —"
			both "           the template's map will have broken references. Recorded, not hidden."
		fi
	done

	# The descriptor. Module name == project name is the wizard's own shape (that is why
	# the redirects above exist); the plugin is enabled here rather than by hand.
	cat > "${UPROJECT}" <<-EOF
	{
		"FileVersion": 3,
		"EngineAssociation": "",
		"Category": "",
		"Description": "VaCuus macOS pass host project — created by Tools/mac_bootstrap.sh from ${TEMPLATE}.",
		"Modules": [
			{
				"Name": "${PROJECT_NAME}",
				"Type": "Runtime",
				"LoadingPhase": "Default"
			}
		],
		"Plugins": [
			{
				"Name": "VaCuus",
				"Enabled": true
			}
		]
	}
	EOF
	rep "- Created \`${PROJECT_NAME}\` from the engine's \`${TEMPLATE}\` template, with the wizard's own renames (Config/TemplateDefs.ini) and the two \`+ActiveGameNameRedirects\` lines the wizard writes (DefaultTemplateProjectDefs.cpp:50-53) so template Blueprints still resolve."
	rep "- VaCuus enabled in the \`.uproject\` (setup.md §1)."
}

configure_project() {
	local eng="${PROJ_DIR}/Config/DefaultEngine.ini"
	local game="${PROJ_DIR}/Config/DefaultGame.ini"
	mkdir -p "${PROJ_DIR}/Config"

	# Plan §2 checklist 5: an EMPTY default map, because the first Metal shader compile is
	# DDC-cold. /Engine/Maps/Templates/Template_Default ships with the engine.
	ini_set "${eng}" "/Script/EngineSettings.GameMapsSettings" "GameDefaultMap" "/Engine/Maps/Templates/Template_Default"
	ini_set "${eng}" "/Script/EngineSettings.GameMapsSettings" "EditorStartupMap" "/Engine/Maps/Templates/Template_Default"

	# Plan §2 checklist 8 / block 10: needed before packaging, harmless now, and set here
	# so the human's packaging block does not start with a signing refusal.
	# bMacSignToRunLocally=True writes CODE_SIGN_IDENTITY = - (ad-hoc) into the xcconfig
	# (XcodeProject.cs:2274-2290) — enough to run the package on this same Mac.
	ini_set "${eng}" "/Script/MacTargetPlatform.XcodeProjectSettings" "bMacSignToRunLocally" "True"
	# Pin the architecture: the shipped default is MacTargetArchitectureUniversal
	# (BaseEngine.ini:3439), which forks the compile and makes any disk figure
	# non-comparable. Plan block 10, "Pin the architecture".
	ini_set "${eng}" "/Script/MacTargetPlatform.MacTargetSettings" "TargetArchitecture" "MacTargetArchitectureHost"

	# setup.md §3 + plan §2 checklist 8: row 15 needs the bundle to actually cook. A config
	# soft path is invisible to the cooker, so the directory must be force-cooked.
	ini_add_line "${game}" "/Script/UnrealEd.ProjectPackagingSettings" '+DirectoriesToAlwaysCook=(Path="/VaCuus/Bundles")'
	ini_add_line "${game}" "/Script/UnrealEd.ProjectPackagingSettings" '+DirectoriesToAlwaysCook=(Path="/VaCuus/Spike")'
	ini_set "${game}" "VaCuus" "BundleAssetPath" "/VaCuus/Bundles/DevUIBundle.DevUIBundle"

	# The plugin must be enabled even when the project was created by an earlier run or by
	# hand. Refuse rather than rewrite someone's descriptor.
	if ! LC_ALL=C grep -q '"VaCuus"' "${UPROJECT}"; then
		die "VaCuus is not enabled in ${UPROJECT}" \
		    "add {\"Name\": \"VaCuus\", \"Enabled\": true} to its \"Plugins\" array" \
		    "docs/buyer/setup.md §1"
	fi
	say "config: empty default map, ad-hoc Mac signing, host architecture pinned, bundle path + cook dirs"
	rep "- Config: empty default map (\`Template_Default\`, plan §2.5), \`bMacSignToRunLocally=True\` and \`TargetArchitecture=MacTargetArchitectureHost\` (plan block 10), bundle path + \`DirectoriesToAlwaysCook\` (setup.md §3)."
}

# ---------------------------------------------------------------------------------------
# 4. BUILD.
# ---------------------------------------------------------------------------------------
build_editor() {
	local log="${LOGS_DIR}/build.log" rc=0

	banner "4. BUILD — ${PROJECT_NAME}Editor Mac Development"
	rep ""
	rep "## 4. Build"
	rep ""

	check_no_editor_running quiet || \
		die "an UnrealEditor is running and holds the plugin's .dylib" \
		    "kill it BY PID (see the PIDs printed by preflight); never 'pkill -f UnrealEditor'" \
		    "CLAUDE.md dev-loop hazards"

	say "\$ ${ENGINE_DIR}/Engine/Build/BatchFiles/Mac/Build.sh ${PROJECT_NAME}Editor Mac Development -project=${UPROJECT}"
	rep "\`\`\`"
	rep "${ENGINE_DIR}/Engine/Build/BatchFiles/Mac/Build.sh ${PROJECT_NAME}Editor Mac Development -project=${UPROJECT}"
	rep "\`\`\`"

	set +e
	"${ENGINE_DIR}/Engine/Build/BatchFiles/Mac/Build.sh" \
		"${PROJECT_NAME}Editor" Mac Development \
		-project="${UPROJECT}" 2>&1 | tee "${log}"
	rc=${PIPESTATUS[0]}
	set -e

	if [ "${rc}" != "0" ] || ! LC_ALL=C grep -q "Result: Succeeded" "${log}"; then
		rep ""
		rep "**BUILD FAILED** (exit ${rc}). First ${MAX_BUILD_ERRORS} compile diagnostics:"
		rep ""
		rep '```'
		LC_ALL=C grep -E "error:|Error:|fatal error" "${log}" | head -"${MAX_BUILD_ERRORS}" >> "${REPORT}" || true
		rep '```'
		rep ""
		rep "This is the leg where Apple clang meets this code for the first time. Two shapes the"
		rep "plan predicts, both of them findings rather than accidents:"
		rep ""
		rep "- **quickjs Darwin branches under bare \`-std=c11\`** (plan §4 risk 12): an implicit"
		rep "  declaration of \`pthread_cond_timedwait_relative_np\`, or a missing \`malloc_size\`."
		rep "  The fix is a Mac-only \`PrivateDefinitions.Add(\"_DARWIN_C_SOURCE\")\` beside the Win64"
		rep "  block at \`VaCuusJs.Build.cs:39-48\` — **not** a source patch."
		rep "- **a missing-include class** like the one \`-StrictIncludes\` found on Linux (plan §5.5):"
		rep "  Mac's wave is a header-set difference (\`Mac/MacPlatform*.h\` instead of"
		rep "  \`Unix/UnixPlatform*.h\`) and should be small."
		rep ""
		rep "Full build log: \`${log}\`"
		say ""
		say "first ${MAX_BUILD_ERRORS} diagnostics:"
		LC_ALL=C grep -E "error:|Error:|fatal error" "${log}" | head -"${MAX_BUILD_ERRORS}" | tee -a "${CONSOLE_LOG}" || true
		die "build failed (exit ${rc})" \
		    "read ${log}; if it is quickjs, plan §4 risk 12 names the one-line Build.cs fix" \
		    "${PLAN_REL} block 1"
	fi

	say "BUILD: Result: Succeeded"
	rep "- **Result: Succeeded.** Apple clang compiled the five plugin modules, including the vendored quickjs-ng and RmlUi trees, unmodified. (Plan block 1: \"a real new data point\" either way.)"
	rep "- Full log: \`${log}\`"

	# Any clang diagnostic that did NOT stop the build is still evidence — the plan asks for
	# "any vendored-C clang diagnostic" out of block 1.
	local warns
	warns="$(LC_ALL=C grep -c "warning:" "${log}" 2>/dev/null || true)"
	rep "- clang warnings in this build: ${warns:-0} (\`grep warning: ${log}\`)."

	mach_o_export_check
	rep ""
}

# Plan §6: the Mach-O twin of the Linux export check. Confirms vendored patch #1 (quickjs
# symbols are not exported) on macOS — a claim VENDORED_TAG.txt currently only half-supports.
mach_o_export_check() {
	local dylib="${PROJ_DIR}/Binaries/Mac/UnrealEditor-VaCuusJs.dylib" leaked
	if [ ! -f "${dylib}" ]; then
		say "export check: NOT RUN — ${dylib} not found (a monolithic or unusual layout?)"
		rep "- Mach-O export check: **not run** — \`${dylib}\` does not exist. Recorded, not passed."
		return 0
	fi
	leaked="$(nm -gU "${dylib}" 2>/dev/null | LC_ALL=C grep ' _JS_' || true)"
	if [ -n "${leaked}" ]; then
		rep "- Mach-O export check: **FAIL** — quickjs symbols are exported from \`UnrealEditor-VaCuusJs.dylib\`:"
		rep '```'
		printf '%s\n' "${leaked}" | head -20 >> "${REPORT}"
		rep '```'
		say "export check: FAIL — quickjs symbols leak out of the VaCuusJs dylib (plan §6)"
		issue "Mach-O export check: quickjs symbols are exported from UnrealEditor-VaCuusJs.dylib (plan §6)."
	else
		say "export check: PASS — no ' _JS_' symbols exported from UnrealEditor-VaCuusJs.dylib"
		rep "- Mach-O export check: **PASS** — \`nm -gU … | grep ' _JS_'\` is empty (note the Mach-O leading underscore and \`.dylib\`, not \`.so\`). Plan §6: confirms vendored patch #1 on macOS."
	fi
}

# ---------------------------------------------------------------------------------------
# Session runner — every editor launch in this script goes through here, so every one of
# them honours the same three recorded hazards.
# ---------------------------------------------------------------------------------------
EDITOR_BIN=""
LAST_SESSION_LOG=""
LAST_SESSION_SHOTS=""

editor_bin() {
	EDITOR_BIN="${ENGINE_DIR}/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
}

# run_session <tag> <timeout-s> <marker|-> <exec-cmds> [extra flags…]
#
#   * -ExecCmds is ALWAYS LAST and ALWAYS carries a TRAILING COMMA. The value parse uses
#     bShouldStopOnSeparator=false (ParseExecCommands.cpp:63 — plan §3 (b)), so the value
#     swallows later arguments; putting it last means there is nothing to swallow, and the
#     trailing comma means anything the launcher appends becomes its own (ignored) command
#     instead of corrupting the last real one. It splits on COMMAS, never semicolons.
#   * The editor is launched with an ABSOLUTE .uproject path (CLAUDE.md: a relative path
#     gives "Project file not found" then SIGSEGV) and killed BY PID.
#   * No -RenderOffscreen: there is no offscreen mode on Mac at all
#     (NullPlatformApplicationMisc.cpp:17-23 is #if PLATFORM_WINDOWS || PLATFORM_LINUX —
#     verified). Windowed is the only mode. Plan §3 (c).
run_session() {
	local tag="$1" timeout="$2" marker="$3" cmds="$4"; shift 4
	local pid rc=0 waited=0 shots_before shots_after
	local proj_log="${PROJ_DIR}/Saved/Logs/${PROJECT_NAME}.log"
	local shots_src="${PROJ_DIR}/Saved/Screenshots/MacEditor"
	local out="${LOGS_DIR}/${tag}.stdout.log"

	editor_bin
	mkdir -p "${shots_src}" "$(dirname "${proj_log}")"
	shots_before="$(mktmp)"; ls -1 "${shots_src}" 2>/dev/null | LC_ALL=C sort > "${shots_before}"

	# Remove the previous session's log BEFORE launching. The engine rotates its own log at
	# startup, but not instantly, and the poll loop below starts at once: without this the
	# marker grep can match the PREVIOUS run's "Sending StopTestSession" and SIGTERM the
	# editor seconds after it launched. (Seen in a dry run: the GPU-pair session was killed
	# on the suite's stale marker and reported the suite's counts.) Every session's log is
	# copied out to ${LOGS_DIR} afterwards, so nothing is lost by deleting it here.
	rm -f "${proj_log}"

	say ""
	say "--- session '${tag}' (limit ${timeout}s) ---"
	say "\$ UnrealEditor ${UPROJECT} $* -ExecCmds=\"${cmds},\""

	if [ "${METAL_DEBUG_LAYER}" = "1" ]; then
		export MTL_DEBUG_LAYER=1
		say "    MTL_DEBUG_LAYER=1 (plan §4 risk 9 — a latent bounds/usage violation becomes a named abort)"
	fi

	set +e
	"${EDITOR_BIN}" "${UPROJECT}" "$@" -ExecCmds="${cmds}," </dev/null > "${out}" 2>&1 &
	pid=$!
	set -e

	# Wait for the marker if one was named (the automation runs), else for the clock.
	while [ "${waited}" -lt "${timeout}" ]; do
		if ! kill -0 "${pid}" 2>/dev/null; then break; fi
		if [ "${marker}" != "-" ] && [ -f "${proj_log}" ] && LC_ALL=C grep -q "${marker}" "${proj_log}"; then
			say "    marker seen: '${marker}'"
			break
		fi
		sleep 2
		waited=$((waited + 2))
	done

	# CLAUDE.md + plan §3 (f): the editor often does NOT exit after a Quit that was
	# dispatched at frame 0. Kill BY PID — never 'pkill -f', whose pattern would match this
	# script's own shell. SIGTERM first, because matrix row 14's teardown tail IS the
	# SIGTERM path.
	if kill -0 "${pid}" 2>/dev/null; then
		say "    sending SIGTERM to PID ${pid} (row 14's teardown path)"
		kill -TERM "${pid}" 2>/dev/null || true
		waited=0
		while [ "${waited}" -lt 60 ] && kill -0 "${pid}" 2>/dev/null; do sleep 2; waited=$((waited + 2)); done
		if kill -0 "${pid}" 2>/dev/null; then
			say "    still alive after 60s; SIGKILL to PID ${pid}"
			kill -9 "${pid}" 2>/dev/null || true
		fi
	fi
	wait "${pid}" 2>/dev/null || rc=$?

	# The project log is overwritten by the next launch, so it is copied out per session.
	LAST_SESSION_LOG="${LOGS_DIR}/${tag}.log"
	if [ -f "${proj_log}" ]; then
		cp "${proj_log}" "${LAST_SESSION_LOG}"
	else
		: > "${LAST_SESSION_LOG}"
		both "    WARNING: ${proj_log} does not exist — the session may not have started."
	fi

	# Screenshots are numbered ScreenShot0000N.png with no per-command name, so new files
	# are attributed to this session by diffing the directory. Plan §3 (e):
	# Saved/Screenshots/MacEditor/ for Dev -game (Paths.cpp:553-556 + MacPlatformProperties.h:52-69).
	shots_after="$(mktmp)"; ls -1 "${shots_src}" 2>/dev/null | LC_ALL=C sort > "${shots_after}"
	LAST_SESSION_SHOTS=""
	mkdir -p "${SHOTS_DIR}/${tag}"
	local s
	for s in $(LC_ALL=C comm -13 "${shots_before}" "${shots_after}"); do
		cp "${shots_src}/${s}" "${SHOTS_DIR}/${tag}/${s}"
		LAST_SESSION_SHOTS="${LAST_SESSION_SHOTS} ${SHOTS_DIR}/${tag}/${s}"
	done
	rm -f "${shots_before}" "${shots_after}"
	unset MTL_DEBUG_LAYER
	say "    log: ${LAST_SESSION_LOG}${LAST_SESSION_SHOTS:+ | shots:${LAST_SESSION_SHOTS}}"
	return 0
}

# grep_report <log> <pattern> <label> — quotes matching lines into the report, or says
# plainly that there were none. Silence is never allowed to read as success.
grep_report() {
	local log="$1" pat="$2" label="$3" hits
	hits="$(LC_ALL=C grep -E "${pat}" "${log}" 2>/dev/null | head -12 || true)"
	if [ -n "${hits}" ]; then
		rep "- ${label}:"
		rep '```'
		printf '%s\n' "${hits}" >> "${REPORT}"
		rep '```'
	else
		rep "- ${label}: **no matching line in the log** (pattern \`${pat}\`)."
	fi
}

teardown_tail_check() {
	local log="$1"
	local shutdown unpublished
	shutdown="$(LC_ALL=C grep -c "RmlUi shut down" "${log}" 2>/dev/null || true)"
	unpublished="$(LC_ALL=C grep -c "unpublished resource traffic" "${log}" 2>/dev/null || true)"
	if [ "${shutdown:-0}" -gt 0 ] && [ "${unpublished:-0}" = "0" ]; then
		rep "- Row 14 (teardown, this session): **clean** — \`RmlUi shut down\` present, zero \`unpublished resource traffic\` warnings."
	else
		rep "- Row 14 (teardown, this session): **look** — \`RmlUi shut down\` ×${shutdown:-0}, \`unpublished resource traffic\` ×${unpublished:-0}. A missing shutdown line usually means SIGKILL beat the in-band shutdown; an unpublished-traffic warning is a real finding."
	fi
}

# ---------------------------------------------------------------------------------------
# 5. AUTOMATION SUITE.
# ---------------------------------------------------------------------------------------
run_suite() {
	local ok bad counts

	banner "5. AUTOMATION SUITE — Automation RunTests VaCuus, under -nullrhi"
	rep ""
	rep "## 5. Automation suite (plan block 3)"
	rep ""

	check_no_editor_running quiet || die "an UnrealEditor is running" "kill it by PID (never pkill -f)" "CLAUDE.md dev-loop hazards"

	run_session "suite-nullrhi" 2400 "Sending StopTestSession" \
		"Automation RunTests VaCuus,Quit" \
		-unattended -nullrhi -nosplash

	counts="$(count_results "${LAST_SESSION_LOG}")"
	ok="${counts%% *}"; bad="${counts##* }"
	say "suite: ${ok} Success, ${bad} non-Success  (read from the log, never stdout)"
	rep "- **${ok} \`Result={Success}\`, ${bad} non-Success**, counted from \`${LAST_SESSION_LOG}\` — never from stdout, because an interleaved UnrealTraceServer fork clobbers the tail of every run (CLAUDE.md; plan §3 (g))."
	rep "- The plan (block 3) expected **189** at the HEAD it was written against, and records that the number moves with HEAD (the last Linux run was 186/186 at an older HEAD). A different count is a fact to record; a non-Success is a failure."
	if [ "${bad}" != "0" ]; then
		issue "automation suite: ${bad} non-Success test(s) — see §5. A non-Success on Mac is a finding, not a venue artifact."
		rep "- Non-Success tests:"
		rep '```'
		LC_ALL=C grep 'Test Completed\. Result=' "${LAST_SESSION_LOG}" | LC_ALL=C grep -v 'Result={Success}' | head -40 >> "${REPORT}" || true
		rep '```'
	fi

	# Plan block 3b — "the highest-value automated result of the trip". On Linux both sides
	# of VaCuus.Input.TextEntry's relative assertion take the "absent" branch and
	# registration is never exercised; on Mac FMacApplication returns a live
	# FMacTextInputMethodSystem, so the test flips branch. This one line decides whether the
	# run exercised the never-run path at all (VaCuusTextInput.cpp:966-975).
	rep ""
	rep "### The IME branch line — plan block 3b, the highest-value automated result"
	rep ""
	grep_report "${LAST_SESSION_LOG}" "IME: (platform ITextInputMethodSystem present|this platform exposes no ITextInputMethodSystem)" \
		"IME platform observable"
	if LC_ALL=C grep -q "IME: platform ITextInputMethodSystem present" "${LAST_SESSION_LOG}"; then
		say "IME: platform system PRESENT — the registration path ran for the first time anywhere."
		rep "- **PRESENT.** \`VaCuus.Input.TextEntry\` asserts relatively (\`TestEqual(\"A context is only REGISTERED where a platform system exists\", ImeStatus.bRegistered, !bPlatformImeAbsent)\` — VaCuusTextEntryTest.cpp:327-328 at this HEAD; the plan cites :433-436, which has since drifted onto a different assertion), so this run is the first automated proof that registration actually happens. A failure of that assertion would be a **real bug** in the registration plumbing, not a venue artifact."
	else
		rep "- **ABSENT or not logged.** If the suite ran under \`-nullrhi\` without a platform application, the bridge may not have been built at all; the windowed rows below print the same line again (\`vacuus.M1HUD.TypeShot\`). Record which one you read."
	fi
	rep ""
	rep "- Session log: \`${LAST_SESSION_LOG}\`"
	rep ""
	rep "**Note (CLAUDE.md):** \`ShutdownModule\` never runs in an \`Automation RunTests …, Quit\` session, so module-teardown behaviour is NOT covered by this leg. The windowed rows below end in SIGTERM and are."
	rep ""
}

# Plan block 4: the two tests that self-skip loudly under -nullrhi, run on the real Metal
# RHI. This is also the first non-nullrhi editor launch, i.e. plan block 2's DDC-cold Metal
# shader compile — hence the long limit.
run_gpu_pair() {
	local counts ok bad

	banner "6. GPU PAIR on real Metal — and the first (DDC-cold) Metal shader compile"
	rep ""
	rep "## 6. Real-RHI legs (plan blocks 2 and 4)"
	rep ""
	rep "First non-\`-nullrhi\` launch: this is where every VaCuus global shader"
	rep "(\`VaCuusUI/Blur/Gradient/Material.usf\`) meets Apple's Metal front end, twice —"
	rep "the Mac profile targets both \`SF_METAL_SM5\` and \`SF_METAL_SM6\` (plan block 2)."
	rep ""

	check_no_editor_running quiet || die "an UnrealEditor is running" "kill it by PID (never pkill -f)" "CLAUDE.md dev-loop hazards"

	run_session "gpu-pair" 5400 "Sending StopTestSession" \
		"Automation RunTests VaCuus.Render.Composite.LinearOutputGPU+VaCuus.World.MipContentGPU,Quit" \
		-unattended -nosplash -ForceRes -resx=1920 -resy=1080

	counts="$(count_results "${LAST_SESSION_LOG}")"
	ok="${counts%% *}"; bad="${counts##* }"
	say "GPU pair: ${ok} Success, ${bad} non-Success (expected 2 / 0)"
	rep "- **${ok} Success, ${bad} non-Success** (plan block 4 expects 2/2 Success and no \`SKIPPED under NullRHI\`)."
	if [ "${ok}" != "2" ] || [ "${bad}" != "0" ]; then
		issue "GPU pair on real Metal: ${ok} Success / ${bad} non-Success, expected 2 / 0 — see §6."
	fi
	grep_report "${LAST_SESSION_LOG}" "SKIPPED under NullRHI" "any NullRHI self-skip (there must be none here)"

	rep ""
	rep "### Which feature level bound (plan §2 checklist 11)"
	rep ""
	grep_report "${LAST_SESSION_LOG}" "SM6 is enabled but is not supported|To use SM6 on this system|Metal Feature Level|MetalLanguageVersion" \
		"feature-level lines"
	if LC_ALL=C grep -q "SM6 is enabled but is not supported\|To use SM6 on this system" "${LAST_SESSION_LOG}"; then
		say "feature level: SM5 (the fallback Warning is in the log)"
		rep "- **SM5 bound** — the fallback Warning is present (MetalRHI.cpp:421-427, then :433-441 pins \`SP_METAL_SM5\`)."
	else
		say "feature level: no fallback Warning found — read as SM6 unless the log says otherwise"
		rep "- **No fallback Warning** — read this as SM6 (MetalRHI.cpp:255-267 needs macOS ≥ ${MACOS_SM6_MIN} and \`GPUFamilyApple8\`, i.e. M2+). Preflight predicted: ${FEATURE_LEVEL_PREDICTION}."
	fi
	rep "- **An SM5 run and an SM6 run are not the same cell** for matrix rows 8 and 12. Whatever bound here must be written into every Method sentence."
	rep ""
	rep "### Shader compile (plan block 2, and §4 risk 5)"
	rep ""
	grep_report "${LAST_SESSION_LOG}" "LogShaderCompilers: Error.*VaCuus|FVaCuusMaterialPS|FVaCuusMaterialVS" \
		"VaCuus shader-compiler errors (there must be none)"
	if LC_ALL=C grep -qE "LogShaderCompilers: Error.*VaCuus|no FVaCuusMaterialVS/PS pair" "${LAST_SESSION_LOG}"; then
		issue "Metal shader compile: LogShaderCompilers errors naming VaCuus shaders — a REAL finding (plan §4 risk 5), and it blocks every visual row."
	fi
	rep ""
}

# ---------------------------------------------------------------------------------------
# 7. THE SCRIPTED MATRIX ROWS (plan block 5, plus block 6's window-immune half and block
#    8's -game legs). Windowed, real Metal RHI — there is no offscreen mode on Mac.
# ---------------------------------------------------------------------------------------
GAME_FLAGS="-game -windowed -ForceRes -resx=1920 -resy=1080 -nosplash"

row_header() {
	rep ""
	rep "### ${1}"
	rep ""
}

row_shots() {
	local tag="$1"
	if [ -n "${LAST_SESSION_SHOTS}" ]; then
		rep "- Screenshots:"
		local s
		for s in ${LAST_SESSION_SHOTS}; do rep "  - \`${s}\`"; done
	else
		rep "- Screenshots: **none were produced.** If the row's command takes one, that is itself a finding — check \`${LOGS_DIR}/${tag}.log\`. (Note \`AutoShot N\` fires after N *recorded* frames; windowed at 60 Hz vsync \`AutoShot 1200\` is ~20 s, not the ~5 s it was at Linux's offscreen 227 fps — plan block 5.)"
	fi
}

run_rows() {
	banner "7. MATRIX ROWS — windowed, real Metal (plan block 5)"
	rep ""
	rep "## 7. Matrix rows the script ran"
	rep ""
	rep "Venue for every row below: **windowed, real Metal RHI, \`-ForceRes\` 1920×1080**, Dev"
	rep "editor binary run \`-game\`. \`-RenderOffscreen\` is NOT used and cannot be: there is no"
	rep "null application on Mac (\`NullPlatformApplicationMisc.cpp:17-23\`). Plan §3 (c), §5.4."
	rep ""
	rep "The script produces the images and the log lines. **The verdicts are the human's** —"
	rep "each row says what to look for."
	rep ""

	# ---- Row 1 ---------------------------------------------------------------------
	row_header "Row 1 — screen-space HUD composite"
	run_session "row01-refhud" 150 "-" "vacuus.RefHud, vacuus.M1HUD.AutoShot 1200" ${GAME_FLAGS}
	rep "Command: \`vacuus.RefHud, vacuus.M1HUD.AutoShot 1200\`"
	rep ""
	rep "**Look for:** the full 1,732-node HUD — two 24-row scoreboards with distinct K/D/A/score/ping"
	rep "columns, killfeed at the right edge, minimap blip cloud bottom-right, player plate, compass,"
	rep "ammo in a warm glow, damage numbers. (Linux read: all present, one cosmetic overlap of the"
	rep "objective line over the scoreboard headers at 1920×1080.)"
	row_shots "row01-refhud"
	hidpi_observable "${LAST_SESSION_LOG}"
	teardown_tail_check "${LAST_SESSION_LOG}"

	# ---- Row 2 ---------------------------------------------------------------------
	row_header "Row 2 — steady-state node count (an assertion the script CAN check)"
	run_session "row02-count" 90 "-" "vacuus.RefHud, vacuus.RefHud.Count 12" ${GAME_FLAGS}
	rep "Command: \`vacuus.RefHud, vacuus.RefHud.Count 12\` — assertion: the count is in [1650, 1850]."
	grep_report "${LAST_SESSION_LOG}" "NodeCount:" "node count"
	rep "Linux recorded 1732, equal to the automation twin \`VaCuus.RefHud.Count\`."
	teardown_tail_check "${LAST_SESSION_LOG}"

	# ---- Rows 3 & 4, the window-immune half ---------------------------------------
	row_header "Rows 3 and 4 — the window-position-IMMUNE half only (plan block 6)"
	run_session "row0304-hit" 90 "-" \
		"vacuus.M2Demo, vacuus.M2Demo.Rects 5, vacuus.M2Demo.Hit 120 115 6, vacuus.M1HUD.TypeShot 710 113 vacuus" \
		${GAME_FLAGS}
	rep "On Linux the window sat at the desktop origin, so \"view pixels\" and \"Slate absolute pixels\""
	rep "were the same numbers. **On Mac they are not, and the plugin does not compensate**:"
	rep "\`HoverShot\`/\`TypeShot\` feed their x y straight to \`FSlateApplication::SetCursorPos\`"
	rep "(\`VaCuusRender.cpp:1357-1397\`) while the widget converts back with"
	rep "\`AbsoluteToLocal(...) * Geometry.Scale\` (\`SVaCuusWidget.cpp:381-388\`). The delta is the"
	rep "window origin plus the title bar. So the script runs only the parts that are immune:"
	rep ""
	grep_report "${LAST_SESSION_LOG}" "Rects\[|rects=|interactive rect" "rect list (view pixels — window-position-immune, plan block 6 step 2)"
	grep_report "${LAST_SESSION_LOG}" "covered=|focusable=|answered Handled" "hit answer (pure view-pixel space, VaCuusRender.cpp:1311-1343)"
	grep_report "${LAST_SESSION_LOG}" "IME bridge built=" "row 4's own evidence line — it must read **absent=no, registered=yes**, the opposite of the Linux record"
	grep_report "${LAST_SESSION_LOG}" "the event handled somewhere on the bubble path|unhandled \(it fell through\)|the press was taken by" \
		"the built-in self-check (\`unhandled\` means fix the offset and re-run, NOT record a FAIL)"
	rep ""
	rep "**Left to the human (plan block 6):** move the window to the top-left of the main display"
	rep "(or run fullscreen), add the window origin to the coordinates, then re-run"
	rep "\`vacuus.M1HUD.HoverShot 120 115\` and \`vacuus.M1HUD.TypeShot 710 113 vacuus\` and read the"
	rep "shots. Arbiter for the coordinate literals: if the TextInput field is no longer near"
	rep "(511,95)-(909,132) as the Linux column records, the literals are invalid for that window."
	rep "Note \`FMacCursor::SetPosition\` warps the **real system cursor** (\`MacCursor.cpp:491-506\`):"
	rep "the pointer jumps on the desktop and the game window must be frontmost."
	row_shots "row0304-hit"
	teardown_tail_check "${LAST_SESSION_LOG}"

	# ---- Row 6 ---------------------------------------------------------------------
	row_header "Row 6 — gamepad spatial navigation"
	run_session "row06-nav" 90 "-" \
		"vacuus.M2Demo, vacuus.M1HUD.NavShot Gamepad_DPad_Down Gamepad_DPad_Right" ${GAME_FLAGS}
	rep "This row needs no human despite appearances: \`NavShot\` defers to \`OnBeginFrame\` and then"
	rep "synthesizes the FKey sequence through \`FSlateApplication\` (\`VaCuusRender.cpp:1851-1890\` at"
	rep "this HEAD — the plan cites :1814-1856, which is \`HoverShot\`'s twin of the same deferral;"
	rep "plan block 5)."
	rep ""
	rep "**Look for:** the magenta focus ring on **BRAVO** — Down enters the grid at ALPHA, Right"
	rep "lands on BRAVO."
	grep_report "${LAST_SESSION_LOG}" "navigation config overridden" "navigation config"
	row_shots "row06-nav"
	teardown_tail_check "${LAST_SESSION_LOG}"

	# ---- Row 7 ---------------------------------------------------------------------
	row_header "Row 7 — world-space panel + raycast input"
	run_session "row07-world" 150 "-" \
		"vacuus.M5World 1, vacuus.M5World.InputSmoke 5, vacuus.WorldDemo.Shot" ${GAME_FLAGS}
	grep_report "${LAST_SESSION_LOG}" "InputSmoke: all [0-9]+ assertion\(s\) passed|InputSmoke.*FAIL" \
		"InputSmoke result (Linux: all 16 assertions passed)"
	rep "**Look for:** the quad in-scene running the JS demo, with the raycast click's routed write"
	rep "visible in pixels (Linux read Ammo 29 = 30 − 1)."
	rep ""
	rep "**Also worth a look (plan §4 risk 8):** \`PF_B8G8R8A8\` is not in Metal's typed-UAV list, so"
	rep "world-panel mips may be forced onto the raster path — stale or black far mips when the"
	rep "camera walks away. The compute-branch \`ensureMsgf\` does **not** fire, so only the picture"
	rep "and the \`WorldMips\` PerfLog scope shape will tell you."
	row_shots "row07-world"
	teardown_tail_check "${LAST_SESSION_LOG}"

	# ---- Row 8 ---------------------------------------------------------------------
	row_header "Row 8 — glass (backdrop blur) over the scene"
	run_session "row08-glass" 120 "-" \
		"vacuus.M5Glass, vacuus.M1HUD.PerfLog 1, vacuus.M1HUD.HoverShot" ${GAME_FLAGS}
	rep "**Look for:** the ROUNDED blur panel and the SQUARE blur panel both smearing the scene"
	rep "behind them, and the CONTROL panel with the same fill and NO blur (the scene reads sharp"
	rep "through it)."
	rep ""
	rep "**Mac-first observable, capture it deliberately (plan §3, §4 risk 4):** the latched"
	rep "\`Exp-GLASS-BACKBUFFER-SRV\` line names which route glass took — direct SRV or the copy"
	rep "fallback. Then force the other route once with \`vacuus.GlassBackbufferSRV 0\` to prove both"
	rep "work on Metal."
	grep_report "${LAST_SESSION_LOG}" "Exp-GLASS-BACKBUFFER-SRV" "the glass route Mac took"
	grep_report "${LAST_SESSION_LOG}" "published=[0-9]+ / [0-9]+ recorded|% idle" "idle economy (Linux: published=0 / 3,363 recorded, 100.0% idle)"
	row_shots "row08-glass"
	teardown_tail_check "${LAST_SESSION_LOG}"

	# ---- Row 9 ---------------------------------------------------------------------
	row_header "Row 9 — gradient and builtin decorators"
	run_session "row09-deco" 90 "-" "vacuus.M5Deco, vacuus.M1HUD.AutoShot 10" ${GAME_FLAGS}
	rep "**Look for:** all six cells — linear 90° red→blue, repeating 45° gold/black hazard stripes,"
	rep "radial white-core→deep-blue rim, conic full hue wheel, builtin \`shader(glass-panel)\`"
	rep "translucent fill + border glow, and a plain-fill control."
	row_shots "row09-deco"
	teardown_tail_check "${LAST_SESSION_LOG}"

	# ---- Row 10 --------------------------------------------------------------------
	row_header "Row 10 — material decorators (UMaterial in the UI pass)"
	run_session "row10-matspike" 90 "-" "vacuus.M5MatSpike, vacuus.M1HUD.AutoShot 10" ${GAME_FLAGS}
	rep "**Look for:** TRANSLUCENT (brown, text over), ADDITIVE (cyan), OPAQUE (replaces the cell box),"
	rep "MID base (textured), TIME-ANIMATED (gradient bars mid-motion); the control cell stays a plain"
	rep "fill and the refused cells are named in the log."
	rep ""
	rep "**Compare against the Linux row-10 shot at the same resolution (plan §4 risk 7):** \`half\` is"
	rep "a real 16-bit float on Mac (\`bSupportsRealTypes=RuntimeGuaranteed\`) and the material gamma"
	rep "encode uses it — look for **banding** in the smooth gradients or a slight hue shift, worst in"
	rep "dark tones."
	grep_report "${LAST_SESSION_LOG}" "DrawShader: no FVaCuusMaterial|refused|MD_UI" "material draw diagnostics"
	row_shots "row10-matspike"
	teardown_tail_check "${LAST_SESSION_LOG}"

	# ---- Row 11 --------------------------------------------------------------------
	row_header "Row 11 — M5 acceptance demo (TSX + translation + glass + world quad)"
	run_session "row11-m5demo" 120 "-" \
		"vacuus.M5Demo, vacuus.M1HUD.AutoShot 10, vacuus.M5Glass.Shot 8" ${GAME_FLAGS}
	grep_report "${LAST_SESSION_LOG}" "LogVaCuusJS: Error" "JS errors (the assertion is ZERO)"
	if LC_ALL=C grep -q "LogVaCuusJS: Error" "${LAST_SESSION_LOG}"; then
		issue "matrix row 11: LogVaCuusJS: Error lines present — the row asserts zero."
	fi
	grep_report "${LAST_SESSION_LOG}" "translation: published table|model '.*' bound" "translation table and model binding"
	rep "**Look for:** the TSX HUD with the model-fed health sweep, five translated killfeed rows,"
	rep "the glass panel blurring what is behind it, and the world quad running the same document."
	row_shots "row11-m5demo"
	teardown_tail_check "${LAST_SESSION_LOG}"

	# ---- Row 12, the -game legs ----------------------------------------------------
	row_12_ab

	# ---- Rows 16/17, Mac-first candidates ------------------------------------------
	row_header "Row 16 (candidate, Mac-first) — the lobby demo"
	rep "Not in the Linux column: HEAD added \`vacuus.LobbyDemo\` after the matrix was written."
	rep "The plan (§5, \"staleness items\") recommends running it as a **row 16 candidate marked"
	rep "Mac-first** rather than quietly extending or quietly omitting the matrix."
	run_session "row16-lobby" 120 "-" \
		"vacuus.LobbyDemo, vacuus.LobbyDemo.Rects, vacuus.LobbyDemo.Stats, vacuus.LobbyDemo.Shot" ${GAME_FLAGS}
	grep_report "${LAST_SESSION_LOG}" "LobbyRects\[|LobbyDemo:" "lobby demo lines (the \`size=WxH\` here is also risk 11's evidence — see below)"
	row_shots "row16-lobby"
	teardown_tail_check "${LAST_SESSION_LOG}"

	row_header "Row 17 (candidate, Mac-first) — world-panel mip chain and its off-switch"
	run_session "row17-mips" 120 "-" \
		"vacuus.WorldDemo, vacuus.WorldDemo.Mips 1, vacuus.WorldDemo.Stats, vacuus.WorldDemo.Shot" ${GAME_FLAGS}
	grep_report "${LAST_SESSION_LOG}" "WorldMips|mip" "mip-chain lines"
	row_shots "row17-mips"
	teardown_tail_check "${LAST_SESSION_LOG}"
}

# The HiDPI arbiter. Plan §4 risk 1 is the highest-ranked risk and has never executed
# anywhere: the view may lay out in DEVICE pixels, giving a HUD that is crisp but half
# physical size. The plan's own instruction is "compare the logged ViewSize to the window's
# point size. 2x => confirmed, before a single screenshot." We ask for 1920x1080 points via
# -ForceRes, so a logged 3840x2160 is the confirmation.
hidpi_observable() {
	local log="$1"
	rep ""
	rep "**HiDPI arbiter (plan §4 risk 1 — read this before any screenshot verdict).** The window"
	rep "was asked for 1920×1080 *points* via \`-ForceRes\`. The view sizes this session logged:"
	grep_report "${log}" "Created view [0-9]+ \([0-9]+x[0-9]+\)|View [0-9]+ size now [0-9]+x[0-9]+|initial view [0-9]+x[0-9]+" \
		"logged view sizes"
	rep "If those read ~3840×2160, risk 1 is **confirmed**: the HUD will be crisp but half physical"
	rep "size — tiny text, tiny buttons, huge margins. NOT blurry, NOT mis-aimed. File the bead."
	rep "If they read 1920×1080, risk 1 did not fire and the coordinate space is self-consistent."
}

# Plan block 8 / matrix row 12: the two -game legs of the A/B. The PIE leg is the human's.
# The ini edit is made through ini_set (idempotent) and reverted in the same function; the
# revert is verified, and a failed revert is a loud finding because it would silently change
# every later run.
row_12_ab() {
	local eng="${PROJ_DIR}/Config/DefaultEngine.ini"

	row_header "Row 12 — PF_FloatRGBA composite permutation, the two \`-game\` legs"
	rep "Steps 1–3 are \`-game\` and run here; **only the PIE leg needs a human**, and it is the leg"
	rep "the Linux column explicitly deferred to this pass (plan block 8)."
	rep ""
	rep "The Mac device profile does **not** override \`r.DefaultBackBufferPixelFormat\`"
	rep "(\`BaseDeviceProfiles.ini:1486-1493\`, default 4 = A2B10G10R10 per \`SceneTextures.cpp:71-79\`),"
	rep "so the control run is *expected* to log the same \`A2B10G10R10 -> pass-through\` as Linux."
	rep "If Metal returns something else that is **data, not failure** — the row asserts the"
	rep "format→permutation *pairing*."
	rep ""

	# Control leg (whatever the project's current default is — normally no override at all).
	run_session "row12-control" 120 "-" "vacuus.RefHud, vacuus.M1HUD.AutoShot 300" ${GAME_FLAGS}
	grep_report "${LAST_SESSION_LOG}" "elements texture is" "control leg permutation line"
	local control_shots="${LAST_SESSION_SHOTS}"

	# Forced-FloatRGBA leg.
	ini_set "${eng}" "/Script/Engine.RendererSettings" "r.DefaultBackBufferPixelFormat" "3"
	say "row 12: set r.DefaultBackBufferPixelFormat=3 in ${eng}"
	run_session "row12-floatrgba" 120 "-" "vacuus.RefHud, vacuus.M1HUD.AutoShot 300" ${GAME_FLAGS}
	grep_report "${LAST_SESSION_LOG}" "elements texture is" "FloatRGBA leg permutation line"
	rep "- Control shots:${control_shots:- none}"
	rep "- FloatRGBA shots:${LAST_SESSION_SHOTS:- none}"

	# Revert, and verify the revert. The plan says revert before block 10; leaving it set
	# would silently change every packaged run the human does next.
	ini_set "${eng}" "/Script/Engine.RendererSettings" "r.DefaultBackBufferPixelFormat" "4"
	if LC_ALL=C grep -q '^r.DefaultBackBufferPixelFormat=4' "${eng}"; then
		say "row 12: reverted r.DefaultBackBufferPixelFormat to 4 (the engine default)"
		rep "- Ini reverted to \`r.DefaultBackBufferPixelFormat=4\` (the engine default, \`SceneTextures.cpp:71-79\`) and the revert was verified."
	else
		say "row 12: WARNING — could not verify the ini revert in ${eng}"
		rep "- **WARNING: the revert of \`r.DefaultBackBufferPixelFormat\` could not be verified in \`${eng}\`. Check it by hand before packaging (plan block 8: revert before block 10).**"
	fi
	rep ""
	rep "**Look for:** the expected line is \`VaCuus composite: elements texture is FloatRGBA ->"
	rep "LinearOutput\` on the forced leg and \`A2B10G10R10 -> pass-through\` on the control"
	rep "(\`GPixelFormats\` names carry no \`PF_\` prefix — engine \`Misc/PixelFormat.cpp:44\`). Then compare"
	rep "the two HUD screenshots by eye: opaque surfaces must match — none of the ~2.2× global"
	rep "brightening a missed decode produces."
	teardown_tail_check "${LAST_SESSION_LOG}"
}

# Plan block 7. OFF by default and that is deliberate: a soak produces numbers, and "a number
# without a Method is not a filled cell" — venue, duration, frame count, WHICH window was
# selected (the boot window is excluded from steady figures) and which scopes were summed are
# judgements the human makes. With --with-soaks the script collects the raw PerfLog windows so
# the human has material to apply that method to; it does not fill any cell.
run_soaks() {
	banner "8. DEV PERF SOAKS (raw material only — plan block 7)"
	rep ""
	rep "## 8. Dev perf soaks — RAW MATERIAL, not filled cells"
	rep ""
	rep "Collected because \`--with-soaks\` was passed. **These are not filled passport cells.**"
	rep "Plan block 7: a number without a Method is not a filled cell — name the venue, the duration,"
	rep "the frame count, the window selection (exclude the boot window) and which scopes were summed."
	rep ""

	run_session "soak-refhud-100s" 160 "-" "vacuus.M1HUD.PerfLog 1, vacuus.RefHud" ${GAME_FLAGS}
	rep "### RefHud, ~100 s → passport rows 1, 2a, 2b, 3, 4, 7, 8"
	grep_report "${LAST_SESSION_LOG}" "PerfLog|published=|recorded" "PerfLog windows"

	run_session "soak-idle-35s" 80 "-" "vacuus.M1HUD.PerfLog 1, vacuus.M1HUD" ${GAME_FLAGS}
	rep "### Static idle, ~35 s → row 4's confirmed idle venue and row 8's idle gate"
	grep_report "${LAST_SESSION_LOG}" "PerfLog|published=|recorded" "PerfLog windows"

	run_session "soak-glass-25s" 70 "-" "vacuus.M1HUD.PerfLog 1, vacuus.M5Glass" ${GAME_FLAGS}
	rep "### Glass idle, ~25 s → row 8's glass line"
	grep_report "${LAST_SESSION_LOG}" "PerfLog|published=|recorded|Glass" "PerfLog windows"

	run_session "soak-m2-60s" 110 "-" "vacuus.M1HUD.PerfLog 1, vacuus.M2Demo" ${GAME_FLAGS}
	rep "### M2 demo, ~60 s → row 2's typical-scale figures"
	grep_report "${LAST_SESSION_LOG}" "PerfLog|published=|recorded" "PerfLog windows"
	rep ""
}

# ---------------------------------------------------------------------------------------
# 9. THE HONEST HALF — what the script did NOT run, and why.
# ---------------------------------------------------------------------------------------
write_not_run_section() {
	rep ""
	rep "## What this script did NOT run — and why"
	rep ""
	rep "Plan §1's rule: a row that cannot run gets its reason recorded, never silently dropped."
	rep "Everything below is owed and unclosed. Nothing here is a pass."
	rep ""
	rep "### Interactive — a human must be at the keyboard"
	rep ""
	rep "| Item | Why the script cannot | What to do |"
	rep "|---|---|---|"
	rep "| **Matrix row 5 — IME composition** | Composition needs real keystrokes through the platform IME; there is nothing to synthesize. It is also plan §4 **risk 2**: the whole \`ITextInputMethodContext\` runs for the first time here. | Focus the input, then a **Latin** keyboard first: hold \`e\` for the accent picker, or Option-e then e. Marked text, same never-run path, no CJK IME needed. Watch for duplicated composition, wrong insertion offset, double commit, or the field refusing characters after one composition. |"
	rep "| **Risk 2b — IME shadow staleness within one frame** | Needs a timing difference only human typing produces. | Same field, same text, twice: ~2 chars/sec, then as fast as you can. A divergence is this and nothing else. |"
	rep "| **Risk 3 — candidate window misplaced ~2× on Retina** | Requires seeing where macOS drew the candidate window. | **Differential test:** compose into a native Slate text box (the UE console, \`~\`) in the same session. Misplaced for both ⇒ engine, record as a venue note; misplaced only over RmlUi ⇒ ours. |"
	rep "| **Risk 6 — AppKit intercepting keys after the IME context is active** | Needs a human-ordered sequence. | **Row 4 → row 5 → row 4 again, one session.** The second row-4 run is the whole test; it has no Linux equivalent. |"
	rep "| **Matrix row 13 — live reload** | The watcher lives in \`VaCuusEditor\` and needs an interactive editor + a mid-run file edit; not expressible in a frame-0 \`-ExecCmds\` run. | PIE with a loaded document → edit the \`.rcss\` on disk → watch the view repaint → mount a bundle → edit again → assert the shadowing Warning names the path. Note macOS is FSEvents (\`DirectoryWatchRequestMac.cpp:66-81\`, 0.2 s coalescing), so the timing differs from Linux inotify, and the two \`#if PLATFORM_LINUX\` assertions in \`VaCuusLiveReloadTest.cpp:280-282\` compile out here — the test still runs, it just proves less. |"
	rep "| **Row 12's editor-PIE leg** | PIE cannot be driven from \`-ExecCmds\` at frame 0. | Run the A/B again inside the editor; this is the leg the Linux column explicitly deferred to this pass (plan block 8). |"
	rep "| **Rows 3/4 HoverShot & TypeShot coordinates** | The plugin feeds \`x y\` straight to \`FSlateApplication::SetCursorPos\` as **absolute** Slate pixels; on Mac that is not view-pixel space and the delta is the window origin plus the title bar. The script cannot place the window. | Move the window to the top-left of the main display (or fullscreen), add the window origin, re-run. \`unhandled\` in the self-check means fix the offset and re-run — **not** record a FAIL. |"
	rep "| **Risk 11 — Retina ↔ non-Retina display move** | Needs a physical drag between displays. | \`vacuus.LobbyDemo\` windowed, drag between displays, watch the logged ViewSize and whether the design box letterboxes (1280×800 points is 2560×1600 px fluid on Retina, 1280×800 px letterboxed to the 1920×1080 design externally). Re-run any coordinate row afterwards — \`CachedInputGeometry\` only refreshes in Tick. |"
	rep "| **Every \"read the screenshot by eye\" verdict** | The script takes the images; it cannot read them. | Each row above says what to look for; the images are under \`${SHOTS_DIR}\`. |"
	rep ""
	rep "### Needs judgement, or a longer session than a bootstrap should own"
	rep ""
	rep "| Item | Why | What to do |"
	rep "|---|---|---|"
	if [ "${WITH_SOAKS}" = "1" ]; then
		rep "| **Passport §11 soaks (block 7)** | Run with \`--with-soaks\`, but only as **raw material**: window selection, boot-window exclusion and which scopes were summed are judgements. | Apply the Method and fill rows 1, 2, 2a, 2b, 3, 4, 7, 8 from the collected PerfLog windows. |"
	else
		rep "| **Passport §11 Dev soaks (block 7)** — RefHud 100 s, static idle 35 s, glass idle 25 s, M2 ~60 s | Not run: a soak produces numbers, and a number without a Method is not a filled cell. | Re-run this script with \`--with-soaks\` to collect the raw PerfLog windows, then write the Method: venue, duration, frame count, window selection (exclude the boot window), scopes summed. |"
	fi
	rep "| **Blocks 10–11 — packaging (Development, then Shipping) and the Shipping column** | 60–150 min, drives \`xcodebuild\` through a generated stub project (\`AppleExports.cs:45-65, :229-271\`), and is the block that can eat the day. | The config this script already wrote unblocks it: \`bMacSignToRunLocally=True\` (ad-hoc \`CODE_SIGN_IDENTITY = -\`, \`XcodeProject.cs:2274-2290\`) and \`TargetArchitecture=MacTargetArchitectureHost\`. Remember: touch \`VaCuus.Build.cs\` first (stale receipt), Development **then** Shipping, pass \`-SaveToUserDir\`, and check the staging shape (Shipping must stage only \`DevUIBundle.uasset\`). |"
	rep "| **Matrix row 15 — Shipping ignition flags** | Needs the packaged Shipping build from block 10. | Run the staged inner binary with \`-VaCuusRefHud\` / \`-VaCuusM5Demo\` (+\`-VaCuusPerfLog\`, \`-VaCuusMemProbe\`, \`-VaCuusLobbyDemo\`); gate screenshot at t+8 s. Assert \`M == 0\` on both VFS serving lines. |"
	rep "| **The RAM row's cross-check (passport row 5)** | A real methodological difference, not boilerplate: \`FPlatformMemory::GetStats().UsedPhysical\` is \`ri_phys_footprint\` from \`proc_pid_rusage\` on Apple (\`ApplePlatformMemory.cpp:423\`), while the Linux figure it is compared to is \`VmRSS\` (\`UnixPlatformMemory.cpp:917\`). | Say so on the row. And note **there is no Mac command that reads \`phys_footprint\` from outside** (\`ps -o rss\` is not it), so the external cross-check either changes instrument or is recorded as not reproduced. |"
	rep "| **The cook's bundle hash** | Needs a cook. | Expected to match Linux's \`adcb1da0b34dffdb071d3f9db02fd780eceb1f4e700eae66ed79966ed8015017\` at 461881 bytes — identical is a free cross-platform determinism proof, different is a finding worth chasing before anything else in the packaging block. |"
	if [ "${METAL_DEBUG_LAYER}" = "1" ]; then
		rep "| **\`MTL_DEBUG_LAYER=1\`** | Was enabled for this run (\`--metal-debug-layer\`). | Read the session logs for named aborts — plan §4 risk 9 (the composite output rect is the one unclamped viewport/scissor in the render path, and Metal is stricter than Vulkan). |"
	else
		rep "| **\`MTL_DEBUG_LAYER=1\` sweep** | Not enabled: it turns latent bounds/usage violations into named aborts, which would stop the visual rows mid-pass. | Plan §4 risk 9 calls it \"highest value per keystroke on this list\" — re-run with \`--metal-debug-layer\` once the rows above have been read, especially exercising **window resize and monitor switch** (transitions only, which is why it survived Linux). |"
	fi
	rep "| **\`Proof.LiveReload.PIE\`** | Its path has no \"VaCuus\" substring, so \`Automation RunTests VaCuus\` does not select it (\`AutomationCommandline.cpp:133-135\` is a plain substring match). | Name it explicitly, in a PIE-capable editor session. |"
	rep "| **SHIM-1 / \`BuildPlugin -StrictIncludes\` for Mac** | ~40 min, and expected to fail once — that is the point. **This leg can only be run on a Mac**, and conversely a Mac-host \`BuildPlugin\` **silently drops** the Win64/Linux legs (\`BuildPluginCommand.Automation.cs:499-508\`), so a green Mac run says nothing about the others. | \`RunUAT.sh BuildPlugin -Plugin=<abs>/VaCuus.uplugin -Package=<abs-outside> -TargetPlatforms=Mac -StrictIncludes\` |"
	rep ""
	rep "### Not this platform's evidence at all (plan §5)"
	rep ""
	rep "- **Passport row 6's Win64 disk literal** — another platform's row by definition; a Mac disk delta is a *second proxy*, not a substitute, and only meaningful with \`TargetArchitecture=Host\` pinned."
	rep "- **The Win64 IME re-check (bead akj.6.19)** — TSF-specific. A Mac IME pass is *additional* evidence, never a substitute: keep the bead open and open a Mac sibling line."
	rep "- **The quickjs \`/experimental:c11atomics\` question** — MSVC vs clang-cl only. Apple clang compiling the vendored C proves nothing about it."
	rep "- **Any headless-with-RHI row** — impossible on Mac by construction. Every visual row's venue note must say \"windowed, real Metal RHI\"."
	rep ""
	rep "### One venue property to record, not report as a regression"
	rep ""
	rep "The packaged game's \`Info.plist\` sets \`NSHighResolutionCapable = false\` while the editor's"
	rep "sets it \`true\` (\`Info.Template.plist:10-11\` vs \`Info-Editor.Template.plist:43-44\`, both"
	rep "verified), and \`IsHighDPIModeEnabled()\` is \`bIsHighResolutionCapable && IsHighDPIAwarenessEnabled()\`"
	rep "(\`MacPlatformApplicationMisc.h:35\`). So the **Dev column runs Retina-aware and the Shipping"
	rep "column runs at 1× upscaled by the WindowServer**: risk 1 shows in Dev and hides in Shipping,"
	rep "and the packaged HUD will look **softer** on a Retina panel at the same 1920×1080 RT and the"
	rep "same perf numbers."
}

write_header() {
	mkdir -p "${RESULTS_DIR}" "${SHOTS_DIR}" "${LOGS_DIR}"
	: > "${REPORT}"
	: > "${CONSOLE_LOG}"
	ISSUES_FILE="${RESULTS_DIR}/.issues"; : > "${ISSUES_FILE}"
	rep "# VaCuus macOS/Metal pass — bootstrap report"
	rep ""
	rep "Generated by \`Tools/mac_bootstrap.sh\` at ${STAMP} (UTC)."
	rep ""
	rep "This report is **evidence and a to-do list**, not a verdict. The script ran the plan's"
	rep "automatable blocks; the verdicts that need an eye or a keyboard are listed, by name and"
	rep "with their reason, in \"What this script did NOT run\" at the end. The reasoning behind"
	rep "every check lives in \`${PLAN_REL}\`."
	rep ""
	rep "| | |"
	rep "|---|---|"
	rep "| host | \`$(uname -a 2>/dev/null | cut -c1-120)\` |"
	rep "| macOS | \`$(sw_vers -productVersion 2>/dev/null || echo n/a)\` (build \`$(sw_vers -buildVersion 2>/dev/null || echo n/a)\`) |"
	rep "| work dir | \`${WORK_DIR}\` |"
	rep "| results | \`${RESULTS_DIR}\` |"
	rep ""
	rep "<!-- HEADLINE -->"
	rep ""
}

finish_report() {
	rep ""
	rep "## Venue facts to copy into every Method sentence"
	rep ""
	rep "| | |"
	rep "|---|---|"
	rep "| engine | ${ENGINE_VERSION}, ${ENGINE_FLAVOUR} |"
	rep "| feature level | see §6 above — the log, not the prediction (predicted: ${FEATURE_LEVEL_PREDICTION}) |"
	rep "| venue | windowed, real Metal RHI, \`-ForceRes\` 1920×1080 (there is no offscreen mode on Mac) |"
	rep "| architecture | host only for the editor target (\`EditorDefaultArchitecture=MacTargetArchitectureHost\`, \`BaseEngine.ini:3441\`); packaging is pinned to Host by the config this script wrote |"
	rep "| plugin HEAD | \`$(cd "${PLUGIN_DIR}" 2>/dev/null && git rev-parse --short HEAD 2>/dev/null || echo unknown)\` |"
	rep ""
	write_not_run_section
	rep ""
	rep "---"
	rep ""
	rep "Logs: \`${LOGS_DIR}\` · screenshots: \`${SHOTS_DIR}\` · console transcript: \`${CONSOLE_LOG}\`"
	FINISHED=1
}

# ---------------------------------------------------------------------------------------
main() {
	write_header
	trap on_exit EXIT

	banner "VaCuus macOS bootstrap — report will be written to ${REPORT}"
	say "plan: ${PLAN_REL} (read it; this script implements its automatable half)"
	say ""

	run_self_tests
	preflight

	if [ "${PREFLIGHT_ONLY}" = "1" ]; then
		say ""
		say "--preflight-only: stopping before anything expensive."
		rep ""
		rep "\`--preflight-only\` was passed: nothing after the checks ran."
		finish_report
		exit 0
	fi

	fetch_plugin
	ensure_project
	build_editor
	run_suite
	run_gpu_pair
	run_rows
	if [ "${WITH_SOAKS}" = "1" ]; then run_soaks; fi

	finish_report
	banner "DONE — read ${REPORT}"
	say "The script produced evidence. The verdicts that need an eye or a keyboard are listed"
	say "under 'What this script did NOT run' at the end of that report."
}

main "$@"
