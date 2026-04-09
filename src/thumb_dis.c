/* thumb_dis.c — Thumb/Thumb-2 disassembler (P1 + P2 + IT block)
 *
 * Coverage:
 *   P1: MOV/MOVW/MOVT, LDR/STR (T1-T4), PUSH/POP, BL/BLX, B (all conds)
 *   P2: ADD/SUB/MUL, CMP/TST/AND/ORR/EOR/BIC/MVN, LSL/LSR/ASR, IT block
 *   P3: VLDR/VSTR, VADD/VSUB/VMUL (F32), VMOV (imm/reg/core↔FPU), VCVT
 *   Unknown encodings → "<unknown 0xXXXX>" (never crashes)
 *
 * SPDX-License-Identifier: MIT
 */

#include "thumb_dis.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ── helpers ─────────────────────────────────────────────────────────────── */

static const char *s_reg[16] = {
    "r0","r1","r2","r3","r4","r5","r6","r7",
    "r8","r9","r10","r11","r12","sp","lr","pc"
};

static const char *s_cc[16] = {
    "eq","ne","cs","cc","mi","pl","vs","vc",
    "hi","ls","ge","lt","gt","le","al","nv"
};

/* Sign-extend a value of <bits> width to int32_t. */
static int32_t sext(uint32_t v, int bits)
{
    uint32_t mask = 1u << (bits - 1);
    return (int32_t)((v ^ mask) - mask);
}

/* Build a register-list string like "{r4, r5, lr}".
 * bits[0..12] map to r0..r12; extra is a named reg (sp/lr/pc) or -1. */
static void fmt_reglist(char *buf, size_t sz, uint16_t list8, int extra_reg)
{
    char tmp[THUMB_DIS_OUT_MAX];
    int pos = 0;
    tmp[pos++] = '{';
    int first = 1;
    for (int i = 0; i < 8; i++) {
        if (list8 & (1u << i)) {
            if (!first) { tmp[pos++] = ','; tmp[pos++] = ' '; }
            const char *rn = s_reg[i];
            for (int k = 0; rn[k]; k++) tmp[pos++] = rn[k];
            first = 0;
        }
    }
    if (extra_reg >= 0) {
        if (!first) { tmp[pos++] = ','; tmp[pos++] = ' '; }
        const char *rn = s_reg[extra_reg];
        for (int k = 0; rn[k]; k++) tmp[pos++] = rn[k];
    }
    tmp[pos++] = '}';
    tmp[pos] = '\0';
    snprintf(buf, sz, "%s", tmp);
}

/* IT mnemonic: "it", "itt", "ite", "ittt", "itte", etc.
 * firstcond and mask from IT instruction. */
static void fmt_it_mnemonic(char *buf, size_t sz,
                             uint8_t firstcond, uint8_t mask)
{
    char tmp[8] = "it";
    int pos = 2;
    /* Number of extra slots: find lowest set bit position in mask (0-indexed from MSB of nibble) */
    /* trailing-1 at bit[3] → 1 slot total (just "it");
     * trailing-1 at bit[2] → 2 slots ("itt" or "ite");
     * etc. */
    /* Work from bit[3] downward: the bits ABOVE the trailing-1 are T/E bits. */
    for (int b = 3; b >= 0; b--) {
        if (!(mask & (1u << b))) {
            /* This is a T/E slot above the trailing-1. */
            uint8_t te = (mask >> (b + 1)) & 1; /* The actual T/E bit is one higher... */
            /* Simpler: bits[3:0] of mask, from MSB down, until we hit the trailing 1 */
            (void)te;
            break;
        }
    }
    /* Cleaner approach: iterate from bit[3] downward.
     * Stop when we see the lowest set bit (trailing 1). */
    /* The trailing 1 is at position ctz4 = position of lowest set bit in mask[3:0]. */
    int trailing_pos = -1;
    for (int b = 0; b <= 3; b++) {
        if (mask & (1u << b)) { trailing_pos = b; break; }
    }
    if (trailing_pos < 0) { snprintf(buf, sz, "it"); return; }

    /* Slots 2..N encoded in mask bits above trailing_pos (i.e., bits[3..trailing_pos+1]). */
    int n_extra = 3 - trailing_pos; /* number of T/E bits encoded above trailing */
    for (int k = 0; k < n_extra; k++) {
        /* bit for slot (k+2) is at mask[3-k] */
        uint8_t te_bit = (mask >> (3 - k)) & 1;
        tmp[pos++] = (te_bit == (firstcond & 1)) ? 't' : 'e';
    }
    tmp[pos] = '\0';
    snprintf(buf, sz, "%s", tmp);
}

/* Advance IT state after consuming one IT-conditioned instruction. */
static void it_advance(uint8_t *itstate)
{
    if (*itstate & 0x07)
        *itstate = (uint8_t)((*itstate & 0xe0u) | ((*itstate << 1) & 0x1fu));
    else
        *itstate = 0;
}

/* ── 16-bit decoder ──────────────────────────────────────────────────────── */

