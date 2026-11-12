/*
 * Per-monitor fractional scaling for the Steam client under XWayland.
 *
 * Chromium on X11 takes each display's scale from GTK: for every GdkMonitor it reads
 * gdk_monitor_get_geometry and gdk_monitor_get_scale_factor, multiplies by the font scale
 * (gdk_screen_get_resolution / 96) and assigns the result to the XRandR display that overlaps
 * most (ui/gtk/gtk_ui.cc GetDisplayConfig, ui/base/x/x11_display_util.cc GetDisplayScale).
 * Under XWayland GDK reports scale 1 for every monitor. libcef resolves those functions with
 * dlsym on its GTK handle, so this library wraps dlsym and hands libcef replacements that
 * report each monitor's real scale.
 *
 * The real scale is the monitor's size in X11 pixels (XRandR) divided by its logical size on the
 * compositor (xdg-output), matched by output name. When XWayland is not running unscaled the two
 * are equal and this library changes nothing.
 *
 * GDK scales are integers, so a fractional scale s is reported as monitor scale K*s with a font
 * scale of 1/K. K is a power of two up to 64 because Chromium rounds the font scale to 1/64.
 *
 * Steam's windows hold a Chromium window two levels down (SDL window > CEF container > Chromium).
 * Chromium picks that window's display, and so its scale, from the window's position on the root,
 * but as a child window it only learns its position relative to its parent, so it stays on the
 * display at 0,0. A thread here watches this process's top-level windows and tells each Chromium
 * window where it is with a synthetic ConfigureNotify, which Chromium reads as root coordinates
 * (ui/ozone/platform/x11/x11_window.cc OnConfigureEvent).
 *
 * Loaded with LD_PRELOAD into Steam. It only acts in the steamwebhelper browser process, and takes
 * itself out of LD_PRELOAD where Steam launches games.
 *
 *   STEAM_SCALE_MONITORS  "X,Y,W,H=SCALE;..." in X11 pixels. Replaces detection.
 *   STEAM_SCALE_LOG       log file (appended). Unset: no log.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>

#include "xdg-output-unstable-v1-client-protocol.h"

#define SONAME "libsteamscale.so"

enum { MAX_MONS = 16, MAX_TOPS = 128, SETTLE_MS = 200, DETECT_MS = 150 };

typedef struct { int x, y, w, h; } rect_t;
typedef struct { rect_t r; double scale; char name[32]; } mon_t;

/* The monitor table. Read from Chromium's UI thread and SDL, written by the watcher thread. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static mon_t mons[MAX_MONS];
static int n_mons, denom = 1, scaling;

static int active, fixed;
static int log_fd = -1;
static pthread_once_t init_once = PTHREAD_ONCE_INIT, start_once = PTHREAD_ONCE_INIT;
static void *(*real_dlsym)(void *, const char *);
static void *(*real_dlopen)(const char *, int);
static int (*real_dlclose)(void *);

static void *gtk;
static int (*real_scale_factor)(void *);
static void (*real_geometry)(void *, rect_t *);
static double (*real_resolution)(void *);

static void logf_(const char *fmt, ...)
{
    if (log_fd < 0) return;
    char buf[512];
    int n = snprintf(buf, sizeof buf, "%d ", getpid());
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
    buf[n++] = '\n';
    if (write(log_fd, buf, n) < 0) {}
}

/* ---- which process this is */

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* Games must not inherit this library. The string is edited in place so the change holds whichever
 * copy of the environment the process passes on. */
static void leave_preload(void)
{
    char *v = getenv("LD_PRELOAD");
    if (!v) return;
    char *out = v, *in = v;
    while (*in) {
        size_t len = strcspn(in, ": ");
        char sep = in[len], *next = in + len + (sep ? 1 : 0);
        in[len] = 0;
        if (len && strcmp(base_name(in), SONAME)) {
            if (out != v) *out++ = ':';
            memmove(out, in, len);
            out += len;
        }
        in = next;
    }
    *out = 0;
}

static int is_browser_process(void)
{
    char buf[4096];
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    for (char *arg = buf; arg < buf + n; arg += strlen(arg) + 1)
        if (!strncmp(arg, "--type=", 7)) return 0;
    return 1;
}

