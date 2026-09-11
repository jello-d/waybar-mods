#include "modules/wayfire/taskbar.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include <gdkmm/general.h>
#include <spdlog/spdlog.h>

namespace waybar::modules::wayfire {

static uint32_t read_u32_le(int fd) {
  uint8_t b[4];
  ssize_t n = ::read(fd, b, 4);
  if (n != 4) throw std::runtime_error("short read header");
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
         ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void write_u32_le(int fd, uint32_t v) {
  uint8_t b[4] = {(uint8_t)(v & 0xff), (uint8_t)((v >> 8) & 0xff),
                  (uint8_t)((v >> 16) & 0xff), (uint8_t)((v >> 24) & 0xff)};
  ssize_t n = ::send(fd, b, 4, MSG_NOSIGNAL);
  if (n != 4) throw std::runtime_error("short write header");
}

static std::string read_exact(int fd, size_t n) {
  std::string out;
  out.resize(n);
  size_t off = 0;
  while (off < n) {
    ssize_t r = ::read(fd, out.data() + off, n - off);
    if (r <= 0) throw std::runtime_error("socket closed");
    off += (size_t)r;
  }
  return out;
}

static void write_all(int fd, const std::string& s) {
  size_t off = 0;
  while (off < s.size()) {
    ssize_t w = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
    if (w <= 0) throw std::runtime_error("socket write failed");
    off += (size_t)w;
  }
}

static int64_t j_i64(const Json::Value& obj, const char* key, int64_t def) {
  if (!obj.isObject() || !obj.isMember(key)) return def;
  const auto& v = obj[key];
  if (v.isInt64()) return v.asInt64();
  if (v.isUInt64()) return (int64_t)v.asUInt64();
  if (v.isInt()) return v.asInt();
  if (v.isUInt()) return v.asUInt();
  return def;
}

static std::string j_str(const Json::Value& obj, const char* key,
                         const std::string& def = "") {
  if (!obj.isObject() || !obj.isMember(key)) return def;
  const auto& v = obj[key];
  return v.isString() ? v.asString() : def;
}

static bool j_bool(const Json::Value& obj, const char* key, bool def) {
  if (!obj.isObject() || !obj.isMember(key)) return def;
  const auto& v = obj[key];
  if (v.isBool()) return v.asBool();
  if (v.isInt()) return (v.asInt() != 0);
  return def;
}

static std::string resolve_wayfire_socket(const Json::Value& cfg) {
  if (cfg.isObject() && cfg.isMember("socket") && cfg["socket"].isString()) {
    return cfg["socket"].asString();
  }
  if (const char* env = std::getenv("WAYFIRE_SOCKET")) {
    return env;
  }
  return "/run/user/" + std::to_string(getuid()) +
         "/wayfire-wayland-1-.socket";
}

static bool vec_contains(const std::vector<int64_t>& v, int64_t id) {
  return std::find(v.begin(), v.end(), id) != v.end();
}

static void vec_erase(std::vector<int64_t>& v, int64_t id) {
  v.erase(std::remove(v.begin(), v.end(), id), v.end());
}

static void vec_move_to_front(std::vector<int64_t>& z_back_to_front,
                              int64_t id) {
  vec_erase(z_back_to_front, id);
  z_back_to_front.push_back(id);
}

static void vec_move_to_back(std::vector<int64_t>& z_back_to_front,
                             int64_t id) {
  vec_erase(z_back_to_front, id);
  z_back_to_front.insert(z_back_to_front.begin(), id);
}

Taskbar::Taskbar(const std::string& id, const waybar::Bar& bar,
                 const Json::Value& config)
    : AModule(config, "taskbar", id),
      bar_(bar),
      config_(config),
      box_(bar.orientation, 0) {
  socket_path_ = resolve_wayfire_socket(config_);
  if (socket_path_.empty()) {
    throw std::runtime_error("wayfire/taskbar: no socket path");
  }

  if (config_["reconcile-interval"].isInt())
    reconcile_ms_ = config_["reconcile-interval"].asInt();
  if (config_["refresh-debounce"].isInt())
    refresh_debounce_ms_ = config_["refresh-debounce"].asInt();

  all_workspaces_ = config_["all-workspaces"].isBool() ?
                    config_["all-workspaces"].asBool() : all_workspaces_;

  button_spacing_ = config_["button-spacing"].isInt() ?
                    config_["button-spacing"].asInt() : button_spacing_;

  if (config_["item-min-width"].isInt())
    button_min_width_ = config_["item-min-width"].asInt();
  else if (config_["button-min-width"].isInt())
    button_min_width_ = config_["button-min-width"].asInt();

  if (config_["item-max-width"].isInt())
    button_max_width_ = config_["item-max-width"].asInt();
  else if (config_["button-max-width"].isInt())
    button_max_width_ = config_["button-max-width"].asInt();

  icon_theme_ = Gtk::IconTheme::get_default();

  box_.set_name("taskbar");
  box_.set_spacing(button_spacing_);
  box_.set_homogeneous(false);
  box_.set_hexpand(false);
  box_.set_halign(Gtk::ALIGN_START);

  // Row width BUDGET comes from THIS bar's output (not the global primary --
  // on a multi-output setup those differ), minus room for the modules that
  // share the bar: the launcher icons on the left and the clipboard preview
  // on the right. This is only a budget for per-button widths; the taskbar
  // must NOT claim the whole span as a fixed size, or it squeezes the right-
  // hand modules off the bar even when nearly empty. The event box below is
  // therefore left to size to its actual button content.
  if (bar_.output && bar_.output->monitor) {
    Gdk::Rectangle geom;
    bar_.output->monitor->get_geometry(geom);
    taskbar_width_ = geom.get_width() - 360;
  }

  event_box_.set_name("taskbar-wrap");
  event_box_.set_hexpand(false);
  event_box_.set_halign(Gtk::ALIGN_START);
  event_box_.add(box_);
  event_box_.show_all();

  spdlog::debug("wayfire/taskbar: using socket {}, min_w={}, max_w={}",
                socket_path_, button_min_width_, button_max_width_);

  // Event-driven: paint once, subscribe to view/workspace events, and let a
  // slow reconcile timer backstop any missed event. No fast poll loop.
  schedule_update();
  start_events();
  if (reconcile_ms_ > 0) {
    timer_ = Glib::signal_timeout().connect(
        sigc::mem_fun(*this, &Taskbar::on_timer), reconcile_ms_);
  }
}

Taskbar::~Taskbar() {
  if (evt_io_.connected()) evt_io_.disconnect();
  if (refresh_pending_.connected()) refresh_pending_.disconnect();
  if (timer_.connected()) timer_.disconnect();
  if (evt_fd_ >= 0) { ::close(evt_fd_); evt_fd_ = -1; }
  disconnect_socket();
}

void Taskbar::connect_socket() {
  if (sock_fd_ >= 0) return;

  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) throw std::runtime_error("socket() failed");

  sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  if (socket_path_.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    throw std::runtime_error("socket path too long");
  }
  std::strncpy(addr.sun_path, socket_path_.c_str(),
               sizeof(addr.sun_path) - 1);

  if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
    ::close(fd);
    std::ostringstream ss;
    ss << "connect() failed: " << std::strerror(errno);
    throw std::runtime_error(ss.str());
  }
  sock_fd_ = fd;
}

