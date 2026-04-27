#pragma once
/*
 * probe_bridge.h — C API for the probe-bridge static library.
 *
 * This header is the only surface that mkdbg's C code touches.
 * The Rust side is built via `cargo build --release` and linked as
 * libprobe_bridge.a.
 *
 * Return-value conventions (probe_read / probe_write):
 *   >= 0   bytes transferred
 *   -1     I/O error (probe disconnected, USB fault)
 *   -2     timeout (probe_read only, timeout_ms elapsed with no data)
 *   -3     handle already closed
 *   -4     bridge internal error (ring-buffer overflow, thread crash)
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- probe enumeration ------------------------------------------ */

typedef struct {
    char     identifier[128]; /* e.g. "ST-Link V2", "DAPLink"           */
    char     serial[64];      /* USB serial string, may be empty         */
    uint16_t vid;
    uint16_t pid;
} ProbeInfo;

/*
 * probe_list — enumerate connected debug probes.
 *
 * Writes up to `max` ProbeInfo entries into `out`.
 * Returns the total number of probes found (may exceed `max`).
 * Returns -1 on internal error.
 */
int probe_list(ProbeInfo *out, int max);

/* ---------- session handle --------------------------------------------- */

typedef struct ProbeHandle ProbeHandle;

/*
 * probe_open — connect to probe[probe_idx] and attach to target chip.
 *
 * `chip`  chip name accepted by probe-rs (e.g. "STM32F446RETx").
 *         Pass NULL to let probe-rs auto-detect from SWD IDCODE.
 *
 * Starts an internal RSP interpreter thread.  The thread translates RSP
 * packets received via probe_write() into probe-rs calls and writes RSP
 * replies accessible via probe_read().
 *
 * Returns NULL on failure.
 */
ProbeHandle *probe_open(int probe_idx, const char *chip);

/*
 * probe_write — push RSP bytes from mkdbg into the bridge.
 *
 * Thread-safe.  Non-blocking.
 */
int probe_write(ProbeHandle *h, const uint8_t *buf, int len);

/*
 * probe_read — pull RSP reply bytes out of the bridge.
 *
 * Blocks up to `timeout_ms` milliseconds waiting for data.
 * Thread-safe.
 */
int probe_read(ProbeHandle *h, uint8_t *buf, int len, int timeout_ms);

/*
 * probe_close — detach from target and release all resources.
 *
 * Blocks until the internal RSP thread exits.  Safe to call multiple times.
 */
void probe_close(ProbeHandle *h);

#ifdef __cplusplus
}
#endif
