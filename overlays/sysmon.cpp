#include "modules/sysmon/graph.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <cctype>
#include <cstdio>
#include <filesystem>

#include <spdlog/spdlog.h>

#include "nvml.hpp"   // shared runtime-NVML glue (util/temp/VRAM)

namespace waybar::modules::sysmon {

namespace fs = std::filesystem;

static bool is_percent(const std::string& m) {
  return m == "cpu" || m == "mem" || m == "swap" || m == "gpu" ||
         m == "gpu_mem" || m == "gpu_temp";
}

Graph::Graph(const std::string& id, const waybar::Bar& bar,
             const Json::Value& config)
    : AModule(config, "sysmon", id, /*enable_click=*/true), bar_(bar) {
  if (config_["metric"].isString()) metric_ = config_["metric"].asString();
  if (config_["label"].isString()) label_fmt_ = config_["label"].asString();
  if (config_["max-scale"].isNumeric())
    max_scale_ = config_["max-scale"].asDouble();
  if (config_["samples"].isInt()) samples_ = config_["samples"].asInt();
  if (config_["graph-width"].isInt())
    graph_width_ = config_["graph-width"].asInt();
  if (config_["show-average"].isBool())
    show_avg_ = config_["show-average"].asBool();
  if (config_["fill"].isBool()) fill_ = config_["fill"].asBool();
  if (config_["show-label"].isBool())
    show_label_ = config_["show-label"].asBool();
  if (config_["font-size"].isNumeric())
    font_px_ = config_["font-size"].asDouble();
  if (config_["trend-gray"].isNumeric())
    trend_gray_ = config_["trend-gray"].asDouble();
  if (config_["vmargin"].isInt()) vmargin_ = config_["vmargin"].asInt();
  if (config_["hmargin"].isInt()) hmargin_ = config_["hmargin"].asInt();
  if (config_["fps"].isInt()) fps_ = config_["fps"].asInt();
  if (fps_ < 1) fps_ = 1;
  if (fps_ > 60) fps_ = 60;

  double iv = config_["interval"].isNumeric() ? config_["interval"].asDouble()
                                              : 2.0;
  interval_ms_ = static_cast<int>(iv * 1000.0);
  if (interval_ms_ < 50) interval_ms_ = 50;   // cap the floor
  if (samples_ < 2) samples_ = 2;
  if (graph_width_ < 2) graph_width_ = 2;
  if (font_px_ < 1.0) font_px_ = 1.0;

  // Scale the whole segment with the BAR height (48 = design). wb sets a taller
  // bar on the 3-monitor wall, so the graph + legend grow with it and need no
  // per-bar config. The base 48px bar is a strict no-op (ui == 1).
  const double ui = bar_.config["height"].isInt()
                        ? bar_.config["height"].asInt() / 48.0
                        : 1.0;
  // The graph WIDTH tracks the bar, ONE-SIDED: scaling UP (wall) it widens
  // faster (1.5x at the wall's 1.25 ui -- amplify the deviation, room to spare)
  // scaling DOWN (a compact low-res bar) it goes ~15% NARROWER than the height
  // ratio, so all EIGHT graphs fit at once WITH room to keep the clock centred.
  // A no-op at base (ui == 1). Font tracks height.
  const double wf = ui < 1.0 ? ui * 0.84 : 1.0 + (ui - 1.0) * 2.0;
  graph_width_ = static_cast<int>(graph_width_ * wf + 0.5);
  // The legend font tracks height, ONE-SIDED with an EXTRA shrink DOWN: at a
  // straight ratio the text hogs the short compact well, so take another ~20%
  // off below base. A no-op at base and above (the wall keeps the full ratio).
  font_px_ *= ui < 1.0 ? ui * 0.80 : ui;

  // CSS handle: the bar packs event_box_ verbatim without renaming it, so this
  // name is what style.css targets (#sysmon-cpu, ...). The graph colour is read
  // from this widget's style context, so a per-metric `color` in CSS colours
  // the trace, the average line, and the overlay text alike.
  event_box_.set_name("sysmon-" + (id.empty() ? metric_ : id));

  // One fixed-width drawing area IS the whole segment; the legend/value is
  // painted onto it, so the widget never resizes with the value. The bar is a
  // fixed-height layer-shell window and ignores CSS margins here, so
  // the breathing room around the well is set here as GTK widget margins (which
  // are honoured in allocation) -- the exposed margin shows the bar background.
  area_.set_size_request(graph_width_, -1);
  area_.set_hexpand(false);
  // The bar biases content downward a couple of px, so equal margins leave the
  // well sitting low; bias the bottom margin up to read centred (a hair high
  // by taste). The +3 is that compensation, not a second knob.
  area_.set_margin_top(vmargin_);
  area_.set_margin_bottom(vmargin_ + 3);
  area_.set_margin_start(hmargin_);
  area_.set_margin_end(hmargin_);
  area_.signal_draw().connect(sigc::mem_fun(*this, &Graph::on_draw));

  event_box_.add(area_);
  event_box_.show_all();

  // A gpu_* metric on a box with no Nvidia driver -> collapse, not draw a
  // flat dead graph (so the same config runs on a GPU-less laptop). no_show_all
  // keeps a parent show_all from resurrecting it; the timers below never start.
  if (metric_.rfind("gpu", 0) == 0 && !Nvml::get().ok()) {
    event_box_.set_no_show_all(true);
    event_box_.hide();
    return;
  }

  spdlog::debug("sysmon/graph#{}: metric={} interval={}ms scale={}",
                id, metric_, interval_ms_, max_scale_);

  update();  // prime (rate metrics read 0 until the second tick)
  timer_ = Glib::signal_timeout().connect(
      sigc::mem_fun(*this, &Graph::on_timer), interval_ms_);

  // Redraw at fps_ to scroll the trace. Skip while unmapped so a
  // hidden bar (fullscreen/occluded) costs only the timer wakeup, no rendering.
  anim_timer_ = Glib::signal_timeout().connect(
      sigc::mem_fun(*this, &Graph::on_anim), 1000 / fps_);
}

Graph::~Graph() {
  if (timer_.connected()) timer_.disconnect();
  if (anim_timer_.connected()) anim_timer_.disconnect();
}

bool Graph::on_timer() {
  update();
  return true;
}

bool Graph::on_anim() {
  if (area_.get_mapped()) area_.queue_draw();
  return true;  // keep ticking
}

// ---- collection -----------------------------------------------------------

double Graph::read_cpu() {
  std::ifstream f("/proc/stat");
  std::string tag;
  f >> tag;  // "cpu"
  uint64_t v[8] = {0};
  for (int i = 0; i < 8 && (f >> v[i]); i++) {
  }
  uint64_t idle = v[3] + v[4];  // idle + iowait
  uint64_t total = 0;
  for (int i = 0; i < 8; i++) total += v[i];

  double pct = 0.0;
  if (have_prev_ && total > prev_cpu_total_) {
    uint64_t di = idle - prev_cpu_idle_;
    uint64_t dt = total - prev_cpu_total_;
    pct = 100.0 * (1.0 - static_cast<double>(di) / static_cast<double>(dt));
  }
  prev_cpu_idle_ = idle;
  prev_cpu_total_ = total;
  return pct;
}

double Graph::read_mem_field(const char* total_key, const char* other_key,
                             bool avail) {
  uint64_t total = 0, other = 0;
  bool have_total = false, have_other = false;
  std::ifstream f("/proc/meminfo");
  std::string key;
  uint64_t val;
  std::string rest;
  while (f >> key >> val) {
    std::getline(f, rest);  // consume trailing " kB"
    if (key == total_key) { total = val; have_total = true; }
    else if (key == other_key) { other = val; have_other = true; }
    if (have_total && have_other) break;  // both found; skip rest
  }
  if (total == 0) return 0.0;
  // avail=true: other is MemAvailable/SwapFree (free space) -> used = 1 - free.
  return 100.0 *
         (1.0 - static_cast<double>(other) / static_cast<double>(total));
}

double Graph::read_load() {
  std::ifstream f("/proc/loadavg");
  double one = 0.0;
  f >> one;
  return one;
}

double Graph::read_net(bool rx) {
  auto now = Clock::now();
  // Refresh the interface list only on a slow TTL; reading the counters each
  // tick is cheap, re-enumerating the directory is what we avoid.
  if (net_ifaces_.empty() ||
      std::chrono::duration<double>(now - net_ifaces_ts_).count() >= 5.0) {
    net_ifaces_.clear();
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/sys/class/net", ec)) {
      const std::string name = e.path().filename().string();
      if (name == "lo") continue;
      net_ifaces_.push_back(name);
    }
    net_ifaces_ts_ = now;
  }

