# How steam-scale works

Steam's interface is a Chromium browser (CEF, in the `steamwebhelper` process) that runs as an X11 client
under XWayland. steam-scale is a library, `libsteamscale.so`, that the `steam-scale` script preloads into
Steam with `LD_PRELOAD`. The library changes three answers that Chromium receives and adds one thread.
It patches no Steam file, so a Steam update never removes it. An update can only change the code that it
relies on.

## Where the scales come from

The library reads each monitor twice. XRandR gives the monitor's size in X11 pixels. The Wayland `xdg-output`
protocol gives the monitor's logical size on the compositor. The scale is the first width divided by the
second width, rounded to the nearest 1/120. The library matches the two lists by output name, such as `DP-1`.

When XWayland is scaled by the compositor, the two sizes are equal and every scale is 1. The library then
passes all answers through unchanged.

The library reads the monitors again when XRandR or the compositor reports a change, after a 150 ms pause
that lets a burst of events finish.

## How Chromium learns the scale

Chromium on X11 asks GTK for the scale of each monitor. It calls `gdk_monitor_get_geometry` and
`gdk_monitor_get_scale_factor`, then multiplies the result by the font scale, which is
`gdk_screen_get_resolution` divided by 96. Under XWayland, GTK answers 1 for every monitor.

CEF finds those three functions with `dlsym` on its GTK handle. The library wraps `dlsym` and returns its own
versions of the three functions.

GTK monitor scales are integers, so the library cannot return 1.5. For a scale `s`, it returns the integer
`K * s` as the monitor scale and `96 / K` as the resolution. Chromium multiplies the two and gets `s`. `K` is
one value for all monitors. It is the smallest power of two that makes every `K * s` an integer, up to 64,
because Chromium rounds the font scale to 1/64. With one monitor at 1.5 and one at 1, `K` is 2. The monitor
scales become 3 and 2, and the resolution becomes 48.

After a scale change, the library calls `g_object_notify(monitor, "scale-factor")` on GTK's main loop for each
monitor. Chromium listens for that signal and reads the scales again.

The webhelper also asks SDL for a display's scale through `SDL_GetDisplayContentScale`. The library defines
that function and returns the same per-monitor scale.

## How a window learns which monitor it is on

A Steam window is three nested X11 windows: the SDL window, a CEF container, and the Chromium window.
Chromium chooses the scale from the position of its own window on the root. A child window only receives its
position inside its parent, which is always 0,0, so Chromium believes that every window is on the monitor at
the origin.

A thread in the library watches the top-level windows of the process over its own X11 connection. When a
top-level window moves, the thread sends the Chromium window a synthetic `ConfigureNotify` event that carries
the root position. Chromium reads the coordinates of a synthetic event as root coordinates and picks the
right monitor.

When a window lands on a monitor with a different scale, the thread waits 200 ms for the move to finish. It
then resizes the window so that the window keeps its logical size. The thread does the same for every window
after a scale change, because the compositor does not resize X11 windows when a scale changes.

## Which processes it acts in

`LD_PRELOAD` loads the library into every process that Steam starts. The library decides what to do from the
process name:

- In `steamwebhelper` without a `--type=` argument, which is the browser process, it does everything above.
- In `steam-launch-wrapper` and `reaper`, which start games, it removes itself from `LD_PRELOAD`. Games and
  their children never load it.
- In every other process, it does nothing.

## What a Steam update can break

Each row is one thing that the library assumes about Steam. The versions in use during development were
Chromium 126 in CEF, and SDL 3.

| Assumption | If an update changes it |
|------------|-------------------------|
| CEF finds `gdk_monitor_get_scale_factor`, `gdk_monitor_get_geometry`, and `gdk_screen_get_resolution` with `dlsym` | All windows return to scale 1. A move to GTK 4 or to direct linking has this effect |
| Chromium computes the scale as monitor scale times resolution / 96, and rounds the font scale to 1/64 | Windows draw at a wrong scale, such as 3 in place of 1.5 |
| Chromium reads the scales again on `notify::scale-factor` | A live scale change has no effect until Steam restarts |
| A Steam window is SDL window, then CEF container, then Chromium window | Every window uses the scale of the monitor at 0,0 |
| Chromium reads a synthetic `ConfigureNotify` as root coordinates | Every window uses the scale of the monitor at 0,0 |
| The browser process is named `steamwebhelper` and has no `--type=` argument | The library stays inactive and all windows return to scale 1 |
| Games start through `steam-launch-wrapper` or `reaper` | Games load the library. It does nothing there, but it stays in their `LD_PRELOAD` |
| SDL exports `SDL_GetDisplayContentScale(display)` and `SDL_GetDisplayBounds` | Window sizes that Steam computes from the SDL scale are wrong |
| The Steam runtime container has `libxcb`, `libxcb-randr`, and `libwayland-client`, and lets `LD_PRELOAD` paths through | `ld.so` prints an error for the 64-bit processes and Steam runs unscaled |
| Steam's interface runs under XWayland | If Steam moves to native Wayland, the compositor scales it and steam-scale is no longer needed. That case is untested |

The compositor side has two assumptions of its own. XRandR output names must equal Wayland output names,
which holds for current XWayland. The compositor must also offer `xdg-output` version 2.

## Find what changed

1. Start Steam with a log:

        STEAM_SCALE_LOG=/tmp/steam-scale.log steam

2. Read the log and compare it with this list.
   - No `load exe=...steamwebhelper` line: the library did not load, or the process name changed.
   - A `monitors=` block with the wrong scales, or `no wayland output named ...`: detection failed. Set
     `STEAM_SCALE_MONITORS` to confirm that the rest still works.
   - No `x11 watching root` line: Chromium never asked GTK or SDL for a monitor, so the `dlsym` path changed.
   - `x11 top ... scale ... wants ...` lines with the right scale, but the window looks wrong: Chromium no
     longer accepts the synthetic `ConfigureNotify`, or the window nesting changed.

3. To read the scale that a Steam page uses, create the file
   `~/.local/share/Steam/.cef-enable-remote-debugging`, restart Steam, open `http://localhost:8080` in a
   browser, select a page, and evaluate `devicePixelRatio` in the console. It equals the monitor's scale when
   steam-scale works.
