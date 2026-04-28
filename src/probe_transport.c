/* probe_transport.c — WireTransport adapter backed by probe-bridge.
 *
 * Wraps the five probe_bridge C FFI functions (probe_open / probe_read /
 * probe_write / probe_close) as a WireTransport so that debug_session.c's
 * RSP client works identically over UART or a hardware debug probe.
 *
 * Compiled only when MKDBG_PROBE_SUPPORT is defined by CMakeLists.txt.
 *
 * SPDX-License-Identifier: MIT
 */

#include "probe_transport.h"
#include "probe_bridge.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* ── WireTransport callbacks ─────────────────────────────────────────────── */

static int pt_read(void *ctx, uint8_t *buf, int len, int timeout_ms)
{
    return probe_read((ProbeHandle *)ctx, buf, len, timeout_ms);
}

static int pt_write(void *ctx, const uint8_t *buf, int len)
{
    return probe_write((ProbeHandle *)ctx, buf, len);
}

static void pt_close(void *ctx)
{
    probe_close((ProbeHandle *)ctx);
}

/* ── public API ──────────────────────────────────────────────────────────── */

WireTransport *probe_transport_open(int probe_idx, const char *chip)
{
    ProbeHandle *h = probe_open(probe_idx, chip);
    if (!h) {
        fprintf(stderr, "mkdbg: probe_open(%d, %s) failed\n",
                probe_idx, chip ? chip : "auto");
        return NULL;
    }

    WireTransport *t = malloc(sizeof(WireTransport));
    if (!t) {
        probe_close(h);
        return NULL;
    }

    t->read  = pt_read;
    t->write = pt_write;
    t->close = pt_close;
    t->ctx   = h;
    return t;
}
