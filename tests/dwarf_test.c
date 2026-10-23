/* tests/dwarf_test.c — unit tests for DWARF source ranges and ELF symbols
 *
 * Builds a minimal ELF32 LE binary in memory (written to a temp file),
 * calls dwarf_open(), then exercises:
 *
 * dwarf_sym_to_addr():
 *   1. STT_FUNC symbol "main" found → returns 0, correct address
 *   2. STT_OBJECT symbol "pxCurrentTCB" found → returns 0 (FreeRTOS TCB ptr)
 *   3. STT_NOTYPE "foo" not found → -1
 *   4. Unknown name returns -1
 *   5. NULL dbi returns -1
 *   6. NULL name returns -1
 *   7. NULL addr pointer returns -1
 *
 * dwarf_addr_to_sym():
 *   8.  NULL dbi returns -1
 *   9.  NULL out_name returns -1
 *   10. exact start of "main" (ADDR_MAIN + 0) → name="main", offset=0
 *   11. mid-function (ADDR_MAIN + 0x20) → name="main", offset=0x20
 *   12. last byte (ADDR_MAIN + 63) → name="main", offset=63
 *   13. past interval end (ADDR_MAIN + 64) → nearest-symbol, offset=64
 *   14. before all symbols (ADDR_MAIN - 1) → returns -1
 *
 * SPDX-License-Identifier: MIT
 */

#include "dwarf.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── test harness ────────────────────────────────────────────────────────── */

static int s_run = 0, s_pass = 0;

#define CHECK(cond, label) do { \
    s_run++; \
    if (cond) { s_pass++; printf("  PASS  %s\n", (label)); } \
    else             printf("  FAIL  %s\n", (label)); \
} while (0)

/* ── little-endian writers ───────────────────────────────────────────────── */

static void pu16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

static void pu32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* ── ELF layout constants ─────────────────────────────────────────────────
 *
 * Sections (5 total, indexed 0-4):
 *   0  NULL
 *   1  .shstrtab   offset=252  size=39
 *   2  .debug_line offset=291  size=32  (minimal DWARF3 CU, empty program)
 *   3  .symtab     offset=323  size=64  (4 Elf32_Sym entries)
 *   4  .strtab     offset=387  size=23
 *
 * Total ELF file size = 410 bytes.
 */

#define ELF_SHOFF       52u    /* section headers immediately after ELF header */
#define SHSTRTAB_OFF   252u
#define DEBUGLINE_OFF  291u
#define SYMTAB_OFF     323u
#define STRTAB_OFF     387u
#define ELF_SIZE       410u

/* Offsets within .shstrtab */
#define SHSTR_SHSTRTAB   1u
#define SHSTR_DEBUGLINE 11u
#define SHSTR_SYMTAB    23u
#define SHSTR_STRTAB    31u

/* .strtab symbol name offsets */
#define SYM_NONE         0u   /* "" (STN_UNDEF) */
#define SYM_MAIN         1u   /* "main" */
#define SYM_FOO          6u   /* "foo"  (STT_NOTYPE — should be ignored) */
#define SYM_PXCURRENT   10u   /* "pxCurrentTCB" */

/* Symbol addresses baked into .symtab */
#define ADDR_MAIN        0x00001000u
#define ADDR_FOO         0x20000000u
#define ADDR_PXCURRENT   0x20003F00u

/* ── build_test_elf ───────────────────────────────────────────────────────── */

