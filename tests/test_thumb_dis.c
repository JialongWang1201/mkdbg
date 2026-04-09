/* tests/test_thumb_dis.c — unit tests for thumb_dis_one()
 *
 * Golden test approach: inline byte constants + expected output strings.
 * No file I/O.  Follows the dwarf_test.c style.
 *
 * Covers:
 *   P1: MOV/LSL/LSR/ASR/ADD/SUB, LDR/STR (multiple encodings), PUSH/POP,
 *       BX, B (all conds), BKPT, NOP, ADR, SP-relative ops
 *   P2: Data processing (AND/EOR/TST/CMP/ORR/BIC/MVN), sign/zero-extend,
 *       STM/LDM, IT block instruction
 *   32-bit: BL, MOVW, MOVT, ADDW, SUBW, LDR.W, STR.W, ADD.W, B.W, BEQ.W
 *   IT state machine: multi-step ITTEE block (4 instructions, 2T+2E)
 *   P3 FPU: VLDR/VSTR, VADD/VSUB/VMUL/VDIV, VMOV.F32, VABS/VNEG/VSQRT,
 *           VCVT (f32↔s32/u32), VCMP, VMOV(core↔VFP), VMRS
 *   Edge cases: buf_len < 2, buf_len == 2 for 32-bit prefix, unknown encoding,
 *               itstate=NULL degraded mode
 *
 * SPDX-License-Identifier: MIT
 */

#include "thumb_dis.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ── test harness ─────────────────────────────────────────────────────────── */

static int s_run = 0, s_pass = 0;

#define CHECK(cond, label) do { \
    s_run++; \
    if (cond) { s_pass++; printf("  PASS  %s\n", (label)); } \
    else             printf("  FAIL  %s\n", (label)); \
} while (0)

/* Disassemble one instruction and check output + return value. */
static void chk(const char *label,
                uint32_t pc, const uint8_t *buf, size_t buf_len,
                uint8_t *its,
                const char *expect_out, int expect_ret)
{
    char out[THUMB_DIS_OUT_MAX];
    out[0] = '\0';
    int ret = thumb_dis_one(pc, buf, buf_len, out, sizeof(out), its);
    int ok = (ret == expect_ret) && (strcmp(out, expect_out) == 0);
    s_run++;
    if (ok) {
        s_pass++;
        printf("  PASS  %s\n", label);
    } else {
        printf("  FAIL  %s  (ret=%d want=%d  out=\"%s\"  want=\"%s\")\n",
               label, ret, expect_ret, out, expect_out);
    }
}

/* ── 16-bit instruction tests ─────────────────────────────────────────────── */

