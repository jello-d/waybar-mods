#pragma once

#include <gio/gio.h>
#include <gtkmm/drawingarea.h>
#include <cairomm/context.h>
#include <json/json.h>

#include <optional>
#include <string>

#include "AModule.hpp"
#include "bar.hpp"

namespace waybar::modules::hw {

// A Cairo-drawn Bluetooth indicator (no font glyphs), the sibling of hw/gauge.
// Draws the Bluetooth rune -- coloured by connection state (grey off, dim blue
// on, bright blue connected) -- and, when the connected device reports battery
// over BlueZ (Battery1), a sleek vertical battery that drains with the level,
// with the % overlaid across both at the bottom (black-outlined, like the temp
// dial's readout). Reads BlueZ over D-Bus via a GDBusObjectManager with cached
// properties -- no fork, no poll of an external tool. Collapses with no
// controller. Registered as `hw/bluetooth`.
class Bluetooth final : public waybar::AModule {
 public:
  Bluetooth(const std::string& id, const waybar::Bar& bar,
            const Json::Value& config);
  ~Bluetooth();

  auto update() -> void override;

 private:
  const waybar::Bar& bar_;
  int width_ = 34;             // widget width WITH battery (px, pre-scale)
  int width_narrow_ = 20;      // rune-only width, no battery (derived)
  int vmargin_ = 3, hmargin_ = 4;

  Gtk::DrawingArea area_;
  GDBusObjectManager* manager_ = nullptr;
  gulong sig_add_ = 0, sig_del_ = 0, sig_chg_ = 0;

  // live state, read from BlueZ cached properties
  bool have_adapter_ = false;
  bool powered_ = false;
  bool connected_ = false;
  std::optional<int> battery_;   // 0..100 when the connected device reports it
  std::string tip_;

  void read_state();   // recompute state from the manager's cached properties
  bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr);
  void draw_rune(const Cairo::RefPtr<Cairo::Context>& cr, double cx, double cy,
                 double h, double r, double g, double b);
  void draw_battery(const Cairo::RefPtr<Cairo::Context>& cr, double cx,
                    double cy, double h, double level);
};

}  // namespace waybar::modules::hw