static void build_test_elf(uint8_t *buf)
{
    memset(buf, 0, ELF_SIZE);

    /* ── ELF header (52 bytes) ─────────────────────────────────────────── */
    uint8_t *h = buf;
    h[0] = 0x7f; h[1] = 'E'; h[2] = 'L'; h[3] = 'F';
    h[4] = 1;           /* EI_CLASS = ELFCLASS32  */
    h[5] = 1;           /* EI_DATA  = ELFDATA2LSB */
    h[6] = 1;           /* EI_VERSION */
    pu16(h + 16, 2);    /* e_type    = ET_EXEC */
    pu16(h + 18, 0x28); /* e_machine = EM_ARM  */
    pu32(h + 20, 1);    /* e_version */
    pu32(h + 32, ELF_SHOFF);
    pu16(h + 40, 52);   /* e_ehsize    */
    pu16(h + 42, 32);   /* e_phentsize */
    pu16(h + 44, 0);    /* e_phnum     */
    pu16(h + 46, 40);   /* e_shentsize */
    pu16(h + 48, 5);    /* e_shnum     */
    pu16(h + 50, 1);    /* e_shstrndx  = section 1 (.shstrtab) */

    /* ── Section headers (5 × 40 bytes) ───────────────────────────────── */
    /* Elf32_Shdr fields (each 4 bytes unless noted):
     *   sh_name sh_type sh_flags sh_addr sh_offset sh_size
     *   sh_link sh_info sh_addralign sh_entsize
     */
#define SHDR(idx, nm, ty, fl, ad, off, sz, lk, info, al, es) do { \
    uint8_t *s = buf + ELF_SHOFF + (idx) * 40u; \
    pu32(s+ 0, (nm));  pu32(s+ 4, (ty));  pu32(s+ 8, (fl)); \
    pu32(s+12, (ad));  pu32(s+16, (off)); pu32(s+20, (sz)); \
    pu32(s+24, (lk));  pu32(s+28, (info));pu32(s+32, (al)); \
    pu32(s+36, (es)); } while(0)

    SHDR(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);                           /* NULL  */
    SHDR(1, SHSTR_SHSTRTAB,  3, 0, 0, SHSTRTAB_OFF,  39, 0, 0, 1, 0);/* .shstrtab  */
    SHDR(2, SHSTR_DEBUGLINE, 1, 0, 0, DEBUGLINE_OFF, 32, 0, 0, 1, 0);/* .debug_line */
    SHDR(3, SHSTR_SYMTAB,    2, 0, 0, SYMTAB_OFF,    64, 4, 1, 4,16);/* .symtab   */
    SHDR(4, SHSTR_STRTAB,    3, 0, 0, STRTAB_OFF,    23, 0, 0, 1, 0);/* .strtab   */
#undef SHDR

    /* ── .shstrtab ─────────────────────────────────────────────────────── */
    uint8_t *ss = buf + SHSTRTAB_OFF;
    /* [0]=NUL [1..10]=".shstrtab\0" [11..22]=".debug_line\0"
     * [23..30]=".symtab\0" [31..38]=".strtab\0" */
    memcpy(ss +  1, ".shstrtab",  9); ss[10] = '\0';
    memcpy(ss + 11, ".debug_line",11); ss[22] = '\0';
    memcpy(ss + 23, ".symtab",    7); ss[30] = '\0';
    memcpy(ss + 31, ".strtab",    7); ss[38] = '\0';

    /* ── .debug_line (32 bytes) — minimal DWARF3 empty CU ──────────────── */
    uint8_t *dl = buf + DEBUGLINE_OFF;
    pu32(dl,     28);   /* unit_length (everything after this field) */
    pu16(dl + 4,  3);   /* version = DWARF3 */
    pu32(dl + 6, 19);   /* header_length */
    /* header body (19 bytes): */
    dl[10] = 2;   /* minimum_instruction_length */
    dl[11] = 1;   /* default_is_stmt */
    dl[12] = 0xfb;/* line_base = -5 (int8) */
    dl[13] = 14;  /* line_range */
    dl[14] = 13;  /* opcode_base */
    /* standard_opcode_lengths[12]: */
    static const uint8_t oplen[12] = {0,1,1,1,1,0,0,0,1,0,0,1};
    memcpy(dl + 15, oplen, 12);
    dl[27] = 0;   /* include_directories: empty (terminating NUL) */
    dl[28] = 0;   /* file_names: empty (terminating NUL) */
    /* line program: DW_LNE_end_sequence */
    dl[29] = 0x00; /* extended opcode marker */
    dl[30] = 0x01; /* length = 1 */
    dl[31] = 0x01; /* DW_LNE_end_sequence */

    /* ── .strtab ────────────────────────────────────────────────────────── */
    uint8_t *st = buf + STRTAB_OFF;
    /* [0]='\0' [1]="main\0" [6]="foo\0" [10]="pxCurrentTCB\0" */
    memcpy(st +  1, "main",        4); st[5]  = '\0';
    memcpy(st +  6, "foo",         3); st[9]  = '\0';
    memcpy(st + 10, "pxCurrentTCB",12); st[22] = '\0';

    /* ── .symtab (4 × Elf32_Sym) ─────────────────────────────────────────
     * Elf32_Sym: st_name(4) st_value(4) st_size(4) st_info(1) st_other(1) st_shndx(2)
     */
