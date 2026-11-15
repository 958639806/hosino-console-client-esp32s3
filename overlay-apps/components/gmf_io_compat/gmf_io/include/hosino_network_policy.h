/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
static inline size_t hosino_network_length(const char *value, size_t limit)
{
    size_t n = 0;
    while (n < limit && value[n]) ++n;
    return n;
}

static inline bool hosino_network_path_plain(const char *path)
{
    const char *segment = path;
    for (const char *p = path;; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (c == '/' || c == 0) {
            const size_t n = (size_t)(p - segment);
            if ((n == 1 && segment[0] == '.') ||
                (n == 2 && segment[0] == '.' && segment[1] == '.')) return false;
            segment = p + 1;
        }
        if (c == 0) break;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '/' || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}

/* Managed bridge URLs are deliberately simple: HTTPS, an exact configured
 * base path, and opaque unescaped stream paths. No credentials in a URL. */
static inline bool hosino_network_url_allowed(const char *url, const char *base)
{
    if (!url || !base || strncmp(base, "https://", 8) != 0) return false;
    const size_t n = hosino_network_length(base, 512), m = hosino_network_length(url, 512);
    if (n <= 8 || n >= 512 || m <= n || m >= 512 || base[n - 1] == '/' ||
        strncmp(url, base, n) != 0 || url[n] != '/') return false;
    const char *end = strchr(base + 8, '/');
    if (!end) end = base + n;
    if (end == base + 8) return false;
    for (const char *p = base + 8; p < end; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':')) return false;
    }
    for (const char *p = base; *p; ++p)
        if ((unsigned char)*p <= 32 || (unsigned char)*p >= 127 || strchr("?#@%\\", *p)) return false;
    for (const char *p = url + n; *p; ++p)
        if ((unsigned char)*p <= 32 || (unsigned char)*p >= 127 || strchr("?#@%\\", *p)) return false;
    return hosino_network_path_plain(end) && hosino_network_path_plain(url + (end - base));
}

static inline bool hosino_network_token_valid(const char *token)
{
    if (!token) return false;
    const size_t n = hosino_network_length(token, 193);
    if (n < 24 || n > 192) return false;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char)token[i] <= 32 || (unsigned char)token[i] >= 127) return false;
    return true;
}
