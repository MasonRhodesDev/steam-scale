# steam-scale

Per-monitor fractional scaling for the Steam client on Wayland.

![The same Steam window on a 150% monitor, without steam-scale and with it](docs/before-after.png)

The same Steam window, at the same size, on a 150% monitor. Left: stock Steam draws at 100%, so everything is
small. Right: with steam-scale it draws at 150%. Account details are blurred.

Steam runs under XWayland and draws every window at one scale, so on a scaled monitor its text is small.
With steam-scale, each Steam window is drawn at the scale of the monitor it is on, and it follows the window
from monitor to monitor. There is nothing to configure. Steam's files are not changed, and games are not
affected.

## Install

Download packages from the [latest release](https://github.com/MasonRhodesDev/steam-scale/releases/latest).

| Distribution | Command |
|--------------|---------|
| Arch | `sudo pacman -U steam-scale-*.pkg.tar.zst` |
| Debian, Ubuntu | `sudo apt install ./steam-scale_*_amd64.deb` |
| Fedora | `sudo dnf install ./steam-scale-*.x86_64.rpm` |
| Any other | `make && sudo make install` (see [Development](docs/development.md)) |

Flatpak and Snap Steam are not supported.

## Use

Start Steam from a terminal as before:

    steam

For the application menu, run this once as your user:

    steam-scale --install-desktop-entry

It copies Steam's menu entry into your home directory and points it at steam-scale. To undo it, delete
`~/.local/share/applications/steam.desktop`.

## Requirements

- A Wayland compositor with the `xdg-output` protocol, version 2 or later.
- XWayland that runs unscaled. On Hyprland, set `xwayland { force_zero_scaling = true }`. On KDE Plasma,
  select "Apply scaling themselves".

Tested on Hyprland. Other compositors are untested.

## Options

| Variable | Meaning |
|----------|---------|
| `STEAM_SCALE_STEAM` | Steam launcher to run. Default: the next `steam` on `PATH` |
| `STEAM_SCALE_MONITORS` | `X,Y,W,H=SCALE;...` in X11 pixels. Replaces detection |
| `STEAM_SCALE_LOG` | File to append a log to. Unset: no log |

## More

- [How it works, and what to check when a Steam update breaks it](docs/internals.md)
- [Building, packaging and releasing](docs/development.md)

MIT licensed. Not affiliated with Valve.
