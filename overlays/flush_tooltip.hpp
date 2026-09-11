#pragma once

// A single shared tooltip popup, drawn as a gtk-layer-shell OVERLAY surface
// anchored FLUSH to the bar edge and pointing at the hovered module with a
// speech-bubble CALLOUT. The body is OFFSET to one side (away from the nearer
// screen edge) so it does not sit under the cursor/icon, and a tapered tail
// angles diagonally from the body's near top CORNER up to the bar edge under
// the icon. Header-only so it needs no meson wiring: AModule drives this for
// every module's tooltip; hw/gauge reuses anchorUnder() for its own popup.

#include <gtk-layer-shell.h>
#include <gtk/gtk.h>   // GtkTooltip, g_signal_* (native-tooltip kill)
#include <gtkmm/container.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>
#include <glibmm/main.h>
#include <glibmm/markup.h>
#include <gdkmm/display.h>
#include <gdkmm/general.h>   // Gdk::Cairo::set_source_pixbuf
#include <gdkmm/monitor.h>
#include <gdkmm/pixbuf.h>
#include <pangomm/layout.h>
#include <pangomm/tabarray.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

namespace waybar {

class FlushTooltip {
 public:
  static FlushTooltip& instance() {
    static FlushTooltip inst;
    return inst;
  }

