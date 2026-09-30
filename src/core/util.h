#pragma once

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(x, lo, hi) MIN(MAX((x), (lo)), (hi))

void *xmalloc(size_t size);
void *xcalloc(size_t nmemb, size_t size);
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *s);

enum log_level { LOG_LEVEL_ERR,
                 LOG_LEVEL_WARN,
                 LOG_LEVEL_INFO,
                 LOG_LEVEL_DBG };

void log_msg(enum log_level lvl, const char *module, int errnum,
             const char *fmt, ...) __attribute__((format(printf, 4, 5)));
void log_set_level(enum log_level lvl);

#ifndef LOG_MODULE
#define LOG_MODULE "astralia"
#endif

#define LOG_ERR(...) log_msg(LOG_LEVEL_ERR, LOG_MODULE, 0, __VA_ARGS__)
#define LOG_ERRNO(...) log_msg(LOG_LEVEL_ERR, LOG_MODULE, errno, __VA_ARGS__)
#define LOG_WARN(...) log_msg(LOG_LEVEL_WARN, LOG_MODULE, 0, __VA_ARGS__)
#define LOG_INFO(...) log_msg(LOG_LEVEL_INFO, LOG_MODULE, 0, __VA_ARGS__)
#define LOG_DBG(...) log_msg(LOG_LEVEL_DBG, LOG_MODULE, 0, __VA_ARGS__)

/* Display width of a codepoint: 0, 1 or 2 (table in width_table.h;
 * independent of the locale). */
int cp_width(uint32_t cp);
