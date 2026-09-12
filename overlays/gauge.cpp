#include "modules/hw/gauge.hpp"

#include <gtk-layer-shell.h>
#include <glibmm.h>

#include "flush_tooltip.hpp"
#include "nvml.hpp"   // GPU temp source for the temp dial ("source":"gpu")

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <vector>

#include <spdlog/spdlog.h>

namespace waybar::modules::hw {

namespace fs = std::filesystem;

static constexpr double kPi = 3.14159265358979323846;

static long read_long(const std::string& path) {
  std::ifstream f(path);
  long v = 0;
  f >> v;
  return v;
}

static std::string read_str(const std::string& path) {
  std::ifstream f(path);
  std::string s;
  std::getline(f, s);
  return s;
}

// Find a CPU package temperature sensor under /sys/class/hwmon. Prefer a chip
// that reads the CPU die (coretemp / k10temp / zenpower / cpu_thermal) and,
// within it, the tempN whose label is the package ("Package id 0" / "Tctl" /
// "Tdie"); else that chip's first tempN_input. An ACPI thermal zone (acpitz) is
// a last-resort backstop. Returns the tempN_input path (millidegrees C), or ""
// if nothing suitable. Portable across manifestor (Xeon) and manifold (Lunar
// Lake) -- both expose coretemp + "Package id 0".
static std::string discover_cpu_temp() {
  std::error_code ec;
  std::string acpi_fallback;
  for (const auto& e : fs::directory_iterator("/sys/class/hwmon", ec)) {
    const std::string dir = e.path().string();
    const std::string name = read_str(dir + "/name");
    const bool cpu = (name == "coretemp" || name == "k10temp" ||
                      name == "zenpower" || name == "cpu_thermal");
    if (!cpu && name != "acpitz") continue;

    std::string first, package;
    for (const auto& f : fs::directory_iterator(dir, ec)) {
      const std::string fn = f.path().filename().string();
      if (fn.compare(0, 4, "temp") != 0 || fn.size() <= 6 ||
          fn.compare(fn.size() - 6, 6, "_input") != 0)
        continue;
      if (first.empty()) first = f.path().string();
      const std::string stem = fn.substr(0, fn.size() - 6);   // "tempN"
      const std::string lbl = read_str(dir + "/" + stem + "_label");
      if (lbl == "Package id 0" || lbl == "Tctl" || lbl == "Tdie")
        package = f.path().string();
    }
    const std::string pick = !package.empty() ? package : first;
    if (pick.empty()) continue;
    if (cpu) return pick;                               // a real CPU chip wins
    if (acpi_fallback.empty()) acpi_fallback = pick;    // else remember acpitz
  }
  return acpi_fallback;
}

// Run ddcutil async, accumulate its stdout, and hand the whole output to `done`
// once the pipe closes -- so a slow I2C round trip never blocks the bar's main
// loop. stderr (ddcutil's permission/retry chatter) is discarded; GLib
// auto-reaps the child (no DO_NOT_REAP_CHILD flag), IOChannel closes the
// read fd when its last ref drops (the io slot returning false).
static void ddc_capture(const std::vector<std::string>& argv,
                        std::function<void(const std::string&)> done) {
  int outfd = -1;
  try {
    Glib::spawn_async_with_pipes(
        "", argv, Glib::SPAWN_SEARCH_PATH | Glib::SPAWN_STDERR_TO_DEV_NULL,
        Glib::SlotSpawnChildSetup(), nullptr, nullptr, &outfd, nullptr);
  } catch (const Glib::Error& e) {
    spdlog::warn("hw/gauge: ddcutil spawn failed: {}", e.what().raw());
    return;
  }
  auto buf = std::make_shared<std::string>();
  auto ch = Glib::IOChannel::create_from_fd(outfd);
  ch->set_encoding("");                  // binary: ddcutil output is ASCII
  ch->set_close_on_unref(true);
  Glib::signal_io().connect(
      [ch, buf, done](Glib::IOCondition cond) -> bool {
        if (cond & Glib::IO_IN) {
          char tmp[4096];
          gsize n = 0;
          auto st = ch->read(tmp, sizeof tmp, n);
          if (st == Glib::IO_STATUS_NORMAL && n > 0) {
            buf->append(tmp, n);
            return true;
          }
          if (st == Glib::IO_STATUS_AGAIN) return true;
        }
        done(*buf);                      // EOF/HUP/ERR: deliver what we have
        return false;
      },
      outfd, Glib::IO_IN | Glib::IO_HUP | Glib::IO_ERR);
}

Gauge::Gauge(const std::string& id, const waybar::Bar& bar,
             const Json::Value& config)
    : AModule(config, "hw", id, /*enable_click=*/false,
              /*enable_scroll=*/false),
      bar_(bar) {
  std::string k = config_["kind"].isString() ? config_["kind"].asString() : id;
  if (k == "brightness") kind_ = Kind::Brightness;
  else if (k == "volume") kind_ = Kind::Volume;
  else if (k == "temp" || k == "temperature") kind_ = Kind::Temp;
  else kind_ = Kind::Battery;

  if (config_["width"].isInt()) width_ = config_["width"].asInt();
  if (config_["font-size"].isNumeric())
    font_px_ = config_["font-size"].asDouble();
  if (config_["vmargin"].isInt()) vmargin_ = config_["vmargin"].asInt();
  if (config_["hmargin"].isInt()) hmargin_ = config_["hmargin"].asInt();
  if (config_["scroll-step"].isNumeric())
    scroll_step_ = config_["scroll-step"].asDouble();
  if (config_["style"].isString())
    bulb_style_ = (config_["style"].asString() == "bulb");
  if (config_["popup-top"].isInt()) popup_top_ = config_["popup-top"].asInt();
  if (config_["temp-min"].isNumeric())
    temp_min_ = config_["temp-min"].asDouble();
  if (config_["temp-max"].isNumeric())
    temp_max_ = config_["temp-max"].asDouble();
  if (config_["temp-warm"].isNumeric())
    temp_warm_ = config_["temp-warm"].asDouble();
  if (config_["temp-hot"].isNumeric())
    temp_hot_ = config_["temp-hot"].asDouble();
  read_batt_config();
  gpu_ = config_["source"].isString() && config_["source"].asString() == "gpu";
  double iv = config_["interval"].isNumeric()
                  ? config_["interval"].asDouble()
                  : (kind_ == Kind::Battery ? 10.0 : 2.0);
  interval_ms_ = static_cast<int>(iv * 1000.0);
  if (interval_ms_ < 200) interval_ms_ = 200;

  if (width_ <= 0) {
    switch (kind_) {
      case Kind::Battery: width_ = 54; break;
      case Kind::Volume:  width_ = 42; break;
      case Kind::Temp:    width_ = 46; break;
      default:            width_ = 30;   // brightness
    }
  }

  // Scale the gauge with the BAR height (48 = design), so the taller wall bar
  // enlarges the glyph + readout with no per-bar config. Base bar is a no-op.
  const double ui = bar_.config["height"].isInt()
                        ? bar_.config["height"].asInt() / 48.0
                        : 1.0;
  width_ = static_cast<int>(width_ * ui + 0.5);
  font_px_ *= ui;

  event_box_.set_name("hwgauge-" + (id.empty() ? k : id));

  area_.set_size_request(width_, -1);
  area_.set_hexpand(false);
  area_.set_margin_top(vmargin_);
  area_.set_margin_bottom(vmargin_);
  area_.set_margin_start(hmargin_);
  area_.set_margin_end(hmargin_);
  area_.signal_draw().connect(sigc::mem_fun(*this, &Gauge::on_draw));
  event_box_.add(area_);
  event_box_.show_all();

  if (kind_ == Kind::Volume) {
    audio_ = util::AudioBackend::getInstance([this] { this->dp.emit(); });
    available_ = (audio_ != nullptr);
  } else {
    // locate the sysfs node once
    std::error_code ec;
    if (kind_ == Kind::Brightness) {
      for (const auto& e : fs::directory_iterator("/sys/class/backlight", ec)) {
        bl_dir_ = e.path().string();
        break;
      }
      available_ = !bl_dir_.empty();
    } else if (kind_ == Kind::Temp) {
      if (gpu_) {
        available_ = Nvml::get().ok();          // GPU via NVML
      } else {
        temp_path_ = discover_cpu_temp();
        available_ = !temp_path_.empty();
      }
    } else {
      for (const auto& e :
           fs::directory_iterator("/sys/class/power_supply", ec)) {
        if (e.path().filename().string().rfind("BAT", 0) == 0) {
          bat_dir_ = e.path().string();
          break;
        }
      }
      available_ = !bat_dir_.empty();
    }
  }

  // A brightness gauge with no kernel backlight (a desktop driving external
  // monitors over DP/HDMI) falls back to DDC/CI: probe the monitors' I2C
  // side-channel asynchronously (an I2C sweep is slow) and reveal the widget
  // only if one answers. Until then it is PENDING, not dead -- so wire the rest
  // of the widget below but keep it hidden; probe_ddc reveals it on a hit.
  const bool ddc_pending = (kind_ == Kind::Brightness && !available_);
  if (ddc_pending) probe_ddc();

  // No hardware backend and nothing pending (a desktop with no battery, or no
  // audio): collapse rather than draw a dead glyph. no_show_all keeps
  // a parent show_all from resurrecting it, and nothing below (timer, input,
  // popup) is needed for a gauge that never shows.
  if (!available_ && !ddc_pending) {
    event_box_.set_no_show_all(true);
    event_box_.hide();
    return;
  }
  if (ddc_pending) {           // wired but hidden until the probe confirms DDC
    event_box_.set_no_show_all(true);
    event_box_.hide();
  }

  // Poll sysfs on a timer (backlight brightness / battery). DDC brightness is
  // event-driven (primed once, then written on input) -- no periodic read,
  // so no poll; volume is libpulse-driven.
  if (kind_ == Kind::Battery || kind_ == Kind::Temp ||
      (kind_ == Kind::Brightness && !bl_dir_.empty())) {
    timer_ = Glib::signal_timeout().connect(
        sigc::mem_fun(*this, &Gauge::on_timer), interval_ms_);
  }

  // Own all pointer input on the DrawingArea itself -- it has its own GdkWindow
  // and is what sits under the cursor, so (unlike the parent EventBox) it
  // actually receives motion events, which the click-vs-drag test needs.
  area_.add_events(Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK |
                   Gdk::BUTTON1_MOTION_MASK | Gdk::SCROLL_MASK |
                   Gdk::SMOOTH_SCROLL_MASK);
  area_.signal_button_press_event().connect(
      sigc::mem_fun(*this, &Gauge::on_press));
  area_.signal_button_release_event().connect(
      sigc::mem_fun(*this, &Gauge::on_release));
  area_.signal_motion_notify_event().connect(
      sigc::mem_fun(*this, &Gauge::on_motion));
  if (kind_ != Kind::Battery && kind_ != Kind::Temp) {
    area_.signal_scroll_event().connect(
        sigc::mem_fun(*this, &Gauge::handleScroll));
  }

  // The readout (brightness/volume value, battery detail) is published as a GTK
  // tooltip; the shared flush CALLOUT (driven by AModule) renders it under the
  // icon, in the same visual language as every other module's tooltip. We only
  // track hover here, so a value change while hovering refreshes it live.
  area_.add_events(Gdk::ENTER_NOTIFY_MASK | Gdk::LEAVE_NOTIFY_MASK);
  area_.signal_enter_notify_event().connect([this](GdkEventCrossing*) {
    hovered_ = true;
    return false;
  });
  area_.signal_leave_notify_event().connect([this](GdkEventCrossing*) {
    hovered_ = false;
    return false;
  });

  dp.emit();  // prime
}

Gauge::~Gauge() {
  if (timer_.connected()) timer_.disconnect();
}

bool Gauge::on_timer() {
  update();
  return true;
}

// ---- data ----------------------------------------------------------------

void Gauge::read_brightness() {
  if (bl_dir_.empty()) return;
  long b = read_long(bl_dir_ + "/brightness");
  long m = read_long(bl_dir_ + "/max_brightness");
  level_ = m > 0 ? static_cast<double>(b) / m : 0.0;
}

void Gauge::read_volume() {
  if (!audio_) return;
  level_ = audio_->getSinkVolume() / 100.0;
  muted_ = audio_->getSinkMuted();
}

void Gauge::read_temp() {
  if (gpu_)
    temp_c_ = Nvml::get().temp();
  else if (!temp_path_.empty())
    temp_c_ = read_long(temp_path_) / 1000.0;
  else
    return;
  double f = (temp_c_ - temp_min_) / (temp_max_ - temp_min_);
  level_ = f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);   // dial fraction
}

