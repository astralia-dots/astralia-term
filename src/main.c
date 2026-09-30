#include <fcntl.h>
#include <getopt.h>
#include <langinfo.h>
#include <locale.h>
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "config-build.h"

#define LOG_MODULE "main"
#include "backend/backend.h"
#include "core/loop.h"
#include "core/pty.h"
#include "core/util.h"
#include "input/input.h"
#include "render/font.h"
#include "render/render.h"
#include "term/selection.h"
#include "term/term.h"
#include "term/url.h"

#define APP_ID "astralia-term"
#define DEFAULT_FONT "monospace:size=15"
#define FONT_SIZE_MIN 6.0
#define FONT_SIZE_MAX 72.0
#define FONT_SIZE_STEP 1.0
#define DEFAULT_BG_ALPHA 0.7 /* opacity of the default background */
#define PTY_READ_CHUNK (64 * 1024)
#define PTY_READ_MAX (1024 * 1024) /* per wakeup, so input stays responsive */
#define BLINK_NS 500000000ull
#define BELL_NS 100000000ull         /* visual bell flash duration */
#define SYNC_TIMEOUT_NS 150000000ull /* longest a 2026 update may hold frames */
#define SCROLL_LINES_PER_STEP 3      /* scrollback lines per wheel notch, mouse tracking off */
#define DOUBLE_CLICK_NS 400000000ull

static uint64_t
now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

struct app {
    struct loop *loop;
    struct term term;
    struct pty pty;
    struct fonts fonts;
    char *font_base;       /* fontconfig pattern the size is substituted into */
    double font_base_size; /* size Ctrl+0 returns to */
    double font_size;
    int font_dpi;
    double scale; /* output scale reported by the backend; 1.0 = 96 dpi */
    struct renderer renderer;

    const struct backend_ops *ops;
    struct backend *backend;
    int width, height;
    bool frame_ready;

    char *wbuf; /* bytes waiting for the pty to become writable */
    size_t wlen, wcap;
    bool pty_watched; /* pty master registered with the loop */

    int blink_timer;
    bool blink_armed;
    int sync_timer;
    bool sync_armed;
    int bell_timer; /* one-shot: ends the visual-bell flash */

    int mouse_button_held; /* 0 = none, else 1-3; for 1002 drag reporting */

    int click_count;              /* consecutive same-cell left clicks; 1/2/3 = char/word/line */
    struct grid_point last_click; /* screen coordinates */
    uint64_t last_click_ns;

    bool pointer_seen;
    struct grid_point pointer; /* screen cell under the pointer, for link hover */
    bool link_clicked;         /* swallow the release of a Ctrl+click that opened a link */

    bool quit;
    int exit_code;
};

/* ---- pty I/O ---- */

static void
pty_flush(struct app *app) {
    while (app->wlen > 0) {
        ssize_t n = write(app->pty.master, app->wbuf, app->wlen);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN)
                app->wlen = 0; /* child is gone; drop it */
            break;
        }
        memmove(app->wbuf, app->wbuf + n, app->wlen - (size_t)n);
        app->wlen -= (size_t)n;
    }

    if (!app->pty_watched)
        app->wlen = 0;
    else if (app->wlen > 0)
        loop_event_add(app->loop, app->pty.master, EPOLLOUT);
    else
        loop_event_del(app->loop, app->pty.master, EPOLLOUT);
}

static void
pty_write(void *user, const void *data, size_t len) {
    struct app *app = user;
    if (app->pty.master < 0 || len == 0)
        return;

    if (app->wlen + len > app->wcap) {
        app->wcap = MAX(app->wcap * 2, app->wlen + len);
        app->wbuf = xrealloc(app->wbuf, app->wcap);
    }
    memcpy(app->wbuf + app->wlen, data, len);
    app->wlen += len;
    pty_flush(app);
}

static bool
pty_cb(struct loop *loop, int fd, uint32_t events, void *data) {
    struct app *app = data;

    if (events & EPOLLOUT)
        pty_flush(app);

    if (events & (EPOLLIN | EPOLLHUP)) {
        static uint8_t buf[PTY_READ_CHUNK];
        size_t total = 0;
        while (total < PTY_READ_MAX) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n > 0) {
                term_feed(&app->term, buf, (size_t)n);
                total += (size_t)n;
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && errno == EAGAIN)
                break;
            /* EOF or EIO: the slave side is closed. Stop watching the pty
             * and let SIGCHLD end the loop with the child's exit status. */
            loop_del_no_close(loop, fd);
            app->pty_watched = false;
            break;
        }
    }
    return true;
}

