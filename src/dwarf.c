/* dwarf.c — ELF/DWARF .debug_line parser for 32-bit little-endian ELF
 *
 * Parses ELF32 section headers, locates .debug_line, runs the DWARF line
 * number state machine (DWARF 2/3/4), and builds a sorted PC→file:line
 * table for fast binary-search lookup.
 *
 * SPDX-License-Identifier: MIT
 */

/* strdup is POSIX, not C11 — request it before any system header */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "dwarf.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── ELF32 e_ident indices ───────────────────────────────────────────────── */
#define EI_CLASS    4
#define EI_DATA     5
#define ELFCLASS32  1
#define ELFDATA2LSB 1   /* little-endian */

/* ── DWARF line standard opcodes ─────────────────────────────────────────── */
#define DW_LNS_copy                1
#define DW_LNS_advance_pc          2
#define DW_LNS_advance_line        3
#define DW_LNS_set_file            4
#define DW_LNS_set_column          5
#define DW_LNS_negate_stmt         6
#define DW_LNS_set_basic_block     7
#define DW_LNS_const_add_pc        8
#define DW_LNS_fixed_advance_pc    9
#define DW_LNS_set_prologue_end    10
#define DW_LNS_set_epilogue_begin  11
#define DW_LNS_set_isa             12

/* ── DWARF extended opcodes ──────────────────────────────────────────────── */
#define DW_LNE_end_sequence  1
#define DW_LNE_set_address   2
#define DW_LNE_define_file   3

/* ── Internal types ──────────────────────────────────────────────────────── */

typedef struct {
    uint32_t address;
    uint32_t file_idx;   /* index into DwarfDBI.files[] */
    int      line;
    int      column;
} DwarfRow;

struct DwarfDBI {
    uint8_t  *elf_data;    /* raw ELF kept alive so dir pointers remain valid */
    size_t    elf_size;
    DwarfRow *rows;
    size_t    nrows;
    size_t    rows_cap;
    char    **files;       /* heap-allocated resolved paths */
    size_t    nfiles;
    size_t    files_cap;
    /* ELF symbol table (.symtab + .strtab) — pointers into elf_data */
    const uint8_t *sym_data;   /* NULL if .symtab absent or stripped */
    size_t         sym_count;  /* number of Elf32_Sym entries (16 bytes each) */
    const uint8_t *str_data;   /* .strtab string table */
    size_t         str_size;
};

/* ── Little-endian readers ───────────────────────────────────────────────── */

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* ── LEB128 decoders ─────────────────────────────────────────────────────── */

static uint64_t uleb128(const uint8_t *p, const uint8_t *end, size_t *out_n)
{
    uint64_t val = 0;
    int shift = 0;
    size_t n = 0;
    while (p + n < end) {
        uint8_t b = p[n++];
        val |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }
    *out_n = n;
    return val;
}

static int64_t sleb128(const uint8_t *p, const uint8_t *end, size_t *out_n)
{
    int64_t val = 0;
    int shift = 0;
    size_t n = 0;
    uint8_t b = 0;
    while (p + n < end) {
        b = p[n++];
        val |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }
    /* Sign-extend */
    if (shift < 64 && (b & 0x40))
        val |= -(int64_t)((uint64_t)1 << shift);
    *out_n = n;
    return val;
}

/* ── DBI row and file management ─────────────────────────────────────────── */

static int dbi_add_file(DwarfDBI *dbi, const char *path)
{
    if (dbi->nfiles >= dbi->files_cap) {
        size_t nc = dbi->files_cap ? dbi->files_cap * 2 : 64;
        char **nf = realloc(dbi->files, nc * sizeof(char *));
        if (!nf) return -1;
        dbi->files     = nf;
        dbi->files_cap = nc;
    }
    dbi->files[dbi->nfiles] = strdup(path);
    if (!dbi->files[dbi->nfiles]) return -1;
    return (int)(dbi->nfiles++);
}

static int dbi_emit_row(DwarfDBI *dbi,
                        uint32_t addr, uint32_t fidx, int line, int col)
{
    if (dbi->nrows >= dbi->rows_cap) {
        size_t nc = dbi->rows_cap ? dbi->rows_cap * 2 : 4096;
        DwarfRow *nr = realloc(dbi->rows, nc * sizeof(DwarfRow));
        if (!nr) return -1;
        dbi->rows     = nr;
        dbi->rows_cap = nc;
    }
    dbi->rows[dbi->nrows].address  = addr;
    dbi->rows[dbi->nrows].file_idx = fidx;
    dbi->rows[dbi->nrows].line     = line;
    dbi->rows[dbi->nrows].column   = col;
    dbi->nrows++;
    return 0;
}

