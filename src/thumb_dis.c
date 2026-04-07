/* thumb_dis.c — Thumb/Thumb-2 disassembler (P1 + P2 + IT block)
 *
 * Coverage:
 *   P1: MOV/MOVW/MOVT, LDR/STR (T1-T4), PUSH/POP, BL/BLX, B (all conds)
 *   P2: ADD/SUB/MUL, CMP/TST/AND/ORR/EOR/BIC/MVN, LSL/LSR/ASR, IT block
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

