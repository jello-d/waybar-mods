#pragma once

#include <gtkmm/drawingarea.h>
#include <cairomm/context.h>
#include <gdkmm/pixbuf.h>
#include <json/json.h>

#include <string>

#include "AModule.hpp"
#include "bar.hpp"

namespace waybar::modules::cal {

// Fixed natural width so the card claims a steady slot in the aux lane; the
// draw ellipsizes its text to whatever it is allocated.
class CardArea : public Gtk::DrawingArea {
 public:
  int nat_w = 300;

 protected:
  void get_preferred_width_vfunc(int& minimum, int& natural) const override {
    minimum = nat_w;
    natural = nat_w;
  }
};

// The next-meeting card, drawn in Cairo: the Google Calendar icon on the left
// with the LIVE countdown overlaid at its base (black-outlined, like the dial
// readouts), and two text lines to its right -- the start time + meeting name
// (+ attendee count in parens), then the local room. Reads ONLY the tmpfs
// summary corp-cal-fetch writes (no credential, no network); recomputes the
// countdown from the start epoch each tick. Collapses when nothing is upcoming.
// Private-visibility events redact the title to "Busy" and drop the room.
// Registered as `cal/card`.
class Card final : public waybar::AModule {
 public:
  Card(const std::string& id, const waybar::Bar& bar,
       const Json::Value& config);
  ~Card();

  auto update() -> void override;

 private:
  const waybar::Bar& bar_;

  int width_ = 300;      // card width (px, pre-scale)
  int vmargin_ = 3, hmargin_ = 6;
  double ui_scale_ = 1.0;      // bar_height / 48; scales the text on the wall
  std::string state_path_;     // corp-cal.json (tmpfs, fetcher-published)
  std::string icon_path_;      // the gcal png
  bool show_private_ = false;  // reveal private titles/rooms (default: redact)
  int max_days_ = 3;           // horizon: hide an event more than N days out

  CardArea area_;
  sigc::connection poll_;

  // parsed state
  bool have_ = false;
  std::string title_, time_, room_, visibility_;
  int attendees_ = 0;
  long start_epoch_ = 0;

  // icon decode cache
  std::string icon_loaded_;
  Glib::RefPtr<Gdk::Pixbuf> icon_;

  bool readState();
  void ensureIcon();
  bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr);
};

}  // namespace waybar::modules::cal
