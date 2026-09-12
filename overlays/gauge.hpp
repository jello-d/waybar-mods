#pragma once

#include <gtkmm/drawingarea.h>
#include <cairomm/context.h>
#include <json/json.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "AModule.hpp"
#include "bar.hpp"
#include "util/audio_backend.hpp"

namespace waybar::modules::hw {

// One Cairo-drawn hardware gauge. One class backs three metrics; the bar
// instances it per metric via the factory's name#id split (hw/gauge#brightness,
// hw/gauge#volume, hw/gauge#battery), each carrying its own `kind`. No font
// glyphs: every icon is drawn (a sun, a volume ring, a filling battery), so
// state channels stay independent -- e.g. battery charge (fill + colour) and
// charging (a bolt overlay) never share one glyph.
//
// The battery's charge TIERS are user-set ("batt-warn" / "batt-crit", in
// percent) and each carries an optional hook ("on-warn" / "on-crit" /
// "on-normal") spawned when the charge crosses INTO that tier -- so a consumer
// wires a notification off the bar's existing poll instead of running its own.
//
// Data is read without a poll-driven fork: volume comes from libpulse
// (util::AudioBackend, event-driven); brightness and battery are tiny /sys
// reads on a slow timer (plus an immediate re-read after a scroll). The numeric
// readout is a transient overlay -- brightness/volume show their % only while
// changing, then hide; battery always shows.
class Gauge final : public waybar::AModule {
 public:
  Gauge(const std::string& id, const waybar::Bar& bar,
        const Json::Value& config);
  ~Gauge();

  auto update() -> void override;

 private:
  enum class Kind { Brightness, Volume, Battery, Temp };

  // Battery charge tiers: Normal above batt-warn, Warn down to batt-crit, Crit
  // at or below it. One enum drives BOTH the colour the cell is drawn in and
  // the state-crossing hooks, so the two can never disagree.
  enum class BattState { Normal, Warn, Crit };

  const waybar::Bar& bar_;
  Kind kind_ = Kind::Battery;

  // config
  int width_ = 0;               // 0 => per-kind default
  double font_px_ = 11.0;
  int vmargin_ = 4;
  int hmargin_ = 1;   // per-side gap to neighbours; small so the gauges tuck up
  int interval_ms_ = 5000;      // sysfs poll (brightness/battery)
  double scroll_step_ = 5.0;
  bool bulb_style_ = false;     // brightness: light-bulb idiom instead of a sun
  int popup_top_ = 0;           // popup gap below the bar (0 = flush; the bar's
                                // exclusive zone offsets it past the bar)

  Gtk::DrawingArea area_;

  // live state (level normalised to 0..1)
  double level_ = 0.0;
  bool muted_ = false;                         // volume
  bool charging_ = false, plugged_ = false;    // battery
  std::string status_;
  double health_ = 1.0;         // battery wear (energy_full / design)
  double power_w_ = 0.0;
  int time_min_ = -1;           // battery time-to (minutes; -1 unknown)
  int cycles_ = -1;

  // battery: the two user-set tier boundaries (config "batt-warn"/"batt-crit",
  // given in PERCENT, held here as a 0..1 fraction to match level_). Defaults
  // are the tiers this gauge drew before they were configurable. Crossing INTO
  // a tier spawns that tier's hook -- config "on-warn" / "on-crit" /
  // "on-normal", each optional: an undefined one fires nothing.
  double batt_warn_ = 0.40;     // green -> amber boundary
  double batt_crit_ = 0.20;     // amber -> red boundary
  BattState batt_state_ = BattState::Normal;
  bool batt_primed_ = false;    // first reading arms the tier without firing

  std::shared_ptr<util::AudioBackend> audio_;   // volume only
  std::string bl_dir_;          // backlight sysfs dir (brightness)
  std::string bat_dir_;         // battery sysfs dir

