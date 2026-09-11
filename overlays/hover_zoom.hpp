#pragma once

// HoverZoom -- a shared, macOS-dock-style hover magnifier for ANY icon-shaped
// widget in the bar. On hover of a widget that sits under a ".zoomable"
// ancestor, it CAPTURES the widget's rendered pixels (gtk_widget_draw) and
// redraws them MAGNIFIED, with a soft halo, on a layer-shell OVERLAY surface
// floating above the bar. Because it is a separate surface, the zoomed icon and
// halo bleed freely OVER neighbours and past bar edges -- no per-widget clip,
// no per-icon CSS zoom/glow, no ink-region trick. And because it magnifies
// captured pixels not re-rendered from a source, it is agnostic to what
// the widget is: a CSS launcher, a Cairo gauge dial, a foreign StatusNotifier
// tray icon -- one mechanism covers them all.
//
// Wiring (see the waybar-hover-zoom patch): AModule calls attach(event_box_) on
// every module, and adds the CSS class "zoomable" to any module/group whose
// config carries "zoomable": true. Opt-in is therefore per CLUSTER: tag a group
// (group/launchers, group/gadgets, ...) and every leaf inside it -- launchers,
// gauges, tray icons -- inherits the effect. sysmon and now-playing are simply
// not tagged. Header-only, exactly like flush_tooltip.hpp.

#include <gtk-layer-shell.h>
#include <gtk/gtk.h>
#include <gtkmm/container.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/eventbox.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>
#include <cairomm/context.h>
#include <cairomm/region.h>
#include <glibmm/main.h>
#include <gdkmm/display.h>
#include <gdkmm/general.h>
#include <gdkmm/monitor.h>

#include <algorithm>
#include <cmath>

#include "flush_tooltip.hpp"   // reuse screenRect()

namespace waybar {

class HoverZoom {
 public:
  static HoverZoom& instance() {
    static HoverZoom inst;
    return inst;
  }

  // Wire a module: any leaf under a .zoomable ancestor magnifies on hover. The
  // gate is evaluated at hover time (class added in the AModule ctor), so
  // this can be attached unconditionally -- it no-ops for non-zoomable modules.
  static void attach(Gtk::EventBox& ebox) {
    ebox.add_events(Gdk::POINTER_MOTION_MASK);
    ebox.signal_enter_notify_event().connect([&ebox](GdkEventCrossing* e) {
      instance().hover(ebox, static_cast<int>(e->x_root),
                       static_cast<int>(e->y_root));
      return false;
    });
    ebox.signal_motion_notify_event().connect([&ebox](GdkEventMotion* e) {
      instance().hover(ebox, static_cast<int>(e->x_root),
                       static_cast<int>(e->y_root));
      return false;
    });
    ebox.signal_leave_notify_event().connect([&ebox](GdkEventCrossing* e) {
      // A crossing INTO a child window (e.g. a gauge DrawingArea) is still
      // inside the module -- keep the zoom up; only a real exit hides it.
      if (e->detail == GDK_NOTIFY_INFERIOR) return false;
      instance().leaveFrom(&ebox);
      return false;
    });
  }

  // Pointer at (sx,sy) [monitor-local root coords] over module `root`.
  void hover(Gtk::Widget& root, int sx, int sy) {
    if (!ancestorHasClass(root, "zoomable")) return;   // skip non-clusters
    Gtk::Widget* leaf = deepestAt(root, sx, sy);
    // Zoom only a genuine leaf ICON that opts in. Skip: a multi-child container
    // (the pointer is in a GAP between icons -- e.g. tray items -- where
    // deepestAt lands on the container, not an icon); and anything vetoed with
    // a "no-zoom" nearer than the cluster's "zoomable" (separators, the wide
    // copyq readout).
    if (leaf == nullptr || !isLeafIcon(*leaf) || !zoomGate(*leaf)) {
      leaveFrom(&root);
      return;
    }
    if (leaf == target_) return;   // already magnifying this icon
    show(*leaf);
  }

  // Hide only if the icon we are showing belongs to `root` (so a leave on one
  // module does not cancel a zoom that a neighbouring enter just started).
  void leaveFrom(Gtk::Widget* root) {
    if (target_ == nullptr) return;
    if (root != nullptr && !isDescendant(target_, root)) return;
    startClose();
  }

 private:
  static constexpr double kZoom = 1.55;   // magnification at full hover
  static constexpr double kHalo = 0.18;   // halo reach past the icon (x height)
  static constexpr int kAnimMs = 120;     // grow/shrink duration
  static constexpr int kFrameMs = 16;     // ~60fps
  static constexpr int kSuper = 3;        // capture supersample (crisp magnify)
  static constexpr int kRecapMs = 100;    // re-capture cadence for live gauges

  HoverZoom() = default;

