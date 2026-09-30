/*
 * DEC/ANSI escape sequence parser, after Paul Williams' state machine
 * (https://vt100.net/emu/dec_ansi_parser), in UTF-8 mode: bytes >= 0x80 are
 * UTF-8 in the ground state and 8-bit C1 controls are not recognized.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VT_MAX_PARAMS 32
#define VT_MAX_INTERMEDIATES 2
#define VT_PARAM_MAX 0xffffff

struct vt_csi {
    uint8_t private_marker; /* '?', '>', '=', '<' or 0 */
    uint8_t n_inter;
    uint8_t inter[VT_MAX_INTERMEDIATES];
    uint8_t final;
    int nparams;
    uint32_t params[VT_MAX_PARAMS];
    uint32_t sub_mask; /* bit i: params[i] was introduced by ':' */
};

/* Parameter i, or def when missing or 0 (the usual CSI default rule). */
static inline uint32_t
vt_param(const struct vt_csi *csi, int i, uint32_t def) {
    return i < csi->nparams && csi->params[i] != 0 ? csi->params[i] : def;
}

static inline bool
vt_param_is_sub(const struct vt_csi *csi, int i) {
    return i < csi->nparams && (csi->sub_mask & (1u << i));
}

/* Any callback may be NULL. String payloads (osc, apc) are NUL-terminated. */
struct vt_callbacks {
    void (*print)(void *user, uint32_t cp);
    void (*print_ascii)(void *user, const uint8_t *s, size_t len);
    void (*execute)(void *user, uint8_t c);
    void (*csi)(void *user, const struct vt_csi *csi);
    void (*esc)(void *user, const uint8_t *inter, int n_inter, uint8_t final);
    void (*osc)(void *user, const uint8_t *data, size_t len, bool bel); /* bel: BEL-terminated */
    void (*apc)(void *user, const uint8_t *data, size_t len);
    /* private_marker: '<','=','>','?' or 0. params/nparams: as in vt_csi. data is NUL-terminated. */
    void (*dcs)(void *user, uint8_t private_marker, const uint32_t *params, int nparams,
                uint8_t final, const uint8_t *data, size_t len);
};

enum vt_state {
    VT_GROUND,
    VT_ESCAPE,
    VT_ESCAPE_INTERMEDIATE,
    VT_CSI_ENTRY,
    VT_CSI_PARAM,
    VT_CSI_INTERMEDIATE,
    VT_CSI_IGNORE,
    VT_DCS_ENTRY,
    VT_DCS_PARAM,
    VT_DCS, /* passthrough: buffering the DCS string body until ST */
    VT_OSC_STRING,
    VT_APC_STRING,
    VT_SOS_PM_STRING,
};

struct vt_parser {
    enum vt_state state;
    struct vt_callbacks cb;
    void *user;

    struct vt_csi csi;
    bool param_overflow;

    uint32_t utf8_cp;
    uint32_t utf8_min;
    int utf8_left;

    uint8_t *str; /* OSC / APC payload */
    size_t str_len, str_cap;
    bool str_overflow;
};

void vt_parser_init(struct vt_parser *p, const struct vt_callbacks *cb, void *user);
void vt_parser_destroy(struct vt_parser *p);
void vt_parser_feed(struct vt_parser *p, const uint8_t *buf, size_t len);
