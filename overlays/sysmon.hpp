#pragma once

#include <gtkmm/drawingarea.h>
#include <cairomm/context.h>
#include <cairomm/surface.h>
#include <json/json.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "AModule.hpp"
#include "bar.hpp"

namespace waybar::modules::sysmon {

// A single system-metric segment rendered as a Cairo line graph. One class
// backs every segment: the bar instances it per metric via the factory's
// name#id split (sysmon/graph#cpu, sysmon/graph#net, ...), each instance
// carrying its own config block (metric, label, scale) and CSS id. It reads
// /proc on a GLib timer in-process -- no forks, no state files -- and draws
// into one fixed-size DrawingArea: an inset "well" bevel, the current
// rolling-window trace (bright, filled), a flat lifetime-average line (darker,
// same hue, no fill), and the legend/value as text overlaid in the top-left
// corner. Colour is read from the widget's CSS `color`, so per-metric styling
// stays in style.css.
class Graph final : public waybar::AModule {
 public:
  Graph(const std::string& id, const waybar::Bar& bar,
        const Json::Value& config);
  ~Graph();

  auto update() -> void override;

 private:
  const waybar::Bar& bar_;

  // config
  std::string metric_ = "cpu";
  std::string label_fmt_ = "{value}";
  double max_scale_ = 0.0;      // >0: fixed full-scale; 0: percent or auto
  int samples_ = 60;            // rolling-window length (polyline resolution)
  int graph_width_ = 150;       // fixed widget width in px
  bool show_avg_ = true;        // draw the flat lifetime-average line
  bool fill_ = true;            // translucent fill under the "now" line
  bool show_label_ = true;      // overlay the legend/value text
  double font_px_ = 15.0;       // overlay text size in px
  double trend_gray_ = 0.55;    // avg-line grey level (0..1); neutral by design
  int vmargin_ = 4;             // bar padding above the well (bottom gets +)
  int hmargin_ = 1;             // bar padding left/right of the well, px
  int fps_ = 10;                // scroll redraw cadence (frames/sec)
  int interval_ms_ = 2000;

  // the whole segment is one drawing area; text is painted onto it, so the
  // widget width never changes with the value (no bar wiggle).
  Gtk::DrawingArea area_;

  // current rolling window, in RAW units (percent, bytes/s, or load). Mapped to
  // the drawing area against a scale computed at draw time, so an auto-scaled
  // metric re-fits as its window shifts.
  std::deque<double> hist_;

  // lifetime average: a running mean of the RAW value, normalised at draw time
  // against the same scale as the trace. Ephemeral: lives for this widget's
  // session, settling as samples accumulate.
  double life_mean_ = 0.0;      // running mean of raw values
  uint64_t life_n_ = 0;         // samples folded into life_mean_

  // delta bookkeeping (rate metrics)
  using Clock = std::chrono::steady_clock;
  bool have_prev_ = false;
  uint64_t prev_cpu_idle_ = 0, prev_cpu_total_ = 0;
  uint64_t prev_net_ = 0, prev_disk_ = 0;
  Clock::time_point prev_net_t_, prev_disk_t_;

  // net-interface cache: enumerating /sys/class/net every tick is wasteful,
  // and interfaces change rarely, so refresh it on a slow TTL.
  std::vector<std::string> net_ifaces_;
  Clock::time_point net_ifaces_ts_{};

  std::string label_text_;      // last formatted "legend value" for the overlay

  // smooth-scroll animation: data is sampled on the interval timer, but the
  // graph is redrawn at fps_ on a second timer and slid left by the fraction of
  // the interval elapsed since the last sample, so motion is continuous, not
  // ticky. (A plain timer, not the frame clock: it would run at the
  // full display refresh, whatever the throttle -- fps_ here really
  // is the wakeup rate.)
  Clock::time_point last_sample_ts_;

  // cached static layers, so a per-frame redraw is two blits + the scrolling
  // trace, not a full re-render (text-outline stroking is the pricey part). The
  // trace is sandwiched between them: below is under it, above is over it.
  Cairo::RefPtr<Cairo::Surface> below_;  // well background + 3D bevel
  Cairo::RefPtr<Cairo::Surface> above_;  // trend line + legend text
  int cache_w_ = 0, cache_h_ = 0;        // size the caches were built at
  bool above_dirty_ = true;              // trend/text changed (new sample)

  sigc::connection timer_;       // data sampling, at interval_ms_
  sigc::connection anim_timer_;  // scroll redraw, at fps_

  bool on_timer();
  bool on_anim();
  bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr);
  void render_below(const Cairo::RefPtr<Cairo::Context>& cr, double w,
                    double h);
  void render_above(const Cairo::RefPtr<Cairo::Context>& cr, double w,
                    double h);

  double collect();             // raw metric value (percent, bytes/s, or load)
  double scale_denom();         // draw-time full-scale (percent/fixed/windowed)
  std::string format_value(double v);

  // /proc + /sys readers
  double read_cpu();
  double read_mem_field(const char* total_key, const char* free_or_avail_key,
                        bool avail);
  double read_load();
  double read_net(bool rx);
  double read_disk(bool read);
};

}  // namespace waybar::modules::sysmon
