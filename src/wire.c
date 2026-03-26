/* mkdbg_wire.c — native wire crash-dump integration for mkdbg (Phase 3b)
 *
 * Phase 3b replaces the Phase 3a fork+exec approach with a direct call to
 * the embedded wire host library.  wire_serial_open() + wire_dump_crash_to_buf()
 * run in-process for wire_probe_dump(), eliminating the wire-host subprocess.
 *
 * For the dashboard non-blocking probe (wire_probe_start / wire_probe_poll),
 * we still fork() so the UART read does not block the TUI event loop — but we
 * no longer exec() a separate binary.  The child calls wire_serial_open() +
 * wire_dump_crash() (writing JSON to stdout) and exits; the parent reads via
 * the pipe as before.
 *
 * Provides:
 *   wire_probe_dump()   — blocking crash dump (used by cmd_attach wire path)
 *   wire_probe_start()  — non-blocking start (used by dashboard PROBE panel)
 *   wire_probe_poll()   — non-blocking poll  (used by dashboard tick loop)
 *
 * SPDX-License-Identifier: MIT
 */

#include "mkdbg.h"
#include "wire_host.h"

#include <sys/wait.h>

/* ── JSON parsing helpers ────────────────────────────────────────────────── */

/*
 * Find the value after "\"<key>\": " in json_buf.
 * For string values: copies content between quotes into out.
 * For integer values: copies digits into out.
 * Returns pointer past the value, or NULL if key not found.
 */
static const char *json_find_int(const char *buf, const char *key, int *out)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(buf, search);
    if (!p) return NULL;
    p += strlen(search);
    while (*p == ' ') p++;
    char tmp[32];
    size_t i = 0;
    if (*p == '-') { tmp[i++] = *p++; }
    while (*p >= '0' && *p <= '9' && i + 1 < sizeof(tmp))
        tmp[i++] = *p++;
    tmp[i] = '\0';
    if (i == 0) return NULL;
    *out = atoi(tmp);
    return p;
}

static const char *json_find_string(const char *buf, const char *key,
                                    char *out, size_t out_size)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(buf, search);
    if (!p) return NULL;
    p += strlen(search);
    while (*p == ' ') p++;
    if (*p != '"') return NULL;
    p++;  /* skip opening quote */
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_size) {
        if (*p == '\\' && *(p+1)) p++;  /* skip escape */
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (*p == '"') ? p + 1 : NULL;
}

/* Parse "stack_frames": ["0x...", "0x...", ...] into report->stack_frames. */
static void parse_stack_frames(const char *buf, WireCrashReport *r)
{
    const char *p = strstr(buf, "\"stack_frames\":");
    if (!p) return;
    p = strchr(p, '[');
    if (!p) return;
    p++;
    r->nframes = 0;
    while (r->nframes < WIRE_MAX_FRAMES) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == ',') p++;
        if (*p != '"') break;
        p++;
        char *dst = r->stack_frames[r->nframes];
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < WIRE_REG_HEX_LEN)
            dst[i++] = *p++;
        dst[i] = '\0';
        if (*p == '"') p++;
        r->nframes++;
    }
}

/* Parse "registers": { "r0": "0x...", ... } */
static const char *reg_names[WIRE_NREGS] = {
    "r0","r1","r2","r3","r4","r5","r6","r7",
    "r8","r9","r10","r11","r12","sp","lr","pc","xpsr"
};

static void parse_registers(const char *buf, WireCrashReport *r)
{
    const char *regs_section = strstr(buf, "\"registers\":");
    if (!regs_section) return;
    const char *end = strchr(regs_section, '}');
    /* Limit search to within the registers object */
    char tmp[512] = {0};
    if (end) {
        size_t len = (size_t)(end - regs_section);
        if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
        memcpy(tmp, regs_section, len);
    } else {
        copy_string(tmp, sizeof(tmp), regs_section);
    }
    for (int i = 0; i < WIRE_NREGS; i++)
        json_find_string(tmp, reg_names[i], r->regs[i], WIRE_REG_HEX_LEN);
}

/* Fill WireCrashReport from a wire_dump_crash JSON output buffer. */
static void parse_json(const char *json, WireCrashReport *r)
{
    memset(r, 0, sizeof(*r));
    json_find_int(json, "halt_signal", &r->halt_signal);
    int timeout_val = 0;
    json_find_int(json, "timeout", &timeout_val);
    r->timeout = timeout_val;
    json_find_string(json, "cfsr",         r->cfsr,         sizeof(r->cfsr));
    json_find_string(json, "cfsr_decoded", r->cfsr_decoded, sizeof(r->cfsr_decoded));
    json_find_string(json, "timestamp",    r->timestamp,    sizeof(r->timestamp));
    parse_registers(json, r);
    parse_stack_frames(json, r);
}

/* ── public API ──────────────────────────────────────────────────────────── */

int wire_probe_dump(const char *port, const char *baud,
                    WireCrashReport *report)
{
    int baud_int = atoi(baud ? baud : "115200");
    int uart_fd = wire_serial_open(port, baud_int);
    if (uart_fd < 0) return -1;

    char json[8192];
    int rc = wire_dump_crash_to_buf(uart_fd, json, sizeof(json));
    close(uart_fd);

    if (rc < 0) return -1;

    parse_json(json, report);

    if (report->timeout) return 1;
    if (report->halt_signal == 0) return 1;
    return 0;
}

pid_t wire_probe_start(const char *port, const char *baud, int *pipe_fd_out)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* child: redirect stdout to pipe write end, then run dump in-process */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        int baud_int = atoi(baud ? baud : "115200");
        int uart_fd = wire_serial_open(port, baud_int);
        if (uart_fd < 0) _exit(1);
        wire_dump_crash(uart_fd);  /* writes JSON to stdout (→ pipe) */
        close(uart_fd);
        _exit(0);
    }

    /* parent: close write end, make read end non-blocking */
    close(pipefd[1]);
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags >= 0) fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    *pipe_fd_out = pipefd[0];
    return pid;
}

int wire_probe_poll(pid_t pid, int pipe_fd, WireCrashReport *report)
{
    /* Drain any available bytes */
    static char  buf[8192];
    static size_t buf_len = 0;

    char tmp[512];
    for (;;) {
        ssize_t n = read(pipe_fd, tmp, sizeof(tmp));
        if (n > 0) {
            if (buf_len + (size_t)n < sizeof(buf)) {
                memcpy(buf + buf_len, tmp, (size_t)n);
                buf_len += (size_t)n;
            }
        } else {
            break;  /* EAGAIN (no data) or EOF */
        }
    }

    /* Check if subprocess has exited */
    int status = 0;
    int rc = waitpid(pid, &status, WNOHANG);
    if (rc == 0) return 0;  /* still running */
    if (rc < 0) return -1;

    /* Process finished — parse whatever we got */
    buf[buf_len] = '\0';
    parse_json(buf, report);
    buf_len = 0;  /* reset for next call */
    return 1;
}
