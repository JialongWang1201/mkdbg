#include "debug_parse.h"

#include <stdio.h>
#include <limits.h>
#include <stdlib.h>

static void expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

int main(void)
{
    const char *valid[] = {"0", "0x08001234", "ffffffff", "0XFFFFFFFF", "ABCDEF"};
    const uint32_t values[] = {0, 0x08001234, UINT32_MAX, UINT32_MAX, 0xabcdef};
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        uint32_t value = 123;
        expect(debug_parse_address(valid[i], &value) == 0 && value == values[i],
               "valid hexadecimal address is preserved");
    }
    const char *invalid[] = {NULL, "", "0x", "-1", "+1", " 1", "1 ",
        "0x100000000", "0xffffffffffffffffffff", "0x08001000junk", "garbage", "１"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        uint32_t value = 0xdeadbeef;
        expect(debug_parse_address(invalid[i], &value) != 0 && value == 0xdeadbeef,
               "invalid address is rejected without modifying output");
    }
    expect(debug_parse_address("0", NULL) != 0, "NULL output is rejected");

    int id;
    expect(debug_parse_id("1", &id) == 0 && id == 1, "first ID parses");
    char maximum[32];
    snprintf(maximum, sizeof(maximum), "%d", INT_MAX);
    expect(debug_parse_id(maximum, &id) == 0 && id == INT_MAX, "largest int ID parses");
    const char *bad_ids[] = {NULL, "", "0", "-1", "+1", " 1", "1 ", "1junk",
        "1 2", "0x1", "2147483648", "999999999999999999999999"};
    for (size_t i = 0; i < sizeof(bad_ids) / sizeof(bad_ids[0]); i++) {
        id = 42;
        expect(debug_parse_id(bad_ids[i], &id) != 0 && id == 42,
               "malformed/out-of-range ID cannot select an existing record");
    }
    size_t length;
    expect(debug_parse_length("256", 256, &length) == 0 && length == 256,
           "maximum memory read length parses");
    expect(debug_parse_length("0x10", 256, &length) == 0 && length == 16,
           "hexadecimal length remains supported");
    expect(debug_parse_length("010", 256, &length) == 0 && length == 8,
           "octal length remains supported");
    const char *bad_lengths[] = {NULL, "", "0", "257", "-1", "+1", "1junk", "1 2",
        "0x100000000", "999999999999999999999999"};
    for (size_t i = 0; i < sizeof(bad_lengths) / sizeof(bad_lengths[0]); i++) {
        length = 42;
        expect(debug_parse_length(bad_lengths[i], 256, &length) != 0 && length == 42,
               "invalid lengths are rejected rather than replaced with defaults");
    }
    expect(debug_parse_id("1", NULL) != 0 && debug_parse_length("1", 1, NULL) != 0,
           "numeric parsers reject NULL outputs");
    puts("debug_parse_test: OK");
    return 0;
}
