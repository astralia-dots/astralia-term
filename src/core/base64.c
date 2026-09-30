#include "core/base64.h"

#define LOG_MODULE "base64"
#include "core/util.h"

static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int
decode_char(char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

size_t base64_decode(const char *in, size_t len, uint8_t *out) {
    if (len % 4 != 0)
        return (size_t)-1;

    size_t n = 0;
    for (size_t i = 0; i < len; i += 4) {
        int v[4];
        int pad = 0;
        for (int j = 0; j < 4; j++) {
            if (in[i + j] == '=' && i + 4 == len && j >= 2) {
                v[j] = 0;
                pad++;
            } else if (pad > 0 || (v[j] = decode_char(in[i + j])) < 0)
                return (size_t)-1;
        }

        uint32_t bits = (uint32_t)v[0] << 18 | (uint32_t)v[1] << 12 | (uint32_t)v[2] << 6 | (uint32_t)v[3];
        out[n++] = bits >> 16;
        if (pad < 2)
            out[n++] = (bits >> 8) & 0xff;
        if (pad < 1)
            out[n++] = bits & 0xff;
    }
    return n;
}

char *
base64_encode(const uint8_t *in, size_t len) {
    char *out = xmalloc((len + 2) / 3 * 4 + 1);
    size_t n = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t bits = (uint32_t)in[i] << 16;
        if (i + 1 < len)
            bits |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len)
            bits |= in[i + 2];
        out[n++] = alphabet[bits >> 18];
        out[n++] = alphabet[(bits >> 12) & 63];
        out[n++] = i + 1 < len ? alphabet[(bits >> 6) & 63] : '=';
        out[n++] = i + 2 < len ? alphabet[bits & 63] : '=';
    }
    out[n] = '\0';
    return out;
}