void Taskbar::disconnect_socket() {
  if (sock_fd_ >= 0) {
    ::close(sock_fd_);
    sock_fd_ = -1;
  }
}

Json::Value Taskbar::rpc(const std::string& method,
                         const Json::Value& data_in) {
  Json::Value data = data_in.isObject() ? data_in
                                        : Json::Value(Json::objectValue);
  Json::Value msg;
  msg["method"] = method;
  msg["data"] = data;

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  const std::string payload = Json::writeString(wb, msg);

  // Persistent connection: open once, reuse every call. A stale fd costs one
  // transparent reconnect, not a lost tick. Transport faults reconnect; a
  // parsed {"error": ...} does not, so an application error never silently
  // re-sends a mutating call.
  std::string resp;
  bool got = false;
  for (int attempt = 0; attempt < 2 && !got; ++attempt) {
    try {
      connect_socket();
      write_u32_le(sock_fd_, (uint32_t)payload.size());
      write_all(sock_fd_, payload);
      uint32_t resp_len = read_u32_le(sock_fd_);
      resp = read_exact(sock_fd_, resp_len);
      got = true;
    } catch (const std::runtime_error&) {
      disconnect_socket();
      if (attempt == 1) throw;
    }
  }

  Json::CharReaderBuilder rb;
  std::string errs;
  Json::Value root;
  std::istringstream iss(resp);
  if (!Json::parseFromStream(rb, iss, &root, &errs)) {
    throw std::runtime_error("invalid json response: " + errs);
  }
  if (root.isObject() && root.isMember("error")) {
    throw std::runtime_error(root["error"].asString());
  }
  return root;
}