static int decode_16(uint32_t pc, uint16_t hw,
                     char *out, size_t out_sz, uint8_t *itstate)
{
    /* Condition suffix when inside an IT block. */
    const char *cs = "";
    if (itstate && (*itstate & 0x0f)) {
        cs = s_cc[(*itstate >> 4) & 0xf];
    }

    /* ── Shift / Add / Sub / Move / Compare ────────────────────────── */
    if ((hw & 0xe000u) == 0x0000u) {   /* bits[15:13] = 000 */
        uint8_t op11 = (hw >> 11) & 0x3u; /* bits[12:11] */
        uint8_t op   = (hw >> 9)  & 0x7u; /* bits[11:9] as fine op */
        (void)op;

        if ((hw & 0xf800u) == 0x0000u) {
            /* LSL T1: bits[15:11]=00000 */
            uint8_t imm5 = (hw >> 6) & 0x1fu;
            uint8_t rm   = (hw >> 3) & 0x7u;
            uint8_t rd   = hw & 0x7u;
            if (imm5 == 0)
                snprintf(out, out_sz, "movs%s %s, %s", cs, s_reg[rd], s_reg[rm]);
            else
                snprintf(out, out_sz, "lsls%s %s, %s, #%u", cs, s_reg[rd], s_reg[rm], imm5);
            goto done16;
        }
        if ((hw & 0xf800u) == 0x0800u) {
            /* LSR T1: bits[15:11]=00001 */
            uint8_t imm5 = (hw >> 6) & 0x1fu;
            uint8_t rm   = (hw >> 3) & 0x7u;
            uint8_t rd   = hw & 0x7u;
            uint8_t shift = (imm5 == 0) ? 32u : imm5;
            snprintf(out, out_sz, "lsrs%s %s, %s, #%u", cs, s_reg[rd], s_reg[rm], shift);
            goto done16;
        }
        if ((hw & 0xf800u) == 0x1000u) {
            /* ASR T1: bits[15:11]=00010 */
            uint8_t imm5 = (hw >> 6) & 0x1fu;
            uint8_t rm   = (hw >> 3) & 0x7u;
            uint8_t rd   = hw & 0x7u;
            uint8_t shift = (imm5 == 0) ? 32u : imm5;
            snprintf(out, out_sz, "asrs%s %s, %s, #%u", cs, s_reg[rd], s_reg[rm], shift);
            goto done16;
        }
        if ((hw & 0xf800u) == 0x1800u) {
            /* ADD/SUB register/imm3: bits[15:11]=00011 */
            uint8_t sub  = (hw >> 9) & 1u;
            uint8_t imm  = (hw >> 10) & 1u;
            uint8_t val  = (hw >> 6) & 0x7u;
            uint8_t rn   = (hw >> 3) & 0x7u;
            uint8_t rd   = hw & 0x7u;
            if (imm)
                snprintf(out, out_sz, "%ss%s %s, %s, #%u",
                         sub ? "sub" : "add", cs, s_reg[rd], s_reg[rn], val);
            else
                snprintf(out, out_sz, "%ss%s %s, %s, %s",
                         sub ? "sub" : "add", cs, s_reg[rd], s_reg[rn], s_reg[val]);
            goto done16;
        }
        if ((hw & 0xe000u) == 0x2000u) {  /* bits[15:13]=001 overlaps here */
            /* handled below */
        }
        (void)op11;
    }
    if ((hw & 0xe000u) == 0x2000u) {   /* bits[15:13] = 001 */
        uint8_t op5 = (hw >> 11) & 0x3u;
        uint8_t rdn = (hw >> 8) & 0x7u;
        uint8_t imm8 = hw & 0xffu;
        switch (op5) {
        case 0: snprintf(out, out_sz, "movs%s %s, #%u", cs, s_reg[rdn], imm8); goto done16;
        case 1: snprintf(out, out_sz, "cmp%s %s, #%u",  cs, s_reg[rdn], imm8); goto done16;
        case 2: snprintf(out, out_sz, "adds%s %s, #%u", cs, s_reg[rdn], imm8); goto done16;
        case 3: snprintf(out, out_sz, "subs%s %s, #%u", cs, s_reg[rdn], imm8); goto done16;
        }
    }

    /* ── Data processing ────────────────────────────────────────────── */
    if ((hw & 0xfc00u) == 0x4000u) {   /* bits[15:10]=010000 */
        uint8_t op4  = (hw >> 6) & 0xfu;
        uint8_t rm   = (hw >> 3) & 0x7u;
        uint8_t rdn  = hw & 0x7u;
        static const char *dp_ops[16] = {
            "ands","eors","lsls","lsrs","asrs","adcs","sbcs","rors",
            "tst", "rsbs","cmp", "cmn", "orrs","muls","bics","mvns"
        };
        /* TST, CMP, CMN don't write a destination register. */
        if (op4 == 8 || op4 == 10 || op4 == 11)
            snprintf(out, out_sz, "%s%s %s, %s",
                     dp_ops[op4], cs, s_reg[rdn], s_reg[rm]);
        else if (op4 == 9)  /* RSB T1: rsbs rd, rn, #0 */
            snprintf(out, out_sz, "rsbs%s %s, %s, #0", cs, s_reg[rdn], s_reg[rm]);
        else if (op4 == 13) /* MUL T1 */
            snprintf(out, out_sz, "muls%s %s, %s, %s",
                     cs, s_reg[rdn], s_reg[rm], s_reg[rdn]);
        else
            snprintf(out, out_sz, "%s%s %s, %s",
                     dp_ops[op4], cs, s_reg[rdn], s_reg[rm]);
        goto done16;
    }

    /* ── Special data / BX / BLX / MOV hi-reg ──────────────────────── */
    if ((hw & 0xfc00u) == 0x4400u) {   /* bits[15:10]=010001 */
        uint8_t op2 = (hw >> 8) & 0x3u;
        uint8_t rm  = (hw >> 3) & 0xfu;
        uint8_t rdn = (hw & 0x7u) | ((hw >> 4) & 0x8u);  /* DN:Rdn */
        switch (op2) {
        case 0: snprintf(out, out_sz, "add%s %s, %s",  cs, s_reg[rdn], s_reg[rm]); goto done16;
        case 1: snprintf(out, out_sz, "cmp%s %s, %s",  cs, s_reg[rdn], s_reg[rm]); goto done16;
        case 2: snprintf(out, out_sz, "mov%s %s, %s",  cs, s_reg[rdn], s_reg[rm]); goto done16;
        case 3:
            if (hw & 0x0080u)
                snprintf(out, out_sz, "blx%s %s", cs, s_reg[rm]);
            else
                snprintf(out, out_sz, "bx%s %s", cs, s_reg[rm]);
            goto done16;
        }
    }

    /* ── LDR literal T1 ─────────────────────────────────────────────── */
    if ((hw & 0xf800u) == 0x4800u) {   /* bits[15:11]=01001 */
        uint8_t rt   = (hw >> 8) & 0x7u;
        uint8_t imm8 = hw & 0xffu;
        uint32_t addr = ((pc & ~3u) + 4u) + (uint32_t)imm8 * 4u;
        snprintf(out, out_sz, "ldr%s %s, [pc, #%u]  ; 0x%08x",
                 cs, s_reg[rt], imm8 * 4u, addr);
        goto done16;
    }

    /* ── Load/Store register offset ─────────────────────────────────── */
    if ((hw & 0xf000u) == 0x5000u) {   /* bits[15:12]=0101 */
        static const char *ls_reg_ops[8] = {
            "str","strh","strb","ldrsb","ldr","ldrh","ldrb","ldrsh"
        };
        uint8_t op3 = (hw >> 9) & 0x7u;
        uint8_t rm  = (hw >> 6) & 0x7u;
        uint8_t rn  = (hw >> 3) & 0x7u;
        uint8_t rt  = hw & 0x7u;
        snprintf(out, out_sz, "%s%s %s, [%s, %s]",
                 ls_reg_ops[op3], cs, s_reg[rt], s_reg[rn], s_reg[rm]);
        goto done16;
    }

    /* ── Load/Store word/byte immediate offset ───────────────────────── */
    if ((hw & 0xe000u) == 0x6000u) {   /* bits[15:13]=011 */
        uint8_t byte = (hw >> 12) & 1u;
        uint8_t load = (hw >> 11) & 1u;
        uint8_t imm5 = (hw >> 6) & 0x1fu;
        uint8_t rn   = (hw >> 3) & 0x7u;
        uint8_t rt   = hw & 0x7u;
        uint32_t off = byte ? (uint32_t)imm5 : (uint32_t)imm5 * 4u;
        const char *mn = load ? (byte ? "ldrb" : "ldr") : (byte ? "strb" : "str");
        if (off == 0)
            snprintf(out, out_sz, "%s%s %s, [%s]", mn, cs, s_reg[rt], s_reg[rn]);
        else
            snprintf(out, out_sz, "%s%s %s, [%s, #%u]", mn, cs, s_reg[rt], s_reg[rn], off);
        goto done16;
    }

    /* ── Load/Store halfword immediate ──────────────────────────────── */
    if ((hw & 0xf000u) == 0x8000u) {   /* bits[15:12]=1000 */
        uint8_t load = (hw >> 11) & 1u;
        uint8_t imm5 = (hw >> 6) & 0x1fu;
        uint8_t rn   = (hw >> 3) & 0x7u;
        uint8_t rt   = hw & 0x7u;
        uint32_t off = (uint32_t)imm5 * 2u;
        if (off == 0)
            snprintf(out, out_sz, "%sh%s %s, [%s]",
                     load ? "ldr" : "str", cs, s_reg[rt], s_reg[rn]);
        else
            snprintf(out, out_sz, "%sh%s %s, [%s, #%u]",
                     load ? "ldr" : "str", cs, s_reg[rt], s_reg[rn], off);
        goto done16;
    }

    /* ── SP-relative Load/Store ──────────────────────────────────────── */
    if ((hw & 0xf000u) == 0x9000u) {   /* bits[15:12]=1001 */
        uint8_t load = (hw >> 11) & 1u;
        uint8_t rt   = (hw >> 8) & 0x7u;
        uint8_t imm8 = hw & 0xffu;
        uint32_t off = (uint32_t)imm8 * 4u;
        if (off == 0)
            snprintf(out, out_sz, "%s%s %s, [sp]",
                     load ? "ldr" : "str", cs, s_reg[rt]);
        else
            snprintf(out, out_sz, "%s%s %s, [sp, #%u]",
                     load ? "ldr" : "str", cs, s_reg[rt], off);
        goto done16;
    }

    /* ── ADR T1 (ADD PC + imm) ───────────────────────────────────────── */
    if ((hw & 0xf800u) == 0xa000u) {   /* bits[15:11]=10100 */
        uint8_t rd   = (hw >> 8) & 0x7u;
        uint8_t imm8 = hw & 0xffu;
        uint32_t addr = (pc & ~3u) + 4u + (uint32_t)imm8 * 4u;
        snprintf(out, out_sz, "adr%s %s, 0x%08x", cs, s_reg[rd], addr);
        goto done16;
    }

    /* ── ADD SP-relative (T1/T2) ─────────────────────────────────────── */
    if ((hw & 0xf800u) == 0xa800u) {   /* bits[15:11]=10101 — ADD T1 */
        uint8_t rd   = (hw >> 8) & 0x7u;
        uint8_t imm8 = hw & 0xffu;
        snprintf(out, out_sz, "add%s %s, sp, #%u", cs, s_reg[rd], imm8 * 4u);
        goto done16;
    }
    if ((hw & 0xff80u) == 0xb000u) {   /* ADD SP T2: 1011 0000 0xxx xxxx */
        uint8_t imm7 = hw & 0x7fu;
        snprintf(out, out_sz, "add%s sp, sp, #%u", cs, imm7 * 4u);
        goto done16;
    }
    if ((hw & 0xff80u) == 0xb080u) {   /* SUB SP T1: 1011 0000 1xxx xxxx */
        uint8_t imm7 = hw & 0x7fu;
        snprintf(out, out_sz, "sub%s sp, sp, #%u", cs, imm7 * 4u);
        goto done16;
    }

    /* ── PUSH T1 ─────────────────────────────────────────────────────── */
    if ((hw & 0xfe00u) == 0xb400u) {   /* bits[15:9]=1011010 */
        uint8_t list8 = hw & 0xffu;
        int lr_bit = (hw >> 8) & 1;
        char rl[THUMB_DIS_OUT_MAX];
        fmt_reglist(rl, sizeof(rl), list8, lr_bit ? 14 : -1);  /* lr=14 */
        snprintf(out, out_sz, "push%s %s", cs, rl);
        goto done16;
    }

    /* ── POP T1 ──────────────────────────────────────────────────────── */
    if ((hw & 0xfe00u) == 0xbc00u) {   /* bits[15:9]=1011110 */
        uint8_t list8 = hw & 0xffu;
        int pc_bit = (hw >> 8) & 1;
        char rl[THUMB_DIS_OUT_MAX];
        fmt_reglist(rl, sizeof(rl), list8, pc_bit ? 15 : -1);  /* pc=15 */
        snprintf(out, out_sz, "pop%s %s", cs, rl);
        goto done16;
    }

    /* ── BKPT ────────────────────────────────────────────────────────── */
    if ((hw & 0xff00u) == 0xbe00u) {
        snprintf(out, out_sz, "bkpt #%u", hw & 0xffu);
        goto done16;
    }

    /* ── IT and hints (0xBFxx) ───────────────────────────────────────── */
    if ((hw & 0xff00u) == 0xbf00u) {
        uint8_t firstcond = (hw >> 4) & 0xfu;
        uint8_t mask      = hw & 0xfu;
        if (mask == 0) {
            /* Hints: NOP, YIELD, WFE, WFI, SEV */
            static const char *hints[8] = {
                "nop","yield","wfe","wfi","sev","hint5","hint6","hint7"
            };
            snprintf(out, out_sz, "%s", hints[firstcond & 0x7u]);
        } else {
            /* IT instruction: set itstate for subsequent instructions */
            char itm[8];
            fmt_it_mnemonic(itm, sizeof(itm), firstcond, mask);
            snprintf(out, out_sz, "%s %s", itm, s_cc[firstcond]);
            if (itstate)
                *itstate = (uint8_t)((firstcond << 4) | mask);
            return 2;  /* skip the generic IT advance at done16 */
        }
        goto done16;
    }

    /* ── Sign/zero extend ───────────────────────────────────────────── */
    if ((hw & 0xff00u) == 0xb200u) {
        static const char *ext_ops[4] = {"sxth","sxtb","uxth","uxtb"};
        uint8_t op2 = (hw >> 6) & 0x3u;
        uint8_t rm  = (hw >> 3) & 0x7u;
        uint8_t rd  = hw & 0x7u;
        snprintf(out, out_sz, "%s%s %s, %s", ext_ops[op2], cs, s_reg[rd], s_reg[rm]);
        goto done16;
    }

    /* ── STM / LDM T1 ────────────────────────────────────────────────── */
    if ((hw & 0xf000u) == 0xc000u) {
        uint8_t load  = (hw >> 11) & 1u;
        uint8_t rn    = (hw >> 8) & 0x7u;
        uint8_t list8 = hw & 0xffu;
        char rl[THUMB_DIS_OUT_MAX];
        fmt_reglist(rl, sizeof(rl), list8, -1);
        snprintf(out, out_sz, "%smia%s %s!, %s",
                 load ? "ld" : "st", cs, s_reg[rn], rl);
        goto done16;
    }

    /* ── B conditional T1 ───────────────────────────────────────────── */
    if ((hw & 0xf000u) == 0xd000u) {
        uint8_t cond = (hw >> 8) & 0xfu;
        if (cond == 0xe) { snprintf(out, out_sz, "udf #%u", hw & 0xffu); goto done16; }
        if (cond == 0xf) { snprintf(out, out_sz, "svc #%u", hw & 0xffu); goto done16; }
        int8_t  off8  = (int8_t)(hw & 0xffu);
        uint32_t dest = pc + 4u + (int32_t)off8 * 2;
        snprintf(out, out_sz, "b%s 0x%08x", s_cc[cond], dest);
        goto done16;
    }

    /* ── B unconditional T2 ─────────────────────────────────────────── */
    if ((hw & 0xf800u) == 0xe000u) {
        int32_t off11 = sext(hw & 0x7ffu, 11);
        uint32_t dest = pc + 4u + (uint32_t)(off11 * 2);
        snprintf(out, out_sz, "b%s 0x%08x", cs, dest);
        goto done16;
    }

    /* Unknown 16-bit */
    snprintf(out, out_sz, "<unknown 0x%04x>", hw);
    if (itstate && (*itstate & 0x0f)) it_advance(itstate);
    return 2;

done16:
    if (itstate && (*itstate & 0x0f)) it_advance(itstate);
    return 2;
}

