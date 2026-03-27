/* debug_cli.c — Interactive REPL for live MCU debugging over UART
 *
 * Commands:
 *   break 0x<addr>      set FPBv1 hardware breakpoint
 *   clear <id>          clear breakpoint by number
 *   continue  (c)       resume MCU; block until next halt
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
#include "debug_tui.h"
#include "dwarf.h"
#include "wire_host.h"
#include "arch.h"

/* ── Session-scoped DWARF handle (NULL if no --elf provided) ─────────────── */

static DwarfDBI *s_dbi = NULL;

/* pcTaskName byte offset in TCB (default 52 per FreeRTOS 10.x standard config) */
static int s_freertos_offset = 52;

/* ── Breakpoint list ─────────────────────────────────────────────────────── */

#define BP_MAX 8  /* FPBv1 hardware limit */

typedef struct { int id; uint32_t addr; } Breakpoint;

static Breakpoint s_bp[BP_MAX];
static int        s_bp_count  = 0;
static int        s_bp_nextid = 1;

/* ── Watchpoint list (DWT) ───────────────────────────────────────────────── */

#define WP_MAX 4  /* DWT comparator hardware limit */

typedef struct { int id; uint32_t addr; WatchpointType type; } Watchpoint;

static Watchpoint s_wp[WP_MAX];
static int        s_wp_count  = 0;
static int        s_wp_nextid = 1;

static int wp_add(uint32_t addr, WatchpointType type)
{
    if (s_wp_count >= WP_MAX) return -1;
    s_wp[s_wp_count].id   = s_wp_nextid++;
    s_wp[s_wp_count].addr = addr;
    s_wp[s_wp_count].type = type;
    s_wp_count++;
    return s_wp[s_wp_count - 1].id;
}

static int wp_remove(int id, uint32_t *addr_out)
{
    for (int i = 0; i < s_wp_count; i++) {
        if (s_wp[i].id == id) {
            *addr_out = s_wp[i].addr;
            s_wp[i]   = s_wp[--s_wp_count];
            return 0;
        }
    }
    return -1;
}

/* ── Memory display list ─────────────────────────────────────────────────── */

#define DISPLAY_MAX 8

static uint32_t s_display[DISPLAY_MAX];
static int      s_ndisplay = 0;

static int bp_add(uint32_t addr)
{
    if (s_bp_count >= BP_MAX) return -1;
    s_bp[s_bp_count].id   = s_bp_nextid++;
    s_bp[s_bp_count].addr = addr;
    s_bp_count++;
    return s_bp[s_bp_count - 1].id;
}

/* Remove entry by id; return 0 and fill addr_out on success, -1 if not found. */
static int bp_remove(int id, uint32_t *addr_out)
{
    for (int i = 0; i < s_bp_count; i++) {
        if (s_bp[i].id == id) {
            *addr_out = s_bp[i].addr;
            s_bp[i]   = s_bp[--s_bp_count];  /* swap with last */
            return 0;
        }
    }
    return -1;
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

static uint32_t parse_addr(const char *s)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return (uint32_t)strtoul(s + 2, NULL, 16);
    return (uint32_t)strtoul(s, NULL, 16);
}

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
    if (args[0] == '0' && (args[1] == 'x' || args[1] == 'X')) {
        addr = parse_addr(args);
    } else if (s_dbi && dwarf_sym_to_addr(s_dbi, args, &addr) == 0) {
        printf("(symbol '%s' = 0x%08x)\n", args, addr);
    } else if (!s_dbi) {
        printf("error: pass --elf to use symbol names\n"); return;
    } else {
        printf("error: symbol '%s' not found in ELF\n", args); return;
    }
    if (debug_session_set_hw_breakpoint(s, addr) != WIRE_OK) {
        printf("error: could not set breakpoint (no free FPB comparator?)\n");
        return;
    }
    int id = bp_add(addr);
    if (id < 0) {
        debug_session_clear_hw_breakpoint(s, addr);  /* roll back */
        printf("error: breakpoint list full\n");
        return;
    }
    printf("Breakpoint %d at 0x%08x\n", id, addr);
}