void Taskbar::start_events() {
  try {
    if (evt_fd_ >= 0) { ::close(evt_fd_); evt_fd_ = -1; }

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("event socket() failed");

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(),
                 sizeof(addr.sun_path) - 1);
    if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
      ::close(fd);
      throw std::runtime_error("event connect() failed");
    }
    evt_fd_ = fd;

    // Subscribe to ALL events by sending no "events" list. Wayfire's watch
    // rejects the entire request if any named event is unknown (ipc-rules
    // returns "Event not found"), which makes a curated list a version
    // landmine. Subscribing to everything can't be rejected and survives
    // Wayfire upgrades; on_event_io filters for relevance.
    Json::Value msg;
    msg["method"] = "window-rules/events/watch";
    msg["data"] = Json::Value(Json::objectValue);

    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    const std::string payload = Json::writeString(wb, msg);
    write_u32_le(evt_fd_, (uint32_t)payload.size());
    write_all(evt_fd_, payload);

    // The subscription ack is just a non-event message that on_event_io
    // reads and ignores; no synchronous drain needed.
    evt_io_ = Glib::signal_io().connect(
        sigc::mem_fun(*this, &Taskbar::on_event_io),
        evt_fd_, Glib::IO_IN | Glib::IO_HUP | Glib::IO_ERR);
  } catch (const std::exception& e) {
    if (evt_fd_ >= 0) { ::close(evt_fd_); evt_fd_ = -1; }
    spdlog::warn("wayfire/taskbar: event subscribe failed ({}); retrying",
                 e.what());
    Glib::signal_timeout().connect_once(
        sigc::mem_fun(*this, &Taskbar::start_events), 1000);
  }
}

bool Taskbar::on_event_io(Glib::IOCondition cond) {
  if (cond & (Glib::IO_HUP | Glib::IO_ERR)) {
    if (evt_fd_ >= 0) { ::close(evt_fd_); evt_fd_ = -1; }
    Glib::signal_timeout().connect_once(
        sigc::mem_fun(*this, &Taskbar::start_events), 1000);
    return false;
  }
  try {
    uint32_t len = read_u32_le(evt_fd_);
    std::string payload = read_exact(evt_fd_, len);

    Json::CharReaderBuilder rb;
    std::string errs;
    Json::Value root;
    std::istringstream iss(payload);
    if (Json::parseFromStream(rb, iss, &root, &errs) &&
        root.isObject() && root.isMember("event")) {
      // Refresh on anything that can change what the bar shows; skip the
      // high-frequency events that never do.
      const std::string ev = root["event"].asString();
      if (ev != "keyboard-modifier-state-changed" &&
          ev != "plugin-activation-state-changed") {
        // Output geometry and the current workspace are cached and pulled
        // lazily; only a workspace/output change can move them, so invalidate
        // the cache on those and let the coming refresh re-pull. Everything
        // else (focus, title, geometry, map/unmap) reuses the cached values.
        if (ev == "wset-workspace-changed" || ev == "output-wset-changed" ||
            ev == "output-added" || ev == "output-removed") {
          geom_synced_ = false;
        }
        schedule_update();
      }
    }
  } catch (const std::exception&) {
    if (evt_fd_ >= 0) { ::close(evt_fd_); evt_fd_ = -1; }
    Glib::signal_timeout().connect_once(
        sigc::mem_fun(*this, &Taskbar::start_events), 1000);
    return false;
  }
  return true;
}

void Taskbar::schedule_update() {
  // Trailing debounce: a burst (open a window fires mapped + title + focused;
  // a drag fires many geometry events) collapses to one refresh once it
  // settles.
  if (refresh_pending_.connected()) refresh_pending_.disconnect();
  refresh_pending_ = Glib::signal_timeout().connect(
      sigc::mem_fun(*this, &Taskbar::on_refresh), refresh_debounce_ms_);
}