static void init(void)
{
    real_dlsym = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
    if (!real_dlsym) real_dlsym = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    real_dlopen = dlvsym(RTLD_NEXT, "dlopen", "GLIBC_2.34");
    if (!real_dlopen) real_dlopen = dlvsym(RTLD_NEXT, "dlopen", "GLIBC_2.2.5");
    real_dlclose = dlvsym(RTLD_NEXT, "dlclose", "GLIBC_2.34");
    if (!real_dlclose) real_dlclose = dlvsym(RTLD_NEXT, "dlclose", "GLIBC_2.2.5");
    char exe[4096] = "";
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) exe[n] = 0;
    const char *name = base_name(exe);
    if (!strcmp(name, "steam-launch-wrapper") || !strcmp(name, "reaper")) leave_preload();
    if (strcmp(name, "steamwebhelper") || !is_browser_process()) return;
    active = 1;
    const char *path = getenv("STEAM_SCALE_LOG");
    if (path && *path) log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    logf_("load exe=%s", exe);
}

__attribute__((constructor)) static void on_load(void)
{
    pthread_once(&init_once, init);
}

/* ---- the monitor table */

static long overlap(const rect_t *a, const rect_t *b)
{
    long w = (a->x + a->w < b->x + b->w ? a->x + a->w : b->x + b->w) - (a->x > b->x ? a->x : b->x);
    long h = (a->y + a->h < b->y + b->h ? a->y + a->h : b->y + b->h) - (a->y > b->y ? a->y : b->y);
    return w > 0 && h > 0 ? w * h : 0;
}

/* Index of the monitor a rectangle is mostly on, -1 if none. Call with the lock held. */
static int monitor_at(const rect_t *r)
{
    long best = 0;
    int mon = -1;
    for (int i = 0; i < n_mons; i++) {
        long o = overlap(r, &mons[i].r);
        if (o > best) { best = o; mon = i; }
    }
    return mon;
}

static int parse_monitors(const char *s, mon_t *out)
{
    int n = 0;
    while (s && *s && n < MAX_MONS) {
        mon_t *m = &out[n];
        int *field[4] = {&m->r.x, &m->r.y, &m->r.w, &m->r.h};
        char *end = (char *)s;
        int ok = 1;
        for (int i = 0; i < 4 && ok; i++) {
            *field[i] = (int)strtol(end, &end, 10);
            ok = *end == (i < 3 ? ',' : '=');
            if (ok) end++;
        }
        if (ok) m->scale = strtod(end, &end);
        m->name[0] = 0;
        if (ok && m->r.w > 0 && m->r.h > 0 && m->scale >= 0.5 && m->scale <= 8) n++;
        s = strchr(end, ';');
        if (s) s++;
    }
    return n;
}

/* Returns 1 if the table changed. */
static int set_monitors(const mon_t *next, int n)
{
    int d = 1, any = 0;
    for (; d < 64; d *= 2) {
        int ok = 1;
        for (int i = 0; i < n; i++)
            if (fabs(next[i].scale * d - round(next[i].scale * d)) > 1e-6) ok = 0;
        if (ok) break;
    }
    for (int i = 0; i < n; i++)
        if (fabs(next[i].scale - 1) > 1e-6) any = 1;
    pthread_mutex_lock(&lock);
    int changed = n != n_mons;
    for (int i = 0; i < n && !changed; i++)
        changed = memcmp(&next[i].r, &mons[i].r, sizeof(rect_t)) || next[i].scale != mons[i].scale;
    if (changed) {
        memcpy(mons, next, n * sizeof *next);
        n_mons = n;
        denom = d;
        scaling = any;
    }
    pthread_mutex_unlock(&lock);
    if (changed) {
        logf_("monitors=%d denom=%d", n, d);
        for (int i = 0; i < n; i++)
            logf_("  %s %d,%d %dx%d scale %g", next[i].name, next[i].r.x, next[i].r.y, next[i].r.w, next[i].r.h,
                  next[i].scale);
    }
    return changed;
}

/* ---- detection: X11 rectangles from XRandR, logical sizes from xdg-output */

typedef struct {
    uint32_t id;
    struct wl_output *out;
    struct zxdg_output_v1 *xdg;
    char name[32];
    int w, h;
} wout_t;

