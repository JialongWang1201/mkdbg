/* rsp_transport.c — GDB RSP packet framing over WireTransport.
 *
 * Re-implements the RSP client (deps/wire/host/wire_rsp_client.c) using
 * WireTransport callbacks.  The fd-based RSP client is still used by the crash
 * dump path (wire_dump_crash_to_buf); this file is for the live debug session.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "rsp_transport.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define RSP_BUF_MAX        4096
#define RSP_MAX_RETRIES       3
#define RSP_TIMEOUT_MS     2000
#define RSP_STOP_TIMEOUT_MS 60000

typedef struct {
    int (*interrupt_requested)(void *);
    void *ctx;
    int interrupt_sent;
    int64_t deadline_ms;
} StopWait;

static int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* ── helpers ─────────────────────────────────────────────────────────────── */

static uint8_t rsp_checksum(const char *data, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum = (uint8_t)(sum + (uint8_t)data[i]);
    return sum;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * Read exactly one byte with timeout_ms via transport.
 * Returns 1 on success, WIRE_ERR_TIMEOUT on timeout, WIRE_ERR_IO on error.
 */
static int read_byte_t(WireTransport *t, uint8_t *out, int timeout_ms)
{
    int r = t->read(t->ctx, out, 1, timeout_ms);
    if (r == 1) return 1;
    if (r == TRANSPORT_ERR_TIMEOUT) return WIRE_ERR_TIMEOUT;
    return WIRE_ERR_IO;
}

/*
 * Write one byte via transport.  Returns WIRE_OK or WIRE_ERR_IO.
 */
static int write_byte_t(WireTransport *t, uint8_t b)
{
    int r = t->write(t->ctx, &b, 1);
    return (r == 1) ? WIRE_OK : WIRE_ERR_IO;
}

/* Keep the packet parser's state across idle periods, including partial
 * packets. Only this thread reads or writes the transport. */
static int read_wait_byte(WireTransport *t, uint8_t *out, StopWait *wait)
{
    if (!wait) return read_byte_t(t, out, RSP_TIMEOUT_MS);
    int poll_ms = wait->interrupt_requested ? 100 : RSP_TIMEOUT_MS;
    for (;;) {
        int64_t now = monotonic_ms();
        if (now < 0) return WIRE_ERR_IO;
        if (!wait->interrupt_sent && wait->interrupt_requested &&
            wait->interrupt_requested(wait->ctx)) {
            if (write_byte_t(t, 0x03) != WIRE_OK) return WIRE_ERR_IO;
            wait->interrupt_sent = 1;
            wait->deadline_ms = now + RSP_STOP_TIMEOUT_MS;
        }
        int64_t remaining = wait->deadline_ms - now;
        if (remaining <= 0) return WIRE_ERR_TIMEOUT;
        int timeout_ms = remaining < poll_ms ? (int)remaining : poll_ms;
        int rc = read_byte_t(t, out, timeout_ms);
        if (rc != WIRE_ERR_TIMEOUT) return rc;
    }
}

/* ── send ────────────────────────────────────────────────────────────────── */

int rsp_send_packet_t(WireTransport *t, const char *data)
{
    size_t  dlen = strlen(data);
    uint8_t sum  = rsp_checksum(data, dlen);
    char    buf[RSP_BUF_MAX + 8];
    int     n    = snprintf(buf, sizeof(buf), "$%s#%02x", data, sum);
    if (n < 0 || (size_t)n >= sizeof(buf)) return WIRE_ERR_OVERFLOW;

    int r = t->write(t->ctx, (const uint8_t *)buf, (int)n);
    return (r == n) ? WIRE_OK : WIRE_ERR_IO;
}

/* ── receive ─────────────────────────────────────────────────────────────── */

static int rsp_recv_packet_t(WireTransport *t, char *out_buf, size_t out_size,
                             StopWait *wait)
{
    uint8_t c;

    /* drain until '$' */
    for (;;) {
        int r = read_wait_byte(t, &c, wait);
        if (r == WIRE_ERR_IO)      return WIRE_ERR_IO;
        if (r == WIRE_ERR_TIMEOUT) return WIRE_ERR_TIMEOUT;
        if (c == '$') break;
    }

    /* accumulate data until '#' */
    size_t  dlen        = 0;
    uint8_t running_sum = 0;
    for (;;) {
        int r = read_wait_byte(t, &c, wait);
        if (r == WIRE_ERR_IO)      return WIRE_ERR_IO;
        if (r == WIRE_ERR_TIMEOUT) return WIRE_ERR_TIMEOUT;
        if (c == '#') break;
        if (dlen + 1 >= out_size) {
            write_byte_t(t, '-');
            return WIRE_ERR_OVERFLOW;
        }
        out_buf[dlen++] = (char)c;
        running_sum     = (uint8_t)(running_sum + c);
    }
    out_buf[dlen] = '\0';

    /* read 2-hex checksum */
    uint8_t hi, lo;
    int rc = read_wait_byte(t, &hi, wait);
    if (rc != 1) return rc;
    rc = read_wait_byte(t, &lo, wait);
    if (rc != 1) return rc;

    int h = hex_nibble((char)hi);
    int l = hex_nibble((char)lo);
    if (h < 0 || l < 0) {
        write_byte_t(t, '-');
        return WIRE_ERR_CHECKSUM;
    }

    uint8_t expected = (uint8_t)((h << 4) | l);
    if (running_sum != expected) {
        write_byte_t(t, '-');
        return WIRE_ERR_CHECKSUM;
    }

    return write_byte_t(t, '+');
}

/* ── public API ──────────────────────────────────────────────────────────── */

static int wait_for_stop(WireTransport *t, char *out_buf, size_t out_size,
                         int (*interrupt_requested)(void *), void *ctx)
{
    int64_t now = monotonic_ms();
    if (now < 0) return WIRE_ERR_IO;
    StopWait wait = {
        .interrupt_requested = interrupt_requested,
        .ctx = ctx,
        .deadline_ms = now + RSP_STOP_TIMEOUT_MS,
    };
    for (;;) {
        int rc = rsp_recv_packet_t(t, out_buf, out_size, &wait);
        if (rc != WIRE_OK)          return rc;
        if (out_buf[0] == 'E' || out_buf[0] == '\0') return WIRE_ERR_IO;
        if (out_buf[0] == 'S' || out_buf[0] == 'T') {
            if (strlen(out_buf) < 3 || hex_nibble(out_buf[1]) < 0 ||
                hex_nibble(out_buf[2]) < 0) return WIRE_ERR_PARSE;
            if (out_buf[1] != '0' || out_buf[2] != '0') return WIRE_OK;
        }
        /* stale ACK or unexpected packet — keep waiting */
    }
}

int rsp_wait_for_stop_t(WireTransport *t, char *out_buf, size_t out_size)
{
    return wait_for_stop(t, out_buf, out_size, NULL, NULL);
}

static int send_command(WireTransport *t, const char *cmd)
{
    for (int cmd_try = 0; cmd_try < RSP_MAX_RETRIES; cmd_try++) {
        int rc = rsp_send_packet_t(t, cmd);
        if (rc != WIRE_OK) return rc;

        /* Wait for server ACK/NAK of our command */
        uint8_t ack;
        int r = read_byte_t(t, &ack, RSP_TIMEOUT_MS);
        if (r == WIRE_ERR_IO)      return WIRE_ERR_IO;
        if (r == WIRE_ERR_TIMEOUT) return WIRE_ERR_TIMEOUT;
        if (ack == '-') {
            fprintf(stderr, "mkdbg: RSP NAK on '%s', retrying (%d/%d)\n",
                    cmd, cmd_try + 1, RSP_MAX_RETRIES);
            continue;
        }
        if (ack != '+') continue;  /* unexpected byte, retransmit */
        return WIRE_OK;
    }
    return WIRE_ERR_CHECKSUM;
}

int rsp_continue_t(WireTransport *t, char *out_buf, size_t out_size,
                   int (*interrupt_requested)(void *), void *ctx)
{
    int rc = send_command(t, "c");
    if (rc != WIRE_OK) return rc;
    return wait_for_stop(t, out_buf, out_size, interrupt_requested, ctx);
}

int rsp_transaction_t(WireTransport *t, const char *cmd,
                      char *resp_buf, size_t resp_size)
{
    int rc = send_command(t, cmd);
    if (rc != WIRE_OK) return rc;
    for (int resp_try = 0; resp_try < RSP_MAX_RETRIES; resp_try++) {
        rc = rsp_recv_packet_t(t, resp_buf, resp_size, NULL);
        if (rc == WIRE_OK) return WIRE_OK;
        if (rc != WIRE_ERR_CHECKSUM) return rc;
        fprintf(stderr, "mkdbg: RSP response checksum error, retry %d/%d\n",
                resp_try + 1, RSP_MAX_RETRIES);
    }
    return WIRE_ERR_CHECKSUM;
}