// The charge tiers, given in PERCENT ("batt-warn" 40, "batt-crit" 20 by
// default) and held as a 0..1 fraction to compare against level_ directly. An
// INCOHERENT pair is refused LOUDLY and wholesale rather than clamped: clamping
// would silently redefine what the user asked for, and a gauge drawing tiers
// nobody chose is worse than one drawing the documented defaults.
void Gauge::read_batt_config() {
  const bool has_warn = config_["batt-warn"].isNumeric();
  const bool has_crit = config_["batt-crit"].isNumeric();
  // Configure NOTHING when neither key is set: the members keep their in-class
  // fractions untouched, so an unconfigured gauge draws the exact tiers it drew
  // before they were configurable -- no percent -> fraction round trip to be
  // off by an ulp at a boundary.
  if (!has_warn && !has_crit) return;
  double warn = has_warn ? config_["batt-warn"].asDouble() : batt_warn_ * 100.0;
  double crit = has_crit ? config_["batt-crit"].asDouble() : batt_crit_ * 100.0;
  if (crit < 0.0 || crit > warn || warn > 100.0) {
    spdlog::error("hw/gauge: batt-crit {} / batt-warn {} incoherent (need "
                  "0 <= crit <= warn <= 100); keeping {} / {}",
                  crit, warn, batt_crit_ * 100.0, batt_warn_ * 100.0);
    return;
  }
  batt_crit_ = crit / 100.0;
  batt_warn_ = warn / 100.0;
}

// Which tier a charge falls in. The low end of each is INCLUSIVE (at or below
// batt-crit is Crit, at or below batt-warn is Warn), matching how this gauge
// has always drawn its boundaries. The ONE place the tiers are decided: the
// colour, the halo, the empty-cell void and the hooks all route through here.
Gauge::BattState Gauge::batt_state_for(double lvl) const {
  if (lvl <= batt_crit_) return BattState::Crit;
  if (lvl <= batt_warn_) return BattState::Warn;
  return BattState::Normal;
}

// Fire a tier's hook when the charge crosses INTO it. Edge-triggered, so a poll
// that finds the same tier spawns nothing. Three deliberate choices:
//  - the FIRST reading arms the tier SILENTLY. A bar restart (`wb restart`) is
//    routine here, and re-announcing "battery critical" on each one is noise.
//  - a hook sees BOTH directions: charging up past batt-crit enters Warn and
//    fires on-warn. Suppressing the upward edge would make on-normal ("back to
//    healthy") impossible, which is the more useful of the two; a hook that
//    cares about direction reads /sys status itself.
//  - the tier advances even with NO hook defined, so an undefined state cannot
//    desync the machine and misattribute the next crossing.
void Gauge::check_batt_state() {
  const BattState st = batt_state_for(level_);
  if (!batt_primed_) {
    batt_primed_ = true;
    batt_state_ = st;
    return;
  }
  if (st == batt_state_) return;
  batt_state_ = st;
  const char* key = st == BattState::Crit   ? "on-crit"
                    : st == BattState::Warn ? "on-warn"
                                            : "on-normal";
  if (!config_[key].isString()) return;      // undefined: nothing fires
  try {
    Glib::spawn_command_line_async(config_[key].asString());
  } catch (const Glib::Error& err) {
    spdlog::warn("hw/gauge: {} failed: {}", key, err.what().raw());
  }
}

void Gauge::read_battery() {
  if (bat_dir_.empty()) return;
  status_ = read_str(bat_dir_ + "/status");
  level_ = read_long(bat_dir_ + "/capacity") / 100.0;

  long efull = read_long(bat_dir_ + "/energy_full");
  long edesign = read_long(bat_dir_ + "/energy_full_design");
  long enow = read_long(bat_dir_ + "/energy_now");
  long pnow = read_long(bat_dir_ + "/power_now");
  if (efull == 0) {  // charge_*/current_* fallback (some batteries)
    efull = read_long(bat_dir_ + "/charge_full");
    edesign = read_long(bat_dir_ + "/charge_full_design");
    enow = read_long(bat_dir_ + "/charge_now");
    pnow = read_long(bat_dir_ + "/current_now");
  }
  health_ = edesign > 0 ? static_cast<double>(efull) / edesign : 1.0;
  power_w_ = pnow / 1e6;
  cycles_ = static_cast<int>(read_long(bat_dir_ + "/cycle_count"));

  charging_ = (status_ == "Charging");
  plugged_ = (status_ == "Full" || status_ == "Not charging" || charging_);

  time_min_ = -1;
  if (pnow > 0) {
    double hrs = 0.0;
    if (status_ == "Charging" && efull > enow)
      hrs = static_cast<double>(efull - enow) / pnow;
    else if (status_ == "Discharging" && enow > 0)
      hrs = static_cast<double>(enow) / pnow;
    if (hrs > 0.0) time_min_ = static_cast<int>(hrs * 60.0 + 0.5);
  }
  // no meaningful estimate when full/topped off
  if (status_ == "Full" || level_ >= 0.995 || time_min_ == 0) time_min_ = -1;

  check_batt_state();   // level_ is settled: fire a hook if the tier changed
}

void Gauge::update_tooltip() {
  char buf[256];
  std::string tt;
  int pct = static_cast<int>(level_ * 100.0 + 0.5);
  if (kind_ == Kind::Brightness) {
    std::snprintf(buf, sizeof buf, "Brightness %d%%", pct);
    tt = buf;
  } else if (kind_ == Kind::Volume) {
    if (muted_) {
      std::snprintf(buf, sizeof buf, "Volume %d%%  (muted)", pct);
    } else {
      std::snprintf(buf, sizeof buf, "Volume %d%%", pct);
    }
    tt = buf;
  } else if (kind_ == Kind::Temp) {
    std::snprintf(buf, sizeof buf, "%s  %.0f °F  (%.0f °C)",
                  gpu_ ? "GPU" : "CPU",
                  temp_c_ * 9.0 / 5.0 + 32.0, temp_c_);
    tt = buf;
  } else {  // battery
    std::snprintf(buf, sizeof buf, "Battery %d%%  (%s)\n", pct,
                  status_.empty() ? "?" : status_.c_str());
    tt += buf;
    if (time_min_ >= 0) {
      std::snprintf(buf, sizeof buf, "%d:%02d %s\n", time_min_ / 60,
                    time_min_ % 60, charging_ ? "to full" : "remaining");
      tt += buf;
    }
    std::snprintf(buf, sizeof buf, "%.1f W\nHealth %d%% (%d cycles)", power_w_,
                  static_cast<int>(health_ * 100.0 + 0.5), cycles_);
    tt += buf;
  }
  tip_text_ = tt;
  // publish for the shared flush callout (AModule reads this on hover); refresh
  // live if we are currently hovered so a scroll-to-adjust updates the readout.
  event_box_.set_tooltip_text(tt);
  if (hovered_)
    FlushTooltip::instance().show(event_box_,
                                  FlushTooltip::findTooltip(event_box_));
}