static xcb_connection_t *xc;
static xcb_window_t xroot;
static int randr_event = -1;
static struct wl_display *wd;
static struct zxdg_output_manager_v1 *wmgr;
static wout_t wouts[MAX_MONS];
static int wl_dirty;

static void xo_position(void *data, struct zxdg_output_v1 *o, int32_t x, int32_t y)
{
    (void)data; (void)o; (void)x; (void)y;
}

static void xo_size(void *data, struct zxdg_output_v1 *o, int32_t w, int32_t h)
{
    (void)o;
    wout_t *wo = data;
    wo->w = w;
    wo->h = h;
    wl_dirty = 1;
}

static void xo_done(void *data, struct zxdg_output_v1 *o)
{
    (void)data; (void)o;
}

static void xo_name(void *data, struct zxdg_output_v1 *o, const char *name)
{
    (void)o;
    wout_t *wo = data;
    snprintf(wo->name, sizeof wo->name, "%s", name);
    wl_dirty = 1;
}

static void xo_description(void *data, struct zxdg_output_v1 *o, const char *text)
{
    (void)data; (void)o; (void)text;
}

static const struct zxdg_output_v1_listener xo_listener = {xo_position, xo_size, xo_done, xo_name, xo_description};

static void wl_track(void)
{
    for (int i = 0; wmgr && i < MAX_MONS; i++) {
        wout_t *wo = &wouts[i];
        if (!wo->out || wo->xdg) continue;
        wo->xdg = zxdg_output_manager_v1_get_xdg_output(wmgr, wo->out);
        zxdg_output_v1_add_listener(wo->xdg, &xo_listener, wo);
    }
}

static void reg_global(void *data, struct wl_registry *reg, uint32_t id, const char *iface, uint32_t version)
{
    (void)data;
    if (!strcmp(iface, wl_output_interface.name)) {
        for (int i = 0; i < MAX_MONS; i++) {
            if (wouts[i].out) continue;
            wouts[i] = (wout_t){.id = id, .out = wl_registry_bind(reg, id, &wl_output_interface, 1)};
            break;
        }
    } else if (!strcmp(iface, zxdg_output_manager_v1_interface.name) && version >= 2 && !wmgr) {
        wmgr = wl_registry_bind(reg, id, &zxdg_output_manager_v1_interface, 2);
    }
    wl_track();
}

static void reg_remove(void *data, struct wl_registry *reg, uint32_t id)
{
    (void)data; (void)reg;
    for (int i = 0; i < MAX_MONS; i++) {
        wout_t *wo = &wouts[i];
        if (!wo->out || wo->id != id) continue;
        if (wo->xdg) zxdg_output_v1_destroy(wo->xdg);
        wl_output_destroy(wo->out);
        *wo = (wout_t){0};
        wl_dirty = 1;
    }
}

static const struct wl_registry_listener reg_listener = {reg_global, reg_remove};

static void wl_open(void)
{
    wd = wl_display_connect(NULL);
    if (!wd) { logf_("wayland connect failed"); return; }
    wl_registry_add_listener(wl_display_get_registry(wd), &reg_listener, NULL);
    if (wl_display_roundtrip(wd) < 0 || wl_display_roundtrip(wd) < 0 || !wmgr) {
        logf_("wayland has no xdg-output");
        wl_display_disconnect(wd);
        wd = NULL;
    }
}

static int x_open(void)
{
    xc = xcb_connect(NULL, NULL);
    if (!xc || xcb_connection_has_error(xc)) { logf_("x11 connect failed"); return 0; }
    xroot = xcb_setup_roots_iterator(xcb_get_setup(xc)).data->root;
    return 1;
}

static void randr_open(void)
{
    const xcb_query_extension_reply_t *ext = xcb_get_extension_data(xc, &xcb_randr_id);
    if (!ext || !ext->present) return;
    free(xcb_randr_query_version_reply(xc, xcb_randr_query_version(xc, 1, 5), NULL));
    xcb_randr_select_input(xc, xroot, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE | XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE
                                          | XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE);
    randr_event = ext->first_event;
}

