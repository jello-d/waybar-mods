#include "modules/analog/clock.hpp"

#include <cairomm/context.h>
#include <glibmm/main.h>

#include <cmath>
#include <ctime>

namespace waybar::modules::analog {

static constexpr double kPi = 3.14159265358979323846;

Clock::Clock(const std::string& id, const waybar::Bar& bar,
             const Json::Value& config)
    : AModule(config, "analog-clock", id, false, false), bar_(bar) {
  if (config_["size"].isInt()) size_ = config_["size"].asInt();
  if (config_["vmargin"].isInt()) vmargin_ = config_["vmargin"].asInt();
  if (config_["hmargin-left"].isInt())
    hmargin_l_ = config_["hmargin-left"].asInt();
  if (config_["hmargin-right"].isInt())
    hmargin_r_ = config_["hmargin-right"].asInt();

  // Scale the face with the BAR height (48 = design): the size_ is the face box
  // width, so a taller wall bar grows the clock with no per-bar config. The
  // draw already fits the face to min(w, h). Base 48px bar is a strict no-op.
  const double ui = bar_.config["height"].isInt()
                        ? bar_.config["height"].asInt() / 48.0
                        : 1.0;
  size_ = static_cast<int>(size_ * ui + 0.5);
  // The side margins are fixed px, so scale them with the bar too -- else a
  // compact (short) bar keeps the full 12px left gap and reads lopsided next to
  // the tighter digital-clock side. No-op at base (ui == 1).
  hmargin_l_ = static_cast<int>(hmargin_l_ * ui + 0.5);
  hmargin_r_ = static_cast<int>(hmargin_r_ * ui + 0.5);

  event_box_.set_name(id.empty() ? "analog-clock" : "analog-clock-" + id);
  area_.set_size_request(size_, -1);   // margins are added separately, below
  area_.set_margin_top(vmargin_);
  area_.set_margin_bottom(vmargin_);
  area_.set_margin_start(hmargin_l_);
  area_.set_margin_end(hmargin_r_);
  area_.signal_draw().connect(sigc::mem_fun(*this, &Clock::on_draw));
  event_box_.add(area_);
  event_box_.show_all();

  // No second hand, so a slow timer is plenty; redraw only when the minute
  // rolls over (on_timer checks). 5s cadence keeps the flip prompt without
  // waking often.
  timer_ = Glib::signal_timeout().connect_seconds(
      sigc::mem_fun(*this, &Clock::on_timer), 5);
}

Clock::~Clock() {
  if (timer_.connected()) timer_.disconnect();
}

bool Clock::on_timer() {
  std::time_t t = std::time(nullptr);
  std::tm lt{};
  localtime_r(&t, &lt);
  if (lt.tm_min != last_min_) area_.queue_draw();
  return true;
}

auto Clock::update() -> void {
  area_.queue_draw();
  AModule::update();
}

bool Clock::on_draw(const Cairo::RefPtr<Cairo::Context>& cr) {
  const double w = area_.get_allocated_width();
  const double h = area_.get_allocated_height();
  if (w <= 0 || h <= 0) return true;
  const double cx = w / 2.0, cy = h / 2.0;
  const double R = std::min(w, h) / 2.0 - 2.5;   // face radius

  std::time_t t = std::time(nullptr);
  std::tm lt{};
  localtime_r(&t, &lt);
  last_min_ = lt.tm_min;
  const double hour = (lt.tm_hour % 12) + lt.tm_min / 60.0;
  const double ha = hour / 12.0 * 2.0 * kPi - kPi / 2.0;      // 12 at top
  const double ma = lt.tm_min / 60.0 * 2.0 * kPi - kPi / 2.0;

  cr->set_antialias(Cairo::ANTIALIAS_DEFAULT);

  // Accent colour from CSS (#analog-clock color) -- used for the 12 marker and
  // the hub, so the face stays neutral but carries a hint of the palette.
  auto sc = event_box_.get_style_context();
  const Gdk::RGBA acc = sc->get_color(sc->get_state());
  const double ar = acc.get_red(), ag = acc.get_green(), ab = acc.get_blue();

  // Inset face: a dark disc with a top-shadow -> bottom-light radial, so it
  // reads as recessed. Semi-transparent so it blends with the bar/wallpaper the
  // way the rest of the bar does (a fully opaque face over-pops on a light bg).
  const double kA = 0.82;
  auto face = Cairo::RadialGradient::create(cx, cy - R * 0.5, R * 0.2,
                                            cx, cy + R * 0.2, R * 1.25);
  face->add_color_stop_rgba(0.0, 0.18, 0.11, 0.33, kA);   // dark purple, shaded
  face->add_color_stop_rgba(1.0, 0.37, 0.22, 0.62, kA);   // lit purple
  cr->arc(cx, cy, R, 0, 2.0 * kPi);
  cr->set_source(face);
  cr->fill();

  // Bevelled rim: a dark top edge and a bright bottom edge = an INSET lip.
  auto rim = Cairo::LinearGradient::create(cx, cy - R, cx, cy + R);
  rim->add_color_stop_rgba(0.0, 0.06, 0.06, 0.08, 0.85);   // top in shadow
  rim->add_color_stop_rgba(0.5, 0.30, 0.30, 0.34, 0.45);
  rim->add_color_stop_rgba(1.0, 0.80, 0.82, 0.88, 0.80);   // catches light
  cr->arc(cx, cy, R, 0, 2.0 * kPi);
  cr->set_line_width(2.0);
  cr->set_source(rim);
  cr->stroke();

  // Hour ticks: majors (12/3/6/9) longer/brighter; the 12 marker (i==9) accent.
  for (int i = 0; i < 12; i++) {
    const double a = i / 12.0 * 2.0 * kPi;
    const bool major = (i % 3 == 0);
    const double r0 = R * (major ? 0.70 : 0.80);
    const double r1 = R * 0.90;
    cr->move_to(cx + r0 * std::cos(a), cy + r0 * std::sin(a));
    cr->line_to(cx + r1 * std::cos(a), cy + r1 * std::sin(a));
    if (i == 9) {   // 12 o'clock
      cr->set_line_width(2.6);
      cr->set_source_rgba(ar, ag, ab, 0.95);
    } else {
      cr->set_line_width(major ? 2.0 : 1.0);
      cr->set_source_rgba(0.72, 0.74, 0.80, major ? 0.95 : 0.6);
    }
    cr->stroke();
  }

  // Hands: a small tail past the hub, white core wrapped in a dark outline.
  auto hand = [&](double ang, double len, double width) {
    const double ex = cx + R * len * std::cos(ang);
    const double ey = cy + R * len * std::sin(ang);
    const double bx = cx - R * 0.14 * std::cos(ang);
    const double by = cy - R * 0.14 * std::sin(ang);
    cr->set_line_cap(Cairo::LINE_CAP_ROUND);
    cr->move_to(bx, by);
    cr->line_to(ex, ey);
    cr->set_line_width(width + 2.0);
    cr->set_source_rgba(0.0, 0.0, 0.0, 0.7);
    cr->stroke();
    cr->move_to(bx, by);
    cr->line_to(ex, ey);
    cr->set_line_width(width);
    cr->set_source_rgba(0.878, 0.957, 0.659, 1.0);   // warm green (active)
    cr->stroke();
  };
  hand(ha, 0.48, 4.4);   // hour: shorter + thicker, shows behind the minute
  hand(ma, 0.82, 2.3);   // minute: longer + slimmer

  // Centre hub (accent), capping both hands.
  cr->arc(cx, cy, 2.8, 0, 2.0 * kPi);
  cr->set_source_rgba(ar, ag, ab, 1.0);
  cr->fill_preserve();
  cr->set_line_width(1.0);
  cr->set_source_rgba(0.0, 0.0, 0.0, 0.6);
  cr->stroke();

  return true;
}

}  // namespace waybar::modules::analog
