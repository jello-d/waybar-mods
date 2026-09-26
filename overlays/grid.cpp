#include "modules/wayfire/grid.hpp"

#include <glibmm.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace waybar::modules::wayfire {

namespace wfi = waybar::wf;

static constexpr double kPi = 3.14159265358979323846;

// The UNLIT cell: opaque, and deliberately a MID slate rather than a dark one.
// The bar is translucent, so the colour it effectively shows moves with the
// wallpaper behind it: a dark fill would vanish into the bar over a dark
// wallpaper, and a light one would compete with the lit cell. Sitting between
// the two keeps the grid legible in BOTH directions, whatever is behind. Tinted
// toward the bar's purple family so it belongs to the same chrome. One place to
// tune if the balance ever needs adjusting.
//
// The value was chosen by RENDERING candidates over both a dark and a light
// wallpaper rather than by eye in the abstract, because the light case is the
// binding one: the bar lightens with the wallpaper behind it, so a tone that
// looks fine over a dark desktop can slide into the bar over a light one. 0.34
// nearly merged there and 0.30 was marginal; 0.22 bought little over 0.26 while
// dulling the dark case. Re-render before changing it.
static constexpr double kOffR = 0.26, kOffG = 0.24, kOffB = 0.31;

Grid::Grid(const std::string& id, const waybar::Bar& bar,
           const Json::Value& config)
    : AModule(config, "wfgrid", id, /*enable_click=*/false,
              /*enable_scroll=*/false),
      bar_(bar) {
  if (config_["vmargin"].isInt()) vmargin_ = config_["vmargin"].asInt();
  if (config_["hmargin"].isInt()) hmargin_ = config_["hmargin"].asInt();
  if (config_["gap"].isNumeric()) gap_ = config_["gap"].asDouble();
  if (config_["reconcile"].isNumeric())
    reconcile_ms_ = static_cast<int>(config_["reconcile"].asDouble() * 1000.0);

  // Scale with the BAR height (48 = design), the same factor every native
  // module here uses, so a taller bar enlarges the grid with no per-bar config.
  ui_ = bar_.config["height"].isInt() ? bar_.config["height"].asInt() / 48.0
                                      : 1.0;
  gap_ *= ui_;

  socket_path_ = wfi::resolve_socket(config_);
  ipc_ = std::make_unique<wfi::Client>(socket_path_);

  event_box_.set_name("wfgrid" + (id.empty() ? std::string() : "-" + id));
  area_.set_hexpand(false);
  area_.set_margin_top(vmargin_);
  area_.set_margin_bottom(vmargin_);
  area_.set_margin_start(hmargin_);
  area_.set_margin_end(hmargin_);
  area_.signal_draw().connect(sigc::mem_fun(*this, &Grid::on_draw));
  event_box_.add(area_);
  event_box_.show_all();

  area_.add_events(Gdk::BUTTON_PRESS_MASK);
  area_.signal_button_press_event().connect(
      sigc::mem_fun(*this, &Grid::on_press));

  // Event-driven: paint once, then repaint on the workspace/output events, with
  // a slow reconcile as the backstop. No fast poll.
  safe_update();
  start_events();
  if (reconcile_ms_ > 0) {
    timer_ = Glib::signal_timeout().connect(
        sigc::mem_fun(*this, &Grid::on_timer), reconcile_ms_);
  }
}

Grid::~Grid() {
  if (evt_io_.connected()) evt_io_.disconnect();
  if (refresh_pending_.connected()) refresh_pending_.disconnect();
  if (timer_.connected()) timer_.disconnect();
  if (evt_fd_ >= 0) {
    ::close(evt_fd_);
    evt_fd_ = -1;
  }
}

// ---- IPC -----------------------------------------------------------------

void Grid::start_events() {
  try {
    if (evt_fd_ >= 0) {
      ::close(evt_fd_);
      evt_fd_ = -1;
    }
    evt_fd_ = wfi::open_event_stream(socket_path_);
    // The subscription ack is just a non-event message on_event_io reads and
    // ignores, so there is nothing to drain synchronously.
    evt_io_ = Glib::signal_io().connect(
        sigc::mem_fun(*this, &Grid::on_event_io), evt_fd_,
        Glib::IO_IN | Glib::IO_HUP | Glib::IO_ERR);
  } catch (const std::exception& e) {
    if (evt_fd_ >= 0) {
      ::close(evt_fd_);
      evt_fd_ = -1;
    }
    spdlog::warn("wayfire/grid: event subscribe failed ({}); retrying",
                 e.what());
    Glib::signal_timeout().connect_once(
        sigc::mem_fun(*this, &Grid::start_events), 1000);
  }
}

