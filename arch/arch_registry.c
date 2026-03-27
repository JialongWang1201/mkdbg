/* arch/arch_registry.c — static registry of built-in MkdbgArch implementations
 *
 * To register a new arch: declare it extern here and add a pointer to arches[].
 *
 * SPDX-License-Identifier: MIT
 */

#include "arch.h"

#include <string.h>

extern const MkdbgArch cortex_m_arch;
extern const MkdbgArch riscv32_arch;

static const MkdbgArch *arches[] = {
    &cortex_m_arch,
    &riscv32_arch,
    NULL,
};

const MkdbgArch *mkdbg_arch_find(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; arches[i]; i++) {
        if (strcmp(arches[i]->name, name) == 0)
            return arches[i];
    }
    return NULL;
}