/* ── DWARF line program state machine ────────────────────────────────────── */

/* Parse one compilation unit of .debug_line data.
 * Returns bytes consumed on success (>0), or -1 on a fatal parse error.
 * Skips unsupported DWARF versions gracefully. */
static ptrdiff_t parse_line_cu(DwarfDBI *dbi,
                                const uint8_t *p,
                                const uint8_t *sect_end)
{
    const uint8_t *cu_start = p;

    /* unit_length (32-bit form; 0xffffffff would indicate DWARF64) */
    if (p + 4 > sect_end) return -1;
    uint32_t unit_length = le32(p);
    if (unit_length == 0xffffffffu) return -1;  /* DWARF64 not supported */
    p += 4;
    const uint8_t *cu_end = p + unit_length;
    if (cu_end > sect_end || cu_end < p) return -1;

    /* version */
    if (p + 2 > cu_end) return -1;
    uint16_t version = le16(p); p += 2;
    if (version < 2 || version > 4) {
        /* Unknown version — skip this CU without error */
        return (ptrdiff_t)(cu_end - cu_start);
    }

    /* header_length */
    if (p + 4 > cu_end) return -1;
    uint32_t hdr_len = le32(p); p += 4;
    const uint8_t *prog = p + hdr_len;  /* start of the line program */
    if (prog > cu_end || prog < p) return -1;

    /* minimum_instruction_length */
    if (p >= cu_end) return -1;
    uint8_t min_insn = *p++; if (!min_insn) min_insn = 1;

    /* maximum_operations_per_instruction (DWARF 4 only; always 1 for Cortex-M) */
    if (version >= 4) { if (p >= cu_end) return -1; p++; }

    /* default_is_stmt */
    if (p >= cu_end) return -1;
    uint8_t default_is_stmt = *p++;

    /* line_base (signed) */
    if (p >= cu_end) return -1;
    int8_t line_base = (int8_t)*p++;

    /* line_range */
    if (p >= cu_end) return -1;
    uint8_t line_range = *p++; if (!line_range) return -1;

    /* opcode_base */
    if (p >= cu_end) return -1;
    uint8_t opcode_base = *p++;

    /* standard_opcode_lengths[opcode_base - 1] */
    if (p + (opcode_base - 1) > cu_end) return -1;
    const uint8_t *opcode_lengths = p;
    p += opcode_base - 1;

    /* include_directories: null-terminated strings, final empty entry */
    /* Pointers into elf_data — valid for lifetime of DBI. */
#define MAX_DIRS 512
    const char *dirs[MAX_DIRS];
    size_t ndirs = 0;
    while (p < cu_end && *p) {
        if (ndirs < MAX_DIRS) dirs[ndirs++] = (const char *)p;
        while (p < cu_end && *p) p++;
        p++;  /* consume NUL */
    }
    if (p >= cu_end) return -1;
    p++;  /* consume final empty-string terminator */

    /* file_names: map CU-local (1-based) indices to global DBI file indices */
    uint32_t *file_map   = NULL;
    size_t    nfiles_cu  = 0;
    size_t    fmap_cap   = 0;

    while (p < cu_end && *p) {
        const char *fname = (const char *)p;
        while (p < cu_end && *p) p++;
        p++;  /* consume NUL */

        size_t n;
        uint64_t dir_idx = uleb128(p, cu_end, &n); p += n;
        uleb128(p, cu_end, &n); p += n;  /* mtime — ignored */
        uleb128(p, cu_end, &n); p += n;  /* file size — ignored */

        char path_buf[1024];
        if (dir_idx > 0 && dir_idx <= ndirs)
            snprintf(path_buf, sizeof(path_buf), "%s/%s", dirs[dir_idx - 1], fname);
        else
            snprintf(path_buf, sizeof(path_buf), "%s", fname);

        /* Find or add in global file table */
        uint32_t gidx = (uint32_t)dbi->nfiles;
        for (size_t j = 0; j < dbi->nfiles; j++) {
            if (strcmp(dbi->files[j], path_buf) == 0) { gidx = (uint32_t)j; break; }
        }
        if (gidx == (uint32_t)dbi->nfiles) {
            if (dbi_add_file(dbi, path_buf) < 0) { free(file_map); return -1; }
        }

        /* Grow file_map */
        if (nfiles_cu >= fmap_cap) {
            size_t nc = fmap_cap ? fmap_cap * 2 : 32;
            uint32_t *nm = realloc(file_map, nc * sizeof(uint32_t));
            if (!nm) { free(file_map); return -1; }
            file_map = nm; fmap_cap = nc;
        }
        file_map[nfiles_cu++] = gidx;
    }
    if (p >= cu_end) { free(file_map); return -1; }
    p++;  /* consume final empty-name terminator */

    /* ── Run the line number program ─────────────────────────────────────── */
    p = prog;

    /* State machine registers (initial values per DWARF spec) */
    uint32_t address  = 0;
    uint32_t file_reg = 1;  /* 1-based index into file_map */
    int      line_reg = 1;
    int      col_reg  = 0;
    int      is_stmt  = default_is_stmt;
    (void)is_stmt;  /* not used in output currently */

    while (p < cu_end) {
        uint8_t op = *p++;
        size_t  n;

        if (op == 0) {
            /* Extended opcode: length ULEB128, then 1-byte opcode, then operands */
            uint64_t ext_len = uleb128(p, cu_end, &n); p += n;
            if (!ext_len || p + ext_len > cu_end) { free(file_map); return -1; }

            uint8_t ext_op = *p++;  /* consume opcode byte */

            switch (ext_op) {
            case DW_LNE_end_sequence:
                /* End of a sequence: reset state, do NOT emit a row */
                address = 0; file_reg = 1; line_reg = 1; col_reg = 0;
                is_stmt = default_is_stmt;
                break;

            case DW_LNE_set_address: {
                size_t addr_sz = (size_t)(ext_len - 1);
                if (addr_sz == 4)      address = le32(p);
                else if (addr_sz == 8) address = le32(p);  /* take low 32 bits */
                p += addr_sz;
                break;
            }

            case DW_LNE_define_file:
                /* Rarely used; skip all operand bytes */
                p += ext_len - 1;
                break;

            default:
                p += ext_len - 1;  /* skip unknown extended opcode */
                break;
            }

        } else if (op < opcode_base) {
            /* Standard opcode */
            switch (op) {
            case DW_LNS_copy:
                if (file_reg >= 1 && file_reg <= nfiles_cu)
                    dbi_emit_row(dbi, address,
                                 file_map[file_reg - 1], line_reg, col_reg);
                break;

            case DW_LNS_advance_pc: {
                uint64_t delta = uleb128(p, cu_end, &n); p += n;
                address += (uint32_t)(delta * min_insn);
                break;
            }

            case DW_LNS_advance_line: {
                int64_t delta = sleb128(p, cu_end, &n); p += n;
                line_reg += (int)delta;
                break;
            }

            case DW_LNS_set_file: {
                uint64_t f = uleb128(p, cu_end, &n); p += n;
                file_reg = (uint32_t)f;
                break;
            }

            case DW_LNS_set_column: {
                uint64_t c = uleb128(p, cu_end, &n); p += n;
                col_reg = (int)c;
                break;
            }

            case DW_LNS_negate_stmt:
                is_stmt = !is_stmt;
                break;

            case DW_LNS_set_basic_block:
                break;

            case DW_LNS_const_add_pc: {
                uint8_t adj = (uint8_t)(255 - opcode_base);
                address += (uint32_t)((adj / line_range) * min_insn);
                break;
            }

            case DW_LNS_fixed_advance_pc: {
                if (p + 2 > cu_end) { free(file_map); return -1; }
                uint16_t delta = le16(p); p += 2;
                address += delta;
                break;
            }

            case DW_LNS_set_prologue_end:
            case DW_LNS_set_epilogue_begin:
                break;

            case DW_LNS_set_isa:
                uleb128(p, cu_end, &n); p += n;
                break;

            default:
                /* Unknown standard opcode: skip operands per opcode_lengths */
                if (op >= 1 && (size_t)(op - 1) < (size_t)(opcode_base - 1)) {
                    uint8_t nops = opcode_lengths[op - 1];
                    for (uint8_t i = 0; i < nops; i++) {
                        uleb128(p, cu_end, &n); p += n;
                    }
                }
                break;
            }

        } else {
            /* Special opcode: encodes both PC and line advance + implicit emit */
            uint8_t adj = (uint8_t)(op - opcode_base);
            line_reg += line_base + (int)(adj % line_range);
            address  += (uint32_t)((adj / line_range) * min_insn);
            if (file_reg >= 1 && file_reg <= nfiles_cu)
                dbi_emit_row(dbi, address,
                             file_map[file_reg - 1], line_reg, col_reg);
        }
    }

    free(file_map);
    return (ptrdiff_t)(cu_end - cu_start);
}