#define SYM(idx, nm, val, sz, info) do { \
    uint8_t *e = buf + SYMTAB_OFF + (idx) * 16u; \
    pu32(e+0, (nm)); pu32(e+4, (val)); pu32(e+8, (sz)); \
    e[12] = (info); e[13] = 0; pu16(e+14, 1); } while(0)

    /* Entry 0: STN_UNDEF (all zeros — already zeroed by memset) */
    /* Entry 1: "main" STT_FUNC(2), size=64 (needed for addr_to_sym interval tests) */
    SYM(1, SYM_MAIN,      ADDR_MAIN,     64, 0x02);
    /* Entry 2: "foo" STT_NOTYPE(0) — must be skipped */
    SYM(2, SYM_FOO,       ADDR_FOO,       4, 0x00);
    /* Entry 3: "pxCurrentTCB" STT_OBJECT(1) — FreeRTOS global pointer */
    SYM(3, SYM_PXCURRENT, ADDR_PXCURRENT, 4, 0x01);
#undef SYM
}

typedef struct {
    const char *file;
    const uint8_t *program;
    size_t program_size;
} LineUnit;

/* Append real line programs to the existing ELF fixture, leaving its symbol
 * sections intact. Each unit has one file and one operation per instruction. */
static DwarfDBI *open_line_units(uint16_t version, const LineUnit *units, size_t count)
{
    uint8_t elf[2048];
    size_t offset = ELF_SIZE;
    build_test_elf(elf);
    for (size_t i = 0; i < count; i++) {
        size_t file_size = strlen(units[i].file) + 1;
        size_t header_size = (version == 4 ? 6u : 5u) + 12u + 1u + file_size + 4u;
        size_t unit_size = 10u + header_size + units[i].program_size;
        if (unit_size > sizeof(elf) - offset) {
            fprintf(stderr, "line fixture exceeds ELF buffer\n");
            exit(2);
        }
        uint8_t *cu = elf + offset;
        pu32(cu, (uint32_t)(unit_size - 4u));
        pu16(cu + 4, version);
        pu32(cu + 6, (uint32_t)header_size);
        uint8_t *p = cu + 10;
        *p++ = 1;                          /* minimum_instruction_length */
        if (version == 4) *p++ = 1;         /* maximum_operations_per_instruction */
        *p++ = 1; *p++ = 0xfb; *p++ = 14; *p++ = 13;
        static const uint8_t lengths[12] = {0,1,1,1,1,0,0,0,1,0,0,1};
        memcpy(p, lengths, sizeof(lengths)); p += sizeof(lengths);
        *p++ = 0;                          /* no include directories */
        memcpy(p, units[i].file, file_size); p += file_size;
        memset(p, 0, 4); p += 4;           /* dir, mtime, size, file-table end */
        memcpy(p, units[i].program, units[i].program_size);
        offset += unit_size;
    }
    uint8_t *debug_header = elf + ELF_SHOFF + 2u * 40u;
    pu32(debug_header + 16, ELF_SIZE);
    pu32(debug_header + 20, (uint32_t)(offset - ELF_SIZE));
    return dwarf_open_memory(elf, offset);
}

static int location_matches(DwarfDBI *dbi, uint32_t pc, const char *file, int line)
{
    DwarfLocation location = {0};
    return dwarf_pc_to_location(dbi, pc, &location) == 0 &&
           strcmp(location.file, file) == 0 && location.line == line;
}

