#include "debug_parse.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

static int parse_unsigned(const char *text, int base, uint64_t maximum, uint64_t *out)
{
    if (!text || !*text) return -1;
    unsigned char first = (unsigned char)text[0];
    if (!(first >= '0' && first <= '9') &&
        !(base == 16 && ((first >= 'a' && first <= 'f') ||
                         (first >= 'A' && first <= 'F')))) return -1;
    char *end;
    errno = 0;
    unsigned long long value = strtoull(text, &end, base);
    if (errno != 0 || end == text || *end != '\0' || value > maximum) return -1;
    *out = (uint64_t)value;
    return 0;
}

int debug_parse_address(const char *text, uint32_t *out)
{
    uint64_t value;
    if (!out || parse_unsigned(text, 16, UINT32_MAX, &value) != 0) return -1;
    *out = (uint32_t)value;
    return 0;
}

int debug_parse_id(const char *text, int *out)
{
    uint64_t value;
    if (!out || parse_unsigned(text, 10, INT_MAX, &value) != 0 || value == 0)
        return -1;
    *out = (int)value;
    return 0;
}

int debug_parse_length(const char *text, size_t maximum, size_t *out)
{
    uint64_t value;
    if (!out || parse_unsigned(text, 0, maximum, &value) != 0 || value == 0)
        return -1;
    *out = (size_t)value;
    return 0;
}
