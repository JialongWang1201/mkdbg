/* debug_cli.c — Interactive REPL for live MCU debugging over UART
 *
 * Commands:
 *   break 0x<addr>      set FPBv1 hardware breakpoint
 *   clear <id>          clear breakpoint by number
 *   continue  (c)       resume MCU; wait for halt (Ctrl-C interrupts)
 *   step      (s)       execute one instruction; block until DebugMonitor
 *   regs                print all 17 Cortex-M registers
 *   mem 0x<addr> [len]  hexdump target memory (default 16 bytes, max 256)
 *   info breakpoints    list active breakpoints
 *   tui                 switch to TUI mode (implemented in PR 11)
 *   quit      (q)       resume MCU and exit
 *
 * SPDX-License-Identifier: MIT
 */

#include "mkdbg.h"
#include "debug_session.h"
#include "debug_parse.h"
#include "debug_tui.h"
#include "dwarf.h"
#include "transport.h"
#include "wire_host.h"
#include "arch.h"
#include "capture_transport.h"
#include "uart_transport.h"
#ifdef MKDBG_PROBE_SUPPORT
#include "probe_bridge.h"
#include "probe_transport.h"
#endif

/* ── Session-scoped DWARF handle (NULL if no --elf provided) ─────────────── */

static DwarfDBI *s_dbi = NULL;

static volatile sig_atomic_t s_interrupt_requested;

static void request_interrupt(int signo)
{
    (void)signo;
    s_interrupt_requested = 1;
}

static int cli_interrupt_requested(void *ctx)
{
    (void)ctx;
    return s_interrupt_requested != 0;
}

static int cli_continue(DebugSession *s)
{
    struct sigaction action = {0}, previous;
    action.sa_handler = request_interrupt;
    sigemptyset(&action.sa_mask);
    s_interrupt_requested = 0;
    if (sigaction(SIGINT, &action, &previous) != 0) return WIRE_ERR_IO;
    int rc = debug_session_continue_interruptible(s, cli_interrupt_requested, NULL);
    if (sigaction(SIGINT, &previous, NULL) != 0) return WIRE_ERR_IO;
    return rc;
}

/* pcTaskName byte offset in TCB (default 52 per FreeRTOS 10.x standard config) */
static int s_freertos_offset = 52;

/* ── Memory display list ─────────────────────────────────────────────────── */

#define DISPLAY_MAX 8
static uint32_t s_display[DISPLAY_MAX];
static int s_ndisplay = 0;

static const DebugBreakpoint *bp_find(DebugSession *s, int id)
{
    for (size_t i = 0; i < debug_session_breakpoint_count(s); i++) {
        const DebugBreakpoint *bp = debug_session_breakpoint_at(s, i);
        if (bp->id == id) return bp;
    }
    return NULL;
}

static const DebugWatchpoint *wp_find(DebugSession *s, int id)
{
    for (size_t i = 0; i < debug_session_watchpoint_count(s); i++) {
        const DebugWatchpoint *wp = debug_session_watchpoint_at(s, i);
        if (wp->id == id) return wp;
    }
    return NULL;
}

/* ── Print helpers ───────────────────────────────────────────────────────── */

static const char *signal_name(int sig)
{
    switch (sig) {
    case 4:  return "SIGILL";
    case 5:  return "SIGTRAP";
    case 11: return "SIGSEGV";
    default: return "?";
    }
}

/* ── FreeRTOS helpers ────────────────────────────────────────────────────── */

/* Read the current FreeRTOS task name into name_out (max sz bytes).
 * Returns 1 on success, 0 if FreeRTOS not detected or read failed. */
static int read_freertos_task(DebugSession *s, char *name_out, size_t sz)
{
    if (!s_dbi) return 0;

    uint32_t sym_addr;
    if (dwarf_sym_to_addr(s_dbi, "pxCurrentTCB", &sym_addr) != 0) return 0;

    uint8_t ptr_buf[4];
    if (debug_session_read_mem(s, sym_addr, 4, ptr_buf) != WIRE_OK) return 0;
    uint32_t tcb = (uint32_t)ptr_buf[0]
                 | (uint32_t)ptr_buf[1] << 8
                 | (uint32_t)ptr_buf[2] << 16
                 | (uint32_t)ptr_buf[3] << 24;
    if (tcb == 0) return 0;

    uint8_t name_buf[16];
    if (debug_session_read_mem(s, tcb + (uint32_t)s_freertos_offset, 15, name_buf) != WIRE_OK)
        return 0;
    name_buf[15] = '\0';
    snprintf(name_out, sz, "%s", (char *)name_buf);
    return 1;
}