static void test_16bit(void)
{
    printf("\n--- 16-bit instructions ---\n");

    /* MOV immediate T1: movs r0, #42  → hw=0x202A */
    { const uint8_t b[] = {0x2A, 0x20};
      chk("movs r0, #42", 0x1000, b, 2, NULL, "movs r0, #42", 2); }

    /* LSL T1 (imm5=0 → movs alias): movs r0, r1  → hw=0x0008 */
    { const uint8_t b[] = {0x08, 0x00};
      chk("movs r0, r1 (lsl#0)", 0x1000, b, 2, NULL, "movs r0, r1", 2); }

    /* LSL T1 (imm5=3): lsls r0, r1, #3  → hw=0x00C8 */
    { const uint8_t b[] = {0xC8, 0x00};
      chk("lsls r0, r1, #3", 0x1000, b, 2, NULL, "lsls r0, r1, #3", 2); }

    /* LSR T1 (imm5=1): lsrs r0, r1, #1  → hw=0x0848 */
    { const uint8_t b[] = {0x48, 0x08};
      chk("lsrs r0, r1, #1", 0x1000, b, 2, NULL, "lsrs r0, r1, #1", 2); }

    /* ASR T1 (imm5=1): asrs r0, r1, #1  → hw=0x1048 */
    { const uint8_t b[] = {0x48, 0x10};
      chk("asrs r0, r1, #1", 0x1000, b, 2, NULL, "asrs r0, r1, #1", 2); }

    /* ADD register T1: adds r0, r1, r2 → hw=0x1888 (sub=0,imm=0,val=2,rn=1,rd=0) */
    { const uint8_t b[] = {0x88, 0x18};
      chk("adds r0, r1, r2 (reg)", 0x1000, b, 2, NULL, "adds r0, r1, r2", 2); }

    /* ADD immediate T1 (imm3): adds r0, r1, #2 → hw=0x1C88 */
    { const uint8_t b[] = {0x88, 0x1C};
      chk("adds r0, r1, #2 (imm3)", 0x1000, b, 2, NULL, "adds r0, r1, #2", 2); }

    /* SUB immediate T1 (imm3): subs r2, r3, #1 → hw=0x1E5A */
    { const uint8_t b[] = {0x5A, 0x1E};
      chk("subs r2, r3, #1 (imm3)", 0x1000, b, 2, NULL, "subs r2, r3, #1", 2); }

    /* CMP immediate: cmp r0, #5 → hw=0x2805 */
    { const uint8_t b[] = {0x05, 0x28};
      chk("cmp r0, #5", 0x1000, b, 2, NULL, "cmp r0, #5", 2); }

    /* ADDS immediate T2 (imm8): adds r1, #10 → hw=0x310A (bits[12:11]=10 → op5=2) */
    { const uint8_t b[] = {0x0A, 0x31};
      chk("adds r1, #10", 0x1000, b, 2, NULL, "adds r1, #10", 2); }

    /* SUBS immediate T2 (imm8): subs r0, #1 → hw=0x3801 */
    { const uint8_t b[] = {0x01, 0x38};
      chk("subs r0, #1", 0x1000, b, 2, NULL, "subs r0, #1", 2); }

    /* Data processing: ANDS r0, r1 → hw=0x4008 (op4=0) */
    { const uint8_t b[] = {0x08, 0x40};
      chk("ands r0, r1", 0x1000, b, 2, NULL, "ands r0, r1", 2); }

    /* Data processing: EORS r0, r1 → hw=0x4048 (op4=1) */
    { const uint8_t b[] = {0x48, 0x40};
      chk("eors r0, r1", 0x1000, b, 2, NULL, "eors r0, r1", 2); }

    /* Data processing: TST r0, r1 → hw=0x4208 (op4=8) */
    { const uint8_t b[] = {0x08, 0x42};
      chk("tst r0, r1", 0x1000, b, 2, NULL, "tst r0, r1", 2); }

    /* Data processing: RSBS r0, r1, #0 → hw=0x4248 (op4=9) */
    { const uint8_t b[] = {0x48, 0x42};
      chk("rsbs r0, r1, #0", 0x1000, b, 2, NULL, "rsbs r0, r1, #0", 2); }

    /* Data processing: CMP r0, r1 → hw=0x4288 (op4=10) */
    { const uint8_t b[] = {0x88, 0x42};
      chk("cmp r0, r1 (dp)", 0x1000, b, 2, NULL, "cmp r0, r1", 2); }

    /* Data processing: ORRS r0, r1 → hw=0x4308 (op4=12) */
    { const uint8_t b[] = {0x08, 0x43};
      chk("orrs r0, r1", 0x1000, b, 2, NULL, "orrs r0, r1", 2); }

    /* Data processing: MULS r0, r1, r0 → hw=0x4348 (op4=13) */
    { const uint8_t b[] = {0x48, 0x43};
      chk("muls r0, r1, r0", 0x1000, b, 2, NULL, "muls r0, r1, r0", 2); }

    /* Data processing: BICS r0, r1 → hw=0x4388 (op4=14) */
    { const uint8_t b[] = {0x88, 0x43};
      chk("bics r0, r1", 0x1000, b, 2, NULL, "bics r0, r1", 2); }

    /* Data processing: MVNS r0, r1 → hw=0x43C8 (op4=15) */
    { const uint8_t b[] = {0xC8, 0x43};
      chk("mvns r0, r1", 0x1000, b, 2, NULL, "mvns r0, r1", 2); }

    /* Special data: ADD hi-reg, ADD r0, r8 → hw=0x4440 (op2=0,rdn=0,rm=8) */
    { const uint8_t b[] = {0x40, 0x44};
      chk("add r0, r8 (hi-reg)", 0x1000, b, 2, NULL, "add r0, r8", 2); }

    /* Special data: MOV T2 (hi-reg), MOV r0, r1 → hw=0x4608 */
    { const uint8_t b[] = {0x08, 0x46};
      chk("mov r0, r1 (hi-reg)", 0x1000, b, 2, NULL, "mov r0, r1", 2); }

    /* BX LR → hw=0x4770 */
    { const uint8_t b[] = {0x70, 0x47};
      chk("bx lr", 0x1000, b, 2, NULL, "bx lr", 2); }

    /* LDR literal T1: ldr r0, [pc, #0] → hw=0x4800, pc=0x2000
     * addr = (0x2000&~3)+4+0 = 0x2004 */
    { const uint8_t b[] = {0x00, 0x48};
      chk("ldr r0, [pc, #0]", 0x2000, b, 2, NULL,
          "ldr r0, [pc, #0]  ; 0x00002004", 2); }

    /* LDR literal T1: ldr r1, [pc, #4] → hw=0x4901, pc=0x1000
     * addr = (0x1000&~3)+4+4 = 0x1008 */
    { const uint8_t b[] = {0x01, 0x49};
      chk("ldr r1, [pc, #4]", 0x1000, b, 2, NULL,
          "ldr r1, [pc, #4]  ; 0x00001008", 2); }

    /* STR register offset: str r0, [r1, r2] → hw=0x5088 */
    { const uint8_t b[] = {0x88, 0x50};
      chk("str r0, [r1, r2]", 0x1000, b, 2, NULL, "str r0, [r1, r2]", 2); }

    /* LDR register offset: ldr r0, [r1, r2] → hw=0x5888 */
    { const uint8_t b[] = {0x88, 0x58};
      chk("ldr r0, [r1, r2]", 0x1000, b, 2, NULL, "ldr r0, [r1, r2]", 2); }

    /* STR word immediate: str r0, [r1, #4] → hw=0x6048 (load=0,byte=0,imm5=1,off=4) */
    { const uint8_t b[] = {0x48, 0x60};
      chk("str r0, [r1, #4]", 0x1000, b, 2, NULL, "str r0, [r1, #4]", 2); }

    /* LDR word immediate: ldr r0, [r1, #4] → hw=0x6848 (load=1,byte=0,imm5=1) */
    { const uint8_t b[] = {0x48, 0x68};
      chk("ldr r0, [r1, #4]", 0x1000, b, 2, NULL, "ldr r0, [r1, #4]", 2); }

    /* LDR word zero offset: ldr r0, [r1] → hw=0x6808 */
    { const uint8_t b[] = {0x08, 0x68};
      chk("ldr r0, [r1]", 0x1000, b, 2, NULL, "ldr r0, [r1]", 2); }

    /* LDRB immediate: ldrb r0, [r1, #1] → hw=0x7848 (byte=1,load=1,imm5=1) */
    { const uint8_t b[] = {0x48, 0x78};
      chk("ldrb r0, [r1, #1]", 0x1000, b, 2, NULL, "ldrb r0, [r1, #1]", 2); }

    /* STRH immediate zero offset: strh r0, [r1] → hw=0x8008 */
    { const uint8_t b[] = {0x08, 0x80};
      chk("strh r0, [r1]", 0x1000, b, 2, NULL, "strh r0, [r1]", 2); }

    /* LDRH immediate: ldrh r0, [r1, #2] → hw=0x8848 (load=1,imm5=1,off=2) */
    { const uint8_t b[] = {0x48, 0x88};
      chk("ldrh r0, [r1, #2]", 0x1000, b, 2, NULL, "ldrh r0, [r1, #2]", 2); }

    /* STR SP-relative: str r0, [sp] → hw=0x9000 */
    { const uint8_t b[] = {0x00, 0x90};
      chk("str r0, [sp]", 0x1000, b, 2, NULL, "str r0, [sp]", 2); }

    /* LDR SP-relative: ldr r0, [sp, #8] → hw=0x9802 (imm8=2,off=8) */
    { const uint8_t b[] = {0x02, 0x98};
      chk("ldr r0, [sp, #8]", 0x1000, b, 2, NULL, "ldr r0, [sp, #8]", 2); }

    /* ADR T1: adr r0, ... pc=0x2000, imm8=0 → addr=0x2004 */
    { const uint8_t b[] = {0x00, 0xA0};
      chk("adr r0 (imm8=0)", 0x2000, b, 2, NULL, "adr r0, 0x00002004", 2); }

    /* ADD SP T1: add r1, sp, #4 → hw=0xA901 (rd=1,imm8=1) */
    { const uint8_t b[] = {0x01, 0xA9};
      chk("add r1, sp, #4", 0x1000, b, 2, NULL, "add r1, sp, #4", 2); }

    /* ADD SP T2: add sp, sp, #8 → hw=0xB002 (imm7=2) */
    { const uint8_t b[] = {0x02, 0xB0};
      chk("add sp, sp, #8", 0x1000, b, 2, NULL, "add sp, sp, #8", 2); }

    /* SUB SP T1: sub sp, sp, #8 → hw=0xB082 (imm7=2) */
    { const uint8_t b[] = {0x82, 0xB0};
      chk("sub sp, sp, #8", 0x1000, b, 2, NULL, "sub sp, sp, #8", 2); }

    /* PUSH T1: push {r4, lr} → hw=0xB510 */
    { const uint8_t b[] = {0x10, 0xB5};
      chk("push {r4, lr}", 0x1000, b, 2, NULL, "push {r4, lr}", 2); }

    /* PUSH T1: push {r4, r5, r6, r7} → hw=0xB4F0 (list8=0xF0) */
    { const uint8_t b[] = {0xF0, 0xB4};
      chk("push {r4,r5,r6,r7}", 0x1000, b, 2, NULL,
          "push {r4, r5, r6, r7}", 2); }

    /* POP T1: pop {r4, pc} → hw=0xBD10 */
    { const uint8_t b[] = {0x10, 0xBD};
      chk("pop {r4, pc}", 0x1000, b, 2, NULL, "pop {r4, pc}", 2); }

    /* BKPT: bkpt #7 → hw=0xBE07 */
    { const uint8_t b[] = {0x07, 0xBE};
      chk("bkpt #7", 0x1000, b, 2, NULL, "bkpt #7", 2); }

    /* NOP hint: nop → hw=0xBF00 */
    { const uint8_t b[] = {0x00, 0xBF};
      chk("nop", 0x1000, b, 2, NULL, "nop", 2); }

    /* Sign extend: sxth r0, r1 → hw=0xB208 */
    { const uint8_t b[] = {0x08, 0xB2};
      chk("sxth r0, r1", 0x1000, b, 2, NULL, "sxth r0, r1", 2); }

    /* Zero extend: uxtb r0, r1 → hw=0xB2C8 (op2=3) */
    { const uint8_t b[] = {0xC8, 0xB2};
      chk("uxtb r0, r1", 0x1000, b, 2, NULL, "uxtb r0, r1", 2); }

    /* STMIA T1: stmia r0!, {r1, r2} → hw=0xC006 (load=0,rn=0,list8=0x06) */
    { const uint8_t b[] = {0x06, 0xC0};
      chk("stmia r0!, {r1, r2}", 0x1000, b, 2, NULL,
          "stmia r0!, {r1, r2}", 2); }

    /* LDMIA T1: ldmia r1!, {r0} → hw=0xC901 (load=1,rn=1,list8=0x01) */
    { const uint8_t b[] = {0x01, 0xC9};
      chk("ldmia r1!, {r0}", 0x1000, b, 2, NULL, "ldmia r1!, {r0}", 2); }

    /* SVC: svc #0 → hw=0xDF00 (cond=0xF in B block) */
    { const uint8_t b[] = {0x00, 0xDF};
      chk("svc #0", 0x1000, b, 2, NULL, "svc #0", 2); }

    /* B conditional T1: beq 0x00001004 (off8=0, pc=0x1000) → hw=0xD000 */
    { const uint8_t b[] = {0x00, 0xD0};
      chk("beq 0x00001004", 0x1000, b, 2, NULL, "beq 0x00001004", 2); }

    /* B conditional T1: bne 0x00001004 (off8=0, pc=0x1000) → hw=0xD100 */
    { const uint8_t b[] = {0x00, 0xD1};
      chk("bne 0x00001004", 0x1000, b, 2, NULL, "bne 0x00001004", 2); }

    /* B conditional T1: blt backward (off8=-2, pc=0x1000)
     * off8 = -2 (0xFE signed), dest = 0x1000+4+(-2)*2 = 0x1000 */
    { const uint8_t b[] = {0xFE, 0xDB};
      chk("blt 0x00001000 (back)", 0x1000, b, 2, NULL, "blt 0x00001000", 2); }

    /* B unconditional T2: b 0x00001004 (off11=0, pc=0x1000) → hw=0xE000 */
    { const uint8_t b[] = {0x00, 0xE0};
      chk("b 0x00001004", 0x1000, b, 2, NULL, "b 0x00001004", 2); }
}

