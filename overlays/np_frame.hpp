#pragma once

// np_frame.hpp - the C++ half of the now-playing shared-memory contract.
//
// MIRRORS libexec/npframe.py in the now-playing package; the authority is that
// package's docs/contract.md. These two files are INDEPENDENT implementations
// of ONE layout, so every constant below must match its counterpart exactly. A
// change on either side alone is a silent misparse, which is precisely what
// the layout version exists to turn into a loud failure instead.
//
// A consumer is a VIEW. It reads this frame and renders it. It does not derive
// playback facts, does not consult a second source of truth, and does not
// extrapolate position: the daemon publishes often enough that there is
// nothing to guess. See "What a consumer may compute" in the contract.

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace waybar::modules::media::np {

inline constexpr std::uint32_t kMagic = 0x5246504EU;   // "NPFR", little-endian
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::size_t kFrameBytes = 4096;
inline constexpr int kMaxBands = 64;

// header
inline constexpr std::size_t kOMagic = 0x0000;
inline constexpr std::size_t kOVersion = 0x0004;
inline constexpr std::size_t kOSeq = 0x000C;
// payload (everything after the counter)
inline constexpr std::size_t kOHeartbeat = 0x0010;
inline constexpr std::size_t kOFlags = 0x001C;
inline constexpr std::size_t kOStatus = 0x0020;
inline constexpr std::size_t kOSource = 0x0024;
inline constexpr std::size_t kOPosition = 0x0028;
inline constexpr std::size_t kOLength = 0x0030;
inline constexpr std::size_t kORate = 0x0038;
inline constexpr std::size_t kOCaps = 0x0040;
inline constexpr std::size_t kOTrackId = 0x0044;
inline constexpr std::size_t kOTitle = 0x0080;
inline constexpr std::size_t kOArtist = 0x0180;
inline constexpr std::size_t kOAlbum = 0x0280;
inline constexpr std::size_t kODevice = 0x0380;
inline constexpr std::size_t kOArt = 0x0400;
inline constexpr std::size_t kOBandCount = 0x0600;
inline constexpr std::size_t kOBands = 0x0608;
inline constexpr std::size_t kOPayload = kOHeartbeat;
inline constexpr std::size_t kOPayloadEnd = 0x0708;

inline constexpr std::size_t kNTitle = 256;
inline constexpr std::size_t kNArtist = 256;
inline constexpr std::size_t kNAlbum = 256;
inline constexpr std::size_t kNDevice = 128;
inline constexpr std::size_t kNArt = 512;

inline constexpr std::uint32_t kStatusIdle = 0;
inline constexpr std::uint32_t kStatusPlaying = 1;
inline constexpr std::uint32_t kStatusPaused = 2;
inline constexpr std::uint32_t kSourceNone = 0;
inline constexpr std::uint32_t kSourceLocal = 1;
inline constexpr std::uint32_t kSourceCast = 2;

inline constexpr std::uint32_t kCapPause = 1;
inline constexpr std::uint32_t kCapNext = 2;
inline constexpr std::uint32_t kCapPrev = 4;
inline constexpr std::uint32_t kCapSeek = 8;
inline constexpr std::uint32_t kFlagSpectrum = 1;

// Several publish intervals, so an ordinary scheduling hiccup on either side
// never reads as the daemon having died.
inline constexpr double kStaleSecs = 2.0;

inline std::uint64_t mono_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

struct Frame {
  std::uint32_t seq = 0;
  std::uint64_t heartbeat_ns = 0;
  std::uint32_t flags = 0;
  std::uint32_t status = kStatusIdle;
  std::uint32_t source = kSourceNone;
  double position = 0.0, length = 0.0, rate = 1.0;
  std::uint32_t caps = 0, track_id = 0;
  std::string title, artist, album, device, art;
  std::vector<float> bands;

  bool playing() const { return status == kStatusPlaying; }
  bool idle() const { return status == kStatusIdle; }
  bool casting() const { return source == kSourceCast; }
  bool can(std::uint32_t bit) const { return (caps & bit) != 0; }
};

// Why these are distinct: only SOME of them mean "show nothing". A contended
// read is transient and the caller must KEEP its last frame rather than blink
// the card to idle; an absent or stale frame genuinely means no daemon.
enum class Read {
  kOk,          // coherent and fresh
  kStale,       // coherent, but the daemon stopped publishing
  kContended,   // a write was in flight; try again next tick
  kAbsent,      // nothing mapped (no daemon has ever run here)
  kBadVersion,  // a layout this build must not guess at
};

class Reader {
 public:
  ~Reader() { unmap(); }

  // FIXED by the contract, so finding the segment needs no discovery; the
  // package's `shm-info` exists to negotiate the VERSION, not the location.
  static std::string path() {
    const char* r = std::getenv("XDG_RUNTIME_DIR");
    return std::string(r && *r ? r : "/tmp") + "/now-playing.frame";
  }

  std::uint32_t bad_version() const { return bad_version_; }

  Read read(Frame& out) {
    if (base_ == nullptr && !map()) return Read::kAbsent;
    const auto* seqp = reinterpret_cast<const std::uint32_t*>(base_ + kOSeq);
    unsigned char buf[kOPayloadEnd - kOPayload];
    // A few attempts, with a pause hint between them. The writer holds the
    // counter odd for microseconds at a time, and these retries are cheap
    // enough to land inside one such window, so spinning harder does not help;
    // failing back to the caller's last frame is the correct answer.
    for (int attempt = 0; attempt < 16; ++attempt) {
      const std::uint32_t s1 = __atomic_load_n(seqp, __ATOMIC_ACQUIRE);
      if ((s1 & 1U) != 0U) {
        cpu_relax();
        continue;
      }
      std::memcpy(buf, base_ + kOPayload, sizeof(buf));
      std::uint32_t magic = 0, ver = 0;
      std::memcpy(&magic, base_ + kOMagic, 4);
      std::memcpy(&ver, base_ + kOVersion, 4);
      if (__atomic_load_n(seqp, __ATOMIC_ACQUIRE) != s1) {
        cpu_relax();
        continue;
      }
      if (magic != kMagic) return Read::kAbsent;
      // Validated INSIDE the accepted read, not only at map time: a restarted
      // daemon may have published a different layout into this same page.
      if (ver != kVersion) {
        bad_version_ = ver;
        return Read::kBadVersion;
      }
      decode(buf, s1, out);
      const std::uint64_t now = mono_ns();
      const double age =
          now > out.heartbeat_ns
              ? static_cast<double>(now - out.heartbeat_ns) / 1e9
              : 0.0;
      return age <= kStaleSecs ? Read::kOk : Read::kStale;
    }
    return Read::kContended;
  }

 private:
  static void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    // A compiler barrier is enough elsewhere; this loop is never hot.
    __asm__ __volatile__("" ::: "memory");
#endif
  }

  bool map() {
    // The daemon never unlinks the segment, so a live mapping survives a
    // restart. Re-open only when we have none, and not on every tick: a
    // consumer may legitimately start before the daemon.
    const std::uint64_t now = mono_ns();
    if (last_try_ns_ != 0 && now - last_try_ns_ < 1000000000ULL) return false;
    last_try_ns_ = now;
    const std::string p = path();
    int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    if (::fstat(fd, &st) != 0 ||
        static_cast<std::size_t>(st.st_size) < kFrameBytes) {
      ::close(fd);
      return false;
    }
    void* m = ::mmap(nullptr, kFrameBytes, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED) return false;
    base_ = static_cast<const unsigned char*>(m);
    return true;
  }

  void unmap() {
    if (base_ != nullptr) {
      ::munmap(const_cast<unsigned char*>(base_), kFrameBytes);
      base_ = nullptr;
    }
  }

  static std::string str_at(const unsigned char* buf, std::size_t off,
                            std::size_t n) {
    const char* p = reinterpret_cast<const char*>(buf + off - kOPayload);
    const void* z = std::memchr(p, '\0', n);
    return std::string(p, z != nullptr
                              ? static_cast<std::size_t>(
                                    static_cast<const char*>(z) - p)
                              : n);
  }

  template <typename T>
  static T at(const unsigned char* buf, std::size_t off) {
    T v;
    std::memcpy(&v, buf + off - kOPayload, sizeof(T));
    return v;
  }

  static void decode(const unsigned char* buf, std::uint32_t seq, Frame& f) {
    f.seq = seq;
    f.heartbeat_ns = at<std::uint64_t>(buf, kOHeartbeat);
    f.flags = at<std::uint32_t>(buf, kOFlags);
    f.status = at<std::uint32_t>(buf, kOStatus);
    f.source = at<std::uint32_t>(buf, kOSource);
    f.position = at<double>(buf, kOPosition);
    f.length = at<double>(buf, kOLength);
    f.rate = at<double>(buf, kORate);
    f.caps = at<std::uint32_t>(buf, kOCaps);
    f.track_id = at<std::uint32_t>(buf, kOTrackId);
    f.title = str_at(buf, kOTitle, kNTitle);
    f.artist = str_at(buf, kOArtist, kNArtist);
    f.album = str_at(buf, kOAlbum, kNAlbum);
    f.device = str_at(buf, kODevice, kNDevice);
    f.art = str_at(buf, kOArt, kNArt);
    std::uint32_t nb = at<std::uint32_t>(buf, kOBandCount);
    if (nb > static_cast<std::uint32_t>(kMaxBands)) nb = kMaxBands;
    f.bands.resize(nb);
    if (nb != 0) {
      std::memcpy(f.bands.data(), buf + kOBands - kOPayload,
                  nb * sizeof(float));
    }
  }

  const unsigned char* base_ = nullptr;
  std::uint64_t last_try_ns_ = 0;
  std::uint32_t bad_version_ = 0;
};

}  // namespace waybar::modules::media::np