  uint64_t total = 0;
  for (const auto& name : net_ifaces_) {
    std::ifstream f("/sys/class/net/" + name + "/statistics/" +
                    (rx ? "rx_bytes" : "tx_bytes"));
    uint64_t b = 0;
    if (f >> b) total += b;
  }
  double rate = 0.0;
  if (have_prev_) {
    double dt = std::chrono::duration<double>(now - prev_net_t_).count();
    if (dt > 0) rate = static_cast<double>(total - prev_net_) / dt;
  }
  prev_net_ = total;
  prev_net_t_ = now;
  return rate;
}

double Graph::read_disk(bool read) {
  uint64_t sectors = 0;
  std::ifstream f("/proc/diskstats");
  std::string line;
  while (std::getline(f, line)) {
    // Parse only the fields we need, no per-line vector<string> allocation.
    // Layout: major minor name reads rd_merged sectors_read ms_read writes
    //         wr_merged sectors_written ...
    std::istringstream ss(line);
    long major, minor;
    std::string name;
    uint64_t rd, rd_m, s_read, ms_rd, wr, wr_m, s_write;
    if (!(ss >> major >> minor >> name >> rd >> rd_m >> s_read >> ms_rd >>
          wr >> wr_m >> s_write))
      continue;

    if (name.rfind("loop", 0) == 0 || name.rfind("dm-", 0) == 0 ||
        name.rfind("ram", 0) == 0)
      continue;
    // Base disks only, not partitions (nvme0n1p3, sda1).
    if (name.rfind("nvme", 0) == 0) {
      if (name.find('p', 4) != std::string::npos) continue;
    } else if (!name.empty() &&
               std::isdigit(static_cast<unsigned char>(name.back()))) {
      continue;
    }
    sectors += read ? s_read : s_write;
  }
  auto now = Clock::now();
  double rate = 0.0;
  if (have_prev_) {
    double dt = std::chrono::duration<double>(now - prev_disk_t_).count();
    if (dt > 0)
      rate = static_cast<double>(sectors - prev_disk_) * 512.0 / dt;
  }
  prev_disk_ = sectors;
  prev_disk_t_ = now;
  return rate;
}