auto Gauge::update() -> void {
  switch (kind_) {
    case Kind::Brightness: read_brightness(); break;
    case Kind::Volume: read_volume(); break;
    case Kind::Battery: read_battery(); break;
    case Kind::Temp: read_temp(); break;
  }
  update_tooltip();
  area_.queue_draw();
}

// ---- input ---------------------------------------------------------------

// Set the level natively: volume via libpulse, brightness by writing sysfs (the
// panel's backlight is user-writable via `video` group) or, on a box with no
// backlight, over DDC/CI to the external monitors. No fork on the pulse/sysfs
// paths; the DDC path spawns a throttled ddcutil (see ddc_set).
void Gauge::apply_level(double lvl) {
  if (lvl < 0.0) lvl = 0.0;
  if (lvl > 1.0) lvl = 1.0;
  if (kind_ == Kind::Volume && audio_) {
    audio_->changeVolume(static_cast<uint16_t>(lvl * 100.0 + 0.5), 0, 100);
  } else if (kind_ == Kind::Brightness && !bl_dir_.empty()) {
    long m = read_long(bl_dir_ + "/max_brightness");
    std::ofstream f(bl_dir_ + "/brightness");
    if (f) f << static_cast<long>(lvl * m + 0.5);
  } else if (kind_ == Kind::Brightness && ddc_) {
    ddc_set(lvl);
  }
  level_ = lvl;         // immediate feedback; the callback/poll confirms it
  area_.queue_draw();
}

// Fire the OSD via swayosd -- the exact client the volume/brightness keybinds
// use -- so a widget change pops the same overlay a key press does. swayosd's
// brightness path drives the kernel backlight, so it is a no-op (or worse, a
// misleading empty OSD) on a DDC box -- skip it there; the gauge itself and its
// hover popup are the feedback.
void Gauge::osd(const std::string& arg) {
  if (kind_ == Kind::Brightness && ddc_) return;
  const std::string opt =
      kind_ == Kind::Volume ? "--output-volume " : "--brightness ";
  try {
    Glib::spawn_command_line_async("swayosd-client " + opt + arg);
  } catch (const Glib::Error& err) {
    spdlog::warn("hw/gauge: swayosd-client failed: {}", err.what().raw());
  }
}

// Async DDC/CI probe: `ddcutil detect` lists each monitor's I2C bus. Parse the
// "I2C bus:" lines (stdout only -- permission warnings are on the discarded
// stderr) for the bus numbers, then reveal the widget and prime its level from
// the first monitor (all monitors are driven in lockstep, so one is the truth).
// No monitor answers -> the widget stays hidden, the same collapse as a missing
// backlight, just decided a beat later off the startup path.
void Gauge::probe_ddc() {
  ddc_capture({"ddcutil", "detect", "--brief"}, [this](const std::string& out) {
    std::istringstream ss(out);
    std::string line;
    while (std::getline(ss, line)) {
      auto p = line.find("/dev/i2c-");
      if (line.find("I2C bus:") == std::string::npos ||
          p == std::string::npos)
        continue;
      ddc_buses_.push_back(std::atoi(line.c_str() + p + 9));  // "/dev/i2c-"=9
    }
    if (ddc_buses_.empty()) return;   // no DDC monitor: leave the gauge hidden
    ddc_ = true;
    available_ = true;
    event_box_.set_no_show_all(false);
    event_box_.show_all();

    // prime the shown level from the first monitor: `getvcp 10 --brief` prints
    // "VCP 10 C <cur> <max>".
    ddc_capture({"ddcutil", "-b", std::to_string(ddc_buses_.front()), "getvcp",
                 "10", "--brief"},
                [this](const std::string& g) {
                  std::istringstream gs(g);
                  std::vector<std::string> f;
                  std::string tok;
                  while (gs >> tok) f.push_back(tok);
                  if (f.size() >= 5 && f[0] == "VCP") {
                    double cur = std::atof(f[3].c_str());
                    double mx = std::atof(f[4].c_str());
                    if (mx > 0.0) {
                      level_ = cur / mx;
                      update_tooltip();
                      area_.queue_draw();
                    }
                  }
                });
  });
}

// Push the level to every DDC monitor in lockstep, throttled. setvcp is a slow
// I2C round trip, so a leading write fires immediately and any further change
// inside the window coalesces into ONE trailing flush of the latest value -- a
// drag never spawns a pile of ddcutil processes, and the final value always
// lands.
void Gauge::ddc_set(double lvl) {
  ddc_pending_val_ = lvl < 0.0 ? 0.0 : lvl > 1.0 ? 1.0 : lvl;
  const auto now = Clock::now();
  if (now - last_ddc_t_ >= std::chrono::milliseconds(200)) {
    ddc_flush();
  } else if (!ddc_flush_scheduled_) {
    ddc_flush_scheduled_ = true;
    Glib::signal_timeout().connect_once(
        [this] {
          ddc_flush_scheduled_ = false;
          ddc_flush();
        },
        200);
  }
}

void Gauge::ddc_flush() {
  last_ddc_t_ = Clock::now();
  const int v = static_cast<int>(ddc_pending_val_ * 100.0 + 0.5);
  for (int bus : ddc_buses_) {
    try {
      Glib::spawn_command_line_async("ddcutil -b " + std::to_string(bus) +
                                     " setvcp 10 " + std::to_string(v));
    } catch (const Glib::Error& err) {
      spdlog::warn("hw/gauge: ddcutil setvcp failed: {}", err.what().raw());
    }
  }
}

bool Gauge::handleScroll(GdkEventScroll* e) {
  auto dir = getScrollDir(e);
  if (dir == SCROLL_DIR::NONE) return true;
  bool up = (dir == SCROLL_DIR::UP || dir == SCROLL_DIR::RIGHT);
  if (kind_ == Kind::Brightness) {
    // Set brightness natively, exactly +/- scroll_step_: the step is symmetric
    // and the icon updates immediately (queue_draw in apply_level). Brightness
    // has no change-event source, so the old path -- route the notch through
    // swayosd raise/lower and re-read /sys once, 150ms later -- both lagged (a
    // late write missed the read and waited for the 2s poll) and stepped
    // unevenly up vs down (swayosd's perceptual curve). Then fire the OSD via
    // +0 to pop the same overlay without a second, differing change.
    apply_level(level_ + (up ? 1.0 : -1.0) * scroll_step_ / 100.0);
    osd("+0");
  } else {
    // volume: swayosd changes it and libpulse's callback refreshes us instantly
    osd(up ? "raise" : "lower");
  }
  return true;
}

bool Gauge::on_press(GdkEventButton* e) {
  if (kind_ == Kind::Volume && e->button == 3) {   // right-click mutes
    osd("mute-toggle");
    return true;
  }
  if (e->button == 1 && kind_ != Kind::Battery && kind_ != Kind::Temp) {
    drag_armed_ = true;   // arm a possible drag (input kinds only)
    dragging_ = false;
    drag_x0_ = e->x_root;
    drag_y0_ = e->y_root;
    drag_l0_ = level_;
    return true;
  }
  return false;
}

bool Gauge::on_motion(GdkEventMotion* e) {
  if (!drag_armed_) return false;
  const double dy = drag_y0_ - e->y_root;   // up = raise
  const double dx = e->x_root - drag_x0_;   // right = raise
  const double d = std::abs(dy) >= std::abs(dx) ? dy : dx;
  if (!dragging_ && std::abs(d) < 3.0) return true;   // click vs drag threshold
  dragging_ = true;
  apply_level(drag_l0_ + d / 160.0);        // ~160px of travel spans 0..100%
  // ping the OSD (throttled) so a drag shows the same overlay as the keys; +0
  // shows the current value without changing it (we already set it natively)
  auto now = Clock::now();
  if (now - last_osd_t_ >= std::chrono::milliseconds(60)) {
    osd("+0");
    last_osd_t_ = now;
  }
  return true;
}

bool Gauge::on_release(GdkEventButton* e) {
  if (e->button != 1) return false;
  const bool was_drag = dragging_;
  drag_armed_ = false;
  dragging_ = false;
  if (!was_drag && config_["on-click"].isString()) {   // a plain click
    try {
      Glib::spawn_command_line_async(config_["on-click"].asString());
    } catch (const Glib::Error& err) {
      spdlog::warn("hw/gauge: on-click failed: {}", err.what().raw());
    }
  }
  return true;
}

// ---- drawing -------------------------------------------------------------

void Gauge::batt_color(double& r, double& g, double& b) const {
  switch (batt_state_for(level_)) {
    case BattState::Crit: r = 0.95; g = 0.30; b = 0.30; break;   // red
    case BattState::Warn: r = 0.97; g = 0.75; b = 0.20; break;   // amber
    default:              r = 0.40; g = 0.85; b = 0.45;          // green
  }
}

