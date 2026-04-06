/* tests/dwarf_test.c — unit tests for dwarf_sym_to_addr() and dwarf_addr_to_sym()
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

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("dwarf_test\n");

    /* Write synthetic ELF to a temp file */
    char tmppath[] = "/tmp/dwarf_test_XXXXXX.elf";
    int fd = mkstemps(tmppath, 4);
    if (fd < 0) { perror("mkstemps"); return 2; }

    uint8_t elf[ELF_SIZE];
    build_test_elf(elf);

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