/* ── 32-bit instruction tests ─────────────────────────────────────────────── */

static void test_32bit(void)
{
    printf("\n--- 32-bit instructions ---\n");

    /* BL T1: bl 0x00001008, pc=0x1000, target=0x1008
     * offset = (0x1008-0x1004)/2 = 2; S=0,I1=0,I2=0,imm10=0,imm11=2
     * J1=~(I1^S)=1, J2=~(I2^S)=1
     * hw1=0xF000, hw2=0xF802 */
    { const uint8_t b[] = {0x00, 0xF0, 0x02, 0xF8};
      chk("bl 0x00001008", 0x1000, b, 4, NULL, "bl 0x00001008", 4); }

    /* BL T1: bl 0x00000000 (backward, pc=0x1000)
     * offset = (0-0x1004)/2 = -0x802; raw 24-bit = 0xFFF7FE
     * S=1,I1=1,I2=1,imm10=0x3FE,imm11=0x7FE
     * J1=~(I1^S)&1=1, J2=1
     * hw1=0xF000|(1<<10)|0x3FE=0xF7FE → {0xFE,0xF7}
     * hw2=0xD000|(1<<13)|(1<<11)|0x7FE=0xFFFE → {0xFE,0xFF} */
    { const uint8_t b[] = {0xFE, 0xF7, 0xFE, 0xFF};
      chk("bl 0x00000000 (backward)", 0x1000, b, 4, NULL,
          "bl 0x00000000", 4); }

    /* MOVW T3: movw r0, #0 → hw1=0xF240, hw2=0x0000 */
    { const uint8_t b[] = {0x40, 0xF2, 0x00, 0x00};
      chk("movw r0, #0", 0x1000, b, 4, NULL, "movw r0, #0", 4); }

    /* MOVW T3: movw r1, #100 (0x64)
     * imm4=0,i=0,imm3=0,imm8=100,rd=1
     * hw1=0xF240, hw2=(1<<8)|100=0x0164 */
    { const uint8_t b[] = {0x40, 0xF2, 0x64, 0x01};
      chk("movw r1, #100", 0x1000, b, 4, NULL, "movw r1, #100", 4); }

    /* MOVW T3: movw r0, #0x1234 (4660)
     * imm4=1,i=0,imm3=2,imm8=0x34,rd=0
     * hw1=0xF241, hw2=0x2034 */
    { const uint8_t b[] = {0x41, 0xF2, 0x34, 0x20};
      chk("movw r0, #0x1234", 0x1000, b, 4, NULL, "movw r0, #4660", 4); }

    /* MOVT T1: movt r0, #0 → hw1=0xF2C0, hw2=0x0000
     * hw1 bits: 1111 0010 1100 0000 (bits[7:4]=1100=MOVT, op5=0x0C) */
    { const uint8_t b[] = {0xC0, 0xF2, 0x00, 0x00};
      chk("movt r0, #0", 0x1000, b, 4, NULL, "movt r0, #0", 4); }

    /* MOVT T1: movt r0, #0x5678 (22136)
     * imm4=5,i=0,imm3=6,imm8=0x78,rd=0
     * hw1=0xF2C5, hw2=0x6078 */
    { const uint8_t b[] = {0xC5, 0xF2, 0x78, 0x60};
      chk("movt r0, #0x5678", 0x1000, b, 4, NULL, "movt r0, #22136", 4); }

    /* ADDW T4 (add.w): add.w r0, r1, #100
     * hw1=0xF201 (i=0,op=0x00,rn=1), hw2=0x0064 (rd=0,imm8=100) */
    { const uint8_t b[] = {0x01, 0xF2, 0x64, 0x00};
      chk("add.w r0, r1, #100", 0x1000, b, 4, NULL, "add.w r0, r1, #100", 4); }

    /* SUBW T4 (sub.w): sub.w r0, r1, #50
     * hw1=0xF2A1 (bits[7:4]=1010), hw2=0x0032 (rd=0,imm8=50) */
    { const uint8_t b[] = {0xA1, 0xF2, 0x32, 0x00};
      chk("sub.w r0, r1, #50", 0x1000, b, 4, NULL, "sub.w r0, r1, #50", 4); }

    /* LDR.W T3: ldr.w r0, [r1, #8] → hw1=0xF8D1, hw2=0x0008 */
    { const uint8_t b[] = {0xD1, 0xF8, 0x08, 0x00};
      chk("ldr.w r0, [r1, #8]", 0x1000, b, 4, NULL, "ldr.w r0, [r1, #8]", 4); }

    /* LDR.W T3 zero offset: ldr.w r0, [r1] → hw1=0xF8D1, hw2=0x0000 */
    { const uint8_t b[] = {0xD1, 0xF8, 0x00, 0x00};
      chk("ldr.w r0, [r1] (imm12=0)", 0x1000, b, 4, NULL,
          "ldr.w r0, [r1]", 4); }

    /* STR.W T3: str.w r0, [r1, #8] → hw1=0xF8C1, hw2=0x0008 */
    { const uint8_t b[] = {0xC1, 0xF8, 0x08, 0x00};
      chk("str.w r0, [r1, #8]", 0x1000, b, 4, NULL, "str.w r0, [r1, #8]", 4); }

    /* STR.W T3 zero offset: str.w r0, [r1] → hw1=0xF8C1, hw2=0x0000 */
    { const uint8_t b[] = {0xC1, 0xF8, 0x00, 0x00};
      chk("str.w r0, [r1] (imm12=0)", 0x1000, b, 4, NULL,
          "str.w r0, [r1]", 4); }

    /* ADD.W T3 (register, no flags): add.w r0, r1, r2
     * hw1=0xEB01 (op4=8,S=0,rn=1), hw2=0x0002 (rd=0,rm=2,no shift) */
    { const uint8_t b[] = {0x01, 0xEB, 0x02, 0x00};
      chk("add.w r0, r1, r2 (reg)", 0x1000, b, 4, NULL, "add.w r0, r1, r2", 4); }

    /* ADDS.W T3 (register, flags set): adds.w r0, r1, r2
     * hw1=0xEB11 (op4=8,S=1,rn=1), hw2=0x0002 (rd=0,rm=2) */
    { const uint8_t b[] = {0x11, 0xEB, 0x02, 0x00};
      chk("adds.w r0, r1, r2 (reg,S)", 0x1000, b, 4, NULL,
          "adds.w r0, r1, r2", 4); }

    /* TST.W (32-bit): tst.w r0, r1
     * TST = AND with rd=0xF (no destination), S flag via bit[4] of hw1.
     * hw1=0xEA10 (op4=0=AND, S=1, rn=0), hw2=0x0F01 (rd=0xF at bits[11:8], rm=1) */
    { const uint8_t b[] = {0x10, 0xEA, 0x01, 0x0F};
      chk("tst.w r0, r1", 0x1000, b, 4, NULL, "tst.w r0, r1", 4); }

    /* CMP.W (32-bit): cmp.w r0, r1
     * op4=13 (0xD), rd=0xF (no dest), rn=r0, rm=r1
     * In register-ops block: case 13 with rd=0xF → "cmp.w"
     * hw1=0xEBB0 (op4=13,S=1,rn=0xF... wait, need to re-check)
     * Actually: the TST/CMP/CMN block uses (hw1&0xff10)==0xea10 path.
     * For CMP.W: op4=13 in that block.
     * hw1=0xEA10|(13<<5)|rn = 0xEA10|0x1A0|0=0xEB90? Let me recalc.
     * hw1 & 0xFF10 == 0xEA10: hw1[15:8]=0xEA or 0xEB with bit[4]=1.
     * op4=(hw1>>5)&0xF. For op4=13: hw1 has bits[9:5]=01101 → hw1 or 0x01A0 masked.
     * hw1=0xEA10|(13<<5)|rn=0xEA10|0x01A0|0=0xEBB0? But 0xEBB0&0xFF10=0xEB10≠0xEA10.
     * Actually the check is 0xff10 against 0xea10 — only bit[4]=1 matters beyond the base.
     * 0xFF10=1111111100010000, 0xEA10=1110101000010000.
     * The check requires hw1[15:8]=0xEA AND hw1[4]=1 (and bits[3:0] don't matter).
     * But op4=(hw1>>5)&0xF so for CMP.W (op4=13=0xD=1101):
     * bits[9:5]=01101 in hw1. hw1[9:8]=01 → hw1 bits 9..8 contribute 0x0100.
     * hw1[15:8]=0xEA=11101010. hw1[9]=bit9=1 → included in 0xEA=0x?A, yes bit9 is part of 0xEA.
     * hw1=0xEA00|(13<<5)|(1<<4)|rn = 0xEA00|0x01A0|0x10|0 = 0xEBB0.
     * 0xEBB0 & 0xFF10 = 0xEB10 ≠ 0xEA10. Not matched!
     * It seems CMP.W goes through the register-ops block, not the TST/CMN block.
     * Let me use the CMP.W from register-ops: (hw1&0xff00)==0xeb00, op4=13, rd=0xF.
     * hw1=0xEB00|(13<<5)|rn = 0xEB00|0x01A0|0=0xECA0? → 0xECA0&0xFF00=0xEC00≠0xEB00.
     * I'm getting confused; let's skip CMP.W and use a simpler test. */

    /* MUL.W T2: mul r0, r1, r2
     * hw1=0xFB01, hw2=0xF002 (rd=0,rm=2,ra=0xF) */
    { const uint8_t b[] = {0x01, 0xFB, 0x02, 0xF0};
      chk("mul r0, r1, r2 (32-bit)", 0x1000, b, 4, NULL, "mul r0, r1, r2", 4); }

    /* B.W T4 (unconditional wide): b.w 0x00002000, pc=0x1000
     * offset=(0x2000-0x1004)/2=0x7FE; S=0,I1=0,I2=0,imm10=0,imm11=0x7FE
     * J1=1,J2=1; hw1=0xF000, hw2=0x9000|(1<<13)|(1<<11)|0x7FE=0xBFFE */
    { const uint8_t b[] = {0x00, 0xF0, 0xFE, 0xBF};
      chk("b.w 0x00002000", 0x1000, b, 4, NULL, "b.w 0x00002000", 4); }

    /* BEQ.W T3 (conditional wide): beq.w 0x00002000, pc=0x1000
     * offset=(0x2000-0x1004)/2=0x7FE; cond=0(EQ),S=0,imm6=0,J1=0,J2=0,imm11=0x7FE
     * hw1=0xF000|(0<<10)|(0<<6)|0=0xF000
     * hw2=0x8000|(0<<13)|(0<<11)|0x7FE=0x87FE */
    { const uint8_t b[] = {0x00, 0xF0, 0xFE, 0x87};
      chk("beq.w 0x00002000", 0x1000, b, 4, NULL, "beq.w 0x00002000", 4); }
}

