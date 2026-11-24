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

int probe_transport_select(int requested)
{
    ProbeInfo probes[16];
    const int capacity = (int)(sizeof(probes) / sizeof(probes[0]));
    int count = probe_list(probes, capacity);
    if (count < 0) {
        fprintf(stderr, "mkdbg: probe enumeration failed\n");
        return -1;
    }
    if (count == 0) {
        fprintf(stderr, "mkdbg: no debug probe detected\n");
        return -1;
    }
    if (requested >= count) {
        fprintf(stderr, "mkdbg: --probe %d out of range (%d probe(s) found)\n",
                requested, count);
        return -1;
    }
    if (requested >= 0) return requested;
    if (count == 1) return 0;

    fprintf(stderr, "mkdbg: %d probes found — select one with --probe N:\n", count);
    for (int i = 0; i < count && i < capacity; i++) {
        fprintf(stderr, "  [%d] %s  serial: %s\n", i,
                probes[i].identifier[0] ? probes[i].identifier : "unknown",
                probes[i].serial[0] ? probes[i].serial : "(none)");
    }
    return -1;
}

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
    ProbeCapabilities caps = {0};
    if (probe_get_capabilities(h, &caps) != 0) {
        free(t);
        probe_close(h);
        return NULL;
    }
    t->capabilities = caps.flags;
    t->register_count = (int)caps.register_count;
    return t;
}
