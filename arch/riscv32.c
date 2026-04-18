/* arch/riscv32.c — RISC-V 32-bit arch plugin
 *
 * Live debug register layout:
 *   x0-x31 (32 general-purpose registers) + pc = 33 registers.
 *   RSP 'g' returns 33 × 8 hex chars = 264 chars, little-endian.
 *
 *   Notable aliases: x1=ra, x2=sp, x3=gp, x4=tp, x5-x7=t0-t2,
 *   x8=s0/fp, x9=s1, x10-x11=a0-a1, x12-x17=a2-a7, x18-x27=s2-s11,
 *   x28-x31=t3-t6, x32=pc.
 *
 * decode_crash: not yet implemented (no wire firmware port for RISC-V).
 *
 * SPDX-License-Identifier: MIT
 */

#include "arch.h"

#include <stdint.h>
#include <stddef.h>

static int riscv32_decode_crash(const uint8_t *raw, size_t len,
                                 MkdbgCrashReport *out)
{
    (void)raw; (void)len; (void)out;
    return -1;  /* not yet implemented */
}

static const ArchLiveDebug riscv32_live = {
    .nregs      = 33,
    .pc_reg_idx = 32,
    .sp_reg_idx = 2,    /* x2 = sp */
    .fp_reg_idx = -1,   /* no standard fp-chain convention for RISC-V bare-metal */
    .has_thumb2 = 0,
    .reg_names  = {
        "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7",
        "x8", "x9", "x10","x11","x12","x13","x14","x15",
        "x16","x17","x18","x19","x20","x21","x22","x23",
        "x24","x25","x26","x27","x28","x29","x30","x31",
        "pc",
        NULL
    },
};

const MkdbgArch riscv32_arch = {
    .name         = "riscv32",
    .decode_crash = riscv32_decode_crash,
    .live_debug   = &riscv32_live,
};
