#!/usr/bin/env bash
# Tools/fab_scan.sh — the Fab no-executables scan (M6 spec §2(e); research
# docs/research/m6-api-notes/buildplugin-fab.md §2). Run it over a `RunUAT BuildPlugin
# -Package=<dir>` output before zipping:
#
#   bash Tools/fab_scan.sh <package-dir>
#
# Five violation classes, from Fab's recorded rules (source-shipping mandate 4.3.6.1.a,
# .exe/.msi ban 4.3.6.1.e) plus the two failure shapes the research found waiting in
# THIS repo (the esbuild ELF under node_modules; LFS pointers that never smudged):
#
#   EXTENSION     blacklisted name: *.exe *.msi *.dll *.dylib *.so *.bat *.cmd
#                 (.so would be legitimate only under Binaries/ThirdParty, which
#                 VaCuus does not use — so the blacklist is unconditional)
#   EXECBIT      any file with any execute permission bit
#   SHEBANG      any file whose first two bytes are `#!`, minus the whitelist below
#   NODE_MODULES any directory literally named node_modules
#   LFS_POINTER  any file opening with the git-lfs pointer signature — a checkout
#                that never smudged would package ~130-byte pointers SILENTLY
#
# THE SCAN MUST BE SEEN TO FAIL BEFORE IT MAY PASS (spec §2(e): "the wrapper runs
# against the fixture first and must report exactly those hits before its clean verdict
# counts"). Every invocation therefore self-tests against Tools/scan-fixture/ — one
# committed plant per class, each tripping exactly one check — and aborts unless the
# report is EXACTLY the five known plants: a broken check surfaces as a missing line,
# an over-eager one as an extra line. Only then is the real tree scanned.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIXTURE_DIR="${SCRIPT_DIR}/scan-fixture"

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

# Emits "CLASS<TAB>relative-path" lines for every violation under $1, sorted. $2 = "whitelist"
# applies the shebang whitelist (the real scan); the self-test runs without it.
scan_tree() {
	local root="$1"
	local mode="${2:-}"
	local rel first2

	{
		# EXTENSION — blacklisted names.
		find "$root" -type f \( -iname '*.exe' -o -iname '*.msi' -o -iname '*.dll' \
			-o -iname '*.dylib' -o -iname '*.so' -o -iname '*.bat' -o -iname '*.cmd' \) -print |
			while IFS= read -r f; do
				printf 'EXTENSION\t%s\n' "${f#"$root"/}"
			done

		# EXECBIT — any execute bit, any file.
		find "$root" -type f -perm /111 -print |
			while IFS= read -r f; do
				printf 'EXECBIT\t%s\n' "${f#"$root"/}"
			done

		# SHEBANG — first two bytes are '#!'. tr strips NUL so binaries (which can
		# open with one) neither warn in the substitution nor ever equal '#!'.
		find "$root" -type f -print |
			while IFS= read -r f; do
				first2="$(head -c 2 "$f" 2>/dev/null | tr -d '\0')"
				if [ "$first2" = '#!' ]; then
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
		find "$root" -type d -name node_modules -print |
			while IFS= read -r d; do
				printf 'NODE_MODULES\t%s\n' "${d#"$root"/}"
			done

		# LFS_POINTER — the pointer signature within the head of the file (a real
		# pointer's first line is `version https://git-lfs.github.com/spec/v1`).
		find "$root" -type f -print |
			while IFS= read -r f; do
				if head -c 200 "$f" 2>/dev/null | grep -q 'git-lfs\.github\.com/spec/v1'; then
					printf 'LFS_POINTER\t%s\n' "${f#"$root"/}"
				fi
			done
	} | LC_ALL=C sort
}

if [ $# -ne 1 ] || [ ! -d "$1" ]; then
	echo "usage: $0 <package-dir>   (directory produced by RunUAT BuildPlugin -Package=...)" >&2
	exit 3
fi
TARGET_DIR="$(cd "$1" && pwd)"

# --- Phase 1: the self-test. The scan fails against the fixture, and the failure is
# --- printed, every run — or the scan is not trusted to pass anything.
if [ ! -d "$FIXTURE_DIR" ]; then
	echo "SELF-TEST FAILED: fixture missing at $FIXTURE_DIR" >&2
	exit 2
fi

EXPECTED="$(
	LC_ALL=C sort <<-'EOF'
	EXECBIT	planted_execbit.txt
	EXTENSION	planted.exe
	LFS_POINTER	planted_lfs_pointer.txt
	NODE_MODULES	node_modules
	SHEBANG	planted_shebang.sh
	EOF
)"
ACTUAL="$(scan_tree "$FIXTURE_DIR")"

echo "== fab_scan self-test: scanning the planted fixture (must report exactly the 5 plants) =="
echo "$ACTUAL"
if [ "$ACTUAL" != "$EXPECTED" ]; then
	echo "SELF-TEST FAILED: fixture report does not match the known plants." >&2
	echo "--- expected ---" >&2
	echo "$EXPECTED" >&2
	echo "--- got ---" >&2
	echo "$ACTUAL" >&2
	exit 2
fi
echo "== self-test OK: the scan has been seen to fail, exactly as planted =="
echo

# --- Phase 2: the real tree.
echo "== fab_scan: scanning $TARGET_DIR =="
FINDINGS="$(scan_tree "$TARGET_DIR" whitelist)"
if [ -n "$FINDINGS" ]; then
	echo "$FINDINGS"
	echo "SCAN: FAIL ($(printf '%s\n' "$FINDINGS" | wc -l) violation(s))"
	exit 1
fi
echo "SCAN: CLEAN"
exit 0
