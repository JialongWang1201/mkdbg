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

/* ── Session-scoped DWARF handle (NULL if no --elf provided) ─────────────── */

static DwarfDBI *s_dbi = NULL;

/* ── Breakpoint list ─────────────────────────────────────────────────────── */

#define BP_MAX 8  /* FPBv1 hardware limit */

typedef struct { int id; uint32_t addr; } Breakpoint;

static Breakpoint s_bp[BP_MAX];
static int        s_bp_count  = 0;
static int        s_bp_nextid = 1;

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

static const char *reg_name(int i)
{
    static const char *names[17] = {
        "r0","r1","r2","r3","r4","r5","r6","r7",
        "r8","r9","r10","r11","r12","sp","lr","pc","xpsr"
    };
    return (i >= 0 && i < 17) ? names[i] : "??";
}

static void print_halt(DebugSession *s)
{
    int sig = debug_session_last_signal(s);
    printf("Halted.  signal=%d (%s)", sig, signal_name(sig));
    uint32_t regs[DEBUG_SESSION_NREGS];
    if (debug_session_read_regs(s, regs) == WIRE_OK) {
        uint32_t pc = regs[15];
        printf("  pc=0x%08x", pc);
        if (s_dbi) {
            DwarfLocation loc;
            if (dwarf_pc_to_location(s_dbi, pc, &loc) == 0)
                printf("  %s:%d", loc.file, loc.line);
        }
    }
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
    uint32_t regs[DEBUG_SESSION_NREGS];
    if (debug_session_read_regs(s, regs) != WIRE_OK) {
        printf("error: failed to read registers\n");
        return;
    }
    /* r0–r12: four per row */
    for (int i = 0; i <= 12; i++) {
        printf("%-4s 0x%08x", reg_name(i), regs[i]);
        printf(((i % 4) == 3 || i == 12) ? "\n" : "   ");
    }
    printf("sp   0x%08x   lr   0x%08x   pc   0x%08x\n",
           regs[13], regs[14], regs[15]);
    printf("xpsr 0x%08x\n", regs[16]);
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

static void print_help(void)
{
    printf(
        "Commands:\n"
        "  break 0x<addr>      set hardware breakpoint (FPBv1)\n"
        "  break <symbol>      set breakpoint at named function (requires --elf)\n"
        "  clear <id>          clear breakpoint by number\n"
        "  continue  (c)       resume execution; wait for next halt\n"
        "  step      (s)       single-step one instruction\n"
        "  regs                print all CPU registers\n"
        "  mem 0x<addr> [len]  hexdump memory (default 16 bytes, max 256)\n"
        "  info breakpoints    list active breakpoints\n"
        "  tui                 TUI mode (PR 11)\n"
        "  quit      (q)       resume MCU and exit\n"
    );
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int cmd_debug(const DebugOptions *opts)
{
    const char *port = opts->port;
    int         baud = opts->baud ? opts->baud : DEFAULT_BAUD;

    if (!port || !*port)
        die("debug requires --port; e.g. mkdbg debug --port /dev/ttyUSB0");

    DebugSession *s = debug_session_open(port, baud);
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

    /* Reset state for this session */
    s_bp_count  = 0;
    s_bp_nextid = 1;

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
        } else if (strcmp(cmd, "info") == 0) {
            char sub[32];
            split_token(args, sub, sizeof(sub));
            if (strcmp(sub, "breakpoints") == 0) do_info_breakpoints();
            else printf("info: unknown subcommand '%s'\n", sub);
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