/* ---- term_host ---- */

static void
host_set_title(void *user, const char *title) {
    struct app *app = user;
    app->ops->set_title(app->backend, title);
}

static void
host_set_clipboard(void *user, char target, const char *text, size_t len) {
    struct app *app = user;
    if (app->ops->set_selection == NULL)
        return;
    /* 'p' primary, 'c'/'s' clipboard, the OSC 52 target fallback */
    enum selection_target t = target == 'p' ? SELECTION_PRIMARY : SELECTION_CLIPBOARD;
    app->ops->set_selection(app->backend, t, text, len);
}

/* Visual bell: flash for BELL_NS; a repeated bell restarts the flash. */
static void
host_bell(void *user) {
    struct app *app = user;
    app->renderer.bell_on = true;
    loop_timer_set(app->bell_timer, BELL_NS, 0);
}

static const struct term_host term_host = {
    .write = pty_write,
    .set_title = host_set_title,
    .set_clipboard = host_set_clipboard,
    .bell = host_bell,
};

/* ---- timers ---- */

static void
blink_restart(struct app *app) {
    app->renderer.blink_off = false;
    app->term.cursor_dirty = true;
    if (app->blink_armed)
        loop_timer_set(app->blink_timer, BLINK_NS, BLINK_NS);
}

static bool
blink_timer_cb(struct loop *loop, int fd, uint32_t events, void *data) {
    struct app *app = data;
    loop_timer_ack(fd);
    app->renderer.blink_off = !app->renderer.blink_off;
    app->term.cursor_dirty = true;
    return true;
}

static bool
bell_timer_cb(struct loop *loop, int fd, uint32_t events, void *data) {
    struct app *app = data;
    loop_timer_ack(fd);
    app->renderer.bell_on = false;
    return true;
}

static bool
sync_timer_cb(struct loop *loop, int fd, uint32_t events, void *data) {
    struct app *app = data;
    loop_timer_ack(fd);
    app->sync_armed = false;
    if (app->term.modes.sync_updates) {
        LOG_DBG("synchronized update timed out");
        app->term.modes.sync_updates = false;
    }
    return true;
}

/* Arms or disarms the blink and synchronized-update timers to match state. */
static void
update_timers(struct app *app) {
    const struct term *t = &app->term;
    bool want_blink = t->cursor_blink && t->modes.cursor_visible && app->renderer.focused;
    if (want_blink != app->blink_armed) {
        app->blink_armed = want_blink;
        loop_timer_set(app->blink_timer, want_blink ? BLINK_NS : 0, want_blink ? BLINK_NS : 0);
        if (!want_blink && app->renderer.blink_off) {
            app->renderer.blink_off = false;
            app->term.cursor_dirty = true;
        }
    }

    if (t->modes.sync_updates != app->sync_armed) {
        app->sync_armed = t->modes.sync_updates;
        loop_timer_set(app->sync_timer, app->sync_armed ? SYNC_TIMEOUT_NS : 0, 0);
    }
}

/* ---- signals ---- */

static bool
sigchld_cb(struct loop *loop, int signo, void *data) {
    struct app *app = data;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == app->pty.pid) {
            app->pty.pid = -1;
            app->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            app->quit = true;
        }
    }
    return !app->quit;
}

static bool
sigterm_cb(struct loop *loop, int signo, void *data) {
    struct app *app = data;
    app->quit = true;
    return false;
}

/* ---- backend events ---- */

/* Fits the grid to the window at the current cell size; `force` also
 * refreshes the pty's pixel size when only the cell size changed. */
static void
apply_grid_size(struct app *app, bool force) {
    const struct fonts *f = &app->fonts;
    const struct renderer *r = &app->renderer;
    int cols = MAX(1, (app->width - 2 * r->pad_x) / f->cell_width);
    int rows = MAX(1, (app->height - 2 * r->pad_y) / f->cell_height);

    bool changed = cols != app->term.cols || rows != app->term.rows;
    if (changed) {
        LOG_DBG("resize: %dx%d px -> %dx%d cells", app->width, app->height, cols, rows);
        term_resize(&app->term, cols, rows);
    }
    if ((changed || force) && app->pty.master >= 0)
        pty_resize(&app->pty, cols, rows, cols * f->cell_width, rows * f->cell_height);
    render_invalidate(&app->renderer);
}