static void print_halt(DebugSession *s)
{
    int sig = debug_session_last_signal(s);
    printf("Halted.  signal=%d (%s)", sig, signal_name(sig));
    uint32_t regs[DEBUG_SESSION_MAX_REGS];
    if (debug_session_read_regs(s, regs) == WIRE_OK) {
        uint32_t pc = regs[debug_session_pc_reg(s)];
        printf("  pc=0x%08x", pc);
        if (s_dbi) {
            DwarfLocation loc;
            if (dwarf_pc_to_location(s_dbi, pc, &loc) == 0)
                printf("  %s:%d", loc.file, loc.line);
        }
    }
    char task_name[32] = "";
    if (read_freertos_task(s, task_name, sizeof(task_name)))
        printf("  task=%s", task_name);
    printf("\n");
}

/* ── Parsing helpers ─────────────────────────────────────────────────────── */

/* Split leading token from line into cmd_out; return pointer to rest. */
static const char *split_token(const char *line, char *out, size_t out_sz)
{
    while (*line == ' ' || *line == '\t') line++;
    size_t i = 0;
    while (*line && *line != ' ' && *line != '\t' && i + 1 < out_sz)
        out[i++] = *line++;
    out[i] = '\0';
    while (*line == ' ' || *line == '\t') line++;
    return line;
}

static void trim_eol(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' '))
        s[--n] = '\0';
}

/* ── Command handlers ────────────────────────────────────────────────────── */

static void do_break(DebugSession *s, const char *args)
{
    if (!*args) { printf("usage: break 0x<addr> | break <symbol>\n"); return; }

    uint32_t addr;
    if ((args[0] >= '0' && args[0] <= '9') || args[0] == '-' || args[0] == '+') {
        if (debug_parse_address(args, &addr) != 0) {
            printf("error: invalid address\n"); return;
        }
    } else if (s_dbi && dwarf_sym_to_addr(s_dbi, args, &addr) == 0) {
        printf("(symbol '%s' = 0x%08x)\n", args, addr);
    } else if (!s_dbi) {
        printf("error: pass --elf to use symbol names\n"); return;
    } else {
        printf("error: symbol '%s' not found in ELF\n", args); return;
    }
    int id = debug_session_add_breakpoint(s, addr);
    if (id < 0) {
        printf("error: could not set breakpoint (rc=%d)\n", id);
        return;
    }
    printf("Breakpoint %d at 0x%08x\n", id, addr);
}

static void do_clear(DebugSession *s, const char *args)
{
    if (!*args) { printf("usage: clear <id>\n"); return; }

    int id = atoi(args);
    const DebugBreakpoint *bp = bp_find(s, id);
    if (!bp) {
        printf("error: no breakpoint %d\n", id);
        return;
    }
    uint32_t addr = bp->addr;
    int rc = debug_session_remove_breakpoint(s, id);
    if (rc != WIRE_OK) {
        printf("warning: could not clear breakpoint %d (rc=%d); record retained\n", id, rc);
        return;
    }
    printf("Breakpoint %d (0x%08x) cleared.\n", id, addr);
}

static void do_regs(DebugSession *s)
{
    uint32_t regs[DEBUG_SESSION_MAX_REGS];
    if (debug_session_read_regs(s, regs) != WIRE_OK) {
        printf("error: failed to read registers\n");
        return;
    }
    int nregs = debug_session_nregs(s);
    for (int i = 0; i < nregs; i++) {
        printf("%-6s 0x%08x", debug_session_reg_name(s, i), regs[i]);
        printf(((i % 4) == 3 || i == nregs - 1) ? "\n" : "   ");
    }
}

static void do_mem(DebugSession *s, const char *args)
{
    if (!*args) { printf("usage: mem 0x<addr> [len]\n"); return; }

    char addr_tok[256];
    const char *rest = split_token(args, addr_tok, sizeof(addr_tok));
    uint32_t addr;
    if (debug_parse_address(addr_tok, &addr) != 0) {
        printf("error: invalid address\n"); return;
    }
    size_t len = *rest ? (size_t)strtoul(rest, NULL, 0) : 16;
    if (len == 0 || len > 256) len = 16;

    uint8_t buf[256];
    if (debug_session_read_mem(s, addr, len, buf) != WIRE_OK) {
        printf("error: memory read failed (out of allowed RAM range?)\n");
        return;
    }
    for (size_t i = 0; i < len; i++) {
        if (i % 16 == 0) printf("0x%08x: ", addr + (uint32_t)i);
        printf("%02x ", buf[i]);
        if (i % 16 == 15 || i == len - 1) printf("\n");
    }
}

