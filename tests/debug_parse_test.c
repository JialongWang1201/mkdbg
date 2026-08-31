#include "debug_parse.h"

#include <stdio.h>
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
    puts("debug_parse_test: OK");
    return 0;
}
