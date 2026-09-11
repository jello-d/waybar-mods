#pragma once

// Real-audio spectrum source for the now-playing card: capture the default
// sink's MONITOR (the exact mix you hear locally), FFT it, and publish a small
// array of log-spaced band magnitudes (0..1) for a Winamp-style analyzer.
//
// LOCAL ONLY, by design: a Chromecast decodes on the cast device, so there is
// no local PCM to analyze -- during a cast the monitor is silent here and the
// bands fall to zero (the card renders as before). This never fakes a signal.
//
// Capture uses the simple blocking record API (pa_simple) on a worker thread,
// so there is no async pulse lifecycle to wedge the bar. The default-sink
// monitor NAME + change notifications come from waybar's util::AudioBackend
// (already used by hw/gauge); we reopen the stream when the sink switches.

#include <pulse/error.h>
#include <pulse/simple.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "util/audio_backend.hpp"

namespace waybar::modules::media {

class Spectrum {
 public:
  struct Config {
    int bands = 24;          // number of log-spaced frequency bands (columns)
    int fft = 4096;          // FFT size (power of two). Big enough that the low
                             // bands land on DISTINCT bins (1024 was ~43Hz/bin,
                             // so bass bands collapsed onto one bin, pinned);
                             // 4096 is ~10.7Hz/bin.
    double rate = 44100.0;   // capture sample rate (mono)
    double fmin = 45.0;      // lowest band edge (Hz)
    double fmax = 16000.0;   // highest band edge (Hz)
    double gain = 2.0;       // lifts the normalized magnitude into view
    double floor_db = -60.0; // dB mapped to 0 (below this = nothing)
    double tilt = 3.5;       // dB/octave lift toward high freqs, to offset
                             // music's bass-heavy energy (else lows pin, highs
                             // vanish). Pivot = geometric mean of fmin/fmax.
    double attack = 0.65;    // 0..1 rise smoothing per FFT hop (fast)
    double decay = 0.16;     // 0..1 fall smoothing per FFT hop (slower)
    double active_rms = 4e-4;// RMS above this => local audio is playing
  };

  explicit Spectrum(const Config& c) : cfg_(c) {
    bands_.assign(cfg_.bands, 0.0f);
    win_.resize(cfg_.fft);
    for (int i = 0; i < cfg_.fft; ++i)
      win_[i] = 0.5f * (1.0f - std::cos(2.0 * M_PI * i / (cfg_.fft - 1)));
    re_.resize(cfg_.fft);
    im_.resize(cfg_.fft);
    acc_.reserve(cfg_.fft * 2);
    buildBands();
    audio_ = util::AudioBackend::getInstance([] {});
    run_ = true;
    worker_ = std::thread([this] { captureLoop(); });
  }

  ~Spectrum() {
    run_ = false;
    if (worker_.joinable()) worker_.join();
  }

  // GUI-thread read: copies the current smoothed bands (0..1) into dst and
  // returns whether local audio is currently active.
  bool read(std::vector<float>& dst) {
    std::lock_guard<std::mutex> lk(mtx_);
    dst = bands_;
    return active_.load();
  }
  int bands() const { return cfg_.bands; }

 private:
  Config cfg_;
  std::shared_ptr<util::AudioBackend> audio_;
  std::atomic<bool> run_{false};
  std::atomic<bool> active_{false};
  std::thread worker_;

  std::mutex mtx_;
  std::vector<float> bands_;   // published band levels (0..1)

  // worker-thread scratch
  std::vector<float> win_, re_, im_, acc_;
  std::vector<int> bin_lo_, bin_hi_;
  std::vector<double> band_tilt_;   // per-band dB offset (frequency tilt)

  void buildBands() {
    bin_lo_.resize(cfg_.bands);
    bin_hi_.resize(cfg_.bands);
    band_tilt_.resize(cfg_.bands);
    const double nyq = cfg_.rate / 2.0;
    const int half = cfg_.fft / 2;
    const double pivot = std::sqrt(cfg_.fmin * cfg_.fmax);   // tilt centre
    for (int b = 0; b < cfg_.bands; ++b) {
      const double t0 = static_cast<double>(b) / cfg_.bands;
      const double t1 = static_cast<double>(b + 1) / cfg_.bands;
      const double f0 = cfg_.fmin * std::pow(cfg_.fmax / cfg_.fmin, t0);
      const double f1 = cfg_.fmin * std::pow(cfg_.fmax / cfg_.fmin, t1);
      int lo = static_cast<int>(std::floor(f0 / nyq * half));
      int hi = static_cast<int>(std::ceil(f1 / nyq * half));
      lo = std::max(1, std::min(lo, half - 1));
      hi = std::max(lo + 1, std::min(hi, half));
      bin_lo_[b] = lo;
      bin_hi_[b] = hi;
      // +tilt dB per octave away from the pivot: lifts highs, trims lows.
      const double fc = std::sqrt(f0 * f1);
      band_tilt_[b] = cfg_.tilt * std::log2(fc / pivot);
    }
  }