bool Grid::on_event_io(Glib::IOCondition cond) {
  if (cond & (Glib::IO_HUP | Glib::IO_ERR)) {
    if (evt_fd_ >= 0) {
      ::close(evt_fd_);
      evt_fd_ = -1;
    }
    Glib::signal_timeout().connect_once(
        sigc::mem_fun(*this, &Grid::start_events), 1000);
    return false;
  }
  try {
    const Json::Value root = wfi::read_msg(evt_fd_);
    if (root.isObject() && root.isMember("event")) {
      // Only what can move this widget. Deliberately NOT the view-* family:
      // windows opening, closing, moving and gaining focus cannot change which
      // cell you are in, and on a busy desktop they are most of the traffic.
      const std::string ev = root["event"].asString();
      if (ev == "wset-workspace-changed" || ev == "output-wset-changed" ||
          ev == "output-layout-changed" || ev == "output-added" ||
          ev == "output-removed" || ev == "output-gain-focus") {
        schedule_update();
      }
    }
  } catch (const std::exception&) {
    if (evt_fd_ >= 0) {
      ::close(evt_fd_);
      evt_fd_ = -1;
    }
    Glib::signal_timeout().connect_once(
        sigc::mem_fun(*this, &Grid::start_events), 1000);
    return false;
  }
  return true;
}

void Grid::schedule_update() {
  // Trailing debounce: a workspace switch arrives with output events around it,
  // so a burst collapses into one refresh once it settles.
  if (refresh_pending_.connected()) refresh_pending_.disconnect();
  refresh_pending_ = Glib::signal_timeout().connect(
      sigc::mem_fun(*this, &Grid::on_refresh), debounce_ms_);
}

bool Grid::on_refresh() {
  safe_update();
  return false;   // one-shot
}

// The reconcile tick. It is not belt-and-braces: wayfire emits NO event when
// the workspace GRID is resized (the event list has wset-workspace-changed but
// nothing for the grid), and the grid here is applied live per display shape
// (3x3 on one panel, 2x2 on the wall) by apply-wayfire-runtime. So a monitor
// swap can change the grid under us with nothing to announce it, and this tick
// is what notices. Keep it.
bool Grid::on_timer() {
  safe_update();
  return true;
}

void Grid::safe_update() {
  try {
    update();
  } catch (const std::exception& e) {
    // Rate-limited: a compositor restart would otherwise flood the log at the
    // reconcile rate for as long as it is away.
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - last_error_log_)
            .count() > 1000) {
      last_error_log_ = Clock::now();
      spdlog::warn("wayfire/grid update error: {}", e.what());
    }
  }
}