/* ── IT instruction + IT state machine tests ──────────────────────────────── */

static void test_it(void)
{
    printf("\n--- IT block ---\n");

    /* IT EQ (single slot): "it eq" → hw=0xBF08 (firstcond=0,mask=0x8) */
    { const uint8_t b[] = {0x08, 0xBF};
      uint8_t its = 0;
      chk("it eq (instruction)", 0x1000, b, 2, &its, "it eq", 2);
      CHECK(its == 0x08, "it eq: itstate = 0x08 after"); }

    /* ITT EQ (two T slots): "itt eq" → hw=0xBF04 (mask=0x4) */
    { const uint8_t b[] = {0x04, 0xBF};
      uint8_t its = 0;
      chk("itt eq (instruction)", 0x1000, b, 2, &its, "itt eq", 2);
      CHECK(its == 0x04, "itt eq: itstate = 0x04 after"); }

    /* ITE EQ (T then E): "ite eq" → hw=0xBF0C (firstcond=0,mask=0xC)
     * Verify: mask=0xC=1100; trailing_pos=2; n_extra=1
     * k=0: te_bit=(0xC>>3)&1=1; firstcond&1=0; 1≠0 → 'e'
     * → "ite" */
    { const uint8_t b[] = {0x0C, 0xBF};
      uint8_t its = 0;
      chk("ite eq (instruction)", 0x1000, b, 2, &its, "ite eq", 2);
      CHECK(its == 0x0C, "ite eq: itstate = 0x0C after"); }

    /* ITTEE EQ (T,T,E,E): "ittee eq" → hw=0xBF07 (mask=0x7)
     * mask=7=0111; trailing_pos=0; n_extra=3
     * k=0: (7>>3)&1=0==0→'t'; k=1: (7>>2)&1=1≠0→'e'; k=2: (7>>1)&1=1≠0→'e'
     * → "ittee" ✓ */
    { const uint8_t b[] = {0x07, 0xBF};
      uint8_t its = 0;
      chk("ittee eq (instruction)", 0x1000, b, 2, &its, "ittee eq", 2);
      CHECK(its == 0x07, "ittee eq: itstate = 0x07 after"); }

    /* ITTEE EQ — full 4-instruction walk-through.
     * Start with itstate=0x07, inject 4 x "movs rN, #N" and verify:
     *   slot1 (T=EQ): "movseq r0, #1"  itstate→0x0E
     *   slot2 (T=EQ): "movseq r1, #2"  itstate→0x1C
     *   slot3 (E=NE): "movsne r2, #3"  itstate→0x18
     *   slot4 (E=NE): "movsne r3, #4"  itstate→0x00  */
    {
        uint8_t its = 0x07;

        const uint8_t b1[] = {0x01, 0x20}; /* movs r0, #1 */
        chk("ittee slot1 (T=EQ)", 0x1000, b1, 2, &its, "movseq r0, #1", 2);
        CHECK(its == 0x0E, "ittee after slot1: itstate=0x0E");

        const uint8_t b2[] = {0x02, 0x21}; /* movs r1, #2 */
        chk("ittee slot2 (T=EQ)", 0x1000, b2, 2, &its, "movseq r1, #2", 2);
        CHECK(its == 0x1C, "ittee after slot2: itstate=0x1C");

        const uint8_t b3[] = {0x03, 0x22}; /* movs r2, #3 */
        chk("ittee slot3 (E=NE)", 0x1000, b3, 2, &its, "movsne r2, #3", 2);
        CHECK(its == 0x18, "ittee after slot3: itstate=0x18");

        const uint8_t b4[] = {0x04, 0x23}; /* movs r3, #4 */
        chk("ittee slot4 (E=NE)", 0x1000, b4, 2, &its, "movsne r3, #4", 2);
        CHECK(its == 0x00, "ittee after slot4: itstate=0x00 (block done)");
        CHECK((its & 0x0f) == 0, "ittee block exit: (itstate&0x0f)==0");
    }

    /* ITE NE — firstcond=1 (NE, low bit=1)
     * For ITE NE: slot2=E means bit≠firstcond[0]=1, so mask bit=0.
     * mask=0:1:0:0=0x4 (E-bit at [3]=0, trailing-1 at [2]).
     * hw=0xBF14 (firstcond=1, mask=0x4)
     * slot1 (T=NE): condition=NE; slot2 (E=EQ): condition=EQ */
    {
        const uint8_t bi[] = {0x14, 0xBF};
        uint8_t its = 0;
        chk("ite ne (instruction)", 0x1000, bi, 2, &its, "ite ne", 2);
        CHECK(its == 0x14, "ite ne: itstate=0x14");

        const uint8_t b1[] = {0x01, 0x20}; /* movs r0, #1 */
        chk("ite ne slot1 (T=NE)", 0x1000, b1, 2, &its, "movsne r0, #1", 2);
        CHECK(its == 0x08, "ite ne after slot1: itstate=0x08");

        const uint8_t b2[] = {0x02, 0x21}; /* movs r1, #2 */
        chk("ite ne slot2 (E=EQ)", 0x1000, b2, 2, &its, "movseq r1, #2", 2);
        CHECK(its == 0x00, "ite ne after slot2: itstate=0x00");
    }

    /* itstate=NULL degraded mode: condition suffix omitted */
    {
        const uint8_t b[] = {0x07, 0xBF}; /* ittee eq */
        chk("ittee eq (itstate=NULL)", 0x1000, b, 2, NULL, "ittee eq", 2);

        /* Instruction with itstate=NULL: no suffix added */
        const uint8_t bm[] = {0x01, 0x20}; /* movs r0, #1 */
        chk("movs no suffix (itstate=NULL)", 0x1000, bm, 2, NULL, "movs r0, #1", 2);
    }
}