static void
on_configure(void *data, int width, int height) {
    struct app *app = data;
    app->width = width;
    app->height = height;
    apply_grid_size(app, false);
}

static void set_font(struct app *app, double size, int dpi);

/* The output scale changed: fonts follow it (dpi = 96 * scale) and the window
 * pixel size is rescaled first, so the grid keeps its cell count instead of
 * being reflowed to the new cell size and back. */
static void
on_scale(void *data, double scale) {
    struct app *app = data;
    double ratio = scale / app->scale;
    app->scale = scale;
    app->width = (int)lround(app->width * ratio);
    app->height = (int)lround(app->height * ratio);
    set_font(app, app->font_size, (int)lround(96 * scale));
}

/* Reloads the fonts at `size` points and the current dpi, keeping the window
 * size; the grid absorbs the new cell size. */
static void
set_font(struct app *app, double size, int dpi) {
    size = CLAMP(size, FONT_SIZE_MIN, FONT_SIZE_MAX);
    char *pattern = fonts_pattern_with_size(app->font_base, size);
    bool ok = fonts_reload(&app->fonts, pattern, dpi);
    free(pattern);
    if (!ok)
        return;

    app->font_size = size;
    app->font_dpi = dpi;
    term_set_cell_size(&app->term, app->fonts.cell_width, app->fonts.cell_height);
    apply_grid_size(app, true);
}

static void
on_frame(void *data) {
    struct app *app = data;
    app->frame_ready = true;
}

/* Window pixel coordinates to a grid cell, clamped inside the visible grid. */
static void
pixel_to_cell(struct app *app, int x, int y, int *col, int *row) {
    const struct fonts *f = &app->fonts;
    const struct renderer *r = &app->renderer;
    *col = CLAMP((x - r->pad_x) / f->cell_width, 0, app->term.cols - 1);
    *row = CLAMP((y - r->pad_y) / f->cell_height, 0, app->term.rows - 1);
}

/* Shift+PgUp/PgDn/Home/End move the scrollback view. */
static bool
scrollback_key(struct app *app, const struct key_event *ev) {
    struct term *t = &app->term;
    if (ev->mods != MOD_SHIFT || t->modes.alt_screen)
        return false;

    int page = MAX(1, t->rows - 1);
    switch (ev->sym) {
    case XKB_KEY_Page_Up:
    case XKB_KEY_KP_Page_Up:
        term_scroll_view(t, page);
        return true;
    case XKB_KEY_Page_Down:
    case XKB_KEY_KP_Page_Down:
        term_scroll_view(t, -page);
        return true;
    case XKB_KEY_Home:
    case XKB_KEY_KP_Home:
        term_scroll_view(t, t->grid->scrollback_used);
        return true;
    case XKB_KEY_End:
    case XKB_KEY_KP_End:
        term_scroll_view_reset(t);
        return true;
    }
    return false;
}

/* Ctrl+Shift+C/V and Shift+Insert: clipboard copy/paste of the selection. */
static bool
clipboard_key(struct app *app, const struct key_event *ev) {
    if (ev->mods == (MOD_CTRL | MOD_SHIFT) && (ev->sym == XKB_KEY_C || ev->sym == XKB_KEY_c)) {
        if (app->term.selection.active && app->ops->set_selection != NULL) {
            size_t len;
            char *text = selection_to_text(&app->term, &len);
            if (text != NULL) {
                app->ops->set_selection(app->backend, SELECTION_CLIPBOARD, text, len);
                free(text);
            }
        }
        return true;
    }
    if (((ev->mods == (MOD_CTRL | MOD_SHIFT)) && (ev->sym == XKB_KEY_V || ev->sym == XKB_KEY_v)) ||
        (ev->mods == MOD_SHIFT && ev->sym == XKB_KEY_Insert)) {
        if (app->ops->request_paste != NULL)
            app->ops->request_paste(app->backend, SELECTION_CLIPBOARD);
        return true;
    }
    return false;
}