/* Returns 1 if the table changed. */
static int detect(void)
{
    mon_t next[MAX_MONS];
    int n = 0;
    xcb_randr_get_monitors_reply_t *rep = xcb_randr_get_monitors_reply(xc, xcb_randr_get_monitors(xc, xroot, 1), NULL);
    if (!rep) return 0;
    for (xcb_randr_monitor_info_iterator_t it = xcb_randr_get_monitors_monitors_iterator(rep); it.rem && n < MAX_MONS;
         xcb_randr_monitor_info_next(&it)) {
        mon_t *m = &next[n];
        *m = (mon_t){.r = {it.data->x, it.data->y, it.data->width, it.data->height}, .scale = 1};
        xcb_get_atom_name_reply_t *an = xcb_get_atom_name_reply(xc, xcb_get_atom_name(xc, it.data->name), NULL);
        if (an) snprintf(m->name, sizeof m->name, "%.*s", xcb_get_atom_name_name_length(an), xcb_get_atom_name_name(an));
        free(an);
        if (m->r.w <= 0 || m->r.h <= 0) continue;
        const wout_t *wo = NULL;
        for (int i = 0; i < MAX_MONS; i++)
            if (wouts[i].out && wouts[i].w > 0 && !strcmp(wouts[i].name, m->name)) wo = &wouts[i];
        if (wo) {
            /* compositors set fractional scales in steps of 1/120 */
            double s = round((double)m->r.w / wo->w * 120) / 120;
            if (s >= 0.5 && s <= 8) m->scale = s;
        } else {
            logf_("no wayland output named %s", m->name);
        }
        n++;
    }
    free(rep);
    return set_monitors(next, n);
}

/* ---- what libcef gets from GTK */

/* The monitor's rectangle in X11 pixels and the integer scale to report for it; 0 to leave GDK's answer alone. */
static int monitor_scale(void *monitor, rect_t *px)
{
    rect_t g = {0, 0, 0, 0};
    if (real_geometry) real_geometry(monitor, &g);
    int rs = real_scale_factor ? real_scale_factor(monitor) : 1;
    if (rs < 1) rs = 1;
    *px = (rect_t){g.x * rs, g.y * rs, g.w * rs, g.h * rs};
    pthread_mutex_lock(&lock);
    int mon = monitor_at(px);
    int ms = scaling ? (int)lround((mon < 0 ? rs : mons[mon].scale) * denom) : 0;
    pthread_mutex_unlock(&lock);
    return scaling && ms < 1 ? 1 : ms;
}

static void start(void);

static int shim_scale_factor(void *monitor)
{
    pthread_once(&start_once, start);
    rect_t px;
    int ms = monitor_scale(monitor, &px);
    return ms ? ms : real_scale_factor(monitor);
}

static void shim_geometry(void *monitor, rect_t *out)
{
    pthread_once(&start_once, start);
    rect_t px;
    int ms = monitor_scale(monitor, &px);
    if (!ms) { real_geometry(monitor, out); return; }
    *out = (rect_t){(int)lround((double)px.x / ms), (int)lround((double)px.y / ms),
                    (int)lround((double)px.w / ms), (int)lround((double)px.h / ms)};
}

static double shim_resolution(void *screen)
{
    pthread_once(&start_once, start);
    pthread_mutex_lock(&lock);
    double res = scaling ? 96.0 / denom : 0;
    pthread_mutex_unlock(&lock);
    return res ? res : real_resolution(screen);
}

/* Chromium reads the monitors again when GDK says one changed (ui/gtk/gtk_ui.cc TrackMonitor). */
static int gtk_reread(void *data)
{
    (void)data;
    void *(*get_display)(void) = real_dlsym(gtk, "gdk_display_get_default");
    int (*count)(void *) = real_dlsym(gtk, "gdk_display_get_n_monitors");
    void *(*get_monitor)(void *, int) = real_dlsym(gtk, "gdk_display_get_monitor");
    void (*notify)(void *, const char *) = real_dlsym(gtk, "g_object_notify");
    void *display = get_display && count && get_monitor && notify ? get_display() : NULL;
    for (int i = 0; display && i < count(display); i++) notify(get_monitor(display, i), "scale-factor");
    return 0;
}

static void gtk_poke(void)
{
    unsigned (*idle_add)(int (*)(void *), void *) = gtk ? real_dlsym(gtk, "g_idle_add") : NULL;
    if (idle_add) idle_add(gtk_reread, NULL);
}

