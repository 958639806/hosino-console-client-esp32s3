/* Host regression cases for the actual firmware source policy. */
#include <assert.h>
#include <stdio.h>
#include "../../overlay-apps/components/gmf_io_compat/gmf_io/include/hosino_network_policy.h"
int main(void)
{
    const char *base = "https://media.example.test/bili";
    assert(hosino_network_url_allowed("https://media.example.test/bili/v1/streams/a_B-1.flv", base));
    assert(hosino_network_url_allowed("https://media.example.test/v1/streams/a", "https://media.example.test"));
    assert(!hosino_network_url_allowed("http://media.example.test/bili/v1/streams/a", base));
    assert(!hosino_network_url_allowed("https://media.example.test.attacker.test/bili/a", base));
    assert(!hosino_network_url_allowed("https://media.example.test/bilibad/a", base));
    assert(!hosino_network_url_allowed("https://media.example.test/bili/../admin", base));
    assert(!hosino_network_url_allowed("https://media.example.test/bili/v1/..", base));
    assert(!hosino_network_url_allowed("https://media.example.test/bili/v1/%2e%2e", base));
    assert(!hosino_network_url_allowed("https://media.example.test/bili/v1/a?token=secret", base));
    assert(!hosino_network_url_allowed("https://media.example.test/bili/v1/a\r\nHost:evil", base));
    assert(!hosino_network_url_allowed("https://u:p@media.example.test/bili/a", "https://u:p@media.example.test/bili"));
    assert(!hosino_network_url_allowed("https://media.example.test/bili/v1/a", "https://media.example.test/bili/"));
    assert(hosino_network_token_valid("example-device-token-123456"));
    assert(!hosino_network_token_valid("tiny"));
    assert(!hosino_network_token_valid("example-device\r\nX-Evil:1"));
    assert(!hosino_network_token_valid("example-device token-123456"));
    puts("network policy: 16 origin/path/header boundary cases passed");
    return 0;
}
