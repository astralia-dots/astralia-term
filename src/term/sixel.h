#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SIXEL_MAX_COLORS 1024u
#define SIXEL_MAX_WIDTH 4096u
#define SIXEL_MAX_HEIGHT 4096u

/* Decodes a sixel DCS body (the bytes between the "q" introducer and ST)
 * into a straight-alpha RGBA pixmap (width * height * 4 bytes, row-major,
 * caller-owned on success). Pixels never painted by the stream are left
 * fully transparent. Returns false, leaving *out_rgba untouched, on an
 * empty/malformed image or one exceeding SIXEL_MAX_WIDTH/HEIGHT. */
bool sixel_decode(const uint8_t *data, size_t len, uint8_t **out_rgba, int *out_width,
                  int *out_height);
