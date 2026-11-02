#pragma once

// bus_menu.hpp - a minimal com.canonical.dbusmenu CLIENT on an explicit D-Bus
// connection, header-only, for a tray on another bus (patches/tray-bus.patch,
// wired by patches/tray-bus-menu.patch).
//
// libdbusmenu-gtk cannot do this: dbusmenu_gtkmenu_new() takes no connection
// and the library only ever calls g_bus_get(), so it reaches the session bus
// and nothing else. The session tray keeps using it; this serves the rest.
//
// The menu is fetched FRESH every time it opens (AboutToShow, then GetLayout
// of the whole tree), so nothing has to track LayoutUpdated or ItemsUpdated;
// see fetch() for why that happens synchronously, inside the click.
// Supported properties: type (separator), label (with `_` mnemonics), enabled,
// visible, toggle-type (checkmark/radio), toggle-state, and children (a
// submenu). Icons and shortcuts are not drawn. Activating an entry sends
// Event(id, "clicked", 0, time).

#include <gio/gio.h>
#include <glibmm/refptr.h>
#include <gtk/gtk.h>
#include <gtkmm/checkmenuitem.h>
#include <gtkmm/menu.h>
#include <gtkmm/separatormenuitem.h>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <memory>
#include <string>

namespace waybar::bus_menu {

inline const char* const IFACE = "com.canonical.dbusmenu";

// Typed lookups in a layout node's a{sv}, each with the spec's default.
inline std::string prop_str(GVariant* props, const char* key, const char* fallback) {
  GVariant* v = g_variant_lookup_value(props, key, G_VARIANT_TYPE_STRING);
  if (v == nullptr) return fallback;
  std::string s = g_variant_get_string(v, nullptr);
  g_variant_unref(v);
  return s;
}

inline bool prop_bool(GVariant* props, const char* key, bool fallback) {
  GVariant* v = g_variant_lookup_value(props, key, G_VARIANT_TYPE_BOOLEAN);
  if (v == nullptr) return fallback;
  bool b = g_variant_get_boolean(v);
  g_variant_unref(v);
  return b;
}

inline int32_t prop_int(GVariant* props, const char* key, int32_t fallback) {
  GVariant* v = g_variant_lookup_value(props, key, G_VARIANT_TYPE_INT32);
  if (v == nullptr) return fallback;
  int32_t i = g_variant_get_int32(v);
  g_variant_unref(v);
  return i;
}

// Fire-and-forget: the menu is gone by the time any reply could matter.
inline void send_clicked(GDBusConnection* conn, const std::string& dest, const std::string& path,
                         int32_t id) {
  g_dbus_connection_call(
      conn, dest.c_str(), path.c_str(), IFACE, "Event",
      g_variant_new("(isvu)", id, "clicked", g_variant_new_int32(0), gtk_get_current_event_time()),
      nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
}

inline void fill(Gtk::Menu& menu, GVariant* children, GDBusConnection* conn,
                 const std::string& dest, const std::string& path);

// One layout node, (ia{sv}av), as a menu item; nullptr when it is invisible.
inline Gtk::MenuItem* make_item(GVariant* node, GDBusConnection* conn, const std::string& dest,
                                const std::string& path) {
  int32_t id = 0;
  GVariant* props = nullptr;
  GVariant* kids = nullptr;
  g_variant_get(node, "(i@a{sv}@av)", &id, &props, &kids);
  Gtk::MenuItem* item = nullptr;
  if (!prop_bool(props, "visible", true)) {
    // hidden: no item
  } else if (prop_str(props, "type", "standard") == "separator") {
    item = Gtk::manage(new Gtk::SeparatorMenuItem());
  } else {
    const std::string label = prop_str(props, "label", "");
    const std::string toggle = prop_str(props, "toggle-type", "");
    if (toggle == "checkmark" || toggle == "radio") {
      auto* check = Gtk::manage(new Gtk::CheckMenuItem(label, true));
      check->set_draw_as_radio(toggle == "radio");
      // set_active() emits "activate", so it must run before the handler
      // below is connected, or building the menu would click the entry.
      check->set_active(prop_int(props, "toggle-state", 0) == 1);
      item = check;
    } else {
      item = Gtk::manage(new Gtk::MenuItem(label, true));
    }
    item->set_sensitive(prop_bool(props, "enabled", true));
    if (g_variant_n_children(kids) > 0) {
      auto* sub = Gtk::manage(new Gtk::Menu());
      fill(*sub, kids, conn, dest, path);
      item->set_submenu(*sub);
    } else {
      // Each handler holds its own reference, so a click after the bus went
      // away reaches a closed connection (a logged error), never freed memory.
      std::shared_ptr<GDBusConnection> ref(G_DBUS_CONNECTION(g_object_ref(conn)),
                                           [](GDBusConnection* c) { g_object_unref(c); });
      item->signal_activate().connect(
          [ref, dest, path, id] { send_clicked(ref.get(), dest, path, id); });
    }
  }
  if (item != nullptr) item->show();
  g_variant_unref(props);
  g_variant_unref(kids);
  return item;
}

inline void fill(Gtk::Menu& menu, GVariant* children, GDBusConnection* conn,
                 const std::string& dest, const std::string& path) {
  const gsize n = g_variant_n_children(children);
  for (gsize i = 0; i < n; ++i) {
    GVariant* boxed = g_variant_get_child_value(children, i);
    GVariant* node = g_variant_get_variant(boxed);
    if (g_variant_is_of_type(node, G_VARIANT_TYPE("(ia{sv}av)"))) {
      if (auto* item = make_item(node, conn, dest, path)) menu.append(*item);
    }
    g_variant_unref(node);
    g_variant_unref(boxed);
  }
}

// How long a click may wait for the item's menu before giving up on it.
inline constexpr int FETCH_TIMEOUT_MS = 500;

// Fetch the menu tree at dest/path on conn, SYNCHRONOUSLY, and return the
// root's children (an av, owned: g_variant_unref it), or nullptr when there
// is nothing to show (an error, a timeout, or an empty menu).
//
// Synchronous on purpose, and called from inside the click: GTK takes a
// menu's popup grab from the click that opens it, so a menu that first
// appears after the reply lands, with the button already released, shows but
// never tracks hover or opens a submenu; and a menu popped up EMPTY inside the
// click and filled later stays at its empty size on Wayland. Both measured.
// The price is bounded: a hung item can hold the bar for FETCH_TIMEOUT_MS
// per click, once, and the caller then falls back to the item's own menu.
inline GVariant* fetch(GDBusConnection* conn, const std::string& dest, const std::string& path) {
  // AboutToShow's answer is ignored, errors included: some servers do not
  // implement it, and the layout below is fetched whole regardless.
  GVariant* shown = g_dbus_connection_call_sync(
      conn, dest.c_str(), path.c_str(), IFACE, "AboutToShow", g_variant_new("(i)", 0), nullptr,
      G_DBUS_CALL_FLAGS_NONE, FETCH_TIMEOUT_MS, nullptr, nullptr);
  if (shown != nullptr) g_variant_unref(shown);
  GError* error = nullptr;
  GVariant* reply = g_dbus_connection_call_sync(
      conn, dest.c_str(), path.c_str(), IFACE, "GetLayout",
      g_variant_new("(ii@as)", 0, -1, g_variant_new_strv(nullptr, 0)),
      G_VARIANT_TYPE("(u(ia{sv}av))"), G_DBUS_CALL_FLAGS_NONE, FETCH_TIMEOUT_MS, nullptr, &error);
  if (reply == nullptr) {
    spdlog::warn("Tray: GetLayout from {}{} failed: {}", dest, path, error->message);
    g_error_free(error);
    return nullptr;
  }
  uint32_t revision = 0;
  GVariant* root = nullptr;
  g_variant_get(reply, "(u@(ia{sv}av))", &revision, &root);
  GVariant* kids = g_variant_get_child_value(root, 2);
  g_variant_unref(root);
  g_variant_unref(reply);
  if (g_variant_n_children(kids) == 0) {
    g_variant_unref(kids);
    return nullptr;
  }
  return kids;
}

}  // namespace waybar::bus_menu
