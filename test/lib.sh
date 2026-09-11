# test/lib.sh - harness for waybar-mods's shell tests (test/*.t), sourced by
# each. Call `harness_init <name>`: sets HERE (the repo root, so a test reaches
# setup.sh + overlays/ + patches/ + libexec/), a private scratch dir T (removed
# on exit), and the pass/fail helpers. Everything a test touches is confined to
# T. POSIX sh; run one with `sh test/<name>.t`, all with test/run.
harness_init() {   # <name>
  TEST_NAME=$1
  HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
  T=$(mktemp -d)
  trap 'rm -rf "$T"' EXIT INT TERM
}
pass() { printf 'ok   %s%s\n' "$TEST_NAME" "${1:+ ($1)}"; }
fail() { printf 'FAIL %s: %s\n' "$TEST_NAME" "$1" >&2; exit 1; }
skip() { printf 'skip %s (%s)\n' "$TEST_NAME" "$1"; exit 0; }
