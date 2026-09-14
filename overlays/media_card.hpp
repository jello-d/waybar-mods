#pragma once

#include <gtkmm/drawingarea.h>
#include <cairomm/context.h>
#include <gdkmm/pixbuf.h>
#include <json/json.h>

#include <memory>
#include <string>
#include <vector>

#include "AModule.hpp"
#include "bar.hpp"
#include "np_frame.hpp"
#include "spectrum.hpp"

namespace waybar::modules::media {

// A DrawingArea that reports a flexible width [min, natural] instead of a hard
// fixed size, so the bar shrinks the card when the tray/other modules need room
// and gives it its full width when there is slack. The draw already adapts to
// whatever width it is allocated.
class CardArea : public Gtk::DrawingArea {
 public:
  int min_w = 210, nat_w = 380;

 protected:
  void get_preferred_width_vfunc(int& minimum, int& natural) const override {
    minimum = min_w;
    natural = nat_w;
  }
};

// A "now playing" card drawn in Cairo: a rounded cover-art thumbnail, the title
// over artist / album, a play-state glyph, a cast badge, prev / play / next
// transport, and a progress scrubber.
//
// A DUMB VIEW, deliberately. Everything it draws comes from the now-playing
// daemon's shared-memory frame (np_frame.hpp; the spec is that package's
// docs/contract.md): playback state, position, which transport controls are
// usable, and eventually the spectrum. The card derives NO playback facts of
// its own. It does not decide what "idle" means, does not extrapolate the
// playhead from a wall clock, and does not ask any second source what is
// playing. Transport goes back out through np-ctl, which the card treats as an
// opaque command; it does not model what a command will do.
//
// What IS the card's own business: layout, colour, type, glyph shapes, the
// marquee phase, peak-hold cap physics, decoding the cover art, and which
// gesture maps to which command. Registered as `media/card`.
class Card final : public waybar::AModule {
 public:
  Card(const std::string& id, const waybar::Bar& bar,
       const Json::Value& config);
  ~Card();

  auto update() -> void override;

 private:
  const waybar::Bar& bar_;

  // config (defaults in the ctor)
  int width_ = 380;      // card natural (max) width (px)
  int min_width_ = 210;  // shrink floor when the row is squeezed
  int vmargin_ = 3;      // bar padding above/below
  int hmargin_ = 6;      // bar padding left/right
  double ui_scale_ = 1.0;    // bar_height / 48; scales the text
                             // (1.0 on the base bar -> a strict no-op)
  std::string ctl_cmd_;      // np-ctl helper (transport)

  CardArea area_;
  sigc::connection frame_;   // the ONE clock: samples the frame and redraws

  // The daemon's frame, verbatim. This is the card's whole model; no field
  // below is computed from anything else.
  np::Reader reader_;
  np::Frame f_;
  bool live_ = false;        // a fresh frame was read (else render nothing)
  bool warned_version_ = false;

  // title marquee (Winamp-style linger-then-scroll)
  double marquee_t0_ = 0.0;   // monotonic ref, reset on each new track
  std::uint32_t marquee_track_ = 0;   // the daemon's track id the phase is for
  bool overflowing_ = false;  // title wider than column -> 30fps redraw
  unsigned tick_ = 0;         // frame counter (marquee 30fps vs scrubber ~4fps)
  bool hovered_ = false;      // pointer over card -> refresh tooltip

  // cover-art decode cache (keyed by path, re-decoded only on change)
  std::string art_loaded_;
  Glib::RefPtr<Gdk::Pixbuf> art_;

  // click hit-rects, recomputed each draw (x, then width)
  int prev_x_ = 0, prev_w_ = 0, play_x_ = 0, play_w_ = 0,
      next_x_ = 0, next_w_ = 0;

  // ---- audio spectrum (Winamp-style analyzer) -------------------------------
  // The bands come from the FRAME when the daemon publishes them: the card is
  // not told, and must not care, whether they were derived from a local sink
  // monitor or from a cast-side reconstruction.
  //
  // TRANSITIONAL: until the daemon owns the analyser, a local capture here
  // fills in when the frame carries no bands. That fallback (and spectrum.hpp,
  // and every spectrum-* DSP key in this module's config) is scheduled for
  // deletion; do not build on it. Signal config belongs with the producer, and
  // only the LOOK keys below stay here.
  bool spectrum_on_ = false;
  std::unique_ptr<Spectrum> spec_;
  std::vector<float> levels_;   // current band levels 0..1 (from spec_)
  std::vector<float> caps_;     // peak-hold cap height per band 0..1
  std::vector<float> cap_vel_;  // cap fall velocity (units/sec)
  std::vector<double> cap_hold_;// remaining hold time per band (sec)
  double spec_last_ = 0.0;      // monotonic secs of the last cap-animation step
  // render / animation tunables (config, defaults here)
  double cap_hold_s_ = 0.5;     // how long a peak cap hangs before falling
  double cap_gravity_ = 2.6;    // cap fall acceleration (units/sec^2)
  double dot_h_ = 3.0;          // dithered dot cell height (px, pre-ui-scale)
  double dot_gap_ = 1.4;        // vertical gap between dots (px)
  double spec_alpha_ = 0.85;    // overall opacity (contrast tuned later)
  double text_scrim_ = 0.35;    // dark plate opacity behind the text column
                                // (0 disables), a contrast floor for the names
  double text_outline_ = 2.0;   // black outline (px) around the text glyphs, so
                                // the names read over the colored analyzer

  bool pollFrame();       // sample the daemon; true when the face changed
  void updateTooltip();   // full, untruncated info for the shared flush callout
  void ensureArt();
  void sendCtl(const std::string& cmd);

  bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr);
  bool on_button(GdkEventButton* e);
  bool on_scroll(GdkEventScroll* e);
  bool on_frame();

  // Pull the latest bands and advance the peak caps; returns true while any
  // level or cap is still visibly above zero (so redraws stop when it settles).
  bool updateSpectrum();
  // Paint the dithered analyzer confined to the horizontal span [bx0, bx1]
  // (the progress-bar column), full card height h.
  void drawSpectrum(const Cairo::RefPtr<Cairo::Context>& cr, double bx0,
                    double bx1, double h);

  // Cairo glyph helpers (no font glyphs, like hw/gauge).
  static void glyphPlay(const Cairo::RefPtr<Cairo::Context>& cr, double cx,
                        double cy, double s);
  static void glyphPause(const Cairo::RefPtr<Cairo::Context>& cr, double cx,
                         double cy, double s);
  static void glyphPrev(const Cairo::RefPtr<Cairo::Context>& cr, double cx,
                        double cy, double s);
  static void glyphNext(const Cairo::RefPtr<Cairo::Context>& cr, double cx,
                        double cy, double s);
  static void glyphCast(const Cairo::RefPtr<Cairo::Context>& cr, double x,
                        double y, double s);
};

}  // namespace waybar::modules::media
