#!/usr/bin/env bash
# Regenerates Private/Gen/relay_*.cpp — one relay TU per vendored RmlUi Core .cpp.
# UBT only compiles sources inside the module directory, so each vendored .cpp
# gets a relay TU that #includes it. Re-run only when the vendored SHA changes,
# then commit the result.
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"; RML="$ROOT/../ThirdParty/RmlUi/Source"
OUT="$ROOT/Private/Gen"; rm -rf "$OUT"; mkdir -p "$OUT"
find "$RML/Core" -name '*.cpp' | sort | while read -r f; do
  rel="$(realpath --relative-to="$OUT" "$f")"
  base="${f#"$RML"/}"; base="${base%.cpp}"; name="$(echo "$base" | tr '/' '_')"
  printf '#include "%s"\n' "$rel" > "$OUT/relay_${name}.cpp"
done
echo "generated $(ls "$OUT" | wc -l) relay TUs"
