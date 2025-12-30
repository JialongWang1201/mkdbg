/* mkdbg_wire.c — wire-host --dump integration for mkdbg
 *
 * Provides:
 *   wire_probe_dump()   — blocking crash dump (used by cmd_attach wire path)
 *   wire_probe_start()  — non-blocking start (used by dashboard PROBE panel)
 *   wire_probe_poll()   — non-blocking poll  (used by dashboard tick loop)
 *
 * wire-host --dump outputs JSON to stdout.  We fork+exec it, capture stdout
 * via a pipe, and parse the JSON with simple string scanning (no library
 * dependency — we control the JSON format).
 *
 * wire-host binary lookup order:
 *   1. Same directory as the running mkdbg-native binary (argv[0])
 *   2. $PATH via execvp
 *
 * SPDX-License-Identifier: MIT
 */

#include "mkdbg.h"

#include <sys/wait.h>

/* ── find wire-host binary ───────────────────────────────────────────────── */

static void find_wire_host(char *out, size_t out_size)
{
    /* Try same directory as this binary */
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = '\0';
        char dir[PATH_MAX];
        path_dirname(self, dir, sizeof(dir));
        char candidate[PATH_MAX];
        join_path(dir, "wire-host", candidate, sizeof(candidate));
        if (path_executable(candidate)) {
            copy_string(out, out_size, candidate);
            return;
        }
    }
    /* Fall back to PATH lookup (let execvp find it) */
    copy_string(out, out_size, "wire-host");
}

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

/* Fill WireCrashReport from a wire-host --dump JSON output buffer. */
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

/* ── subprocess helpers ──────────────────────────────────────────────────── */

/*
 * Fork wire-host --dump and return pid + read end of stdout pipe.
 * Returns -1 on error.
 */
static pid_t start_dump_process(const char *wire_host_bin,
                                const char *port, const char *baud,
                                int *pipe_fd_out)
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
        /* child: redirect stdout to pipe write end */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        char *argv[9];
        int   argc = 0;
        argv[argc++] = (char *)wire_host_bin;
        argv[argc++] = "--port";
        argv[argc++] = (char *)port;
        argv[argc++] = "--baud";
        argv[argc++] = (char *)(baud ? baud : "115200");
        argv[argc++] = "--dump";
        argv[argc]   = NULL;
        execvp(wire_host_bin, argv);
        /* execvp failed */
        fprintf(stderr, "mkdbg: failed to exec wire-host: %s\n", wire_host_bin);
        _exit(1);
    }

    /* parent: close write end, return read end */
    close(pipefd[1]);
    *pipe_fd_out = pipefd[0];
    return pid;
}

/* ── public API ──────────────────────────────────────────────────────────── */

int wire_probe_dump(const char *port, const char *baud,
                    WireCrashReport *report)
{
    char wire_host[PATH_MAX];
    find_wire_host(wire_host, sizeof(wire_host));

    int  pipe_fd = -1;
    pid_t pid = start_dump_process(wire_host, port, baud, &pipe_fd);
    if (pid < 0) return -1;

    /* Read all output */
    char   json[8192];
    size_t total = 0;
    for (;;) {
        ssize_t n = read(pipe_fd, json + total, sizeof(json) - total - 1);
        if (n <= 0) break;
        total += (size_t)n;
        if (total + 1 >= sizeof(json)) break;
    }
    json[total] = '\0';
    close(pipe_fd);

    int status = 0;
    waitpid(pid, &status, 0);

    parse_json(json, report);

    if (report->timeout) return 1;
    if (report->halt_signal == 0) return 1;
    return 0;
}

pid_t wire_probe_start(const char *port, const char *baud, int *pipe_fd_out)
{
    char wire_host[PATH_MAX];
    find_wire_host(wire_host, sizeof(wire_host));
    pid_t pid = start_dump_process(wire_host, port, baud, pipe_fd_out);
    if (pid > 0 && *pipe_fd_out >= 0) {
        /* Make read end non-blocking for dashboard tick loop */
        int flags = fcntl(*pipe_fd_out, F_GETFL, 0);
        if (flags >= 0) fcntl(*pipe_fd_out, F_SETFL, flags | O_NONBLOCK);
    }
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
