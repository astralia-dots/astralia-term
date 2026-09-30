#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_MODULE "osc"
#include "core/base64.h"
#include "core/util.h"
#include "term/term.h"

static int
hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Parses n hex digits and scales them to 8 bits. */
static bool
hex_component(const char *s, int n, uint32_t *out) {
    if (n < 1 || n > 4)
        return false;
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        int d = hex_digit(s[i]);
        if (d < 0)
            return false;
        v = v << 4 | (uint32_t)d;
    }
    uint32_t max = (1u << (4 * n)) - 1;
    *out = (v * 255 + max / 2) / max;
    return true;
}

/* XParseColor subset: rgb:r/g/b (1-4 hex digits each) and #rgb forms. */
static bool
parse_color(const char *spec, uint32_t *rgb) {
    uint32_t c[3];

    if (strncmp(spec, "rgb:", 4) == 0) {
        const char *s = spec + 4;
        for (int i = 0; i < 3; i++) {
            const char *end = strchr(s, i < 2 ? '/' : '\0');
            if (end == NULL || !hex_component(s, (int)(end - s), &c[i]))
                return false;
            s = end + 1;
        }
    } else if (spec[0] == '#') {
        size_t len = strlen(spec + 1);
        if (len == 0 || len % 3 != 0 || len > 12)
            return false;
        /* Unlike rgb:, #rgb forms keep the most significant digits */
        int n = (int)(len / 3);
        for (int i = 0; i < 3; i++) {
            if (!hex_component(spec + 1 + i * n, MIN(n, 2), &c[i]))
                return false;
        }
    } else
        return false;

    *rgb = c[0] << 16 | c[1] << 8 | c[2];
    return true;
}

static void
reply_color(struct term *t, bool bel, const char *prefix, uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    term_reply_st(t, bel, "\033]%s;rgb:%04x/%04x/%04x", prefix, r * 257, g * 257, b * 257);
}

/* Splits at the next ';', returning the start of the following field or NULL. */
static char *
next_field(char *s) {
    char *semi = strchr(s, ';');
    if (semi == NULL)
        return NULL;
    *semi = '\0';
    return semi + 1;
}

static void
osc_palette(struct term *t, char *args, bool bel) {
    while (args != NULL) {
        char *spec = next_field(args);
        if (spec == NULL)
            return;
        char *rest = next_field(spec);

        char *end;
        long idx = strtol(args, &end, 10);
        if (*end == '\0' && idx >= 0 && idx < 256) {
            uint32_t rgb;
            if (strcmp(spec, "?") == 0) {
                char prefix[16];
                snprintf(prefix, sizeof(prefix), "4;%ld", idx);
                reply_color(t, bel, prefix, t->palette[idx]);
            } else if (parse_color(spec, &rgb)) {
                t->palette[idx] = rgb;
                t->colors_changed = true;
            }
        }
        args = rest;
    }
}

/* OSC 10/11/12; extra arguments apply to the following numbers. */
static void
osc_dynamic_colors(struct term *t, int ps, char *args, bool bel) {
    for (; args != NULL && ps <= 12; ps++) {
        char *rest = next_field(args);
        uint32_t *color = ps == 10 ? &t->default_fg : ps == 11 ? &t->default_bg
                                                               : &t->cursor_color;

        if (strcmp(args, "?") == 0) {
            char prefix[8];
            snprintf(prefix, sizeof(prefix), "%d", ps);
            uint32_t rgb = ps == 12 && !t->cursor_color_set ? t->default_fg : *color;
            reply_color(t, bel, prefix, rgb);
        } else if (parse_color(args, color)) {
            if (ps == 12)
                t->cursor_color_set = true;
            t->colors_changed = true;
            t->cursor_dirty = true;
        }
        args = rest;
    }
}

static void
osc_reset_palette(struct term *t, char *args) {
    if (args == NULL || *args == '\0') {
        memcpy(t->palette, t->initial_palette, sizeof(t->palette));
        t->colors_changed = true;
        return;
    }
    while (args != NULL) {
        char *rest = next_field(args);
        char *end;
        long idx = strtol(args, &end, 10);
        if (*end == '\0' && idx >= 0 && idx < 256) {
            t->palette[idx] = t->initial_palette[idx];
            t->colors_changed = true;
        }
        args = rest;
    }
}

static void
osc_clipboard(struct term *t, char *args) {
    char *data = next_field(args);
    if (data == NULL)
        return;

    if (strcmp(data, "?") == 0) {
        LOG_DBG("OSC 52 clipboard read denied");
        return;
    }

    char target = 'c';
    for (const char *p = args; *p != '\0'; p++) {
        if (strchr("cps", *p) != NULL) {
            target = *p;
            break;
        }
    }

    size_t len = strlen(data);
    uint8_t *text = xmalloc(len / 4 * 3 + 1);
    size_t n = base64_decode(data, len, text);
    if (n == (size_t)-1)
        LOG_DBG("OSC 52: invalid base64");
    else if (t->host != NULL && t->host->set_clipboard != NULL) {
        text[n] = '\0';
        t->host->set_clipboard(t->host_user, target, (const char *)text, n);
    }
    free(text);
}

/* OSC 8 ; params ; URI. An empty URI closes the link; only `id=` is read from
 * the ':'-separated params. Control characters in a URI reject the link. */
static void
osc_hyperlink(struct term *t, char *args) {
    char *uri = next_field(args);
    if (uri == NULL || uri[0] == '\0') {
        t->link = 0;
        return;
    }
    for (const char *p = uri; *p != '\0'; p++) {
        if ((uint8_t)*p < 0x20 || *p == 0x7f) {
            t->link = 0;
            return;
        }
    }

    const char *id = "";
    for (char *param = args; param != NULL && *param != '\0';) {
        char *sep = strchr(param, ':');
        if (sep != NULL)
            *sep = '\0';
        if (strncmp(param, "id=", 3) == 0)
            id = param + 3;
        param = sep != NULL ? sep + 1 : NULL;
    }
    t->link = hyperlink_intern(&t->hyperlinks, id, uri);
}

void term_osc(struct term *t, const uint8_t *data, size_t len, bool bel) {
    char *buf = xmalloc(len + 1);
    memcpy(buf, data, len);
    buf[len] = '\0';

    char *args = next_field(buf);
    char *end;
    long ps = strtol(buf, &end, 10);
    if (end == buf || *end != '\0') {
        free(buf);
        return;
    }

    switch (ps) {
    case 0:
    case 2:
        if (args != NULL && t->host != NULL && t->host->set_title != NULL)
            t->host->set_title(t->host_user, args);
        break;
    case 1:
        break; /* icon name */
    case 4:
        osc_palette(t, args, bel);
        break;
    case 10:
    case 11:
    case 12:
        osc_dynamic_colors(t, (int)ps, args, bel);
        break;
    case 8:
        if (args != NULL)
            osc_hyperlink(t, args);
        break;
    case 52:
        if (args != NULL)
            osc_clipboard(t, args);
        break;
    case 104:
        osc_reset_palette(t, args);
        break;
    case 110:
        t->default_fg = t->initial_fg;
        t->colors_changed = true;
        break;
    case 111:
        t->default_bg = t->initial_bg;
        t->colors_changed = true;
        break;
    case 112:
        t->cursor_color_set = false;
        t->cursor_dirty = true;
        break;
    default:
        LOG_DBG("unhandled OSC %ld", ps);
        break;
    }
    free(buf);
}
