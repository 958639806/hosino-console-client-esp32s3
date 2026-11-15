/* SPDX-License-Identifier: Apache-2.0
 * Tests the production parser/credential/encoding helpers, no HTTP or account. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "console_protocol.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static unsigned checks;
static hs_json_token tokens[512];
static hs_json doc;
static void accept(const char *s) { CHECK(hs_json_parse(&doc, s, strlen(s), tokens, 512) > 0); ++checks; }
static void reject_n(const char *s, size_t n) { CHECK(hs_json_parse(&doc, s, n, tokens, 512) < 0); ++checks; }
static void reject(const char *s) { reject_n(s, strlen(s)); }
static void full_sessions_page(const char *path) {
    FILE *file = fopen(path, "rb"); CHECK(file);
    CHECK(fseek(file, 0, SEEK_END) == 0); long length = ftell(file); CHECK(length > 0);
    CHECK((unsigned long)length > 128u * 1024u); // also reproduces the old byte-cap failure
    CHECK((unsigned long)length <= HS_CONSOLE_RESPONSE_LIMIT_BYTES); rewind(file);
    char *data = malloc((size_t)length + 1); CHECK(data);
    CHECK(fread(data, 1, (size_t)length, file) == (size_t)length); fclose(file); data[length] = 0;
    hs_json_token *full_tokens = calloc(HS_CONSOLE_JSON_TOKEN_LIMIT + 1, sizeof(*full_tokens)); CHECK(full_tokens);
    full_tokens[HS_CONSOLE_JSON_TOKEN_LIMIT].next = 0x12345678;
    hs_json full;
    // Regression: the old 8192-token allocation cannot parse this legal page.
    CHECK(hs_json_parse(&full, data, (size_t)length, full_tokens, 8192) < 0);
    int count = hs_json_parse(&full, data, (size_t)length, full_tokens, HS_CONSOLE_JSON_TOKEN_LIMIT);
    CHECK(count > 12200 && (unsigned)count <= HS_CONSOLE_JSON_TOKEN_LIMIT);
    int sessions = hs_json_member(&full, 0, "sessions"); CHECK(sessions >= 0);
    unsigned rows = 0; char key[256];
    for (int item = hs_json_first(&full, sessions); item >= 0; item = hs_json_next(&full, sessions, item)) {
        bool cut;
        CHECK(hs_json_text(&full, hs_json_member(&full, item, "key"), key, sizeof(key), &cut) && !cut);
        CHECK(strncmp(key, "agent:main:dashboard:", 21) == 0);
        ++rows;
    }
    CHECK(rows == 200);
    printf("PASS: fictitious full sessions page bytes=%lu tokens=%d rows=%u; old8192 rejected, current%u accepted\n",
           (unsigned long)length, count, rows, HS_CONSOLE_JSON_TOKEN_LIMIT);
    // Exact new token-cap boundary and one-token overflow, using the same parser.
    char *stress = malloc(HS_CONSOLE_JSON_TOKEN_LIMIT * 2u + 3u); CHECK(stress);
    size_t used = 0; stress[used++] = '[';
    for (unsigned i = 0; i < HS_CONSOLE_JSON_TOKEN_LIMIT - 1; ++i) {
        if (i) stress[used++] = ','; stress[used++] = '0';
    }
    stress[used++] = ']';
    CHECK(hs_json_parse(&full, stress, used, full_tokens, HS_CONSOLE_JSON_TOKEN_LIMIT) == HS_CONSOLE_JSON_TOKEN_LIMIT);
    stress[used - 1] = ','; stress[used++] = '0'; stress[used++] = ']';
    CHECK(hs_json_parse(&full, stress, used, full_tokens, HS_CONSOLE_JSON_TOKEN_LIMIT) < 0);
    CHECK(full_tokens[HS_CONSOLE_JSON_TOKEN_LIMIT].next == 0x12345678);
    free(stress);
    puts("PASS: exact 16384-token boundary accepted; 16385 tokens rejected without overwriting canary");
    hs_secure_zero(data, (size_t)length + 1); free(data); free(full_tokens);
}
int main(int argc, char **argv) {
    accept("{\"authed\":false,\"isOwner\":false}");
    bool b = true;
    CHECK(hs_json_bool(&doc, hs_json_member(&doc, 0, "authed"), &b) && !b);
    accept("{\"content\":[{\"type\":\"thinking\",\"text\":\"hidden\"},{\"type\":\"text\",\"text\":\"\\u661f\\u91ce\\ud83d\\ude00\"}],\"n\":-1.25e2}");
    int array = hs_json_member(&doc, 0, "content");
    int second = hs_json_next(&doc, array, hs_json_first(&doc, array));
    char out[64]; bool cut;
    CHECK(hs_json_text(&doc, hs_json_member(&doc, second, "text"), out, sizeof(out), &cut));
    CHECK(!cut && !strcmp(out, "\xe6\x98\x9f\xe9\x87\x8e\xf0\x9f\x98\x80"));
    double d; CHECK(hs_json_number(&doc, hs_json_member(&doc, 0, "n"), &d) && d == -125.0);
    CHECK(hs_json_next(&doc, array, second) == -1);
    CHECK(hs_json_member(&doc, -1, "missing") == -1);
    accept("{\"\\u0069d\":\"correct\"}");
    CHECK(hs_json_text(&doc, hs_json_member(&doc, 0, "id"), out, sizeof(out), NULL) && !strcmp(out, "correct"));
    accept("[]"); accept("{}"); accept("null"); accept("0"); accept("-0"); accept("1.0e-9");
    const char *bad[] = {"", " ", "{", "[", "[,]", "[1,]", "{\"a\":1,}", "{a:1}",
        "{\"a\" 1}", "01", "+1", "-", "1.", "1e", "1e+", "NaN", "truex", "{}{}", "\"\\x\"",
        "\"\\ud800\"", "\"\\udc00\"", "\"\\ud800\\u0061\"", "\"unterminated", "\"\n\""};
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) reject(bad[i]);
    const char escaped_nul[] = {'"','\\',0,'"'}; reject_n(escaped_nul, sizeof(escaped_nul));
    const char raw_nul[] = {'"','x',0,'"'}; reject_n(raw_nul, sizeof(raw_nul));
    const char invalid_utf8[][8] = {{'"', (char)0xc0, (char)0xaf, '"'}, {'"',(char)0xed,(char)0xa0,(char)0x80,'"'},
        {'"',(char)0xf4,(char)0x90,(char)0x80,(char)0x80,'"'}, {'"',(char)0xe6,'"'}};
    for (size_t i = 0; i < 4; ++i) reject(invalid_utf8[i]);
    char nested[64]; for (int i = 0; i < 24; ++i) nested[i] = '[';
    nested[24] = '0'; for (int i = 0; i < 24; ++i) nested[25+i] = ']'; nested[49] = 0; accept(nested);
    memmove(nested+1, nested, 50); nested[0] = '['; nested[50] = ']'; nested[51] = 0; reject(nested);
    struct { unsigned before; hs_json_token t[3]; unsigned after; } guarded = {0x12345678, {{0}}, 0x98765432};
    CHECK(hs_json_parse(&doc, "[1,2]", 5, guarded.t, 3) == 3);
    CHECK(hs_json_parse(&doc, "[1,2]", 5, guarded.t, 2) < 0);
    CHECK(guarded.before == 0x12345678 && guarded.after == 0x98765432);
    char small[5]; CHECK(hs_utf8_copy(small, 4, "\xe6\x98\x9f\xe9\x87\x8e", 6)); CHECK(!strcmp(small, "\xe6\x98\x9f"));
    CHECK(hs_utf8_copy(small, 3, "\xe6\x98\x9f", 3)); CHECK(!small[0]);
    char quoted[128]; const char *original = "a\"b\\c\n\t\xe6\x98\x9f";
    CHECK(hs_json_quote(original, quoted, sizeof(quoted))); accept(quoted);
    CHECK(hs_json_text(&doc, 0, out, sizeof(out), &cut) && !cut && !strcmp(out, original));
    CHECK(!hs_json_quote(original, small, sizeof(small)) && !small[0]);
    CHECK(!hs_json_quote("\xc0\xaf", quoted, sizeof(quoted)));
    accept("\"\\u661f\\u91ce\""); CHECK(hs_json_text(&doc, 0, small, 4, &cut) && cut && !strcmp(small, "\xe6\x98\x9f"));
    char url[128]; CHECK(hs_url_encode("a&b=/ #?\xe6\x98\x9f", url, sizeof(url)));
    CHECK(!strcmp(url, "a%26b%3D%2F%20%23%3F%E6%98%9F"));
    CHECK(!hs_url_encode("/", small, 3) && !small[0]); CHECK(hs_url_encode("/", small, 4) && !strcmp(small, "%2F"));
    char cookie[65], header[160]; memset(header, 'a', sizeof(header));
    memcpy(header, "hs_sid=", 7); header[71] = ';'; memcpy(header+72, " Secure", 8);
    CHECK(hs_cookie_parse(header, cookie) && strlen(cookie) == 64);
    header[71] = 'a'; header[72] = ';'; header[73] = 0; CHECK(!hs_cookie_parse(header, cookie));
    const char *short_cookies[] = {"", "h", "hs_sid=", "hs_sid=a", "hs_sid=;", "other=1; hs_sid=a"};
    for (size_t i = 0; i < sizeof(short_cookies)/sizeof(short_cookies[0]); ++i) CHECK(!hs_cookie_parse(short_cookies[i], cookie));
    header[71] = ';'; header[72] = 0; header[23] = 'g'; CHECK(!hs_cookie_parse(header, cookie));
    char secret[16] = "not-a-secret"; hs_secure_zero(secret, sizeof(secret));
    for (unsigned i=0; i<sizeof(secret); ++i) CHECK(secret[i] == 0);
    CHECK(hs_failed_post_is_uncertain(true, false, true, 200));  // malformed/truncated 2xx
    CHECK(hs_failed_post_is_uncertain(true, false, true, 502));
    CHECK(hs_failed_post_is_uncertain(true, false, true, 307));
    CHECK(hs_failed_post_is_uncertain(true, true, true, 0));
    CHECK(hs_failed_post_is_uncertain(true, false, false, 0));
    CHECK(!hs_failed_post_is_uncertain(false, true, false, 0));
    CHECK(!hs_failed_post_is_uncertain(true, false, true, 401));
    CHECK(!hs_failed_post_is_uncertain(true, false, true, 423));
    // Deterministic bounded fuzz: real parser must preserve supplied token bounds.
    unsigned seed = 1234567; char input[256];
    for (unsigned trial=0; trial<10000; ++trial) {
        seed=seed*1664525u+1013904223u; size_t n=seed%sizeof(input);
        for (size_t i=0;i<n;++i) { seed=seed*1664525u+1013904223u; input[i]=(char)(seed>>24); }
        (void)hs_json_parse(&doc,input,n,guarded.t,3);
        CHECK(guarded.before==0x12345678 && guarded.after==0x98765432);
    }
    printf("PASS: %u JSON cases; UTF-8/Unicode, depth/token caps, cookie, URL/JSON encoding, zeroization, 10000 bounded fuzz inputs\n", checks);
    if (argc > 1) full_sessions_page(argv[1]);
    return 0;
}
