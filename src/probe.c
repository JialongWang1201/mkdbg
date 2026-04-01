/* probe.c — Wire-based hardware probe commands (no openocd/gdb required)
 *
 * All probe commands open a direct UART connection to the MCU's wire agent,
 * send one RSP packet, and close.  The MCU must be in wire_debug_loop
 * (i.e. already halted at a crash or breakpoint) for halt/read32/write32.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mkdbg.h"
#include "wire_host.h"

/* ── UART open helper ────────────────────────────────────────────────────── */

static int probe_open(const ProbeOptions *opts)
{
  if (opts->port == NULL || opts->port[0] == '\0') {
    fprintf(stderr,
            "mkdbg: probe requires --port (e.g. --port /dev/ttyUSB0)\n");
    return -1;
  }
  int baud = 115200;
  if (opts->baud != NULL && opts->baud[0] != '\0')
    baud = atoi(opts->baud);
  int fd = wire_serial_open(opts->port, baud);
  if (fd < 0)
    fprintf(stderr, "mkdbg: cannot open %s\n", opts->port);
  return fd;
}

/* ── Hex helpers (local) ─────────────────────────────────────────────────── */

static int hn(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

/* Parse little-endian 4-byte hex (RSP 'm' response) → uint32. */
static uint32_t le_hex_to_u32(const char *s)
{
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) {
    uint8_t b = (uint8_t)((hn(s[i * 2]) << 4) | hn(s[i * 2 + 1]));
    v |= (uint32_t)b << (i * 8);
  }
  return v;
}

/* Encode uint32 as 8 little-endian hex chars (for RSP 'M' payload). */
static void u32_to_le_hex(uint32_t v, char out[9])
{
  static const char h[] = "0123456789abcdef";
  for (int i = 0; i < 4; i++) {
    uint8_t b    = (uint8_t)(v >> (i * 8));
    out[i * 2]   = h[b >> 4];
    out[i * 2 + 1] = h[b & 0xf];
  }
  out[8] = '\0';
}

/* ── Probe commands ──────────────────────────────────────────────────────── */

/* probe halt: query halt reason via RSP '?' */
int cmd_probe_halt(const ProbeOptions *opts)
{
  if (opts->dry_run) {
    printf("[dry-run] port=%s  rsp=?\n", opts->port ? opts->port : "<unset>");
    return 0;
  }
  int fd = probe_open(opts);
  if (fd < 0) return 1;

  char resp[32];
  int rc = rsp_transaction(fd, "?", resp, sizeof(resp));
  close(fd);

  if (rc != WIRE_OK) {
    fprintf(stderr,
            "mkdbg: probe halt: no response — is the MCU halted in wire_debug_loop?\n");
    return 1;
  }
  if (resp[0] == 'S' || resp[0] == 'T') {
    int sig = (hn(resp[1]) << 4) | hn(resp[2]);
    printf("halted  signal=%d  (%s)\n", sig,
           sig == 5  ? "SIGTRAP"  :
           sig == 11 ? "SIGSEGV"  :
           sig == 6  ? "SIGABRT"  : "unknown");
    return 0;
  }
  fprintf(stderr, "mkdbg: unexpected response: %s\n", resp);
  return 1;
}

/* probe resume: send RSP 'c' and get the S00 acknowledge */
int cmd_probe_resume(const ProbeOptions *opts)
{
  if (opts->dry_run) {
    printf("[dry-run] port=%s  rsp=c\n", opts->port ? opts->port : "<unset>");
    return 0;
  }
  int fd = probe_open(opts);
  if (fd < 0) return 1;

  char resp[16];
  int rc = rsp_transaction(fd, "c", resp, sizeof(resp));
  close(fd);

  if (rc != WIRE_OK) {
    fprintf(stderr, "mkdbg: probe resume: no response\n");
    return 1;
  }
  printf("resumed\n");
  return 0;
}

