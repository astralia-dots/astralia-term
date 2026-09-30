#pragma once

#include <stddef.h>
#include <stdint.h>

/* Decodes into out (at least len / 4 * 3 bytes). Returns the decoded
 * length, or (size_t)-1 on malformed input. */
size_t base64_decode(const char *in, size_t len, uint8_t *out);

/* Returns a NUL-terminated xmalloc'ed string. */
char *base64_encode(const uint8_t *in, size_t len);
