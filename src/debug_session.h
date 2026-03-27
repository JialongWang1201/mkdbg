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

/* Opaque session handle. */
typedef struct DebugSession DebugSession;

/* Number of Cortex-M registers returned by debug_session_read_regs().
 * Order: r0-r12, sp(r13), lr(r14), pc(r15), xpsr  — matches RSP 'g' layout. */
#define DEBUG_SESSION_NREGS 17

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/* Open UART port and return a new session.  baud=0 for PTY (QEMU).
 * Returns NULL on error (error printed to stderr). */
DebugSession *debug_session_open(const char *port, int baud);

/* Close UART and free the session. */
void debug_session_close(DebugSession *s);

/* ── Execution control ───────────────────────────────────────────────────── */

/* Resume MCU execution; block until the next halt (breakpoint or step).
 * Returns WIRE_OK on halt, WIRE_ERR_TIMEOUT if no halt within 60 s. */
int debug_session_continue(DebugSession *s);

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

/* ── Register / memory inspection ───────────────────────────────────────── */

/* Read all CPU registers into regs[DEBUG_SESSION_NREGS].
 * Indices: 0-12 = r0-r12, 13 = sp, 14 = lr, 15 = pc, 16 = xpsr. */
int debug_session_read_regs(DebugSession *s, uint32_t regs[DEBUG_SESSION_NREGS]);

/* Read len bytes from target address addr into out.
 * Returns WIRE_OK, or WIRE_ERR_IO if address is out of the allowed RAM range. */
int debug_session_read_mem(DebugSession *s, uint32_t addr, size_t len, uint8_t *out);

/* ── Status ──────────────────────────────────────────────────────────────── */

/* GDB signal number from the most recent stop reply (5=SIGTRAP, 11=SIGSEGV).
 * Valid after debug_session_continue() or debug_session_step() returns WIRE_OK. */
int debug_session_last_signal(const DebugSession *s);

#endif /* DEBUG_SESSION_H */