bool Taskbar::on_refresh() {
  safe_update();
  return false;  // one-shot
}

bool Taskbar::on_timer() {
  geom_synced_ = false;  // reconcile: re-pull geometry + workspace too
  safe_update();
  return true;   // reconcile: repeat
}

void Taskbar::safe_update() {
  try {
    update();
  } catch (const std::exception& e) {
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - last_error_log_).count() > 1000) {
      last_error_log_ = Clock::now();
      spdlog::warn("wayfire/taskbar update error: {}", e.what());
    }
  }
}

void Taskbar::resolve_output_id(const Json::Value& views) {
  // Our output's numeric id, needed to filter views to this bar's output.
  // Cheap: scan the already-fetched list-views (the bar's own layer-shell
  // views always carry our output-name), so this costs no extra RPC and runs
  // every refresh. An output identity change invalidates the geometry cache.
  const std::string& my_name = bar_.output->name;
  int64_t found = -1;
  if (views.isArray()) {
    for (Json::ArrayIndex i = 0; i < views.size(); i++) {
      if (j_str(views[i], "output-name", "") == my_name) {
        found = j_i64(views[i], "output-id", -1);
        break;
      }
    }
  }

  if (found < 0) {
    focused_output_id_ = -1;
    spdlog::warn("wayfire/taskbar: could not find output-id for output '{}'",
                 my_name);
    return;
  }
  if (found != focused_output_id_) geom_synced_ = false;
  focused_output_id_ = found;
}

void Taskbar::sync_output_geometry() {
  // Output pixel geometry (static) and current workspace (moves only on a
  // workspace switch). Pulled from wayfire, not GDK, so it stays in the
  // compositor pixel space that view bboxes are expressed in. This is the
  // module's one remaining query off list-views, so keep it off the hot path:
  // it is seeded at startup, re-pulled only when a workspace/output event
  // invalidates the cache, and re-pulled on the reconcile tick as a backstop.
  // The frequent focus/title/geometry refreshes reuse the cached values.
  if (focused_output_id_ < 0) return;

  Json::Value oi_data;
  oi_data["id"] = (Json::Int64)focused_output_id_;
  Json::Value oi = rpc("window-rules/output-info", oi_data);

  out_x_ = j_i64(oi["geometry"], "x", 0);
  out_y_ = j_i64(oi["geometry"], "y", 0);
  out_w_ = j_i64(oi["geometry"], "width", 0);
  out_h_ = j_i64(oi["geometry"], "height", 0);
  ws_x_  = j_i64(oi["workspace"], "x", 0);
  ws_y_  = j_i64(oi["workspace"], "y", 0);
  geom_synced_ = true;

  spdlog::debug("wayfire/taskbar: outp='{}' id={} geo=({},{} {}x{}) ws=({},{})",
                 bar_.output->name, focused_output_id_,
                 out_x_, out_y_, out_w_, out_h_, ws_x_, ws_y_);
}

bool Taskbar::bbox_center_on_output(const View& v) const {
  if (out_w_ <= 0 || out_h_ <= 0) return true;
  const int64_t cx = v.bx + (v.bw / 2);
  const int64_t cy = v.by + (v.bh / 2);
  return (cx >= 0 && cx < out_w_ && cy >= 0 && cy < out_h_);
}