/* probe reset: send RSP 'R00' — send-only, MCU resets immediately */
int cmd_probe_reset(const ProbeOptions *opts)
{
  if (opts->dry_run) {
    printf("[dry-run] port=%s  rsp=R00\n", opts->port ? opts->port : "<unset>");
    return 0;
  }
  int fd = probe_open(opts);
  if (fd < 0) return 1;

  int rc = rsp_send_packet(fd, "R00");
  close(fd);

  if (rc != WIRE_OK) {
    fprintf(stderr, "mkdbg: probe reset: send failed\n");
    return 1;
  }
  printf("reset sent\n");
  return 0;
}

/* probe read32: read 4 bytes via RSP 'm addr,4' */
int cmd_probe_read32(const ProbeOptions *opts)
{
  if (opts->address == NULL) {
    fprintf(stderr, "mkdbg: probe read32 requires an address\n");
    return 1;
  }

  char *end;
  errno = 0;
  uint32_t addr = (uint32_t)strtoul(opts->address, &end, 0);
  if (errno != 0 || *end != '\0') {
    fprintf(stderr, "mkdbg: invalid address: %s\n", opts->address);
    return 1;
  }

  if (opts->dry_run) {
    printf("[dry-run] port=%s  rsp=m%x,4\n",
           opts->port ? opts->port : "<unset>", addr);
    return 0;
  }

  int fd = probe_open(opts);
  if (fd < 0) return 1;

  char cmd[32];
  snprintf(cmd, sizeof(cmd), "m%x,4", addr);
  char resp[32];
  int rc = rsp_transaction(fd, cmd, resp, sizeof(resp));
  close(fd);

  if (rc != WIRE_OK) {
    fprintf(stderr, "mkdbg: probe read32: no response\n");
    return 1;
  }
  if (resp[0] == 'E') {
    fprintf(stderr, "mkdbg: probe read32: target error %s\n", resp);
    return 1;
  }

  uint32_t val = le_hex_to_u32(resp);
  printf("0x%08x  =  0x%08x  (%u)\n", addr, val, val);
  return 0;
}

/* probe write32: write 4 bytes via RSP 'M addr,4:hexdata' */
int cmd_probe_write32(const ProbeOptions *opts)
{
  if (opts->address == NULL || opts->value == NULL) {
    fprintf(stderr, "mkdbg: probe write32 requires an address and value\n");
    return 1;
  }

  char *end;
  errno = 0;
  uint32_t addr = (uint32_t)strtoul(opts->address, &end, 0);
  if (errno != 0 || *end != '\0') {
    fprintf(stderr, "mkdbg: invalid address: %s\n", opts->address);
    return 1;
  }
  errno = 0;
  uint32_t val = (uint32_t)strtoul(opts->value, &end, 0);
  if (errno != 0 || *end != '\0') {
    fprintf(stderr, "mkdbg: invalid value: %s\n", opts->value);
    return 1;
  }

  if (opts->dry_run) {
    char hexdata[9];
    u32_to_le_hex(val, hexdata);
    printf("[dry-run] port=%s  rsp=M%x,4:%s\n",
           opts->port ? opts->port : "<unset>", addr, hexdata);
    return 0;
  }

  int fd = probe_open(opts);
  if (fd < 0) return 1;

  char hexdata[9];
  u32_to_le_hex(val, hexdata);
  char cmd[64];
  snprintf(cmd, sizeof(cmd), "M%x,4:%s", addr, hexdata);
  char resp[16];
  int rc = rsp_transaction(fd, cmd, resp, sizeof(resp));
  close(fd);

  if (rc != WIRE_OK) {
    fprintf(stderr, "mkdbg: probe write32: no response\n");
    return 1;
  }
  if (strcmp(resp, "OK") != 0) {
    fprintf(stderr, "mkdbg: probe write32: target error %s\n", resp);
    return 1;
  }
  printf("wrote 0x%08x → 0x%08x\n", val, addr);
  return 0;
}
