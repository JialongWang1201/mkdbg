/* arch/cortex_m.c — Cortex-M crash decoder
 *
 * Extracted from deps/wire/host/wire_crash.c.  All functions in this file
 * are pure: no I/O, no UART, no RSP protocol — only data transformations.
 *
 * Raw crash payload format (little-endian):
 *   bytes  [0..3]   halt_signal    (uint32_t)
 *   bytes  [4..71]  registers[17]  (17 × uint32_t, r0–r12 sp lr pc xpsr)
 *   bytes [72..75]  cfsr           (uint32_t)
 *   bytes [76..]    stack_bytes    (optional; for heuristic backtrace)
 *
 * Minimum payload: 76 bytes.
 *
 * SPDX-License-Identifier: MIT
 */

#include "arch.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ── hex helpers ─────────────────────────────────────────────────────────── */

static int hex2(const char *s)
{
    int hi = 0, lo = 0;
    if      (s[0] >= '0' && s[0] <= '9') hi = s[0] - '0';
    else if (s[0] >= 'a' && s[0] <= 'f') hi = s[0] - 'a' + 10;
    else if (s[0] >= 'A' && s[0] <= 'F') hi = s[0] - 'A' + 10;
    else return -1;
    if      (s[1] >= '0' && s[1] <= '9') lo = s[1] - '0';
    else if (s[1] >= 'a' && s[1] <= 'f') lo = s[1] - 'a' + 10;
    else if (s[1] >= 'A' && s[1] <= 'F') lo = s[1] - 'A' + 10;
    else return -1;
    return (hi << 4) | lo;
}

/* Parse 8 hex chars in little-endian byte order into uint32_t. */
static int parse_le32(const char *hex8, uint32_t *out)
{
    uint32_t val = 0;
    for (int b = 0; b < 4; b++) {
        int byte = hex2(hex8 + b * 2);
        if (byte < 0) return -1;
        val |= (uint32_t)((unsigned)byte << (b * 8));
    }
    *out = val;
    return 0;
}

/* ── halt signal ─────────────────────────────────────────────────────────── */

static const char *signal_name(int sig)
{
    switch (sig) {
    case 1:  return "SIGHUP";
    case 2:  return "SIGINT";
    case 5:  return "SIGTRAP";
    case 6:  return "SIGABRT";
    case 7:  return "SIGBUS";
    case 8:  return "SIGFPE";
    case 11: return "SIGSEGV";
    case 15: return "SIGTERM";
    default: return "UNKNOWN";
    }
}

/*
 * Parse halt response: S<2hex> or T<2hex>...
 * Returns signal number, or -1 on parse error.
 */
static int parse_halt_signal(const char *resp)
{
    if ((resp[0] == 'S' || resp[0] == 'T') && resp[1] && resp[2]) {
        int s = hex2(resp + 1);
        if (s >= 0) return s;
    }
    return -1;
}

/* ── registers ───────────────────────────────────────────────────────────── */

/*
 * Cortex-M GDB register order:
 *   r0–r12 (0–12), sp/r13 (13), lr/r14 (14), pc/r15 (15), xpsr (16)
 * Each register: 4 bytes = 8 hex chars, little-endian.
 * Total: 17 * 8 = 136 hex chars.
 */
#define CM_NREGS 17
static const char *reg_names[CM_NREGS] = {
    "r0","r1","r2","r3","r4","r5","r6","r7",
    "r8","r9","r10","r11","r12","sp","lr","pc","xpsr"
};

static int parse_registers(const char *resp, uint32_t regs[CM_NREGS])
{
    if (strlen(resp) < CM_NREGS * 8u) return -1;
    for (int i = 0; i < CM_NREGS; i++) {
        if (parse_le32(resp + i * 8, &regs[i]) != 0) return -1;
    }
    return 0;
}

/* ── CFSR decode ─────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t    bit;
    const char *name;
    const char *desc;
} CfsrBit;

static const CfsrBit cfsr_bits[] = {
    /* MemManage fault (bits 0–7) */
    { 1u << 0,  "IACCVIOL",    "instruction access MPU violation" },
    { 1u << 1,  "DACCVIOL",    "data access MPU violation" },
    { 1u << 3,  "MUNSTKERR",   "MemManage fault on unstacking" },
    { 1u << 4,  "MSTKERR",     "MemManage fault on stacking" },
    { 1u << 5,  "MLSPERR",     "MemManage fault on lazy FP save" },
    /* BusFault (bits 8–15) */
    { 1u << 8,  "IBUSERR",     "instruction bus error" },
    { 1u << 9,  "PRECISERR",   "precise data bus error" },
    { 1u << 10, "IMPRECISERR", "imprecise data bus error" },
    { 1u << 11, "UNSTKERR",    "bus fault on unstacking" },
    { 1u << 12, "STKERR",      "bus fault on stacking" },
    { 1u << 13, "LSPERR",      "bus fault on lazy FP save" },
    /* UsageFault (bits 16–31) */
    { 1u << 16, "UNDEFINSTR",  "undefined instruction" },
    { 1u << 17, "INVSTATE",    "invalid execution state (EPSR)" },
    { 1u << 18, "INVPC",       "integrity check violation on EXC_RETURN" },
    { 1u << 19, "NOCP",        "coprocessor not available" },
    { 1u << 24, "UNALIGNED",   "unaligned memory access" },
    { 1u << 25, "DIVBYZERO",   "divide by zero" },
};
#define CFSR_NBITS ((int)(sizeof(cfsr_bits) / sizeof(cfsr_bits[0])))