std::vector<View> Taskbar::list_views_filtered(const Json::Value& views) {
  std::vector<View> out;
  std::vector<int64_t> ids_now;
  if (!views.isArray()) return out;

  for (Json::ArrayIndex i = 0; i < views.size(); i++) {
    const auto& j = views[i];
    View v;
    v.id = j_i64(j, "id", -1);
    if (v.id < 0) continue;

    v.app_id = j_str(j, "app-id", "");
    v.title = j_str(j, "title", "");
    v.activated = j_bool(j, "activated", false);
    v.minimized = j_bool(j, "minimized", false);
    v.sticky = j_bool(j, "sticky", false);
    v.mapped = j_bool(j, "mapped", false);
    v.layer = j_str(j, "layer", "");
    v.role = j_str(j, "role", "");
    v.type = j_str(j, "type", "");
    v.output_id = j_i64(j, "output-id", -1);

    const auto ts = j_i64(j, "last-focus-timestamp", 0);
    const auto& bbox = j["bbox"];
    v.bx = j_i64(bbox, "x", 0); v.by = j_i64(bbox, "y", 0);
    v.bw = j_i64(bbox, "width", 0); v.bh = j_i64(bbox, "height", 0);

    if (!v.mapped || v.layer != "workspace") continue;
    if (v.role != "toplevel" || v.type != "toplevel") continue;
    if (focused_output_id_ >= 0 && v.output_id != focused_output_id_) continue;

    if (!v.minimized) last_known_ws_[v.id] = {ws_x_, ws_y_};

    bool show = false;
    if (v.sticky || all_workspaces_) show = true;
    else if (v.minimized && last_known_ws_.count(v.id)) {
      auto last = last_known_ws_[v.id];
      if (last.first == ws_x_ && last.second == ws_y_) show = true;
    } else if (bbox_center_on_output(v)) show = true;

    if (!show) continue;

    ids_now.push_back(v.id);
    out.push_back(std::move(v));

    const auto prev = focus_ts_.count(v.id) ? focus_ts_[v.id] : 0;
    focus_ts_[v.id] = ts;
    if (!vec_contains(z_order_, v.id)) z_order_.push_back(v.id);
    else if (ts > prev) {
      vec_move_to_front(z_order_, v.id);
      sent_back_[v.id] = false;
    }
    if (v.activated && !sent_back_[v.id]) vec_move_to_front(z_order_, v.id);
  }

  focused_view_id_ = -1;
  for (const auto& v : out) {
    if (v.activated && !sent_back_[v.id]) { focused_view_id_ = v.id; break; }
  }

  for (auto it = z_order_.begin(); it != z_order_.end();) {
    if (!vec_contains(ids_now, *it)) {
      sent_back_.erase(*it);
      focus_ts_.erase(*it);
      last_known_ws_.erase(*it);
      it = z_order_.erase(it);
    } else ++it;
  }

  for (const auto& [id, back] : sent_back_) {
    if (back && vec_contains(z_order_, id)) vec_move_to_back(z_order_, id);
  }

  std::map<int64_t, View> by_id;
  for (auto& v : out) by_id[v.id] = std::move(v);
  std::vector<View> ordered;
  for (auto id : z_order_) {
    auto it = by_id.find(id);
    if (it != by_id.end()) {
      ordered.push_back(std::move(it->second));
      by_id.erase(it);
    }
  }
  for (auto& kv : by_id) ordered.push_back(std::move(kv.second));
  return ordered;
}