  // Show `markup` as a callout pointing up at `module`, flush to the bar. Empty
  // markup hides. `centered` places the body centred under the icon with a
  // vertical tail (opt-in, e.g. the clock) instead of the default off-to-a-side
  // offset. Also suppresses `module`'s native GTK tooltip.
  void show(Gtk::Widget& module, const std::string& markup,
            bool centered = false) {
    if (markup.empty()) {
      hide();
      return;
    }
    if (win_ && win_->get_visible() && &module == last_target_ &&
        markup == last_markup_)
      return;   // same target + text; skip the per-motion redraw
    ensure();
    markup_ = markup;
    last_target_ = &module;
    last_markup_ = markup;

    // Scale the whole callout by the hovered module's BAR height (bar_h / 48),
    // so it tracks the larger 3-monitor-wall bar and stays 1.0 on the base bar.
    // Computed before the layout below, configureLayout() sizes by scale_.
    scale_ = 1.0;
    if (auto* top = module.get_toplevel()) {
      const int bh = top->get_allocated_height();
      if (bh > 0) scale_ = static_cast<double>(bh) / 48.0;
    }
    scale_ = kCallout * std::min(std::max(scale_, 0.75), 3.0);
    auto sc = [this](int base) {
      return static_cast<int>(base * scale_ + 0.5); };
    pad_ = sc(kPad);
    imgGap_ = sc(kImgGap);
    tailH_ = sc(kTailH);
    tailW_ = sc(kTailW);
    rad_ = sc(kRad);
    tipOut_ = sc(kTipOut);

    // optional album-art image for a rich tooltip, keyed by the module widget
    img_side_ = 0;
    {
      auto it = imageMap().find(&module);
      const std::string p =
          (it != imageMap().end()) ? it->second : std::string();
      if (p.empty()) {
        img_pix_.reset();
        img_path_.clear();
      } else if (p != img_path_) {
        img_path_ = p;
        try {
          img_pix_ = Gdk::Pixbuf::create_from_file(p);
        } catch (const Glib::Error&) {
          img_pix_.reset();
        }
      }
    }

    auto layout = area_.create_pango_layout("");
    configureLayout(layout);
    applyText(layout, markup_);
    int tw = 0, th = 0;
    layout->get_pixel_size(tw, th);
    if (img_pix_) img_side_ = th;   // square, the full text-block height
    body_w_ = 2 * pad_ + (img_side_ > 0 ? img_side_ + imgGap_ : 0) + tw;
    body_h_ = th + 2 * pad_;

    const Gdk::Rectangle mr = screenRect(module);
    const int icon_cx = mr.get_x() + mr.get_width() / 2;
    auto mwin = module.get_window();

    auto display = Gdk::Display::get_default();
    Glib::RefPtr<Gdk::Monitor> mon;
    if (display && mwin) mon = display->get_monitor_at_window(mwin);
    Gdk::Rectangle geo(0, 0, 100000, 100000);
    if (mon) {
      mon->get_geometry(geo);
      gtk_layer_set_monitor(win_->gobj(), mon->gobj());
    }
    // COORDINATE SPACE: on Wayland a layer-shell surface cannot know its global
    // position, so get_origin() (in screenRect) is 0,0 and icon_cx is monitor-
    // LOCAL (0..width); pointer x_root is likewise local, so hit test agrees.
    // geo, however, is GLOBAL (x = monitor's origin: 0, 2160, 4320 ...). The
    // layer margins we set are relative to the monitor gtk_layer_set_monitor()
    // pins us to, i.e. also LOCAL -- so only geo's WIDTH/HEIGHT are meaningful
    // here; its origin is treated as 0. Subtracting geo.get_x() (as this did)
    // pinned every tooltip to the left edge on the 2nd/3rd output. Keep all the
    // math below local.
    const int mon_w = geo.get_width();
    // Flip for a bottom bar (tail points DOWN, body above). Detect it from the
    // BAR's own layer-shell anchor -- a layer-shell surface's get_origin() is
    // unreliable (often 0,0), so icon screen-Y can't tell top from bottom.
    flip_ = false;
    if (auto* tw = dynamic_cast<Gtk::Window*>(module.get_toplevel())) {
      GtkWindow* bw = tw->gobj();
      if (gtk_layer_is_layer_window(bw))
        flip_ = gtk_layer_get_anchor(bw, GTK_LAYER_SHELL_EDGE_BOTTOM) &&
                !gtk_layer_get_anchor(bw, GTK_LAYER_SHELL_EDGE_TOP);
    }

    // Offset the body to one side so the icon (where the cursor sits) is over a
    // top CORNER, not the middle. Body extends away from the nearer edge; the
    // tail angles from that corner up to the icon.
    const bool icon_right = icon_cx > mon_w / 2;
    const int win_h = tailH_ + body_h_;
    int win_w, win_left;
    if (centered) {                   // body CENTRED under the icon
      win_w = body_w_;
      body_x_ = 0;
      win_left = icon_cx - body_w_ / 2;
    } else if (icon_right) {           // body LEFT of icon, tail from top-right
      win_w = body_w_ + tipOut_;
      body_x_ = 0;
      win_left = icon_cx - (body_w_ + tipOut_);
    } else {                          // body RIGHT of icon, tail from top-left
      win_w = body_w_ + tipOut_;
      body_x_ = tipOut_;
      win_left = icon_cx;
    }
    win_left = std::max(0, std::min(win_left, mon_w - win_w));

    // aim the tail apex at the icon; base at the centre (centred) or a corner
    tip_x_ = std::max(2, std::min(icon_cx - win_left, win_w - 2));
    const int near = body_x_ + rad_ + tailW_ / 2;
    const int far = body_x_ + body_w_ - rad_ - tailW_ / 2;
    const int want = centered ? tip_x_ : (icon_right ? far : near);
    base_cx_ = std::max(near, std::min(want, far));

    area_.set_size_request(win_w, win_h);
    auto* gw = win_->gobj();
    gtk_layer_set_anchor(gw, GTK_LAYER_SHELL_EDGE_LEFT, true);
    gtk_layer_set_anchor(gw, GTK_LAYER_SHELL_EDGE_RIGHT, false);
    gtk_layer_set_anchor(gw, GTK_LAYER_SHELL_EDGE_TOP, !flip_);
    gtk_layer_set_anchor(gw, GTK_LAYER_SHELL_EDGE_BOTTOM, flip_);
    gtk_layer_set_margin(gw, GTK_LAYER_SHELL_EDGE_LEFT, win_left);
    gtk_layer_set_margin(gw, flip_ ? GTK_LAYER_SHELL_EDGE_BOTTOM
                                   : GTK_LAYER_SHELL_EDGE_TOP,
                         0);

    area_.queue_draw();
    win_->show_all();
    killNativeTooltip(module);   // permanent source-level kill; no re-suppress
  }