  // --- widget-tree helpers ---------------------------------------------------
  static bool ancestorHasClass(Gtk::Widget& w, const char* cls) {
    for (Gtk::Widget* p = &w; p != nullptr; p = p->get_parent())
      if (p->get_style_context()->has_class(cls)) return true;
    return false;
  }

  // Opt-in with a veto: walking up from the leaf, whichever of "no-zoom" /
  // "zoomable" is NEAREST wins. A cluster tagged "zoomable" covers its leaves,
  // but a member tagged "no-zoom" (separator, copyq readout) opts back out.
  static bool zoomGate(Gtk::Widget& w) {
    for (Gtk::Widget* p = &w; p != nullptr; p = p->get_parent()) {
      auto ctx = p->get_style_context();
      if (ctx->has_class("no-zoom")) return false;
      if (ctx->has_class("zoomable")) return true;
    }
    return false;
  }

  // A single icon, not a gap. A container with 2+ kids is a cluster/gap (the
  // tray box between icons, a group box between modules) -- don't magnify it.
  static bool isLeafIcon(Gtk::Widget& w) {
    auto* c = dynamic_cast<Gtk::Container*>(&w);
    return c == nullptr || c->get_children().size() <= 1;
  }

  static bool isDescendant(Gtk::Widget* w, Gtk::Widget* of) {
    for (Gtk::Widget* p = w; p != nullptr; p = p->get_parent())
      if (p == of) return true;
    return false;
  }

  // Deepest descendant of `root` whose rect contains (sx,sy). This is the
  // individual icon under the pointer -- the tray icon, the launcher, the gauge
  // DrawingArea -- not the whole cluster.
  static Gtk::Widget* deepestAt(Gtk::Widget& root, int sx, int sy) {
    const Gdk::Rectangle r = FlushTooltip::screenRect(root);
    if (r.get_width() <= 0 || sx < r.get_x() ||
        sx >= r.get_x() + r.get_width() || sy < r.get_y() ||
        sy >= r.get_y() + r.get_height())
      return nullptr;
    Gtk::Widget* best = &root;
    if (auto* c = dynamic_cast<Gtk::Container*>(&root)) {
      for (auto* ch : c->get_children())
        if (auto* hit = deepestAt(*ch, sx, sy)) best = hit;
    }
    return best;
  }

  // --- overlay surface -------------------------------------------------------
  void ensure() {
    if (win_ != nullptr) return;
    win_ = new Gtk::Window(Gtk::WINDOW_TOPLEVEL);
    win_->set_name("hover-zoom");
    win_->set_decorated(false);
    win_->set_app_paintable(true);
    if (auto vis = win_->get_screen()->get_rgba_visual())
      gtk_widget_set_visual(GTK_WIDGET(win_->gobj()), vis->gobj());
    win_->add(area_);
    area_.signal_draw().connect(sigc::mem_fun(*this, &HoverZoom::on_draw));
    gtk_layer_init_for_window(win_->gobj());
    gtk_layer_set_layer(win_->gobj(), GTK_LAYER_SHELL_LAYER_OVERLAY);
    // Exclusive-zone -1: anchor to the TRUE screen edge, ignoring the bar's
    // reserved strip, so the overlay OVERLAPS the bar. Without this a top-edge
    // surface is placed BELOW the bar's exclusive zone (at y=bar_height), which
    // is why the magnified icon rendered under the bar instead of over it.
    gtk_layer_set_exclusive_zone(win_->gobj(), -1);
    // Click-through: the magnified icon sits UNDER the cursor, so the overlay
    // must not capture pointer events (or it would eat the hover it depends on,
    // and swallow clicks meant for the real icon). Empty input region on map.
    win_->signal_map().connect([this]() {
      if (auto w = win_->get_window())
        w->input_shape_combine_region(Cairo::Region::create(), 0, 0);
    });
    area_.show();
  }

  // Capture `leaf`'s current appearance into an ARGB surface at its own size
  // (HiDPI-correct via device scale). Base state, since we no longer apply a
  // CSS hover zoom -- so the capture is always the clean icon.
  void capture(Gtk::Widget& leaf) {
    const auto a = leaf.get_allocation();
    cap_w_ = a.get_width();
    cap_h_ = a.get_height();
    if (cap_w_ <= 0 || cap_h_ <= 0) {
      cap_.clear();
      return;
    }
    // Supersample: render the capture at kSuper x the display scale so a CSS
    // background-image (re-rastered by GTK from its source at the target
    // resolution) stays crisp when magnified. Neutral for already-small icons.
    const int s = kSuper * std::max(1, leaf.get_scale_factor());
    cap_ = Cairo::ImageSurface::create(Cairo::FORMAT_ARGB32, cap_w_ * s,
                                       cap_h_ * s);
    cairo_surface_set_device_scale(cap_->cobj(), s, s);
    auto cr = Cairo::Context::create(cap_);
    // gtk_widget_draw renders the widget to the current origin (0,0), so the
    // icon lands filling the surface. It also applies the widget's OWN opacity,
    // and while zoomed we hide the original (opacity 0) so a hollow glyph does
    // not double up behind the magnified copy -- so force full opacity JUST for
    // the capture, then restore. Synchronous, so the live widget never repaints
    // visible in between.
    const double op = leaf.get_opacity();
    if (op < 1.0) leaf.set_opacity(1.0);
    gtk_widget_draw(GTK_WIDGET(leaf.gobj()), cr->cobj());
    if (op < 1.0) leaf.set_opacity(op);
  }

