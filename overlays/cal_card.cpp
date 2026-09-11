#include "modules/cal/card.hpp"

#include <gdkmm/general.h>   // Gdk::Cairo::set_source_pixbuf
#include <glibmm/main.h>
#include <glibmm/markup.h>
#include <glibmm/spawn.h>
#include <pango/pangocairo.h>   // pango_cairo_layout_path (outlined text)

#include <cmath>
#include <ctime>
#include <fstream>

namespace waybar::modules::cal {

static constexpr double kPi = 3.14159265358979323846;

static std::string expand_home(const std::string& p) {
  if (!p.empty() && p[0] == '~') {
    const char* h = getenv("HOME");
    if (h) return std::string(h) + p.substr(1);
  }
  return p;
}

Card::Card(const std::string& id, const waybar::Bar& bar,
           const Json::Value& config)
    : AModule(config, "cal-card", id, false, false), bar_(bar) {
  if (config_["width"].isInt()) width_ = config_["width"].asInt();
  if (config_["vmargin"].isInt()) vmargin_ = config_["vmargin"].asInt();
  if (config_["hmargin"].isInt()) hmargin_ = config_["hmargin"].asInt();
  if (config_["show-private"].isBool())
    show_private_ = config_["show-private"].asBool();
  if (config_["max-days"].isInt()) max_days_ = config_["max-days"].asInt();
  if (bar_.config["height"].isInt())
    ui_scale_ = bar_.config["height"].asInt() / 48.0;

  const char* xrd = getenv("XDG_RUNTIME_DIR");
  const std::string run = xrd ? xrd : "/tmp";
  state_path_ = config_["state-path"].isString()
                    ? expand_home(config_["state-path"].asString())
                    : run + "/corp-cal.json";
  icon_path_ = config_["icon"].isString()
                   ? expand_home(config_["icon"].asString())
                   : expand_home("~/.config/waybar/icons/gcal.png");

  event_box_.set_name(id.empty() ? "cal-card" : "cal-card-" + id);
  area_.nat_w = static_cast<int>(width_ * ui_scale_ + 0.5);
  area_.set_margin_top(vmargin_);
  area_.set_margin_bottom(vmargin_);
  area_.set_margin_start(hmargin_);
  area_.set_margin_end(hmargin_);
  area_.signal_draw().connect(sigc::mem_fun(*this, &Card::on_draw));
  event_box_.add(area_);
  event_box_.show_all();   // always shown -- an explicit "nothing" when idle

  readState();
  // The minute countdown moves slowly and the fetcher rewrites the file every
  // few minutes; a 20s poll keeps the countdown fresh without churn.
  poll_ = Glib::signal_timeout().connect_seconds(
      [this]() {
        readState();
        area_.queue_draw();
        return true;
      },
      20);
}

Card::~Card() {
  if (poll_.connected()) poll_.disconnect();
}

auto Card::update() -> void {
  area_.queue_draw();
  AModule::update();
}

// Parse an RFC3339 start ("2026-07-31T16:30:00-04:00") -> local HH:MM and the
// unix epoch. A fallback for a fetcher predating the time/start_epoch fields --
// a stale deploy still shows the time + countdown instead of dropping them.
static bool parse_iso(const std::string& s, long& epoch, std::string& hhmm) {
  int Y, Mo, D, H, Mi, S, oh = 0, om = 0;
  char sign = '+';
  int n = std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d%c%d:%d", &Y, &Mo, &D, &H,
                      &Mi, &S, &sign, &oh, &om);
  if (n < 5) return false;
  char b[8];
  std::snprintf(b, sizeof b, "%02d:%02d", H, Mi);
  hhmm = b;
  if (n >= 9 && (sign == '+' || sign == '-')) {
    struct tm tm{};
    tm.tm_year = Y - 1900;
    tm.tm_mon = Mo - 1;
    tm.tm_mday = D;
    tm.tm_hour = H;
    tm.tm_min = Mi;
    tm.tm_sec = S;
    long off = static_cast<long>(oh * 3600 + om * 60) * (sign == '-' ? -1 : 1);
    epoch = timegm(&tm) - off;
  }
  return true;
}