  // Like show(), but only after `delay_ms` (0 == immediate). Re-hovering the
  // tooltip that is already up keeps it; a new target, empty markup, or hide()
  // cancels a pending show. Motion within a module reschedules, so the callout
  // appears only once the pointer settles for the delay (standard tooltip
  // behaviour). Instant modules (clock, gauges) pass delay 0.
  void requestShow(Gtk::Widget& module, const std::string& markup, int delay_ms,
                   bool centered = false) {
    if (markup.empty()) {
      hide();
      return;
    }
    if (win_ && win_->get_visible() && &module == last_target_ &&
        markup == last_markup_) {
      if (show_timer_.connected()) show_timer_.disconnect();
      return;   // already up for this target; no pending show needed
    }
    if (show_timer_.connected()) show_timer_.disconnect();
    if (delay_ms <= 0) {
      show(module, markup, centered);
      return;
    }
    // Kill GTK's native tooltip NOW (permanently), before the deferred show, so
    // its own ~500ms timer can never pop the old floating tip during the dwell.
    killNativeTooltip(module);
    pending_target_ = &module;
    pending_markup_ = markup;
    pending_centered_ = centered;
    show_timer_ = Glib::signal_timeout().connect(
        [this]() {
          if (pending_target_ != nullptr)
            show(*pending_target_, pending_markup_, pending_centered_);
          return false;   // one-shot
        },
        delay_ms);
  }

  void hide() {
    if (show_timer_.connected()) show_timer_.disconnect();
    pending_target_ = nullptr;
    if (win_) win_->hide();
    last_target_ = nullptr;
  }

  // First tooltip (markup preferred, else escaped text) set on `w` or any
  // descendant; empty if none. Reads the PROPERTY directly (not has-tooltip),
  // which killNativeTooltip() leaves intact -- so it is the flush callout's
  // source of truth.
  static std::string findTooltip(Gtk::Widget& w) {
    const auto m = w.get_tooltip_markup();
    if (!m.empty()) return m;
    const auto t = w.get_tooltip_text();
    if (!t.empty()) return Glib::Markup::escape_text(t);
    if (auto* c = dynamic_cast<Gtk::Container*>(&w)) {
      for (auto* child : c->get_children()) {
        auto s = findTooltip(*child);
        if (!s.empty()) return s;
      }
    }
    return {};
  }

  // Just `w`'s own tooltip (no recursion), markup preferred.
  static std::string ownTooltip(Gtk::Widget& w) {
    const auto m = w.get_tooltip_markup();
    if (!m.empty()) return m;
    const auto t = w.get_tooltip_text();
    if (!t.empty()) return Glib::Markup::escape_text(t);
    return {};
  }

  // `w`'s rectangle in absolute screen coordinates (via the toplevel), robust
  // for both windowed and windowless widgets.
  static Gdk::Rectangle screenRect(Gtk::Widget& w) {
    Gdk::Rectangle out(0, 0, 0, 0);
    auto* top = w.get_toplevel();
    if (!top) return out;
    int tx = 0, ty = 0;
    if (!w.translate_coordinates(*top, 0, 0, tx, ty)) return out;
    auto win = top->get_window();
    if (!win) return out;
    int ox = 0, oy = 0;
    win->get_origin(ox, oy);
    const auto a = w.get_allocation();
    return Gdk::Rectangle(ox + tx, oy + ty, a.get_width(), a.get_height());
  }

  // Deepest descendant of `root` (or root) that HAS a tooltip AND whose screen
  // rect contains (sx, sy) -- so a multi-icon module (the tray) shows the
  // tooltip of the icon actually under the pointer. nullptr if none.
  static Gtk::Widget* pickTooltipped(Gtk::Widget& root, int sx, int sy) {
    if (auto* c = dynamic_cast<Gtk::Container*>(&root)) {
      auto ch = c->get_children();
      for (auto it = ch.rbegin(); it != ch.rend(); ++it) {
        if (auto* hit = pickTooltipped(**it, sx, sy)) return hit;
      }
    }
    if (ownTooltip(root).empty()) return nullptr;
    const Gdk::Rectangle r = screenRect(root);
    if (r.get_width() > 0 && sx >= r.get_x() &&
        sx < r.get_x() + r.get_width() && sy >= r.get_y() &&
        sy < r.get_y() + r.get_height())
      return &root;
    return nullptr;
  }

