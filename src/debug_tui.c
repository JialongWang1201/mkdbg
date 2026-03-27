/* debug_tui.c — 4-panel TUI debugger (termbox2-based)
 *
 * Layout (dynamic, fits any terminal ≥ 22×60):
 *
 *   ┌─ Source: foo.c:43 ──────────────────────────────────────────┐
 *   │   41:  void task_dispatch(TaskID id) {                      │
 *   │   42:    if (id >= TASK_COUNT) return;                      │
 *   │ ► 43:    handler_table[id]();            ← PC               │
 *   │   44:  }                                                    │
 *   ├─ Registers ──────────────────────────┬─ Breakpoints ────────┤
 *   │ r0   0x00000000  r8   0x00000000    │ #1  0x08001234        │
 *   │ r1   0x00000000  r9   0x00000000    │ #2  0x08002000        │
 *   │ sp   0x00000000  lr   0x00000000    │                       │
 *   │ pc   0x08001234  xpsr 0x21000000    │                       │
 *   ├─ Memory Watch ────────────────────────────────────────────────┤
 *   │ 0x20001234: 1a 00 00 00 ff ff ff ff  00 00 00 00 ab cd ef 01 │
 *   └────────────────────────────────────────────────────────────────┘
 *   [s]tep  [c]ontinue  [b]reak  [d]el  [m]em  [t]cli  [q]uit
 *
 * SPDX-License-Identifier: MIT
 */

#include "termbox2.h"

#include "debug_tui.h"
#include "mkdbg.h"
#include "wire_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Layout ──────────────────────────────────────────────────────────────── */

#define REG_ROWS     8    /* register panel content rows (r0-r12 + sp/lr/pc/xpsr) */
#define CTX_HALF     4    /* source context: ±4 lines around PC = 9 visible lines */
#define MAX_WATCHES  4

/* ── Local state ─────────────────────────────────────────────────────────── */

#define TUI_BP_MAX 8
typedef struct { int id; uint32_t addr; } TuiBP;

static TuiBP    s_bp[TUI_BP_MAX];
static int      s_nbp      = 0;
static int      s_bp_next  = 1;

static uint32_t s_watches[MAX_WATCHES];
static int      s_nwatches = 0;

static uint32_t s_regs[DEBUG_SESSION_NREGS];
static int      s_regs_ok  = 0;

static uint32_t s_pc       = 0;

/* ── Box-drawing helpers ─────────────────────────────────────────────────── */

/* Fill a horizontal run with '─' from x0 to x1-1. */
static void hline(int y, int x0, int x1)
{
    for (int x = x0; x < x1; x++)
        tb_print(x, y, TB_DEFAULT, TB_DEFAULT, "\xe2\x94\x80"); /* U+2500 ─ */
}

/* Print a UTF-8 box-drawing char at (x,y). */
static void bchar(int x, int y, const char *utf8)
{
    tb_print(x, y, TB_DEFAULT, TB_DEFAULT, utf8);
}

/* ── Source panel ────────────────────────────────────────────────────────── */