  void show(Gtk::Widget& leaf) {
    ensure();
    // Switching icons: un-hide the previous one before taking over the overlay.
    if (target_ != nullptr && target_ != &leaf)
      target_->set_opacity(1.0);
    target_ = &leaf;
    capture(leaf);            // captures at full opacity (leaf still visible)
    if (!cap_) {
      hideNow();
      return;
    }
    // Hide the ORIGINAL while its magnified copy is up, so a hollow icon (the
    // power glyph, a wifi wedge) shows plain bar through it -- not a second,
    // un-zoomed icon doubling behind the magnified one.
    leaf.set_opacity(0.0);

    // Leaf rect + monitor (monitor-local coords; a layer-shell toplevel reports
    // origin 0,0, so screenRect is already monitor-local -- see flush_tooltip).
    const Gdk::Rectangle lr = FlushTooltip::screenRect(leaf);
    auto lwin = leaf.get_window();
    auto display = Gdk::Display::get_default();
    Glib::RefPtr<Gdk::Monitor> mon;
    if (display && lwin) mon = display->get_monitor_at_window(lwin);
    Gdk::Rectangle geo(0, 0, 100000, 48);
    if (mon) {
      mon->get_geometry(geo);
      gtk_layer_set_monitor(win_->gobj(), mon->gobj());
    }
    const int mon_w = geo.get_width();

    // Bottom bar? A layer-shell toplevel's origin is unreliable, so read the
    // bar's own anchor to know which edge to pin to (mirrors flush_tooltip).
    flip_ = false;
    int bar_h = 48;
    if (auto* top = dynamic_cast<Gtk::Window*>(leaf.get_toplevel())) {
      bar_h = std::max(1, top->get_allocated_height());
      GtkWindow* bw = top->gobj();
      if (gtk_layer_is_layer_window(bw))
        flip_ = gtk_layer_get_anchor(bw, GTK_LAYER_SHELL_EDGE_BOTTOM) &&
                !gtk_layer_get_anchor(bw, GTK_LAYER_SHELL_EDGE_TOP);
    }

    // Overlay box: big enough for the icon at full zoom + halo, centred on the
    // icon. Horizontal margin is from the LEFT edge; vertical is from whichever
    // bar edge we are anchored to.
    // Size the zoom+halo off the icon HEIGHT, never the width -- a wide readout
    // (the copyq preview, the battery) must not get a width-sized halo.
    const double base = lr.get_height();
    const double halfMag = base * (kZoom * 0.5 + kHalo);   // icon+halo half

    icon_cx_ = lr.get_x() + lr.get_width() / 2.0;             // monitor-local x
    const double icon_cy = lr.get_y() + lr.get_height() / 2.0;  // y in the bar
    // Icon centre's distance from whichever bar edge we anchor to.
    const double edge_dist = flip_ ? (bar_h - icon_cy) : icon_cy;

    win_w_ = static_cast<int>(lr.get_width() * (kZoom + 2 * kHalo)) + 4;
    // A layer-shell surface cannot extend PAST its anchored edge (a negative
    // margin is clamped to 0), so we do NOT centre the window on the icon.
    // Instead pin it FLUSH to the bar edge (margin 0) and place the icon at its
    // true offset inside the window -- the magnified icon straddles the bar
    // and grows out into the desktop, dock-style, instead of landing below it.
    win_h_ = static_cast<int>(edge_dist + halfMag) + 6;
    const int vmargin = 0;

    int left = static_cast<int>(icon_cx_ - win_w_ / 2.0);
    left = std::max(0, std::min(left, mon_w - win_w_));

    ov_cx_ = icon_cx_ - left;
    ov_cy_ = flip_ ? (win_h_ - edge_dist) : edge_dist;

    area_.set_size_request(win_w_, win_h_);
    auto* gw = win_->gobj();
    gtk_layer_set_anchor(gw, GTK_LAYER_SHELL_EDGE_LEFT, true);
    gtk_layer_set_anchor(gw, GTK_LAYER_SHELL_EDGE_RIGHT, false);
    gtk_layer_set_anchor(gw, GTK_LAYER_SHELL_EDGE_TOP, !flip_);
    gtk_layer_set_anchor(gw, GTK_LAYER_SHELL_EDGE_BOTTOM, flip_);
    gtk_layer_set_margin(gw, GTK_LAYER_SHELL_EDGE_LEFT, left);
    gtk_layer_set_margin(gw, flip_ ? GTK_LAYER_SHELL_EDGE_BOTTOM
                                   : GTK_LAYER_SHELL_EDGE_TOP,
                         vmargin);

    closing_ = false;
    win_->show();
    startAnim();
    if (!recap_.connected())
      recap_ = Glib::signal_timeout().connect(
          sigc::mem_fun(*this, &HoverZoom::onRecap), kRecapMs);
  }