/* Ctrl +/-/0 (with or without Shift): grow, shrink, reset the font size. */
static bool
font_key(struct app *app, const struct key_event *ev) {
    if (ev->mods != MOD_CTRL && ev->mods != (MOD_CTRL | MOD_SHIFT))
        return false;

    switch (ev->sym) {
    case XKB_KEY_plus:
    case XKB_KEY_equal:
    case XKB_KEY_KP_Add:
        set_font(app, app->font_size + FONT_SIZE_STEP, app->font_dpi);
        return true;
    case XKB_KEY_minus:
    case XKB_KEY_underscore:
    case XKB_KEY_KP_Subtract:
        set_font(app, app->font_size - FONT_SIZE_STEP, app->font_dpi);
        return true;
    case XKB_KEY_0:
    case XKB_KEY_parenright:
    case XKB_KEY_KP_0:
        set_font(app, app->font_base_size, app->font_dpi);
        return true;
    }
    return false;
}

static void
on_key(void *data, const struct key_event *ev) {
    struct app *app = data;
    if (ev->action != KEY_RELEASE && scrollback_key(app, ev))
        return;
    if (ev->action != KEY_RELEASE && font_key(app, ev))
        return;
    if (ev->action == KEY_PRESS && clipboard_key(app, ev))
        return;

    char seq[INPUT_MAX_SEQ];
    size_t n = input_encode(ev, &app->term, seq);
    if (n > 0) {
        term_scroll_view_reset(&app->term);
        if (ev->action != KEY_RELEASE)
            blink_restart(app);
        pty_write(app, seq, n);
    }
}

static void
on_focus(void *data, bool focused) {
    struct app *app = data;
    app->renderer.focused = focused;
    app->term.cursor_dirty = true;
    if (app->term.modes.focus_events)
        pty_write(app, focused ? "\033[I" : "\033[O", 3);
}

static void
on_close(void *data) {
    struct app *app = data;
    app->quit = true;
}

/* Allow-listed schemes only: the URI comes from whatever wrote to the pty. */
static bool
url_scheme_allowed(const char *uri) {
    static const char *const schemes[] = {"http://", "https://", "mailto:", "ftp://", "file://"};
    for (size_t i = 0; i < ARRAY_LEN(schemes); i++) {
        if (strncasecmp(uri, schemes[i], strlen(schemes[i])) == 0)
            return true;
    }
    return false;
}

