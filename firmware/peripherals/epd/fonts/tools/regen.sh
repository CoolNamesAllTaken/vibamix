#!/usr/bin/env bash
# Regenerate the Poppins pFONT tables from the vendored OFL TTFs.
# Requires Pillow with the freetype backend:  pip install Pillow
#
#   ./regen.sh            # regenerate all three .cpp into ../
#   ./regen.sh prev       # also write PNG previews into ./preview/
set -euo pipefail
cd "$(dirname "$0")"

PY="${PYTHON:-python3}"
PREVIEW=""
if [ "${1:-}" = "prev" ]; then
  mkdir -p preview
  PREVIEW=1
fi

gen() { # ttf size inst out
  local args=(--ttf "$1" --size "$2" --inst "$3" --out "../$4")
  [ -n "$PREVIEW" ] && args+=(--preview "preview/$3.png")
  "$PY" gen_font.py "${args[@]}"
}

# Mixed weights: SemiBold for the name (24px), Medium for body/labels (20/16px).
gen Poppins-SemiBold.ttf 24 PoppinsSB24 poppins_sb24.cpp
gen Poppins-Medium.ttf   20 PoppinsMd20 poppins_md20.cpp
gen Poppins-Medium.ttf   16 PoppinsMd16 poppins_md16.cpp