static void test_line_sequence_bounds(void)
{
    /* Two sequences: [0x1000,0x1008) with a line change at 0x1004,
     * and [0x2000,0x2004). */
    static const uint8_t separated[] = {
        0,5,2,0,0x10,0,0, 1, 2,4, 3,1, 1, 2,4, 0,1,1,
        0,5,2,0,0x20,0,0, 1, 2,4, 0,1,1,
    };
    for (uint16_t version = 2; version <= 4; version++) {
        LineUnit unit = {"main.c", separated, sizeof(separated)};
        DwarfDBI *dbi = open_line_units(version, &unit, 1);
        CHECK(dbi != NULL, "line sequences parse in DWARF 2/3/4");
        CHECK(location_matches(dbi, 0x1000, "main.c", 1), "first instruction maps");
        CHECK(location_matches(dbi, 0x1003, "main.c", 1), "line interval interior maps");
        CHECK(location_matches(dbi, 0x1004, "main.c", 2), "line change maps");
        CHECK(location_matches(dbi, 0x1007, "main.c", 2), "last byte before end maps");
        CHECK(location_matches(dbi, 0x2000, "main.c", 1), "new sequence resets line state");
        const uint32_t gaps[] = {0xfff, 0x1008, 0x1fff, 0x2004, 0x20000000};
        for (size_t i = 0; i < sizeof(gaps) / sizeof(gaps[0]); i++) {
            DwarfLocation location = {"unchanged", -1, -1};
            CHECK(dwarf_pc_to_location(dbi, gaps[i], &location) == 1 &&
                  location.line == -1 && strcmp(location.file, "unchanged") == 0,
                  "sequence ends and gaps return no location without changing output");
        }
        dwarf_close(dbi);
    }

    static const uint8_t wide[] = {0,5,2,0,0x10,0,0, 1, 2,0x80,2, 0,1,1};
    static const uint8_t narrow[] = {0,5,2,0x40,0x10,0,0, 1, 2,4, 0,1,1};
    static const uint8_t adjacent[] = {0,5,2,0,0x11,0,0, 1, 2,4, 0,1,1};
    /* Units need not be in address order. Ending the narrow sequence must
     * not hide the wide sequence's still-valid instructions. */
    LineUnit overlap[] = {{"short.c", narrow, sizeof(narrow)},
                          {"wide.c", wide, sizeof(wide)}};
    DwarfDBI *dbi = open_line_units(3, overlap, 2);
    CHECK(dbi != NULL, "overlapping units parse out of address order");
    CHECK(location_matches(dbi, 0x1040, "short.c", 1), "later starting interval wins overlap");
    CHECK(location_matches(dbi, 0x1044, "wide.c", 1), "lookup falls back to covering interval");
    CHECK(location_matches(dbi, 0x10ff, "wide.c", 1), "wide sequence stays valid after short end");
    DwarfLocation location;
    CHECK(dwarf_pc_to_location(dbi, 0x1100, &location) == 1, "all overlapping sequences have ended");
    dwarf_close(dbi);

    LineUnit neighbors[] = {{"next.c", adjacent, sizeof(adjacent)},
                            {"wide.c", wide, sizeof(wide)}};
    dbi = open_line_units(4, neighbors, 2);
    CHECK(location_matches(dbi, 0x10ff, "wide.c", 1), "adjacent sequence retains previous last byte");
    CHECK(location_matches(dbi, 0x1100, "next.c", 1), "adjacent start maps to new sequence");
    CHECK(dwarf_pc_to_location(dbi, 0x1104, &location) == 1, "adjacent end remains exclusive");
    dwarf_close(dbi);

    static const uint8_t duplicate[] = {
        0,5,2,0,0x10,0,0, 1, 3,1, 1, 2,4, 1, 0,1,1,
    };
    static const uint8_t empty[] = {0,1,1};
    LineUnit duplicates[] = {{"same.c", duplicate, sizeof(duplicate)},
                             {"empty.c", empty, sizeof(empty)}};
    dbi = open_line_units(3, duplicates, 2);
    CHECK(location_matches(dbi, 0x1000, "same.c", 2), "duplicate address selects nonempty row");
    CHECK(location_matches(dbi, 0x1003, "same.c", 2), "empty unit does not hide a location");
    CHECK(dwarf_pc_to_location(dbi, 0x1004, &location) == 1, "row at sequence end has no address range");
    dwarf_close(dbi);

    static const uint8_t top[] = {0,5,2,0xf0,0xff,0xff,0xff, 1, 2,16, 0,1,1};
    LineUnit top_unit = {"top.c", top, sizeof(top)};
    dbi = open_line_units(3, &top_unit, 1);
    CHECK(location_matches(dbi, UINT32_MAX, "top.c", 1), "exclusive end can be one past 32-bit address space");
    dwarf_close(dbi);

    static const uint8_t top64[] = {
        0,5,2,0xf0,0xff,0xff,0xff, 1, 0,9,2,0,0,0,0,1,0,0,0, 0,1,1,
    };
    LineUnit top64_unit = {"top.c", top64, sizeof(top64)};
    dbi = open_line_units(3, &top64_unit, 1);
    CHECK(location_matches(dbi, UINT32_MAX, "top.c", 1), "8-byte end address is not truncated");
    dwarf_close(dbi);

    static const uint8_t special[] = {0,5,2,0,0x10,0,0, 18,33, 9,3,0, 8, 0,1,1};
    LineUnit special_unit = {"special.c", special, sizeof(special)};
    dbi = open_line_units(4, &special_unit, 1);
    CHECK(location_matches(dbi, 0x1000, "special.c", 1), "special opcode emits first interval");
    CHECK(location_matches(dbi, 0x1014, "special.c", 2), "special and fixed/constant advances bound interval");
    CHECK(dwarf_pc_to_location(dbi, 0x1015, &location) == 1, "special opcode sequence end is excluded");
    dwarf_close(dbi);

    static const uint8_t truncated[] = {0,5,2,0,0x10,0,0, 1};
    static const uint8_t backwards[] = {
        0,5,2,4,0x10,0,0, 1, 0,5,2,0,0x10,0,0, 0,1,1,
    };
    static const uint8_t overflow[] = {
        0,5,2,0,0x10,0,0, 1, 2,0xff,0xff,0xff,0xff,0x7f, 0,1,1,
    };
    LineUnit invalid[] = {{"bad.c", truncated, sizeof(truncated)},
                          {"bad.c", backwards, sizeof(backwards)},
                          {"bad.c", overflow, sizeof(overflow)}};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        dbi = open_line_units(3, &invalid[i], 1);
        CHECK(dbi == NULL, "unterminated, backwards or overflowing sequence is rejected");
        dwarf_close(dbi);
    }
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("dwarf_test\n");
    test_line_sequence_bounds();

    /* Write synthetic ELF to a temp file */
    char tmppath[] = "/tmp/dwarf_test_XXXXXX.elf";
    int fd = mkstemps(tmppath, 4);
    if (fd < 0) { perror("mkstemps"); return 2; }

    uint8_t elf[ELF_SIZE];
    build_test_elf(elf);

    /* Malformed fixture: section-name table has no terminating NUL. */
    {
        uint8_t malformed[ELF_SIZE];
        memcpy(malformed, elf, sizeof(malformed));
        uint8_t *shstr = malformed + ELF_SHOFF + 40u;
        pu32(shstr + 16, ELF_SIZE - 8u);
        pu32(shstr + 20, 8u);
        memset(malformed + ELF_SIZE - 8u, 'X', 8u);
        for (unsigned int i = 0; i < 5; i++)
            pu32(malformed + ELF_SHOFF + i * 40u, 0u);
        CHECK(dwarf_open_memory(malformed, sizeof(malformed)) == NULL,
              "unterminated section name is rejected");
    }

    /* Malformed fixture: extended-opcode length is an overlong ULEB128. */
    {
        uint8_t malformed[ELF_SIZE];
        memcpy(malformed, elf, sizeof(malformed));
        uint8_t *debug_hdr = malformed + ELF_SHOFF + 2u * 40u;
        uint8_t *debug_line = malformed + DEBUGLINE_OFF;
        pu32(debug_hdr + 20, 48u);
        pu32(debug_line, 44u);
        debug_line[29] = 0;
        memset(debug_line + 30, 0x80, 12u);
        CHECK(dwarf_open_memory(malformed, sizeof(malformed)) == NULL,
              "overlong ULEB128 is rejected");
    }

    /* Malformed fixture: a symbol name is not terminated inside .strtab. */
    {
        uint8_t malformed[ELF_SIZE];
        uint32_t ignored = 0;
        memcpy(malformed, elf, sizeof(malformed));
        malformed[STRTAB_OFF + 22u] = 'X';
        pu32(malformed + SYMTAB_OFF + 16u, SYM_PXCURRENT);
        DwarfDBI *bad = dwarf_open_memory(malformed, sizeof(malformed));
        CHECK(bad != NULL, "ELF with malformed symbol string table opens safely");
        CHECK(bad && dwarf_sym_to_addr(bad, "pxCurrentTCB", &ignored) == -1,
              "unterminated symbol name is rejected");
        dwarf_close(bad);
    }

    ssize_t written = write(fd, elf, ELF_SIZE);
    close(fd);
    if (written != (ssize_t)ELF_SIZE) {
        fprintf(stderr, "write failed\n");
        unlink(tmppath);
        return 2;
    }

    /* ── NULL / error guard tests (no ELF needed) ─────────────────────── */
    CHECK(dwarf_sym_to_addr(NULL, "main", &(uint32_t){0}) == -1,
          "NULL dbi returns -1");

    DwarfDBI *dbi = dwarf_open(tmppath);
    unlink(tmppath);

    if (!dbi) {
        fprintf(stderr, "dwarf_open failed — ELF construction bug?\n");
        return 2;
    }

    CHECK(dwarf_sym_to_addr(dbi, NULL, &(uint32_t){0}) == -1,
          "NULL name returns -1");
    CHECK(dwarf_sym_to_addr(dbi, "main", NULL) == -1,
          "NULL addr returns -1");

    /* ── STT_FUNC lookup ──────────────────────────────────────────────── */
    uint32_t addr = 0;
    int rc = dwarf_sym_to_addr(dbi, "main", &addr);
    CHECK(rc == 0,                    "dwarf_sym_to_addr(\"main\") returns 0");
    CHECK(addr == ADDR_MAIN,          "dwarf_sym_to_addr(\"main\") correct address");

    /* ── STT_OBJECT lookup (FreeRTOS pxCurrentTCB) ───────────────────── */
    addr = 0;
    rc = dwarf_sym_to_addr(dbi, "pxCurrentTCB", &addr);
    CHECK(rc == 0,                    "dwarf_sym_to_addr(\"pxCurrentTCB\") returns 0");
    CHECK(addr == ADDR_PXCURRENT,     "dwarf_sym_to_addr(\"pxCurrentTCB\") correct address");

    /* ── STT_NOTYPE symbol must NOT be found ─────────────────────────── */
    addr = 0;
    rc = dwarf_sym_to_addr(dbi, "foo", &addr);
    CHECK(rc == -1,                   "STT_NOTYPE \"foo\" not found (returns -1)");

    /* ── Unknown symbol returns -1 ───────────────────────────────────── */
    addr = 0;
    rc = dwarf_sym_to_addr(dbi, "nonexistent", &addr);
    CHECK(rc == -1,                   "unknown symbol returns -1");

    /* ── dwarf_addr_to_sym: null-guard tests ─────────────────────────── */
    {
        const char *sym_name   = NULL;
        uint32_t    sym_offset = 0;
        CHECK(dwarf_addr_to_sym(NULL, ADDR_MAIN, &sym_name, &sym_offset) == -1,
              "addr_to_sym: null dbi");
        CHECK(dwarf_addr_to_sym(dbi, ADDR_MAIN, NULL, &sym_offset) == -1,
              "addr_to_sym: null out_name");
    }

    /* ── dwarf_addr_to_sym: address lookups ──────────────────────────── */
    {
        const char *name   = NULL;
        uint32_t    offset = 0;

        /* T1: exact start of main — pass 1 hit, offset must be 0 */
        name = NULL; offset = 0xFFFFFFFFu;
        CHECK(dwarf_addr_to_sym(dbi, ADDR_MAIN, &name, &offset) == 0 &&
              name != NULL && strcmp(name, "main") == 0 && offset == 0u,
              "addr_to_sym: exact start");

        /* T2: mid-function (ADDR_MAIN + 0x20) — pass 1 hit */
        name = NULL; offset = 0xFFFFFFFFu;
        CHECK(dwarf_addr_to_sym(dbi, ADDR_MAIN + 0x20u, &name, &offset) == 0 &&
              name != NULL && strcmp(name, "main") == 0 && offset == 0x20u,
              "addr_to_sym: mid-function");

        /* T3: last byte within interval (ADDR_MAIN + 63) — pass 1 hit */
        name = NULL; offset = 0xFFFFFFFFu;
        CHECK(dwarf_addr_to_sym(dbi, ADDR_MAIN + 63u, &name, &offset) == 0 &&
              name != NULL && strcmp(name, "main") == 0 && offset == 63u,
              "addr_to_sym: last byte");

        /* T4: one past interval end (ADDR_MAIN + 64) — pass 2 nearest-symbol */
        name = NULL; offset = 0xFFFFFFFFu;
        CHECK(dwarf_addr_to_sym(dbi, ADDR_MAIN + 64u, &name, &offset) == 0 &&
              name != NULL && strcmp(name, "main") == 0 && offset == 64u,
              "addr_to_sym: past end");

        /* T5: before all symbols (ADDR_MAIN - 1) — both passes fail */
        name = NULL; offset = 0xFFFFFFFFu;
        CHECK(dwarf_addr_to_sym(dbi, ADDR_MAIN - 1u, &name, &offset) == -1,
              "addr_to_sym: before all syms");
    }

    dwarf_close(dbi);

    printf("\n%d/%d tests passed\n", s_pass, s_run);
    return (s_pass == s_run) ? 0 : 1;
}