  // Permanently stop GTK from ever displaying its OWN tooltip for `w` and its
  // current descendants: install a query-tooltip handler that HALTS the signal
  // before GTK's default handler (the one that would show the tooltip) and
  // returns FALSE, so the native tooltip window never maps. GTK only shows a
  // tooltip when query-tooltip ends TRUE, so stopping the emission is the
  // source-level kill. The tooltip-markup/text PROPERTY is left intact, so
  // ownTooltip()/findTooltip() still read it for the flush callout. Idempotent
  // per widget (guarded by a data flag) and permanent, so unlike clearing
  // has-tooltip there is nothing for a module's own tooltip updates to race
  // against -- this is the holistic fix, and it retires the re-suppress timer.
  static void killNativeTooltip(Gtk::Widget& w) {
    GtkWidget* gw = GTK_WIDGET(w.gobj());
    if (g_object_get_data(G_OBJECT(gw), "flush-native-killed") == nullptr) {
      g_object_set_data(G_OBJECT(gw), "flush-native-killed",
                        GINT_TO_POINTER(1));
      g_signal_connect(
          gw, "query-tooltip",
          G_CALLBACK(+[](GtkWidget* self, gint, gint, gboolean, GtkTooltip*,
                         gpointer) -> gboolean {
            g_signal_stop_emission_by_name(self, "query-tooltip");
            return FALSE;   // never let GTK's own tooltip show
          }),
          nullptr);
    }
    if (auto* c = dynamic_cast<Gtk::Container*>(&w)) {
      for (auto* child : c->get_children()) killNativeTooltip(*child);
    }
  }

  // Anchor a caller-owned layer-shell window flush to the bar, left edge under
  // `module` (used by hw/gauge's own popup).
  static void anchorUnder(Gtk::Window& win, Gtk::Widget& module) {
    auto* gw = win.gobj();
    int mx = 0, my = 0;
    auto mwin = module.get_window();
    if (mwin) mwin->get_origin(mx, my);
    auto display = Gdk::Display::get_default();
    Glib::RefPtr<Gdk::Monitor> mon;
    if (display && mwin) mon = display->get_monitor_at_window(mwin);
    int left = mx;
    if (mon) {
      Gdk::Rectangle geo;
      mon->get_geometry(geo);
      left = mx - geo.get_x();
      gtk_layer_set_monitor(gw, mon->gobj());
    }
    gtk_layer_set_anchor(gw, GTK_LAYER_SHELL_EDGE_LEFT, true);
    gtk_layer_set_anchor(gw, GTK_LAYER_SHELL_EDGE_RIGHT, false);
    gtk_layer_set_anchor(gw, GTK_LAYER_SHELL_EDGE_TOP, true);
    gtk_layer_set_anchor(gw, GTK_LAYER_SHELL_EDGE_BOTTOM, false);
    gtk_layer_set_margin(gw, GTK_LAYER_SHELL_EDGE_LEFT, left);
    gtk_layer_set_margin(gw, GTK_LAYER_SHELL_EDGE_TOP, 0);
  }

  // Optional image for a module's RICH tooltip (album art), keyed by the module
  // widget. A module sets this with its tooltip markup; show() renders it as
  // a full-height rounded thumbnail left of the text. Empty path clears it.
  static std::map<Gtk::Widget*, std::string>& imageMap() {
    static std::map<Gtk::Widget*, std::string> m;
    return m;
  }
  static void setImage(Gtk::Widget& w, const std::string& path) {
    if (path.empty())
      imageMap().erase(&w);
    else
      imageMap()[&w] = path;
  }

