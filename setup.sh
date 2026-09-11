#!/bin/sh
# setup.sh - build / verify / check the waybar-mods package: upstream Waybar
# (pinned in refs) plus our overlay modules and patches, built from source into
# a prefix. The SINGLE entry point a consumer or provisioning layer uses.
#
#   ./setup.sh install     build + install waybar into ~/.local (INCREMENTAL:
#                          a no-op when the installed waybar matches the current
#                          overlays/patches/upstream pin -- safe to re-run every
#                          provision)
#   ./setup.sh verify      build into a THROWAWAY prefix and discard it; proves
#                          the overlays + patches still apply and compile
#                          against the pinned upstream, without touching
#                          ~/.local (CI /
#                          the check you run while iterating on a mod)
#   ./setup.sh check       waybar installed, self-contained by rpath, and in
#                          sync with the source; [OK]/[FAIL] markers
#   ./setup.sh uninstall   remove the installed waybar + the build stamp
#   ./setup.sh test        run the in-repo suite (test/run)
#   ./setup.sh version     the packaged version + the upstream pin
#
# POSIX sh, non-privileged: the whole path is a user build into a user prefix,
# no sudo. The heavy work is delegated to libexec/build-waybar (the shared
# recipe); this file owns the incremental gate, the stamp, and the audit.
set -eu

PKG=waybar-mods
VERSION=0.1.0
_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ -z "${HOME:-}" ]; then
  HOME=$(getent passwd "$(id -u)" 2>/dev/null | cut -d: -f6 || true)
  if [ -z "$HOME" ]; then
    echo "$PKG: HOME unset and not derivable from passwd" >&2; exit 1
  fi
  export HOME
fi

PREFIX=${PREFIX:-$HOME/.local}
CACHE=${WBM_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/waybar-mods}
STAMP=$CACHE/stamp
WAYBAR=$PREFIX/bin/waybar
RC=0

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
  _G=$(printf '\033[32m'); _R=$(printf '\033[31m')
  _Y=$(printf '\033[33m'); _O=$(printf '\033[0m')
else _G=; _R=; _Y=; _O=; fi
ok()   { printf '  %s[OK]%s   %s\n' "$_G" "$_O" "$1"; }
bad()  { printf '  %s[FAIL]%s %s\n' "$_R" "$_O" "$1"; RC=1; }
warn() { printf '  %s[WARN]%s %s\n' "$_Y" "$_O" "$1"; }

# The stamp is the whole build input: the upstream pin plus every overlay source
# and patch. Any edit to a module, a patch, or the ref changes it, forcing a
# rebuild; nothing else does. Order is stabilised by the shell glob (sorted).
_want() {
  { cat "$_root/refs"; cat "$_root"/overlays/* "$_root"/patches/*; } \
    | cksum | awk '{print $1"-"$2}'
}

_build() {   # <prefix>
  WBM_ROOT="$_root" WBM_CACHE="$CACHE" \
    sh "$_root/libexec/build-waybar" "$1"
}

_link_man() {   # cheap, idempotent; runs regardless of the build gate
  _man=${XDG_DATA_HOME:-$PREFIX/share}/man
  for _m in "$_root"/man/man*/*.[0-9]; do
    [ -e "$_m" ] || continue
    _d=$_man/$(basename "$(dirname "$_m")")
    mkdir -p "$_d"; ln -sfn "$_m" "$_d/$(basename "$_m")"
  done
}

do_install() {
  _link_man
  _w=$(_want)
  if [ -x "$WAYBAR" ] && [ -r "$STAMP" ] \
     && [ "$(cat "$STAMP" 2>/dev/null)" = "$_w" ]; then
    echo "$PKG: waybar in sync (nothing to build)"; return 0
  fi
  _build "$PREFIX"
  mkdir -p "$CACHE"; printf '%s\n' "$_w" > "$STAMP"
}

do_verify() {
  . "$_root/refs"
  _tmp=$(mktemp -d)
  trap 'rm -rf "$_tmp"' EXIT INT TERM
  echo "$PKG: verify build into $_tmp (throwaway) ..."
  _build "$_tmp"
  [ -x "$_tmp/bin/waybar" ] || {
    echo "$PKG: verify FAILED (no waybar produced)" >&2; exit 1; }
  echo "$PKG: verify OK (overlays + patches build against $WAYBAR_REF)"
}

do_uninstall() {
  _bdir=$CACHE/_build
  if [ -d "$_bdir" ]; then
    ninja -C "$_bdir" uninstall >/dev/null 2>&1 || true
  fi
  rm -f "$WAYBAR" "$STAMP"
  echo "$PKG: removed installed waybar + stamp"
}

do_check() {
  . "$_root/refs"
  echo "== $PKG (waybar from source) =="
  if [ -x "$WAYBAR" ]; then
    ok "waybar installed ($WAYBAR)"
  else
    bad "waybar not installed ($WAYBAR; run setup.sh install)"
    return
  fi
  if readelf -d "$WAYBAR" 2>/dev/null | grep -E 'RUNPATH|RPATH' \
       | grep -qF '$ORIGIN/../lib'; then
    ok "self-contained by rpath (\$ORIGIN/../lib)"
  else
    bad "waybar has no relative rpath (rebuild: setup.sh install)"
  fi
  if [ -r "$STAMP" ] && [ "$(cat "$STAMP" 2>/dev/null)" = "$(_want)" ]; then
    ok "in sync with overlays/patches/upstream pin ($WAYBAR_REF)"
  else
    bad "installed waybar STALE vs source (run setup.sh install)"
  fi
}

_U="usage: setup.sh [install|verify|check|uninstall|test|version]"
case "${1:-install}" in
  install)   do_install ;;
  verify)    do_verify ;;
  uninstall) do_uninstall ;;
  check)     do_check; exit "$RC" ;;
  test)      exec sh "$_root/test/run" ;;
  version)   . "$_root/refs"; echo "$PKG $VERSION (Waybar $WAYBAR_REF)" ;;
  -h|--help|help) echo "$_U" ;;
  *) echo "setup.sh: unknown command '${1:-}'" >&2; echo "$_U" >&2; exit 2 ;;
esac