double Graph::collect() {
  if (metric_ == "cpu") return read_cpu();
  if (metric_ == "mem")
    return read_mem_field("MemTotal:", "MemAvailable:", true);
  if (metric_ == "swap") return read_mem_field("SwapTotal:", "SwapFree:", true);
  if (metric_ == "load") return read_load();
  if (metric_ == "net_rx") return read_net(true);
  if (metric_ == "net_tx") return read_net(false);
  if (metric_ == "disk_read") return read_disk(true);
  if (metric_ == "disk_write") return read_disk(false);
  if (metric_ == "gpu") return Nvml::get().util();
  if (metric_ == "gpu_temp") return Nvml::get().temp();
  if (metric_ == "gpu_mem") return Nvml::get().mem_pct();
  return 0.0;
}

// Full-scale denominator used to map raw values into the well at draw time.
// Percent metrics are fixed at 100; a configured max-scale pins the top; every
// other (non-percent) metric auto-scales to the window -- the max of the
// visible samples and the lifetime average, plus headroom -- so it re-fits as
// spikes scroll off (a monotonic ceiling would flatten the graph forever after
// one burst).
double Graph::scale_denom() {
  if (is_percent(metric_)) return 100.0;
  if (max_scale_ > 0) return max_scale_;
  double s = life_mean_;
  for (double v : hist_) if (v > s) s = v;
  s *= 1.15;                       // headroom above the peak
  if (s < 1.0) s = 1.0;            // floor: avoids divide-by-zero when idle
  return s;
}

std::string Graph::format_value(double v) {
  char buf[32];
  if (is_percent(metric_)) {
    std::snprintf(buf, sizeof buf, "%3.0f", v);  // fixed width, no label jitter
    return buf;
  }
  if (metric_ == "load") {
    std::snprintf(buf, sizeof buf, "%.2f", v);
    return buf;
  }
  char num[16];
  if (v >= 1e9) std::snprintf(num, sizeof num, "%.1fG", v / 1e9);
  else if (v >= 1e6) std::snprintf(num, sizeof num, "%.1fM", v / 1e6);
  else if (v >= 1e3) std::snprintf(num, sizeof num, "%.0fK", v / 1e3);
  else std::snprintf(num, sizeof num, "%.0fB", v);
  std::snprintf(buf, sizeof buf, "%5s", num);
  return buf;
}

