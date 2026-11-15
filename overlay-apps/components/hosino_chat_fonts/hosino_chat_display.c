#include "hosino_chat_display.h"
#include <stdint.h>
#include <string.h>

static size_t decode(const unsigned char *s, uint32_t *cp)
{
    const unsigned char c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    unsigned n;
    if (c >= 0xC2 && c <= 0xDF) { n = 2; *cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 3; *cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 4; *cp = c & 7; }
    else return 0;
    for (unsigned i = 1; i < n; ++i) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        *cp = (*cp << 6) | (s[i] & 0x3F);
    }
    if ((n == 3 && *cp < 0x800) || (n == 4 && *cp < 0x10000) ||
        (*cp >= 0xD800 && *cp <= 0xDFFF) || *cp > 0x10FFFF) return 0;
    return n;
}

size_t hosino_chat_format_display(char *dst, size_t cap, const char *src)
{
    size_t used = 0, required = 0;
    int full = 0;
    if (cap && dst) dst[0] = '\0';
    if (!src || (cap && !dst)) return 0;
    const unsigned char *s = (const unsigned char *)src;
    while (*s) {
        uint32_t cp = 0;
        size_t input = decode(s, &cp), n = input;
        const char *text = (const char *)s;
        char token[16];
        if (!input) { input = 1; text = "[?]"; n = 3; }
        else if (cp == 0xFE0E || cp == 0xFE0F) { n = 0; }
        else if (cp == 0x200D) { text = "+"; n = 1; }
        else if (cp >= 0x1F3FB && cp <= 0x1F3FF) {
            memcpy(token, "[skin1]", 7); token[5] = (char)('1' + cp - 0x1F3FB);
            text = token; n = 7;
        } else if (cp >= 0xE0020 && cp <= 0xE007F) {
            static const char hex[] = "0123456789ABCDEF";
            memcpy(token, "[tag:00]", 8);
            token[5] = hex[(cp & 255) >> 4]; token[6] = hex[cp & 15];
            text = token; n = 8;
        }
        if (n && !full) {
            if (cap && n < cap - used) { memcpy(dst + used, text, n); used += n; }
            else full = 1;
        }
        required += n;
        s += input;
    }
    if (cap) dst[used] = '\0';
    return required;
}