static void do_info_breakpoints(DebugSession *s)
{
    size_t count = debug_session_breakpoint_count(s);
    if (count == 0) { printf("No breakpoints.\n"); return; }
    printf("Num  Address\n");
    for (size_t i = 0; i < count; i++) {
        const DebugBreakpoint *bp = debug_session_breakpoint_at(s, i);
        printf("%-4d 0x%08x\n", bp->id, bp->addr);
    }
}

static void do_watch(DebugSession *s, const char *args, WatchpointType type)
{
    if (!*args) {
        printf("usage: watch/rwatch/awatch 0x<addr>\n"); return;
    }
    uint32_t addr;
    if (debug_parse_address(args, &addr) != 0) {
        printf("error: invalid address\n"); return;
    }
    int id = debug_session_add_watchpoint(s, addr, type);
    if (id < 0) {
        printf("error: could not set watchpoint (rc=%d)\n", id);
        return;
    }
    const char *tname = (type == WATCHPOINT_WRITE)  ? "write"  :
                        (type == WATCHPOINT_READ)   ? "read"   : "access";
    printf("Watchpoint %d at 0x%08x (%s)\n", id, addr, tname);
}

static void do_delete_watch(DebugSession *s, const char *args)
{
    if (!*args) { printf("usage: delete watch <id>\n"); return; }
    int id = atoi(args);
    const DebugWatchpoint *wp = wp_find(s, id);
    if (!wp) {
        printf("error: no watchpoint %d\n", id); return;
    }
    uint32_t addr = wp->addr;
    int rc = debug_session_remove_watchpoint(s, id);
    if (rc != WIRE_OK) {
        printf("warning: could not clear watchpoint %d (rc=%d); record retained\n", id, rc);
        return;
    }
    printf("Watchpoint %d (0x%08x) cleared.\n", id, addr);
}

static void do_info_watchpoints(DebugSession *s)
{
    size_t count = debug_session_watchpoint_count(s);
    if (count == 0) { printf("No watchpoints.\n"); return; }
    printf("Num  Address     Type\n");
    for (size_t i = 0; i < count; i++) {
        const DebugWatchpoint *wp = debug_session_watchpoint_at(s, i);
        const char *type = wp->type == WATCHPOINT_WRITE ? "write" :
                           wp->type == WATCHPOINT_READ ? "read" : "access";
        printf("%-4d 0x%08x  %s\n", wp->id, wp->addr, type);
    }
}

static void do_display(const char *args)
{
    if (!*args) { printf("usage: display 0x<addr>\n"); return; }
    if (s_ndisplay >= DISPLAY_MAX) { printf("error: display list full\n"); return; }
    uint32_t addr;
    if (debug_parse_address(args, &addr) != 0) {
        printf("error: invalid address\n"); return;
    }
    s_display[s_ndisplay++] = addr;
    printf("display %d: 0x%08x\n", s_ndisplay, s_display[s_ndisplay - 1]);
}

static void do_undisplay(const char *args)
{
    if (!*args) { printf("usage: undisplay <id>\n"); return; }
    int id = atoi(args) - 1;
    if (id < 0 || id >= s_ndisplay) { printf("error: no display %d\n", id + 1); return; }
    for (int i = id; i < s_ndisplay - 1; i++) s_display[i] = s_display[i + 1];
    s_ndisplay--;
    printf("display %d removed\n", id + 1);
}

