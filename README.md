# steam-scale

Per-monitor fractional scaling for the Steam client on Wayland.

Steam runs under XWayland and draws every window at one scale. With steam-scale each Steam window is drawn
at the scale of the monitor it is on, and follows the window when it moves to another monitor or when a
monitor's scale changes. Nothing is configured: the scales are read from the compositor.

It needs XWayland to run unscaled, so that X11 applications see real pixels (on Hyprland:
`xwayland { force_zero_scaling = true }`; on KDE Plasma: "Apply scaling themselves"). Where XWayland is
scaled by the compositor, steam-scale changes nothing.

## Build and install

Needs a C compiler, make, pkg-config, wayland-scanner, wayland-protocols and the development files of
libxcb (with RandR) and libwayland-client.

    make
    sudo make install            # into /usr/local

or, without root:

    make install PREFIX=$HOME/.local

## Run

Start Steam with `steam-scale` in place of `steam`; arguments are passed on.

    steam-scale

It preloads `libsteamscale.so` into Steam. The library acts only in Steam's own interface process and removes
itself from the environment of games.

| Variable | Meaning |
|----------|---------|
| `STEAM_SCALE_STEAM` | Steam launcher to run, when it is not `steam` |
| `STEAM_SCALE_MONITORS` | `X,Y,W,H=SCALE;...` in X11 pixels. Replaces detection |
| `STEAM_SCALE_LOG` | File to append a log to |