  // temp: an industrial pressure-gauge dial reading a CPU package sensor, or --
  // with "source":"gpu" -- the Nvidia GPU via NVML. gpu_ also picks the housing
  // theme: brass + brown + a blue chip (CPU) vs gunmetal + slate + a green
  // fan glyph (GPU), so the two dials read as a matched pair, distinct at a
  // glance. The green/amber/red band + white needle stay identical (universal
  // hot/cool).
  bool gpu_ = false;            // temp source: GPU (NVML) or CPU (hwmon)
  std::string temp_path_;       // hwmon tempN_input (millidegrees C), CPU only
  double temp_c_ = 0.0;         // last reading, degrees C
  double temp_min_ = 20.0;      // dial scale bounds
  double temp_max_ = 100.0;
  double temp_warm_ = 55.0;     // green -> amber zone boundary
  double temp_hot_ = 85.0;      // amber -> red (danger) boundary + needle alarm
  bool available_ = true;       // false => no backend (hidden, drawn empty)

  // Brightness fallback for a box with no kernel backlight (a desktop driving
  // external monitors): DDC/CI over each monitor's I2C side-channel, all
  // monitors driven in lockstep. Populated by an async probe (probe_ddc).
  bool ddc_ = false;
  std::vector<int> ddc_buses_;      // i2c bus numbers of brightness monitors
  double ddc_pending_val_ = 0.0;    // latest target awaiting a throttled flush
  bool ddc_flush_scheduled_ = false;

  // battery detail popup: a gtk-layer-shell overlay window anchored just under
  // the bar (GTK tooltips float near the pointer; popovers don't render on a
  // layer-shell bar -- so we draw our own).
  std::string tip_text_;   // readout published as a GTK tooltip
  bool hovered_ = false;

  using Clock = std::chrono::steady_clock;
  sigc::connection timer_;

  // drag to set (volume/brightness): up/right raises, down/left lowers
  bool drag_armed_ = false;
  bool dragging_ = false;
  double drag_x0_ = 0.0, drag_y0_ = 0.0, drag_l0_ = 0.0;
  Clock::time_point last_osd_t_{};   // throttle the OSD ping during a drag
  Clock::time_point last_ddc_t_{};   // throttle DDC setvcp writes during a drag

  bool on_timer();
  bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr);
  bool on_press(GdkEventButton* e);
  bool on_release(GdkEventButton* e);
  bool on_motion(GdkEventMotion* e);
  bool handleScroll(GdkEventScroll* e) override;
  void apply_level(double lvl);   // set volume/brightness natively
  void osd(const std::string& arg);  // fire swayosd (the keybind's OSD path)

  void probe_ddc();               // async DDC detect -> reveal + prime level
  void ddc_set(double lvl);       // throttled brightness write to all monitors
  void ddc_flush();               // push ddc_pending_val_ to every monitor

  void read_batt_config();        // parse + validate the tier boundaries
  void read_brightness();
  void read_battery();
  void read_volume();
  void read_temp();
  void update_tooltip();

  BattState batt_state_for(double lvl) const;
  void check_batt_state();        // fire a hook on a tier crossing
  void batt_color(double& r, double& g, double& b) const;
  void draw_sun(const Cairo::RefPtr<Cairo::Context>& cr, double cx, double cy,
                double rad, double r, double g, double b);
  void draw_bulb(const Cairo::RefPtr<Cairo::Context>& cr, double cx, double cy,
                 double rad, double r, double g, double b);
  void draw_ring(const Cairo::RefPtr<Cairo::Context>& cr, double cx, double cy,
                 double rad, double r, double g, double b);
  void draw_battery(const Cairo::RefPtr<Cairo::Context>& cr, double x, double y,
                    double w, double h);
  void draw_dial(const Cairo::RefPtr<Cairo::Context>& cr, double cx, double cy,
                 double R);
  void draw_text(const Cairo::RefPtr<Cairo::Context>& cr, const std::string& s,
                 double cx, double baseline, double px, double fr = 1.0,
                 double fg = 1.0, double fb = 1.0);
};

}  // namespace waybar::modules::hw
