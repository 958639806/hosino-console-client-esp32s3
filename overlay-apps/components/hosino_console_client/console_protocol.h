/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define HS_CONSOLE_RESPONSE_LIMIT_BYTES (256u * 1024u)
#define HS_CONSOLE_JSON_TOKEN_LIMIT 16384u
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { HS_JSON_OBJECT, HS_JSON_ARRAY, HS_JSON_STRING, HS_JSON_NUMBER,
               HS_JSON_BOOL, HS_JSON_NULL } hs_json_type;
typedef struct { uint32_t start, end, next; hs_json_type type; } hs_json_token;
typedef struct {
    const char *data;
    size_t length;
    hs_json_token *tokens;
    size_t count;
} hs_json;
// Strict bounded JSON parser, maximum nesting 24, no allocation/global hooks.
int hs_json_parse(hs_json *json, const char *data, size_t length,
                  hs_json_token *tokens, size_t capacity);
int hs_json_member(const hs_json *json, int object, const char *name);
int hs_json_first(const hs_json *json, int array);
int hs_json_next(const hs_json *json, int parent, int child);
bool hs_json_text(const hs_json *json, int token, char *out, size_t capacity, bool *truncated);
bool hs_json_bool(const hs_json *json, int token, bool *value);
bool hs_json_number(const hs_json *json, int token, double *value);
bool hs_cookie_parse(const char *header_value, char out[65]);
// Percent-encodes complete UTF-8 byte sequence for a URL query. No truncation.
bool hs_url_encode(const char *in, char *out, size_t capacity);
// JSON quotes a C UTF-8 string. No truncation; rejects invalid UTF-8.
bool hs_json_quote(const char *in, char *out, size_t capacity);
// Copies valid UTF-8 without splitting a character. Replaces malformed bytes.
bool hs_utf8_copy(char *out, size_t capacity, const char *in, size_t length);
void hs_secure_zero(void *data, size_t size);
// Called only for a failed/unreadable POST response, never for valid success.
bool hs_failed_post_is_uncertain(bool headers_sent, bool cancelled, bool transport_ok, int http_status);
#ifdef __cplusplus
}
#endif