/* Fire-and-forget xdg-open; SIGCHLD reaps it (unknown pids are ignored). */
static void
open_url(const char *uri) {
    if (!url_scheme_allowed(uri)) {
        LOG_WARN("not opening link with unsupported scheme");
        return;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    for (int fd = 0; fd <= 2; fd++)
        posix_spawn_file_actions_addopen(&fa, fd, "/dev/null", O_RDWR, 0);
    char *argv[] = {"xdg-open", (char *)uri, NULL};
    pid_t pid;
    int err = posix_spawnp(&pid, "xdg-open", &fa, NULL, argv, environ);
    if (err != 0)
        LOG_WARN("cannot run xdg-open: %s", strerror(err));
    posix_spawn_file_actions_destroy(&fa);
}

/* Re-highlights the link under the pointer, switching to the hand cursor over one. */
static void
update_link_hover(struct app *app) {
    struct term *t = &app->term;
    bool was = t->link_hover.active;
    url_hover(t, app->pointer.row, app->pointer.col);
    if (t->link_hover.active != was && app->ops->set_cursor_shape != NULL)
        app->ops->set_cursor_shape(app->backend, t->link_hover.active ? CURSOR_SHAPE_POINTER
                                                                      : CURSOR_SHAPE_TEXT);
}

static void
on_pointer_motion(void *data, int x, int y, unsigned mods) {
    struct app *app = data;
    struct term *t = &app->term;

    app->pointer_seen = true;
    pixel_to_cell(app, x, y, &app->pointer.col, &app->pointer.row);
    update_link_hover(app);

    if (t->modes.mouse_mode == MOUSE_OFF) {
        if (app->mouse_button_held == 1) {
            int col, row;
            pixel_to_cell(app, x, y, &col, &row);
            selection_update(t, col, row - t->view_offset);
        }
        return;
    }
    if (t->modes.mouse_mode != MOUSE_ANY &&
        !(t->modes.mouse_mode == MOUSE_BUTTON && app->mouse_button_held != 0))
        return;

    int col, row;
    pixel_to_cell(app, x, y, &col, &row);
    char seq[INPUT_MAX_SEQ];
    size_t n = input_mouse_encode(app->mouse_button_held, true, true, col, row, mods,
                                  t->modes.mouse_sgr, seq);
    if (n > 0)
        pty_write(app, seq, n);
}

/* Opens the link at screen cell (col, row), if any. */
static bool
open_link(struct app *app, int col, int row) {
    struct grid_range range;
    char *uri;
    if (!url_at(&app->term, row - app->term.view_offset, col, &range, &uri))
        return false;
    open_url(uri);
    free(uri);
    return true;
}

/* Click/double-click/triple-click selection, and middle-click PRIMARY paste;
 * only active while the application hasn't taken over clicks itself. */
static void
on_pointer_button_selection(struct app *app, int button, bool pressed, int col, int row) {
    struct term *t = &app->term;

    if (button == 1 && pressed) {
        uint64_t ns = now_ns();
        struct grid_point cell = {row, col};
        if (app->click_count > 0 && ns - app->last_click_ns < DOUBLE_CLICK_NS &&
            cell.row == app->last_click.row && cell.col == app->last_click.col)
            app->click_count = app->click_count % 3 + 1;
        else
            app->click_count = 1;
        app->last_click = cell;
        app->last_click_ns = ns;

        enum selection_kind kind = app->click_count == 2   ? SEL_WORD
                                   : app->click_count == 3 ? SEL_LINE
                                                           : SEL_CHAR;
        selection_start(t, col, row - t->view_offset, kind);
    } else if (button == 1 && !pressed) {
        selection_finish(t);
    } else if (button == 2 && pressed && app->ops->request_paste != NULL) {
        app->ops->request_paste(app->backend, SELECTION_PRIMARY);
    }
}

static void
on_pointer_button(void *data, int button, bool pressed, int x, int y, unsigned mods) {
    struct app *app = data;
    struct term *t = &app->term;

    app->mouse_button_held = pressed ? button : 0;

    int col, row;
    pixel_to_cell(app, x, y, &col, &row);

    if (button == 1 && !pressed && app->link_clicked) {
        app->link_clicked = false;
        return;
    }
    /* Ctrl+click opens a link instead of starting a selection or reporting it */
    if (button == 1 && pressed && (mods & MOD_CTRL) && open_link(app, col, row)) {
        app->mouse_button_held = 0;
        app->link_clicked = true;
        return;
    }

    if (t->modes.mouse_mode == MOUSE_OFF) {
        on_pointer_button_selection(app, button, pressed, col, row);
        return;
    }

    char seq[INPUT_MAX_SEQ];
    size_t n = input_mouse_encode(button, pressed, false, col, row, mods, t->modes.mouse_sgr, seq);
    if (n > 0) {
        term_scroll_view_reset(t);
        pty_write(app, seq, n);
    }
}

static void
on_scroll(void *data, int steps_x, int steps_y, int x, int y, unsigned mods) {
    struct app *app = data;
    struct term *t = &app->term;
    int col, row;
    pixel_to_cell(app, x, y, &col, &row);

    if (t->modes.mouse_mode != MOUSE_OFF) {
        char seq[INPUT_MAX_SEQ];
        for (int i = 0; i < abs(steps_y); i++) {
            size_t n = input_wheel_encode(steps_y > 0 ? 1 : 0, col, row, mods,
                                          t->modes.mouse_sgr, seq);
            if (n > 0)
                pty_write(app, seq, n);
        }
        for (int i = 0; i < abs(steps_x); i++) {
            size_t n = input_wheel_encode(steps_x > 0 ? 3 : 2, col, row, mods,
                                          t->modes.mouse_sgr, seq);
            if (n > 0)
                pty_write(app, seq, n);
        }
        return;
    }

    if (steps_y == 0)
        return;

    if (t->modes.alt_screen) {
        if (!t->modes.mouse_alt_scroll)
            return;
        char seq[INPUT_MAX_SEQ];
        for (int i = 0; i < abs(steps_y); i++) {
            size_t n = input_encode_arrow(steps_y < 0, t->modes.app_cursor_keys, seq);
            if (n > 0)
                pty_write(app, seq, n);
        }
        return;
    }

    term_scroll_view(t, -steps_y * SCROLL_LINES_PER_STEP);
}

/* Bracketed (2004) wraps the payload as-is; otherwise strip C0 controls
 * other than tab/newline, the standard mitigation against a paste smuggling
 * an ESC or other control sequence into a shell that isn't expecting one. */
static void
on_paste(void *data, const char *text, size_t len) {
    struct app *app = data;
    struct term *t = &app->term;
    if (len == 0)
        return;

    if (t->modes.bracketed_paste) {
        char *buf = xmalloc(len + 12);
        size_t n = 0;
        memcpy(buf + n, "\033[200~", 6);
        n += 6;
        memcpy(buf + n, text, len);
        n += len;
        memcpy(buf + n, "\033[201~", 6);
        n += 6;
        pty_write(app, buf, n);
        free(buf);
        return;
    }

    char *buf = xmalloc(len);
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 0x20 || c == '\t' || c == '\n')
            buf[n++] = (char)c;
    }
    pty_write(app, buf, n);
    free(buf);
}