static void print_help(void)
{
    printf(
        "Commands:\n"
        "  break 0x<addr>           set hardware breakpoint (FPBv1)\n"
        "  break <symbol>           set breakpoint at named function (requires --elf)\n"
        "  clear <id>               clear breakpoint by number\n"
        "  watch 0x<addr>           DWT write watchpoint\n"
        "  rwatch 0x<addr>          DWT read watchpoint\n"
        "  awatch 0x<addr>          DWT read+write watchpoint\n"
        "  delete watch <id>        clear DWT watchpoint by number\n"
        "  display 0x<addr>         add memory address to display list\n"
        "  undisplay <id>           remove from display list\n"
        "  continue  (c)            resume execution; Ctrl-C interrupts the wait\n"
        "  interrupt (int)          send Ctrl-C break-in; halt running MCU\n"
        "  step      (s)            single-step one instruction\n"
        "  regs                     print all CPU registers\n"
        "  mem 0x<addr> [len]       hexdump memory (default 16 bytes, max 256)\n"
        "  info breakpoints         list active breakpoints\n"
        "  info watchpoints         list active DWT watchpoints\n"
        "  info display             list memory display addresses\n"
        "  freertos current         show current FreeRTOS task name (requires --elf)\n"
        "  tui                      TUI mode\n"
        "  quit      (q)            resume MCU and exit\n"
    );
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int cmd_debug(const DebugOptions *opts)
{
    const char *port = opts->port;
    int         baud = opts->baud ? opts->baud : DEFAULT_BAUD;
    DebugSession *s = NULL;
    WireTransport *transport = NULL;

    const char     *arch_name = opts->arch ? opts->arch : "cortex-m";
    const MkdbgArch *arch     = mkdbg_arch_find(arch_name);
    if (!arch || !arch->live_debug)
        die("arch '%s' does not support live debug", arch_name);

    if (opts->dry_run) {
        if (opts->replay_debug) {
            printf("[dry-run] replay debug capture=%s arch=%s\n",
                   opts->replay_debug, arch_name);
        } else if (opts->use_probe) {
            printf("[dry-run] probe_open(%s, %s) -> debug session arch=%s",
                   opts->probe_idx >= 0 ? "selected" : "auto",
                   opts->chip ? opts->chip : "auto", arch_name);
            if (opts->record_debug) printf(" capture=%s", opts->record_debug);
            putchar('\n');
        } else {
            if (!port || !*port)
                die("debug requires --port or --probe; e.g. mkdbg debug --port /dev/ttyUSB0");
            printf("[dry-run] uart_open(%s, %d) -> debug session arch=%s",
                   port, baud, arch_name);
            if (opts->record_debug) printf(" capture=%s", opts->record_debug);
            putchar('\n');
        }
        return 0;
    }

    if (opts->replay_debug) {
        transport = capture_replay_transport_open(opts->replay_debug, arch_name);
        if (!transport) {
            fprintf(stderr, "mkdbg: cannot replay debug capture %s for arch %s\n",
                    opts->replay_debug, arch_name);
            return 1;
        }
        printf("mkdbg debug replay  capture=%s  arch=%s\n",
               opts->replay_debug, arch_name);
    } else if (opts->use_probe) {
#ifdef MKDBG_PROBE_SUPPORT
        ProbeInfo probes[16];
        int n = probe_list(probes, 16);
        if (n < 0) {
            fprintf(stderr, "mkdbg: probe enumeration failed\n");
            return 1;
        }
        if (n == 0) {
            fprintf(stderr, "mkdbg: no debug probe detected\n");
            return 1;
        }
        int idx = opts->probe_idx;
        if (idx < 0) {
            if (n > 1) {
                fprintf(stderr, "mkdbg: %d probes found — select one with --probe N:\n", n);
                for (int i = 0; i < n; i++) {
                    fprintf(stderr, "  [%d] %s  serial: %s\n",
                            i,
                            probes[i].identifier[0] ? probes[i].identifier : "unknown",
                            probes[i].serial[0] ? probes[i].serial : "(none)");
                }
                return 1;
            }
            idx = 0;
        } else if (idx >= n) {
            fprintf(stderr, "mkdbg: --probe %d out of range (%d probe(s) found)\n", idx, n);
            return 1;
        }
        transport = probe_transport_open(idx, opts->chip);
        if (!transport) return 1;
        printf("mkdbg live debug  probe=%d  chip=%s\n",
               idx, opts->chip ? opts->chip : "auto");
#else
        fprintf(stderr,
                "mkdbg: probe support not compiled in; rebuild with cargo in PATH\n");
        return 1;
#endif
    } else {
        if (!port || !*port)
            die("debug requires --port or --probe; e.g. mkdbg debug --port /dev/ttyUSB0");
        transport = uart_transport_open(port, baud);
        if (!transport) return 1;

        printf("mkdbg live debug  port=%s  baud=%d\n", port, baud);
    }

    if (opts->record_debug) {
        transport = capture_transport_wrap(transport, opts->record_debug, arch_name);
        if (!transport) return 1;
        printf("debug capture=%s\n", opts->record_debug);
    }
    s = debug_session_open_transport(transport, arch);
    if (!s) return 1;

    /* Load DWARF line info if an ELF path was supplied */
    s_dbi = NULL;
    if (opts->elf_path && *opts->elf_path) {
        s_dbi = dwarf_open(opts->elf_path);
        if (s_dbi)
            printf("elf=%s  (source info loaded)\n", opts->elf_path);
        else
            printf("elf=%s  (warning: could not parse .debug_line)\n",
                   opts->elf_path);
    }

    printf("Type 'help' for commands.\n\n");

    /* Override default TCB offset if user supplied --freertos-tcb-offset */
    if (opts->freertos_name_offset != 0)
        s_freertos_offset = opts->freertos_name_offset;

    /* Reset state for this session */
    s_ndisplay  = 0;

    char line[256];
    for (;;) {
        printf("(mkdbg) ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) { printf("\n"); break; }
        trim_eol(line);
        if (!line[0]) continue;

        char cmd[32];
        const char *args = split_token(line, cmd, sizeof(cmd));

        if (strcmp(cmd, "break") == 0 || strcmp(cmd, "b") == 0) {
            do_break(s, args);
        } else if (strcmp(cmd, "clear") == 0) {
            do_clear(s, args);
        } else if (strcmp(cmd, "continue") == 0 || strcmp(cmd, "c") == 0) {
            printf("Continuing... (Ctrl-C to interrupt)\n");
            int rc = cli_continue(s);
            if (rc == WIRE_OK) print_halt(s);
            else printf("error: continue failed (rc=%d)\n", rc);
        } else if (strcmp(cmd, "interrupt") == 0 || strcmp(cmd, "int") == 0) {
            printf("Sending break-in...\n");
            int rc = debug_session_interrupt(s);
            if (rc == WIRE_OK) print_halt(s);
            else printf("error: interrupt failed (rc=%d; target halt not confirmed)\n", rc);
        } else if (strcmp(cmd, "step") == 0 || strcmp(cmd, "s") == 0) {
            int rc = debug_session_step(s);
            if (rc == WIRE_OK) print_halt(s);
            else printf("error: step failed (rc=%d)\n", rc);
        } else if (strcmp(cmd, "regs") == 0) {
            do_regs(s);
        } else if (strcmp(cmd, "mem") == 0) {
            do_mem(s, args);
        } else if (strcmp(cmd, "watch") == 0) {
            do_watch(s, args, WATCHPOINT_WRITE);
        } else if (strcmp(cmd, "rwatch") == 0) {
            do_watch(s, args, WATCHPOINT_READ);
        } else if (strcmp(cmd, "awatch") == 0) {
            do_watch(s, args, WATCHPOINT_ACCESS);
        } else if (strcmp(cmd, "delete") == 0) {
            char sub[16];
            const char *rest = split_token(args, sub, sizeof(sub));
            if (strcmp(sub, "watch") == 0) do_delete_watch(s, rest);
            else printf("delete: unknown subcommand '%s'\n", sub);
        } else if (strcmp(cmd, "display") == 0) {
            do_display(args);
        } else if (strcmp(cmd, "undisplay") == 0) {
            do_undisplay(args);
        } else if (strcmp(cmd, "info") == 0) {
            char sub[32];
            split_token(args, sub, sizeof(sub));
            if (strcmp(sub, "breakpoints") == 0) do_info_breakpoints(s);
            else if (strcmp(sub, "watchpoints") == 0) do_info_watchpoints(s);
            else if (strcmp(sub, "display") == 0) {
                if (s_ndisplay == 0) printf("No display addresses.\n");
                else for (int i = 0; i < s_ndisplay; i++)
                    printf("%d: 0x%08x\n", i + 1, s_display[i]);
            } else printf("info: unknown subcommand '%s'\n", sub);
        } else if (strcmp(cmd, "freertos") == 0) {
            char task_name[32] = "";
            if (read_freertos_task(s, task_name, sizeof(task_name)))
                printf("current task: %s\n", task_name);
            else if (!s_dbi)
                printf("error: pass --elf to use FreeRTOS awareness\n");
            else
                printf("(pxCurrentTCB not found — not a FreeRTOS build?)\n");
        } else if (strcmp(cmd, "tui") == 0) {
            int ret = debug_tui_run(s, s_dbi);
            if (ret == TUI_QUIT) {
                debug_session_detach(s);
                break;
            }
            printf("(back in CLI mode — type 'help' for commands)\n");
        } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
            print_help();
        } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0) {
            debug_session_detach(s);
            break;
        } else {
            printf("unknown command: %s  (type 'help')\n", cmd);
        }
    }

    debug_session_close(s);
    dwarf_close(s_dbi);
    s_dbi = NULL;
    return 0;
}
