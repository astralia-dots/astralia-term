#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "vt/vt_parser.h"

static int failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                              \
        }                                                                            \
    } while (0)

/* Records every callback as text, e.g. "P(41)X(0a)C(?25h)" */
static char log_buf[4096];

static void __attribute__((format(printf, 1, 2)))
logf_(const char *fmt, ...) {
    size_t len = strlen(log_buf);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(log_buf + len, sizeof(log_buf) - len, fmt, ap);
    va_end(ap);
}

static void
on_print(void *user, uint32_t cp) {
    logf_("P(%x)", cp);
}

static void
on_execute(void *user, uint8_t c) {
    logf_("X(%02x)", c);
}

static void
on_csi(void *user, const struct vt_csi *c) {
    logf_("C(");
    if (c->private_marker)
        logf_("%c", c->private_marker);
    for (int i = 0; i < c->nparams; i++)
        logf_("%s%u", i == 0 ? "" : vt_param_is_sub(c, i) ? ":"
                                                          : ";",
              c->params[i]);
    for (int i = 0; i < c->n_inter; i++)
        logf_("%c", c->inter[i]);
    logf_("%c)", c->final);
}

static void
on_esc(void *user, const uint8_t *inter, int n, uint8_t final) {
    logf_("E(%.*s%c)", n, (const char *)inter, final);
}

static void
on_osc(void *user, const uint8_t *data, size_t len, bool bel) {
    logf_("O(%s%s)", (const char *)data, bel ? "|bel" : "");
}

static void
on_apc(void *user, const uint8_t *data, size_t len) {
    logf_("A(%s)", (const char *)data);
}

static const char *
run(const char *input) {
    static const struct vt_callbacks cbs = {
        .print = on_print,
        .execute = on_execute,
        .csi = on_csi,
        .esc = on_esc,
        .osc = on_osc,
        .apc = on_apc,
    };
    struct vt_parser p;
    vt_parser_init(&p, &cbs, NULL);
    log_buf[0] = '\0';
    vt_parser_feed(&p, (const uint8_t *)input, strlen(input));
    vt_parser_destroy(&p);
    return log_buf;
}

/* Same, but fed one byte at a time to exercise state carried across reads */
static const char *
run_bytewise(const char *input) {
    static const struct vt_callbacks cbs = {
        .print = on_print,
        .execute = on_execute,
        .csi = on_csi,
        .esc = on_esc,
        .osc = on_osc,
        .apc = on_apc,
    };
    struct vt_parser p;
    vt_parser_init(&p, &cbs, NULL);
    log_buf[0] = '\0';
    for (size_t i = 0; input[i] != '\0'; i++)
        vt_parser_feed(&p, (const uint8_t *)&input[i], 1);
    vt_parser_destroy(&p);
    return log_buf;
}

#define EXPECT(input, expected)                                                           \
    do {                                                                                  \
        const char *got_ = run(input);                                                    \
        if (strcmp(got_, expected) != 0) {                                                \
            fprintf(stderr, "%s:%d: input %s\n  expected %s\n  got      %s\n",            \
                    __FILE__, __LINE__, #input, expected, got_);                          \
            failures++;                                                                   \
        }                                                                                 \
        got_ = run_bytewise(input);                                                       \
        if (strcmp(got_, expected) != 0) {                                                \
            fprintf(stderr, "%s:%d: (bytewise) input %s\n  expected %s\n  got      %s\n", \
                    __FILE__, __LINE__, #input, expected, got_);                          \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

int main(void) {
    /* plain text and C0 */
    EXPECT("ab", "P(61)P(62)");
    EXPECT("a\r\nb", "P(61)X(0d)X(0a)P(62)");

    /* UTF-8 */
    EXPECT("\xc3\xa9", "P(e9)");            /* é */
    EXPECT("\xe4\xb8\xad", "P(4e2d)");      /* 中 */
    EXPECT("\xf0\x9f\x98\x80", "P(1f600)"); /* 😀 */
    EXPECT("\xc0\xaf", "P(fffd)P(fffd)");   /* invalid lead byte */
    EXPECT("\xe0\x80\xaf", "P(fffd)");      /* overlong */
    EXPECT("\xed\xa0\x80", "P(fffd)");      /* surrogate */
    EXPECT("\xc3"
           "a",
           "P(fffd)P(61)");                  /* truncated sequence */
    EXPECT("\xe4\xb8\x1b[m", "P(fffd)C(m)"); /* ESC interrupts UTF-8 */

    /* CSI */
    EXPECT("\x1b[H", "C(H)");
    EXPECT("\x1b[12;34H", "C(12;34H)");
    EXPECT("\x1b[;5H", "C(0;5H)");
    EXPECT("\x1b[?25l", "C(?25l)");
    EXPECT("\x1b[?1049;2004h", "C(?1049;2004h)");
    EXPECT("\x1b[38:2::10:20:30m", "C(38:2:0:10:20:30m)");
    EXPECT("\x1b[4:3m", "C(4:3m)");
    EXPECT("\x1b[2 q", "C(2 q)");
    EXPECT("\x1b[>c", "C(>c)");
    EXPECT("\x1b[1\n2H", "X(0a)C(12H)");         /* C0 executes inside CSI */
    EXPECT("\x1b[1?2Hx", "P(78)");               /* misplaced marker: ignored */
    EXPECT("\x1b[99999999999A", "C(16777215A)"); /* clamped */

    /* ESC */
    EXPECT("\x1b"
           "7\x1b"
           "8",
           "E(7)E(8)");
    EXPECT("\x1b(0", "E((0)");
    EXPECT("\x1b#8", "E(#8)");

    /* OSC terminated by BEL and by ST */
    EXPECT("\x1b]0;title\a", "O(0;title|bel)");
    EXPECT("\x1b]2;t\xc3\xa9\x1b\\", "O(2;t\xc3\xa9)E(\\)");

    /* APC (kitty graphics) */
    EXPECT("\x1b_Gf=24;AAAA\x1b\\x", "A(Gf=24;AAAA)E(\\)P(78)");

    /* DCS and SOS/PM are swallowed */
    EXPECT("\x1bPq#0;1\x1b\\z", "E(\\)P(7a)");
    EXPECT("\x1b^hidden\x1b\\z", "E(\\)P(7a)");

    /* CAN aborts a sequence */
    EXPECT("\x1b[12\x18"
           "a",
           "X(18)P(61)");
    EXPECT("\x1b]0;x\x18"
           "a",
           "X(18)P(61)");

    if (failures == 0)
        printf("test_parser: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