  // In-place iterative radix-2 Cooley-Tukey FFT (re/im, size = power of two).
  static void fft(std::vector<float>& re, std::vector<float>& im) {
    const int n = static_cast<int>(re.size());
    for (int i = 1, j = 0; i < n; ++i) {
      int bit = n >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) {
        std::swap(re[i], re[j]);
        std::swap(im[i], im[j]);
      }
    }
    for (int len = 2; len <= n; len <<= 1) {
      const double ang = -2.0 * M_PI / len;
      const float wr = std::cos(ang), wi = std::sin(ang);
      for (int i = 0; i < n; i += len) {
        float cr = 1.0f, ci = 0.0f;
        for (int k = 0; k < len / 2; ++k) {
          const float xr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
          const float xi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
          re[i + k + len / 2] = re[i + k] - xr;
          im[i + k + len / 2] = im[i + k] - xi;
          re[i + k] += xr;
          im[i + k] += xi;
          const float ncr = cr * wr - ci * wi;
          ci = cr * wi + ci * wr;
          cr = ncr;
        }
      }
    }
  }

  // Consume samples, transform in 50%-overlapping FFT hops, publish bands.
  void process(const float* s, size_t n) {
    const int N = cfg_.fft;
    for (size_t i = 0; i < n; ++i) acc_.push_back(s[i]);
    while (static_cast<int>(acc_.size()) >= N) {
      double sum = 0.0;
      for (int k = 0; k < N; ++k) {
        re_[k] = acc_[k] * win_[k];
        im_[k] = 0.0f;
        sum += static_cast<double>(acc_[k]) * acc_[k];
      }
      const double rms = std::sqrt(sum / N);
      const bool act = rms > cfg_.active_rms;
      fft(re_, im_);
      const double norm = N / 2.0;
      const double span = 0.0 - cfg_.floor_db;
      std::lock_guard<std::mutex> lk(mtx_);
      for (int b = 0; b < cfg_.bands; ++b) {
        double m = 0.0;
        int cnt = 0;
        for (int k = bin_lo_[b]; k < bin_hi_[b]; ++k) {
          m += std::sqrt(static_cast<double>(re_[k]) * re_[k] +
                         static_cast<double>(im_[k]) * im_[k]);
          ++cnt;
        }
        m = (cnt ? m / cnt : 0.0) / norm;
        const double db = 20.0 * std::log10(m + 1e-9) + band_tilt_[b];
        double v = (db - cfg_.floor_db) / span;   // 0..1 across the dB window
        v = std::min(1.0, std::max(0.0, v) * cfg_.gain);
        const float tgt = static_cast<float>(v);
        const float cur = bands_[b];
        const float c = tgt > cur ? cfg_.attack : cfg_.decay;
        bands_[b] = cur + (tgt - cur) * c;
      }
      active_.store(act);
      acc_.erase(acc_.begin(), acc_.begin() + N / 2);   // 50% overlap
    }
  }

  void captureLoop() {
    std::vector<float> buf(cfg_.fft / 2);
    const pa_sample_spec ss{PA_SAMPLE_FLOAT32NE,
                            static_cast<uint32_t>(cfg_.rate), 1};
    while (run_.load()) {
      std::string src = audio_ ? audio_->getMonitor() : std::string();
      if (src.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        continue;
      }
      int err = 0;
      pa_buffer_attr attr;
      attr.maxlength = static_cast<uint32_t>(-1);
      attr.fragsize = static_cast<uint32_t>(buf.size() * sizeof(float));
      attr.tlength = attr.prebuf = attr.minreq = static_cast<uint32_t>(-1);
      pa_simple* rec =
          pa_simple_new(nullptr, "waybar-spectrum", PA_STREAM_RECORD,
                        src.c_str(), "now-playing viz", &ss, nullptr, &attr,
                        &err);
      if (!rec) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        continue;
      }
      // Record until the sink switches (monitor name changes) or a read fails.
      while (run_.load() && audio_ && audio_->getMonitor() == src) {
        if (pa_simple_read(rec, buf.data(), buf.size() * sizeof(float), &err) <
            0)
          break;
        process(buf.data(), buf.size());
      }
      pa_simple_free(rec);
      // Fade to silence between streams so stale bars do not linger.
      {
        std::lock_guard<std::mutex> lk(mtx_);
        std::fill(bands_.begin(), bands_.end(), 0.0f);
        active_.store(false);
      }
    }
  }
};

}  // namespace waybar::modules::media
