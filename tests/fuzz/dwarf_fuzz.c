#include "dwarf.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    DwarfDBI *dbi = dwarf_open_memory(data, size);
    if (dbi) {
        uint32_t addr;
        uint32_t offset;
        const char *name;
        DwarfLocation location;
        (void)dwarf_sym_to_addr(dbi, "main", &addr);
        (void)dwarf_addr_to_sym(dbi, 0, &name, &offset);
        (void)dwarf_pc_to_location(dbi, 0, &location);
        dwarf_close(dbi);
    }
    return 0;
}

#if defined(__AFL_COMPILER) || defined(MKDBG_AFL_DRIVER)
int main(void)
{
    size_t capacity = 4096;
    size_t size = 0;
    uint8_t *data = malloc(capacity);
    if (!data) return 1;

    for (;;) {
        size_t n;
        if (size == capacity) {
            if (capacity >= 16u * 1024u * 1024u) break;
            capacity *= 2u;
            uint8_t *grown = realloc(data, capacity);
            if (!grown) { free(data); return 1; }
            data = grown;
        }
        n = fread(data + size, 1, capacity - size, stdin);
        size += n;
        if (n == 0) break;
    }
    (void)LLVMFuzzerTestOneInput(data, size);
    free(data);
    return 0;
}
#endif