auto Graph::update() -> void {
  double raw = collect();

  // rolling window (the bright "now" trace), kept in raw units
  hist_.push_back(raw);
  if (static_cast<int>(hist_.size()) > samples_) hist_.pop_front();

  // lifetime average: running mean of the RAW value, settling as 1/n. Kept in
  // raw units so it re-normalises correctly if an auto-scaled metric rescales.
  life_n_++;
  life_mean_ += (raw - life_mean_) / static_cast<double>(life_n_);

  have_prev_ = true;
  last_sample_ts_ = Clock::now();  // anchor for the scroll interpolation

  if (show_label_) {
    label_text_ = label_fmt_;
    auto pos = label_text_.find("{value}");
    if (pos != std::string::npos)
      label_text_.replace(pos, 7, format_value(raw));
  }
  above_dirty_ = true;  // trend Y and legend text changed; rebuild that cache
  area_.queue_draw();
}

// ---- drawing --------------------------------------------------------------

bool Graph::on_draw(const Cairo::RefPtr<Cairo::Context>& cr) {
  const Gtk::Allocation a = area_.get_allocation();
  const double w = a.get_width();
  const double h = a.get_height();
  if (w <= 0 || h <= 0) return true;

  // (Re)build the cached static layers: `below_` (well background + bevel) is
  // size-invariant, `above_` (trend line + legend text) changes only on a new
  // sample. Both are matched to the target's device scale via create_similar so
  // they stay crisp on HiDPI. This keeps the per-frame path to two blits plus
  // the scrolling trace -- the pricey text-outline stroking runs once per
  // second, not once per frame.
  if (!below_ || cache_w_ != static_cast<int>(w) ||
      cache_h_ != static_cast<int>(h)) {
    below_ = Cairo::Surface::create(cr->get_target(),
                                    Cairo::CONTENT_COLOR_ALPHA,
                                    static_cast<int>(w), static_cast<int>(h));
    render_below(Cairo::Context::create(below_), w, h);
    cache_w_ = static_cast<int>(w);
    cache_h_ = static_cast<int>(h);
    above_.clear();  // size changed -> the above cache is stale too
  }
  if (!above_ || above_dirty_) {
    above_ = Cairo::Surface::create(cr->get_target(),
                                    Cairo::CONTENT_COLOR_ALPHA,
                                    static_cast<int>(w), static_cast<int>(h));
    render_above(Cairo::Context::create(above_), w, h);
    above_dirty_ = false;
  }

  // 1) cached background + bevel, under the trace
  cr->set_source(below_, 0, 0);
  cr->paint();

  // 2) the scrolling trace -- the only geometry redrawn every frame
  const double bevel = 4.0;
  const double ix0 = bevel + 1.0, iy0 = bevel + 1.0;
  const double iw = w - 2.0 * (bevel + 1.0);
  const double ih = h - 2.0 * (bevel + 1.0);
  const double pad = 1.5;  // keep 0% and 100% off the very edge
  const int n = static_cast<int>(hist_.size());
  const double scale = scale_denom();

  // Fraction of the sample interval elapsed since the last sample: the whole
  // trace slides left by this fraction of one slot, so a new sample enters at
  // the right edge and the motion is continuous instead of a per-tick hop.
  double elapsed_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - last_sample_ts_)
          .count();
  double t = interval_ms_ > 0 ? elapsed_ms / interval_ms_ : 1.0;
  if (t < 0.0) t = 0.0;
  if (t > 1.0) t = 1.0;
  const double slot = iw / static_cast<double>(samples_ > 1 ? samples_ - 1 : 1);

  cr->set_line_join(Cairo::LINE_JOIN_ROUND);
  cr->set_line_cap(Cairo::LINE_CAP_ROUND);

  auto NY = [&](double raw) {                    // raw -> [0,1], clamped
    double v = raw / scale;
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
  };

  if (n >= 2 && iw > 0 && ih > 0) {
    auto sc = event_box_.get_style_context();
    const Gdk::RGBA col = sc->get_color(sc->get_state());
    const double r = col.get_red(), g = col.get_green(), b = col.get_blue();

    // newest sample (i = n-1) anchored at the right edge, each older sample one
    // slot further left, the whole set slid left by the elapsed fraction t
    auto X = [&](int i) { return ix0 + iw - slot * ((n - 1 - i) + t); };
    auto Y = [&](double raw) {
      return (iy0 + ih - pad) - NY(raw) * (ih - 2.0 * pad);
    };

    // Clip everything graph-related to the plotting region, so no stroke (a
    // round line cap, a spike at full scale, a wide trend line) can bleed
    // onto the bevel. The legend text is drawn after this scope, unclipped.
    cr->save();
    cr->rectangle(ix0, iy0, iw, ih);
    cr->clip();

    // current rolling-window trace: translucent fill + bright line
    if (fill_) {
      cr->begin_new_path();
      cr->move_to(X(0), iy0 + ih);
      for (int i = 0; i < n; i++) cr->line_to(X(i), Y(hist_[i]));
      cr->line_to(X(n - 1), iy0 + ih);
      cr->close_path();
      cr->set_source_rgba(r, g, b, 0.16);
      cr->fill();
    }
    cr->begin_new_path();
    for (int i = 0; i < n; i++) {
      if (i == 0) cr->move_to(X(i), Y(hist_[i]));
      else cr->line_to(X(i), Y(hist_[i]));
    }
    cr->set_source_rgba(r, g, b, 1.0);
    cr->set_line_width(1.5);
    cr->stroke();
    cr->restore();  // drop the plot-region clip before compositing the overlay
  }

  // 3) cached trend line + legend text, composited over the trace
  cr->set_source(above_, 0, 0);
  cr->paint();

  return true;
}

