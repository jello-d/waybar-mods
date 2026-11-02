#pragma once

// activation_token.hpp - mint an xdg-activation token for a click on the bar,
// header-only, for a tray on another bus (patches/tray-bus-activate.patch).
//
// WHY: an app in another session (an enclave's Slack) has no input serial of
// its own when its tray icon is clicked, so the compositor refuses to let it
// raise its window on Activate. The bar, which DID get the click, mints a
// token and hands it over (SNI ProvideXdgActivationToken) before Activate.
//
// WHY NOT GTK: GdkAppLaunchContext can mint a token, but it attaches the
// KEYBOARD-focus surface, and the bar never has keyboard focus. A token with
// no surface cannot be attributed to its creator, and wayfire-mods' activation
// patch refuses an unattributed token across a security-context boundary,
// which is exactly the case this exists for. So this attaches the BAR's
// surface, which holds pointer focus during the click (wlroots accepts
// either).
//
// THE SERIAL: GTK3 exposes no input serial, so this binds a wl_seat of its
// own, takes a wl_pointer on it once the seat HAS a pointer, and records each
// button press. The compositor sends every pointer event to every wl_pointer
// a client holds. Both live on a private queue, which mint() dispatches
// first, so the press being handled is the latest one recorded by then.
//
// Never ask GTK's seat for a pointer, and never ask before the capability:
// wlroots answers wl_seat.get_pointer on a seat that has never had a pointer
// with a PROTOCOL ERROR, which ends the bar's whole Wayland connection
// (measured, headless; a real box can be there at login before its touchpad
// registers).

#include <gdk/gdkwayland.h>
#include <spdlog/spdlog.h>
#include <wayland-client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "xdg-activation-v1-client-protocol.h"

namespace waybar::activation {

class Tokens {
 public:
  // The process-wide instance. Creating it starts recording press serials, so
  // ask for it before the first click (a tray item does, when it is made).
  static Tokens& instance() {
    static Tokens tokens;
    return tokens;
  }

  // A token for the click being handled, on the bar's surface; empty when one
  // cannot be had (not Wayland, no xdg-activation, no press seen yet).
  std::string mint(GdkWindow* window) {
    if (activation_ == nullptr || seat_ == nullptr) return "";
    // Collect the press: its event is read already, queued on our queue.
    wl_display_roundtrip_queue(display_, queue_);
    if (serial_ == 0) return "";
    struct wl_surface* surface = nullptr;
    if (window != nullptr && GDK_IS_WAYLAND_WINDOW(window)) {
      surface = gdk_wayland_window_get_wl_surface(gdk_window_get_toplevel(window));
    }
    std::string result;
    auto* token = xdg_activation_v1_get_activation_token(activation_);
    xdg_activation_token_v1_add_listener(token, &TOKEN_LISTENER, &result);
    xdg_activation_token_v1_set_serial(token, serial_, seat_);
    if (surface != nullptr) xdg_activation_token_v1_set_surface(token, surface);
    xdg_activation_token_v1_commit(token);
    // wlroots answers a commit with `done` at once, even when it rejects the
    // token (it hands back an unusable one), so one roundtrip is enough and
    // cannot hang the bar waiting for an answer that never comes.
    wl_display_roundtrip_queue(display_, queue_);
    xdg_activation_token_v1_destroy(token);
    return result;
  }

 private:
  Tokens() {
    GdkDisplay* gdk = gdk_display_get_default();
    if (gdk == nullptr || !GDK_IS_WAYLAND_DISPLAY(gdk)) return;
    display_ = gdk_wayland_display_get_wl_display(gdk);
    // Everything here lives on a queue of our own, so no roundtrip of ours
    // dispatches anything of GTK's.
    queue_ = wl_display_create_queue(display_);
    auto* wrapped = static_cast<struct wl_display*>(wl_proxy_create_wrapper(display_));
    wl_proxy_set_queue(reinterpret_cast<struct wl_proxy*>(wrapped), queue_);
    registry_ = wl_display_get_registry(wrapped);
    wl_proxy_wrapper_destroy(wrapped);
    wl_registry_add_listener(registry_, &REGISTRY_LISTENER, this);
    wl_display_roundtrip_queue(display_, queue_);  // the globals
    wl_display_roundtrip_queue(display_, queue_);  // the seat's capabilities
    if (activation_ == nullptr) {
      spdlog::warn("Tray: the compositor offers no xdg-activation; clicks cannot raise windows");
      return;
    }
    // Pointer events queue up on our queue between clicks (motion above all),
    // and only mint() would dispatch them; drain it every second so it stays
    // small. Dispatching pending events reads nothing from the socket.
    g_timeout_add_seconds(
        1,
        [](gpointer data) -> gboolean {
          auto* self = static_cast<Tokens*>(data);
          wl_display_dispatch_queue_pending(self->display_, self->queue_);
          return G_SOURCE_CONTINUE;
        },
        this);
  }

