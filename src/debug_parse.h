#ifndef DEBUG_PARSE_H
#define DEBUG_PARSE_H

#include <stdint.h>
#include <stddef.h>

/* Hex address with an optional 0x prefix. Reject signs, whitespace, overflow
 * and trailing characters. Output is unchanged on failure. */
int debug_parse_address(const char *text, uint32_t *out);

/* Positive decimal ID, bounded by INT_MAX. Same failure/output rules. */
int debug_parse_id(const char *text, int *out);

/* Positive byte length, using C integer prefixes (decimal, octal, hex). */
int debug_parse_length(const char *text, size_t maximum, size_t *out);

#endif
