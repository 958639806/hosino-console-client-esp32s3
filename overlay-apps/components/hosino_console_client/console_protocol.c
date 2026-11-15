/* SPDX-License-Identifier: Apache-2.0 */
#include "console_protocol.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

void hs_secure_zero(void *data, size_t size) {
    volatile unsigned char *p = data;
    while (size--) *p++ = 0;
}
bool hs_failed_post_is_uncertain(bool sent, bool cancelled, bool transport_ok, int status) {
    return sent && (cancelled || !transport_ok || status <= 0 ||
                    (status >= 200 && status < 400) || status >= 500);
}
static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static size_t utf8_len(const char *s, size_t n) {
    if (!n) return 0;
    const unsigned char c = (unsigned char)s[0];
    size_t k = c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
    if (!k || k > n) return 0;
    for (size_t i = 1; i < k; ++i) if (((unsigned char)s[i] & 0xc0) != 0x80) return 0;
    if (k == 3 && ((c == 0xe0 && (unsigned char)s[1] < 0xa0) || (c == 0xed && (unsigned char)s[1] >= 0xa0))) return 0;
    if (k == 4 && ((c == 0xf0 && (unsigned char)s[1] < 0x90) || (c == 0xf4 && (unsigned char)s[1] >= 0x90))) return 0;
    return k;
}
bool hs_utf8_copy(char *out, size_t cap, const char *in, size_t n) {
    if (!out || !cap || !in) return true;
    size_t p = 0, used = 0;
    while (p < n && in[p]) {
        size_t k = utf8_len(in + p, n - p);
        if (!k) { if (used + 1 >= cap) break; out[used++] = '?'; ++p; continue; }
        if (used + k >= cap) break;
        memcpy(out + used, in + p, k); used += k; p += k;
    }
    out[used] = 0;
    return p < n && in[p] != 0;
}
typedef struct { hs_json *j; size_t pos, capacity; } parser;
static void space(parser *p) {
    while (p->pos < p->j->length && (p->j->data[p->pos] == ' ' || p->j->data[p->pos] == '\t' || p->j->data[p->pos] == '\r' || p->j->data[p->pos] == '\n')) ++p->pos;
}
static bool u16(parser *p, unsigned *v) {
    if (p->j->length - p->pos < 4) return false;
    *v = 0;
    for (unsigned i = 0; i < 4; ++i) { int x = hex(p->j->data[p->pos++]); if (x < 0) return false; *v = (*v << 4) | (unsigned)x; }
    return true;
}
static bool string(parser *p, hs_json_token *t) {
    const char *s = p->j->data; const size_t n = p->j->length;
    t->type = HS_JSON_STRING; t->start = (uint32_t)++p->pos;
    while (p->pos < n) {
        unsigned char c = (unsigned char)s[p->pos++];
        if (c == '"') { t->end = (uint32_t)p->pos - 1; return true; }
        if (c < 0x20) return false;
        if (c == '\\') {
            if (p->pos == n) return false;
            c = (unsigned char)s[p->pos++];
            if (c == 'u') {
                unsigned v; if (!u16(p, &v)) return false;
                if (v >= 0xdc00 && v <= 0xdfff) return false;
                if (v >= 0xd800 && v <= 0xdbff) {
                    if (n - p->pos < 6 || s[p->pos] != '\\' || s[p->pos + 1] != 'u') return false;
                    p->pos += 2; if (!u16(p, &v) || v < 0xdc00 || v > 0xdfff) return false;
                }
            } else if (c == 0 || !strchr("\"\\/bfnrt", c)) return false;
        } else if (c >= 0x80) {
            const size_t k = utf8_len(s + p->pos - 1, n - p->pos + 1);
            if (!k) return false;
            p->pos += k - 1;
        }
    }
    return false;
}
static bool value(parser *p, unsigned depth) {
    space(p);
    if (depth > 24 || p->pos == p->j->length || p->j->count == p->capacity) return false;
    const size_t index = p->j->count++;
    hs_json_token *t = &p->j->tokens[index];
    t->start = (uint32_t)p->pos;
    const char *s = p->j->data; const size_t n = p->j->length;
    char c = s[p->pos];
    if (c == '"') { if (!string(p, t)) return false; }
    else if (c == '{' || c == '[') {
        const bool object = c == '{'; const char close = object ? '}' : ']';
        t->type = object ? HS_JSON_OBJECT : HS_JSON_ARRAY; ++p->pos; space(p);
        if (p->pos == n) return false;
        if (s[p->pos] != close) for (;;) {
            if (object) {
                if (p->pos == n || s[p->pos] != '"' || !value(p, depth + 1)) return false;
                space(p); if (p->pos == n || s[p->pos++] != ':') return false;
            }
            if (!value(p, depth + 1)) return false;
            space(p); if (p->pos == n) return false;
            if (s[p->pos] == close) break;
            if (s[p->pos++] != ',') return false;
            space(p); if (p->pos == n || s[p->pos] == close) return false;
        }
        ++p->pos; t->end = (uint32_t)p->pos;
    } else if (c == 't' || c == 'f' || c == 'n') {
        const char *literal = c == 't' ? "true" : c == 'f' ? "false" : "null";
        const size_t len = strlen(literal);
        if (n - p->pos < len || memcmp(s + p->pos, literal, len)) return false;
        p->pos += len; t->end = (uint32_t)p->pos; t->type = c == 'n' ? HS_JSON_NULL : HS_JSON_BOOL;
    } else {
        t->type = HS_JSON_NUMBER;
        if (c == '-') { if (++p->pos == n) return false; }
        if (s[p->pos] == '0') ++p->pos;
        else { if (s[p->pos] < '1' || s[p->pos] > '9') return false; while (p->pos < n && s[p->pos] >= '0' && s[p->pos] <= '9') ++p->pos; }
        if (p->pos < n && s[p->pos] == '.') {
            ++p->pos; size_t begin = p->pos;
            while (p->pos < n && s[p->pos] >= '0' && s[p->pos] <= '9') ++p->pos;
            if (begin == p->pos) return false;
        }
        if (p->pos < n && (s[p->pos] == 'e' || s[p->pos] == 'E')) {
            ++p->pos; if (p->pos < n && (s[p->pos] == '+' || s[p->pos] == '-')) ++p->pos;
            size_t begin = p->pos;
            while (p->pos < n && s[p->pos] >= '0' && s[p->pos] <= '9') ++p->pos;
            if (begin == p->pos) return false;
        }
        t->end = (uint32_t)p->pos;
    }
    t->next = (uint32_t)p->j->count;
    return true;
}
int hs_json_parse(hs_json *j, const char *data, size_t n, hs_json_token *tokens, size_t cap) {
    if (!j || !data || !tokens || !cap || n > UINT32_MAX) return -1;
    *j = (hs_json){data, n, tokens, 0}; parser p = {j, 0, cap};
    if (!value(&p, 0)) { j->count = 0; return -1; }
    space(&p); if (p.pos != n) { j->count = 0; return -1; }
    return (int)j->count;
}
static bool valid(const hs_json *j, int t) { return j && t >= 0 && (size_t)t < j->count; }
bool hs_json_text(const hs_json *j, int index, char *out, size_t cap, bool *cut) {
    if (cut) *cut = false;
    if (!out || !cap) return false;
    out[0] = 0;
    if (!valid(j, index)) return false;
    const hs_json_token *t = &j->tokens[index];
    if (t->type != HS_JSON_STRING) {
        if (t->type != HS_JSON_NUMBER && t->type != HS_JSON_BOOL) return false;
        const bool c = hs_utf8_copy(out, cap, j->data + t->start, t->end - t->start);
        if (cut) *cut = c;
        return true;
    }
    size_t p = t->start, used = 0;
    while (p < t->end) {
        char bytes[4]; size_t k;
        if (j->data[p] != '\\') { k = utf8_len(j->data + p, t->end - p); if (!k) return false; memcpy(bytes, j->data + p, k); p += k; }
        else {
            char c = j->data[++p]; ++p;
            if (c != 'u') { const char *esc = "\"\\/bfnrt", *mapped = "\"\\/\b\f\n\r\t"; bytes[0] = mapped[strchr(esc, c) - esc]; k = 1; }
            else {
                unsigned v = 0; for (unsigned i = 0; i < 4; ++i) v = (v << 4) | (unsigned)hex(j->data[p++]);
                if (v >= 0xd800 && v <= 0xdbff) { p += 2; unsigned low = 0; for (unsigned i = 0; i < 4; ++i) low = (low << 4) | (unsigned)hex(j->data[p++]); v = 0x10000 + ((v - 0xd800) << 10) + low - 0xdc00; }
                if (v == 0) { bytes[0] = '?'; k = 1; }
                else if (v < 0x80) { bytes[0] = (char)v; k = 1; }
                else if (v < 0x800) { bytes[0] = (char)(0xc0 | (v >> 6)); bytes[1] = (char)(0x80 | (v & 63)); k = 2; }
                else if (v < 0x10000) { bytes[0] = (char)(0xe0 | (v >> 12)); bytes[1] = (char)(0x80 | ((v >> 6) & 63)); bytes[2] = (char)(0x80 | (v & 63)); k = 3; }
                else { bytes[0] = (char)(0xf0 | (v >> 18)); bytes[1] = (char)(0x80 | ((v >> 12) & 63)); bytes[2] = (char)(0x80 | ((v >> 6) & 63)); bytes[3] = (char)(0x80 | (v & 63)); k = 4; }
            }
        }
        if (used + k >= cap) { if (cut) *cut = true; break; }
        memcpy(out + used, bytes, k); used += k;
    }
    out[used] = 0; return true;
}
int hs_json_member(const hs_json *j, int object, const char *name) {
    if (!valid(j, object) || j->tokens[object].type != HS_JSON_OBJECT) return -1;
    for (uint32_t p = (uint32_t)object + 1; p < j->tokens[object].next;) {
        char key[96]; bool cut;
        if (!hs_json_text(j, (int)p, key, sizeof(key), &cut)) return -1;
        uint32_t val = p + 1; if (val >= j->tokens[object].next) return -1;
        if (!cut && strcmp(key, name) == 0) return (int)val;
        p = j->tokens[val].next;
    }
    return -1;
}
int hs_json_first(const hs_json *j, int array) {
    if (!valid(j, array) || j->tokens[array].type != HS_JSON_ARRAY || j->tokens[array].next <= (uint32_t)array + 1) return -1;
    return array + 1;
}
int hs_json_next(const hs_json *j, int parent, int child) {
    if (!valid(j, parent) || !valid(j, child)) return -1;
    uint32_t next = j->tokens[child].next;
    return next < j->tokens[parent].next ? (int)next : -1;
}
bool hs_json_bool(const hs_json *j, int t, bool *v) {
    if (!valid(j, t) || j->tokens[t].type != HS_JSON_BOOL || !v) return false;
    *v = j->data[j->tokens[t].start] == 't'; return true;
}
bool hs_json_number(const hs_json *j, int t, double *v) {
    if (!valid(j, t) || j->tokens[t].type != HS_JSON_NUMBER || !v) return false;
    char number[64]; bool cut; if (!hs_json_text(j, t, number, sizeof(number), &cut) || cut) return false;
    char *end; double result = strtod(number, &end); if (*end || !isfinite(result)) return false;
    *v = result; return true;
}
bool hs_cookie_parse(const char *header, char out[65]) {
    if (!header || !out) return false;
    while (*header == ' ' || *header == '\t') ++header;
    if (strncmp(header, "hs_sid=", 7)) return false;
    header += 7;
    for (unsigned i = 0; i < 64; ++i) if (!header[i] || hex(header[i]) < 0) return false;
    if (header[64] && header[64] != ';') return false;
    memcpy(out, header, 64); out[64] = 0; return true;
}
bool hs_url_encode(const char *in, char *out, size_t cap) {
    static const char h[] = "0123456789ABCDEF"; size_t used = 0;
    if (!in || !out || !cap) return false;
    for (size_t i = 0; in[i]; ++i) {
        unsigned char c = (unsigned char)in[i];
        bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-_.~", c);
        size_t n = safe ? 1 : 3; if (used + n >= cap) { out[0] = 0; return false; }
        if (safe) out[used++] = (char)c;
        else { out[used++] = '%'; out[used++] = h[c >> 4]; out[used++] = h[c & 15]; }
    }
    out[used] = 0; return true;
}
bool hs_json_quote(const char *in, char *out, size_t cap) {
    static const char h[] = "0123456789abcdef"; size_t p = 0, used = 0;
    if (!in || !out || cap < 3) return false;
    size_t n = strlen(in); out[used++] = '"';
    while (p < n) {
        unsigned char c = (unsigned char)in[p]; size_t k = utf8_len(in + p, n - p);
        if (!k) { out[0] = 0; return false; }
        size_t need = c < 0x20 ? 6 : c == '"' || c == '\\' ? 2 : k;
        if (used + need + 2 > cap) { out[0] = 0; return false; }
        if (c < 0x20) { out[used++] = '\\'; out[used++] = 'u'; out[used++] = '0'; out[used++] = '0'; out[used++] = h[c >> 4]; out[used++] = h[c & 15]; }
        else if (c == '"' || c == '\\') { out[used++] = '\\'; out[used++] = (char)c; }
        else { memcpy(out + used, in + p, k); used += k; }
        p += k;
    }
    out[used++] = '"'; out[used] = 0; return true;
}
