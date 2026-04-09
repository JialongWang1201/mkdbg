/* thumb_dis.h — Thumb/Thumb-2 disassembler for Cortex-M
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef THUMB_DIS_H
#define THUMB_DIS_H

#include <stddef.h>
#include <stdint.h>

/* Minimum output buffer size guaranteed to hold any P1/P2/P3 mnemonic. */
#define THUMB_DIS_OUT_MAX 96

/* Disassemble one Thumb/Thumb-2 instruction.
 *
 * pc      - address of this instruction (raw Cortex-M PC accepted;
 *           bit[0] is masked internally before any PC-relative arithmetic).
 * buf     - pointer to instruction bytes (little-endian).
 * buf_len - available bytes in buf.
 * out     - output buffer, must be >= THUMB_DIS_OUT_MAX bytes.
 * out_sz  - size of out (pass THUMB_DIS_OUT_MAX or larger).
 * itstate - IT execution-state byte (in/out).  Caller initialises to 0
 *           before the first call and passes the same pointer on every
 *           subsequent call.  The function updates *itstate after each
 *           instruction to track IT-block state.
 *           Pass NULL to disable IT-block condition tracking (unit-test
 *           degraded mode — conditional suffixes are omitted).
 *
 * itstate semantics (ARM DDI0403 A7.7.37):
 *   ITSTATE[7:4] = condition code for the current IT-block instruction.
 *   ITSTATE[3:0] = remaining T/E mask; block is active while this != 0.
 *
 *   Advance after each instruction inside an IT block:
 *     if (itstate & 0x07) itstate = (itstate & 0xe0) | ((itstate << 1) & 0x1f);
 *     else                itstate = 0;
 *
 *   Exit condition: (itstate & 0x0f) == 0.
 *   Do NOT use (itstate == 0) — the high nibble may still be non-zero.
 *
 * Return values:
 *   2 or 4  instruction length in bytes (consumed from buf).
 *  -1       unknown encoding; caller should advance by 2 bytes and retry.
 *  -2       buf_len < 4 for a detected 32-bit instruction prefix;
 *           caller should fetch 2 more bytes and retry.
 */
int thumb_dis_one(uint32_t pc, const uint8_t *buf, size_t buf_len,
                  char *out, size_t out_sz,
                  uint8_t *itstate);

#endif /* THUMB_DIS_H */
