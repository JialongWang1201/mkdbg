/* uart_transport.h — POSIX fd wrapped as WireTransport.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef UART_TRANSPORT_H
#define UART_TRANSPORT_H

#include "transport.h"

/*
 * Open a UART or PTY and return a heap-allocated WireTransport.
 * port  — device path, e.g. "/dev/ttyUSB0"
 * baud  — baud rate; pass 0 for PTY / QEMU (no baud negotiation)
 * Returns NULL on error (message printed to stderr).
 * Caller destroys with transport_destroy().
 */
WireTransport *uart_transport_open(const char *port, int baud);

#endif /* UART_TRANSPORT_H */
