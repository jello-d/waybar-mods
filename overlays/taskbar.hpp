#pragma once

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/icontheme.h>
#include <giomm/desktopappinfo.h>
#include <json/json.h>

#include <cstdint>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "AModule.hpp"
#include "bar.hpp"

namespace waybar::modules::wayfire {

struct View {
  int64_t id = -1;
  std::string app_id;
  std::string title;
  bool activated = false;
  bool sticky = false;
  bool minimized = false;
  bool mapped = false;
  std::string layer;
  std::string role;
  std::string type;
  int64_t output_id = -1;
  int64_t wset_index = -1;
  // bbox:
  int64_t bx = 0, by = 0, bw = 0, bh = 0;
};

class Taskbar final : public AModule {
 public:
  Taskbar(const std::string& id, const waybar::Bar& bar,
          const Json::Value& config);
  ~Taskbar();

  auto update() -> void override;

 private:
  const waybar::Bar& bar_;
  Json::Value config_;

  Gtk::Box box_;
  Glib::RefPtr<Gtk::IconTheme> icon_theme_;

  std::string socket_path_;
  bool all_workspaces_ = false;
  int button_spacing_ = 6;
  int button_min_width_ = 0;     // title-drop threshold; 0=auto, not a floor
  int button_max_width_ = 320;
  int taskbar_width_ = 2400;     // available row width (event box), computed

  // focused output geometry:
  int64_t focused_output_id_ = -1;
  int64_t ws_x_ = 0;
  int64_t ws_y_ = 0;
  int64_t out_x_ = 0;
  int64_t out_y_ = 0;
  int64_t out_w_ = 0;
  int64_t out_h_ = 0;

  int64_t focused_view_id_ = -1;
  std::vector<int64_t> z_order_;
  std::map<int64_t, int64_t> focus_ts_;
  std::map<int64_t, bool> sent_back_;


  // UI: view_id -> button
  struct Item {
    Gtk::Button button;
    Gtk::Box content;
    Gtk::Image icon;
    Gtk::Label label;
    std::string app_id;  // key the resolved icon was set for; skip re-resolve
  };
  std::map<int64_t, std::unique_ptr<Item>> items_;

  // IPC
  int sock_fd_ = -1;                  // request/response connection
  int evt_fd_  = -1;                  // event-stream connection
  sigc::connection evt_io_;           // GLib IO watch on evt_fd_
  sigc::connection refresh_pending_;  // debounce timer
  sigc::connection timer_;
  int refresh_debounce_ms_ = 40;
  int reconcile_ms_ = 10000;
  bool geom_synced_ = false;  // output geometry+workspace cache valid?

  void connect_socket();
  void disconnect_socket();
  Json::Value rpc(const std::string& method, const Json::Value& data);

  void start_events();
  bool on_event_io(Glib::IOCondition cond);
  void schedule_update();
  bool on_refresh();
  void safe_update();

  // queries
  void resolve_output_id(const Json::Value& views);  // cheap scan, no RPC
  void sync_output_geometry();                        // RPC: geometry + ws
  std::vector<View> list_views_filtered(const Json::Value& views);

  // ui
  void rebuild(const std::vector<View>& views);
  void on_click(int64_t view_id);

  // helpers
  bool on_timer();
  bool bbox_center_on_output(const View& v) const;
  static Glib::RefPtr<Gio::DesktopAppInfo> find_app_info(
                                            const std::string& app_id);
  static void set_icon(Gtk::Image& img,
                        const Glib::RefPtr<Gtk::IconTheme>& theme,
                        const Glib::RefPtr<Gio::DesktopAppInfo>& app_info,
                        int size);

  using Clock = std::chrono::steady_clock;
  Clock::time_point last_debug_log_ = Clock::now();
  Clock::time_point last_error_log_ = Clock::now();

  // Map view_id -> last known output and workspace coordinates
  std::map<int64_t, std::pair<int, int>> last_known_ws_;

  int64_t last_focused_output_id_ = -999;
  int64_t last_out_x_ = 0;
  int64_t last_out_y_ = 0;
  int64_t last_out_w_ = -1;
  int64_t last_out_h_ = -1;
  int last_total_views_ = -1;
  int last_filtered_views_ = -1;
};

}  // namespace waybar::modules::wayfire