static void
on_selection_lost(void *data, enum selection_target target) {
    struct app *app = data;
    if (target == SELECTION_PRIMARY)
        selection_clear(&app->term);
}

static const struct backend_listener listener = {
    .configure = on_configure,
    .frame = on_frame,
    .key = on_key,
    .focus = on_focus,
    .close = on_close,
    .scale = on_scale,
    .pointer_motion = on_pointer_motion,
    .pointer_button = on_pointer_button,
    .scroll = on_scroll,
    .paste = on_paste,
    .selection_lost = on_selection_lost,
};

/* Runs before each poll: draws at most one frame per display refresh. */
static void
render_hook(struct loop *loop, void *data) {
    struct app *app = data;
    update_timers(app);
    if (!app->frame_ready || !render_needed(&app->renderer, &app->term))
        return;

    struct buffer *buf = app->ops->get_buffer(app->backend, app->width, app->height);
    if (buf == NULL)
        return; /* all buffers busy; retried on the next wakeup */

    pixman_region32_t damage;
    pixman_region32_init(&damage);
    if (app->pointer_seen) /* the text under a still pointer may have changed */
        update_link_hover(app);
    render_frame(&app->renderer, &app->term, buf, &damage);
    app->frame_ready = false;
    app->ops->commit(app->backend, buf, &damage);
    pixman_region32_fini(&damage);
}

/* ---- startup ---- */

static const struct backend_ops *
pick_backend(const char *name) {
    if (name != NULL) {
#if HAVE_WAYLAND
        if (strcasecmp(name, "wayland") == 0)
            return &backend_wayland;
#endif
#if HAVE_X11
        if (strcasecmp(name, "x11") == 0)
            return &backend_x11;
#endif
        LOG_ERR("unknown or disabled backend '%s'", name);
        return NULL;
    }

#if HAVE_WAYLAND
    if (getenv("WAYLAND_DISPLAY") != NULL)
        return &backend_wayland;
#endif
#if HAVE_X11
    if (getenv("DISPLAY") != NULL)
        return &backend_x11;
#endif
    LOG_ERR("neither WAYLAND_DISPLAY nor DISPLAY is set");
    return NULL;
}

static void
ensure_utf8_locale(void) {
    setlocale(LC_CTYPE, "");
    if (strcmp(nl_langinfo(CODESET), "UTF-8") == 0)
        return;
    if (setlocale(LC_CTYPE, "C.UTF-8") == NULL)
        LOG_WARN("no UTF-8 locale available; wide characters may render wrong");
}

static void
usage(const char *prog) {
    printf("Usage: %s [OPTIONS] [-e] [COMMAND [ARGS...]]\n"
           "\n"
           "  -f, --font=PATTERN     fontconfig pattern (default: " DEFAULT_FONT ")\n"
           "  -b, --backend=NAME     wayland or x11 (default: auto)\n"
           "  -e                     run COMMAND instead of $SHELL\n"
           "  -d, --debug            verbose logging\n"
           "  -v, --version          print version and exit\n"
           "  -h, --help             print this help and exit\n",
           prog);
}

int main(int argc, char *argv[]) {
    const char *font_name = DEFAULT_FONT;
    const char *backend_name = NULL;

    static const struct option longopts[] = {
        {"font", required_argument, NULL, 'f'},
        {"backend", required_argument, NULL, 'b'},
        {"debug", no_argument, NULL, 'd'},
        {"version", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "+f:b:dvhe", longopts, NULL)) != -1) {
        switch (opt) {
        case 'f':
            font_name = optarg;
            break;
        case 'b':
            backend_name = optarg;
            break;
        case 'd':
            log_set_level(LOG_LEVEL_DBG);
            break;
        case 'v':
            printf("astralia-term %s\n", ASTRALIA_VERSION);
            return 0;
        case 'h':
            usage(argv[0]);
            return 0;
        case 'e':
            goto done_opts; /* everything after -e is the command */
        default:
            usage(argv[0]);
            return 2;
        }
    }
