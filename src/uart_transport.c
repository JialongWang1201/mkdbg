/* uart_transport.c — WireTransport adapter for POSIX serial / PTY fds.
 *
 * SPDX-License-Identifier: MIT
 */

#include "uart_transport.h"
#include "wire_host.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/select.h>
#include <unistd.h>

/* ── internal context ────────────────────────────────────────────────────── */

typedef struct {
    int fd;
} UartCtx;

/* ── callbacks ───────────────────────────────────────────────────────────── */

static int uart_read(void *ctx, uint8_t *buf, int len, int timeout_ms)
{
    UartCtx *u = ctx;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(u->fd, &rfds);
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

retry:;
    int r = select(u->fd + 1, &rfds, NULL, NULL, &tv);
    if (r < 0) {
        if (errno == EINTR) {
            FD_ZERO(&rfds);
            FD_SET(u->fd, &rfds);
            goto retry;
        }
        return TRANSPORT_ERR_IO;
    }
    if (r == 0) return TRANSPORT_ERR_TIMEOUT;

    ssize_t n = read(u->fd, buf, (size_t)len);
    if (n < 0)
        return (errno == EAGAIN || errno == EINTR)
               ? TRANSPORT_ERR_TIMEOUT : TRANSPORT_ERR_IO;
    if (n == 0) return TRANSPORT_ERR_CLOSED;
    return (int)n;
}

static int uart_write(void *ctx, const uint8_t *buf, int len)
{
    UartCtx *u = ctx;
    ssize_t n = write(u->fd, buf, (size_t)len);
    if (n < 0) return TRANSPORT_ERR_IO;
    if (n == 0) return TRANSPORT_ERR_CLOSED;
    return (int)n;
}

static void uart_close(void *ctx)
{
    UartCtx *u = ctx;
    close(u->fd);
    free(u);
}

/* ── public ──────────────────────────────────────────────────────────────── */

void transport_destroy(WireTransport *t)
{
    if (!t) return;
    if (t->close) t->close(t->ctx);
    free(t);
}

WireTransport *uart_transport_open(const char *port, int baud)
{
    int fd = wire_serial_open(port, baud);
    if (fd < 0) return NULL;

    UartCtx *ctx = malloc(sizeof(UartCtx));
    if (!ctx) { close(fd); return NULL; }
    ctx->fd = fd;

    WireTransport *t = malloc(sizeof(WireTransport));
    if (!t) { close(fd); free(ctx); return NULL; }
    t->read  = uart_read;
    t->write = uart_write;
    t->close = uart_close;
    t->ctx   = ctx;
    return t;
}