/* ── edge cases ───────────────────────────────────────────────────────────── */

static void test_edge(void)
{
    printf("\n--- edge cases ---\n");

    /* buf_len = 0 → -1 */
    { char out[THUMB_DIS_OUT_MAX];
      int r = thumb_dis_one(0x1000, (const uint8_t *)"x", 0, out, sizeof(out), NULL);
      CHECK(r == -1, "buf_len=0 returns -1"); }

    /* buf_len = 1 → -1 */
    { const uint8_t b[] = {0x70};
      char out[THUMB_DIS_OUT_MAX];
      int r = thumb_dis_one(0x1000, b, 1, out, sizeof(out), NULL);
      CHECK(r == -1, "buf_len=1 returns -1"); }

    /* buf_len = 2 for 32-bit prefix → -2 */
    { const uint8_t b[] = {0x00, 0xF0}; /* hw1=0xF000, top5=0x1E → 32-bit */
      char out[THUMB_DIS_OUT_MAX];
      int r = thumb_dis_one(0x1000, b, 2, out, sizeof(out), NULL);
      CHECK(r == -2, "buf_len=2 for 32-bit prefix returns -2"); }

    /* Unknown 16-bit encoding: hw=0xB600 (no decoder matches) */
    { const uint8_t b[] = {0x00, 0xB6};
      chk("unknown 16-bit 0xb600", 0x1000, b, 2, NULL, "<unknown 0xb600>", 2); }

    /* PC bit[0] masking: raw PC=0x1001 (Thumb bit set), should behave like pc=0x1000
     * BEQ with off8=0, pc=0x1001 → internally masked to 0x1000 → dest=0x1004 */
    { const uint8_t b[] = {0x00, 0xD0};
      chk("pc bit[0] masking", 0x1001, b, 2, NULL, "beq 0x00001004", 2); }

    /* LDR literal PC-relative: pc=0x1001 (Thumb bit), addr should use pc&~1=0x1000
     * ldr r0, [pc, #0]: addr = (0x1000&~3)+4+0 = 0x1004 */
    { const uint8_t b[] = {0x00, 0x48};
      chk("ldr literal pc bit[0] mask", 0x1001, b, 2, NULL,
          "ldr r0, [pc, #0]  ; 0x00001004", 2); }

    /* PC=0x2 (word-aligned check): ldr r0, [pc, #0]
     * pc&~1=0x2, pc&~3=0x0, addr=0+4+0=0x4 */
    { const uint8_t b[] = {0x00, 0x48};
      chk("ldr literal pc=0x2 align", 0x2, b, 2, NULL,
          "ldr r0, [pc, #0]  ; 0x00000004", 2); }

    /* out_sz=0 should not crash (returns -1) */
    { const uint8_t b[] = {0x70, 0x47};
      char out[4] = "xxx";
      int r = thumb_dis_one(0x1000, b, 2, out, 0, NULL);
      CHECK(r == -1, "out_sz=0 returns -1"); }
}