void Gauge::draw_sun(const Cairo::RefPtr<Cairo::Context>& cr, double cx,
                     double cy, double rad, double r, double g, double b) {
  const double lvl = level_ < 0.0 ? 0.0 : level_ > 1.0 ? 1.0 : level_;
  auto clamp01 = [](double v) { return v < 0 ? 0 : v > 1 ? 1 : v; };
  const double raygrow = clamp01((lvl - 0.65) / 0.35);   // rays appear >65%
  const double cloud = clamp01((0.50 - lvl) / 0.50);     // cloud grows <50%
  const double orb_r = rad * 0.72;
  const double orb_cy = cy;   // orb stays centred; the cloud rolls across it

  // fluffy cloud silhouette (a cluster of puffs, rounded all round -- no flat
  // rectangle). Drawn twice: a larger dark pass gives a dark edge, then
  // the light body, plus a soft underside shade for a bit of texture/contrast.
  auto draw_cloud = [&](double mx, double my, double s) {
    auto blob = [&](double e) {
      cr->arc(mx - 0.86 * s, my + 0.12 * s, 0.46 * s + e, 0, 2.0 * kPi);
      cr->fill();
      cr->arc(mx - 0.30 * s, my - 0.30 * s, 0.55 * s + e, 0, 2.0 * kPi);
      cr->fill();
      cr->arc(mx + 0.32 * s, my - 0.36 * s, 0.60 * s + e, 0, 2.0 * kPi);
      cr->fill();
      cr->arc(mx + 0.88 * s, my + 0.06 * s, 0.48 * s + e, 0, 2.0 * kPi);
      cr->fill();
      cr->arc(mx - 0.02 * s, my + 0.20 * s, 0.58 * s + e, 0, 2.0 * kPi);
      cr->fill();
      cr->rectangle(mx - 0.86 * s - e, my + 0.06 * s, 1.72 * s + 2 * e,
                    0.42 * s + e);
      cr->fill();
    };
    cr->set_source_rgba(0.30, 0.33, 0.40, 1.0);   // dark edge
    blob(1.6);
    cr->set_source_rgba(0.87, 0.90, 0.94, 1.0);   // light body
    blob(0.0);
    cr->set_source_rgba(0.55, 0.58, 0.66, 0.45);  // soft underside shade
    cr->arc(mx - 0.02 * s, my + 0.34 * s, 0.52 * s, 0, 2.0 * kPi);
    cr->fill();
  };

  // The orb stays CENTRED and warm; the cloud rises from below to cover it as
  // it dims, so the sun peeks over the cloud top and the glyph never drifts.
  // alpha halo behind the orb, growing with brightness (fades under cloud)
  if (lvl > 0.02) {
    const double halo_a = (0.06 + 0.26 * lvl) * (1.0 - cloud);
    const double halo_r = rad * (1.05 + 1.15 * lvl);
    for (int i = 0; i < 6 && halo_a > 0.005; i++) {
      double t = i / 5.0;
      double rr = halo_r - (halo_r - orb_r * 0.9) * t;
      cr->arc(cx, orb_cy, rr, 0, 2.0 * kPi);
      cr->set_source_rgba(r, g, b, halo_a * (0.12 + 0.9 * t));
      cr->fill();
    }
  }

  // rays (behind the orb): tapered triangles, thick at base, length grows
  if (raygrow > 0.0) {
    const int n = 8;
    const double ri = rad * 0.74;
    const double rlen = rad * (0.35 + 0.95 * raygrow);
    const double wb = rad * 0.16;
    cr->set_source_rgba(r, g, b, 1.0);
    for (int i = 0; i < n; i++) {
      double a = 2.0 * kPi * i / n;
      double dx = std::cos(a), dy = std::sin(a), px = -dy, py = dx;
      cr->move_to(cx + ri * dx + px * wb, cy + ri * dy + py * wb);
      cr->line_to(cx + (ri + rlen) * dx, cy + (ri + rlen) * dy);
      cr->line_to(cx + ri * dx - px * wb, cy + ri * dy - py * wb);
      cr->close_path();
      cr->fill();
    }
  }

  // the orb (always warm/visible; rises with the cloud to stay centred)
  cr->arc(cx, orb_cy, orb_r, 0, 2.0 * kPi);
  cr->set_source_rgba(r, g, b, 1.0);
  cr->fill();

  // the cloud rolls in from the right, drifting across the sun as it dims: it
  // covers the sun from the right first, so the sun peeks out the left, and by
  // ~0 it is centred over the orb and big enough to hide it entirely
  if (cloud > 0.02) {
    const double s = rad * (0.62 + 0.46 * cloud);
    const double cloud_cx = cx + rad * (2.3 - 2.3 * cloud);
    draw_cloud(cloud_cx, cy + rad * 0.05, s);
  }
}

// Alternate brightness idiom: a light bulb. Bright -> full amber glass + a halo
// glow (same cue as the sun). Dimming first drops the halo, then the glass fill
// fades until only the amber filament glows in a clear "Edison" bulb.
void Gauge::draw_bulb(const Cairo::RefPtr<Cairo::Context>& cr, double cx,
                      double cy, double rad, double r, double g, double b) {
  const double lvl = level_ < 0.0 ? 0.0 : level_ > 1.0 ? 1.0 : level_;
  auto c01 = [](double v) { return v < 0 ? 0 : v > 1 ? 1 : v; };
  // Fade the glow colour toward a bright, pale warm-gold as brightness climbs
  // past 40%, so a bright bulb reads as brighter/whiter -- but the target is
  // COMPRESSED (not full white) so the per-level steps stay small and the ramp
  // reads smooth. At or below 40% the colour is untouched. This lightens every
  // element drawn in (r,g,b): the halo, the glass fill, and the filament glow
  // (the deep-amber filament core stays amber).
  {
    const double w = c01((lvl - 0.40) / 0.60);
    r += (1.00 - r) * w;
    g += (0.93 - g) * w;
    b += (0.78 - b) * w;
  }
  // Canonical A19 / E26 proportions (glass diameter D = 2R): glass height
  // ~1.15 D, max diameter ~40% down from the crown, a near-hemisphere crown, a
  // long concave shoulder, and an E26 screw base about as wide as the neck and
  // ~0.55 D tall. Drawn at true aspect (no horizontal distortion).
  const double R = rad * 0.62;               // glass radius (half of D)
  const double baseH = rad * 0.68;           // E26 base length (~0.55 D)
  const double ey = cy - rad * 0.71;         // reference, centres the tall bulb
  const double neckW = R * 0.45;             // glass neck (~0.45 D wide)
  const double widestY = ey + R * 0.10;      // max diameter, ~40% down glass
  const double apexY = widestY - R * 0.92;   // crown: near-hemisphere above it
  const double neckY = widestY + R * 1.38;   // neck: long concave shoulder
  const double gcy = ey - R * 0.30;          // glass centroid (halo)
  const double fcy = ey - R * 0.05;          // filament in the belly

  // A19 silhouette: a rounded crown, widest ~40% down, a concave shoulder
  // tapering to a narrow neck; taller than wide, so it reads as a bulb.
  auto glass = [&]() {
    cr->begin_new_path();
    cr->move_to(cx - neckW, neckY);
    // neck -> widest: the concave shoulder. Rises steeply off the neck, then
    // sweeps out to the belly with a vertical tangent at the widest point.
    cr->curve_to(cx - R * 0.55, neckY - R * 0.72,
                 cx - R, widestY + R * 0.52, cx - R, widestY);
    // widest -> apex: a near-hemisphere crown (quarter-circle bezier).
    cr->curve_to(cx - R, widestY - R * 0.51,
                 cx - R * 0.55, apexY, cx, apexY);
    cr->curve_to(cx + R * 0.55, apexY,
                 cx + R, widestY - R * 0.51, cx + R, widestY);
    cr->curve_to(cx + R, widestY + R * 0.52,
                 cx + R * 0.55, neckY - R * 0.72, cx + neckW, neckY);
    cr->close_path();
  };

  // True aspect (no horizontal distortion) so the A19 silhouette stays
  // canonical -- glass slightly taller than wide. Kept as an explicit knob
  // about the centre; matched by the restore at the end.
  cr->save();
  cr->translate(cx, 0.0);
  cr->scale(1.0, 1.0);
  cr->translate(-cx, 0.0);

  // halo behind the bulb -- a glow OUTSIDE the glass. A faint ambient glow is
  // ALWAYS present (even hollow/off, the bulb still reads as lit glass), and it
  // grows with the level (the light it throws). The rim is drawn LAST so it
  // never washes out the shape.
  const double halo_a = 0.06 + lvl * 0.26;
  {
    const double halo_r = R * (1.2 + 0.8 * lvl);
    for (int i = 0; i < 6; i++) {
      double t = i / 5.0;
      double rr = halo_r - (halo_r - R * 0.9) * t;
      cr->arc(cx, gcy, rr, 0, 2.0 * kPi);
      cr->set_source_rgba(r, g, b, halo_a * (0.12 + 0.9 * t));
      cr->fill();
    }
  }

  // E26 screw base: a bright metal body, darker grooves, and a crisp outline so
  // it never washes into the grey bar. Straight sides with CURVED top/bottom
  // edges so it reads as a cylinder; the glass stays A19, the base does not.
  // bW runs over the neck to offset the rim stroke (else the base looks thin).
  const double bW = neckW * 2.9;                  // E26 base, over the neck
  const double x0 = cx - bW / 2.0, x1 = cx + bW / 2.0;
  const double bTop = neckY - R * 0.02;
  const double bBot = bTop + baseH;
  const double eC = bW * 0.16;                    // top/bottom ellipse depth
  const double kEdge[3] = {0.33, 0.33, 0.38};     // crisp grey edge
  auto base_body = [&]() {
    cr->begin_new_path();
    cr->move_to(x0, bTop);
    cr->curve_to(x0 + bW * 0.28, bTop - eC, x1 - bW * 0.28, bTop - eC,
                 x1, bTop);
    cr->line_to(x1, bBot);
    cr->curve_to(x1 - bW * 0.28, bBot + eC, x0 + bW * 0.28, bBot + eC,
                 x0, bBot);
    cr->close_path();
  };
  base_body();
  cr->set_source_rgba(0.96, 0.96, 0.98, 1.0);     // bright metal body
  cr->fill();
  cr->set_line_cap(Cairo::LINE_CAP_ROUND);
  for (int i = 0; i < 4; i++) {                   // curved thread grooves
    double yy = bTop + baseH * (0.18 + 0.21 * i);
    cr->set_source_rgba(0.40, 0.40, 0.45, 1.0);   // darker grooves
    cr->set_line_width(1.4);
    cr->move_to(x0 + 1.5, yy);
    cr->curve_to(x0 + bW * 0.3, yy + eC * 0.7, x1 - bW * 0.3, yy + eC * 0.7,
                 x1 - 1.5, yy);
    cr->stroke();
  }
  base_body();                                     // thin grey outline last
  cr->set_source_rgba(kEdge[0], kEdge[1], kEdge[2], 1.0);
  cr->set_line_width(1.2);
  cr->stroke();
  // rounded contact tip under the base: light and obvious, a clear solder blob
  const double tipR = bW * 0.24;
  cr->arc(cx, bBot + eC * 0.5 + tipR * 0.45, tipR, 0, 2.0 * kPi);
  cr->set_source_rgba(0.90, 0.90, 0.93, 1.0);   // bright contact tip
  cr->fill_preserve();
  cr->set_source_rgba(kEdge[0], kEdge[1], kEdge[2], 1.0);
  cr->set_line_width(1.1);
  cr->stroke();

  // glass fill: the bulb fills with light GRADUALLY across the 40->100% band
  // (clear/hollow at/below 40%), colour whitening with level (the near-white
  // fade above), so the brightening reads smoothly across the range rather
  // than popping in near max. Boundaries stay crisp -- the rim is drawn last.
  const double glass_a = c01((lvl - 0.40) / 0.60);
  if (glass_a > 0.01) {
    glass();
    cr->set_source_rgba(r, g, b, 0.85 * glass_a);
    cr->fill();
  }

  // filament: an amber zigzag around the glass centroid. A DEEP-amber core (a
  // touch darker/redder than the glass) so it doesn't vanish into the yellow
  // fill, wrapped in a soft warm glow that's always a little present and grows
  // as it dims.
  auto filament = [&]() {
    cr->begin_new_path();
    cr->move_to(cx - 0.28 * R, fcy + 0.30 * R);
    cr->line_to(cx - 0.25 * R, fcy - 0.04 * R);
    cr->line_to(cx - 0.11 * R, fcy - 0.32 * R);
    cr->line_to(cx, fcy + 0.08 * R);
    cr->line_to(cx + 0.11 * R, fcy - 0.32 * R);
    cr->line_to(cx + 0.25 * R, fcy - 0.04 * R);
    cr->line_to(cx + 0.28 * R, fcy + 0.30 * R);
  };
  cr->set_line_join(Cairo::LINE_JOIN_ROUND);
  // The filament dims and thins WITH the bulb: a faint, thin hint when low (a
  // dim bulb), filling out and gaining a warm glow only as it brightens.
  const double glow_a = c01((lvl - 0.55) / 0.45) * 0.30;
  if (glow_a > 0.02) {
    filament();
    cr->set_source_rgba(r, g, b, glow_a);
    cr->set_line_width(4.2);
    cr->stroke();
  }
  filament();
  cr->set_source_rgba(0.85, 0.46, 0.12, 0.18 + 0.55 * lvl * lvl);
  cr->set_line_width(0.7 + 1.0 * lvl);
  cr->stroke();

  // glass rim LAST -- a crisp outline. White when lit (halo), shifting
  // to amber as it goes hollow, so a clear bulb still "reads" as a bulb.
  glass();
  const double rb = c01((lvl - 0.80) / 0.20);   // white near max; amber <80%
  cr->set_source_rgba(r + (0.90 - r) * rb, g + (0.94 - g) * rb,
                      b + (0.96 - b) * rb, 0.92);
  cr->set_line_width(1.8);
  cr->stroke();

  cr->restore();   // end the 10%-wider X scale
}

