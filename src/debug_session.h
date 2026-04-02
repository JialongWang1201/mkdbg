/* debug_session.h — Live debug session over UART (RSP client)
 *
 * Wraps wire_rsp_client into a high-level API for breakpoint/step/inspect.
 * Used by debug_cli.c (PR 9) and debug_tui.c (PR 11).
 *
 * Target requirements:
 *   - Cortex-M3/M4 firmware built with WIRE_LIVE_DEBUG=1
 *   - DebugMonitor priority not masked by BASEPRI/PRIMASK
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef DEBUG_SESSION_H
#define DEBUG_SESSION_H

#include <stddef.h>
#include <stdint.h>

/* Forward-declare MkdbgArch to avoid circular includes. */
typedef struct MkdbgArch MkdbgArch;

/* Opaque session handle. */
typedef struct DebugSession DebugSession;

/* Maximum registers any supported arch can return from RSP 'g'.
 * Cortex-M: 17.  RISC-V 32: 33.  Increase if a future arch needs more. */
#define DEBUG_SESSION_MAX_REGS 64

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/* Open UART port and return a new session.  baud=0 for PTY (QEMU).
 * arch must point to an MkdbgArch with a valid live_debug descriptor.
 * Returns NULL on error (error printed to stderr). */
DebugSession *debug_session_open(const char *port, int baud,
                                  const MkdbgArch *arch);

/* Close UART and free the session. */
void debug_session_close(DebugSession *s);

/* ── Execution control ───────────────────────────────────────────────────── */

/* Resume MCU execution; block until the next halt (breakpoint or step).
 * Returns WIRE_OK on halt, WIRE_ERR_TIMEOUT if no halt within 60 s. */
int debug_session_continue(DebugSession *s);

/* Send 'c' and return immediately (do not wait for a stop reply).
 * Used on quit to leave the MCU running. */
int debug_session_detach(DebugSession *s);

/* Send a Ctrl-C (0x03) break-in byte to the MCU and wait for the halt
 * stop reply.  The firmware must be polling wire_poll_break_in() from its
 * main loop for this to work.  Returns WIRE_OK on halt,
 * WIRE_ERR_TIMEOUT if the MCU does not halt within 60 s. */
int debug_session_interrupt(DebugSession *s);

/* Execute one instruction; block until DebugMonitor fires.
 * Returns WIRE_OK on halt, WIRE_ERR_TIMEOUT if step takes > 60 s. */
int debug_session_step(DebugSession *s);

/* ── Breakpoints ─────────────────────────────────────────────────────────── */

/* Set FPBv1 hardware breakpoint at addr.
 * Returns WIRE_OK, or WIRE_ERR_IO if all comparators are occupied. */
int debug_session_set_hw_breakpoint(DebugSession *s, uint32_t addr);

/* Clear hardware breakpoint previously set at addr.
 * Returns WIRE_OK, or WIRE_ERR_IO if addr was not an active breakpoint. */
int debug_session_clear_hw_breakpoint(DebugSession *s, uint32_t addr);

/* ── DWT Hardware Watchpoints ────────────────────────────────────────────── */

/* Watchpoint type matches GDB RSP Z-packet type codes. */
typedef enum {
    WATCHPOINT_WRITE  = 2,  /* Z2 — halt on write to addr */
    WATCHPOINT_READ   = 3,  /* Z3 — halt on read from addr */
    WATCHPOINT_ACCESS = 4,  /* Z4 — halt on read or write */
} WatchpointType;

/* Set a DWT hardware watchpoint at addr (len bytes, exact match when len=1).
 * Returns WIRE_OK, or WIRE_ERR_IO if all DWT comparators are occupied. */
int debug_session_set_watchpoint(DebugSession *s, uint32_t addr,
                                  uint32_t len, WatchpointType type);

/* Clear DWT watchpoint at addr.
 * Returns WIRE_OK, or WIRE_ERR_IO if addr was not an active watchpoint. */
int debug_session_clear_watchpoint(DebugSession *s, uint32_t addr);

/* ── Register / memory inspection ───────────────────────────────────────── */

/* Read all CPU registers into regs[].  The number of registers is arch-dependent;
 * use debug_session_nregs() to find out how many were written.
 * regs must have room for at least DEBUG_SESSION_MAX_REGS elements. */
int debug_session_read_regs(DebugSession *s, uint32_t regs[DEBUG_SESSION_MAX_REGS]);

/* Read len bytes from target address addr into out.
 * Returns WIRE_OK, or WIRE_ERR_IO if address is out of the allowed RAM range. */
int debug_session_read_mem(DebugSession *s, uint32_t addr, size_t len, uint8_t *out);

/* Write len bytes of data to target address addr.
 * Returns WIRE_OK, or WIRE_ERR_IO on target error or bounds violation. */
int debug_session_write_mem(DebugSession *s, uint32_t addr, size_t len,
                             const uint8_t *data);

/* Trigger a software system reset (RSP 'R' packet).
 * Send-only — does NOT wait for a reply (the MCU resets immediately).
 * Returns WIRE_OK if the packet was transmitted, WIRE_ERR_IO on UART error. */
int debug_session_reset(DebugSession *s);

/* ── Arch metadata ───────────────────────────────────────────────────────── */

/* Number of registers returned by debug_session_read_regs() for this session. */
int debug_session_nregs(const DebugSession *s);

/* Register name at index i (e.g. "r0", "pc", "x2").  Returns "??" if out of range. */
const char *debug_session_reg_name(const DebugSession *s, int i);

/* Index of the PC register (e.g. 15 for Cortex-M, 32 for RISC-V). */
int debug_session_pc_reg(const DebugSession *s);

/* ── Status ──────────────────────────────────────────────────────────────── */

/* GDB signal number from the most recent stop reply (5=SIGTRAP, 11=SIGSEGV).
 * Valid after debug_session_continue() or debug_session_step() returns WIRE_OK. */
int debug_session_last_signal(const DebugSession *s);

#endif /* DEBUG_SESSION_H */