/* ── 32-bit decoder ──────────────────────────────────────────────────────── */

static int decode_32(uint32_t pc, uint16_t hw1, uint16_t hw2,
                     char *out, size_t out_sz, uint8_t *itstate)
{
    const char *cs = "";
    if (itstate && (*itstate & 0x0f))
        cs = s_cc[(*itstate >> 4) & 0xf];

    uint32_t hw = ((uint32_t)hw1 << 16) | hw2;

    /* ── BL T1 ───────────────────────────────────────────────────────── */
    /* hw1[15:11]=11110, hw2[15:14]=11, hw2[12]=1 */
    if ((hw1 & 0xf800u) == 0xf000u &&
        (hw2 & 0xd000u) == 0xd000u) {
        uint32_t S    = (hw1 >> 10) & 1u;
        uint32_t imm10= hw1 & 0x3ffu;
        uint32_t J1   = (hw2 >> 13) & 1u;
        uint32_t J2   = (hw2 >> 11) & 1u;
        uint32_t imm11= hw2 & 0x7ffu;
        uint32_t I1   = ~(J1 ^ S) & 1u;
        uint32_t I2   = ~(J2 ^ S) & 1u;
        uint32_t raw  = (S << 23) | (I1 << 22) | (I2 << 21) | (imm10 << 11) | imm11;
        int32_t  off  = sext(raw, 24);
        uint32_t dest = pc + 4u + (uint32_t)(off * 2);
        snprintf(out, out_sz, "bl%s 0x%08x", cs, dest);
        goto done32;
    }

    /* ── BLX immediate T2 (branch to ARM, imm[1:0]=10) ──────────────── */
    if ((hw1 & 0xf800u) == 0xf000u &&
        (hw2 & 0xd000u) == 0xc000u) {
        /* BLX imm: skip, show as unknown — Cortex-M stays in Thumb */
        snprintf(out, out_sz, "<blx-imm 0x%08x>", hw);
        goto done32;
    }

    /* ── 32-bit data processing (plain binary immediate): hw1[15:11]=11110, hw1[9:8]=10 */
    if ((hw1 & 0xfb00u) == 0xf200u) {
        uint8_t  op5  = (hw1 >> 4) & 0x1fu;   /* bits[8:4] of hw1 */
        uint8_t  rn   = hw1 & 0xfu;
        uint8_t  rd   = (hw2 >> 8) & 0xfu;
        uint16_t imm12 = (uint16_t)(((hw1 >> 10) & 1u) << 11)
                       | (uint16_t)(((hw2 >> 12) & 0x7u) << 8)
                       | (hw2 & 0xffu);
        uint32_t imm32 = (uint32_t)imm12; /* simplified: no ThumbExpandImm for plain */

        switch (op5) {
        case 0x00: /* ADD T3 */
            snprintf(out, out_sz, "add%s.w %s, %s, #%u",
                     cs, s_reg[rd], s_reg[rn], imm32);
            goto done32;
        case 0x04: /* MOVW T3: rn holds imm4 (high nibble of 16-bit immediate) */
            {
                uint16_t imm16 = (uint16_t)(((uint32_t)(hw1 & 0xfu) << 12)
                               | ((uint32_t)((hw1 >> 10) & 1u) << 11)
                               | ((uint32_t)((hw2 >> 12) & 0x7u) << 8)
                               | (hw2 & 0xffu));
                snprintf(out, out_sz, "movw%s %s, #%u", cs, s_reg[rd], imm16);
                goto done32;
            }
        case 0x0a: /* SUBW T4 */
            snprintf(out, out_sz, "sub%s.w %s, %s, #%u",
                     cs, s_reg[rd], s_reg[rn], imm32);
            goto done32;
        case 0x0c: /* MOVT T1: rn holds imm4 (high nibble of 16-bit immediate) */
            {
                uint16_t imm16 = (uint16_t)(((uint32_t)(hw1 & 0xfu) << 12)
                               | ((uint32_t)((hw1 >> 10) & 1u) << 11)
                               | ((uint32_t)((hw2 >> 12) & 0x7u) << 8)
                               | (hw2 & 0xffu));
                snprintf(out, out_sz, "movt%s %s, #%u", cs, s_reg[rd], imm16);
                goto done32;
            }
        }
    }

    /* ── 32-bit data processing register: hw1[15:11]=11101 ────────────── */
    if ((hw1 & 0xff00u) == 0xea00u || (hw1 & 0xff00u) == 0xeb00u) {
        uint8_t S   = (hw1 >> 4) & 1u;
        uint8_t op4 = (hw1 >> 5) & 0xfu;
        uint8_t rn  = hw1 & 0xfu;
        uint8_t rd  = (hw2 >> 8) & 0xfu;
        uint8_t rm  = hw2 & 0xfu;
        uint8_t type = (hw2 >> 4) & 0x3u;  /* shift type */
        /* imm5 = imm3:imm2 — bits[14:12]:bits[7:6] of hw2 (NOT bit[8] which belongs to Rd) */
        uint8_t imm5 = (uint8_t)(((hw2 >> 12) & 0x7u) << 2) | ((hw2 >> 6) & 0x3u);

        static const char *sh_types[4] = {"lsl","lsr","asr","ror"};
        const char *sf = S ? "s" : "";

        if (imm5 == 0 && type == 0) {
            /* no shift */
            switch (op4) {
            case 0:
                if (rd == 0xf) { snprintf(out, out_sz, "tst%s.w %s, %s", cs, s_reg[rn], s_reg[rm]); goto done32; }
                snprintf(out, out_sz, "and%s%s.w %s, %s, %s", sf, cs, s_reg[rd], s_reg[rn], s_reg[rm]); goto done32;
            case 1:  snprintf(out, out_sz, "bic%s%s.w %s, %s, %s", sf, cs, s_reg[rd], s_reg[rn], s_reg[rm]); goto done32;
            case 2:
                if (rn == 0xf) { snprintf(out, out_sz, "mov%s%s.w %s, %s", sf, cs, s_reg[rd], s_reg[rm]); goto done32; }
                snprintf(out, out_sz, "orr%s%s.w %s, %s, %s", sf, cs, s_reg[rd], s_reg[rn], s_reg[rm]); goto done32;
            case 3:
                if (rn == 0xf) { snprintf(out, out_sz, "mvn%s%s.w %s, %s", sf, cs, s_reg[rd], s_reg[rm]); goto done32; }
                snprintf(out, out_sz, "orn%s%s.w %s, %s, %s", sf, cs, s_reg[rd], s_reg[rn], s_reg[rm]); goto done32;
            case 4:  snprintf(out, out_sz, "eor%s%s.w %s, %s, %s", sf, cs, s_reg[rd], s_reg[rn], s_reg[rm]); goto done32;
            case 8:
                if (rd == 0xf) { snprintf(out, out_sz, "cmn%s.w %s, %s", cs, s_reg[rn], s_reg[rm]); goto done32; }
                snprintf(out, out_sz, "add%s%s.w %s, %s, %s", sf, cs, s_reg[rd], s_reg[rn], s_reg[rm]); goto done32;
            case 10: snprintf(out, out_sz, "adc%s%s.w %s, %s, %s", sf, cs, s_reg[rd], s_reg[rn], s_reg[rm]); goto done32;
            case 11: snprintf(out, out_sz, "sbc%s%s.w %s, %s, %s", sf, cs, s_reg[rd], s_reg[rn], s_reg[rm]); goto done32;
            case 13:
                if (rd == 0xf) { snprintf(out, out_sz, "cmp%s.w %s, %s", cs, s_reg[rn], s_reg[rm]); goto done32; }
                snprintf(out, out_sz, "sub%s%s.w %s, %s, %s", sf, cs, s_reg[rd], s_reg[rn], s_reg[rm]); goto done32;
            case 14: snprintf(out, out_sz, "rsb%s%s.w %s, %s, %s", sf, cs, s_reg[rd], s_reg[rn], s_reg[rm]); goto done32;
            }
        } else {
            /* shifted register */
            snprintf(out, out_sz, "op%u%s.w %s, %s, %s %s #%u",
                     op4, cs, s_reg[rd], s_reg[rn], s_reg[rm], sh_types[type], imm5);
            goto done32;
        }
    }

    /* ── 32-bit shifts (LSL/LSR/ASR/ROR immediate): hw1[15:11]=11101, hw1[8]=0, rn=1111 */
    if ((hw1 & 0xffcfu) == 0xea4fu) {   /* MOVS.W with shift — catches LSL/LSR/ASR T2/T3 */
        uint8_t rd   = (hw2 >> 8) & 0xfu;
        uint8_t rm   = hw2 & 0xfu;
        uint8_t type = (hw2 >> 4) & 0x3u;
        uint8_t imm3 = (hw2 >> 12) & 0x3u;
        uint8_t imm2 = (hw2 >> 6) & 0x3u;
        uint8_t imm5 = (uint8_t)(imm3 << 2) | imm2;
        static const char *sh_imm[4] = {"lsl","lsr","asr","ror"};
        snprintf(out, out_sz, "%ss%s.w %s, %s, #%u",
                 sh_imm[type], cs, s_reg[rd], s_reg[rm], imm5);
        goto done32;
    }

    /* ── 32-bit Load/Store: hw1[15:12]=1111 ─────────────────────────── */
    /* LDR T3 (immediate, wide): 1111 1000 1101 xxxx */
    if ((hw1 & 0xfff0u) == 0xf8d0u) {
        uint8_t  rn   = hw1 & 0xfu;
        uint8_t  rt   = (hw2 >> 12) & 0xfu;
        uint16_t imm12 = hw2 & 0xfffu;
        if (imm12 == 0)
            snprintf(out, out_sz, "ldr%s.w %s, [%s]", cs, s_reg[rt], s_reg[rn]);
        else
            snprintf(out, out_sz, "ldr%s.w %s, [%s, #%u]", cs, s_reg[rt], s_reg[rn], imm12);
        goto done32;
    }
    /* STR T3 (immediate, wide): 1111 1000 1100 xxxx */
    if ((hw1 & 0xfff0u) == 0xf8c0u) {
        uint8_t  rn   = hw1 & 0xfu;
        uint8_t  rt   = (hw2 >> 12) & 0xfu;
        uint16_t imm12 = hw2 & 0xfffu;
        if (imm12 == 0)
            snprintf(out, out_sz, "str%s.w %s, [%s]", cs, s_reg[rt], s_reg[rn]);
        else
            snprintf(out, out_sz, "str%s.w %s, [%s, #%u]", cs, s_reg[rt], s_reg[rn], imm12);
        goto done32;
    }

    /* ── 32-bit B conditional T3: hw1[15:11]=11110, hw2[15:14]=10, hw2[12]=0 */
    if ((hw1 & 0xf800u) == 0xf000u &&
        (hw2 & 0xd000u) == 0x8000u) {
        uint8_t cond  = (hw1 >> 6) & 0xfu;
        if (cond == 0xf || cond == 0xe) goto unknown32;
        uint32_t S    = (hw1 >> 10) & 1u;
        uint32_t imm6 = hw1 & 0x3fu;
        uint32_t J1   = (hw2 >> 13) & 1u;
        uint32_t J2   = (hw2 >> 11) & 1u;
        uint32_t imm11 = hw2 & 0x7ffu;
        uint32_t raw  = (S << 19) | (J2 << 18) | (J1 << 17) | (imm6 << 11) | imm11;
        int32_t  off  = sext(raw, 20);
        uint32_t dest = pc + 4u + (uint32_t)(off * 2);
        snprintf(out, out_sz, "b%s.w 0x%08x", s_cc[cond], dest);
        goto done32;
    }

    /* ── 32-bit B unconditional T4: hw1[15:11]=11110, hw2[15:14]=10, hw2[12]=1 */
    if ((hw1 & 0xf800u) == 0xf000u &&
        (hw2 & 0xd000u) == 0x9000u) {
        uint32_t S    = (hw1 >> 10) & 1u;
        uint32_t imm10= hw1 & 0x3ffu;
        uint32_t J1   = (hw2 >> 13) & 1u;
        uint32_t J2   = (hw2 >> 11) & 1u;
        uint32_t imm11= hw2 & 0x7ffu;
        uint32_t I1   = ~(J1 ^ S) & 1u;
        uint32_t I2   = ~(J2 ^ S) & 1u;
        uint32_t raw  = (S << 23) | (I1 << 22) | (I2 << 21) | (imm10 << 11) | imm11;
        int32_t  off  = sext(raw, 24);
        uint32_t dest = pc + 4u + (uint32_t)(off * 2);
        snprintf(out, out_sz, "b%s.w 0x%08x", cs, dest);
        goto done32;
    }

    /* ── MUL T2 (32-bit): 1111 1011 0000 ─────────────────────────────── */
    if ((hw1 & 0xfff0u) == 0xfb00u && (hw2 & 0xf0f0u) == 0xf000u) {
        uint8_t rn = hw1 & 0xfu;
        uint8_t rd = (hw2 >> 8) & 0xfu;
        uint8_t rm = hw2 & 0xfu;
        snprintf(out, out_sz, "mul%s %s, %s, %s", cs, s_reg[rd], s_reg[rn], s_reg[rm]);
        goto done32;
    }

    /* ── TST/CMP/CMN/TEQ (32-bit): hw1[15:11]=11101, with rd=1111 ─────── */
    if ((hw1 & 0xff10u) == 0xea10u) {
        uint8_t rn  = hw1 & 0xfu;
        uint8_t rm  = hw2 & 0xfu;
        uint8_t op4 = (hw1 >> 5) & 0xfu;
        switch (op4) {
        case 0: snprintf(out, out_sz, "tst%s.w %s, %s", cs, s_reg[rn], s_reg[rm]); goto done32;
        case 4: snprintf(out, out_sz, "teq%s.w %s, %s", cs, s_reg[rn], s_reg[rm]); goto done32;
        case 8: snprintf(out, out_sz, "cmn%s.w %s, %s", cs, s_reg[rn], s_reg[rm]); goto done32;
        case 13: snprintf(out, out_sz, "cmp%s.w %s, %s", cs, s_reg[rn], s_reg[rm]); goto done32;
        }
    }

    /* ── VFP/FPU (Cortex-M4 FPv4-SP-D16) ────────────────────────────────── */
    /*
     * All VFP SP instructions have hw1[15:8] in [0xEC..0xEF] and
     * hw2[11:8]=0b1010 (cp10, single-precision).
     *
     * Register encoding (5-bit SP regs s0..s31):
     *   Sd = Vd*2+D  where Vd=hw2[15:12], D=hw1[6]
     *   Sn = Vn*2+N  where Vn=hw1[3:0],   N=hw2[7]
     *   Sm = Vm*2+M  where Vm=hw2[3:0],    M=hw2[5]
     *
     * Instruction groups distinguished by hw1[15:8]:
     *   0xED: VLDR / VSTR
     *   0xEE, hw2&0x0F70==0x0A10: VMOV (core↔VFP) or VMRS
     *   0xEE, otherwise:          VFP data-processing
     *
     * Within VFP data-processing, op_nD removes the D bit from hw1[7:4]:
     *   op_nD = ((hw1>>5)&4) | ((hw1>>4)&3)
     *     2 → VMUL
     *     3 → VADD (hw2[6]=0) / VSUB (hw2[6]=1)
     *     4 → VDIV
     *     7 → miscellaneous unary/convert (distinguished by hw1[3:0] and hw2[7])
     *
     * Verified against arm-none-eabi-as output (ARMv7E-M, FPv4-SP-D16).
     */
    if ((hw1 & 0xff00u) >= 0xec00u && (hw2 & 0x0f00u) == 0x0a00u) {
        uint8_t  D  = (hw1 >> 6) & 1u;
        uint8_t  Vn = hw1 & 0xfu;
        uint8_t  Vd = (hw2 >> 12) & 0xfu;
        uint8_t  N  = (hw2 >> 7) & 1u;   /* also used as op2 (signed) in VCVT */
        uint8_t  Vm = hw2 & 0xfu;
        uint8_t  M  = (hw2 >> 5) & 1u;
        unsigned sd = (unsigned)Vd * 2u + D;
        unsigned sn = (unsigned)Vn * 2u + N;
        unsigned sm = (unsigned)Vm * 2u + M;

        /* ── VLDR / VSTR (hw1[15:8]=0xED) ──────────────────────────────── */
        if ((hw1 & 0xff00u) == 0xed00u) {
            uint8_t  load = (hw1 >> 4) & 1u;   /* L=1 → VLDR, L=0 → VSTR */
            uint8_t  U    = (hw1 >> 7) & 1u;   /* U=1 → add offset */
            uint8_t  Rn   = hw1 & 0xfu;
            uint32_t off  = (uint32_t)(hw2 & 0xffu) * 4u;
            const char *base = (Rn == 15) ? "pc" : s_reg[Rn];
            if (off == 0)
                snprintf(out, out_sz, "%s%s s%u, [%s]",
                         load ? "vldr" : "vstr", cs, sd, base);
            else if (U)
                snprintf(out, out_sz, "%s%s s%u, [%s, #%u]",
                         load ? "vldr" : "vstr", cs, sd, base, off);
            else
                snprintf(out, out_sz, "%s%s s%u, [%s, #-%u]",
                         load ? "vldr" : "vstr", cs, sd, base, off);
            goto done32;
        }

        if ((hw1 & 0xff00u) == 0xee00u) {
            /* ── VMOV core↔VFP / VMRS (fixed hw2 pattern) ─────────────── */
            /* hw2[6:4]=001 (with N free at hw2[7]):  hw2 & 0x0F70 == 0x0A10 */
            if ((hw2 & 0x0f70u) == 0x0a10u) {
                uint8_t op47 = (hw1 >> 4) & 0xfu;   /* hw1[7:4] */
                uint8_t Rt   = (hw2 >> 12) & 0xfu;
                if (op47 == 0xfu) {
                    /* VMRS: hw1[7:4]=1111 (FPSCR system register) */
                    if (Rt == 15)
                        snprintf(out, out_sz, "vmrs%s APSR_nzcv, fpscr", cs);
                    else
                        snprintf(out, out_sz, "vmrs%s %s, fpscr", cs, s_reg[Rt]);
                } else {
                    /* VMOV between ARM core reg and SP VFP reg.
                     * L=hw1[4]: 0=ARM→VFP, 1=VFP→ARM.
                     * Sn = Vn*2+N where Vn=hw1[3:0], N=hw2[7]. */
                    if ((hw1 >> 4) & 1u)
                        snprintf(out, out_sz, "vmov%s %s, s%u", cs, s_reg[Rt], sn);
                    else
                        snprintf(out, out_sz, "vmov%s s%u, %s", cs, sn, s_reg[Rt]);
                }
                goto done32;
            }

            /* ── VFP data-processing ─────────────────────────────────────── */
            /* op_nD: hw1[7] in bit2, hw1[5:4] in bits[1:0] — removes D(hw1[6]) */
            uint8_t op_nD = (uint8_t)(((hw1 >> 5) & 4u) | ((hw1 >> 4) & 3u));
            switch (op_nD) {
            case 0x2:   /* VMUL */
                snprintf(out, out_sz, "vmul%s.f32 s%u, s%u, s%u", cs, sd, sn, sm);
                goto done32;
            case 0x3:   /* VADD / VSUB distinguished by hw2[6] */
                if (hw2 & 0x40u)
                    snprintf(out, out_sz, "vsub%s.f32 s%u, s%u, s%u", cs, sd, sn, sm);
                else
                    snprintf(out, out_sz, "vadd%s.f32 s%u, s%u, s%u", cs, sd, sn, sm);
                goto done32;
            case 0x4:   /* VDIV */
                snprintf(out, out_sz, "vdiv%s.f32 s%u, s%u, s%u", cs, sd, sn, sm);
                goto done32;
            case 0x7: { /* miscellaneous: VMOV, VABS, VNEG, VSQRT, VCVT, VCMP */
                uint8_t vn_lo = hw1 & 0xfu;   /* hw1[3:0] selects sub-operation */
                if (vn_lo & 0x8u) {
                    /* VCVT: hw1[3]=1. hw1[2]=to_int, hw1[0]=signed(to_int case)
                     * hw2[7](=N)=signed for to_float case. */
                    if (hw1 & 0x4u) {
                        /* to integer (float→int) */
                        const char *sfx = (hw1 & 0x1u) ? "s" : "u";
                        snprintf(out, out_sz, "vcvt%s.%s32.f32 s%u, s%u",
                                 cs, sfx, sd, sm);
                    } else {
                        /* to float (int→float): signed/unsigned from hw2[7] */
                        const char *sfx = N ? "s" : "u";
                        snprintf(out, out_sz, "vcvt%s.f32.%s32 s%u, s%u",
                                 cs, sfx, sd, sm);
                    }
                    goto done32;
                }
                if (vn_lo == 0x4u) {
                    /* VCMP */
                    snprintf(out, out_sz, "vcmp%s.f32 s%u, s%u", cs, sd, sm);
                    goto done32;
                }
                /* VMOV.F32 / VABS / VNEG / VSQRT: distinguished by vn_lo and hw2[7] */
                if (vn_lo == 0x0u) {
                    /* hw2[7]=0 → VMOV.F32;  hw2[7]=1 → VABS */
                    if (N)
                        snprintf(out, out_sz, "vabs%s.f32 s%u, s%u", cs, sd, sm);
                    else
                        snprintf(out, out_sz, "vmov%s.f32 s%u, s%u", cs, sd, sm);
                    goto done32;
                }
                if (vn_lo == 0x1u) {
                    /* hw2[7]=0 → VNEG;  hw2[7]=1 → VSQRT */
                    if (N)
                        snprintf(out, out_sz, "vsqrt%s.f32 s%u, s%u", cs, sd, sm);
                    else
                        snprintf(out, out_sz, "vneg%s.f32 s%u, s%u", cs, sd, sm);
                    goto done32;
                }
                break;
            }
            default:
                break;
            }
        }
    }

unknown32:
    snprintf(out, out_sz, "<unknown 0x%04x%04x>", hw1, hw2);
    if (itstate && (*itstate & 0x0f)) it_advance(itstate);
    return 4;

done32:
    if (itstate && (*itstate & 0x0f)) it_advance(itstate);
    return 4;
}

/* ── public API ──────────────────────────────────────────────────────────── */

int thumb_dis_one(uint32_t pc, const uint8_t *buf, size_t buf_len,
                  char *out, size_t out_sz,
                  uint8_t *itstate)
{
    if (!buf || !out || out_sz == 0 || buf_len < 2) {
        if (out && out_sz > 0) out[0] = '\0';
        return -1;
    }

    /* Mask Thumb bit in PC (raw Cortex-M register value may have bit[0]=1). */
    pc &= ~1u;

    /* Read first halfword (little-endian). */
    uint16_t hw1 = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);

    /* 32-bit instruction if bits[15:11] == 0b11101, 0b11110, or 0b11111. */
    uint8_t top5 = (hw1 >> 11) & 0x1fu;
    if (top5 == 0x1d || top5 == 0x1e || top5 == 0x1f) {
        if (buf_len < 4) {
            snprintf(out, out_sz, "<trunc 0x%04x>", hw1);
            return -2;  /* buf_len insufficient for 32-bit instruction */
        }
        uint16_t hw2 = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
        return decode_32(pc, hw1, hw2, out, out_sz, itstate);
    }

    return decode_16(pc, hw1, out, out_sz, itstate);
}
