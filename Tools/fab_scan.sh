#!/usr/bin/env bash
# Tools/fab_scan.sh — the Fab no-executables scan (M6 spec §2(e); research
# docs/research/m6-api-notes/buildplugin-fab.md §2). Run it over a `RunUAT BuildPlugin
# -Package=<dir>` output before zipping:
#
#   bash Tools/fab_scan.sh <package-dir>
#
# Six violation classes — Fab's recorded rules (source-shipping mandate 4.3.6.1.a,
# .exe/.msi ban 4.3.6.1.e) plus the failure shapes the research found waiting in THIS
# repo (the esbuild ELF under node_modules; LFS pointers that never smudged):
#
#   EXTENSION     blacklisted name: *.exe *.msi *.dll *.dylib *.so *.bat *.cmd
#                 (.so would be legitimate only under Binaries/ThirdParty, which
#                 VaCuus does not use — so the blacklist is unconditional)
#   EXECBIT      any file with any execute permission bit
#   SHEBANG      any file whose first two bytes are `#!`, minus the whitelist below
#   ELF_MAGIC    any file whose first four bytes are \x7fELF — an ELF payload with a
#                harmless name, no exec bit and no shebang evades the other five
#                classes, and that renamed-esbuild shape is exactly what a
#                no-executables scan exists to stop
#   NODE_MODULES any directory literally named node_modules
#   LFS_POINTER  any file opening with the git-lfs pointer signature — a checkout
#                that never smudged would package ~130-byte pointers SILENTLY
#
# FAIL-CLOSED CONTRACT (M6 review round 1 — the reviewer demonstrated the original
# scan blessing an empty directory and a tree with an unreadable subdir):
#   - the target must LOOK like a BuildPlugin package: a *.uplugin at its root
#     (every -Package output carries the rewritten descriptor,
#     BuildPluginCommand.Automation.cs:433-445) — else abort, exit 2;
#   - every find/head error lands in an error file; if ANY check could not read any
#     part of any scanned tree, the scan ABORTS (exit 2) instead of reporting CLEAN
#     over the part it saw.
#
# THE SCAN MUST BE SEEN TO FAIL BEFORE IT MAY PASS (spec §2(e)). Every invocation
# therefore self-tests, twice, and aborts unless both reports are EXACT:
#   1. Tools/scan-fixture/ (no whitelist): one committed plant per class, each
#      tripping exactly one check — must report exactly the six plants;
#   2. Tools/scan-fixture-whitelist/ (whitelist mode): a shebang file AT a
#      whitelisted path (must be forgiven) beside a near-miss (must be reported) —
#      the whitelist branch itself is seen to both admit and refuse.
#
# Known limits, stated rather than discovered:
#   - symlinks are not followed (`find` default) — right for UAT-materialized
#     package trees, which contain none; a tree that hides content behind symlinks
#     is out of contract;
#   - on Windows noacl mounts (Git-for-Windows), -perm /111 reports spurious exec
#     bits and the self-test refuses everything — run the scan under WSL or on
#     Linux (fail-closed, see SHIM-1 step 4).
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIXTURE_DIR="${SCRIPT_DIR}/scan-fixture"
WHITELIST_FIXTURE_DIR="${SCRIPT_DIR}/scan-fixture-whitelist"

# The shebang whitelist (package-relative paths). Shebang lines OUTSIDE this list are
# violations; entries IN it still must not carry exec bits (EXECBIT has no whitelist).
#  - gen_relays.sh ×2: the documented re-vendor procedure (invoked as `bash .../gen_relays.sh`);
#    ships as source on purpose, exec bit stripped in M6.
#  - vacuus.mjs: the npm CLI entry (`bin` in Web/packages/cli/package.json); npm itself
#    chmods bin targets on install, so the committed file carries a shebang and no bit.
SHEBANG_WHITELIST=(
	"Source/VaCuusJs/gen_relays.sh"
	"Source/VaCuusRml/gen_relays.sh"
	"Web/packages/cli/bin/vacuus.mjs"
)

ERRFILE="$(mktemp)" || exit 2
trap 'rm -f "$ERRFILE"' EXIT

# Emits "CLASS<TAB>relative-path" lines for every violation under $1, sorted. $2 =
# "whitelist" applies the shebang whitelist (the real scan); self-test 1 runs without
# it, self-test 2 with it. All read errors land in ERRFILE (the fail-closed hook).
scan_tree() {
	local root="$1"
	local mode="${2:-}"
	local rel head4

	{
		# EXTENSION — blacklisted names.
		find "$root" -type f \( -iname '*.exe' -o -iname '*.msi' -o -iname '*.dll' \
			-o -iname '*.dylib' -o -iname '*.so' -o -iname '*.bat' -o -iname '*.cmd' \) -print 2>>"$ERRFILE" |
			while IFS= read -r f; do
				printf 'EXTENSION\t%s\n' "${f#"$root"/}"
			done

		# EXECBIT — any execute bit, any file.
		find "$root" -type f -perm /111 -print 2>>"$ERRFILE" |
			while IFS= read -r f; do
				printf 'EXECBIT\t%s\n' "${f#"$root"/}"
			done

		# SHEBANG + ELF_MAGIC — one 4-byte read per file, compared as hex so binary
		# content neither warns in the substitution nor confuses the comparison
		# ('#!' = 2321..; ELF = exactly 7f454c46).
		find "$root" -type f -print 2>>"$ERRFILE" |
			while IFS= read -r f; do
				head4="$(head -c 4 "$f" 2>>"$ERRFILE" | od -An -tx1 | tr -d ' \n')"
				if [ "$head4" = '7f454c46' ]; then
					printf 'ELF_MAGIC\t%s\n' "${f#"$root"/}"
				elif [ "${head4:0:4}" = '2321' ]; then
					rel="${f#"$root"/}"
					if [ "$mode" = "whitelist" ]; then
						local listed=0 w
						for w in "${SHEBANG_WHITELIST[@]}"; do
							[ "$rel" = "$w" ] && listed=1 && break
						done
						[ "$listed" = 1 ] && continue
					fi
					printf 'SHEBANG\t%s\n' "$rel"
				fi
			done

		# NODE_MODULES — the directory itself is the finding.
		find "$root" -type d -name node_modules -print 2>>"$ERRFILE" |
			while IFS= read -r d; do
				printf 'NODE_MODULES\t%s\n' "${d#"$root"/}"
			done

		# LFS_POINTER — the pointer signature within the head of the file (a real
		# pointer's first line is `version https://git-lfs.github.com/spec/v1`).
		find "$root" -type f -print 2>>"$ERRFILE" |
			while IFS= read -r f; do
				if head -c 200 "$f" 2>>"$ERRFILE" | grep -q 'git-lfs\.github\.com/spec/v1'; then
					printf 'LFS_POINTER\t%s\n' "${f#"$root"/}"
				fi
			done
	} | LC_ALL=C sort
}

