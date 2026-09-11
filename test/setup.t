#!/bin/sh
# setup.t - the dispatch + audit path, no build. `version` reports the upstream
# pin; `check` against a prefix with no waybar reports [FAIL] not-installed and
# exits non-zero (the audit is honest about an absent install, not falsely
# green).
. "$(dirname "$0")/lib.sh"
harness_init setup

sh "$HERE/setup.sh" version | grep -q 'Waybar ' \
  || fail "version does not report the upstream pin"

_out=$(env HOME="$T" PREFIX="$T/p" WBM_CACHE="$T/c" \
  sh "$HERE/setup.sh" check 2>&1) && fail "check passed with no waybar built"
printf '%s\n' "$_out" | grep -q 'not installed' \
  || fail "check did not report the missing waybar"

sh "$HERE/setup.sh" bogus >/dev/null 2>&1 \
  && fail "unknown verb did not error" || :

pass
