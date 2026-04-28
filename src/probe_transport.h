/* probe_transport.h — WireTransport adapter backed by probe-bridge.
 *
 * Only compiled when MKDBG_PROBE_SUPPORT is defined (cargo found at
 * configure time).  All callers must guard with #ifdef MKDBG_PROBE_SUPPORT.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PROBE_TRANSPORT_H
#define PROBE_TRANSPORT_H

#include "transport.h"

/*
 * probe_transport_open — open probe[probe_idx] and return a WireTransport.
 *
 * probe_idx  index from probe_list(); pass 0 when only one probe is present.
 * chip       chip name (e.g. "STM32F446RETx"), or NULL for auto-detect.
 *
 * Returns a heap-allocated WireTransport on success, NULL on failure.
 * Caller destroys with transport_destroy().
 */
WireTransport *probe_transport_open(int probe_idx, const char *chip);

#endif /* PROBE_TRANSPORT_H */
