/* tools/mkdbg_seam.c — seam-analyze: host-side causal fault analysis CLI
 *
 * Reads a raw binary seam dump from a file or stdin.
 * The dump may be:
 *   (a) a raw decoded .cfl bundle (no COBS framing), or
 *   (b) a COBS-framed byte stream as received over UART (0x00 delimiters).
 *
 * Scans for the first valid seam bundle, runs the causal rule engine, and
 * prints the causal chain.  Exit code: 0 = SEAM_OK, 1 = no bundle found,
 * 2 = usage error.
 *
 * Usage:
 *   seam-analyze <bundle.bin>
 *   seam-analyze -        # read from stdin
 *
 * SPDX-License-Identifier: MIT
 */
#include "libseam.h"
#include "seam_cobs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Maximum raw bytes we accept from a UART capture (4 MiB is generous) */
#define SEAM_MAX_RAW (4u * 1024u * 1024u)

/* Maximum decoded bundle size — one bundle can't exceed 255 records × 24 B
 * + 12 B header = 6132 B; add a small pad for safety */
#define SEAM_MAX_BUNDLE 8192u

/* ── Read entire file into a heap buffer ─────────────────────────────────── */
static uint8_t *read_all(FILE *fp, size_t *out_len)
{
    uint8_t *buf   = NULL;
    size_t   total = 0;
    size_t   cap   = 4096;

    buf = (uint8_t *)malloc(cap);
    if (!buf) return NULL;

    for (;;) {
        if (total == cap) {
            if (cap >= SEAM_MAX_RAW) {
                free(buf);
                return NULL;
            }
            cap *= 2;
            if (cap > SEAM_MAX_RAW) cap = SEAM_MAX_RAW;
            uint8_t *tmp = (uint8_t *)realloc(buf, cap);
            if (!tmp) { free(buf); return NULL; }
            buf = tmp;
        }
        size_t n = fread(buf + total, 1, cap - total, fp);
        if (n == 0) break;
        total += n;
    }

    *out_len = total;
    return buf;
}

/* ── Try to analyze a raw decoded buffer ─────────────────────────────────── */
static int try_analyze(const uint8_t *data, size_t len)
{
    if (len < sizeof(cfl_bundle_t)) return -1;

    seam_chain_t chain;
    int rc = seam_analyze((const cfl_bundle_t *)data, len, &chain);
    if (rc != SEAM_OK) return rc;

    seam_print(&chain, stdout);
    return SEAM_OK;
}

/* ── Scan COBS-framed stream for the first valid bundle ──────────────────── */
static int scan_cobs_frames(const uint8_t *raw, size_t raw_len)
{
    static uint8_t decoded[SEAM_MAX_BUNDLE];

    const uint8_t *frame_start = raw;
    const uint8_t *end         = raw + raw_len;

    while (frame_start < end) {
        /* Find the next 0x00 delimiter */
        const uint8_t *delim = (const uint8_t *)memchr(frame_start, 0x00,
                                                        (size_t)(end - frame_start));
        size_t frame_len = (delim != NULL)
                               ? (size_t)(delim - frame_start)
                               : (size_t)(end - frame_start);

        if (frame_len > 0) {
            size_t dec_len = seam_cobs_decode(frame_start, frame_len,
                                              decoded);
            if (dec_len != SIZE_MAX && dec_len >= sizeof(cfl_bundle_t)) {
                int rc = try_analyze(decoded, dec_len);
                if (rc == SEAM_OK) return SEAM_OK;
            }
        }

        if (delim == NULL) break;
        frame_start = delim + 1;
    }

    return -1; /* no valid bundle found */
}

/* ── mkdbg_cmd_seam ───────────────────────────────────────────────────────────
 *
 * Called by the mkdbg router as: mkdbg_cmd_seam(argc-2, argv+2)
 *   mkdbg seam analyze foo.cfl  →  argv=["analyze","foo.cfl"], argc=2
 *
 * Also called via seam_main.c shim for the standalone seam-analyze binary:
 *   seam-analyze foo.cfl        →  argv=["seam-analyze","foo.cfl"], argc=2
 *
 * In both cases argv[1] is the file path when argc == 2.
 */
int mkdbg_cmd_seam(int argc, char *argv[])
{
    if (argc != 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        fprintf(stderr, "usage: seam analyze <bundle.bin | ->\n");
        fprintf(stderr, "  Reads a raw or COBS-framed seam binary dump\n");
        fprintf(stderr, "  and prints the causal fault chain.\n");
        return 2;
    }

    FILE *fp;
    if (strcmp(argv[1], "-") == 0) {
        fp = stdin;
    } else {
        fp = fopen(argv[1], "rb");
        if (!fp) {
            fprintf(stderr, "error: cannot open '%s'\n", argv[1]);
            return 2;
        }
    }

    size_t   raw_len = 0;
    uint8_t *raw     = read_all(fp, &raw_len);
    if (fp != stdin) fclose(fp);

    if (!raw) {
        fprintf(stderr, "error: out of memory or file too large\n");
        return 2;
    }

    if (raw_len == 0) {
        fprintf(stderr, "error: empty input\n");
        free(raw);
        return 1;
    }

    /* Try direct parse first (unframed) */
    int rc = try_analyze(raw, raw_len);
    if (rc != SEAM_OK) {
        /* Fall back to COBS scan */
        rc = scan_cobs_frames(raw, raw_len);
    }

    free(raw);

    if (rc != SEAM_OK) {
        fprintf(stderr, "error: no valid seam bundle found in input\n");
        return 1;
    }

    return 0;
}
