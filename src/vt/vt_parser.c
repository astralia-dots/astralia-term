#include "vt/vt_parser.h"

#include <stdlib.h>
#include <string.h>

#define LOG_MODULE "vt"
#include "core/util.h"

#define OSC_MAX_LEN (1u << 20)  /* OSC 52 payloads can be large */
#define APC_MAX_LEN (16u << 20) /* kitty graphics, unchunked */
#define DCS_MAX_LEN (16u << 20) /* sixel images, unchunked */

void vt_parser_init(struct vt_parser *p, const struct vt_callbacks *cb, void *user) {
    *p = (struct vt_parser){.state = VT_GROUND, .cb = *cb, .user = user};
}

void vt_parser_destroy(struct vt_parser *p) {
    free(p->str);
    p->str = NULL;
}

static void
clear(struct vt_parser *p) {
    p->csi.private_marker = 0;
    p->csi.n_inter = 0;
    p->csi.nparams = 0;
    p->csi.sub_mask = 0;
    p->param_overflow = false;
}

static void
collect(struct vt_parser *p, uint8_t c) {
    if (p->csi.n_inter < VT_MAX_INTERMEDIATES)
        p->csi.inter[p->csi.n_inter++] = c;
    else
        p->param_overflow = true;
}

static void
param_digit(struct vt_parser *p, uint8_t c) {
    if (p->csi.nparams == 0) {
        p->csi.nparams = 1;
        p->csi.params[0] = 0;
    }
    if (p->param_overflow)
        return;
    uint32_t *v = &p->csi.params[p->csi.nparams - 1];
    *v = *v * 10 + (c - '0');
    if (*v > VT_PARAM_MAX)
        *v = VT_PARAM_MAX;
}

static void
param_sep(struct vt_parser *p, bool sub) {
    if (p->csi.nparams == 0) {
        p->csi.nparams = 1;
        p->csi.params[0] = 0;
    }
    if (p->csi.nparams >= VT_MAX_PARAMS) {
        p->param_overflow = true;
        return;
    }
    int i = p->csi.nparams++;
    p->csi.params[i] = 0;
    if (sub)
        p->csi.sub_mask |= 1u << i;
}

static void
csi_dispatch(struct vt_parser *p, uint8_t final) {
    p->csi.final = final;
    if (p->cb.csi != NULL)
        p->cb.csi(p->user, &p->csi);
    p->state = VT_GROUND;
}

static void
esc_dispatch(struct vt_parser *p, uint8_t final) {
    if (p->cb.esc != NULL && !p->param_overflow)
        p->cb.esc(p->user, p->csi.inter, p->csi.n_inter, final);
    p->state = VT_GROUND;
}

static void
execute(struct vt_parser *p, uint8_t c) {
    if (p->cb.execute != NULL)
        p->cb.execute(p->user, c);
}

static void
print(struct vt_parser *p, uint32_t cp) {
    if (p->cb.print != NULL)
        p->cb.print(p->user, cp);
}

static void
str_start(struct vt_parser *p, enum vt_state state) {
    p->state = state;
    p->str_len = 0;
    p->str_overflow = false;
}

static void
str_put(struct vt_parser *p, uint8_t c) {
    size_t max = p->state == VT_APC_STRING ? APC_MAX_LEN
               : p->state == VT_DCS        ? DCS_MAX_LEN
                                            : OSC_MAX_LEN;
    if (p->str_overflow)
        return;
    if (p->str_len + 1 >= max) {
        p->str_overflow = true;
        LOG_WARN("string sequence exceeds %zu bytes, discarding", max);
        return;
    }
    if (p->str_len + 1 >= p->str_cap) {
        p->str_cap = p->str_cap ? p->str_cap * 2 : 256;
        p->str = xrealloc(p->str, p->str_cap);
    }
    p->str[p->str_len++] = c;
}