/* dlsym(RTLD_NEXT) depends on who calls it; from here it would search after this library. */
static void *next_from(void *caller, const char *name)
{
    Dl_info di;
    struct link_map *lm = NULL;
    if (!dladdr1(caller, &di, (void **)&lm, RTLD_DL_LINKMAP) || !lm) return NULL;
    for (lm = lm->l_next; lm; lm = lm->l_next) {
        if (!lm->l_name || !*lm->l_name) continue;
        void *h = real_dlopen(lm->l_name, RTLD_NOLOAD | RTLD_LAZY);
        if (!h) continue;
        void *sym = real_dlsym(h, name);
        real_dlclose(h);
        if (sym) return sym;
    }
    return NULL;
}

void *dlsym(void *handle, const char *name)
{
    pthread_once(&init_once, init);
    if (handle == RTLD_NEXT) return next_from(__builtin_return_address(0), name);
    void *sym = real_dlsym(handle, name);
    if (!active || !sym || handle == RTLD_DEFAULT) return sym;
    if (!strcmp(name, "gdk_monitor_get_scale_factor")) {
        gtk = handle;
        real_scale_factor = sym;
        return shim_scale_factor;
    }
    if (!strcmp(name, "gdk_monitor_get_geometry")) {
        real_geometry = sym;
        return shim_geometry;
    }
    if (!strcmp(name, "gdk_screen_get_resolution")) {
        real_resolution = sym;
        return shim_resolution;
    }
    return sym;
}

/* The webhelper asks SDL for a display's content scale, which under XWayland is one global value. */
float SDL_GetDisplayContentScale(unsigned display)
{
    static float (*real)(unsigned);
    static int (*bounds)(unsigned, rect_t *);
    pthread_once(&init_once, init);
    if (!real) real = real_dlsym(RTLD_NEXT, "SDL_GetDisplayContentScale");
    float scale = real(display);
    if (!active) return scale;
    pthread_once(&start_once, start);
    if (!bounds) bounds = real_dlsym(RTLD_NEXT, "SDL_GetDisplayBounds");
    rect_t b = {0, 0, 0, 0};
    if (!bounds || !bounds(display, &b)) return scale;
    pthread_mutex_lock(&lock);
    int mon = monitor_at(&b);
    if (scaling && mon >= 0) scale = (float)mons[mon].scale;
    pthread_mutex_unlock(&lock);
    return scale;
}

/* ---- X11 watcher. Two jobs, both done from a thread with its own connection:
 *
 * 1. Keep each Chromium window informed of its root position (see the top of this file).
 * 2. Keep each window's size right for its monitor. A window keeps its size in pixels unless someone
 *    resizes it. When the compositor moves a window to a monitor with another scale it resizes it; when
 *    a window is mapped on a monitor other than the one Steam sized it for, nobody does. So remember each
 *    window's size in scale-independent units and, once a monitor change has settled, resize the window
 *    if it does not have that size at the new scale.
 *
 * The same thread keeps the monitor table current. */

typedef struct {
    xcb_window_t win;
    int mine;       /* -1 until _NET_WM_PID can be read */
    int managed;    /* not override-redirect */
    rect_t r;       /* on the root, pixels */
    int mon;        /* index into mons, -1 unknown */
    double w, h;    /* size divided by the monitor's scale */
    long long due;  /* when to check the size after a monitor change, 0 = not waiting */
} top_t;

static xcb_atom_t net_wm_pid;
static top_t tops[MAX_TOPS];

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void x_select(xcb_window_t w)
{
    uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
    xcb_change_window_attributes(xc, w, XCB_CW_EVENT_MASK, &mask);
}

static int x_children(xcb_window_t w, xcb_window_t *out, int max)
{
    xcb_query_tree_reply_t *t = xcb_query_tree_reply(xc, xcb_query_tree(xc, w), NULL);
    if (!t) return 0;
    int n = xcb_query_tree_children_length(t);
    if (n > max) n = max;
    memcpy(out, xcb_query_tree_children(t), n * sizeof *out);
    free(t);
    return n;
}

static int x_is_mine(top_t *t)
{
    if (t->mine < 0) {
        xcb_get_property_reply_t *p =
            xcb_get_property_reply(xc, xcb_get_property(xc, 0, t->win, net_wm_pid, XCB_ATOM_CARDINAL, 0, 1), NULL);
        if (p && xcb_get_property_value_length(p) == 4)
            t->mine = *(uint32_t *)xcb_get_property_value(p) == (uint32_t)getpid();
        free(p);
    }
    return t->mine > 0;
}