// Volume as a guitar-amp knob: a value arc sweeping around the top, and a
// central knob disc with a pointer indicating the level.
void Gauge::draw_ring(const Cairo::RefPtr<Cairo::Context>& cr, double cx,
                      double cy, double rad, double r, double g, double b) {
  const double a0 = 0.75 * kPi;        // min at lower-left
  const double sweep = 1.5 * kPi;      // 270deg, gap at the bottom
  // The needle stays at the SET level even when muted -- mute is shown as a
  // distinct state (dial goes monochrome grey, "MUTED" in red on top), not
  // volume 0. The MUTED text is drawn by on_draw.
  const double lvl = level_ < 0.0 ? 0.0 : level_ > 1.0 ? 1.0 : level_;
  const double ang = a0 + sweep * lvl;
  double tr = r, tg = g, tb = b;                          // track / base colour
  double vr = std::min(1.0, r * 1.45), vg = std::min(1.0, g * 1.45),
         vb = std::min(1.0, b * 1.45);                    // lit (behind needle)
  if (muted_) {                        // monochrome: dark track, light lit
    tr = tg = tb = 0.34;
    vr = vg = vb = 0.78;
  }

  cr->set_line_cap(Cairo::LINE_CAP_BUTT);   // butt caps -> clean notch gaps
  // The arc is broken into NP notched pieces (wide gaps between them). Each
  // piece is drawn as short sub-arcs so the stroke tapers -- thin at the
  // minimum, thick toward 100% (width keyed to absolute position on the dial).
  const int NP = 5;
  const double cell = sweep / NP;
  const double gap = cell * 0.28;
  auto piece = [&](double s, double e) {
    if (e <= s) return;
    int seg = std::max(2, static_cast<int>((e - s) / 0.045));
    for (int i = 0; i < seg; i++) {
      double aa = s + (e - s) * i / seg;
      double ab = s + (e - s) * (i + 1) / seg;
      double fabs = ((aa + ab) / 2.0 - a0) / sweep;   // 0..1 along the dial
      cr->set_line_width(2.4 + 6.2 * fabs);
      cr->begin_new_sub_path();
      cr->arc(cx, cy, rad, aa, ab);
      cr->stroke();
    }
  };
  for (int i = 0; i < NP; i++) {
    double cs = a0 + i * cell + gap / 2.0;
    double ce = a0 + (i + 1) * cell - gap / 2.0;
    cr->set_source_rgba(tr, tg, tb, 0.62);     // track: always-visible
    piece(cs, ce);
    if (lvl > 0.0 && ang > cs) {               // lit portion: brighter
      cr->set_source_rgba(vr, vg, vb, 1.0);
      piece(cs, std::min(ce, ang));
    }
  }
  // knob body: a dark metallic disc with a coloured rim
  const double kr = rad * 0.60;
  cr->arc(cx, cy, kr, 0, 2.0 * kPi);
  cr->set_source_rgba(0.17, 0.17, 0.19, 1.0);
  cr->fill_preserve();
  cr->set_line_width(1.2);
  cr->set_source_rgba(tr, tg, tb, 0.55);
  cr->stroke();
  // white pointer from the knob centre toward the current angle. When muted the
  // needle is kept short so it stays inside the knob, below the "MUTED" word.
  const double nlen = muted_ ? 0.55 : 0.92;
  cr->set_line_cap(Cairo::LINE_CAP_ROUND);
  cr->set_line_width(2.2);
  cr->set_source_rgba(1.0, 1.0, 1.0, 1.0);
  cr->move_to(cx + kr * 0.15 * std::cos(ang), cy + kr * 0.15 * std::sin(ang));
  cr->line_to(cx + kr * nlen * std::cos(ang), cy + kr * nlen * std::sin(ang));
  cr->stroke();
}

