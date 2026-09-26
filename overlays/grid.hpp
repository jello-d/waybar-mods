#pragma once

#include <gtkmm/drawingarea.h>
#include <cairomm/context.h>
#include <json/json.h>

#include <chrono>
#include <memory>
#include <string>

#include "AModule.hpp"
#include "bar.hpp"
#include "wf_ipc.hpp"

namespace waybar::modules::wayfire {

// The virtual desktop GRID: wayfire's workspace grid drawn as cells, with the
// one you are on filled. A position indicator, deliberately not a window
// inventory -- wayfire/taskbar already lists the current workspace's windows,
// and this answers the one question that has no other on-screen answer, "which
// cell am I in".
//
// Everything comes from ONE existing RPC, window-rules/list-outputs, which
// returns per output: the id, the pixel geometry, and a workspace block of
// {x, y, grid_width, grid_height}. So the grid SIZE is read from the
// compositor, never configured: it is applied live from
// ~/.config/shapes/<shape>/wayfire-runtime.conf (3x3 on one panel, 2x2 on the
// three-monitor wall) and a config key here would be a second copy of a fact
// that changes underneath it on a monitor swap.
//
// Coordinates are 0-BASED, as wayfire's own bounds check has them
// (is_workspace_valid rejects x >= grid.width and x < 0), so on a 3x3 grid
// (1,1) is the CENTRE cell and not the top left. The tooltip converts to the
// 1-based row-major number that the vswitch select_workspace_N keybinds use,
// so the widget and the keymap count the same way.
//
// Per OUTPUT: each output carries its own workspace set, so each bar draws its
// own monitor's cell, resolved through bar_.output like wayfire/taskbar.
//
// Left click switches, via vswitch/set-workspace. Middle and right are
// deliberately UNBOUND: set-workspace also accepts a view-id to carry a window
// along, but this session runs follow-focus with change_view and a 10ms
// focus_delay, so by the time the pointer has crossed the desktop to reach the
// bar the "focused" view is plausibly something merely traversed -- and with
// raise_on_top false it would not even be the window on top. Carrying a window
// stays a keybind (vswitch with_win_N), where the intent is unambiguous.
class Grid final : public waybar::AModule {
 public:
  Grid(const std::string& id, const waybar::Bar& bar,
       const Json::Value& config);
  ~Grid();

  auto update() -> void override;

 private:
  const waybar::Bar& bar_;

  // config
  int vmargin_ = 4;
  int hmargin_ = 3;
  double gap_ = 2.0;            // between cells, scaled by the bar height
  int reconcile_ms_ = 10000;    // see on_timer: covers a missing grid event
  int debounce_ms_ = 40;
  double ui_ = 1.0;             // bar height / 48, the house scaling factor

  Gtk::DrawingArea area_;

  // live state, from list-outputs. grid_w_/grid_h_ 0 means "not learned yet",
  // which draws nothing rather than guessing a grid.
  int64_t output_id_ = -1;
  int grid_w_ = 0, grid_h_ = 0;
  int ws_x_ = 0, ws_y_ = 0;
  double cell_aspect_ = 16.0 / 10.0;   // from the output's own geometry

  std::string socket_path_;
  std::unique_ptr<waybar::wf::Client> ipc_;   // request/response channel
  int evt_fd_ = -1;                           // event-stream connection
  sigc::connection evt_io_;
  sigc::connection refresh_pending_;
  sigc::connection timer_;

  using Clock = std::chrono::steady_clock;
  Clock::time_point last_error_log_ = Clock::now();

  void start_events();
  bool on_event_io(Glib::IOCondition cond);
  void schedule_update();
  bool on_refresh();
  bool on_timer();
  void safe_update();

  // geometry of the drawn grid, derived from the allocation + the grid size
  struct Layout {
    double cell_w = 0.0, cell_h = 0.0, x0 = 0.0, y0 = 0.0;
  };
  Layout layout(int w, int h) const;
  int want_width() const;          // width request for the learned grid
  void apply_width();

  bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr);
  bool on_press(GdkEventButton* e);
  void switch_to(int cx, int cy);
  void update_tooltip();
};

}  // namespace waybar::modules::wayfire