/* Select on the top-level and its containers, then send every Chromium window its root rectangle. */
static void x_sync(xcb_window_t top)
{
    xcb_window_t boxes[16], inner[16];
    x_select(top);
    int nb = x_children(top, boxes, 16);
    for (int b = 0; b < nb; b++) {
        x_select(boxes[b]);
        int ni = x_children(boxes[b], inner, 16);
        for (int i = 0; i < ni; i++) {
            xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(xc, xcb_get_geometry(xc, inner[i]), NULL);
            xcb_translate_coordinates_reply_t *t =
                xcb_translate_coordinates_reply(xc, xcb_translate_coordinates(xc, inner[i], xroot, 0, 0), NULL);
            if (g && t) {
                union { xcb_configure_notify_event_t c; char pad[32]; } ev;
                memset(&ev, 0, sizeof ev);
                ev.c.response_type = XCB_CONFIGURE_NOTIFY;
                ev.c.event = ev.c.window = inner[i];
                ev.c.x = t->dst_x; ev.c.y = t->dst_y;
                ev.c.width = g->width; ev.c.height = g->height;
                xcb_send_event(xc, 0, inner[i], XCB_EVENT_MASK_STRUCTURE_NOTIFY, ev.pad);
            }
            free(g);
            free(t);
        }
    }
}

static top_t *top_find(xcb_window_t w, int add)
{
    top_t *spare = NULL;
    for (int i = 0; i < MAX_TOPS; i++) {
        if (tops[i].win == w) return &tops[i];
        if (!tops[i].win && !spare) spare = &tops[i];
    }
    if (!add || !spare) return NULL;
    *spare = (top_t){.win = w, .mine = -1, .mon = -1};
    return spare;
}

/* A top-level window's rectangle as the server reported it, in event order. */
static void top_seen(xcb_window_t w, rect_t r, int override_redirect)
{
    top_t *t = top_find(w, 1);
    if (!t) return;
    pthread_mutex_lock(&lock);
    int mon = monitor_at(&r);
    double scale = mon < 0 ? 1 : mons[mon].scale;
    pthread_mutex_unlock(&lock);
    t->r = r;
    t->managed = !override_redirect;
    if (mon < 0) return;
    if (t->mon >= 0 && mon != t->mon) {
        t->due = now_ms() + SETTLE_MS;
    } else if (!t->due) {
        t->w = r.w / scale;
        t->h = r.h / scale;
    }
    t->mon = mon;
}

static void top_settle(top_t *t)
{
    t->due = 0;
    if (t->mon < 0 || t->w <= 0 || !t->managed || !x_is_mine(t)) return;
    pthread_mutex_lock(&lock);
    double scale = t->mon < n_mons ? mons[t->mon].scale : 1;
    pthread_mutex_unlock(&lock);
    uint32_t want[2] = {(uint32_t)lround(t->w * scale), (uint32_t)lround(t->h * scale)};
    if (labs((long)want[0] - t->r.w) <= 1 && labs((long)want[1] - t->r.h) <= 1) return;
    logf_("x11 top 0x%x is %dx%d, scale %g wants %ux%u", t->win, t->r.w, t->r.h, scale, want[0], want[1]);
    xcb_configure_window(xc, t->win, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, want);
}

/* The monitors changed under the windows, which keep their size in pixels: check each against its new scale. */
static void tops_rebase(void)
{
    long long due = now_ms() + SETTLE_MS;
    for (int i = 0; i < MAX_TOPS; i++) {
        top_t *t = &tops[i];
        if (!t->win) continue;
        pthread_mutex_lock(&lock);
        t->mon = monitor_at(&t->r);
        pthread_mutex_unlock(&lock);
        t->due = due;
    }
}

