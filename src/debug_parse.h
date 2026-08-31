#ifndef DEBUG_PARSE_H
#define DEBUG_PARSE_H

#include <stdint.h>

/* Hex address with an optional 0x prefix. Reject signs, whitespace, overflow
 * and trailing characters. Output is unchanged on failure. */
int debug_parse_address(const char *text, uint32_t *out);

#endif