/* Called whenever an OSC/APC string ends (BEL, ST, or a new ESC). */
static void
str_end(struct vt_parser *p, bool bel) {
    bool osc = p->state == VT_OSC_STRING && p->cb.osc != NULL;
    bool apc = p->state == VT_APC_STRING && p->cb.apc != NULL;
    bool dcs = p->state == VT_DCS && p->cb.dcs != NULL;

    if ((osc || apc || dcs) && !p->str_overflow) {
        if (p->str == NULL) {
            p->str_cap = 256;
            p->str = xmalloc(p->str_cap);
        }
        p->str[p->str_len] = '\0';
        if (osc)
            p->cb.osc(p->user, p->str, p->str_len, bel);
        else if (apc)
            p->cb.apc(p->user, p->str, p->str_len);
        else
            p->cb.dcs(p->user, p->csi.private_marker, p->csi.params, p->csi.nparams,
                      p->csi.final, p->str, p->str_len);
    }

    /* Don't hold on to a huge buffer after a large transfer */
    if (p->str_cap > 64 * 1024) {
        free(p->str);
        p->str = NULL;
        p->str_cap = 0;
    }
    p->str_len = 0;
}

static void
utf8_start(struct vt_parser *p, uint8_t c) {
    if (c >= 0xc2 && c <= 0xdf) {
        p->utf8_cp = c & 0x1f;
        p->utf8_left = 1;
        p->utf8_min = 0x80;
    } else if (c >= 0xe0 && c <= 0xef) {
        p->utf8_cp = c & 0x0f;
        p->utf8_left = 2;
        p->utf8_min = 0x800;
    } else if (c >= 0xf0 && c <= 0xf4) {
        p->utf8_cp = c & 0x07;
        p->utf8_left = 3;
        p->utf8_min = 0x10000;
    } else
        print(p, 0xfffd);
}