static void do_clear(DebugSession *s, const char *args)
{
    if (!*args) { printf("usage: clear <id>\n"); return; }

    int id = atoi(args);
    uint32_t addr;
    if (bp_remove(id, &addr) != 0) {
        printf("error: no breakpoint %d\n", id);
        return;
    }
    if (debug_session_clear_hw_breakpoint(s, addr) != WIRE_OK)
        printf("warning: target returned error clearing breakpoint\n");
    else
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

    char addr_tok[32];
    const char *rest = split_token(args, addr_tok, sizeof(addr_tok));
    uint32_t addr = parse_addr(addr_tok);
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

static void do_info_breakpoints(void)
{
    if (s_bp_count == 0) { printf("No breakpoints.\n"); return; }
    printf("Num  Address\n");
    for (int i = 0; i < s_bp_count; i++)
        printf("%-4d 0x%08x\n", s_bp[i].id, s_bp[i].addr);
}

static void do_watch(DebugSession *s, const char *args, WatchpointType type)
{
    if (!*args) {
        printf("usage: watch/rwatch/awatch 0x<addr>\n"); return;
    }
    uint32_t addr = parse_addr(args);
    if (debug_session_set_watchpoint(s, addr, 1, type) != WIRE_OK) {
        printf("error: could not set watchpoint (no free DWT comparator?)\n");
        return;
    }
    int id = wp_add(addr, type);
    if (id < 0) {
        debug_session_clear_watchpoint(s, addr);
        printf("error: watchpoint list full\n");
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
    uint32_t addr;
    if (wp_remove(id, &addr) != 0) {
        printf("error: no watchpoint %d\n", id); return;
    }
    if (debug_session_clear_watchpoint(s, addr) != WIRE_OK)
        printf("warning: target returned error clearing watchpoint\n");
    else
        printf("Watchpoint %d (0x%08x) cleared.\n", id, addr);
}

static void do_info_watchpoints(void)
{
    if (s_wp_count == 0) { printf("No watchpoints.\n"); return; }
    printf("Num  Address     Type\n");
    for (int i = 0; i < s_wp_count; i++) {
        const char *t = (s_wp[i].type == WATCHPOINT_WRITE)  ? "write"  :
                        (s_wp[i].type == WATCHPOINT_READ)   ? "read"   : "access";
        printf("%-4d 0x%08x  %s\n", s_wp[i].id, s_wp[i].addr, t);
    }
}

static void do_display(const char *args)
{
    if (!*args) { printf("usage: display 0x<addr>\n"); return; }
    if (s_ndisplay >= DISPLAY_MAX) { printf("error: display list full\n"); return; }
    s_display[s_ndisplay++] = parse_addr(args);
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
        "  continue  (c)            resume execution; wait for next halt\n"
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

    if (!port || !*port)
        die("debug requires --port; e.g. mkdbg debug --port /dev/ttyUSB0");

    const char     *arch_name = opts->arch ? opts->arch : "cortex-m";
    const MkdbgArch *arch     = mkdbg_arch_find(arch_name);
    if (!arch || !arch->live_debug)
        die("arch '%s' does not support live debug", arch_name);

    DebugSession *s = debug_session_open(port, baud, arch);
    if (!s) return 1;

    printf("mkdbg live debug  port=%s  baud=%d\n", port, baud);

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
    s_bp_count  = 0;
    s_bp_nextid = 1;
    s_wp_count  = 0;
    s_wp_nextid = 1;
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
            printf("Continuing...\n");
            int rc = debug_session_continue(s);
            if (rc == WIRE_OK) print_halt(s);
            else printf("error: continue failed (rc=%d)\n", rc);
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
            if (strcmp(sub, "breakpoints") == 0) do_info_breakpoints();
            else if (strcmp(sub, "watchpoints") == 0) do_info_watchpoints();
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