// One RPC carries everything: this output's id, its pixel geometry (the cell
// aspect) and its workspace block (the grid size and current cell). Matching on
// the output NAME is what keeps a bar on its own monitor, since every output
// has its own workspace set and therefore its own current cell.
auto Grid::update() -> void {
  const Json::Value outs = ipc_->call("window-rules/list-outputs",
                                     Json::Value(Json::objectValue));
  if (!outs.isArray()) throw std::runtime_error("list-outputs not an array");

  const std::string my_name = bar_.output ? bar_.output->name : std::string();
  const Json::Value* mine = nullptr;
  for (Json::ArrayIndex i = 0; i < outs.size(); i++) {
    if (wfi::j_str(outs[i], "name", "") == my_name) {
      mine = &outs[i];
      break;
    }
  }
  if (mine == nullptr) {
    // Our output is not in the list (mid hotplug, or a bar on an output wayfire
    // has already dropped). Keep the last good paint rather than blanking, and
    // let the next event or reconcile settle it.
    spdlog::debug("wayfire/grid: output '{}' not listed yet", my_name);
    return;
  }

  const int old_w = grid_w_, old_h = grid_h_;
  output_id_ = wfi::j_i64(*mine, "id", -1);
  const Json::Value& ws = (*mine)["workspace"];
  grid_w_ = static_cast<int>(wfi::j_i64(ws, "grid_width", 0));
  grid_h_ = static_cast<int>(wfi::j_i64(ws, "grid_height", 0));
  ws_x_ = static_cast<int>(wfi::j_i64(ws, "x", 0));
  ws_y_ = static_cast<int>(wfi::j_i64(ws, "y", 0));

  const double ow = static_cast<double>(wfi::j_i64((*mine)["geometry"],
                                                   "width", 0));
  const double oh = static_cast<double>(wfi::j_i64((*mine)["geometry"],
                                                   "height", 0));
  if (ow > 0.0 && oh > 0.0) cell_aspect_ = ow / oh;

  // A 1x1 grid means this box has no virtual desktops, so the widget has
  // nothing to say: collapse rather than draw a single lit box that can never
  // change. Same reveal/collapse idiom hw/gauge uses for absent hardware.
  const bool useful = (grid_w_ * grid_h_) > 1;
  if (useful) {
    if (grid_w_ != old_w || grid_h_ != old_h) apply_width();
    event_box_.set_no_show_all(false);
    event_box_.show_all();
  } else {
    event_box_.set_no_show_all(true);
    event_box_.hide();
  }

  update_tooltip();
  area_.queue_draw();
}

// ---- geometry ------------------------------------------------------------

// Cells are shaped like the OUTPUT, so the widget reads as a map of screens
// rather than a generic table. Height is the budget (the bar is short and
// wide), so cell height comes from the allocation and cell width follows it.
Grid::Layout Grid::layout(int w, int h) const {
  Layout l;
  if (grid_w_ <= 0 || grid_h_ <= 0) return l;
  const double gaps_y = gap_ * (grid_h_ - 1);
  l.cell_h = (h - gaps_y) / grid_h_;
  if (l.cell_h < 1.0) l.cell_h = 1.0;
  l.cell_w = l.cell_h * cell_aspect_;
  const double total_w = l.cell_w * grid_w_ + gap_ * (grid_w_ - 1);
  const double total_h = l.cell_h * grid_h_ + gaps_y;
  l.x0 = (w - total_w) / 2.0;    // centred, so a width request that is off by a
  l.y0 = (h - total_h) / 2.0;    // pixel does not shove the grid off-centre
  return l;
}

int Grid::want_width() const {
  if (grid_w_ <= 0 || grid_h_ <= 0) return 1;
  // The drawing height is not known until the first allocation, so the REQUEST
  // is computed from the configured bar height instead. layout() then works off
  // the real allocation, and centres, so the two disagreeing is cosmetic.
  const double nominal_h =
      (bar_.config["height"].isInt() ? bar_.config["height"].asInt() : 48) -
      2.0 * vmargin_;
  const double cell_h = (nominal_h - gap_ * (grid_h_ - 1)) / grid_h_;
  const double cell_w = std::max(1.0, cell_h) * cell_aspect_;
  return static_cast<int>(cell_w * grid_w_ + gap_ * (grid_w_ - 1) + 0.5);
}

void Grid::apply_width() { area_.set_size_request(want_width(), -1); }

// ---- drawing -------------------------------------------------------------