  // Keep the magnified image LIVE while it's shown: re-capture the target so a
  // gauge that updates underneath (a moving needle, a scrolled volume knob, a
  // ticking battery) is reflected in the zoom instead of a frozen snapshot.
  bool onRecap() {
    if (target_ == nullptr || closing_ || win_ == nullptr ||
        !win_->get_visible())
      return false;   // one-shot stop; restarted by the next show()
    capture(*target_);
    area_.queue_draw();
    return true;
  }

  // --- animation -------------------------------------------------------------
  void startAnim() {
    if (!anim_.connected())
      anim_ = Glib::signal_timeout().connect(
          sigc::mem_fun(*this, &HoverZoom::onFrame), kFrameMs);
  }
  void startClose() {
    closing_ = true;
    if (recap_.connected()) recap_.disconnect();   // freeze during the shrink
    startAnim();
  }
  bool onFrame() {
    const double step = static_cast<double>(kFrameMs) / kAnimMs;
    const double goal = closing_ ? 0.0 : 1.0;
    if (t_ < goal)
      t_ = std::min(goal, t_ + step);
    else if (t_ > goal)
      t_ = std::max(goal, t_ - step);
    area_.queue_draw();
    if (std::abs(t_ - goal) < 1e-3) {
      t_ = goal;
      area_.queue_draw();
      if (closing_) hideNow();
      anim_.disconnect();
      return false;
    }
    return true;
  }
  void hideNow() {
    if (recap_.connected()) recap_.disconnect();
    if (target_ != nullptr) target_->set_opacity(1.0);   // un-hide the original
    if (win_) win_->hide();
    target_ = nullptr;
    t_ = 0.0;
    closing_ = false;
    cap_.clear();
  }

  // eased 0..1
  static double ease(double t) { return t * t * (3.0 - 2.0 * t); }

  // --- draw ------------------------------------------------------------------
  bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) {
    cr->set_operator(Cairo::OPERATOR_CLEAR);
    cr->paint();
    cr->set_operator(Cairo::OPERATOR_OVER);
    if (!cap_ || cap_w_ <= 0) return true;

    const double e = ease(t_);
    const double scale = 1.0 + (kZoom - 1.0) * e;
    const double base = cap_h_;   // halo off HEIGHT, so a wide icon stays sane

    // Halo: a soft green-white radial bloom behind the icon, fading in with the
    // zoom. Colour matches the launcher glow (#cef4a8); kept faint on purpose.
    const double hr = base * (0.5 * scale + kHalo);
    auto halo = Cairo::RadialGradient::create(
        ov_cx_, ov_cy_, base * 0.4 * scale,
                                              ov_cx_, ov_cy_, hr);
    halo->add_color_stop_rgba(0.0, 0.807, 0.957, 0.658, 0.24 * e);
    halo->add_color_stop_rgba(0.55, 0.807, 0.957, 0.658, 0.07 * e);
    halo->add_color_stop_rgba(1.0, 0.807, 0.957, 0.658, 0.0);
    cr->set_source(halo);
    cr->arc(ov_cx_, ov_cy_, hr, 0, 2 * M_PI);
    cr->fill();

    // The captured icon, scaled about its centre.
    cr->save();
    cr->translate(ov_cx_, ov_cy_);
    cr->scale(scale, scale);
    cr->set_source(cap_, -cap_w_ / 2.0, -cap_h_ / 2.0);
    cr->paint();
    cr->restore();
    return true;
  }

  Gtk::Window* win_ = nullptr;
  Gtk::DrawingArea area_;
  Gtk::Widget* target_ = nullptr;          // icon currently magnified

  Cairo::RefPtr<Cairo::ImageSurface> cap_;  // captured icon pixels
  int cap_w_ = 0, cap_h_ = 0;

  int win_w_ = 0, win_h_ = 0;
  double ov_cx_ = 0, ov_cy_ = 0;           // icon centre in overlay coords
  double icon_cx_ = 0;
  bool flip_ = false;

  double t_ = 0.0;                          // animation progress 0..1
  bool closing_ = false;
  sigc::connection anim_;
  sigc::connection recap_;                  // live re-capture while shown
};

}  // namespace waybar