/* Build a human-readable string into out (capacity out_size). */
static void cfsr_decode(uint32_t cfsr, char *out, size_t out_size)
{
    if (out_size == 0) return;
    out[0] = '\0';
    if (cfsr == 0) {
        snprintf(out, out_size, "no fault bits set");
        return;
    }
    size_t pos = 0;
    int    first = 1;
    for (int i = 0; i < CFSR_NBITS; i++) {
        if (!(cfsr & cfsr_bits[i].bit)) continue;
        int n = snprintf(out + pos, out_size - pos,
                         "%s%s — %s",
                         first ? "" : "; ",
                         cfsr_bits[i].name,
                         cfsr_bits[i].desc);
        if (n < 0 || (size_t)n >= out_size - pos) break;
        pos  += (size_t)n;
        first = 0;
    }
}

/* ── heuristic backtrace ─────────────────────────────────────────────────── */

/*
 * Scan stack_bytes for Thumb return addresses in STM32 flash range.
 * Criteria: word-aligned in buffer, value in 0x08000001–0x08FFFFFF, bit0=1.
 * Collect up to max_frames frames in frames[]; returns count.
 */
static int heuristic_backtrace(const uint8_t *stack_bytes, int stack_len,
                                uint32_t *frames, int max_frames)
{
    int count = 0;
    for (int off = 0; off + 3 < stack_len && count < max_frames; off += 4) {
        uint32_t val = (uint32_t)stack_bytes[off]
                     | ((uint32_t)stack_bytes[off+1] << 8)
                     | ((uint32_t)stack_bytes[off+2] << 16)
                     | ((uint32_t)stack_bytes[off+3] << 24);
        /* Thumb bit set, flash range, not the very base address */
        if ((val & 1u) && val >= 0x08000001u && val <= 0x08FFFFFFu)
            frames[count++] = val;
    }
    return count;
}

/* ── decode_crash implementation ─────────────────────────────────────────── */

/*
 * Raw payload layout (all values little-endian):
 *   [0..3]   halt_signal  (uint32_t)
 *   [4..71]  registers    (17 × uint32_t: r0–r12, sp, lr, pc, xpsr)
 *   [72..75] cfsr         (uint32_t)
 *   [76..]   stack_bytes  (optional; used for heuristic backtrace)
 */
#define CM_RAW_HEADER_LEN 76  /* minimum payload without stack bytes */

static uint32_t le32_load(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static int cortex_m_decode_crash(const uint8_t *raw, size_t len,
                                  MkdbgCrashReport *out)
{
    if (!raw || len < CM_RAW_HEADER_LEN || !out) return -1;

    /* halt signal */
    uint32_t sig_val = le32_load(raw + 0);
    out->halt_signal = (int)sig_val;
    out->timeout     = (sig_val == 0) ? 1 : 0;

    /* registers */
    for (int i = 0; i < CM_NREGS; i++) {
        uint32_t rv = le32_load(raw + 4 + i * 4);
        snprintf(out->regs[i], sizeof(out->regs[i]), "0x%08x", rv);
    }

    /* CFSR */
    uint32_t cfsr_val = le32_load(raw + 72);
    snprintf(out->cfsr, sizeof(out->cfsr), "0x%08x", cfsr_val);
    cfsr_decode(cfsr_val, out->cfsr_decoded, sizeof(out->cfsr_decoded));

    /* heuristic backtrace from optional stack bytes */
    if (len > CM_RAW_HEADER_LEN) {
        const uint8_t *stack_bytes = raw + CM_RAW_HEADER_LEN;
        int stack_len = (int)(len - CM_RAW_HEADER_LEN);
        uint32_t frames[WIRE_MAX_FRAMES];
        int nframes = heuristic_backtrace(stack_bytes, stack_len,
                                           frames, WIRE_MAX_FRAMES);
        out->nframes = nframes;
        for (int i = 0; i < nframes; i++) {
            snprintf(out->stack_frames[i], sizeof(out->stack_frames[i]),
                     "0x%08x", frames[i]);
        }
    }

    return 0;
}

/* ── exported arch descriptor ────────────────────────────────────────────── */

static const ArchLiveDebug cortex_m_live = {
    .nregs       = 17,
    .pc_reg_idx  = 15,
    .sp_reg_idx  = 13,
    .reg_names   = {
        "r0","r1","r2","r3","r4","r5","r6","r7",
        "r8","r9","r10","r11","r12","sp","lr","pc","xpsr",
        NULL
    },
};

const MkdbgArch cortex_m_arch = {
    .name         = "cortex-m",
    .decode_crash = cortex_m_decode_crash,
    .live_debug   = &cortex_m_live,
};

/* suppress "unused function" warnings for parse_registers / parse_halt_signal
 * / signal_name which are available for callers but not used in this file */
static void _cortex_m_unused(void) __attribute__((unused));
static void _cortex_m_unused(void)
{
    (void)parse_halt_signal;
    (void)signal_name;
    (void)parse_registers;
    (void)reg_names;
    (void)hex2;
}
