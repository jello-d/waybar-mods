#!/bin/sh
# recipe.t - every overlay source and patch the build recipe references must
# exist in the repo. Catches a moved/renamed/typo'd source before a real build
# fails cryptically halfway through (the class of bug the old in-tackup recipe
# hid: a stale cp path shipping a binary missing a module).
. "$(dirname "$0")/lib.sh"
harness_init recipe

_r=$HERE/libexec/build-waybar
[ -f "$_r" ] || fail "no libexec/build-waybar"

_missing=
for _f in $(grep -oE '\$OV/[A-Za-z0-9_.-]+' "$_r" | sed 's,[$]OV/,,' \
            | sort -u); do
  [ -e "$HERE/overlays/$_f" ] || _missing="$_missing overlays/$_f"
done
for _f in $(grep -oE '\$PAT/[A-Za-z0-9_.-]+' "$_r" | sed 's,[$]PAT/,,' \
            | sort -u); do
  [ -e "$HERE/patches/$_f" ] || _missing="$_missing patches/$_f"
done
[ -z "$_missing" ] || fail "recipe references missing files:$_missing"

# and nothing in overlays/patches is orphaned (referenced nowhere) -- an unused
# source is either a forgotten wiring or dead weight.
_orphan=
for _p in "$HERE"/overlays/* "$HERE"/patches/*; do
  _b=$(basename "$_p")
  grep -qF "$_b" "$_r" || _orphan="$_orphan $_b"
done
[ -z "$_orphan" ] || fail "sources not referenced by the recipe:$_orphan"

pass