void Gauge::draw_battery(const Cairo::RefPtr<Cairo::Context>& cr, double x,
                         double y, double w, double h) {
  // A cylindrical cell lying on its side: straight top/bottom edges with the
  // left/right ENDS bowed out into elliptical caps (the bulb screw-base trick,
  // rotated 90deg -- curved edges read as a cylinder). A vertical metal bevel
  // catches the light along the top of the tube, over a glossy charge fill and
  // a domed terminal. An OUTSIDE halo glows red when low, green when charging.
  const double nubw = 2.6;                       // positive-terminal nub
  const double bh = std::min(h, 20.0);
  const double eC = std::min(bh * 0.44, 5.6);    // end-cap bulge (bold)
  const double bw = w - 2.0 - nubw - 2.0 * eC;   // body; caps+nub fill the box
  const double bx = x + 1.0 + eC;                // body left; left cap bows out
  const double by = y + (h - bh) / 2.0;
  const double nubh = bh * 0.40;
  const double lvl = level_ < 0.0 ? 0.0 : level_ > 1.0 ? 1.0 : level_;
  double r, g, b;
  batt_color(r, g, b);

  // horizontal cylinder: straight top/bottom, elliptical left/right end caps
  auto hcyl = [&](double X, double Y, double W, double H, double e) {
    cr->begin_new_path();
    cr->move_to(X, Y);
    cr->line_to(X + W, Y);
    cr->curve_to(X + W + e, Y + H * 0.28, X + W + e, Y + H * 0.72,
                 X + W, Y + H);
    cr->line_to(X, Y + H);
    cr->curve_to(X - e, Y + H * 0.72, X - e, Y + H * 0.28, X, Y);
    cr->close_path();
  };

  // OUTSIDE halo (like the bulb's): a soft glow whose HOT CORE hides behind the
  // shell, leaving only a smooth outward falloff -- no crisp rim. Contrasted
  // to the fill so it reads as light: charging is warm yellow over a green
  // (above batt-warn) cell, lime over a warm amber/red one; else red in the
  // crit tier, amber in the warn tier. A healthy unplugged cell stays calm.
  double gr = r, gg = g, gb = b, ga = 0.0, gspread = 6.0;
  const BattState bst = batt_state_for(level_);
  if (plugged_) {
    if (bst == BattState::Normal) {       // over a green cell: warm yellow
      gr = 1.00; gg = 0.80; gb = 0.13;
    } else {                              // over an amber/red one: lime
      gr = 0.80; gg = 0.97; gb = 0.20;
    }
    ga = 0.95; gspread = 11.0;
  } else if (bst == BattState::Crit) { ga = 0.65; gspread = 9.5; }
  else if (bst == BattState::Warn)   { ga = 0.34; gspread = 9.5; }
  if (ga > 0.0) {
    const int N = 18;
    const double inset = 2.5;          // hottest rings sit inside the shell
    for (int i = 0; i < N; i++) {
      const double t = i / static_cast<double>(N - 1);   // 0 far .. 1 inside
      const double pad = gspread - (gspread + inset) * t;
      hcyl(bx - pad, by - pad, bw + 2.0 * pad, bh + 2.0 * pad,
           eC + (pad > 0.0 ? pad * 0.6 : 0.0));
      // soft start at the shell (hot core hidden) but an AGGRESSIVE quadratic
      // taper, so it blooms out yet dissipates fast -- neither a flood-filled
      // tile nor a sharp rim, just a whisper left where the region clips it
      const double fall = 0.02 + 0.98 * t * t;
      cr->set_source_rgba(gr, gg, gb, ga * fall * 0.38);
      cr->fill();
    }
  }

  // domed terminal nub on the right cap, drawn first so the shell overlaps it
  const double nx = bx + bw + eC - 0.6, ny = by + (bh - nubh) / 2.0;
  const double nr = std::min(nubh * 0.45, nubw * 0.7);
  cr->begin_new_path();
  cr->move_to(nx, ny);
  cr->line_to(nx + nubw - nr, ny);
  cr->arc(nx + nubw - nr, ny + nr, nr, -0.5 * kPi, 0.0);
  cr->line_to(nx + nubw, ny + nubh - nr);
  cr->arc(nx + nubw - nr, ny + nubh - nr, nr, 0.0, 0.5 * kPi);
  cr->line_to(nx, ny + nubh);
  cr->close_path();
  cr->set_source_rgba(0.88, 0.88, 0.92, 1.0);
  cr->fill();

  // metal shell: a vertical bevel (bright top -> dark bottom) + crisp outline
  auto shell = Cairo::LinearGradient::create(0.0, by, 0.0, by + bh);
  shell->add_color_stop_rgba(0.0,  0.84, 0.84, 0.89, 1.0);  // top edge (turned)
  shell->add_color_stop_rgba(0.30, 1.00, 1.00, 1.00, 1.0);  // specular top band
  shell->add_color_stop_rgba(0.66, 0.62, 0.62, 0.68, 1.0);
  shell->add_color_stop_rgba(1.0,  0.26, 0.26, 0.32, 1.0);  // deep bottom shade
  hcyl(bx, by, bw, bh, eC);
  cr->set_source(shell);
  cr->fill_preserve();
  cr->set_line_width(1.4);
  cr->set_source_rgba(0.16, 0.16, 0.19, 1.0);    // dark outline for the bar
  cr->stroke();

  // specular sheen along the top of the tube (sells the cylinder curvature)
  cr->save();
  hcyl(bx, by, bw, bh, eC);
  cr->clip();
  cr->rectangle(bx - eC, by + bh * 0.06, bw + 2.0 * eC, bh * 0.16);
  cr->set_source_rgba(1.0, 1.0, 1.0, 0.32);
  cr->fill();
  cr->restore();

  // hollow interior cavity (the empty cell), inset to leave a metal frame
  const double ft = std::max(1.8, bh * 0.15);    // frame thickness
  const double ecx = eC * 0.6;                   // cavity end-cap bulge
  const double ix = bx + ft, iy = by + ft;
  const double iw = bw - 2.0 * ft, ih = bh - 2.0 * ft;
  hcyl(ix, iy, iw, ih, ecx);
  cr->set_source_rgba(0.11, 0.11, 0.13, 1.0);    // dark cavity
  cr->fill();

  const double fw = iw * lvl;
  // low state: paint the empty remainder deep red, so a thin sliver still reads
  // as critically low (two-tone: bright red charge left, deep red void right)
  if (bst == BattState::Crit && fw < iw) {
    cr->save();
    hcyl(ix, iy, iw, ih, ecx);
    cr->clip();
    cr->rectangle(ix + fw, iy - 1.0, iw - fw + ecx + 1.0, ih + 2.0);
    cr->set_source_rgba(0.34, 0.05, 0.05, 0.95);
    cr->fill();
    cr->restore();
  }
  // charge fill: clipped to the cavity (so it takes the rounded end caps), a
  // glossy vertical gradient with a top highlight and a brighter leading edge
  if (fw > 0.5) {
    cr->save();
    hcyl(ix, iy, iw, ih, ecx);
    cr->clip();
    auto fill = Cairo::LinearGradient::create(0.0, iy, 0.0, iy + ih);
    fill->add_color_stop_rgba(0.0, std::min(1.0, r * 1.45 + 0.18),
                                   std::min(1.0, g * 1.45 + 0.18),
                                   std::min(1.0, b * 1.45 + 0.18), 1.0);
    fill->add_color_stop_rgba(0.45, r, g, b, 1.0);
    fill->add_color_stop_rgba(1.0, r * 0.62, g * 0.62, b * 0.62, 1.0);
    cr->rectangle(ix - ecx - 1.0, iy - 1.0, fw + ecx + 1.0, ih + 2.0);
    cr->set_source(fill);
    cr->fill();
    cr->rectangle(ix - ecx, iy + ih * 0.12, fw + ecx, ih * 0.24);   // gloss
    cr->set_source_rgba(1.0, 1.0, 1.0, 0.20);
    cr->fill();
    if (fw > 3.0) {                                     // brighter leading edge
      cr->rectangle(ix + fw - 1.6, iy - 1.0, 1.6, ih + 2.0);
      cr->set_source_rgba(std::min(1.0, r * 1.5 + 0.2),
                          std::min(1.0, g * 1.5 + 0.2),
                          std::min(1.0, b * 1.5 + 0.2), 0.85);
      cr->fill();
    }
    cr->restore();
  }

  // charging bolt on the cell (the OUTSIDE halo above carries the glow now):
  // bright yellow with a thin 1px dark outline.
  if (plugged_) {
    const double mx = bx + bw / 2.0, my = by + bh / 2.0;
    const double s = bh * 0.72;
    auto bolt = [&]() {
      cr->begin_new_path();
      cr->move_to(mx - s * 1.05, my - s * 0.15);
      cr->line_to(mx + s * 0.15, my + s * 0.55);
      cr->line_to(mx + s * 0.15, my + s * 0.05);
      cr->line_to(mx + s * 1.05, my + s * 0.15);
      cr->line_to(mx - s * 0.15, my - s * 0.55);
      cr->line_to(mx - s * 0.15, my - s * 0.05);
      cr->close_path();
    };
    cr->set_line_join(Cairo::LINE_JOIN_ROUND);
    bolt();
    cr->set_source_rgba(1.0, 0.92, 0.20, 1.0);     // bright yellow fill
    cr->fill_preserve();
    cr->set_line_width(1.0);                        // thin 1px outline
    cr->set_source_rgba(0.0, 0.0, 0.0, 0.9);
    cr->stroke();
  }
}