/* ── ELF section finder ──────────────────────────────────────────────────── */

/* Returns 0 on success, 1 if section not found, -1 on malformed ELF. */
static int find_elf_section(const uint8_t *data, size_t size,
                             const char *name,
                             const uint8_t **out_p, size_t *out_sz)
{
    if (size < 52) return -1;

    /* ELF magic */
    if (data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F')
        return -1;
    if (data[EI_CLASS] != ELFCLASS32)  return -1;
    if (data[EI_DATA]  != ELFDATA2LSB) return -1;

    uint32_t shoff     = le32(data + 32);
    uint16_t shentsize = le16(data + 46);
    uint16_t shnum     = le16(data + 48);
    uint16_t shstrndx  = le16(data + 50);

    if (shentsize < 40 || shoff == 0 || shnum == 0) return -1;
    if ((uint64_t)shoff + (uint64_t)shnum * shentsize > size) return -1;
    if (shstrndx >= shnum) return -1;

    /* Locate .shstrtab (section name string table) */
    const uint8_t *strtab_hdr = data + shoff + (size_t)shstrndx * shentsize;
    uint32_t strtab_off  = le32(strtab_hdr + 16);
    uint32_t strtab_size = le32(strtab_hdr + 20);
    if ((uint64_t)strtab_off + strtab_size > size) return -1;
    const char *strtab = (const char *)(data + strtab_off);

    for (uint16_t i = 0; i < shnum; i++) {
        const uint8_t *shdr = data + shoff + (size_t)i * shentsize;
        uint32_t sh_name   = le32(shdr + 0);
        uint32_t sh_offset = le32(shdr + 16);
        uint32_t sh_size   = le32(shdr + 20);

        if (sh_name >= strtab_size) continue;
        if (strcmp(strtab + sh_name, name) != 0) continue;

        if ((uint64_t)sh_offset + sh_size > size) return -1;
        *out_p  = data + sh_offset;
        *out_sz = sh_size;
        return 0;
    }
    return 1;  /* not found */
}

/* ── qsort comparator ────────────────────────────────────────────────────── */

static int row_cmp(const void *a, const void *b)
{
    const DwarfRow *ra = (const DwarfRow *)a;
    const DwarfRow *rb = (const DwarfRow *)b;
    if (ra->address < rb->address) return -1;
    if (ra->address > rb->address) return  1;
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

DwarfDBI *dwarf_open(const char *elf_path)
{
    int fd = open(elf_path, O_RDONLY);
    if (fd < 0) { perror(elf_path); return NULL; }

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0) { close(fd); return NULL; }
    size_t size = (size_t)st.st_size;

    uint8_t *data = malloc(size);
    if (!data) { close(fd); return NULL; }

    ssize_t nr = read(fd, data, size);
    close(fd);
    if (nr < 0 || (size_t)nr != size) { free(data); return NULL; }

    /* Find .debug_line */
    const uint8_t *dl_p;
    size_t dl_sz;
    int rc = find_elf_section(data, size, ".debug_line", &dl_p, &dl_sz);
    if (rc != 0) {
        if (rc == 1) fprintf(stderr, "%s: no .debug_line section\n", elf_path);
        free(data);
        return NULL;
    }

    DwarfDBI *dbi = calloc(1, sizeof(DwarfDBI));
    if (!dbi) { free(data); return NULL; }
    dbi->elf_data = data;
    dbi->elf_size = size;

    /* Parse all compilation units */
    const uint8_t *p   = dl_p;
    const uint8_t *end = dl_p + dl_sz;
    while (p < end) {
        ptrdiff_t consumed = parse_line_cu(dbi, p, end);
        if (consumed <= 0) break;
        p += consumed;
    }

    /* Sort by address for binary search */
    if (dbi->nrows > 0)
        qsort(dbi->rows, dbi->nrows, sizeof(DwarfRow), row_cmp);

    /* Optional: ELF symbol table for name→address lookups (e.g. break main).
     * Failure here is non-fatal — dwarf_sym_to_addr() will return -1. */
    const uint8_t *sym_p = NULL; size_t sym_sz = 0;
    const uint8_t *str_p = NULL; size_t str_sz = 0;
    if (find_elf_section(data, size, ".symtab", &sym_p, &sym_sz) == 0 &&
        find_elf_section(data, size, ".strtab", &str_p, &str_sz) == 0 &&
        sym_sz % 16 == 0) {
        dbi->sym_data  = sym_p;
        dbi->sym_count = sym_sz / 16;
        dbi->str_data  = str_p;
        dbi->str_size  = str_sz;
    }

    return dbi;
}

void dwarf_close(DwarfDBI *dbi)
{
    if (!dbi) return;
    for (size_t i = 0; i < dbi->nfiles; i++) free(dbi->files[i]);
    free(dbi->files);
    free(dbi->rows);
    free(dbi->elf_data);
    free(dbi);
}

int dwarf_pc_to_location(DwarfDBI *dbi, uint32_t pc, DwarfLocation *out)
{
    if (!dbi || !dbi->nrows) return 1;

    /* Find the last row with address <= pc (upper-bound search). */
    size_t lo = 0, hi = dbi->nrows;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (dbi->rows[mid].address <= pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0) return 1;  /* all rows have address > pc */

    size_t idx = lo - 1;
    uint32_t fidx = dbi->rows[idx].file_idx;
    if (fidx >= (uint32_t)dbi->nfiles) return -1;

    out->file   = dbi->files[fidx];
    out->line   = dbi->rows[idx].line;
    out->column = dbi->rows[idx].column;
    return 0;
}

int dwarf_sym_to_addr(DwarfDBI *dbi, const char *name, uint32_t *addr)
{
    if (!dbi || !dbi->sym_data || !name || !addr) return -1;
    /* Walk Elf32_Sym entries (16 bytes each):
     *   offset  0: st_name  (uint32_t) — index into .strtab
     *   offset  4: st_value (uint32_t) — symbol address
     *   offset  8: st_size  (uint32_t)
     *   offset 12: st_info  (uint8_t)  — bits[3:0]=type; STT_FUNC=2
     *   offset 13: st_other (uint8_t)
     *   offset 14: st_shndx (uint16_t)
     */
    for (size_t i = 0; i < dbi->sym_count; i++) {
        const uint8_t *e      = dbi->sym_data + i * 16;
        uint32_t       st_nm  = le32(e + 0);
        uint32_t       st_val = le32(e + 4);
        uint8_t        st_inf = e[12];
        uint8_t        stt    = st_inf & 0xfu;
        if (stt != 1u && stt != 2u) continue;          /* skip non-FUNC/OBJECT */
        if (st_nm >= (uint32_t)dbi->str_size) continue;
        if (strcmp((const char *)dbi->str_data + st_nm, name) == 0) {
            *addr = st_val;
            return 0;
        }
    }
    return -1;
}

int dwarf_addr_to_sym(DwarfDBI *dbi, uint32_t pc,
                      const char **out_name, uint32_t *out_offset)
{
    if (!dbi || !dbi->sym_data || !out_name || !out_offset) return -1;

    /* Pass 1: exact interval hit — st_value <= pc < st_value + st_size */
    for (size_t i = 0; i < dbi->sym_count; i++) {
        const uint8_t *e      = dbi->sym_data + i * 16;
        uint32_t       st_val = le32(e + 4);
        uint32_t       st_sz  = le32(e + 8);
        uint8_t        stt    = e[12] & 0xfu;
        if (stt != 2u) continue;                          /* STT_FUNC only */
        if (st_sz == 0) continue;                         /* skip size-0 entries */
        if (pc >= st_val && pc < st_val + st_sz) {
            uint32_t st_nm = le32(e + 0);
            if (st_nm >= (uint32_t)dbi->str_size) continue;
            *out_name   = (const char *)dbi->str_data + st_nm;
            *out_offset = pc - st_val;
            return 0;
        }
    }

    /* Pass 2: nearest symbol — largest st_value <= pc among STT_FUNC entries */
    const uint8_t *best   = NULL;
    uint32_t       best_v = 0;
    for (size_t i = 0; i < dbi->sym_count; i++) {
        const uint8_t *e      = dbi->sym_data + i * 16;
        uint32_t       st_val = le32(e + 4);
        uint8_t        stt    = e[12] & 0xfu;
        if (stt != 2u) continue;                          /* STT_FUNC only */
        if (st_val > pc) continue;
        if (best == NULL || st_val > best_v) {
            best   = e;
            best_v = st_val;
        }
    }
    if (!best) return -1;

    uint32_t st_nm = le32(best + 0);
    if (st_nm >= (uint32_t)dbi->str_size) return -1;
    *out_name   = (const char *)dbi->str_data + st_nm;
    *out_offset = pc - best_v;
    return 0;
}