Glib::RefPtr<Gio::DesktopAppInfo> Taskbar::find_app_info(
    const std::string& app_id) {
  if (app_id.empty()) return {};

  // Helper: try to create DesktopAppInfo, return nullptr on failure
  auto try_create = [](const std::string& id)
      -> Glib::RefPtr<Gio::DesktopAppInfo> {
    try {
      auto ai = Gio::DesktopAppInfo::create(id);
      if (ai) return ai;
    } catch (...) {}
    return {};
  };

  // 1. Try app_id directly (e.g., "firefox")
  if (auto ai = try_create(app_id)) return ai;

  // 2. Try app_id + ".desktop" (e.g., "firefox.desktop")
  if (auto ai = try_create(app_id + ".desktop")) return ai;

  // 3. Try lowercase
  std::string lower = app_id;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (auto ai = try_create(lower)) return ai;
  if (auto ai = try_create(lower + ".desktop")) return ai;

  // 4. Handle reverse-DNS style (org.gnome.Nautilus -> nautilus)
  auto last_dot = app_id.rfind('.');
  if (last_dot != std::string::npos && last_dot + 1 < app_id.size()) {
    std::string short_name = app_id.substr(last_dot + 1);
    std::transform(short_name.begin(), short_name.end(),
                   short_name.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (auto ai = try_create(short_name)) return ai;
    if (auto ai = try_create(short_name + ".desktop")) return ai;
  }

  // 5. Search all desktop files for matching StartupWMClass
  auto all_apps = Gio::AppInfo::get_all();
  for (const auto& app : all_apps) {
    auto desktop = Glib::RefPtr<Gio::DesktopAppInfo>::cast_dynamic(app);
    if (!desktop) continue;

    // Check StartupWMClass. Compare against the returned std::string directly:
    // .c_str() on the temporary would dangle the moment the statement ends.
    if (app_id == desktop->get_startup_wm_class()) return desktop;

    // Check lowercase match against desktop file basename
    std::string desktop_id = desktop->get_id();
    std::string desktop_lower = desktop_id;
    std::transform(desktop_lower.begin(), desktop_lower.end(),
                   desktop_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Strip .desktop suffix for comparison
    if (desktop_lower.size() > 8 &&
        desktop_lower.substr(desktop_lower.size() - 8) == ".desktop") {
      desktop_lower = desktop_lower.substr(0, desktop_lower.size() - 8);
    }

    if (lower == desktop_lower) return desktop;

    // Partial match: app_id contains desktop name or vice versa
    if (lower.find(desktop_lower) != std::string::npos ||
        desktop_lower.find(lower) != std::string::npos) {
      return desktop;
    }
  }

  return {};
}

void Taskbar::set_icon(Gtk::Image& img,
                       const Glib::RefPtr<Gtk::IconTheme>&,
                       const Glib::RefPtr<Gio::DesktopAppInfo>& app_info,
                       int size) {
  if (app_info && app_info->get_icon()) {
    auto icon = app_info->get_icon();
    img.set(Glib::RefPtr<const Gio::Icon>::cast_static(icon),
            Gtk::ICON_SIZE_INVALID);
  } else {
    img.set_from_icon_name("application-x-executable", Gtk::ICON_SIZE_INVALID);
  }
  img.set_pixel_size(size);
}

void Taskbar::rebuild(const std::vector<View>& views) {
  // Remove buttons for views that no longer exist
  for (auto it = items_.begin(); it != items_.end();) {
    bool keep = false;
    for (const auto& v : views) {
      if (v.id == it->first) { keep = true; break; }
    }
    if (!keep) {
      box_.remove(it->second->button);
      it = items_.erase(it);
    } else {
      ++it;
    }
  }

  // Uniform button width: divide the available row width among the visible
  // buttons, capped at button_max_width_ and floored at an icon-only width,
  // so the row always fits and every button is the same size. Below a text
  // threshold the title is dropped entirely (icon only) -- the extreme the
  // row degrades to as the window count climbs.
  // Scale the app icon with the bar height (ui = bar_h/48), like the other
  // native modules -- so a compact low-res bar shrinks the icons instead of
  // being floored by them. ui == 1 at the base is a strict no-op.
  const double ui = bar_.config["height"].isInt()
                        ? bar_.config["height"].asInt() / 48.0 : 1.0;
  const int cfg_icon = config_["icon-size"].isInt()
                       ? config_["icon-size"].asInt() : 22;
  const int icon_sz = static_cast<int>(cfg_icon * ui + 0.5);
  // Two regimes, one shared width so buttons stay uniform. While a title fits
  // (per >= text_threshold) buttons stretch to divide the available width,
  // capped at button_max_width_. Once there is no room for a title they go
  // icon-only AND stay COMPACT at icon_only_w rather than stretching -- wide,
  // near-empty icon buttons look too generous; the row instead ends early and
  // leaves whitespace before the right-hand modules. icon_only_w is also the
  // hard floor, so the row always collapses to fit every window.
  // button_min_width_ (if set) is only the narrowest a button keeps its title.
  const int icon_only_w = icon_sz + 18;
  const int text_threshold =
      button_min_width_ > 0 ? std::max(icon_only_w, button_min_width_)
                            : icon_only_w + 44;
  const int n = static_cast<int>(views.size());
  // Per-button chrome not covered by set_size_request (CSS border, rounding);
  // budgeting for it keeps the row from overflowing and dropping a window.
  const int chrome = 8;
  int per = button_max_width_;
  if (n > 0) {
    const int avail = taskbar_width_ - button_spacing_ * (n - 1) - chrome * n;
    per = avail / n;
    if (per > button_max_width_) per = button_max_width_;
    if (per < text_threshold) per = icon_only_w;
  }

  for (const auto& v : views) {
    Item* item = nullptr;
    auto it = items_.find(v.id);
    if (it == items_.end()) {
      auto up = std::make_unique<Item>();
      up->content = Gtk::Box(bar_.orientation, 6);
      up->content.set_halign(Gtk::ALIGN_FILL);
      up->content.set_valign(Gtk::ALIGN_CENTER);
      up->icon.set_pixel_size(icon_sz);
      up->label.set_ellipsize(Pango::ELLIPSIZE_END);
      up->label.set_hexpand(true);
      up->content.pack_start(up->icon, false, false, 0);
      up->content.pack_start(up->label, true, true, 0);
      up->button.add(up->content);
      up->button.set_relief(Gtk::RELIEF_NORMAL);

      // Explicit CSS naming so GTK CSS can target these
      up->button.set_name("task-btn");
      up->button.get_style_context()->add_class("task-btn");

      box_.pack_start(up->button, false, false, 0);
      up->button.signal_clicked().connect(
          [this, vid = v.id]() { this->on_click(vid); });
      up->button.show_all();
      item = up.get();
      items_[v.id] = std::move(up);
    } else {
      item = it->second.get();
    }

    // A view's app icon is stable for its lifetime, and resolving it walks
    // every .desktop file on the system in the worst case. rebuild() runs on
    // the frequent focus/title/geometry events, so resolve the icon once (at
    // button creation, or the rare app_id change) rather than every refresh.
    if (it == items_.end() || item->app_id != v.app_id) {
      set_icon(item->icon, icon_theme_, find_app_info(v.app_id), icon_sz);
      item->app_id = v.app_id;
    }

    // Update CSS classes for state
    auto ctx = item->button.get_style_context();
    ctx->remove_class("active");
    ctx->remove_class("focused");
    ctx->remove_class("minimized");

    if (v.minimized) {
      ctx->add_class("minimized");
    } else if (v.id == focused_view_id_) {
      ctx->add_class("focused");
    } else if (v.activated) {
      ctx->add_class("active");
    }

    // Every button is exactly `per` wide; the label ellipsizes within it and
    // is dropped (icon centered) once there is no room for text.
    item->button.set_size_request(per, -1);

    const bool show_text =
        j_bool(config_, "show-title", true) && per >= text_threshold;
    if (show_text) {
      item->label.set_text(v.title);
      item->content.set_halign(Gtk::ALIGN_FILL);
      item->label.show();
    } else {
      item->label.set_text("");
      item->content.set_halign(Gtk::ALIGN_CENTER);
      item->label.hide();
    }
  }
}

void Taskbar::on_click(int64_t view_id) {
  try {
    Json::Value q;
    q["id"] = (Json::Int64)view_id;
    Json::Value info = rpc("window-rules/view-info", q);
    bool is_minimized = j_bool(info["info"], "minimized", false);

    // Case 1: Window is minimized - Unminimize and force to front
    if (is_minimized) {
      Json::Value d;
      d["view_id"] = (Json::Int64)view_id;
      d["state"] = false; // Reset send-to-back state
      rpc("wm-actions/set-minimized", d);
      rpc("wm-actions/send-to-back", d);
      rpc("window-rules/focus-view", q);
      focused_view_id_ = view_id;
      return;
    }

    // Case 2: Toggle logic
    if (view_id != focused_view_id_) {
      // Not focused: Reset "back" state and focus
      Json::Value d;
      d["view_id"] = (Json::Int64)view_id;
      d["state"] = false;
      rpc("wm-actions/send-to-back", d);
      rpc("window-rules/focus-view", q);

      vec_move_to_front(z_order_, view_id);
      sent_back_[view_id] = false;
      focused_view_id_ = view_id;
    } else {
      // Already focused: Send to back and clear focus state
      Json::Value d;
      d["view_id"] = (Json::Int64)view_id;
      d["state"] = true;
      rpc("wm-actions/send-to-back", d);

      vec_move_to_back(z_order_, view_id);
      sent_back_[view_id] = true;
      focused_view_id_ = -1; // Critical: allow next click to re-focus
    }
  } catch (const std::exception& e) {
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - last_error_log_).count() > 1000) {
      last_error_log_ = Clock::now();
      spdlog::warn("wayfire/taskbar click error: {}", e.what());
    }
  }
}

auto Taskbar::update() -> void {
  // One list-views per refresh, shared by output discovery and filtering.
  Json::Value lv = rpc("window-rules/list-views", Json::objectValue);
  const Json::Value& views =
      (lv.isObject() && lv.isMember("views")) ? lv["views"] : lv;
  resolve_output_id(views);
  if (!geom_synced_) sync_output_geometry();
  rebuild(list_views_filtered(views));
}

}  // namespace waybar::modules::wayfire