static void draw_source(int y0, int src_h, int w, DwarfDBI *dbi, uint32_t pc)
{
    DwarfLocation loc;
    int has_loc = (dbi != NULL && dwarf_pc_to_location(dbi, pc, &loc) == 0);

    /* Top border */
    bchar(0, y0, "\xe2\x94\x8c");           /* ┌ */
    if (has_loc) {
        const char *base = strrchr(loc.file, '/');
        base = base ? base + 1 : loc.file;
        char hdr[128];
        int hdr_len = snprintf(hdr, sizeof(hdr), "\xe2\x94\x80 Source: %s:%d ",
                               base, loc.line);
        tb_print(1, y0, TB_CYAN, TB_DEFAULT, hdr);
        hline(y0, 1 + hdr_len, w - 1);
    } else {
        tb_print(1, y0, TB_CYAN, TB_DEFAULT,
                 "\xe2\x94\x80 Source ");
        hline(y0, 9, w - 1);
    }
    bchar(w - 1, y0, "\xe2\x94\x90");       /* ┐ */

    /* Content */
    if (!has_loc) {
        int mid = y0 + 1 + src_h / 2;
        for (int r = 1; r <= src_h; r++) {
            bchar(0, y0 + r, "\xe2\x94\x82");        /* │ */
            if (y0 + r == mid) {
                if (dbi)
                    tb_printf(2, y0 + r, TB_DEFAULT, TB_DEFAULT,
                              "[no source for pc=0x%08x]", pc);
                else
                    tb_print(2, y0 + r, TB_DEFAULT, TB_DEFAULT,
                             "[pass --elf firmware.elf for source context]");
            }
            bchar(w - 1, y0 + r, "\xe2\x94\x82");    /* │ */
        }
        return;
    }

    int start = loc.line - CTX_HALF;
    if (start < 1) start = 1;

    FILE *f = fopen(loc.file, "r");
    int row = 1;
    if (f) {
        char buf[512];
        int lineno = 1;
        while (lineno < start && fgets(buf, sizeof(buf), f))
            lineno++;

        int content_w = w - 10;  /* ─ │(1) + prefix(6) + space(1) + content + │(1) */
        if (content_w < 1) content_w = 1;

        while (row <= src_h && fgets(buf, sizeof(buf), f)) {
            size_t n = strlen(buf);
            while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
            buf[content_w < (int)sizeof(buf)-1 ? content_w : (int)sizeof(buf)-1] = '\0';

            int is_pc = (lineno == loc.line);
            uintattr_t fg = is_pc ? (TB_WHITE | TB_BOLD) : TB_DEFAULT;
            uintattr_t bg = is_pc ? TB_BLUE : TB_DEFAULT;

            bchar(0, y0 + row, "\xe2\x94\x82");            /* │ */
            if (is_pc)
                tb_printf(1, y0 + row, fg, bg, " \xe2\x96\xba%4d: %-*s",
                          lineno, content_w, buf);          /* ► */
            else
                tb_printf(1, y0 + row, fg, bg, "  %4d: %-*s",
                          lineno, content_w, buf);
            bchar(w - 1, y0 + row, "\xe2\x94\x82");        /* │ */
            lineno++;
            row++;
        }
        fclose(f);
    }
    while (row <= src_h) {
        bchar(0, y0 + row, "\xe2\x94\x82");
        bchar(w - 1, y0 + row, "\xe2\x94\x82");
        row++;
    }
}

/* ── Register/Breakpoint panel ───────────────────────────────────────────── */

/* Register name at index 0-16 */
static const char *rname(int i)
{
    static const char *n[] = {
        "r0","r1","r2","r3","r4","r5","r6","r7",
        "r8","r9","r10","r11","r12","sp","lr","pc","xpsr"
    };
    return (i >= 0 && i < 17) ? n[i] : "??";
}

/*
 * Register pairs displayed per row (left index, right index).
 * Rows: 0..REG_ROWS-1 = 8 rows.
 */
static const int REG_PAIRS[REG_ROWS][2] = {
    {0, 8}, {1, 9}, {2, 10}, {3, 11}, {4, 12},
    {5, 13},   /* r5, sp */
    {14, 15},  /* lr, pc */
    {6, 16},   /* r6, xpsr */
};