bool Grid::on_draw(const Cairo::RefPtr<Cairo::Context>& cr) {
  const int w = area_.get_allocated_width();
  const int h = area_.get_allocated_height();
  if (w <= 0 || h <= 0 || grid_w_ <= 0 || grid_h_ <= 0) return true;

  cr->set_antialias(Cairo::ANTIALIAS_DEFAULT);
  auto sc = event_box_.get_style_context();
  const Gdk::RGBA col = sc->get_color(sc->get_state());
  const double r = col.get_red(), g = col.get_green(), b = col.get_blue();

  const Layout l = layout(w, h);
  // Corner radius scales with the cell but stays small, so a cell reads as a
  // panel with rounded corners rather than a lozenge.
  const double rad = std::min(2.5 * ui_, std::min(l.cell_w, l.cell_h) * 0.22);

  auto rounded = [&](double x, double y, double cw, double ch, double rr) {
    cr->begin_new_path();
    cr->arc(x + cw - rr, y + rr, rr, -0.5 * kPi, 0.0);
    cr->arc(x + cw - rr, y + ch - rr, rr, 0.0, 0.5 * kPi);
    cr->arc(x + rr, y + ch - rr, rr, 0.5 * kPi, kPi);
    cr->arc(x + rr, y + rr, rr, kPi, 1.5 * kPi);
    cr->close_path();
  };

  // Every cell is an OPAQUE fill and only the GAPS carry alpha. This is the
  // whole contrast story, and the reason the first version washed out: the
  // bar's background is itself semi-transparent (0.74 over the wallpaper), so
  // anything drawn at low alpha composites against whatever is BEHIND the bar,
  // not against the bar. The old thin outline and its faint wash disappeared
  // over a light or busy wallpaper while the near-opaque lit cell survived.
  // Opaque fills cannot wash out at all, and leaving the gaps transparent is
  // what still reads as separate floating displays.
  for (int cy = 0; cy < grid_h_; cy++) {
    for (int cx = 0; cx < grid_w_; cx++) {
      const double x = l.x0 + cx * (l.cell_w + gap_);
      const double y = l.y0 + cy * (l.cell_h + gap_);
      const bool here = (cx == ws_x_ && cy == ws_y_);
      rounded(x, y, l.cell_w, l.cell_h, rad);
      if (here) {
        cr->set_source_rgba(r, g, b, 1.0);        // lit: the themed fg
      } else {
        cr->set_source_rgba(kOffR, kOffG, kOffB, 1.0);
      }
      cr->fill();
    }
  }
  return true;
}

// ---- input ---------------------------------------------------------------

// Left click switches to the cell under the pointer. The hit test CLAMPS rather
// than rejecting a press that landed in a gap or the centring margin: the cells
// are a few pixels each on a 48px bar, so "nearest cell" is what the user meant
// every time, and a dead zone between them would just feel broken.
bool Grid::on_press(GdkEventButton* e) {
  if (e->button != 1 || grid_w_ <= 0 || grid_h_ <= 0) return false;
  const int w = area_.get_allocated_width();
  const int h = area_.get_allocated_height();
  const Layout l = layout(w, h);
  if (l.cell_w <= 0.0 || l.cell_h <= 0.0) return false;

  const int cx = std::clamp(
      static_cast<int>(std::floor((e->x - l.x0) / (l.cell_w + gap_))), 0,
      grid_w_ - 1);
  const int cy = std::clamp(
      static_cast<int>(std::floor((e->y - l.y0) / (l.cell_h + gap_))), 0,
      grid_h_ - 1);
  switch_to(cx, cy);
  return true;
}

void Grid::switch_to(int cx, int cy) {
  if (output_id_ < 0) return;
  // Already here: say nothing. vswitch would run its slide animation for a
  // move of zero, so a stray click on the lit cell should cost nothing.
  if (cx == ws_x_ && cy == ws_y_) return;
  Json::Value d;
  d["x"] = cx;
  d["y"] = cy;
  d["output-id"] = static_cast<Json::Int64>(output_id_);
  try {
    ipc_->call("vswitch/set-workspace", d);
  } catch (const std::exception& err) {
    spdlog::warn("wayfire/grid: set-workspace({},{}) failed: {}", cx, cy,
                 err.what());
    return;
  }
  // Paint the new cell now rather than waiting for the event round trip, so the
  // click feels immediate; the wset-workspace-changed that follows confirms it.
  ws_x_ = cx;
  ws_y_ = cy;
  update_tooltip();
  area_.queue_draw();
}

// Row-major and 1-BASED, matching the vswitch select_workspace_N keybinds, so
// the number here is the number you would type to get here. The internal
// coordinates are 0-based; this is the one place that difference is bridged.
void Grid::update_tooltip() {
  if (grid_w_ <= 0 || grid_h_ <= 0) return;
  char buf[96];
  const int n = ws_y_ * grid_w_ + ws_x_ + 1;
  std::snprintf(buf, sizeof buf, "Workspace %d of %d  (%dx%d grid)", n,
                grid_w_ * grid_h_, grid_w_, grid_h_);
  event_box_.set_tooltip_text(buf);
}

}  // namespace waybar::modules::wayfire