# Aborts (exit 2) if any check failed to read anything since the last call. $1 names
# the phase for the message. This is what makes silence mean CLEAN, not "unseen".
abort_on_read_errors() {
	if [ -s "$ERRFILE" ]; then
		echo "SCAN ABORTED: find could not read the tree ($1):" >&2
		cat "$ERRFILE" >&2
		exit 2
	fi
}

if [ $# -ne 1 ] || [ ! -d "$1" ]; then
	echo "usage: $0 <package-dir>   (directory produced by RunUAT BuildPlugin -Package=...)" >&2
	exit 3
fi
if ! TARGET_DIR="$(cd "$1" 2>/dev/null && pwd)"; then
	echo "SCAN ABORTED: cannot enter target directory '$1'" >&2
	exit 2
fi

# Fail-closed shape check: a BuildPlugin package always carries the rewritten
# .uplugin at its root (BuildPluginCommand.Automation.cs:433-445). No descriptor
# means this is not a package output — an empty or wrong directory must refuse,
# never pass vacuously.
if ! ls "$TARGET_DIR"/*.uplugin >/dev/null 2>&1; then
	echo "SCAN ABORTED: no *.uplugin at $TARGET_DIR — not a BuildPlugin package root" >&2
	exit 2
fi

# --- Phase 1: the self-tests. The scan fails against the fixtures, and the failures
# --- are printed, every run — or the scan is not trusted to pass anything.
if [ ! -d "$FIXTURE_DIR" ] || [ ! -d "$WHITELIST_FIXTURE_DIR" ]; then
	echo "SELF-TEST FAILED: fixtures missing under $SCRIPT_DIR" >&2
	exit 2
fi

EXPECTED="$(
	LC_ALL=C sort <<-'EOF'
	ELF_MAGIC	planted_elf.bin
	EXECBIT	planted_execbit.txt
	EXTENSION	planted.exe
	LFS_POINTER	planted_lfs_pointer.txt
	NODE_MODULES	node_modules
	SHEBANG	planted_shebang.sh
	EOF
)"
ACTUAL="$(scan_tree "$FIXTURE_DIR")"
abort_on_read_errors "self-test fixture"

echo "== fab_scan self-test 1: scanning the planted fixture (must report exactly the 6 plants) =="
echo "$ACTUAL"
if [ "$ACTUAL" != "$EXPECTED" ]; then
	echo "SELF-TEST FAILED: fixture report does not match the known plants." >&2
	echo "--- expected ---" >&2
	echo "$EXPECTED" >&2
	echo "--- got ---" >&2
	echo "$ACTUAL" >&2
	exit 2
fi

# --- Phase 1b: the whitelist branch, seen to both admit and refuse: the file AT the
# --- whitelisted path must be forgiven, its near-miss neighbour must be reported.
EXPECTED_WL="$(printf 'SHEBANG\tSource/VaCuusJs/gen_relays_nearmiss.sh')"
ACTUAL_WL="$(scan_tree "$WHITELIST_FIXTURE_DIR" whitelist)"
abort_on_read_errors "whitelist fixture"

echo "== fab_scan self-test 2: whitelist mode (must forgive the whitelisted path, report the near-miss) =="
echo "$ACTUAL_WL"
if [ "$ACTUAL_WL" != "$EXPECTED_WL" ]; then
	echo "SELF-TEST FAILED: whitelist report does not match." >&2
	echo "--- expected ---" >&2
	echo "$EXPECTED_WL" >&2
	echo "--- got ---" >&2
	echo "$ACTUAL_WL" >&2
	exit 2
fi
echo "== self-tests OK: the scan has been seen to fail, exactly as planted =="
echo

# --- Phase 2: the real tree.
echo "== fab_scan: scanning $TARGET_DIR =="
FINDINGS="$(scan_tree "$TARGET_DIR" whitelist)"
abort_on_read_errors "target tree"
if [ -n "$FINDINGS" ]; then
	echo "$FINDINGS"
	echo "SCAN: FAIL ($(printf '%s\n' "$FINDINGS" | wc -l) violation(s))"
	exit 1
fi
echo "SCAN: CLEAN"
exit 0
