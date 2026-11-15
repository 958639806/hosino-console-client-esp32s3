#include "hosino_chat_display.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
static void check(const char *input, const char *expected)
{
    char output[256];
    const size_t want = strlen(expected);
    assert(hosino_chat_format_display(NULL, 0, input) == want);
    assert(hosino_chat_format_display(output, sizeof(output), input) == want);
    assert(strcmp(output, expected) == 0);
    ++checks;
    for (size_t cap = 1; cap <= want + 2; ++cap) {
        memset(output, 0x55, sizeof(output));
        assert(hosino_chat_format_display(output, cap, input) == want);
        assert(strlen(output) < cap);
        assert(memcmp(output, expected, strlen(output)) == 0);
        assert((unsigned char)output[cap] == 0x55);
        /* A truncated display must not end in an incomplete UTF-8 character. */
        unsigned need = 0;
        for (const unsigned char *p = (const unsigned char *)output; *p; ++p) {
            if (need) { assert((*p & 0xC0) == 0x80); --need; }
            else if (*p >= 0xF0) need = 3;
            else if (*p >= 0xE0) need = 2;
            else if (*p >= 0xC0) need = 1;
            else assert(*p < 0x80);
        }
        assert(need == 0);
        ++checks;
    }
}
int main(void)
{
    check("", "");
    check("ASCII unchanged", "ASCII unchanged");
    check("\xE4\xBD\xA0\xE5\xA5\xBD", "\xE4\xBD\xA0\xE5\xA5\xBD");
    check("\xE2\x9D\xA4\xEF\xB8\x8F", "\xE2\x9D\xA4");
    check("\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB", "\xF0\x9F\x91\xA9+\xF0\x9F\x92\xBB");
    check("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD", "\xF0\x9F\x91\x8D[skin3]");
    check("\xF3\xA0\x81\xA7\xF3\xA0\x81\xBF", "[tag:67][tag:7F]");
    check("\xCC\x80\xCC\x81", "\xCC\x80\xCC\x81");
    check("\xE4", "[?]");
    check("\xE4\xBD", "[?][?]");
    check("\xED\xA0\x80", "[?][?][?]");
    check("\xF4\x90\x80\x80", "[?][?][?][?]");
    check("\xC0\xAF", "[?][?]");
    char untouched = 'x';
    assert(hosino_chat_format_display(&untouched, 0, "abc") == 3 && untouched == 'x');
    assert(hosino_chat_format_display(&untouched, 1, NULL) == 0 && untouched == 0);
    printf("PASS %u display-copy/UTF-8/capacity checks; original source is never modified\n", checks + 2);
    return 0;
}
