#pragma once

#include <gtkmm/drawingarea.h>
#include <cairomm/context.h>
#include <json/json.h>

#include <string>

#include "AModule.hpp"
#include "bar.hpp"

namespace waybar::modules::analog {

// A small analog clock drawn in Cairo, in the sysmon/gauge "instrument" idiom:
// an inset dark face with a bevelled rim, hour ticks, and hour + minute hands
// (white with a dark outline so they read on any wallpaper). No second hand --
// so it only repaints on the minute and costs a single slow timer. Meant to sit
// beside the digital date/time. Registered as `clock/analog`.
class Clock final : public waybar::AModule {
 public:
  Clock(const std::string& id, const waybar::Bar& bar,
        const Json::Value& config);
  ~Clock();

  auto update() -> void override;

 private:
  const waybar::Bar& bar_;

  int size_ = 44;       // face box (px)
  int vmargin_ = 4;     // bar padding above/below (face breathing room)
  int hmargin_l_ = 12;  // breathing toward the neighbour before it (sysmon)
  int hmargin_r_ = 0;   // tight toward the digital clock (read as one set)

  Gtk::DrawingArea area_;
  sigc::connection timer_;
  int last_min_ = -1;

  bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr);
  bool on_timer();
};

}  // namespace waybar::modules::analog