static void draw_reg_bp(int y0, int h, int w, DwarfDBI *dbi, uint32_t pc)
{
    int split = w / 2;  /* column of the vertical separator ─ │ ─ */

    /* Separator row (top) */
    bchar(0, y0, "\xe2\x94\x9c");                     /* ├ */
    tb_print(1, y0, TB_CYAN, TB_DEFAULT,
             "\xe2\x94\x80 Registers ");
    hline(y0, 12, split);
    bchar(split, y0, "\xe2\x94\xac");                  /* ┬ */
    tb_print(split + 1, y0, TB_CYAN, TB_DEFAULT,
             "\xe2\x94\x80 Breakpoints ");
    hline(y0, split + 14, w - 1);
    bchar(w - 1, y0, "\xe2\x94\xa4");                  /* ┤ */

    int reg_content_w = split - 3;   /* usable chars in left half */
    int bp_content_w  = w - split - 3;

    for (int r = 0; r < h; r++) {
        bchar(0, y0 + 1 + r, "\xe2\x94\x82");          /* │ left edge */

        /* ── Registers (left half) ── */
        if (r < REG_ROWS && s_regs_ok) {
            int li = REG_PAIRS[r][0];
            int ri = REG_PAIRS[r][1];
            /* highlight pc with bold */
            uintattr_t pc_fg = TB_WHITE | TB_BOLD;
            uintattr_t pc_bg = TB_BLUE;
            uintattr_t lfg = (li == 15) ? pc_fg : TB_DEFAULT;
            uintattr_t lbg = (li == 15) ? pc_bg : TB_DEFAULT;
            uintattr_t rfg = (ri == 15) ? pc_fg : TB_DEFAULT;
            uintattr_t rbg = (ri == 15) ? pc_bg : TB_DEFAULT;
            char lbuf[20], rbuf[20];
            snprintf(lbuf, sizeof(lbuf), "%-4s 0x%08x", rname(li), s_regs[li]);
            snprintf(rbuf, sizeof(rbuf), "%-4s 0x%08x", rname(ri), s_regs[ri]);
            tb_printf(1, y0 + 1 + r, lfg, lbg, " %-*s", reg_content_w / 2, lbuf);
            tb_printf(1 + reg_content_w / 2 + 1, y0 + 1 + r, rfg, rbg,
                      " %-*s", reg_content_w / 2, rbuf);
        }
        (void)reg_content_w;

        bchar(split, y0 + 1 + r, "\xe2\x94\x82");      /* │ middle */

        /* ── Breakpoints (right half) ── */
        if (r < s_nbp) {
            DwarfLocation loc;
            char loc_str[64] = "";
            if (dbi && dwarf_pc_to_location(dbi, s_bp[r].addr, &loc) == 0) {
                const char *base = strrchr(loc.file, '/');
                base = base ? base + 1 : loc.file;
                snprintf(loc_str, sizeof(loc_str), " %s:%d", base, loc.line);
            }
            tb_printf(split + 1, y0 + 1 + r, TB_DEFAULT, TB_DEFAULT,
                      " #%-2d 0x%08x%-*s",
                      s_bp[r].id, s_bp[r].addr,
                      bp_content_w - 18, loc_str);
        }
        (void)bp_content_w;

        bchar(w - 1, y0 + 1 + r, "\xe2\x94\x82");      /* │ right edge */
    }

    /* Drain extra rows if panel is taller than REG_ROWS */
    (void)pc;
}

/* ── Memory Watch panel ──────────────────────────────────────────────────── */

static void draw_mem(int y0, int h, int w, DebugSession *s)
{
    /* Separator */
    bchar(0, y0, "\xe2\x94\x9c");                      /* ├ */
    tb_print(1, y0, TB_CYAN, TB_DEFAULT,
             "\xe2\x94\x80 Memory Watch ");
    hline(y0, 15, w - 1);
    bchar(w - 1, y0, "\xe2\x94\xa4");                  /* ┤ */

    for (int r = 0; r < h; r++) {
        bchar(0, y0 + 1 + r, "\xe2\x94\x82");
        if (r < s_nwatches) {
            uint8_t buf[16];
            if (debug_session_read_mem(s, s_watches[r], 16, buf) == WIRE_OK) {
                char hex[64];
                int pos = 0;
                for (int i = 0; i < 16; i++) {
                    if (i == 8) hex[pos++] = ' ';
                    pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", buf[i]);
                }
                hex[pos > 0 ? pos - 1 : 0] = '\0';
                tb_printf(2, y0 + 1 + r, TB_DEFAULT, TB_DEFAULT,
                          "0x%08x: %s", s_watches[r], hex);
            } else {
                tb_printf(2, y0 + 1 + r, TB_RED, TB_DEFAULT,
                          "0x%08x: <read error>", s_watches[r]);
            }
        }
        bchar(w - 1, y0 + 1 + r, "\xe2\x94\x82");
    }
}

