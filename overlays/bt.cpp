#include "modules/hw/bluetooth.hpp"

#include <cmath>
#include <cstdio>

#include <spdlog/spdlog.h>

namespace waybar::modules::hw {

static constexpr double kPi = 3.14159265358979323846;

// --- BlueZ D-Bus helpers (cached properties, no round trip) ----------------
namespace {
bool getBool(GDBusProxy* proxy, const char* name) {
  bool v = false;
  if (auto* g = g_dbus_proxy_get_cached_property(proxy, name)) {
    v = g_variant_get_boolean(g);
    g_variant_unref(g);
  }
  return v;
}
std::string getStr(GDBusProxy* proxy, const char* name) {
  std::string v;
  if (auto* g = g_dbus_proxy_get_cached_property(proxy, name)) {
    v = g_variant_get_string(g, nullptr);
    g_variant_unref(g);
  }
  return v;
}
// The manager fires these on connect/disconnect/battery change; each just pokes
// the module to re-read the cached props and redraw.
void on_obj(GDBusObjectManager*, GDBusObject*, gpointer d) {
  static_cast<Bluetooth*>(d)->update();
}
void on_props(GDBusObjectManagerClient*, GDBusObjectProxy*, GDBusProxy*,
              GVariant*, const gchar* const*, gpointer d) {
  static_cast<Bluetooth*>(d)->update();
}
}  // namespace

Bluetooth::Bluetooth(const std::string& id, const waybar::Bar& bar,
                     const Json::Value& config)
    : AModule(config, "bluetooth", id, /*enable_click=*/true,
              /*enable_scroll=*/false),
      bar_(bar) {
  if (config_["width"].isInt()) width_ = config_["width"].asInt();
  if (config_["vmargin"].isInt()) vmargin_ = config_["vmargin"].asInt();
  if (config_["hmargin"].isInt()) hmargin_ = config_["hmargin"].asInt();

  // Self-scale with the bar height (48 = design), like hw/gauge.
  const double ui = bar_.config["height"].isInt()
                        ? bar_.config["height"].asInt() / 48.0
                        : 1.0;
  width_ = static_cast<int>(width_ * ui + 0.5);
  // Rune-only width: snug around the lone glyph, so the widget shrinks when no
  // battery is present and reflows to the full width when a device reports one.
  width_narrow_ = static_cast<int>(width_ * 0.56 + 0.5);

  GError* err = nullptr;
  manager_ = g_dbus_object_manager_client_new_for_bus_sync(
      G_BUS_TYPE_SYSTEM,
      G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START, "org.bluez", "/",
      nullptr, nullptr, nullptr, nullptr, &err);
  if (err) {
    spdlog::warn("hw/bluetooth: bluez manager failed: {}", err->message);
    g_error_free(err);
  }

  read_state();
  if (!have_adapter_) {   // no controller -> collapse (like the gauges)
    event_box_.set_no_show_all(true);
    event_box_.hide();
    return;
  }

  event_box_.set_name(id.empty() ? "bluetooth" : "bluetooth-" + id);
  const bool batt0 = connected_ && battery_.has_value();
  area_.set_size_request(batt0 ? width_ : width_narrow_, -1);
  area_.set_margin_top(vmargin_);
  area_.set_margin_bottom(vmargin_);
  area_.set_margin_start(hmargin_);
  area_.set_margin_end(hmargin_);
  area_.signal_draw().connect(sigc::mem_fun(*this, &Bluetooth::on_draw));
  event_box_.add(area_);
  event_box_.show_all();

  if (manager_) {
    sig_add_ = g_signal_connect(manager_, "object-added", G_CALLBACK(on_obj),
                                this);
    sig_del_ = g_signal_connect(manager_, "object-removed", G_CALLBACK(on_obj),
                                this);
    sig_chg_ = g_signal_connect(manager_, "interface-proxy-properties-changed",
                                G_CALLBACK(on_props), this);
  }
  update();
}

Bluetooth::~Bluetooth() {
  if (manager_) {
    if (sig_add_) g_signal_handler_disconnect(manager_, sig_add_);
    if (sig_del_) g_signal_handler_disconnect(manager_, sig_del_);
    if (sig_chg_) g_signal_handler_disconnect(manager_, sig_chg_);
    g_object_unref(manager_);
  }
}

void Bluetooth::read_state() {
  have_adapter_ = powered_ = connected_ = false;
  battery_.reset();
  std::string dev;
  if (!manager_) return;
  GList* objects = g_dbus_object_manager_get_objects(manager_);
  for (GList* l = objects; l; l = l->next) {
    GDBusObject* obj = G_DBUS_OBJECT(l->data);
    if (auto* a = g_dbus_object_get_interface(obj, "org.bluez.Adapter1")) {
      have_adapter_ = true;
      if (getBool(G_DBUS_PROXY(a), "Powered")) powered_ = true;
      g_object_unref(a);
    }
    if (auto* di = g_dbus_object_get_interface(obj, "org.bluez.Device1")) {
      GDBusProxy* d = G_DBUS_PROXY(di);
      if (getBool(d, "Connected")) {
        connected_ = true;
        dev = getStr(d, "Alias");
        if (auto* bi = g_dbus_object_get_interface(obj, "org.bluez.Battery1")) {
          if (auto* g =
            g_dbus_proxy_get_cached_property(G_DBUS_PROXY(bi), "Percentage")) {
            battery_ = g_variant_get_byte(g);
            g_variant_unref(g);
          }
          g_object_unref(bi);
        }
      }
      g_object_unref(di);
    }
  }
  g_list_free_full(objects, g_object_unref);

  char buf[128];
  const char* st = !powered_ ? "off" : (connected_ ? "connected" : "on");
  if (connected_ && !dev.empty() && battery_)
    std::snprintf(buf, sizeof buf, "Bluetooth\n%s  %d%%",
                  dev.c_str(), *battery_);
  else if (connected_ && !dev.empty())
    std::snprintf(buf, sizeof buf, "Bluetooth\n%s", dev.c_str());
  else
    std::snprintf(buf, sizeof buf, "Bluetooth %s", st);
  tip_ = buf;
  event_box_.set_tooltip_text(tip_);
}

auto Bluetooth::update() -> void {
  read_state();
  const bool batt = connected_ && battery_.has_value();
  area_.set_size_request(batt ? width_ : width_narrow_, -1);
  area_.queue_draw();
  AModule::update();
}

// --- drawing ---------------------------------------------------------------
// The Bluetooth rune as one continuous stroked polyline.
void Bluetooth::draw_rune(const Cairo::RefPtr<Cairo::Context>& cr, double cx,
                          double cy, double h, double r, double g, double b) {
  const double hh = h * 0.5, hw = h * 0.30;
  auto path = [&] {
    cr->move_to(cx - hw, cy - hh * 0.5);
    cr->line_to(cx + hw, cy + hh * 0.5);
    cr->line_to(cx, cy + hh);
    cr->line_to(cx, cy - hh);
    cr->line_to(cx + hw, cy - hh * 0.5);
    cr->line_to(cx - hw, cy + hh * 0.5);
  };
  cr->set_line_join(Cairo::LINE_JOIN_ROUND);
  cr->set_line_cap(Cairo::LINE_CAP_ROUND);
  path();                                   // dark keyline for contrast
  cr->set_line_width(std::max(2.4, h * 0.20));
  cr->set_source_rgba(0.05, 0.06, 0.09, 0.85);
  cr->stroke();
  path();                                   // coloured rune
  cr->set_line_width(std::max(1.6, h * 0.135));
  cr->set_source_rgba(r, g, b, 1.0);
  cr->stroke();
}

// A vertical cell that reuses the system battery gauge's look (metal shell +
// dark cavity + glossy draining fill + domed terminal), slimmer and MONOCHROME
// BLUE -- orange when critically low (no green/amber, no charge glow/bolt).
void Bluetooth::draw_battery(const Cairo::RefPtr<Cairo::Context>& cr, double cx,
                             double cy, double h, double level) {
  const double lvl = level < 0 ? 0 : (level > 1 ? 1 : level);
  const bool low = lvl <= 0.20;
  const double r = low ? 0.98 : 0.18, g = low ? 0.55 : 0.78,
               b = low ? 0.16 : 1.0;          // bright cyan-blue (orange low)

  const double bw = h * 0.54, bh = h * 0.84;   // wider + taller
  const double e = bw * 0.40;                  // top/bottom cap bulge
  const double bx = cx - bw / 2.0, by = cy - bh / 2.0 + h * 0.04;

  // vertical cylinder: straight sides, elliptical top/bottom caps
  auto vcyl = [&](double X, double Y, double W, double H, double cap) {
    cr->begin_new_path();
    cr->move_to(X, Y);
    cr->curve_to(X + W * 0.28, Y - cap, X + W * 0.72, Y - cap, X + W, Y);
    cr->line_to(X + W, Y + H);
    cr->curve_to(X + W * 0.72, Y + H + cap, X + W * 0.28, Y + H + cap,
                 X, Y + H);
    cr->close_path();
  };

  // domed metal terminal on top (shell overlaps its base)
  const double nw = bw * 0.46, nh = h * 0.09, nr = nh * 0.5;
  cr->begin_new_path();
  cr->move_to(cx - nw / 2 + nr, by - e - nh);
  cr->arc(cx + nw / 2 - nr, by - e - nh + nr, nr, -0.5 * kPi, 0.0);
  cr->line_to(cx + nw / 2, by - e);
  cr->line_to(cx - nw / 2, by - e);
  cr->arc(cx - nw / 2 + nr, by - e - nh + nr, nr, kPi, 1.5 * kPi);
  cr->close_path();
  cr->set_source_rgba(0.82, 0.85, 0.90, 1.0);
  cr->fill();

  // metal shell: a horizontal bevel (lit from the left) + dark outline
  auto shell = Cairo::LinearGradient::create(bx, 0, bx + bw, 0);
  shell->add_color_stop_rgba(0.0, 0.84, 0.86, 0.90, 1.0);
  shell->add_color_stop_rgba(0.28, 1.0, 1.0, 1.0, 1.0);
  shell->add_color_stop_rgba(0.66, 0.60, 0.62, 0.68, 1.0);
  shell->add_color_stop_rgba(1.0, 0.26, 0.27, 0.33, 1.0);
  vcyl(bx, by, bw, bh, e);
  cr->set_source(shell);
  cr->fill_preserve();
  cr->set_line_width(std::max(1.0, h * 0.045));
  cr->set_source_rgba(0.14, 0.15, 0.19, 1.0);
  cr->stroke();

  // dark cavity, inset a THIN frame -- keep the metal to a sliver so the blue
  // meter is the sea, not a footer.
  const double ft = std::max(1.1, bw * 0.12), ex = e * 0.7;
  const double ix = bx + ft, iy = by + ft, iw = bw - 2 * ft, ih = bh - 2 * ft;
  vcyl(ix, iy, iw, ih, ex);
  cr->set_source_rgba(0.10, 0.11, 0.14, 1.0);
  cr->fill();

  // glossy charge fill from the BOTTOM, clipped to the cavity
  const double fh = ih * lvl;
  if (fh > 0.5) {
    cr->save();
    vcyl(ix, iy, iw, ih, ex);
    cr->clip();
    auto fill = Cairo::LinearGradient::create(ix, 0, ix + iw, 0);   // cyl gloss
    fill->add_color_stop_rgba(0.0, std::min(1.0, r * 1.5 + 0.22),
                                   std::min(1.0, g * 1.5 + 0.22),
                                   std::min(1.0, b * 1.5 + 0.10), 1.0);
    fill->add_color_stop_rgba(0.45, r, g, b, 1.0);        // royal, obvious
    fill->add_color_stop_rgba(1.0, r * 0.82, g * 0.82, b * 0.88, 1.0);
    cr->rectangle(ix - ex - 1, iy + ih - fh - 1, iw + 2 * ex + 2, fh + ex + 2);
    cr->set_source(fill);
    cr->fill();
    if (fh > 3.0) {                          // bright meniscus at the fill top
      cr->rectangle(ix - ex, iy + ih - fh - 1.2, iw + 2 * ex, 1.5);
      cr->set_source_rgba(std::min(1.0, r * 1.5 + 0.2),
                          std::min(1.0, g * 1.5 + 0.2),
                          std::min(1.0, b * 1.5 + 0.2), 0.85);
      cr->fill();
    }
    cr->restore();
  }
}

bool Bluetooth::on_draw(const Cairo::RefPtr<Cairo::Context>& cr) {
  const double w = area_.get_allocated_width();
  const double h = area_.get_allocated_height();
  if (w <= 0 || h <= 0) return true;
  cr->set_antialias(Cairo::ANTIALIAS_DEFAULT);

  // rune colour by state
  double r, g, b;
  if (!powered_) {
    r = 0.42; g = 0.42; b = 0.47;              // off / disabled: grey
  } else if (connected_) {
    r = 0.10; g = 0.53; b = 1.0;               // connected: standard BT azure
  } else {
    r = 0.44; g = 0.53; b = 0.78;              // on, no device: washed-out
  }                                            // blue-grey (faded, not bold)

  const bool batt = connected_ && battery_.has_value();
  if (!batt) {
    draw_rune(cr, w / 2.0, h / 2.0, h * 0.64, r, g, b);
    return true;
  }

  // rune + battery: same height, a GAP between them, sitting high (headroom
  // above) so the readout at the bottom only slightly overlaps them.
  const double gy = h * 0.37, gh = h * 0.54;
  draw_rune(cr, w * 0.30, gy, gh, r, g, b);
  draw_battery(cr, w * 0.73, gy, gh, battery_.value() / 100.0);

  char buf[8];
  std::snprintf(buf, sizeof buf, "%d%%", battery_.value());
  cr->select_font_face("sans-serif", Cairo::FONT_SLANT_NORMAL,
                       Cairo::FONT_WEIGHT_BOLD);
  double fs = std::max(8.0, h * 0.34);
  cr->set_font_size(fs);
  Cairo::TextExtents te;
  cr->get_text_extents(buf, te);
  if (te.width > w - 3.0 && te.width > 1.0) {   // fit "100%"
    fs *= (w - 3.0) / te.width;
    cr->set_font_size(fs);
    cr->get_text_extents(buf, te);
  }
  const double tx = w / 2.0 - te.width / 2.0 - te.x_bearing, ty = h * 0.95;
  cr->move_to(tx, ty);                         // black outline
  cr->text_path(buf);
  cr->set_line_join(Cairo::LINE_JOIN_ROUND);
  cr->set_line_width(std::max(2.0, h * 0.10));
  cr->set_source_rgba(0, 0, 0, 0.92);
  cr->stroke();
  cr->move_to(tx, ty);                         // lighter blue, most legible of
  cr->set_source_rgba(0.62, 0.82, 1.0, 1.0);   // the 3 (rune, batt, this)
  cr->show_text(buf);
  return true;
}

}  // namespace waybar::modules::hw
