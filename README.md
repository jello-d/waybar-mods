# waybar-mods

A Waybar distribution: pristine upstream
[Waybar](https://github.com/Alexays/Waybar) plus a set of custom Cairo modules
and behaviour patches, built from source into a prefix.

It is **not a fork**. Upstream is fetched at a known-good tag (`refs`), our
additions are dropped onto a clean checkout as overlay sources and applied as
patches, and the result is compiled. Every addition therefore stays a discrete,
upstreamable unit, and the base is refreshed by bumping a single ref rather than
by rebasing a divergent tree. When an addition lands upstream, delete it here
and bump the pin; when the set empties, this repo degenerates to a pass-through
build of pristine Waybar.

## What it adds

Overlay modules (in `overlays/`, registered via `overlays/registration.patch`):

- `wayfire/taskbar` — lists and switches the compositor's windows.
- `sysmon/graph` — a per-metric system monitor drawn as a Cairo line graph
  (this replaced cava, which is disabled in the build).
- `hw/gauge` — brightness / volume / battery, drawn as Cairo glyphs.
- `clock/analog` — a Cairo analog clock face beside the digital clock.
- `media/card` — a now-playing card (cover art, marquee, transport, scrubber).
- `hw/bluetooth` — a Bluetooth indicator reading BlueZ over D-Bus.
- `cal/card` — a next-meeting card with a live countdown.

Behaviour patches (in `patches/`):

- `flush-tooltips` — route every module's tooltip through one shared popup.
- `hover-zoom` — a hover magnifier overlay (opt-in per module).
- `tray-sort` — a stable, config-driven SNI tray order.
- `sni-refetch` — re-fetch SNI properties when the proxy cache races empty, so
  Chromium/Electron tray icons are not dropped.
- `spectrum-pulse` — link libpulse-simple for the media card's audio spectrum.

## Use

```sh
./setup.sh install     # build + install into ~/.local (incremental, no sudo)
./setup.sh verify      # build into a throwaway prefix; prove it still compiles
./setup.sh check       # installed + self-contained + in sync? [OK]/[FAIL]
./setup.sh uninstall   # remove the installed binary + stamp
./setup.sh version     # package version + upstream pin
```

`install` is incremental: a no-op unless an overlay, a patch, or the upstream
pin changed, so it is safe to run on every provision. The build is entirely
unprivileged (a user build into a user prefix); the installed binary carries a
relative `$ORIGIN/../lib` rpath, so the prefix is self-contained.

`overlays/` and `patches/` are Waybar/GTK C++ (gtkmm, cairomm, jsoncpp,
spdlog); follow upstream Waybar's style there, not any consumer's shell
conventions.

## Two-level pin

`refs` pins upstream Waybar to a real tag that this overlay set is tested
against. A consumer tracks *waybar-mods* at its tip; waybar-mods tracks upstream
at `refs`. Bump `WAYBAR_REF` only after `setup.sh verify` (or CI) proves the
overlays and patches still apply and build against the new upstream.

## License

Apache-2.0. Upstream Waybar is MIT; its source is fetched at build time, not
vendored here.