 private:
  // Base geometry, tuned for the 48px bar. show() scales copies of these (and
  // the font) by scale_ = bar_h / 48, so the callout grows with the bar height
  // -- ~1.25x on the 3-monitor wall's 60px bar, 1.0 on the base bar (manifold
  // untouched). Keyed off the hovered module's bar, exactly like media/card.
  static constexpr int kPad = 9;      // body text padding
  static constexpr int kImgGap = 10;  // gap between image and text
  static constexpr int kTailH = 26;   // tail height (gap from bar to body)
  static constexpr int kTailW = 16;   // tail base width
  static constexpr int kRad = 7;      // body corner radius
  static constexpr int kTipOut = 12;  // apex overhang past the body corner
  static constexpr double kCallout = 1.3;  // overall callout size (x the bar
                                           // -height scale): 1.3 = 30% larger

  double scale_ = 1.0;   // callout size vs the base 48px bar (bar_h / 48)
  int pad_ = kPad, imgGap_ = kImgGap, tailH_ = kTailH,   // scaled copies, set
      tailW_ = kTailW, rad_ = kRad, tipOut_ = kTipOut;   // per show()

  FlushTooltip() = default;

  void ensure() {
    if (win_) return;
    win_ = new Gtk::Window(Gtk::WINDOW_TOPLEVEL);
    win_->set_name("flush-tooltip");
    win_->set_decorated(false);
    win_->set_app_paintable(true);
    if (auto vis = win_->get_screen()->get_rgba_visual())
      gtk_widget_set_visual(GTK_WIDGET(win_->gobj()), vis->gobj());
    win_->add(area_);
    area_.signal_draw().connect(sigc::mem_fun(*this, &FlushTooltip::on_draw));
    gtk_layer_init_for_window(win_->gobj());
    gtk_layer_set_layer(win_->gobj(), GTK_LAYER_SHELL_LAYER_OVERLAY);
    area_.show();
  }

  // Base tooltip text layout: the font plus one tab stop, so a rich tooltip can
  // tab-align "Label<tab>value" columns. Harmless for plain tooltips (no tab).
  void configureLayout(const Glib::RefPtr<Pango::Layout>& l) const {
    Pango::FontDescription d("Sans");
    d.set_size(static_cast<int>(11 * scale_ * PANGO_SCALE + 0.5));   // 11pt
    l->set_font_description(d);
    // Pango units (PANGO_SCALE per px) -- unambiguous; the pixel-flag form is
    // unreliable and lands the stop near 0, which staggers by label length.
    Pango::TabArray tabs(1, false);
    tabs.set_tab(0, Pango::TAB_LEFT,
                 static_cast<int>(96 * scale_) * PANGO_SCALE);   // widest label
    l->set_tabs(tabs);
  }

  // Set the layout from `s`, treating it as Pango markup only if it PARSES --
  // otherwise plain text. A tooltip-format like "Places & bookmarks" carries a
  // bare '&', which is invalid markup and would render an empty label.
  static void applyText(const Glib::RefPtr<Pango::Layout>& layout,
                        const std::string& s) {
    if (pango_parse_markup(s.c_str(), -1, 0, nullptr, nullptr, nullptr,
                           nullptr))
      layout->set_markup(s);
    else
      layout->set_text(s);
  }