/* Returns true if c was consumed as a continuation byte. */
static bool
utf8_continue(struct vt_parser *p, uint8_t c) {
    if ((c & 0xc0) != 0x80) {
        p->utf8_left = 0;
        print(p, 0xfffd);
        return false;
    }

    p->utf8_cp = (p->utf8_cp << 6) | (c & 0x3f);
    if (--p->utf8_left > 0)
        return true;

    uint32_t cp = p->utf8_cp;
    if (cp < p->utf8_min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
        cp = 0xfffd;
    print(p, cp);
    return true;
}

static void
ground(struct vt_parser *p, uint8_t c) {
    if (c < 0x20)
        execute(p, c);
    else if (c < 0x7f)
        print(p, c);
    else if (c >= 0x80)
        utf8_start(p, c);
}

static void
escape(struct vt_parser *p, uint8_t c) {
    if (c < 0x20) {
        execute(p, c);
    } else if (c < 0x30) {
        collect(p, c);
        p->state = VT_ESCAPE_INTERMEDIATE;
    } else if (c == '[') {
        p->state = VT_CSI_ENTRY;
    } else if (c == ']') {
        str_start(p, VT_OSC_STRING);
    } else if (c == '_') {
        str_start(p, VT_APC_STRING);
    } else if (c == 'P') {
        p->state = VT_DCS_ENTRY;
    } else if (c == 'X' || c == '^') {
        p->state = VT_SOS_PM_STRING;
    } else if (c < 0x7f) {
        esc_dispatch(p, c);
    } else if (c >= 0x80) {
        p->state = VT_GROUND;
        ground(p, c);
    }
}

static void
escape_intermediate(struct vt_parser *p, uint8_t c) {
    if (c < 0x20)
        execute(p, c);
    else if (c < 0x30)
        collect(p, c);
    else if (c < 0x7f)
        esc_dispatch(p, c);
    else if (c >= 0x80)
        p->state = VT_GROUND;
}

static void
csi_param(struct vt_parser *p, uint8_t c) {
    if (c < 0x20) {
        execute(p, c);
    } else if (c < 0x30) {
        collect(p, c);
        p->state = VT_CSI_INTERMEDIATE;
    } else if (c <= '9') {
        param_digit(p, c);
        p->state = VT_CSI_PARAM;
    } else if (c == ';' || c == ':') {
        param_sep(p, c == ':');
        p->state = VT_CSI_PARAM;
    } else if (c < 0x40) {
        /* private marker; only valid as the first byte */
        if (p->state == VT_CSI_ENTRY) {
            p->csi.private_marker = c;
            p->state = VT_CSI_PARAM;
        } else
            p->state = VT_CSI_IGNORE;
    } else if (c < 0x7f) {
        csi_dispatch(p, c);
    } else if (c >= 0x80) {
        p->state = VT_CSI_IGNORE;
    }
}

static void
csi_intermediate(struct vt_parser *p, uint8_t c) {
    if (c < 0x20)
        execute(p, c);
    else if (c < 0x30)
        collect(p, c);
    else if (c < 0x40)
        p->state = VT_CSI_IGNORE;
    else if (c < 0x7f)
        csi_dispatch(p, c);
}

/* Ps;Ps;... params + optional private marker, ended by a final byte that
 * enters DCS string passthrough (VT_DCS). No intermediates/ignore-state
 * distinction: sixel headers ("Pi;Pa;Pv q") never use them. */
static void
dcs_header(struct vt_parser *p, uint8_t c) {
    if (c < 0x20) {
        /* ignore C0 controls in the DCS header */
    } else if (c < 0x30) {
        collect(p, c);
    } else if (c <= '9') {
        param_digit(p, c);
        p->state = VT_DCS_PARAM;
    } else if (c == ';' || c == ':') {
        param_sep(p, c == ':');
        p->state = VT_DCS_PARAM;
    } else if (c < 0x40) {
        /* private marker; only valid as the first byte */
        if (p->state == VT_DCS_ENTRY) {
            p->csi.private_marker = c;
            p->state = VT_DCS_PARAM;
        }
    } else if (c < 0x7f) {
        p->csi.final = c;
        str_start(p, VT_DCS);
    }
    /* c >= 0x7f: malformed; ignore */
}

static void
csi_ignore(struct vt_parser *p, uint8_t c) {
    if (c < 0x20)
        execute(p, c);
    else if (c >= 0x40 && c < 0x7f)
        p->state = VT_GROUND;
}

static void
string(struct vt_parser *p, uint8_t c) {
    if (c == 0x07 && p->state == VT_OSC_STRING) {
        /* BEL terminates OSC */
        str_end(p, true);
        p->state = VT_GROUND;
    } else if (c >= 0x20) {
        str_put(p, c);
    }
    /* other C0 controls are ignored inside strings */
}

static void
byte(struct vt_parser *p, uint8_t c) {
    if (p->utf8_left > 0 && utf8_continue(p, c))
        return;

    /* "anywhere" transitions */
    if (c == 0x18 || c == 0x1a) {
        if (p->state == VT_OSC_STRING || p->state == VT_APC_STRING || p->state == VT_DCS)
            p->str_len = 0;
        execute(p, c);
        p->state = VT_GROUND;
        return;
    }
    if (c == 0x1b) {
        if (p->state == VT_OSC_STRING || p->state == VT_APC_STRING || p->state == VT_DCS)
            str_end(p, false);
        clear(p);
        p->state = VT_ESCAPE;
        return;
    }

    switch (p->state) {
    case VT_GROUND:
        ground(p, c);
        break;
    case VT_ESCAPE:
        escape(p, c);
        break;
    case VT_ESCAPE_INTERMEDIATE:
        escape_intermediate(p, c);
        break;
    case VT_CSI_ENTRY:
    case VT_CSI_PARAM:
        csi_param(p, c);
        break;
    case VT_CSI_INTERMEDIATE:
        csi_intermediate(p, c);
        break;
    case VT_CSI_IGNORE:
        csi_ignore(p, c);
        break;
    case VT_OSC_STRING:
    case VT_APC_STRING:
    case VT_DCS:
        string(p, c);
        break;
    case VT_DCS_ENTRY:
    case VT_DCS_PARAM:
        dcs_header(p, c);
        break;
    case VT_SOS_PM_STRING:
        break; /* swallowed until ST */
    }
}

void vt_parser_feed(struct vt_parser *p, const uint8_t *buf, size_t len) {
    size_t i = 0;
    while (i < len) {
        if (p->state == VT_GROUND && p->utf8_left == 0 && p->cb.print_ascii != NULL) {
            size_t start = i;
            while (i < len && buf[i] >= 0x20 && buf[i] < 0x7f)
                i++;
            if (i > start) {
                p->cb.print_ascii(p->user, buf + start, i - start);
                continue;
            }
        }
        byte(p, buf[i++]);
    }
}