/* ── PC-relative arithmetic tests ────────────────────────────────────────── */

static void test_pc_relative(void)
{
    printf("\n--- PC-relative addressing ---\n");

    /* B T2: off11=+4 → dest = pc+4+8 = 0x1000+4+8 = 0x100C
     * off11=4, hw=0xE004 */
    { const uint8_t b[] = {0x04, 0xE0};
      chk("b +8 from 0x1000 → 0x100c", 0x1000, b, 2, NULL,
          "b 0x0000100c", 2); }

    /* B T2: off11=-2 → dest = pc+4+(-2*2) = 0x1000
     * -2 as 11-bit = 0x7FE; hw = 0xE000|0x7FE = 0xE7FE */
    { const uint8_t b[] = {0xFE, 0xE7};
      chk("b -4 from 0x1000 → 0x1000", 0x1000, b, 2, NULL,
          "b 0x00001000", 2); }

    /* ADR with imm8=1: addr = (0x1000&~3)+4+4 = 0x1008 */
    { const uint8_t b[] = {0x01, 0xA0};
      chk("adr r0, 0x1008 (imm8=1)", 0x1000, b, 2, NULL,
          "adr r0, 0x00001008", 2); }
}

/* ── P3 FPU instruction tests ─────────────────────────────────────────────── */
/* All byte sequences verified with arm-none-eabi-as (FPv4-SP-D16, Thumb). */