  // Outline: a rounded body with a tapered tail poking out of its top edge at
  // base_cx_, apex at (tip_x_, 0) -- the bar edge under the icon.
  void path(const Cairo::RefPtr<Cairo::Context>& cr) const {
    const double r = rad_, bx = body_x_, bw = body_w_, bh = body_h_;
    const double bcx = base_cx_, hw = tailW_ / 2.0, tx = tip_x_, kPi = M_PI;
    const double yT = flip_ ? 0.0 : tailH_;            // body top edge
    const double yB = yT + bh;                         // body bottom edge
    const double tipy = flip_ ? yB + tailH_ - 0.5 : 0.5;
    cr->begin_new_path();
    if (!flip_) {                                      // tail on the TOP edge
      cr->move_to(bx + r, yT);
      cr->line_to(bcx - hw, yT);
      cr->line_to(tx, tipy);
      cr->line_to(bcx + hw, yT);
      cr->line_to(bx + bw - r, yT);
      cr->arc(bx + bw - r, yT + r, r, -0.5 * kPi, 0.0);
      cr->line_to(bx + bw, yB - r);
      cr->arc(bx + bw - r, yB - r, r, 0.0, 0.5 * kPi);
      cr->line_to(bx + r, yB);
      cr->arc(bx + r, yB - r, r, 0.5 * kPi, kPi);
      cr->line_to(bx, yT + r);
      cr->arc(bx + r, yT + r, r, kPi, 1.5 * kPi);
    } else {                                           // tail: BOTTOM edge
      cr->move_to(bx + r, yT);
      cr->line_to(bx + bw - r, yT);
      cr->arc(bx + bw - r, yT + r, r, -0.5 * kPi, 0.0);
      cr->line_to(bx + bw, yB - r);
      cr->arc(bx + bw - r, yB - r, r, 0.0, 0.5 * kPi);
      cr->line_to(bcx + hw, yB);
      cr->line_to(tx, tipy);
      cr->line_to(bcx - hw, yB);
      cr->line_to(bx + r, yB);
      cr->arc(bx + r, yB - r, r, 0.5 * kPi, kPi);
      cr->line_to(bx, yT + r);
      cr->arc(bx + r, yT + r, r, kPi, 1.5 * kPi);
    }
    cr->close_path();
  }

  bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) {
    cr->set_operator(Cairo::OPERATOR_CLEAR);
    cr->paint();
    cr->set_operator(Cairo::OPERATOR_OVER);

    path(cr);
    cr->set_source_rgba(0.157, 0.157, 0.157, 0.96);    // gauge dark-grey body
    cr->fill_preserve();
    cr->set_line_width(1.5);
    cr->set_source_rgba(0.82, 0.84, 0.90, 0.90);       // bright border
    cr->stroke();

    const double yT = (flip_ ? 0 : tailH_) + pad_;   // content top

    // album art on the left, full text-height, rounded + cover-fit
    if (img_pix_ && img_side_ > 0) {
      const double ix = body_x_ + pad_, iy = yT, s = img_side_, r = 5 * scale_;
      cr->save();
      cr->begin_new_sub_path();
      cr->arc(ix + s - r, iy + r, r, -0.5 * M_PI, 0.0);
      cr->arc(ix + s - r, iy + s - r, r, 0.0, 0.5 * M_PI);
      cr->arc(ix + r, iy + s - r, r, 0.5 * M_PI, M_PI);
      cr->arc(ix + r, iy + r, r, M_PI, 1.5 * M_PI);
      cr->close_path();
      cr->clip();
      const double pw = img_pix_->get_width(), ph = img_pix_->get_height();
      const double sc = s / std::min(pw, ph);
      cr->translate(ix + s / 2, iy + s / 2);
      cr->scale(sc, sc);
      Gdk::Cairo::set_source_pixbuf(cr, img_pix_, -pw / 2, -ph / 2);
      cr->paint();
      cr->restore();
    }

    auto layout = area_.create_pango_layout("");
    configureLayout(layout);
    applyText(layout, markup_);
    cr->move_to(body_x_ + pad_ + (img_side_ > 0 ? img_side_ + imgGap_ : 0), yT);
    cr->set_source_rgba(0.902, 0.902, 0.902, 1.0);     // #e6e6e6 like the gauge
    layout->show_in_cairo_context(cr);
    return true;
  }

  Gtk::Window* win_ = nullptr;
  Gtk::DrawingArea area_;
  std::string markup_;
  int body_w_ = 0, body_h_ = 0, body_x_ = 0, tip_x_ = 0, base_cx_ = 0;
  bool flip_ = false;   // bottom bar: tail points DOWN, body sits above
  Gtk::Widget* last_target_ = nullptr;   // skip redundant per-motion redraws
  std::string last_markup_;
  Glib::RefPtr<Gdk::Pixbuf> img_pix_;    // decoded rich-tooltip image
  std::string img_path_;                 // cached image path (decode once)
  int img_side_ = 0;                     // 0 = no image on this show
  sigc::connection show_timer_;          // pending delayed show (requestShow)
  Gtk::Widget* pending_target_ = nullptr;
  std::string pending_markup_;
  bool pending_centered_ = false;
};

}  // namespace waybar
