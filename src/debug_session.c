/* debug_session.c — Live debug session over WireTransport (RSP client)
 *
 * SPDX-License-Identifier: MIT
 */

#include "debug_session.h"
#include "arch.h"
#include "transport.h"
#include "rsp_transport.h"
#include "uart_transport.h"
#include "wire_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Internal state ──────────────────────────────────────────────────────── */

struct DebugSession {
    WireTransport   *transport;    /* byte-stream backend (UART or probe) */
    int              last_signal;  /* GDB signal from most recent stop reply */
    const MkdbgArch *arch;         /* active architecture (live_debug guaranteed non-NULL) */
};

/* ── Hex helpers (local) ─────────────────────────────────────────────────── */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse the signal number from an RSP stop reply ("S05", "T05…"). */
static int parse_stop_signal(const char *reply)
{
    if (reply[0] != 'S' && reply[0] != 'T') return -1;
    int hi = hex_nibble(reply[1]);
    int lo = hex_nibble(reply[2]);
    if (hi < 0 || lo < 0) return -1;
    return (hi << 4) | lo;
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/*
 * Open a session from a pre-constructed transport.  Takes ownership of t;
 * transport_destroy() is called on debug_session_close().
 * Used by debug_session_open() (UART) and in PR-3 by probe_transport.
 */
DebugSession *debug_session_open_transport(WireTransport *t,
                                            const MkdbgArch *arch)
{
    if (!t || !arch) return NULL;
    DebugSession *s = malloc(sizeof(DebugSession));
    if (!s) return NULL;
    s->transport   = t;
    s->last_signal = 0;
    s->arch        = arch;
    return s;
}

DebugSession *debug_session_open(const char *port, int baud,
                                  const MkdbgArch *arch)
{
    WireTransport *t = uart_transport_open(port, baud);
    if (!t) return NULL;

    DebugSession *s = debug_session_open_transport(t, arch);
    if (!s) { transport_destroy(t); return NULL; }
    return s;
}

void debug_session_close(DebugSession *s)
{
    if (!s) return;
    transport_destroy(s->transport);
    free(s);
}

/* ── Execution control ───────────────────────────────────────────────────── */

int debug_session_continue(DebugSession *s)
{
    char resp[16];

    /* 'c' gets S00 immediately (MCU resuming); then wait for halt stop reply. */
    int rc = rsp_transaction_t(s->transport, "c", resp, sizeof(resp));
    if (rc != WIRE_OK) return rc;

    char stop[16];
    rc = rsp_wait_for_stop_t(s->transport, stop, sizeof(stop));
    if (rc == WIRE_OK)
        s->last_signal = parse_stop_signal(stop);
    return rc;
}

int debug_session_detach(DebugSession *s)
{
    /* Send 'c' but do NOT wait — leave the MCU running and return. */
    return rsp_send_packet_t(s->transport, "c");
}

int debug_session_interrupt(DebugSession *s)
{
    /* Send raw Ctrl-C (0x03) — not an RSP packet, just one byte.
     * The firmware's wire_poll_break_in() detects it and pends DebugMonitor. */
    uint8_t ctrlc = 0x03u;
    int n = s->transport->write(s->transport->ctx, &ctrlc, 1);
    if (n != 1) return WIRE_ERR_IO;

    char stop[16];
    int rc = rsp_wait_for_stop_t(s->transport, stop, sizeof(stop));
    if (rc == WIRE_OK)
        s->last_signal = parse_stop_signal(stop);
    return rc;
}

int debug_session_step(DebugSession *s)
{
    /* 's' has NO immediate reply per RSP spec — send directly, not via
     * rsp_transaction_t().  The stop-reply S05 arrives when DebugMonitor
     * fires after the CPU executes one instruction. */
    int rc = rsp_send_packet_t(s->transport, "s");
    if (rc != WIRE_OK) return rc;

    char stop[16];
    rc = rsp_wait_for_stop_t(s->transport, stop, sizeof(stop));
    if (rc == WIRE_OK)
        s->last_signal = parse_stop_signal(stop);
    return rc;
}

/* ── Breakpoints ─────────────────────────────────────────────────────────── */

int debug_session_set_hw_breakpoint(DebugSession *s, uint32_t addr)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "Z1,%x,4", addr);

    char resp[16];
    int rc = rsp_transaction_t(s->transport, cmd, resp, sizeof(resp));
    if (rc != WIRE_OK) return rc;
    return (strcmp(resp, "OK") == 0) ? WIRE_OK : WIRE_ERR_IO;
}

int debug_session_clear_hw_breakpoint(DebugSession *s, uint32_t addr)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "z1,%x,4", addr);

    char resp[16];
    int rc = rsp_transaction_t(s->transport, cmd, resp, sizeof(resp));
    if (rc != WIRE_OK) return rc;
    return (strcmp(resp, "OK") == 0) ? WIRE_OK : WIRE_ERR_IO;
}

/* ── DWT Hardware Watchpoints ────────────────────────────────────────────── */