/* ── Bottom border + hint bar ────────────────────────────────────────────── */

static void draw_bottom(int y_border, int y_hint, int w)
{
    bchar(0, y_border, "\xe2\x94\x94");                /* └ */
    hline(y_border, 1, w - 1);
    bchar(w - 1, y_border, "\xe2\x94\x98");            /* ┘ */

    tb_print(0, y_hint, TB_YELLOW, TB_DEFAULT,
             "[s]tep [c]ontinue [b]reak [d]el [m]em [t]cli [q]uit");
}

/* ── Status bar (replaces hint bar during blocking ops) ──────────────────── */

static void draw_status(int y, int w, const char *msg)
{
    for (int x = 0; x < w; x++)
        tb_print(x, y, TB_DEFAULT, TB_DEFAULT, " ");
    tb_print(0, y, TB_WHITE | TB_BOLD, TB_DEFAULT, msg);
    tb_present();
}

/* ── Full redraw ─────────────────────────────────────────────────────────── */

static void redraw(DebugSession *s, DwarfDBI *dbi)
{
    int w = tb_width();
    int h = tb_height();

    tb_clear();

    /* Compute panel heights */
    int mem_h  = (s_nwatches > 0) ? s_nwatches : 1;
    int src_h  = h - REG_ROWS - mem_h - 6;
    /* 6 = top_border(1) + reg_sep(1) + mem_sep(1) + bottom_border(1) + hint(1) + 1 spare */
    if (src_h < 3) src_h = 3;

    int y_src_top  = 0;
    int y_reg_sep  = y_src_top + 1 + src_h;
    int y_mem_sep  = y_reg_sep + 1 + REG_ROWS;
    int y_bottom   = y_mem_sep + 1 + mem_h;
    int y_hint     = y_bottom + 1;

    draw_source(y_src_top, src_h, w, dbi, s_pc);
    draw_reg_bp(y_reg_sep, REG_ROWS, w, dbi, s_pc);
    draw_mem(y_mem_sep, mem_h, w, s);
    draw_bottom(y_bottom, y_hint, w);

    tb_present();
}

/* ── Input prompt at the hint row ────────────────────────────────────────── */

/* Read a line of input at the bottom bar.  Returns 0 on Enter, -1 on Escape. */
static int tui_prompt(int y, int w, const char *label, char *out, int out_sz)
{
    int len = 0;
    out[0] = '\0';

    for (;;) {
        /* Redraw prompt row */
        for (int x = 0; x < w; x++)
            tb_print(x, y, TB_DEFAULT, TB_DEFAULT, " ");
        tb_printf(0, y, TB_YELLOW, TB_DEFAULT, "%s%.*s", label, len, out);
        tb_present();

        struct tb_event ev;
        tb_poll_event(&ev);
        if (ev.type != TB_EVENT_KEY) continue;

        if (ev.key == TB_KEY_ENTER) return 0;
        if (ev.key == TB_KEY_ESC)   return -1;
        if (ev.key == TB_KEY_BACKSPACE || ev.key == TB_KEY_BACKSPACE2) {
            if (len > 0) out[--len] = '\0';
        } else if (ev.ch && len + 1 < out_sz) {
            /* encode ch (Unicode) as UTF-8 — we only expect ASCII hex digits */
            if (ev.ch < 0x80) {
                out[len++] = (char)ev.ch;
                out[len]   = '\0';
            }
        }
    }
}

/* ── Breakpoint helpers ──────────────────────────────────────────────────── */