// An industrial pressure-gauge dial (a locomotive / boiler-room instrument): a
// brass bezel, an aged-ivory face with a tick ring and a red danger zone, and a
// swinging needle reading the CPU package temperature. The scale sweeps 270 deg
// with the gap at the bottom, where a small digital readout sits.
void Gauge::draw_dial(const Cairo::RefPtr<Cairo::Context>& cr, double cx,
                      double cy, double R) {
  const double a_min = 0.75 * kPi;     // lower-left (~7:30), y-down
  const double sweep = 1.5 * kPi;      // 270 deg clockwise, gap at the bottom
  const double a_max = a_min + sweep;
  auto ang = [&](double f) { return a_min + f * sweep; };
  const double rf = R * 0.80;          // face radius (inside the bezel)

  // Housing theme -- the ONLY thing differing between the dials. Mnemonic:
  // CPU is COLD (gunmetal bezel + slate face) with a blue chip; GPU is GOLD
  // (brass bezel + aged-brown face -- and its square mounting plate turns brass
  // too) with the nvidia-green fan glyph. The naked round CPU dial suits bare
  // steel; the bracket-mounted GPU wears the warmer brass. The green/amber/red
  // band and the white needle are identical, so they read as a matched pair.
  double bz0r, bz0g, bz0b, bz1r, bz1g, bz1b, bz2r, bz2g, bz2b;  // bezel
  double fc0r, fc0g, fc0b, fc1r, fc1g, fc1b;   // face centre..edge
  double hb0r, hb0g, hb0b, hb1r, hb1g, hb1b;                    // hub
  double ar, ag, ab;                                           // accent (icon)
  if (gpu_) {
    bz0r = 0.88; bz0g = 0.74; bz0b = 0.42;   // brass (GOLD)
    bz1r = 0.55; bz1g = 0.44; bz1b = 0.20;
    bz2r = 0.30; bz2g = 0.23; bz2b = 0.10;
    fc0r = 0.50; fc0g = 0.40; fc0b = 0.27;   // aged brown face
    fc1r = 0.30; fc1g = 0.23; fc1b = 0.14;
    hb0r = 0.97; hb0g = 0.87; hb0b = 0.57;
    hb1r = 0.40; hb1g = 0.31; hb1b = 0.13;
    ar = 0.53; ag = 0.85; ab = 0.05;         // nvidia green
  } else {
    bz0r = 0.74; bz0g = 0.78; bz0b = 0.84;   // blued gunmetal (COLD steel)
    bz1r = 0.42; bz1g = 0.46; bz1b = 0.52;
    bz2r = 0.17; bz2g = 0.20; bz2b = 0.25;
    fc0r = 0.33; fc0g = 0.37; fc0b = 0.44;   // cool slate face
    fc1r = 0.16; fc1g = 0.19; fc1b = 0.24;
    hb0r = 0.84; hb0g = 0.88; hb0b = 0.94;
    hb1r = 0.26; hb1g = 0.30; hb1b = 0.36;
    ar = 0.40; ag = 0.76; ab = 1.0;          // cool blue
  }

  // GPU only: a square brushed-steel MOUNTING PLATE the round dial sits on, its
  // corners filling the dead space around the bezel so the pair reads as a
  // bracket-mounted GPU sensor vs the CPU's bare round dial. The dial keeps its
  // full size (plate only fractionally wider than bezel); the lighting is
  // deliberately SUBTLE -- a soft top-lit face and a faint chamfer -- so the
  // dial stays the star. The corner bosses below bolt it down. Drawn first.
  if (gpu_) {
    const double hp = R * 1.06;
    const double x0 = cx - hp, y0 = cy - hp, s = 2.0 * hp;
    const double edge = std::max(1.0, R * 0.05);

    cr->rectangle(x0, y0 + R * 0.05, s, s);              // soft drop shadow
    cr->set_source_rgba(0, 0, 0, 0.22);
    cr->fill();

    auto pl = Cairo::LinearGradient::create(cx, y0, cx, y0 + s);   // steel face
    pl->add_color_stop_rgba(0.0, bz1r + 0.03, bz1g + 0.03, bz1b + 0.03, 1.0);
    pl->add_color_stop_rgba(1.0, bz2r + 0.03, bz2g + 0.03, bz2b + 0.04, 1.0);
    cr->rectangle(x0, y0, s, s);
    cr->set_source(pl);
    cr->fill();

    cr->set_line_join(Cairo::LINE_JOIN_MITER);           // faint raised chamfer
    cr->set_line_width(edge);
    const double q = edge * 0.5;
    cr->move_to(x0 + q, y0 + s - q);
    cr->line_to(x0 + q, y0 + q);
    cr->line_to(x0 + s - q, y0 + q);
    cr->set_source_rgba(bz0r, bz0g, bz0b, 0.22);         // top/left sheen
    cr->stroke();
    cr->move_to(x0 + s - q, y0 + q);
    cr->line_to(x0 + s - q, y0 + s - q);
    cr->line_to(x0 + q, y0 + s - q);
    cr->set_source_rgba(0.10, 0.11, 0.13, 0.26);         // soft bottom/right
    cr->stroke();
  }

  // GPU only: 4 bolted mounting bosses in the corners (on the plate, behind the
  // round bezel) -- the flanges that bracket the instrument down, in the dead
  // corner space. A bolt hole + top glint gives each one depth.
  if (gpu_) {
    for (int i = 0; i < 4; i++) {
      const double fa = kPi * 0.25 + i * kPi * 0.5;   // 45 / 135 / 225 / 315
      const double fx = cx + std::cos(fa) * R * 1.06;
      const double fy = cy + std::sin(fa) * R * 1.06;
      const double fr = R * 0.22;
      cr->arc(fx, fy + R * 0.03, fr, 0, 2 * kPi);      // boss shadow
      cr->set_source_rgba(0, 0, 0, 0.30);
      cr->fill();
      auto bp = Cairo::LinearGradient::create(fx, fy - fr, fx, fy + fr);
      bp->add_color_stop_rgba(0.0, bz0r, bz0g, bz0b, 1.0);   // gunmetal top
      bp->add_color_stop_rgba(1.0, bz2r, bz2g, bz2b, 1.0);
      cr->arc(fx, fy, fr, 0, 2 * kPi);
      cr->set_source(bp);
      cr->fill();
      cr->arc(fx, fy, fr * 0.44, 0, 2 * kPi);          // recessed bolt hole
      cr->set_source_rgba(0.05, 0.06, 0.08, 1.0);
      cr->fill();
      cr->arc(fx - fr * 0.12, fy - fr * 0.12, fr * 0.16, 0, 2 * kPi);   // glint
      cr->set_source_rgba(0.80, 0.85, 0.92, 0.55);
      cr->fill();
    }
  }

  // soft drop shadow -- the instrument sits raised on the bar
  cr->arc(cx, cy + R * 0.07, R, 0, 2 * kPi);
  cr->set_source_rgba(0, 0, 0, 0.35);
  cr->fill();

  // bezel: a metal ring lit from above (brass or gunmetal per the theme)
  auto bez = Cairo::LinearGradient::create(cx, cy - R, cx, cy + R);
  bez->add_color_stop_rgba(0.0, bz0r, bz0g, bz0b, 1.0);
  bez->add_color_stop_rgba(0.5, bz1r, bz1g, bz1b, 1.0);
  bez->add_color_stop_rgba(1.0, bz2r, bz2g, bz2b, 1.0);
  cr->arc(cx, cy, R, 0, 2 * kPi);
  cr->set_source(bez);
  cr->fill_preserve();
  cr->set_line_width(1.0);
  cr->set_source_rgba(0.10, 0.08, 0.04, 0.9);
  cr->stroke();

  // dark face with an edge vignette (aged brown or cool slate per the theme) --
  // dark enough that the zone band and the white-keyed needle/text really pop.
  auto face = Cairo::RadialGradient::create(cx, cy - rf * 0.2, rf * 0.2, cx, cy,
                                            rf);
  face->add_color_stop_rgba(0.0, fc0r, fc0g, fc0b, 1.0);
  face->add_color_stop_rgba(1.0, fc1r, fc1g, fc1b, 1.0);
  cr->arc(cx, cy, rf, 0, 2 * kPi);
  cr->set_source(face);
  cr->fill();

  // GPU only: faint concentric machined rings -- a turned-metal face vs the
  // CPU's matte parchment. Kept low-alpha and inside the tick ring so it never
  // competes with the band, needle, or readout.
  if (gpu_) {
    cr->set_line_width(std::max(0.5, R * 0.012));
    cr->set_source_rgba(fc0r + 0.12, fc0g + 0.12, fc0b + 0.13, 0.30);
    for (int i = 1; i <= 4; i++) {
      cr->arc(cx, cy, rf * (0.16 + i * 0.13), 0, 2 * kPi);
      cr->stroke();
    }
  }

  // etched domain glyph on the upper face (drawn UNDER the needle): a chip
  // (CPU) or a fan (GPU), in the accent colour -- the identity mark that pairs
  // with each metric's graph colour.
  {
    const double ix = cx, iy = cy - rf * 0.46, s = rf * 0.26;
    cr->set_source_rgba(ar, ag, ab, 0.62);
    cr->set_line_width(std::max(0.8, R * 0.03));
    cr->set_line_join(Cairo::LINE_JOIN_ROUND);
    if (gpu_) {
      cr->arc(ix, iy, s, 0, 2 * kPi);                 // fan shroud
      cr->stroke();
      for (int i = 0; i < 3; i++) {                   // 3 curved blades
        const double a0 = i * 2.0 * kPi / 3.0;
        cr->move_to(ix + s * 0.2 * std::cos(a0), iy + s * 0.2 * std::sin(a0));
        cr->curve_to(ix + s * 0.55 * std::cos(a0), iy + s * 0.55 * std::sin(a0),
                     ix + s * 0.75 * std::cos(a0 + 1.1),
                     iy + s * 0.75 * std::sin(a0 + 1.1),
                     ix + s * 0.82 * std::cos(a0 + 1.5),
                     iy + s * 0.82 * std::sin(a0 + 1.5));
        cr->stroke();
      }
      cr->arc(ix, iy, s * 0.18, 0, 2 * kPi);          // hub
      cr->fill();
    } else {
      cr->rectangle(ix - s * 0.7, iy - s * 0.7, s * 1.4, s * 1.4);   // chip die
      cr->stroke();
      for (int i = -1; i <= 1; i++) {                 // side pins
        const double py = iy + i * s * 0.5, pxv = ix + i * s * 0.5;
        cr->move_to(ix - s * 0.7, py); cr->line_to(ix - s * 1.05, py);
        cr->move_to(ix + s * 0.7, py); cr->line_to(ix + s * 1.05, py);
        cr->move_to(pxv, iy - s * 0.7); cr->line_to(pxv, iy - s * 1.05);
        cr->move_to(pxv, iy + s * 0.7); cr->line_to(pxv, iy + s * 1.05);
      }
      cr->stroke();
    }
  }

  // FAT green / amber / red zone band -- the at-a-glance scale. Which zone the
  // needle sits in reads instantly at any size, where fine ticks turn to mush.
  auto clampf = [](double x) { return x < 0 ? 0.0 : (x > 1 ? 1.0 : x); };
  const double f_warm =
      clampf((temp_warm_ - temp_min_) / (temp_max_ - temp_min_));
  const double f_hot =
      clampf((temp_hot_ - temp_min_) / (temp_max_ - temp_min_));
  const double band_r = rf * 0.80;
  cr->set_line_cap(Cairo::LINE_CAP_BUTT);
  cr->set_line_width(std::max(3.0, R * 0.20));
  cr->set_source_rgba(0.29, 0.68, 0.33, 1.0);          // green (cool)
  cr->arc(cx, cy, band_r, ang(0.0), ang(f_warm));
  cr->stroke();
  cr->set_source_rgba(0.97, 0.73, 0.13, 1.0);          // amber (warm)
  cr->arc(cx, cy, band_r, ang(f_warm), ang(f_hot));
  cr->stroke();
  cr->set_source_rgba(0.87, 0.16, 0.12, 1.0);          // red (hot)
  cr->arc(cx, cy, band_r, ang(f_hot), a_max);
  cr->stroke();

  // a few BOLD major ticks just inside the band (cream, so they read on the
  // dark face; not a fine ring)
  cr->set_source_rgba(0.88, 0.82, 0.70, 0.85);
  cr->set_line_width(std::max(1.4, R * 0.05));
  for (int i = 0; i <= 4; i++) {
    const double a = ang(i / 4.0);
    const double r0 = rf * 0.55, r1 = rf * 0.68;
    cr->move_to(cx + r0 * std::cos(a), cy + r0 * std::sin(a));
    cr->line_to(cx + r1 * std::cos(a), cy + r1 * std::sin(a));
    cr->stroke();
  }

  // EXAGGERATED needle: a fat tapered pointer with a round counterweight on the
  // far side, wrapped in a white keyline so it pops over any zone colour.
  const double a = ang(level_), nx = std::cos(a), ny = std::sin(a);
  const double px = -ny, py = nx;      // unit perpendicular
  const double tip = rf * 0.94, tail = rf * 0.26, hw = std::max(2.6, R * 0.15);
  const double cw = rf * 0.30, cwr = std::max(1.8, R * 0.13);   // counterweight
  auto needle_shape = [&] {
    cr->move_to(cx + nx * tip, cy + ny * tip);
    cr->line_to(cx + px * hw - nx * tail, cy + py * hw - ny * tail);
    cr->line_to(cx - px * hw - nx * tail, cy - py * hw - ny * tail);
    cr->close_path();
    cr->begin_new_sub_path();
    cr->arc(cx - nx * cw, cy - ny * cw, cwr, 0, 2 * kPi);
  };
  cr->save();                          // soft drop shadow lifts it off the face
  cr->translate(R * 0.04, R * 0.07);
  needle_shape();
  cr->set_source_rgba(0, 0, 0, 0.38);
  cr->fill();
  cr->restore();
  needle_shape();                      // dark keyline for a hard edge
  cr->set_line_join(Cairo::LINE_JOIN_ROUND);
  cr->set_line_width(std::max(1.8, R * 0.08));
  cr->set_source_rgba(0.05, 0.05, 0.07, 0.92);
  cr->stroke();
  needle_shape();                      // bright WHITE fill
  cr->set_source_rgba(0.98, 0.98, 1.0, 1.0);
  cr->fill();

  // hub cap over the needle base (metal per the theme)
  auto hub = Cairo::RadialGradient::create(cx - R * 0.03, cy - R * 0.03,
                                           R * 0.02, cx, cy, R * 0.18);
  hub->add_color_stop_rgba(0.0, hb0r, hb0g, hb0b, 1.0);
  hub->add_color_stop_rgba(1.0, hb1r, hb1g, hb1b, 1.0);
  cr->arc(cx, cy, R * 0.15, 0, 2 * kPi);
  cr->set_source(hub);
  cr->fill();

  // Fahrenheit readout, sitting LOW so it overlays the bottom lip (not inside
  // the face). Coloured by the current zone -- so the number reinforces the
  // state and does not fight the white needle -- with a heavy black
  // outline so it reads over the brass lip and the wallpaper beyond.
  char buf[16];
  std::snprintf(buf, sizeof buf, "%.0f", temp_c_ * 9.0 / 5.0 + 32.0);
  cr->select_font_face("sans-serif", Cairo::FONT_SLANT_NORMAL,
                       Cairo::FONT_WEIGHT_BOLD);
  double fs = std::max(9.0, R * 0.64);
  cr->set_font_size(fs);
  Cairo::TextExtents te;
  cr->get_text_extents(buf, te);
  if (te.width > R * 1.5 && te.width > 1.0) {   // fit a 3-digit F value
    fs *= R * 1.5 / te.width;
    cr->set_font_size(fs);
    cr->get_text_extents(buf, te);
  }
  double zr, zg, zb;
  if (temp_c_ >= temp_hot_) { zr = 0.96; zg = 0.30; zb = 0.24; }        // red
  else if (temp_c_ >= temp_warm_) { zr = 0.99; zg = 0.80; zb = 0.26; }  // amber
  else { zr = 0.44; zg = 0.84; zb = 0.46; }                            // green
  const double bx = cx - te.width / 2.0 - te.x_bearing, by = cy + R * 0.94;
  cr->move_to(bx, by);                 // heavy black outline
  cr->text_path(buf);
  cr->set_line_join(Cairo::LINE_JOIN_ROUND);
  cr->set_line_width(std::max(2.2, R * 0.12));
  cr->set_source_rgba(0, 0, 0, 0.95);
  cr->stroke();
  cr->move_to(bx, by);                 // zone-coloured fill
  cr->set_source_rgba(zr, zg, zb, 1.0);
  cr->show_text(buf);
}

