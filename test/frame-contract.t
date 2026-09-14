#!/bin/sh
# frame-contract.t - overlays/np_frame.hpp against the now-playing shared-memory
# contract. Two layers, because they catch different bugs:
#
#   1. STATIC: the header's constants are compared, at COMPILE time, against the
#      literal offset table from docs/contract.md restated here. A third copy of
#      the layout on purpose -- if the header drifts, this fails to build.
#   2. CROSS-LANGUAGE: the now-playing package's Python WRITER lays down a known
#      frame and this C++ READER decodes it. That is the bug neither repo can
#      find alone: two independent implementations of one layout silently
#      disagreeing about an offset. Skipped, with a note, when the counterpart
#      package is not on this box.
. "$(dirname "$0")/lib.sh"
harness_init frame-contract

command -v c++ >/dev/null 2>&1 || skip "no c++ compiler"

cat > "$T/t.cpp" <<'EOF'
#include "np_frame.hpp"

#include <cstdio>

namespace np = waybar::modules::media::np;

// The literal table from the contract. Keep these as NUMBERS, never as
// references to the header's own constants, or this asserts nothing.
static_assert(np::kMagic == 0x5246504EU, "magic");
static_assert(np::kVersion == 1, "version");
static_assert(np::kFrameBytes == 4096, "frame_bytes");
static_assert(np::kMaxBands == 64, "max_bands");
static_assert(np::kOSeq == 0x000C, "seq");
static_assert(np::kOHeartbeat == 0x0010, "heartbeat");
static_assert(np::kOFlags == 0x001C, "flags");
static_assert(np::kOStatus == 0x0020, "status");
static_assert(np::kOSource == 0x0024, "source");
static_assert(np::kOPosition == 0x0028, "position");
static_assert(np::kOLength == 0x0030, "length");
static_assert(np::kORate == 0x0038, "rate");
static_assert(np::kOCaps == 0x0040, "caps");
static_assert(np::kOTrackId == 0x0044, "track_id");
static_assert(np::kOTitle == 0x0080, "title");
static_assert(np::kOArtist == 0x0180, "artist");
static_assert(np::kOAlbum == 0x0280, "album");
static_assert(np::kODevice == 0x0380, "device");
static_assert(np::kOArt == 0x0400, "art");
static_assert(np::kOBandCount == 0x0600, "band_count");
static_assert(np::kOBands == 0x0608, "bands");
static_assert(np::kOPayloadEnd == 0x0708, "payload_end");
static_assert(np::kNTitle == 256 && np::kNArtist == 256, "string slots");
static_assert(np::kNAlbum == 256 && np::kNDevice == 128, "string slots");
static_assert(np::kNArt == 512, "art slot");
static_assert(np::kCapPause == 1 && np::kCapNext == 2, "caps bits");
static_assert(np::kCapPrev == 4 && np::kCapSeek == 8, "caps bits");
// Every field must sit inside the page, and the payload must start right
// after the counter, or the single-memcpy publish would not be coherent.
static_assert(np::kOPayload == np::kOHeartbeat, "payload follows seq");
static_assert(np::kOPayloadEnd <= np::kFrameBytes, "payload fits the page");

int main() {
  np::Reader r;
  np::Frame f;
  const np::Read rc = r.read(f);
  const char* n = rc == np::Read::kOk           ? "ok"
                  : rc == np::Read::kStale      ? "stale"
                  : rc == np::Read::kContended  ? "contended"
                  : rc == np::Read::kBadVersion ? "badversion"
                                                : "absent";
  std::printf("read=%s\n", n);
  if (rc != np::Read::kOk && rc != np::Read::kStale) return 1;
  std::printf("status=%u\nsource=%u\n", f.status, f.source);
  std::printf("position=%.6f\nlength=%.6f\nrate=%.6f\n", f.position, f.length,
              f.rate);
  std::printf("caps=%u\ntrack=%u\n", f.caps, f.track_id);
  std::printf("title=%s\nartist=%s\nalbum=%s\ndevice=%s\nart=%s\n",
              f.title.c_str(), f.artist.c_str(), f.album.c_str(),
              f.device.c_str(), f.art.c_str());
  std::printf("nbands=%zu\n", f.bands.size());
  if (!f.bands.empty()) std::printf("band0=%.3f\n", f.bands[0]);
  std::printf("playing=%d\ncasting=%d\ncan_next=%d\ncan_prev=%d\n",
              f.playing() ? 1 : 0, f.casting() ? 1 : 0,
              f.can(np::kCapNext) ? 1 : 0, f.can(np::kCapPrev) ? 1 : 0);
  return 0;
}
EOF

c++ -std=c++17 -Wall -Wextra -Werror -I "$HERE/overlays" -o "$T/t" \
  "$T/t.cpp" 2>"$T/cc.err" || {
  sed -n '1,25p' "$T/cc.err" >&2
  fail "np_frame.hpp does not match the contract's offsets (or will not build)"
}

# With no segment at all the reader must say so, not crash or invent a frame.
out=$(XDG_RUNTIME_DIR="$T/empty" "$T/t" 2>&1) \
  && fail "read passed with no frame"
echo "$out" | grep -q '^read=absent' || fail "absent frame misreported: $out"

# --- cross-language roundtrip ---------------------------------------------
NP=
for _c in "$HOME/src/now-playing" "$HOME/.cache/tackup/pkgs/now-playing"; do
  [ -f "$_c/libexec/npframe.py" ] && NP=$_c && break
done
[ -n "$NP" ] || pass "offsets only; now-playing package not on this box"
[ -n "$NP" ] || exit 0
command -v python3 >/dev/null 2>&1 || pass "offsets only; python3 absent"
command -v python3 >/dev/null 2>&1 || exit 0

XDG_RUNTIME_DIR=$T python3 - "$NP" <<'EOF' || fail "writer failed"
import sys
sys.path.insert(0, sys.argv[1] + "/libexec")
import npframe as F
F.Writer().publish(status=F.STATUS_PAUSED, source=F.SOURCE_CAST,
                   position=12.5, length=321.25, rate=1.5,
                   caps=F.CAP_PAUSE | F.CAP_PREV, track_id=77,
                   title="Parlor Strut", artist="Parlor Greens",
                   album="In Green We Dream", device="Living Room",
                   art="/tmp/cover.png", bands=[0.25, 0.5, 0.75])
EOF

out=$(XDG_RUNTIME_DIR=$T "$T/t") || fail "C++ could not read the frame: $out"
check() {   # <key=value> -- the value Python wrote must be what C++ decoded
  echo "$out" | grep -qx "$1" || fail "C++/Python disagree: want '$1' in: $out"
}
check 'read=ok'
check 'status=2'          # paused
check 'source=2'          # cast
check 'position=12.500000'
check 'length=321.250000'
check 'rate=1.500000'
check 'caps=5'            # pause|prev
check 'track=77'
check 'title=Parlor Strut'
check 'artist=Parlor Greens'
check 'album=In Green We Dream'
check 'device=Living Room'
check 'art=/tmp/cover.png'
check 'nbands=3'
check 'band0=0.250'
check 'playing=0'
check 'casting=1'
check 'can_next=0'        # the CLEAR bit is the one a renderer must honour
check 'can_prev=1'

pass "offsets + cross-language roundtrip"