// ---- cached static layers -------------------------------------------------

// well background + 3D bevel (shadow top-left, highlight bottom-right, tripled
// for depth): never changes, so it is rendered once per size into `below_`.
void Graph::render_below(const Cairo::RefPtr<Cairo::Context>& cr, double w,
                         double h) {
  cr->set_antialias(Cairo::ANTIALIAS_DEFAULT);
  cr->set_source_rgba(0.0, 0.0, 0.0, 0.42);
  cr->rectangle(0, 0, w, h);
  cr->fill();

  cr->set_line_width(1.0);
  for (int d = 0; d < 3; d++) {                 // three rings -> a deep bevel
    const double o = 0.5 + d;
    const double edge = 0.60 - 0.17 * d;        // outer darkest, fading inward
    const double lite = 0.26 - 0.07 * d;
    cr->set_source_rgba(0.0, 0.0, 0.0, edge);   // top + left: shadow
    cr->move_to(o, h - o);
    cr->line_to(o, o);
    cr->line_to(w - o, o);
    cr->stroke();
    cr->set_source_rgba(1.0, 1.0, 1.0, lite);   // bottom + right: highlight
    cr->move_to(w - o, o);
    cr->line_to(w - o, h - o);
    cr->line_to(o, h - o);
    cr->stroke();
  }
}

// trend reference line + legend text: change only on a new sample, so they are
// rendered into `above_` (transparent elsewhere) and blitted over the trace.
void Graph::render_above(const Cairo::RefPtr<Cairo::Context>& cr, double w,
                         double h) {
  const double bevel = 4.0;
  const double ix0 = bevel + 1.0, iy0 = bevel + 1.0;
  const double iw = w - 2.0 * (bevel + 1.0);
  const double ih = h - 2.0 * (bevel + 1.0);
  const double pad = 1.5;
  cr->set_antialias(Cairo::ANTIALIAS_DEFAULT);

  // lifetime average: a row of round grey dots at the session mean -- always
  // visible on the dark well, never competing with the metric's colour.
  if (show_avg_ && life_n_ > 0 && iw > 0 && ih > 0) {
    const double scale = scale_denom();
    double v = life_mean_ / scale;
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    const double ay = (iy0 + ih - pad) - v * (ih - 2.0 * pad);
    // near-zero "on" length + round cap => a dot (dia = line width) per dash.
    cr->set_line_cap(Cairo::LINE_CAP_ROUND);
    const std::vector<double> dash = {0.5, 5.5};
    cr->set_dash(dash, 0.0);
    cr->move_to(ix0, ay);
    cr->line_to(ix0 + iw, ay);
    cr->set_source_rgba(trend_gray_, trend_gray_, trend_gray_, 1.0);
    cr->set_line_width(3.0);
    cr->stroke();
    cr->unset_dash();
  }

  // legend/value, bottom-left: white glyphs with a black outline, so the text
  // keeps perfect contrast over any trace colour without a background panel.
  if (show_label_ && !label_text_.empty()) {
    cr->select_font_face("sans-serif", Cairo::FONT_SLANT_NORMAL,
                         Cairo::FONT_WEIGHT_BOLD);
    cr->set_font_size(font_px_);
    cr->move_to(bevel + 0.5, h - 2.0);  // baseline just above the bottom edge
    cr->text_path(label_text_);
    cr->set_line_join(Cairo::LINE_JOIN_ROUND);
    cr->set_line_width(4.5);
    cr->set_source_rgba(0.0, 0.0, 0.0, 0.9);
    cr->stroke_preserve();
    cr->set_source_rgba(1.0, 1.0, 1.0, 1.0);
    cr->fill();
  }
}

}  // namespace waybar::modules::sysmon
