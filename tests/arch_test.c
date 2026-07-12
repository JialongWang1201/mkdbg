/* tests/arch_test.c — arch plugin registry smoke tests
 *
 * Verifies that mkdbg_arch_find() returns the expected result for registered
 * and unknown architecture names.
 *
 * SPDX-License-Identifier: MIT
 */

#include "arch.h"

#include <stdio.h>
#include <string.h>

static int tests_run    = 0;
static int tests_passed = 0;

#define CHECK(cond, label) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
        printf("  PASS  %s\n", label); \
    } else { \
        printf("  FAIL  %s\n", label); \
    } \
} while (0)

int main(void)
{
    printf("arch_test\n");

    /* Known arch must be found */
    const MkdbgArch *cm = mkdbg_arch_find("cortex-m");
    CHECK(cm != NULL, "mkdbg_arch_find(\"cortex-m\") != NULL");
    CHECK(cm != NULL && strcmp(cm->name, "cortex-m") == 0,
          "arch->name == \"cortex-m\"");
    CHECK(cm != NULL && cm->decode_crash != NULL,
          "arch->decode_crash is set");

    /* Unknown arch must return NULL */
    CHECK(mkdbg_arch_find("unknown") == NULL,
          "mkdbg_arch_find(\"unknown\") == NULL");
    CHECK(mkdbg_arch_find("esp32") == NULL,
          "mkdbg_arch_find(\"esp32\") == NULL");
    CHECK(mkdbg_arch_find(NULL) == NULL,
          "mkdbg_arch_find(NULL) == NULL");

    /* decode_crash: reject undersized payload */
    if (cm && cm->decode_crash) {
        MkdbgCrashReport report;
        memset(&report, 0, sizeof(report));
        int rc = cm->decode_crash(NULL, 0, &report);
        CHECK(rc == -1, "decode_crash(NULL, 0) returns -1");

        /* Minimal valid payload: 76 bytes of zeros (signal=0 → timeout) */
        unsigned char raw[76];
        memset(raw, 0, sizeof(raw));
        rc = cm->decode_crash(raw, sizeof(raw), &report);
        CHECK(rc == 0, "decode_crash(zeros, 76) returns 0");
        CHECK(report.timeout == 1,
              "zero payload: timeout flag set (halt_signal=0)");
    }

    /* cortex-m live_debug descriptor */
    CHECK(cm != NULL && cm->live_debug != NULL,
          "cortex-m has live_debug descriptor");
    CHECK(cm != NULL && cm->live_debug != NULL && cm->live_debug->nregs == 50,
          "cortex-m live_debug->nregs == 50");
    CHECK(cm != NULL && cm->live_debug != NULL && cm->live_debug->required_nregs == 17,
          "cortex-m live_debug->required_nregs == 17");
    CHECK(cm != NULL && cm->live_debug != NULL && cm->live_debug->pc_reg_idx == 15,
          "cortex-m live_debug->pc_reg_idx == 15");

    /* riscv32 arch registration and live_debug */
    const MkdbgArch *rv = mkdbg_arch_find("riscv32");
    CHECK(rv != NULL, "mkdbg_arch_find(\"riscv32\") != NULL");
    CHECK(rv != NULL && strcmp(rv->name, "riscv32") == 0,
          "riscv32 arch->name == \"riscv32\"");
    CHECK(rv != NULL && rv->live_debug != NULL,
          "riscv32 has live_debug descriptor");
    CHECK(rv != NULL && rv->live_debug != NULL && rv->live_debug->nregs == 33,
          "riscv32 live_debug->nregs == 33");
    CHECK(rv != NULL && rv->live_debug != NULL && rv->live_debug->pc_reg_idx == 32,
          "riscv32 live_debug->pc_reg_idx == 32");
    if (rv && rv->decode_crash) {
        MkdbgCrashReport rv_report;
        unsigned char raw[140];
        memset(raw, 0, sizeof(raw));
        raw[0] = 5;          /* halt_signal */
        raw[4 + 31 * 4] = 31; /* x31 */
        raw[4 + 32 * 4] = 0x78;
        raw[4 + 32 * 4 + 1] = 0x56;
        raw[4 + 32 * 4 + 2] = 0x34;
        raw[4 + 32 * 4 + 3] = 0x12;
        raw[136] = 2;        /* mcause */
        memset(&rv_report, 0, sizeof(rv_report));
        int rc = rv->decode_crash(raw, sizeof(raw), &rv_report);
        CHECK(rc == 0, "riscv32 decode_crash(valid payload) returns 0");
        CHECK(strcmp(rv_report.regs[31], "0x0000001f") == 0,
              "riscv32 decode_crash captures x31");
        CHECK(strcmp(rv_report.regs[32], "0x12345678") == 0,
              "riscv32 decode_crash captures pc");
        CHECK(strcmp(rv_report.cfsr, "0x00000002") == 0,
              "riscv32 decode_crash captures mcause");
    }

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
