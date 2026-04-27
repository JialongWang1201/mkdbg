/* transport.h — Abstract byte-stream transport for debug_session.
 *
 * Decouples debug_session.c from the concrete I/O mechanism so that the same
 * RSP client code works over a UART fd (uart_transport.c) or a hardware probe
 * via probe-bridge (probe_transport.c, PR-3).
 *
 * Error codes returned by read/write callbacks:
 *   TRANSPORT_ERR_IO      — OS-level I/O error
 *   TRANSPORT_ERR_TIMEOUT — read timed out (caller should retry or abort)
 *   TRANSPORT_ERR_CLOSED  — peer closed the connection
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <stdint.h>

#define TRANSPORT_OK           0
#define TRANSPORT_ERR_IO      (-1)
#define TRANSPORT_ERR_TIMEOUT (-2)
#define TRANSPORT_ERR_CLOSED  (-3)

/*
 * WireTransport — pluggable byte-stream backend.
 *
 * read():  read up to len bytes into buf; timeout_ms is a per-call wall-clock
 *          limit.  Returns bytes transferred (>= 1) or a negative error code.
 * write(): write exactly len bytes from buf.  Returns bytes written (== len on
 *          success) or a negative error code.
 * close(): release all resources; transport must not be used after this call.
 * ctx:     opaque pointer forwarded to every callback.
 */
typedef struct WireTransport {
    int  (*read) (void *ctx, uint8_t *buf, int len, int timeout_ms);
    int  (*write)(void *ctx, const uint8_t *buf, int len);
    void (*close)(void *ctx);
    void *ctx;
} WireTransport;

/* Free the WireTransport struct itself after calling t->close(t->ctx). */
void transport_destroy(WireTransport *t);

#endif /* TRANSPORT_H */