  static void global(void* data, struct wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t version) {
    auto* self = static_cast<Tokens*>(data);
    if (std::strcmp(interface, xdg_activation_v1_interface.name) == 0) {
      self->activation_ = static_cast<struct xdg_activation_v1*>(
          wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1));
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0 && self->seat_ == nullptr) {
      self->seat_ = static_cast<struct wl_seat*>(
          wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5U)));
      wl_seat_add_listener(self->seat_, &SEAT_LISTENER, self);
    }
  }
  static void global_remove(void*, struct wl_registry*, uint32_t) {}

  static void capabilities(void* data, struct wl_seat* seat, uint32_t caps) {
    auto* self = static_cast<Tokens*>(data);
    const bool has = (caps & WL_SEAT_CAPABILITY_POINTER) != 0;
    if (has && self->pointer_ == nullptr) {
      self->pointer_ = wl_seat_get_pointer(seat);
      wl_pointer_add_listener(self->pointer_, &POINTER_LISTENER, self);
    } else if (!has && self->pointer_ != nullptr) {
      if (wl_proxy_get_version(reinterpret_cast<struct wl_proxy*>(self->pointer_)) >= 3) {
        wl_pointer_release(self->pointer_);
      } else {
        wl_pointer_destroy(self->pointer_);
      }
      self->pointer_ = nullptr;
    }
  }
  static void seat_name(void*, struct wl_seat*, const char*) {}

  static void token_done(void* data, struct xdg_activation_token_v1*, const char* token) {
    *static_cast<std::string*>(data) = token;
  }

  static void button(void* data, struct wl_pointer*, uint32_t serial, uint32_t, uint32_t,
                     uint32_t state) {
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) static_cast<Tokens*>(data)->serial_ = serial;
  }
  // Every other pointer event is ignored, but each needs a handler: libwayland
  // calls the listener slot for whatever the compositor sends.
  static void enter(void*, struct wl_pointer*, uint32_t, struct wl_surface*, wl_fixed_t,
                    wl_fixed_t) {}
  static void leave(void*, struct wl_pointer*, uint32_t, struct wl_surface*) {}
  static void motion(void*, struct wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {}
  static void axis(void*, struct wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {}
  static void frame(void*, struct wl_pointer*) {}
  static void axis_source(void*, struct wl_pointer*, uint32_t) {}
  static void axis_stop(void*, struct wl_pointer*, uint32_t, uint32_t) {}
  static void axis_discrete(void*, struct wl_pointer*, uint32_t, int32_t) {}
  static void axis_value120(void*, struct wl_pointer*, uint32_t, int32_t) {}
  static void axis_relative_direction(void*, struct wl_pointer*, uint32_t, uint32_t) {}

  static constexpr struct wl_registry_listener REGISTRY_LISTENER = {global, global_remove};
  static constexpr struct wl_seat_listener SEAT_LISTENER = {capabilities, seat_name};
  static constexpr struct xdg_activation_token_v1_listener TOKEN_LISTENER = {token_done};
  static constexpr struct wl_pointer_listener POINTER_LISTENER = {enter,
                                                                  leave,
                                                                  motion,
                                                                  button,
                                                                  axis,
                                                                  frame,
                                                                  axis_source,
                                                                  axis_stop,
                                                                  axis_discrete,
                                                                  axis_value120,
                                                                  axis_relative_direction};

  struct wl_display* display_ = nullptr;
  struct wl_event_queue* queue_ = nullptr;
  struct wl_registry* registry_ = nullptr;
  struct wl_seat* seat_ = nullptr;
  struct wl_pointer* pointer_ = nullptr;
  struct xdg_activation_v1* activation_ = nullptr;
  uint32_t serial_ = 0;
};

}  // namespace waybar::activation