done_opts:;
    char **cmd = optind < argc ? &argv[optind] : NULL;

    ensure_utf8_locale();
    signal(SIGPIPE, SIG_IGN);

    struct app app = {
        .frame_ready = true,
        .exit_code = 1,
        .pty = {.master = -1, .pid = -1},
        .blink_timer = -1,
        .sync_timer = -1,
        .bell_timer = -1,
        .scale = 1.0,
    };

    app.ops = pick_backend(backend_name);
    if (app.ops == NULL)
        return 1;

    if (!fonts_init())
        return 1;
    app.font_base = xstrdup(font_name);
    app.font_base_size = fonts_pattern_size(font_name, 15.0);
    app.font_size = app.font_base_size;
    app.font_dpi = 96;
    if (!fonts_load(&app.fonts, font_name, app.font_dpi)) {
        free(app.font_base);
        fonts_fini();
        return 1;
    }
    render_init(&app.renderer, &app.fonts);
    app.renderer.bg_alpha = (uint16_t)(DEFAULT_BG_ALPHA * 0xffff);

    const int cols = 80, rows = 24;
    app.width = cols * app.fonts.cell_width + 2 * app.renderer.pad_x;
    app.height = rows * app.fonts.cell_height + 2 * app.renderer.pad_y;

    app.loop = loop_new();
    if (app.loop == NULL)
        goto out;

    if (!loop_signal_add(app.loop, SIGCHLD, sigchld_cb, &app) ||
        !loop_signal_add(app.loop, SIGTERM, sigterm_cb, &app) ||
        !loop_signal_add(app.loop, SIGINT, sigterm_cb, &app))
        goto out;

    app.blink_timer = loop_timer_add(app.loop, blink_timer_cb, &app);
    app.sync_timer = loop_timer_add(app.loop, sync_timer_cb, &app);
    app.bell_timer = loop_timer_add(app.loop, bell_timer_cb, &app);
    if (app.blink_timer < 0 || app.sync_timer < 0 || app.bell_timer < 0)
        goto out;

    term_init(&app.term, cols, rows, &term_host, &app);
    term_set_cell_size(&app.term, app.fonts.cell_width, app.fonts.cell_height);

    app.backend = app.ops->create(app.loop, &listener, &app, app.width, app.height,
                                  APP_ID, "astralia-term");
    if (app.backend == NULL)
        goto out_term;

    /* on_scale may already have resized the grid inside create() */
    if (!pty_spawn(&app.pty, cmd, "xterm-256color", app.term.cols, app.term.rows,
                   app.term.cols * app.fonts.cell_width, app.term.rows * app.fonts.cell_height))
        goto out_backend;
    if (!loop_add(app.loop, app.pty.master, EPOLLIN, pty_cb, &app))
        goto out_backend;
    app.pty_watched = true;
    if (!loop_hook_add(app.loop, render_hook, &app, LOOP_HOOK_NORMAL))
        goto out_pty;

    app.exit_code = 0;
    while (!app.quit && loop_poll(app.loop))
        ;

    loop_hook_del(app.loop, render_hook, LOOP_HOOK_NORMAL);
out_pty:
    if (app.pty_watched)
        loop_del_no_close(app.loop, app.pty.master);

out_backend:
    app.ops->destroy(app.backend);
    pty_close(&app.pty);
out_term:
    term_destroy(&app.term);
out:
    if (app.loop != NULL) {
        if (app.blink_timer >= 0)
            loop_del(app.loop, app.blink_timer);
        if (app.sync_timer >= 0)
            loop_del(app.loop, app.sync_timer);
        if (app.bell_timer >= 0)
            loop_del(app.loop, app.bell_timer);
        loop_signal_del(app.loop, SIGCHLD);
        loop_signal_del(app.loop, SIGTERM);
        loop_signal_del(app.loop, SIGINT);
        loop_destroy(app.loop);
    }
    fonts_destroy(&app.fonts);
    fonts_fini();
    free(app.font_base);
    free(app.wbuf);
    return app.exit_code;
}