bool Card::readState() {
  have_ = false;
  title_.clear();
  time_.clear();
  room_.clear();
  visibility_ = "default";
  attendees_ = 0;
  start_epoch_ = 0;
  std::ifstream in(state_path_);
  if (in) {
    Json::Value j;
    Json::CharReaderBuilder b;
    std::string err;
    if (Json::parseFromStream(b, in, &j, &err) && j.isMember("event") &&
        j["event"].isObject()) {
      const Json::Value& e = j["event"];
      have_ = true;
      title_ = e.get("title", "").asString();
      time_ = e.get("time", "").asString();
      room_ = e.get("room", "").asString();
      visibility_ = e.get("visibility", "default").asString();
      attendees_ = e.get("attendees", 0).asInt();
      start_epoch_ = static_cast<long>(e.get("start_epoch", 0).asLargestInt());
      if ((time_.empty() || start_epoch_ == 0) && e.isMember("start")) {
        long ep = 0;
        std::string hm;
        if (parse_iso(e["start"].asString(), ep, hm)) {
          if (time_.empty()) time_ = hm;
          if (start_epoch_ == 0) start_epoch_ = ep;
        }
      }
      // Horizon: an event more than max_days_ CALENDAR days out (in local time)
      // is treated as nothing upcoming -- the card is a soon-list.
      if (start_epoch_ > 0) {
        std::time_t now = std::time(nullptr);
        std::tm nt = *std::localtime(&now);
        std::time_t se = static_cast<std::time_t>(start_epoch_);
        std::tm et = *std::localtime(&se);
        nt.tm_hour = nt.tm_min = nt.tm_sec = 0; nt.tm_isdst = -1;
        et.tm_hour = et.tm_min = et.tm_sec = 0; et.tm_isdst = -1;
        long days = (std::mktime(&et) - std::mktime(&nt) + 43200) / 86400;
        if (days > max_days_) have_ = false;
      }
      if (visibility_ == "private" && !show_private_) title_ = "Busy";
    }
  }
  return have_;
}

void Card::ensureIcon() {
  if (icon_path_ == icon_loaded_) return;
  icon_loaded_ = icon_path_;
  icon_.reset();
  if (icon_path_.empty()) return;
  try {
    icon_ = Gdk::Pixbuf::create_from_file(icon_path_);
  } catch (const Glib::Error&) {
    icon_.reset();
  }
}

// Outlined text: a black keyline then a coloured fill, so it reads over the
// icon and the busy bar alike (the dial-readout idiom).
static void outlined(const Cairo::RefPtr<Cairo::Context>& cr,
                     const Glib::RefPtr<Pango::Layout>& lay, double x, double y,
                     double lw, double r, double g, double b) {
  cr->move_to(x, y);
  pango_cairo_layout_path(cr->cobj(), lay->gobj());
  cr->set_line_join(Cairo::LINE_JOIN_ROUND);
  cr->set_line_width(lw);
  cr->set_source_rgba(0, 0, 0, 0.92);
  cr->stroke_preserve();
  cr->set_source_rgba(r, g, b, 1.0);
  cr->fill();
}

