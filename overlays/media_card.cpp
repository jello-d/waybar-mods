#include "modules/media/card.hpp"

#include <gdkmm/general.h>   // Gdk::Cairo::set_source_pixbuf
#include <glibmm/main.h>
#include <glibmm/markup.h>
#include <glibmm/spawn.h>
#include <pango/pangocairo.h>   // pango_cairo_layout_path (outlined text)

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>

#include "flush_tooltip.hpp"   // rich tooltip: album art + labelled lines

namespace waybar::modules::media {

static constexpr double kPi = 3.14159265358979323846;

// warm-green accent shared with the taskbar active title + clock hands
static constexpr double kAccR = 0.878, kAccG = 0.957, kAccB = 0.659;
// bold progress green -- the one vivid accent, high-contrast against the cool
// grey title/artist/album text and the dark card
static constexpr double kProgR = 0.44, kProgG = 0.92, kProgB = 0.42;

static std::string expand_home(const std::string& p) {
  if (!p.empty() && p[0] == '~') {
    const char* h = getenv("HOME");
    if (h) return std::string(h) + p.substr(1);
  }
  return p;
}

static std::string mmss(double s) {
  if (s < 0 || !std::isfinite(s)) s = 0;
  int t = static_cast<int>(s + 0.5);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%d:%02d", t / 60, t % 60);
  return buf;
}

static double now_mono() { return g_get_monotonic_time() / 1e6; }

Card::Card(const std::string& id, const waybar::Bar& bar,
           const Json::Value& config)
    : AModule(config, "media-card", id, false, false), bar_(bar) {
  if (config_["width"].isInt()) width_ = config_["width"].asInt();
  if (config_["min-width"].isInt()) min_width_ = config_["min-width"].asInt();
  if (config_["vmargin"].isInt()) vmargin_ = config_["vmargin"].asInt();
  if (config_["hmargin"].isInt()) hmargin_ = config_["hmargin"].asInt();
  // Bar-height scale (48 = design); wb sets a taller bar on the 3-monitor wall.
  if (bar_.config["height"].isInt())
    ui_scale_ = bar_.config["height"].asInt() / 48.0;

  // Audio spectrum (opt-in). DSP + render tunables are config, so gain/decay/
  // look can be dialed without a rebuild (a `wb restart` re-reads them).
  spectrum_on_ = config_["spectrum"].isBool() && config_["spectrum"].asBool();
  if (spectrum_on_) {
    Spectrum::Config sc;
    auto cfgi = [&](const char* k, int d) {
      return config_[k].isInt() ? config_[k].asInt() : d;
    };
    auto cfgd = [&](const char* k, double d) {
      return config_[k].isNumeric() ? config_[k].asDouble() : d;
    };
    sc.bands = cfgi("spectrum-bands", sc.bands);
    sc.fft = cfgi("spectrum-fft", sc.fft);
    sc.gain = cfgd("spectrum-gain", sc.gain);
    sc.floor_db = cfgd("spectrum-floor-db", sc.floor_db);
    sc.tilt = cfgd("spectrum-tilt", sc.tilt);
    sc.attack = cfgd("spectrum-attack", sc.attack);
    sc.decay = cfgd("spectrum-decay", sc.decay);
    sc.fmin = cfgd("spectrum-fmin", sc.fmin);
    sc.fmax = cfgd("spectrum-fmax", sc.fmax);
    sc.active_rms = cfgd("spectrum-active-rms", sc.active_rms);
    cap_hold_s_ = cfgd("spectrum-cap-hold", cap_hold_s_);
    cap_gravity_ = cfgd("spectrum-cap-gravity", cap_gravity_);
    dot_h_ = cfgd("spectrum-dot-height", dot_h_);
    dot_gap_ = cfgd("spectrum-dot-gap", dot_gap_);
    spec_alpha_ = cfgd("spectrum-alpha", spec_alpha_);
    text_scrim_ = cfgd("spectrum-text-scrim", text_scrim_);
    text_outline_ = cfgd("spectrum-text-outline", text_outline_);
    levels_.assign(sc.bands, 0.0f);
    caps_.assign(sc.bands, 0.0f);
    cap_vel_.assign(sc.bands, 0.0f);
    cap_hold_.assign(sc.bands, 0.0);
    spec_ = std::make_unique<Spectrum>(sc);
  }

  const char* xrd = getenv("XDG_RUNTIME_DIR");
  const std::string run = xrd ? xrd : "/tmp";
  state_path_ = config_["state-path"].isString()
                    ? expand_home(config_["state-path"].asString())
                    : run + "/now-playing.state";
  ctl_cmd_ = config_["ctl-cmd"].isString()
                 ? expand_home(config_["ctl-cmd"].asString())
                 : expand_home("~/.config/waybar/scripts/np-ctl");

  event_box_.set_name(id.empty() ? "media-card" : "media-card-" + id);
  area_.nat_w = width_;                          // full width when there's room
  area_.min_w = std::min(min_width_, width_);    // shrink floor when squeezed
  area_.set_margin_top(vmargin_);
  area_.set_margin_bottom(vmargin_);
  area_.set_margin_start(hmargin_);
  area_.set_margin_end(hmargin_);
  area_.add_events(Gdk::BUTTON_PRESS_MASK | Gdk::SCROLL_MASK |
                   Gdk::ENTER_NOTIFY_MASK | Gdk::LEAVE_NOTIFY_MASK);
  area_.signal_draw().connect(sigc::mem_fun(*this, &Card::on_draw));
  area_.signal_button_press_event().connect(
      sigc::mem_fun(*this, &Card::on_button));
  area_.signal_scroll_event().connect(sigc::mem_fun(*this, &Card::on_scroll));
  area_.signal_enter_notify_event().connect(
      [this](GdkEventCrossing*) { hovered_ = true; return false; });
  area_.signal_leave_notify_event().connect([this](GdkEventCrossing* e) {
    if (e->detail != GDK_NOTIFY_INFERIOR) hovered_ = false;
    return false;
  });
  event_box_.add(area_);
  event_box_.show_all();

  marquee_t0_ = now_mono();
  readState();
  poll_ = Glib::signal_timeout().connect_seconds(
      [this]() {
        readState();
        area_.queue_draw();
        return true;
      },
      1);
  // 30fps clock: the marquee needs it while a long title scrolls; on_frame
  // throttles to ~4fps otherwise (the scrubber) and idles when not playing.
  frame_ = Glib::signal_timeout().connect(sigc::mem_fun(*this, &Card::on_frame),
                                          33);
}

Card::~Card() {
  if (poll_.connected()) poll_.disconnect();
  if (frame_.connected()) frame_.disconnect();
}

auto Card::update() -> void {
  area_.queue_draw();
  AModule::update();
}

bool Card::on_frame() {
  if (!area_.get_mapped()) return true;
  ++tick_;
  const bool spec = spec_ && updateSpectrum();   // caps animate; true if lively
  if (status_ != "playing") {
    if (spec) area_.queue_draw();   // let the caps fall to rest after a stop
    return true;
  }
  // 30fps while scrolling or the analyzer is live, else ~4fps (scrubber).
  if (overflowing_ || spec || tick_ % 8 == 0) area_.queue_draw();
  return true;
}

bool Card::readState() {
  std::ifstream in(state_path_);
  if (!in) {
    status_ = "idle";
    title_.clear();
    updateTooltip();
    return false;
  }
  Json::Value j;
  Json::CharReaderBuilder b;
  std::string err;
  if (!Json::parseFromStream(b, in, &j, &err)) return false;

  status_ = j.get("status", "idle").asString();
  source_ = j.get("source", "").asString();
  device_ = j.get("device", "").asString();
  const std::string newtitle = j.get("title", "").asString();
  if (newtitle != title_) marquee_t0_ = now_mono();   // restart scroll
  title_ = newtitle;
  artist_ = j.get("artist", "").asString();
  album_ = j.get("album", "").asString();
  art_path_ = expand_home(j.get("art", "").asString());
  length_ = j.get("length", 0.0).asDouble();

  // Fold any staleness of the sample into the starting position: the daemon
  // stamps `at` (wall seconds) when it read `position`, so a sample sitting in
  // the file for a while still starts from the right place, then interpolates.
  double pos = j.get("position", 0.0).asDouble();
  if (status_ == "playing" && j.isMember("at")) {
    const double at = j["at"].asDouble();
    const double wall = g_get_real_time() / 1e6;
    if (at > 0 && wall >= at) pos += (wall - at);
  }
  if (length_ > 0 && pos > length_) pos = length_;
  position_ = pos;
  sample_mono_ = now_mono();
  updateTooltip();
  return true;
}

// Full, untruncated now-playing info published as a tooltip-markup property on
// the drawing area; the AModule hook routes it through the shared flush callout
// (which reads the property, not has-tooltip). Rebuilt each poll, so it tracks
// song changes. Dynamic fields are markup-escaped.
void Card::updateTooltip() {
  std::string t;   // empty when idle -> the callout hides
  if (status_ != "idle" && !title_.empty()) {
    auto esc = [](const std::string& s) {
      return Glib::Markup::escape_text(s).raw();
    };
    // One "Label<tab>value" row per line; the callout tab-aligns the values.
    auto row = [&](const char* label, const std::string& value) {
      return "<span fgcolor='#8a8f98'>" + std::string(label) + "</span>\t" +
             value;
    };
    t = row("Title", "<b>" + esc(title_) + "</b>");
    if (!artist_.empty()) t += "\n" + row("Artist", esc(artist_));
    if (!album_.empty()) t += "\n" + row("Album", esc(album_));
    if (source_ == "cast" && !device_.empty())
      t += "\n" + row("Casting", esc(device_));
    const std::string st = status_ == "playing"  ? "Playing"
                           : status_ == "paused" ? "Paused"
                                                 : "";
    std::string tv;
    if (length_ > 0) tv = mmss(livePosition()) + " / " + mmss(length_);
    if (!st.empty()) tv += (tv.empty() ? "" : "  ·  ") + st;
    if (!tv.empty()) t += "\n" + row("Time", tv);
  }
  event_box_.set_tooltip_markup(t);
  FlushTooltip::setImage(event_box_, t.empty() ? std::string() : art_path_);
  // If the card is hovered, refresh the SHOWN callout now so it tracks the card
  // face (a skip, play/pause, a new track) in sync instead of waiting for the
  // next hover. show() re-renders in place (empty markup hides it when idle).
  if (hovered_) FlushTooltip::instance().show(event_box_, t, false);
}

double Card::livePosition() const {
  double p = position_;
  if (status_ == "playing") p += now_mono() - sample_mono_;
  if (length_ > 0 && p > length_) p = length_;
  return p < 0 ? 0 : p;
}

void Card::ensureArt() {
  if (art_path_ == art_loaded_) return;
  art_loaded_ = art_path_;
  art_.reset();
  if (art_path_.empty()) return;
  try {
    art_ = Gdk::Pixbuf::create_from_file(art_path_);
  } catch (const Glib::Error&) {
    art_.reset();
  }
}

void Card::sendCtl(const std::string& cmd) {
  try {
    Glib::spawn_command_line_async(ctl_cmd_ + " " + cmd);
  } catch (const Glib::Error&) {
  }
}

// ---- Cairo transport glyphs (no font glyphs) ------------------------------
void Card::glyphPlay(const Cairo::RefPtr<Cairo::Context>& cr, double cx,
                     double cy, double s) {
  cr->move_to(cx - s * 0.6, cy - s);
  cr->line_to(cx + s * 0.85, cy);
  cr->line_to(cx - s * 0.6, cy + s);
  cr->close_path();
  cr->fill();
}

void Card::glyphPause(const Cairo::RefPtr<Cairo::Context>& cr, double cx,
                      double cy, double s) {
  const double bw = s * 0.5, gap = s * 0.42;
  cr->rectangle(cx - gap - bw, cy - s, bw, 2 * s);
  cr->rectangle(cx + gap, cy - s, bw, 2 * s);
  cr->fill();
}

void Card::glyphNext(const Cairo::RefPtr<Cairo::Context>& cr, double cx,
                     double cy, double s) {
  cr->move_to(cx - s, cy - s * 0.8);
  cr->line_to(cx + s * 0.2, cy);
  cr->line_to(cx - s, cy + s * 0.8);
  cr->close_path();
  cr->fill();
  cr->rectangle(cx + s * 0.25, cy - s * 0.8, s * 0.32, s * 1.6);
  cr->fill();
}

void Card::glyphPrev(const Cairo::RefPtr<Cairo::Context>& cr, double cx,
                     double cy, double s) {
  cr->move_to(cx + s, cy - s * 0.8);
  cr->line_to(cx - s * 0.2, cy);
  cr->line_to(cx + s, cy + s * 0.8);
  cr->close_path();
  cr->fill();
  cr->rectangle(cx - s * 0.57, cy - s * 0.8, s * 0.32, s * 1.6);
  cr->fill();
}

// A tiny "casting" mark: a screen rect with two concentric signal arcs, drawn
// at (x, y) as the top-left, spanning ~s.
void Card::glyphCast(const Cairo::RefPtr<Cairo::Context>& cr, double x,
                     double y, double s) {
  cr->set_line_width(std::max(1.0, s * 0.10));
  cr->rectangle(x + s * 0.30, y + s * 0.08, s * 0.68, s * 0.52);
  cr->stroke();
  for (int i = 1; i <= 2; i++) {
    cr->arc(x, y + s, s * 0.28 * i, -0.5 * kPi, 0.0);
    cr->stroke();
  }
  cr->arc(x, y + s, s * 0.06, 0, 2 * kPi);
  cr->fill();
}

// ---- draw -----------------------------------------------------------------
bool Card::on_draw(const Cairo::RefPtr<Cairo::Context>& cr) {
  const double w = area_.get_allocated_width();
  const double h = area_.get_allocated_height();
  if (w <= 0 || h <= 0) return true;
  const bool idle = (status_ == "idle") || title_.empty();
  const double dim = idle ? 0.45 : 1.0;   // whole card fades when idle

  // Card text scales with the bar height (ui_scale_ = bar_h / 48). ONE-SIDED:
  // when scaling UP (wall) the card reads too small at a straight bar-ratio, so
  // grow the text faster (2.4x the deviation from 1); when scaling DOWN (a
  // compact low-res bar) go LINEAR, so the text never collapses. ui_scale_ == 1
  // => fscale == 1 either way -- a strict no-op, manifold untouched.
  const double fscale =
      ui_scale_ < 1.0 ? ui_scale_ : 1.0 + (ui_scale_ - 1.0) * 2.4;
  auto scaled_font = [fscale](const char* base, double pt) {
    Pango::FontDescription d(base);
    d.set_size(static_cast<int>(pt * fscale * PANGO_SCALE + 0.5));
    return d;
  };

  cr->set_antialias(Cairo::ANTIALIAS_DEFAULT);

  auto roundRect = [&](double x, double y, double rw, double rh, double r) {
    cr->begin_new_sub_path();
    cr->arc(x + rw - r, y + r, r, -0.5 * kPi, 0.0);
    cr->arc(x + rw - r, y + rh - r, r, 0.0, 0.5 * kPi);
    cr->arc(x + r, y + rh - r, r, 0.5 * kPi, kPi);
    cr->arc(x + r, y + r, r, kPi, 1.5 * kPi);
    cr->close_path();
  };

  // Card backdrop: a rounded translucent panel with a soft top-shadow bevel,
  // so it reads as one raised tile against the bar.
  roundRect(0.75, 0.75, w - 1.5, h - 1.5, 11);
  auto bg = Cairo::LinearGradient::create(0, 0, 0, h);
  bg->add_color_stop_rgba(0.0, 0.16, 0.15, 0.20, 0.66 * dim);
  bg->add_color_stop_rgba(1.0, 0.09, 0.08, 0.12, 0.66 * dim);
  cr->set_source(bg);
  cr->fill_preserve();
  cr->set_line_width(1.0);
  cr->set_source_rgba(1, 1, 1, 0.06 * dim);
  cr->stroke();
  // The analyzer is drawn later, confined to the text/progress-bar column (so
  // it never runs under the art or the transport); see the text block below.

  const double pad = 5;
  const double art = h - 2 * pad;
  const double ax = pad, ay = pad;

  // Cover art (rounded-clipped), else a placeholder tile.
  ensureArt();
  cr->save();
  roundRect(ax, ay, art, art, 6);
  cr->clip();
  if (art_) {
    const double pw = art_->get_width(), ph = art_->get_height();
    const double sc = art / std::min(pw, ph);   // scale to cover
    cr->translate(ax + art / 2, ay + art / 2);
    cr->scale(sc, sc);
    Gdk::Cairo::set_source_pixbuf(cr, art_, -pw / 2, -ph / 2);
    cr->paint_with_alpha(dim);
  } else {
    auto ph2 = Cairo::LinearGradient::create(ax, ay, ax, ay + art);
    ph2->add_color_stop_rgba(0.0, 0.30, 0.26, 0.40, dim);
    ph2->add_color_stop_rgba(1.0, 0.18, 0.15, 0.26, dim);
    cr->set_source(ph2);
    cr->paint();
    // a small centred music note
    cr->set_source_rgba(1, 1, 1, 0.35 * dim);
    const double nx = ax + art * 0.60, ny = ay + art * 0.34;
    cr->set_line_width(std::max(1.5, art * 0.05));
    cr->move_to(nx, ny);
    cr->line_to(nx, ny + art * 0.34);
    cr->stroke();
    cr->arc(nx - art * 0.09, ny + art * 0.34, art * 0.09, 0, 2 * kPi);
    cr->fill();
  }
  cr->restore();

  // Cast badge overlaid on the art's top-left when the source is a Chromecast.
  if (source_ == "cast" && !idle) {
    cr->save();
    roundRect(ax + 3, ay + 3, art * 0.34, art * 0.30, 3);
    cr->set_source_rgba(0.05, 0.05, 0.07, 0.72);
    cr->fill();
    cr->set_source_rgba(kAccR, kAccG, kAccB, 0.95);
    glyphCast(cr, ax + 6, ay + 5, art * 0.24);
    cr->restore();
  }

  // Elapsed time anchored to the art's lower-right, bottom-aligned with the
  // progress bar (so the two read as one unit) and set just left of the bar's
  // start with a breathing gap. Black-outlined, reads even where it bleeds
  // off the artwork.
  if (!idle && length_ > 0) {
    auto tl = area_.create_pango_layout(mmss(livePosition()));
    tl->set_font_description(scaled_font("Sans Bold", 7));
    int tlw = 0, tlh = 0;
    tl->get_pixel_size(tlw, tlh);
    const double bar_x = ax + art + 12;                // progress bar start
    const double bar_bottom = (h - pad + 0.5) + 2.0;   // bar's lower edge
    // +2.5 drops the glyphs past the font's descent space so they
    // truly hug the bar's bottom edge and free more art above.
    cr->move_to(bar_x - 5 - tlw, bar_bottom - tlh + 2.5);
    pango_cairo_layout_path(cr->cobj(), tl->gobj());
    cr->set_line_join(Cairo::LINE_JOIN_ROUND);
    cr->set_line_width(2.4);
    cr->set_source_rgba(0, 0, 0, 0.9 * dim);
    cr->stroke_preserve();
    cr->set_source_rgba(0.941, 0.980, 0.831, dim);   // #f0fad4 (clock green)
    cr->fill();
  }

  // Transport cluster on the right: prev / play-pause / next. The glyph sizes
  // are fixed px, so on a COMPACT (short) bar they read too big next to the
  // shrunken pill -- scale DOWN with the bar (one-sided: no-op at/above the
  // base, so base and the wall are untouched).
  const double tsc = ui_scale_ < 1.0 ? ui_scale_ : 1.0;
  const double skip = 9 * tsc, play = 12 * tsc, gap = 12 * tsc;
  const double cy = h / 2 - 1;
  const double next_cx = w - pad - skip - 2;
  const double play_cx = next_cx - (skip + gap + play);
  const double prev_cx = play_cx - (play + gap + skip);
  const double tcol = idle ? 0.42 : 0.86;
  cr->set_source_rgba(tcol, tcol, tcol * 1.02, dim);
  glyphPrev(cr, prev_cx, cy, skip);
  if (status_ == "playing")
    glyphPause(cr, play_cx, cy, play);
  else
    glyphPlay(cr, play_cx, cy, play);
  glyphNext(cr, next_cx, cy, skip);
  prev_x_ = static_cast<int>(prev_cx - skip - 4);
  prev_w_ = static_cast<int>(2 * skip + 8);
  play_x_ = static_cast<int>(play_cx - play - 4);
  play_w_ = static_cast<int>(2 * play + 8);
  next_x_ = static_cast<int>(next_cx - skip - 4);
  next_w_ = static_cast<int>(2 * skip + 8);

  // Text column: a Winamp-style title marquee (row 1) over the artist and album
  // (row 2), each in its own hue. The artist is greedy (its natural
  // width, capped) and static with an ellipsis; the album takes what's left and
  // scrolls in sync with the title -- share ONE cycle (leading linger, slow
  // scroll, the shorter waiting at its tail), so they reset together. A thin
  // progress hairline hugs the bottom; elapsed time lives over the art, so both
  // rows get the full width.
  const double tx = ax + art + 12;
  // Symmetric with the right margin (next glyph -> pill edge): the spectrum/
  // progress window ends the same distance left of the prev glyph as next sits
  // from the pill edge (pad + 2, minus the ~0.75 pill inset).
  const double tright = prev_cx - skip - 6;
  const double tw = tright - tx;
  if (tw > 24) {
    const double row1 = 2, row2 = h * 0.48;
    const double v = 42.0, tlin = 3.5, tend = 1.0;   // slower lead, quick tail
    const bool hasAlbum = !idle && !album_.empty();

    // Analyzer confined to the progress-bar column [tx, tx+tw] -- never under
    // the art or the transport. Drawn first, so the text sits over it.
    if (spec_ && spectrum_on_ && !idle) {
      cr->save();
      cr->rectangle(tx - 2, 0, tw + 4, h);
      cr->clip();
      drawSpectrum(cr, tx, tx + tw, h);
      cr->restore();
    }

    // Smoked-glass plate behind the text column: the analyzer stays punchy in
    // the open margins (over the art gap, under the transport) and glows dimmed
    // behind the names, which now sit on their own dark strip and read clearly.
    // Soft left/right edges so it blends into the card, not a hard box.
    if (spec_ && spectrum_on_ && !idle && text_scrim_ > 0.0) {
      const double sx0 = tx - 6, sx1 = tright + 4;
      const double sy0 = 1.5, sy1 = h - 3;
      const double sr = std::min(6.0, (sy1 - sy0) * 0.45);
      const double a = text_scrim_ * dim;
      const double fw = std::min(16.0, (sx1 - sx0) * 0.18) / (sx1 - sx0);
      cr->save();
      roundRect(sx0, sy0, sx1 - sx0, sy1 - sy0, sr);
      auto sg = Cairo::LinearGradient::create(sx0, 0, sx1, 0);
      sg->add_color_stop_rgba(0.0, 0.03, 0.03, 0.05, 0.0);
      sg->add_color_stop_rgba(fw, 0.03, 0.03, 0.05, a);
      sg->add_color_stop_rgba(1.0 - fw, 0.03, 0.03, 0.05, a);
      sg->add_color_stop_rgba(1.0, 0.03, 0.03, 0.05, 0.0);
      cr->set_source(sg);
      cr->fill();
      cr->restore();
    }

    // title layout + overflow
    auto titleLay =
        area_.create_pango_layout(idle ? "Nothing playing" : title_);
    titleLay->set_font_description(scaled_font("Sans Bold", 10));
    int tfw = 0, tfh = 0;
    titleLay->get_pixel_size(tfw, tfh);
    const double ov_title = idle ? 0 : (tfw - tw);

    // artist: greedy natural width (capped when an album shares the row),
    // ellipsized past the cap; album takes the rest and scrolls.
    Glib::RefPtr<Pango::Layout> artLay, albLay, sepLay;
    double aw = 0, alb_x = tx, alb_avail = 0, ov_alb = 0;
    if (!idle && !artist_.empty()) {
      artLay = area_.create_pango_layout(artist_);
      artLay->set_font_description(scaled_font("Sans", 9));
      int afw = 0, afh = 0;
      artLay->get_pixel_size(afw, afh);
      aw = std::min(static_cast<double>(afw), hasAlbum ? tw * 0.62 : tw);
      if (afw > aw + 1) {
        artLay->set_ellipsize(Pango::ELLIPSIZE_END);
        artLay->set_width(static_cast<int>(aw) * PANGO_SCALE);
      }
    }
    if (hasAlbum) {
      sepLay = area_.create_pango_layout(artist_.empty() ? "" : "  ·  ");
      sepLay->set_font_description(scaled_font("Sans", 9));
      int sw = 0, sfh = 0;
      sepLay->get_pixel_size(sw, sfh);
      alb_x = tx + aw + sw;
      alb_avail = std::max(10.0, tright - alb_x);
      albLay = area_.create_pango_layout(album_);
      albLay->set_font_description(scaled_font("Sans", 9));
      int lfw = 0, lfh = 0;
      albLay->get_pixel_size(lfw, lfh);
      ov_alb = lfw - alb_avail;
    }
    overflowing_ = ((ov_title > 2) || (ov_alb > 2)) && !idle;

    // ONE shared scroll cycle, sized by the longer of title/album so they reset
    // together (the shorter reaches its tail and waits out the cycle).
    const double tscr_max =
        std::max(std::max(0.0, ov_title), std::max(0.0, ov_alb)) / v;
    const double cyc = tlin + tscr_max + tend;
    double ph = std::fmod(now_mono() - marquee_t0_, cyc);
    if (ph < 0) ph += cyc;
    auto offOf = [&](double ov) -> double {
      if (ov <= 2 || status_ != "playing") return 0.0;
      const double tscr = ov / v;
      if (ph < tlin) return 0.0;
      if (ph < tlin + tscr) return (ph - tlin) * v;
      return ov;   // at its tail; hold until the cycle resets
    };

    // draw a (maybe scrolling) line clipped to its column, soft edge fades in
    // place of an ellipsis
    auto drawScroll = [&](const Glib::RefPtr<Pango::Layout>& lay, double x,
                          double y, double colw, double off, double ov,
                          double r, double g, double b) {
      cr->save();
      cr->rectangle(x, 0, colw, h);
      cr->clip();
      cr->push_group();
      cr->move_to(x - off, y);
      pango_cairo_layout_path(cr->cobj(), lay->gobj());
      if (text_outline_ > 0.0) {   // thick black border so names read over viz
        cr->set_line_join(Cairo::LINE_JOIN_ROUND);
        cr->set_line_width(text_outline_);
        cr->set_source_rgba(0, 0, 0, dim);
        cr->stroke_preserve();
      }
      cr->set_source_rgba(r, g, b, dim);
      cr->fill();
      cr->pop_group_to_source();
      const double fl = (off > 1) ? 12.0 : 0.0;
      const double fr = (ov > 2 && off < ov - 1) ? 14.0 : 0.0;
      if (fl > 0 || fr > 0) {
        auto m = Cairo::LinearGradient::create(x, 0, x + colw, 0);
        m->add_color_stop_rgba(0.0, 0, 0, 0, fl > 0 ? 0.0 : 1.0);
        if (fl > 0) m->add_color_stop_rgba(fl / colw, 0, 0, 0, 1.0);
        if (fr > 0) m->add_color_stop_rgba(1.0 - fr / colw, 0, 0, 0, 1.0);
        m->add_color_stop_rgba(1.0, 0, 0, 0, fr > 0 ? 0.0 : 1.0);
        cr->mask(m);
      } else {
        cr->paint();
      }
      cr->restore();
    };

    // title (row 1), bright white, marquee
    drawScroll(titleLay, tx, row1, tw, offOf(ov_title), ov_title, 0.95, 0.95,
               0.97);
    // outline a static (non-scrolling) layout at (x,y): thick black border
    // under a colour fill, so it reads over the analyzer.
    auto stroked = [&](const Glib::RefPtr<Pango::Layout>& lay, double x,
                       double y, double r, double g, double b) {
      cr->move_to(x, y);
      pango_cairo_layout_path(cr->cobj(), lay->gobj());
      if (text_outline_ > 0.0) {
        cr->set_line_join(Cairo::LINE_JOIN_ROUND);
        cr->set_line_width(text_outline_);
        cr->set_source_rgba(0, 0, 0, dim);
        cr->stroke_preserve();
      }
      cr->set_source_rgba(r, g, b, dim);
      cr->fill();
    };

    // artist (row 2), bright warm-white so it reads over the viz, static in its
    // greedy column
    if (artLay) {
      cr->save();
      cr->rectangle(tx, 0, aw + 1, h);
      cr->clip();
      stroked(artLay, tx, row2, 0.98, 0.98, 0.97);   // near-white
      cr->restore();
    }
    // separator + album (row 2), bright cool-white, marquee synced to title
    if (albLay) {
      if (sepLay) stroked(sepLay, tx + aw, row2, 0.92, 0.92, 0.94);
      drawScroll(albLay, alb_x, row2, alb_avail, offOf(ov_alb), ov_alb, 0.97,
                 0.98, 1.0);   // near-white
    }

    // progress: a thin hairline along the bottom, under the whole text block
    const double sy = h - pad + 0.5;
    const double sh = 2.0;
    const double frac =
        (length_ > 0) ? std::min(1.0, std::max(0.0, livePosition() / length_))
                      : 0.0;
    roundRect(tx, sy, tw, sh, sh / 2);
    cr->set_source_rgba(1, 1, 1, 0.12 * dim);
    cr->fill();
    if (frac > 0) {
      roundRect(tx, sy, std::max(sh, tw * frac), sh, sh / 2);
      cr->set_source_rgba(kProgR, kProgG, kProgB, idle ? 0.45 : 1.0);
      cr->fill();
    }
  }

  return true;
}

// ---- spectrum: peak-hold animation + dithered render ----------------------
bool Card::updateSpectrum() {
  if (!spec_) return false;
  const double now = now_mono();
  double dt = spec_last_ > 0 ? now - spec_last_ : 0.0;
  spec_last_ = now;
  if (dt > 0.1) dt = 0.1;   // clamp after a stall so caps don't jump
  spec_->read(levels_);     // 0..1 per band, already attack/decay-smoothed
  bool alive = false;
  const int n = static_cast<int>(levels_.size());
  if (static_cast<int>(caps_.size()) != n) {
    caps_.assign(n, 0.0f);
    cap_vel_.assign(n, 0.0f);
    cap_hold_.assign(n, 0.0);
  }
  for (int b = 0; b < n; ++b) {
    const float lv = levels_[b];
    if (lv >= caps_[b]) {            // new peak: snap cap up, reset hold
      caps_[b] = lv;
      cap_vel_[b] = 0.0f;
      cap_hold_[b] = cap_hold_s_;
    } else if (cap_hold_[b] > 0.0) {
      cap_hold_[b] -= dt;
    } else {                         // gravity: the cap accelerates downward
      cap_vel_[b] += static_cast<float>(cap_gravity_ * dt);
      caps_[b] -= cap_vel_[b] * static_cast<float>(dt);
      if (caps_[b] < lv) {
        caps_[b] = lv;
        cap_vel_[b] = 0.0f;
      }
      if (caps_[b] < 0.0f) caps_[b] = 0.0f;
    }
    if (lv > 0.004f || caps_[b] > 0.004f) alive = true;
  }
  return alive;
}

// Amplitude ramp: green (low) -> yellow (mid) -> red (crest). No white, so the
// coloured viz always contrasts the white/grey title, artist and controls.
static void spectrumRamp(double frac, double& r, double& g, double& b) {
  if (frac < 0.5) {
    const double t = frac / 0.5;
    r = 0.28 + t * 0.66;
    g = 0.84 + t * 0.06;
    b = 0.36 - t * 0.16;
  } else {
    const double t = (frac - 0.5) / 0.5;
    r = 0.94 + t * 0.04;
    g = 0.90 - t * 0.66;
    b = 0.20;
  }
}

void Card::drawSpectrum(const Cairo::RefPtr<Cairo::Context>& cr, double bx0,
                        double bx1, double h) {
  const int n = static_cast<int>(levels_.size());
  if (n <= 0) return;
  bool any = false;
  for (int b = 0; b < n; ++b)
    if (levels_[b] > 0.004f || caps_[b] > 0.004f) {
      any = true;
      break;
    }
  if (!any) return;

  const double x0 = bx0, x1 = bx1;
  const double top = 3.0, bot = h - 4.0;
  const double areaW = x1 - x0, areaH = bot - top;
  if (areaW <= 4 || areaH <= 4) return;
  const double colW = areaW / n;
  const double cell = std::max(2.0, (dot_h_ + dot_gap_) * ui_scale_);  // pitch
  const double dotW = std::max(1.6, colW * 0.6);
  const int rows = std::max(1, static_cast<int>(areaH / cell));

  // Stable per-cell jitter -> a "messy" scatter that does not strobe frame to
  // frame (hash of band+row, not a per-frame random).
  auto hash01 = [](int a, int b) {
    uint32_t hsh = static_cast<uint32_t>(a) * 73856093u ^
                   static_cast<uint32_t>(b) * 19349663u;
    hsh ^= hsh >> 13;
    hsh *= 0x5bd1e995u;
    hsh ^= hsh >> 15;
    return (hsh & 4095u) / 4095.0f;
  };

  for (int b = 0; b < n; ++b) {
    const double cx = x0 + (b + 0.5) * colW;
    const double lv = std::min(1.0, static_cast<double>(levels_[b]));
    const int lit = static_cast<int>(std::round(lv * rows));
    for (int r = 0; r < lit; ++r) {
      const double frac = (r + 0.5) / rows;   // 0 bottom .. 1 top of the well
      const double y = bot - (r + 1) * cell + cell * 0.5;
      const double jx = (hash01(b, r) - 0.5) * colW * 0.28;   // x scatter
      const double edge = static_cast<double>(r) / std::max(1, lit);
      // thin out toward the crest for a broken, dithered leading edge
      if (edge > 0.72 && hash01(b, r * 3 + 5) < (edge - 0.72) / 0.28 * 0.7)
        continue;
      const double a = spec_alpha_ * (0.74 + 0.26 * (1.0 - frac)) *
                       (0.62 + 0.38 * hash01(b * 7 + 1, r));
      double rr, gg, bb;
      spectrumRamp(frac, rr, gg, bb);
      cr->rectangle(cx - dotW / 2 + jx, y - cell * 0.32, dotW, cell * 0.64);
      cr->set_source_rgba(rr, gg, bb, a);
      cr->fill();
    }
    if (caps_[b] > 0.01f) {                        // peak-hold cap
      const double cap = std::min(1.0, static_cast<double>(caps_[b]));
      const double cy = bot - cap * areaH;
      // the ramp colour at the cap's own height, EMPHASIZED: a brighter, full-
      // alpha version (multiply keeps the hue, never washes to white) so the
      // peak reads as a bold marker -- bold yellow mid, bold red high.
      double cr_, cg_, cb_;
      spectrumRamp(cap, cr_, cg_, cb_);
      const double bo = 1.18;   // hue-preserving brighten
      cr->rectangle(cx - dotW / 2, cy - cell * 0.3, dotW, cell * 0.6);
      cr->set_source_rgba(std::min(1.0, cr_ * bo), std::min(1.0, cg_ * bo),
                          std::min(1.0, cb_ * bo),
                          std::min(1.0, spec_alpha_ * 1.2));
      cr->fill();
    }
  }
}

bool Card::on_button(GdkEventButton* e) {
  if (e->type != GDK_BUTTON_PRESS || e->button != 1) return false;
  const int x = static_cast<int>(e->x);
  if (x >= prev_x_ && x < prev_x_ + prev_w_)
    sendCtl("previous");
  else if (x >= next_x_ && x < next_x_ + next_w_)
    sendCtl("next");
  else
    sendCtl("playpause");   // play/pause glyph or anywhere else on the card
  return true;
}

bool Card::on_scroll(GdkEventScroll* e) {
  if (e->direction == GDK_SCROLL_UP)
    sendCtl("next");
  else if (e->direction == GDK_SCROLL_DOWN)
    sendCtl("previous");
  return true;
}

}  // namespace waybar::modules::media