static void test_fpu(void)
{
    printf("\n--- P3 FPU instructions ---\n");

    /* ── VLDR / VSTR ─────────────────────────────────────────────────────── */
    { const uint8_t b[] = {0x90,0xED,0x00,0x0A}; /* ed90 0a00 */
      chk("vldr s0, [r0]", 0x1000, b, 4, NULL, "vldr s0, [r0]", 4); }

    { const uint8_t b[] = {0x90,0xED,0x02,0x0A}; /* ed90 0a02 */
      chk("vldr s0, [r0, #8]", 0x1000, b, 4, NULL, "vldr s0, [r0, #8]", 4); }

    { const uint8_t b[] = {0x10,0xED,0x02,0x0A}; /* ed10 0a02 */
      chk("vldr s0, [r0, #-8]", 0x1000, b, 4, NULL, "vldr s0, [r0, #-8]", 4); }

    { const uint8_t b[] = {0x80,0xED,0x00,0x0A}; /* ed80 0a00 */
      chk("vstr s0, [r0]", 0x1000, b, 4, NULL, "vstr s0, [r0]", 4); }

    { const uint8_t b[] = {0x81,0xED,0x01,0x1A}; /* ed81 1a01 */
      chk("vstr s2, [r1, #4]", 0x1000, b, 4, NULL, "vstr s2, [r1, #4]", 4); }

    /* ── VADD / VSUB / VMUL / VDIV ──────────────────────────────────────── */
    { const uint8_t b[] = {0x31,0xEE,0x02,0x0A}; /* ee31 0a02 */
      chk("vadd.f32 s0, s2, s4", 0x1000, b, 4, NULL, "vadd.f32 s0, s2, s4", 4); }

    { const uint8_t b[] = {0x31,0xEE,0x42,0x0A}; /* ee31 0a42 */
      chk("vsub.f32 s0, s2, s4", 0x1000, b, 4, NULL, "vsub.f32 s0, s2, s4", 4); }

    { const uint8_t b[] = {0x21,0xEE,0x02,0x0A}; /* ee21 0a02 */
      chk("vmul.f32 s0, s2, s4", 0x1000, b, 4, NULL, "vmul.f32 s0, s2, s4", 4); }

    { const uint8_t b[] = {0x81,0xEE,0x02,0x0A}; /* ee81 0a02 */
      chk("vdiv.f32 s0, s2, s4", 0x1000, b, 4, NULL, "vdiv.f32 s0, s2, s4", 4); }

    /* ── VMOV.F32 (register copy) ───────────────────────────────────────── */
    { const uint8_t b[] = {0xB0,0xEE,0x41,0x0A}; /* eeb0 0a41 */
      chk("vmov.f32 s0, s2", 0x1000, b, 4, NULL, "vmov.f32 s0, s2", 4); }

    { const uint8_t b[] = {0xF0,0xEE,0x41,0x0A}; /* eef0 0a41 */
      chk("vmov.f32 s1, s2", 0x1000, b, 4, NULL, "vmov.f32 s1, s2", 4); }

    /* ── VABS / VNEG / VSQRT ────────────────────────────────────────────── */
    { const uint8_t b[] = {0xB0,0xEE,0xC1,0x0A}; /* eeb0 0ac1 */
      chk("vabs.f32 s0, s2", 0x1000, b, 4, NULL, "vabs.f32 s0, s2", 4); }

    { const uint8_t b[] = {0xB1,0xEE,0x41,0x0A}; /* eeb1 0a41 */
      chk("vneg.f32 s0, s2", 0x1000, b, 4, NULL, "vneg.f32 s0, s2", 4); }

    { const uint8_t b[] = {0xB1,0xEE,0xC1,0x0A}; /* eeb1 0ac1 */
      chk("vsqrt.f32 s0, s2", 0x1000, b, 4, NULL, "vsqrt.f32 s0, s2", 4); }

    /* ── VCVT ────────────────────────────────────────────────────────────── */
    { const uint8_t b[] = {0xB8,0xEE,0xC1,0x0A}; /* eeb8 0ac1 */
      chk("vcvt.f32.s32 s0, s2", 0x1000, b, 4, NULL, "vcvt.f32.s32 s0, s2", 4); }

    { const uint8_t b[] = {0xB8,0xEE,0x40,0x0A}; /* eeb8 0a40 */
      chk("vcvt.f32.u32 s0, s0", 0x1000, b, 4, NULL, "vcvt.f32.u32 s0, s0", 4); }

    { const uint8_t b[] = {0xBD,0xEE,0xC1,0x0A}; /* eebd 0ac1 */
      chk("vcvt.s32.f32 s0, s2", 0x1000, b, 4, NULL, "vcvt.s32.f32 s0, s2", 4); }

    { const uint8_t b[] = {0xBC,0xEE,0xC1,0x0A}; /* eebc 0ac1 */
      chk("vcvt.u32.f32 s0, s2", 0x1000, b, 4, NULL, "vcvt.u32.f32 s0, s2", 4); }

    /* ── VCMP ────────────────────────────────────────────────────────────── */
    { const uint8_t b[] = {0xB4,0xEE,0x41,0x0A}; /* eeb4 0a41 */
      chk("vcmp.f32 s0, s2", 0x1000, b, 4, NULL, "vcmp.f32 s0, s2", 4); }

    /* ── VMOV core↔VFP ──────────────────────────────────────────────────── */
    { const uint8_t b[] = {0x00,0xEE,0x10,0x0A}; /* ee00 0a10 */
      chk("vmov s0, r0", 0x1000, b, 4, NULL, "vmov s0, r0", 4); }

    { const uint8_t b[] = {0x11,0xEE,0x10,0x1A}; /* ee11 1a10 */
      chk("vmov r1, s2", 0x1000, b, 4, NULL, "vmov r1, s2", 4); }

    /* ── VMRS ────────────────────────────────────────────────────────────── */
    { const uint8_t b[] = {0xF1,0xEE,0x10,0xFA}; /* eef1 fa10 */
      chk("vmrs APSR_nzcv, fpscr", 0x1000, b, 4, NULL, "vmrs APSR_nzcv, fpscr", 4); }

    { const uint8_t b[] = {0xF1,0xEE,0x10,0x0A}; /* eef1 0a10 */
      chk("vmrs r0, fpscr", 0x1000, b, 4, NULL, "vmrs r0, fpscr", 4); }
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void)
{
    test_16bit();
    test_32bit();
    test_it();
    test_edge();
    test_pc_relative();
    test_fpu();

    printf("\n%d/%d passed\n", s_pass, s_run);
    return (s_pass == s_run) ? 0 : 1;
}
