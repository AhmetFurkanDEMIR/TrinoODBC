#!/usr/bin/env bash
# Packs the Power Query custom connector into TrinoODBC.mez.
#
# A .mez file is a ZIP archive containing the connector's section document (TrinoODBC.pq) plus any
# resources (resources.resx, *.png icons) placed next to it. The test query file is not part of
# the archive. Works with `zip` or, when that is missing, with python3's zipfile module.
#
# Usage: powerbi/build_mez.sh [output-directory]   (default: powerbi/dist)
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
name="TrinoODBC"
out_dir="${1:-$here/dist}"
mez="$out_dir/$name.mez"

if [[ ! -f "$here/$name.pq" ]]; then
  echo "error: $here/$name.pq not found" >&2
  exit 1
fi

# Section document first, then optional resources. nullglob keeps a missing *.png from becoming a literal.
shopt -s nullglob
files=("$name.pq")
[[ -f "$here/resources.resx" ]] && files+=("resources.resx")
for png in "$here"/*.png; do
  files+=("$(basename "$png")")
done
shopt -u nullglob

mkdir -p "$out_dir"
rm -f "$mez"

if command -v zip >/dev/null 2>&1; then
  # -X: no extra file attributes, -D: no directory entries, -q: quiet. Paths are relative to $here.
  (cd "$here" && zip -X -D -q "$mez" "${files[@]}")
elif command -v python3 >/dev/null 2>&1; then
  python3 - "$here" "$mez" "${files[@]}" <<'PY'
import sys, zipfile, os
root, target, *names = sys.argv[1:]
with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as z:
    for n in names:
        z.write(os.path.join(root, n), n)
PY
else
  echo "error: neither 'zip' nor 'python3' is available to create the archive" >&2
  exit 1
fi

echo "wrote $mez"
echo "install: copy it to '%USERPROFILE%\\Documents\\Power BI Desktop\\Custom Connectors\\' (Desktop)"
echo "         or to the connectors folder configured on the on-premises data gateway (see powerbi/README.md)"
