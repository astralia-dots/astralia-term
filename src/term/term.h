#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "term/composed.h"
#include "term/graphics.h"
#include "term/grid.h"
#include "term/hyperlink.h"
#include "term/selection.h"
#include "vt/vt_parser.h"

#define TERM_SCROLLBACK_DEFAULT 1000

struct pen {
    uint32_t fg, bg;
    uint32_t ul;
    uint16_t attrs;
};

struct cursor {
    int row, col;
    bool wrap_pending; /* last column written with autowrap on */
};

struct saved_cursor {
    struct cursor cursor;
    struct pen pen;
    bool origin_mode;
    bool autowrap;
    uint8_t charsets[4];
    int gl;
};

enum cursor_style {
    CURSOR_BLOCK,
    CURSOR_UNDERLINE,
    CURSOR_BAR,
};

enum mouse_mode {
    MOUSE_OFF,
    MOUSE_NORMAL, /* 1000: press/release only */
    MOUSE_BUTTON, /* 1002: + motion while a button is held */
    MOUSE_ANY,    /* 1003: + motion with no button held */
};

/* How the terminal reaches the program hosting it. Any member may be NULL. */
struct term_host {
    /* Bytes the terminal sends back to the application (replies) */
    void (*write)(void *user, const void *data, size_t len);
    void (*set_title)(void *user, const char *title);
    /* OSC 52; target is 'c' (clipboard), 'p' (primary) or 's' (selection) */
    void (*set_clipboard)(void *user, char target, const char *text, size_t len);
    void (*bell)(void *user);
};

struct term {
    int cols, rows;

    struct grid normal, alt;
    struct grid *grid; /* &normal or &alt */

    struct cursor cursor;
    struct pen pen;
    struct saved_cursor saved[2]; /* [0] normal screen, [1] alt screen */

    int scroll_top, scroll_bottom; /* inclusive */

    struct {
        bool origin;          /* DECOM */
        bool autowrap;        /* DECAWM */
        bool insert;          /* IRM */
        bool newline;         /* LNM */
        bool cursor_visible;  /* DECTCEM */
        bool app_cursor_keys; /* DECCKM */
        bool app_keypad;      /* DECKPAM */
        bool reverse_video;   /* DECSCNM */
        bool alt_screen;
        bool focus_events;    /* 1004 */
        bool bracketed_paste; /* 2004 */
        bool sync_updates;    /* 2026 */

        enum mouse_mode mouse_mode; /* 1000/1002/1003 */
        bool mouse_sgr;             /* 1006 */
        bool mouse_alt_scroll;      /* 1007 */
    } modes;

    uint8_t charsets[4]; /* G0..G3: 'B' ASCII or '0' DEC special graphics */
    int gl;              /* invoked into GL: 0 (SI) or 1 (SO) */

    bool *tabs;
    uint32_t last_cp; /* for REP */

    enum cursor_style cursor_style;
    bool cursor_blink;

    uint32_t palette[256];
    uint32_t default_fg, default_bg;
    uint32_t cursor_color; /* valid when cursor_color_set */
    bool cursor_color_set;
    uint32_t initial_palette[256]; /* for OSC 104/110/111 resets */
    uint32_t initial_fg, initial_bg;

    struct composed_table composed;

    struct hyperlink_table hyperlinks;
    uint16_t link; /* handle of the open OSC 8 link, 0 = none; not part of the pen */

    int view_offset; /* lines scrolled back into history; 0 = live */

    int cell_width, cell_height; /* pixels, for XTWINOPS replies */

    bool cursor_dirty;   /* cursor moved or changed; consumer clears */
    bool view_changed;   /* view_offset changed; consumer clears */
    bool colors_changed; /* palette or default colors changed; consumer clears */

    struct selection selection;
    bool selection_changed; /* selection.coords changed; consumer clears */

    struct graphics_store graphics;

    /* Link under the pointer, highlighted; live-row-relative like selection */
    struct {
        bool active;
        struct grid_range range;
    } link_hover;

    struct vt_parser parser;

    const struct term_host *host;
    void *host_user;
};

void term_init(struct term *t, int cols, int rows, const struct term_host *host, void *user);
void term_destroy(struct term *t);
void term_feed(struct term *t, const uint8_t *data, size_t len);
void term_resize(struct term *t, int cols, int rows);
void term_reset(struct term *t);
void term_set_cell_size(struct term *t, int width, int height);

/* Moves the view by delta lines (positive = back into history). */
void term_scroll_view(struct term *t, int delta);
void term_scroll_view_reset(struct term *t);

/* Row r of what is on screen, taking the scrollback view into account. */
struct row *term_view_row(struct term *t, int r);

/* ---- internals shared with vt_csi.c ---- */

void term_reply(struct term *t, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

struct cell term_blank(const struct term *t);          /* erase cell: current bg (BCE) */
void term_cursor_to(struct term *t, int row, int col); /* honors DECOM */
void term_cursor_left(struct term *t, int n);
void term_cursor_right(struct term *t, int n);
void term_cursor_up(struct term *t, int n);
void term_cursor_down(struct term *t, int n);

void term_erase_display(struct term *t, int mode);          /* ED 0/1/2/3 */
void term_erase_line(struct term *t, int mode);             /* EL 0/1/2 */
void term_erase_chars(struct term *t, int n);               /* ECH */
void term_insert_chars(struct term *t, int n);              /* ICH */
void term_delete_chars(struct term *t, int n);              /* DCH */
void term_insert_lines(struct term *t, int n);              /* IL */
void term_delete_lines(struct term *t, int n);              /* DL */
void term_scroll_up(struct term *t, int n);                 /* SU, within margins */
void term_scroll_down(struct term *t, int n);               /* SD, within margins */
void term_set_margins(struct term *t, int top, int bottom); /* DECSTBM, 0-based */
void term_tab_forward(struct term *t, int n);
void term_tab_backward(struct term *t, int n);
void term_tab_clear(struct term *t, int mode);
void term_print(struct term *t, uint32_t cp);
void term_index(struct term *t); /* LF/IND: cursor down, honoring scroll margins */

void term_save_cursor(struct term *t);
void term_restore_cursor(struct term *t);
void term_set_alt_screen(struct term *t, bool enable, bool save_cursor, bool clear);

void term_csi(struct term *t, const struct vt_csi *csi);                  /* vt_csi.c */
void term_osc(struct term *t, const uint8_t *data, size_t len, bool bel); /* vt_osc.c */
void term_apc(struct term *t, const uint8_t *data, size_t len);           /* graphics.c */
void term_dcs(struct term *t, uint8_t private_marker, const uint32_t *params, int nparams,
              uint8_t final, const uint8_t *data, size_t len);     /* sixel, via graphics.c */
void term_reply_st(struct term *t, bool bel, const char *fmt, ...) /* OSC reply */
    __attribute__((format(printf, 3, 4)));
void term_reply_apc(struct term *t, const char *fmt, ...) /* graphics reply */
    __attribute__((format(printf, 2, 3)));
