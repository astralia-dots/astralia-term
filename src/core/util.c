#include "core/util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/width_table.h"

static enum log_level max_level = LOG_LEVEL_INFO;

static void
oom(void) {
    fputs("astralia-term: out of memory\n", stderr);
    abort();
}

void *
xmalloc(size_t size) {
    void *p = malloc(size);
    if (unlikely(p == NULL && size != 0))
        oom();
    return p;
}

void *
xcalloc(size_t nmemb, size_t size) {
    void *p = calloc(nmemb, size);
    if (unlikely(p == NULL && nmemb != 0 && size != 0))
        oom();
    return p;
}

void *
xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size);
    if (unlikely(p == NULL && size != 0))
        oom();
    return p;
}

char *
xstrdup(const char *s) {
    char *p = strdup(s);
    if (unlikely(p == NULL))
        oom();
    return p;
}

void log_set_level(enum log_level lvl) {
    max_level = lvl;
}

void log_msg(enum log_level lvl, const char *module, int errnum, const char *fmt, ...) {
    if (lvl > max_level)
        return;

    static const char *const prefix[] = {
        [LOG_LEVEL_ERR] = "err",
        [LOG_LEVEL_WARN] = "warn",
        [LOG_LEVEL_INFO] = "info",
        [LOG_LEVEL_DBG] = "dbg",
    };

    fprintf(stderr, "%s: %s: ", prefix[lvl], module);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (errnum != 0)
        fprintf(stderr, ": %s", strerror(errnum));
    fputc('\n', stderr);
}

int cp_width(uint32_t cp) {
    if (likely(cp < 0x7f))
        return cp >= 0x20 ? 1 : 0;
    if (unlikely(cp >= 0x110000))
        return 1;

    /* 2-bit codes: 0 = width 1, 1 = width 0, 2 = width 2 */
    uint8_t byte = width_stage2[width_stage1[cp >> 8]][(cp & 0xff) >> 2];
    unsigned code = (byte >> ((cp & 3) * 2)) & 3;
    return code == 0 ? 1 : code == 1 ? 0
                                     : 2;
}