static int tui_bp_add(DebugSession *s, uint32_t addr)
{
    if (s_nbp >= TUI_BP_MAX) return -1;
    if (debug_session_set_hw_breakpoint(s, addr) != WIRE_OK) return -1;
    s_bp[s_nbp].id   = s_bp_next++;
    s_bp[s_nbp].addr = addr;
    s_nbp++;
    return 0;
}

static int tui_bp_del(DebugSession *s, int id)
{
    for (int i = 0; i < s_nbp; i++) {
        if (s_bp[i].id == id) {
            debug_session_clear_hw_breakpoint(s, s_bp[i].addr);
            s_bp[i] = s_bp[--s_nbp];
            return 0;
        }
    }
    return -1;
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int debug_tui_run(DebugSession *s, DwarfDBI *dbi)
{
    if (tb_init() != TB_OK) {
        fprintf(stderr, "tui: failed to initialise termbox2\n");
        return TUI_CLI;
    }

    /* Reset local state */
    s_nbp      = 0;
    s_bp_next  = 1;
    s_nwatches = 0;
    s_regs_ok  = 0;
    s_pc       = 0;

    /* Read initial register state */
    if (debug_session_read_regs(s, s_regs) == WIRE_OK) {
        s_regs_ok = 1;
        s_pc = s_regs[15];
    }

    redraw(s, dbi);

    int result = TUI_CLI;

    for (;;) {
        struct tb_event ev;
        tb_poll_event(&ev);

        if (ev.type == TB_EVENT_RESIZE) {
            redraw(s, dbi);
            continue;
        }
        if (ev.type != TB_EVENT_KEY) continue;

        int w = tb_width();
        int h = tb_height();
        int y_hint = h - 1;

        /* ── Key dispatch ── */
        if (ev.ch == 's') {
            draw_status(y_hint, w, "Stepping...");
            int rc = debug_session_step(s);
            if (rc == WIRE_OK && debug_session_read_regs(s, s_regs) == WIRE_OK) {
                s_regs_ok = 1;
                s_pc = s_regs[15];
            }
            redraw(s, dbi);

        } else if (ev.ch == 'c') {
            draw_status(y_hint, w, "Running...  (waiting for breakpoint)");
            int rc = debug_session_continue(s);
            if (rc == WIRE_OK && debug_session_read_regs(s, s_regs) == WIRE_OK) {
                s_regs_ok = 1;
                s_pc = s_regs[15];
            }
            redraw(s, dbi);

        } else if (ev.ch == 'b') {
            char inp[64] = "";
            if (tui_prompt(y_hint, w, "break> ", inp, sizeof(inp)) == 0
                    && inp[0]) {
                uint32_t addr = 0;
                if ((inp[0] == '0' && (inp[1] == 'x' || inp[1] == 'X')) ||
                    (inp[0] >= '0' && inp[0] <= '9')) {
                    addr = (uint32_t)strtoul(inp, NULL, 16);
                } else if (dbi) {
                    dwarf_sym_to_addr(dbi, inp, &addr);
                }
                if (addr) tui_bp_add(s, addr);
            }
            redraw(s, dbi);

        } else if (ev.ch == 'd') {
            char inp[16] = "";
            if (tui_prompt(y_hint, w, "delete bp id> ", inp, sizeof(inp)) == 0
                    && inp[0]) {
                tui_bp_del(s, atoi(inp));
            }
            redraw(s, dbi);

        } else if (ev.ch == 'm') {
            if (s_nwatches < MAX_WATCHES) {
                char inp[32] = "";
                if (tui_prompt(y_hint, w, "watch addr> 0x", inp, sizeof(inp)) == 0
                        && inp[0]) {
                    s_watches[s_nwatches++] = (uint32_t)strtoul(inp, NULL, 16);
                }
            }
            redraw(s, dbi);

        } else if (ev.ch == 't') {
            result = TUI_CLI;
            break;

        } else if (ev.ch == 'q' || ev.key == TB_KEY_CTRL_C) {
            result = TUI_QUIT;
            break;
        }
    }

    tb_shutdown();
    return result;
}
