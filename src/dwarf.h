/* dwarf.h — ELF/DWARF .debug_line parser (32-bit little-endian ELF)
 *
 * Parses the .debug_line section of an ELF file and exposes a
 * PC → file:line lookup.  Handles DWARF 2, 3, and 4.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef DWARF_H
#define DWARF_H

#include <stdint.h>

/* Source location returned by dwarf_pc_to_location(). */
typedef struct {
    const char *file;   /* pointer into DwarfDBI storage — valid until dwarf_close() */
    int         line;
    int         column;
} DwarfLocation;

/* Opaque handle holding the parsed line-number table. */
typedef struct DwarfDBI DwarfDBI;

/* Open an ELF file and parse its .debug_line section.
 * Returns NULL on error (bad ELF, missing section, OOM). */
DwarfDBI *dwarf_open(const char *elf_path);

/* Free all resources.  After this call, any DwarfLocation.file pointers
 * previously returned are no longer valid. */
void dwarf_close(DwarfDBI *dbi);

/* Find the source location for a target PC address.
 * Returns  0  on success (out is filled in),
 *          1  if no matching row exists for that address,
 *         -1  on internal error. */
int dwarf_pc_to_location(DwarfDBI *dbi, uint32_t pc, DwarfLocation *out);

/* Look up a function symbol by name in the ELF .symtab.
 * Requires a non-stripped ELF (normal for firmware .elf files).
 * Returns  0  and fills *addr on success,
 *         -1  if not found or no symbol table present. */
int dwarf_sym_to_addr(DwarfDBI *dbi, const char *name, uint32_t *addr);

#endif /* DWARF_H */