static void *watch_main(void *arg)
{
    (void)arg;
    xcb_window_t kids[256];
    long long detect_due = 0;
    x_select(xroot);
    int n = x_children(xroot, kids, 256);
    for (int i = 0; i < n; i++) {
        xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(xc, xcb_get_geometry(xc, kids[i]), NULL);
        if (g) top_seen(kids[i], (rect_t){g->x, g->y, g->width, g->height}, 0);
        free(g);
    }
    for (int dirty = 1;;) {
        long long now = now_ms(), wait = -1;
        if (detect_due && detect_due <= now) {
            detect_due = 0;
            if (detect()) {
                tops_rebase();
                gtk_poke();
                dirty = 1;
            }
        }
        if (detect_due) wait = detect_due - now;
        for (int i = 0; i < MAX_TOPS; i++) {
            top_t *t = &tops[i];
            if (!t->win) continue;
            if (t->due && t->due <= now) top_settle(t);
            if (t->due && (wait < 0 || t->due - now < wait)) wait = t->due - now;
            if (dirty && x_is_mine(t)) x_sync(t->win);
        }
        dirty = 0;
        xcb_flush(xc);
        xcb_generic_event_t *e = xcb_poll_for_event(xc);
        if (!e) {
            struct pollfd pfd[2] = {{xcb_get_file_descriptor(xc), POLLIN, 0}, {-1, POLLIN, 0}};
            if (wd) {
                while (wl_display_prepare_read(wd) != 0) wl_display_dispatch_pending(wd);
                wl_display_flush(wd);
                pfd[1].fd = wl_display_get_fd(wd);
            }
            poll(pfd, 2, (int)wait);
            if (wd) {
                int lost = 0;
                if (pfd[1].revents & POLLIN) lost = wl_display_read_events(wd) < 0;
                else wl_display_cancel_read(wd);
                if (lost || (pfd[1].revents & (POLLERR | POLLHUP)) || wl_display_dispatch_pending(wd) < 0) {
                    logf_("wayland connection lost");
                    wd = NULL;
                }
            }
            if (xcb_connection_has_error(xc)) return NULL;
            e = xcb_poll_for_event(xc);
        }
        if (wl_dirty) {
            wl_dirty = 0;
            detect_due = now_ms() + DETECT_MS;
        }
        /* take everything that is queued, then update once */
        for (; e; e = xcb_poll_for_event(xc)) {
            int type = e->response_type & 0x7f;
            if (randr_event >= 0 && (type == randr_event + XCB_RANDR_SCREEN_CHANGE_NOTIFY
                                     || type == randr_event + XCB_RANDR_NOTIFY)) {
                if (!fixed) detect_due = now_ms() + DETECT_MS;
                free(e);
                continue;
            }
            switch (type) {
            case XCB_CREATE_NOTIFY: {
                xcb_create_notify_event_t *c = (void *)e;
                if (c->parent == xroot) top_seen(c->window, (rect_t){c->x, c->y, c->width, c->height}, c->override_redirect);
                dirty = 1;
                break;
            }
            case XCB_CONFIGURE_NOTIFY: {
                xcb_configure_notify_event_t *c = (void *)e;
                if (c->event == xroot) top_seen(c->window, (rect_t){c->x, c->y, c->width, c->height}, c->override_redirect);
                dirty = 1;
                break;
            }
            case XCB_DESTROY_NOTIFY: {
                xcb_destroy_notify_event_t *d = (void *)e;
                top_t *t = d->event == xroot ? top_find(d->window, 0) : NULL;
                if (t) t->win = 0;
                break;
            }
            case XCB_MAP_NOTIFY:
            case XCB_REPARENT_NOTIFY:
                dirty = 1;
                break;
            }
            free(e);
        }
    }
}

/* Runs once, on the first question about a monitor, so the first answer is already right. */
static void start(void)
{
    mon_t given[MAX_MONS];
    int n = parse_monitors(getenv("STEAM_SCALE_MONITORS"), given);
    if (!x_open()) return;
    if (n) {
        fixed = 1;
        set_monitors(given, n);
    } else {
        wl_open();
        if (wd) {
            randr_open();
            wl_dirty = 0;
            detect();
        }
    }
    xcb_intern_atom_reply_t *a = xcb_intern_atom_reply(xc, xcb_intern_atom(xc, 0, 11, "_NET_WM_PID"), NULL);
    if (!a) return;
    net_wm_pid = a->atom;
    free(a);
    pthread_t t;
    if (!pthread_create(&t, NULL, watch_main, NULL)) pthread_detach(t);
    logf_("x11 watching root 0x%x", xroot);
}