void Gauge::draw_text(const Cairo::RefPtr<Cairo::Context>& cr,
                      const std::string& s, double cx, double baseline,
                      double px, double fr, double fg, double fb) {
  cr->select_font_face("sans-serif", Cairo::FONT_SLANT_NORMAL,
                       Cairo::FONT_WEIGHT_BOLD);
  cr->set_font_size(px);
  Cairo::TextExtents te;
  cr->get_text_extents(s, te);
  double tx = cx - (te.width / 2.0 + te.x_bearing);
  cr->move_to(tx, baseline);
  cr->text_path(s);
  cr->set_line_join(Cairo::LINE_JOIN_ROUND);
  cr->set_line_width(2.5);
  cr->set_source_rgba(0.0, 0.0, 0.0, 0.85);
  cr->stroke_preserve();
  cr->set_source_rgba(fr, fg, fb, 1.0);
  cr->fill();
}

bool Gauge::on_draw(const Cairo::RefPtr<Cairo::Context>& cr) {
  const Gtk::Allocation a = area_.get_allocation();
  const double w = a.get_width(), h = a.get_height();
  if (w <= 0 || h <= 0) return true;

  cr->set_antialias(Cairo::ANTIALIAS_DEFAULT);
  auto sc = event_box_.get_style_context();
  const Gdk::RGBA col = sc->get_color(sc->get_state());
  double r = col.get_red(), g = col.get_green(), b = col.get_blue();

  // brightness/volume carry no on-widget number (the value lives in the hover
  // popup); only the battery shows a % -- underneath a full-width graphic.
  if (kind_ == Kind::Brightness) {
    if (bulb_style_)
      // nudged a touch below centre: the bulb reads better with more room above
      // it (the glow) than below (the screw base)
      draw_bulb(cr, w / 2.0, h / 2.0 + std::min(w, h) * 0.05, std::min(w, h) *
                0.39, r, g, b);
    else
      draw_sun(cr, w / 2.0, h / 2.0, std::min(w, h) * 0.34, r, g, b);
  } else if (kind_ == Kind::Temp) {
    draw_dial(cr, w / 2.0, h / 2.0, std::min(w, h) * 0.43);
  } else if (kind_ == Kind::Volume) {
    draw_ring(cr, w / 2.0, h / 2.0, std::min(w, h) * 0.40, r, g, b);
    if (muted_) {
      // "MUTED" fit to the width, black-outlined and HUGGING THE BOTTOM line --
      // the shared readout idiom (battery %, the temp dial, the cal card).
      cr->select_font_face("sans-serif", Cairo::FONT_SLANT_NORMAL,
                           Cairo::FONT_WEIGHT_BOLD);
      cr->set_font_size(10.0);
      Cairo::TextExtents te;
      cr->get_text_extents("MUTED", te);
      double fit = te.width > 1.0 ? 10.0 * (w - 3.0) / te.width : font_px_;
      // baseline mirrors the dial readout (cy + R*0.94) so it carries the same
      // breathing room under it, not jammed against the bar's bottom edge
      const double mby = h / 2.0 + std::min(w, h) * 0.40;
      draw_text(cr, "MUTED", w / 2.0, mby, fit, 0.95, 0.20, 0.20);
    }
  } else {  // battery: full-width graphic on top, % tucked just beneath
    char pctbuf[8];
    const int pctval = static_cast<int>(level_ * 100 + 0.5);
    std::snprintf(pctbuf, sizeof pctbuf, "%d", pctval);
    const std::string pctstr = std::string(pctbuf) + "%";

    // A bigger % readout, fit to the widget width so a wide "100%" auto-shrinks
    // while the common two-digit values render at the full size.
    const double npx = font_px_ + 9.0;
    cr->select_font_face("sans-serif", Cairo::FONT_SLANT_NORMAL,
                         Cairo::FONT_WEIGHT_BOLD);
    cr->set_font_size(npx);
    Cairo::TextExtents te;
    cr->get_text_extents(pctstr, te);
    const double avail = w - 4.0;
    const double fit =
        (te.width > avail && te.width > 1.0) ? npx * avail / te.width : npx;
    cr->set_font_size(fit);
    cr->get_text_extents("0", te);
    const double cap = -te.y_bearing;   // digit ascent above the baseline

    // Layout: the % baseline sits at h - bot_edge and the icon height is fixed;
    // the icon rides high with only a small top buffer, so the icon-to-text gap
    // opens up by eating into the top rather than pushing the text down.
    const double top_edge = 1.0, bot_edge = 3.0, gap = 3.5;
    const double baseline = h - bot_edge;
    const double icon_h = baseline - cap - gap - top_edge;
    draw_battery(cr, 2.0, top_edge, w - 4.0, icon_h);

    // Colour aligned with the battery body (batt_color: red in the crit tier,
    // amber in the warn tier), diverging only mid-range: the text goes white
    // from just above batt-warn up to 80, then green 81+, while the body stays
    // green the whole way above batt-warn. The 80 is its own choice, not a
    // tier boundary, so it is left literal.
    double tr, tg, tb;
    if (level_ > batt_warn_ && pctval <= 80) { tr = tg = tb = 0.96; }  // white
    else batt_color(tr, tg, tb);                        // red/amber/green

    draw_text(cr, pctstr, w / 2.0, baseline, fit, tr, tg, tb);
  }
  return true;
}

}  // namespace waybar::modules::hw
