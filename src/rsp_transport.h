/* rsp_transport.h — GDB RSP packet framing over WireTransport.
 *
 * Drop-in replacement for deps/wire/host/wire_rsp_client.c that uses
 * WireTransport callbacks instead of a raw POSIX fd, enabling the same
 * RSP logic to run over UART or a hardware probe bridge.
 *
 * Return values mirror wire_host.h WIRE_* codes so callers are unchanged.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RSP_TRANSPORT_H
#define RSP_TRANSPORT_H

#include "transport.h"
#include "wire_host.h"   /* WIRE_OK, WIRE_ERR_* */

#include <stddef.h>

/*
 * Send one RSP packet ($data#checksum) without waiting for a reply.
 * Used for commands that have no immediate response (e.g. 's' single-step).
 */
int rsp_send_packet_t(WireTransport *t, const char *data);

/*
 * Block until the target sends a spontaneous stop reply (S or T packet).
 * Retries for up to ~60 s.  Returns WIRE_OK with the reply in out_buf.
 */
int rsp_wait_for_stop_t(WireTransport *t, char *out_buf, size_t out_size);

/*
 * Send cmd and receive the server response, with NAK/retransmit (up to 3×).
 * Returns WIRE_OK on success.
 */
int rsp_transaction_t(WireTransport *t, const char *cmd,
                      char *resp_buf, size_t resp_size);

#endif /* RSP_TRANSPORT_H */