int debug_session_set_watchpoint(DebugSession *s, uint32_t addr,
                                  uint32_t len, WatchpointType type)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "Z%d,%x,%x", (int)type, addr, len ? len : 1);

    char resp[16];
    int rc = rsp_transaction_t(s->transport, cmd, resp, sizeof(resp));
    if (rc != WIRE_OK) return rc;
    return (strcmp(resp, "OK") == 0) ? WIRE_OK : WIRE_ERR_IO;
}

int debug_session_clear_watchpoint(DebugSession *s, uint32_t addr)
{
    /* Try all three types (write/read/access) — firmware matches by address. */
    char cmd[32];
    char resp[16];
    int types[3] = { WATCHPOINT_WRITE, WATCHPOINT_READ, WATCHPOINT_ACCESS };
    for (int i = 0; i < 3; i++) {
        snprintf(cmd, sizeof(cmd), "z%d,%x,1", types[i], addr);
        int rc = rsp_transaction_t(s->transport, cmd, resp, sizeof(resp));
        if (rc == WIRE_OK && strcmp(resp, "OK") == 0) return WIRE_OK;
    }
    return WIRE_ERR_IO;
}

/* ── Register / memory inspection ───────────────────────────────────────── */

int debug_session_read_regs(DebugSession *s, uint32_t regs[DEBUG_SESSION_MAX_REGS])
{
    int nregs = s->arch->live_debug->nregs;
    /* RSP 'g': nregs registers × 8 hex chars each, little-endian. */
    char resp[DEBUG_SESSION_MAX_REGS * 8 + 4];
    int rc = rsp_transaction_t(s->transport, "g", resp, sizeof(resp));
    if (rc != WIRE_OK) return rc;
    if (resp[0] == 'E') return WIRE_ERR_IO;

    for (int i = 0; i < nregs; i++) {
        const char *p = resp + i * 8;
        uint32_t v = 0;
        for (int b = 0; b < 4; b++) {
            int hi = hex_nibble(p[b * 2]);
            int lo = hex_nibble(p[b * 2 + 1]);
            if (hi < 0 || lo < 0) return WIRE_ERR_PARSE;
            v |= (uint32_t)((hi << 4) | lo) << (b * 8);  /* little-endian */
        }
        regs[i] = v;
    }
    return WIRE_OK;
}

int debug_session_read_mem(DebugSession *s, uint32_t addr, size_t len, uint8_t *out)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "m%x,%zx", addr, len);

    /* Each byte encoded as 2 hex chars; add space for 'E' error replies. */
    char resp[4096];
    if (len * 2 + 4 > sizeof(resp)) return WIRE_ERR_OVERFLOW;

    int rc = rsp_transaction_t(s->transport, cmd, resp, sizeof(resp));
    if (rc != WIRE_OK) return rc;
    if (resp[0] == 'E') return WIRE_ERR_IO;

    for (size_t i = 0; i < len; i++) {
        int hi = hex_nibble(resp[i * 2]);
        int lo = hex_nibble(resp[i * 2 + 1]);
        if (hi < 0 || lo < 0) return WIRE_ERR_PARSE;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return WIRE_OK;
}

int debug_session_write_mem(DebugSession *s, uint32_t addr, size_t len,
                             const uint8_t *data)
{
    /* RSP 'M addr,len:hexdata' — hexdata is one byte per two hex chars. */
    static const char hex[] = "0123456789abcdef";
    char cmd[32 + len * 2 + 1];  /* header + hex payload + NUL */
    int hdr = snprintf(cmd, 32, "M%x,%zx:", addr, len);
    if (hdr < 0 || hdr >= 32) return WIRE_ERR_OVERFLOW;
    for (size_t i = 0; i < len; i++) {
        cmd[hdr + i * 2]     = hex[data[i] >> 4];
        cmd[hdr + i * 2 + 1] = hex[data[i] & 0xf];
    }
    cmd[hdr + len * 2] = '\0';

    char resp[16];
    int rc = rsp_transaction_t(s->transport, cmd, resp, sizeof(resp));
    if (rc != WIRE_OK) return rc;
    return (strcmp(resp, "OK") == 0) ? WIRE_OK : WIRE_ERR_IO;
}

int debug_session_reset(DebugSession *s)
{
    /* RSP 'R' packet: software system reset.
     * The MCU resets immediately and sends NO reply — use rsp_send_packet_t,
     * not rsp_transaction_t, to avoid blocking on a reply that never arrives. */
    return rsp_send_packet_t(s->transport, "R00");
}

/* ── Arch metadata ───────────────────────────────────────────────────────── */

int debug_session_nregs(const DebugSession *s)
{
    return s ? s->arch->live_debug->nregs : 0;
}

const char *debug_session_reg_name(const DebugSession *s, int i)
{
    if (!s || i < 0 || i >= s->arch->live_debug->nregs) return "??";
    return s->arch->live_debug->reg_names[i];
}

int debug_session_pc_reg(const DebugSession *s)
{
    return s ? s->arch->live_debug->pc_reg_idx : 0;
}

int debug_session_fp_reg(const DebugSession *s)
{
    return s ? s->arch->live_debug->fp_reg_idx : -1;
}

/* ── Status ──────────────────────────────────────────────────────────────── */

int debug_session_last_signal(const DebugSession *s)
{
    return s ? s->last_signal : -1;
}