bool Card::on_draw(const Cairo::RefPtr<Cairo::Context>& cr) {
  const double w = area_.get_allocated_width();
  const double h = area_.get_allocated_height();
  if (w <= 0 || h <= 0) return true;
  cr->set_antialias(Cairo::ANTIALIAS_DEFAULT);

  // Text scales with the bar height (ui = bar_h/48). ONE-SIDED: amplify the
  // deviation only when scaling UP (wall); scale DOWN linearly (a compact
  // low-res bar). ui == 1 => fscale == 1 either way -- a no-op, manifold safe.
  const double fscale =
      ui_scale_ < 1.0 ? ui_scale_ : 1.0 + (ui_scale_ - 1.0) * 2.4;
  auto font = [fscale](const char* base, double pt) {
    Pango::FontDescription d(base);
    d.set_size(static_cast<int>(pt * fscale * PANGO_SCALE + 0.5));
    return d;
  };
  auto esc = [](const std::string& s) {
    return Glib::Markup::escape_text(s).raw();
  };
  auto lay = [&](const std::string& markup, const char* f, double pt,
                 double maxw) {
    auto l = area_.create_pango_layout("");
    l->set_markup(markup);
    l->set_font_description(font(f, pt));
    if (maxw > 0) {
      l->set_ellipsize(Pango::ELLIPSIZE_END);
      l->set_width(static_cast<int>(maxw) * PANGO_SCALE);
    }
    return l;
  };

  const double pad = 4;

  // --- live countdown (a real upcoming event only) --------------------------
  std::string cd;
  double cr_ = 0.72, cg_ = 0.77, cb_ = 0.86;   // rest: muted slate (< the time)
  if (have_ && start_epoch_ > 0) {
    long mins = (start_epoch_ - std::time(nullptr)) / 60;
    if (mins <= 0) {
      cd = "now";
      cr_ = 0.925, cg_ = 0.596, cb_ = 0.239;   // deep amber (happening)
    } else if (mins < 60) {
      cd = std::to_string(mins) + "m";
      if (mins < 5) {
        cr_ = 0.925, cg_ = 0.596, cb_ = 0.239;   // deep amber (<5m)
      } else if (mins < 10) {
        cr_ = 0.933, cg_ = 0.816, cb_ = 0.365;   // yellow (<10m)
      }
    } else {
      long hh = mins / 60, mm = mins % 60;
      cd = mm ? std::to_string(hh) + "h" + std::to_string(mm) + "m"
              : std::to_string(hh) + "h";
    }
  }
  const bool hasCd = !cd.empty();

  // Weekday label -- shown ABOVE the time ONLY when not today
  // (a different local calendar day than now). Derived from the start epoch, so
  // it needs no extra field from the fetcher.
  std::string weekday;
  if (have_ && start_epoch_ > 0) {
    std::time_t now = std::time(nullptr);
    std::tm nt = *std::localtime(&now);
    std::time_t se = static_cast<std::time_t>(start_epoch_);
    std::tm et = *std::localtime(&se);
    if (nt.tm_year != et.tm_year || nt.tm_yday != et.tm_yday) {
      static const char* kWd[7] = {"Sunday",    "Monday",   "Tuesday",
                                   "Wednesday", "Thursday", "Friday",
                                   "Saturday"};
      if (et.tm_wday >= 0 && et.tm_wday < 7) weekday = kWd[et.tm_wday];
    }
  }
  const bool hasWd = !weekday.empty();

  ensureIcon();
  const double right = w - pad;
  auto drawIcon = [&](double ix, double iy, double sz) {
    if (!icon_) return;
    const double pw = icon_->get_width(), ph = icon_->get_height();
    const double sc = sz / std::max(pw, ph);
    cr->save();
    cr->translate(ix + sz / 2, iy + sz / 2);
    cr->scale(sc, sc);
    Gdk::Cairo::set_source_pixbuf(cr, icon_, -pw / 2, -ph / 2);
    cr->paint();
    cr->restore();
  };

  // --- no event: full icon + a quiet message, both vertically centred -------
  if (!have_) {
    const double sz = h - 2 * pad;
    drawIcon(pad, (h - sz) / 2, sz);
    const double mx = pad + sz + std::max(10.0, sz * 0.22);
    auto l = lay("<span fgcolor='#8a92a3'>Nothing upcoming</span>", "Sans", 9.5,
                 right - mx);
    int lw = 0, lh = 0;
    l->get_pixel_size(lw, lh);
    cr->move_to(mx, (h - lh) / 2);
    l->show_in_cairo_context(cr);
    return true;
  }

  // --- LEFT: the bold time, the countdown nested right under it, and -- when
  //     the appt is not today -- the weekday nested right above it (a tight
  //     three-line stack), with the gcal icon hugging it on the side --------
  auto timeLay = lay(
      "<span fgcolor='#f2f5fb' weight='bold'>" + esc(time_) + "</span>", "Sans",
      13.5, 0);
  int twW = 0, twH = 0;
  timeLay->get_pixel_size(twW, twH);
  Glib::RefPtr<Pango::Layout> wdLay;
  int wdW = 0, wdH = 0;
  if (hasWd) {
    wdLay = area_.create_pango_layout(weekday);
    wdLay->set_font_description(font("Sans Bold", 9.5));
    wdLay->get_pixel_size(wdW, wdH);
  }
  Glib::RefPtr<Pango::Layout> cdLay;
  int cdW = 0, cdH = 0;
  if (hasCd) {
    cdLay = area_.create_pango_layout(cd);
    cdLay->set_font_description(font("Sans Bold", 7.5));
    cdLay->get_pixel_size(cdW, cdH);
  }
  // The time+countdown are a vcentred pair (the countdown pulls up into the
  // time's digit-only descent) -- and the TIME stays put whether or not there's
  // a weekday. The weekday (when not today) is stamped ON TOP of the time's
  // upper part: it sits at the top and may obscure part of the time, which is
  // harmless -- it is absent on the actual day, so nothing is lost. Drawn LAST.
  const double tighten = hasCd ? std::max(4.0, twH * 0.32) : 0;
  const double timeBlock = hasCd ? (twH - tighten + cdH) : twH;
  const double timeTop = (h - timeBlock) / 2;   // vertically centred

  const double iconSz = std::min(h - 2 * pad, timeBlock);
  drawIcon(pad, (h - iconSz) / 2, iconSz);
  const double lx = pad + iconSz + std::max(4.0, iconSz * 0.09);

  double leftW = twW;
  cr->move_to(lx, timeTop);
  timeLay->show_in_cairo_context(cr);
  if (hasCd) {
    // centre the countdown under the time
    outlined(cr, cdLay, lx + (twW - cdW) / 2.0, timeTop + twH - tighten,
             std::max(1.5, cdH * 0.14), cr_, cg_, cb_);
    leftW = std::max(leftW, static_cast<double>(cdW));
  }
  if (hasWd) {
    // full weekday, cool-blue, pinned RIGHT AT THE TOP of the bar: align the
    // glyph INK to the top edge (past the ascent leading), leaving just
    // the outline's half-width above it. Centred over time, never left of
    // lx (so it won't ride onto the icon); drawn ON TOP, overlapping the time.
    const double wdx = std::max(lx, lx + (twW - wdW) / 2.0);
    const double lw = std::max(1.5, wdH * 0.14);
    Pango::Rectangle wdInk, wdLog;
    wdLay->get_pixel_extents(wdInk, wdLog);
    const double wdTop = lw * 0.5 - wdInk.get_y();
    outlined(cr, wdLay, wdx, wdTop, lw, 0.50, 0.69, 0.86);
    leftW = std::max(leftW, (wdx - lx) + static_cast<double>(wdW));
  }

  // --- RIGHT: title (+ attendee count) over the room, vertically centred ----
  const double rx = lx + leftW + std::max(14.0, iconSz * 0.30);
  const double sw = right - rx;
  if (sw < 16) return true;
  std::string count =
      attendees_ > 0 ? "  <span fgcolor='#8f97a6'>(" +
                           std::to_string(attendees_) + ")</span>"
                     : "";
  auto l1 = lay("<span fgcolor='#eef1f7'>" + esc(title_) + "</span>" + count,
                "Sans", 10, sw);
  const bool redacted = visibility_ == "private" && !show_private_;
  Glib::RefPtr<Pango::Layout> l2;
  if (!redacted) {
    // the local room, brighter; else an explicit "no room booked" (a remote
    // room does not help, so we never surface one)
    l2 = lay(room_.empty()
                 ? "<span fgcolor='#949db0' style='italic'>no room booked"
                   "</span>"
                 : "<span fgcolor='#cbd5ea'>" + esc(room_) + "</span>",
             "Sans", 9, sw);
  }
  int a1w = 0, a1h = 0, a2w = 0, a2h = 0;
  l1->get_pixel_size(a1w, a1h);
  if (l2) l2->get_pixel_size(a2w, a2h);
  // Pull the room up into line 1's descent space so the two lines sit close.
  const double step = a1h - std::max(3.0, a1h * 0.24);
  const double rblock = l2 ? (step + a2h) : a1h;
  double ry = (h - rblock) / 2;
  cr->move_to(rx, ry);
  l1->show_in_cairo_context(cr);
  if (l2) {
    cr->move_to(rx, ry + step);
    l2->show_in_cairo_context(cr);
  }
  return true;
}

}  // namespace waybar::modules::cal
