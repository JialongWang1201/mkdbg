/* mkdbg_dashboard.c — mkdbg dashboard sub-command
 *
 * Live TUI showing three panels:
 *   - Serial output  (real data when a serial port is configured / specified)
 *   - Git status     (branch + clean/dirty, via libgit2 or subprocess)
 *   - Probe status   (stub: "no probe connected" until Phase 3)
 *
 * Uses termbox2 (vendored at tools/termbox2.h) for all terminal control.
 * Single-threaded select/poll design — no threads, no signals to catch.
 *
 * Key bindings:
 *   q / Ctrl-C / Esc   quit
 *   r                  refresh git status now
 *   c                  clear serial panel
 *
 * SPDX-License-Identifier: MIT
 */

#include "mkdbg.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define TB_IMPL
#include "termbox2.h"
#pragma GCC diagnostic pop

#ifdef MKDBG_USE_LG2
#  include <git2.h>
#endif

/* ── constants ──────────────────────────────────────────────────────────── */

#define DASH_SERIAL_LINES    512   /* ring buffer capacity (lines)      */
#define DASH_LINE_MAX        256   /* max chars per stored line         */
#define DASH_STATUS_W_MIN     26   /* minimum status panel width        */
#define DASH_STATUS_W_MAX     40   /* maximum status panel width        */
#define DASH_GIT_REFRESH_S    10   /* git auto-refresh interval (s)     */
#define DASH_POLL_MS          80   /* event poll timeout (ms)           */
#define DASH_PROBE_INTERVAL_MS 500 /* wire-host --dump poll interval    */

/* ── serial ring buffer ─────────────────────────────────────────────────── */

typedef struct {
  char lines[DASH_SERIAL_LINES][DASH_LINE_MAX];
  int  head;        /* next write slot                  */
  int  count;       /* lines stored (≤ DASH_SERIAL_LINES) */
  char partial[DASH_LINE_MAX];
  int  partial_len;
} SerialRing;

static void ring_init(SerialRing *r)
{
  memset(r, 0, sizeof(*r));
}

static void ring_push(SerialRing *r, const char *line, int len)
{
  int n = len < DASH_LINE_MAX - 1 ? len : DASH_LINE_MAX - 1;
  memcpy(r->lines[r->head], line, (size_t)n);
  r->lines[r->head][n] = '\0';
  r->head = (r->head + 1) % DASH_SERIAL_LINES;
  if (r->count < DASH_SERIAL_LINES) r->count++;
}

static void ring_feed(SerialRing *r, const char *buf, int n)
{
  int i;
  for (i = 0; i < n; i++) {
    char c = buf[i];
    if (c == '\n' || c == '\r') {
      if (r->partial_len > 0) {
        ring_push(r, r->partial, r->partial_len);
        r->partial_len = 0;
      }
    } else if (r->partial_len < DASH_LINE_MAX - 1) {
      r->partial[r->partial_len++] = c;
    }
  }
}

static void ring_clear(SerialRing *r)
{
  r->head = r->count = r->partial_len = 0;
}

/* Get the i-th line from the bottom (0 = most recent). */
static const char *ring_get(const SerialRing *r, int from_bottom)
{
  if (from_bottom >= r->count) return NULL;
  int idx = (r->head - 1 - from_bottom + DASH_SERIAL_LINES) % DASH_SERIAL_LINES;
  return r->lines[idx];
}

/* ── git state ──────────────────────────────────────────────────────────── */

typedef struct {
  char   branch[96];  /* "main", "8ab1cd23 (detached)", "(unborn)", "--" */
  int    dirty;       /* 0=clean  1=dirty  -1=unknown                   */
  int    dirty_n;     /* number of changed files                        */
  time_t refreshed;
} GitState;

static void git_state_init(GitState *gs)
{
  memset(gs, 0, sizeof(*gs));
  copy_string(gs->branch, sizeof(gs->branch), "--");
  gs->dirty = -1;
}

#ifdef MKDBG_USE_LG2

static void refresh_git_lg2(GitState *gs, const char *root)
{
  git_repository  *repo = NULL;
  git_reference   *head = NULL;
  git_status_list *list = NULL;

  git_libgit2_init();

  if (git_repository_open(&repo, root) < 0) {
    copy_string(gs->branch, sizeof(gs->branch), "--");
    gs->dirty = -1;
    goto done;
  }

  /* Branch line */
  if (git_repository_head_unborn(repo)) {
    copy_string(gs->branch, sizeof(gs->branch), "(unborn)");
  } else if (git_repository_head_detached(repo)) {
    if (git_repository_head(&head, repo) == 0) {
      const git_oid *o = git_reference_target(head);
      if (o) {
        char sha[GIT_OID_SHA1_HEXSIZE + 1];
        git_oid_tostr(sha, sizeof(sha), o);
        snprintf(gs->branch, sizeof(gs->branch), "%.8s (det)", sha);
      } else {
        copy_string(gs->branch, sizeof(gs->branch), "(detached)");
      }
      git_reference_free(head);
      head = NULL;
    } else {
      copy_string(gs->branch, sizeof(gs->branch), "(detached)");
    }
  } else {
    if (git_repository_head(&head, repo) == 0) {
      copy_string(gs->branch, sizeof(gs->branch),
                  git_reference_shorthand(head));
      git_reference_free(head);
      head = NULL;
    }
  }

  /* Dirty check */
  {
    git_status_options sopts;
    memset(&sopts, 0, sizeof(sopts));
    sopts.version = GIT_STATUS_OPTIONS_VERSION;
    sopts.show    = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    sopts.flags   = GIT_STATUS_OPT_INCLUDE_UNTRACKED
                  | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
    if (git_status_list_new(&list, repo, &sopts) == 0) {
      size_t cnt = git_status_list_entrycount(list);
      gs->dirty_n = (int)cnt;
      gs->dirty   = cnt > 0 ? 1 : 0;
      git_status_list_free(list);
    } else {
      gs->dirty = -1;
    }
  }

done:
  git_repository_free(repo);
  git_libgit2_shutdown();
}

#endif /* MKDBG_USE_LG2 */

#ifndef MKDBG_USE_LG2
static void refresh_git_sub(GitState *gs, const char *root)
{
  char cmd[PATH_MAX + 128];
  FILE *f;

  /* branch */
  snprintf(cmd, sizeof(cmd),
           "git -C \"%s\" rev-parse --abbrev-ref HEAD 2>/dev/null", root);
  f = popen(cmd, "r");
  if (f) {
    char line[128] = {0};
    if (fgets(line, (int)sizeof(line), f)) {
      size_t n = strlen(line);
      while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
      copy_string(gs->branch, sizeof(gs->branch), n ? line : "--");
    }
    pclose(f);
  }

  /* dirty count */
  snprintf(cmd, sizeof(cmd),
           "git -C \"%s\" status --porcelain --untracked-files=no 2>/dev/null"
           " | wc -l", root);
  f = popen(cmd, "r");
  if (f) {
    int cnt = 0;
    if (fscanf(f, "%d", &cnt) == 1) {
      gs->dirty_n = cnt;
      gs->dirty   = cnt > 0 ? 1 : 0;
    }
    pclose(f);
  }
}
#endif /* !MKDBG_USE_LG2 */

static void refresh_git(GitState *gs, const char *root)
{
#ifdef MKDBG_USE_LG2
  refresh_git_lg2(gs, root);
#else
  refresh_git_sub(gs, root);
#endif
  gs->refreshed = time(NULL);
}

/* ── probe state ─────────────────────────────────────────────────────────── */

typedef struct {
  WireCrashReport report;
  int             has_crash;     /* 1 if a crash was detected          */
  pid_t           subprocess;    /* -1 = none running                  */
  int             pipe_fd;       /* read end of subprocess stdout pipe  */
  uint64_t        last_spawn_ms; /* time of last spawn (ms since epoch) */
} ProbeState;

static void probe_state_init(ProbeState *ps)
{
  memset(ps, 0, sizeof(*ps));
  ps->subprocess = -1;
  ps->pipe_fd    = -1;
}

static uint64_t now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

/*
 * Called once per main loop tick when a port is configured.
 * Spawns wire-host --dump every DASH_PROBE_INTERVAL_MS; skips if one is
 * already running; parses result when the subprocess finishes.
 * Returns 1 if the probe state changed (needs redraw), 0 otherwise.
 */
static int probe_tick(ProbeState *ps, const char *port, const char *baud)
{
  if (!port) return 0;

  uint64_t now = now_ms();

  /* If a subprocess is running, poll it */
  if (ps->subprocess > 0) {
    WireCrashReport tmp;
    int done = wire_probe_poll(ps->subprocess, ps->pipe_fd, &tmp);
    if (done > 0) {
      close(ps->pipe_fd);
      ps->pipe_fd    = -1;
      ps->subprocess = -1;
      if (tmp.halt_signal > 0 && !tmp.timeout) {
        ps->report    = tmp;
        ps->has_crash = 1;
        return 1;
      }
    } else if (done < 0) {
      close(ps->pipe_fd);
      ps->pipe_fd    = -1;
      ps->subprocess = -1;
    }
    return 0;
  }

  /* Spawn a new subprocess if the interval has elapsed */
  if (now - ps->last_spawn_ms < DASH_PROBE_INTERVAL_MS) return 0;
  ps->last_spawn_ms = now;

  int pipe_fd = -1;
  pid_t pid = wire_probe_start(port, baud, &pipe_fd);
  if (pid > 0) {
    ps->subprocess = pid;
    ps->pipe_fd    = pipe_fd;
  }
  return 0;
}

/* ── drawing ────────────────────────────────────────────────────────────── */

static void hline(int y, int x0, int x1, uintattr_t fg)
{
  int x;
  for (x = x0; x <= x1; x++)
    tb_set_cell(x, y, '-', fg, TB_DEFAULT);
}

static void vline(int x, int y0, int y1, uintattr_t fg)
{
  int y;
  for (y = y0; y <= y1; y++)
    tb_set_cell(x, y, '|', fg, TB_DEFAULT);
}

static void fill_row(int y, int x0, int w, uintattr_t fg, uintattr_t bg)
{
  int x;
  for (x = x0; x < x0 + w; x++)
    tb_set_cell(x, y, ' ', fg, bg);
}

/* Print s at (x,y), clamped to max_w characters. */
static void pclip(int x, int y, uintattr_t fg, uintattr_t bg,
                  const char *s, int max_w)
{
  char tmp[DASH_LINE_MAX];
  int  len;
  if (max_w <= 0 || !s || !*s) return;
  len = (int)strlen(s);
  if (len > max_w) len = max_w;
  memcpy(tmp, s, (size_t)len);
  tmp[len] = '\0';
  tb_print(x, y, fg, bg, tmp);
}

static void do_redraw(const SerialRing *ring, const GitState *gs,
                      const ProbeState *ps,
                      const char *port_label, int baud)
{
  int W = tb_width();
  int H = tb_height();
  if (W < 12 || H < 6) return;

  /* Right panel is 1/4 of width, clamped to [MIN, MAX] */
  int sw = W / 4;
  if (sw < DASH_STATUS_W_MIN) sw = DASH_STATUS_W_MIN;
  if (sw > DASH_STATUS_W_MAX) sw = DASH_STATUS_W_MAX;
  if (sw >= W - 4) sw = W - 4;

  int ser_w = W - sw - 1;  /* width of serial panel  */
  int div_x = ser_w;       /* x of vertical divider  */
  int hdr_y = 1;           /* y of section headers   */
  int bot_y = H - 2;       /* y of bottom border row */
  int con_h = bot_y - hdr_y - 1; /* content rows     */

  tb_clear();

  /* ── title bar ── */
  fill_row(0, 0, W, TB_BLACK, TB_CYAN);
  {
    char ttl[128];
    snprintf(ttl, sizeof(ttl), " mkdbg dashboard  %s @ %d",
             port_label ? port_label : "(no port)", baud);
    tb_print(0, 0, TB_BLACK, TB_CYAN, ttl);
    const char *ver = " v" MKDBG_NATIVE_VERSION " ";
    int vlen = (int)strlen(ver);
    if (W - vlen > (int)strlen(ttl))
      tb_print(W - vlen, 0, TB_BLACK, TB_CYAN, ver);
  }

  /* ── section headers ── */
  tb_print(1, hdr_y, TB_CYAN | TB_BOLD, TB_DEFAULT, "─ Serial ");
  hline(hdr_y, 10, ser_w - 1, TB_CYAN);
  tb_print(div_x + 2, hdr_y, TB_YELLOW | TB_BOLD, TB_DEFAULT, "─ Status ");
  hline(hdr_y, div_x + 11, W - 1, TB_YELLOW);

  /* ── divider ── */
  vline(div_x, hdr_y + 1, bot_y - 1, TB_WHITE);

  /* ── bottom help bar ── */
  fill_row(H - 1, 0, W, TB_BLACK, TB_WHITE);
  tb_print(1, H - 1, TB_BLACK, TB_WHITE,
           "[q]quit  [r]refresh git  [c]clear serial");

  /* ── serial panel: bottom-up ── */
  {
    int i;
    for (i = 0; i < con_h; i++) {
      int row = bot_y - 1 - i;
      const char *line = ring_get(ring, i);
      if (!line || !*line) continue;
      uintattr_t fg = TB_WHITE;
      if (line[0] == 'E' || line[0] == 'F' || line[0] == 'H')
        fg = TB_RED;
      else if (line[0] == '[' || line[0] == '!')
        fg = TB_YELLOW;
      pclip(1, row, fg, TB_DEFAULT, line, ser_w - 2);
    }
  }

  /* ── status panel ── */
  {
    int sx = div_x + 2;
    int sw2 = W - sx - 1;
    int sy  = hdr_y + 1;

    /* GIT */
    tb_print(sx, sy, TB_GREEN | TB_BOLD, TB_DEFAULT, "GIT");
    sy++;
    {
      char tmp[64];
      snprintf(tmp, sizeof(tmp), "branch: %s", gs->branch);
      pclip(sx, sy, TB_WHITE, TB_DEFAULT, tmp, sw2);
      sy++;
    }
    if (gs->dirty < 0) {
      pclip(sx, sy, TB_WHITE, TB_DEFAULT, "status: --", sw2);
    } else if (gs->dirty == 0) {
      pclip(sx, sy, TB_GREEN, TB_DEFAULT, "status: clean", sw2);
    } else {
      char tmp[48];
      snprintf(tmp, sizeof(tmp), "status: %d changed", gs->dirty_n);
      pclip(sx, sy, TB_YELLOW, TB_DEFAULT, tmp, sw2);
    }
    sy++;
    if (gs->refreshed > 0) {
      int age = (int)(time(NULL) - gs->refreshed);
      char tmp[32];
      if (age < 60) snprintf(tmp, sizeof(tmp), "(%ds ago)", age);
      else          snprintf(tmp, sizeof(tmp), "(%dm ago)", age / 60);
      pclip(sx, sy, TB_DEFAULT, TB_DEFAULT, tmp, sw2);
      sy++;
    }
    sy++; /* spacer */

    /* PROBE */
    if (sy < bot_y - 1) {
      tb_print(sx, sy, TB_YELLOW | TB_BOLD, TB_DEFAULT, "PROBE");
      sy++;
    }
    if (ps && ps->has_crash) {
      const WireCrashReport *r = &ps->report;
      if (sy < bot_y - 1) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "CRASH  signal %d", r->halt_signal);
        pclip(sx, sy, TB_RED | TB_BOLD, TB_DEFAULT, tmp, sw2);
        sy++;
      }
      if (sy < bot_y - 1) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "PC  %s", r->regs[15]);
        pclip(sx, sy, TB_RED, TB_DEFAULT, tmp, sw2);
        sy++;
      }
      if (sy < bot_y - 1 && r->cfsr_decoded[0]) {
        pclip(sx, sy, TB_YELLOW, TB_DEFAULT, r->cfsr_decoded, sw2);
        sy++;
      }
      if (sy < bot_y - 1 && r->nframes > 0) {
        char tmp[128] = "frames:";
        for (int i = 0; i < r->nframes && i < 3; i++) {
          char f[16];
          snprintf(f, sizeof(f), " %s", r->stack_frames[i]);
          append_string(tmp, sizeof(tmp), f);
        }
        pclip(sx, sy, TB_WHITE, TB_DEFAULT, tmp, sw2);
        sy++;
      }
    } else if (ps && ps->subprocess > 0) {
      if (sy < bot_y - 1) {
        pclip(sx, sy, TB_DEFAULT, TB_DEFAULT, "probing...", sw2);
        sy++;
      }
    } else {
      if (sy < bot_y - 1) {
        pclip(sx, sy, TB_DEFAULT, TB_DEFAULT, "no crash detected", sw2);
        sy++;
      }
    }
    sy++; /* spacer */

    /* BUILD */
    if (sy < bot_y - 1) {
      tb_print(sx, sy, TB_MAGENTA | TB_BOLD, TB_DEFAULT, "BUILD");
      sy++;
    }
    if (sy < bot_y - 1) {
      pclip(sx, sy, TB_DEFAULT, TB_DEFAULT, "--", sw2);
    }
  }

  tb_present();
}

/* ── atexit wrapper (tb_shutdown returns int; atexit wants void fn) ─────── */

static void tb_shutdown_atexit(void)
{
  tb_shutdown();
}

/* ── cmd_dashboard ──────────────────────────────────────────────────────── */

int cmd_dashboard(const DashboardOptions *opts)
{
  SerialRing ring;
  GitState   gs;
  ProbeState ps;
  char       config_path[PATH_MAX] = {0};
  char       repo_root[PATH_MAX]   = {0};
  const char *port = NULL;
  int        serial_fd = -1;
  int        baud;

  ring_init(&ring);
  git_state_init(&gs);
  probe_state_init(&ps);

  /* ── resolve config → repo root and default port ── */
  {
    MkdbgConfig cfg;
    if (find_config_upward(config_path, sizeof(config_path)) == 0
        && load_config_file(config_path, &cfg) == 0) {
      const char *rname = NULL;
      if (resolve_repo_name(&cfg, opts->repo, opts->target, &rname) == 0) {
        const RepoConfig *rc = find_repo_const(&cfg, rname);
        if (rc) {
          resolve_repo_root(config_path, rc, repo_root, sizeof(repo_root));
          if (!port && rc->port[0])
            port = rc->port;
        }
      }
    }
  }

  /* --port flag overrides config */
  if (opts->port) port = opts->port;
  baud = opts->baud > 0 ? opts->baud : DEFAULT_BAUD;
  char baud_str[16];
  snprintf(baud_str, sizeof(baud_str), "%d", baud);

  if (opts->dry_run) {
    printf("[dry-run] dashboard port=%s baud=%d repo=%s\n",
           port ? port : "(none)", baud,
           repo_root[0] ? repo_root : "(none)");
    return 0;
  }

  /* ── open serial port non-blocking (failure is non-fatal) ── */
  if (port) {
    serial_fd = open(port, O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (serial_fd >= 0) {
      struct termios tio;
      if (tcgetattr(serial_fd, &tio) == 0) {
        cfmakeraw(&tio);
        cfsetispeed(&tio, baud_to_speed(baud));
        tio.c_cflag |= (CLOCAL | CREAD);
        tcsetattr(serial_fd, TCSANOW, &tio);
      }
    }
    /* Not fatal — dashboard still shows git / probe panels without serial */
  }

  /* ── initial git refresh ── */
  if (repo_root[0])
    refresh_git(&gs, repo_root);

  /* ── init termbox2 ── */
  if (tb_init() != TB_OK) {
    fprintf(stderr, "mkdbg: failed to initialise terminal UI\n");
    if (serial_fd >= 0) close(serial_fd);
    return 1;
  }
  atexit(tb_shutdown_atexit);

  /* initial draw */
  do_redraw(&ring, &gs, &ps, port, baud);

  /* ── main event loop ── */
  char rbuf[512];
  for (;;) {
    /* read serial bytes (non-blocking) */
    if (serial_fd >= 0) {
      ssize_t n = read(serial_fd, rbuf, sizeof(rbuf));
      if (n > 0) {
        ring_feed(&ring, rbuf, (int)n);
        do_redraw(&ring, &gs, &ps, port, baud);
      } else if (n == 0) {
        /* device disconnected */
        close(serial_fd);
        serial_fd = -1;
      }
      /* n == -1 && errno == EAGAIN: no data yet — that's fine */
    }

    /* poll for terminal events (keyboard / resize) */
    struct tb_event ev;
    int need_redraw = 0;
    if (tb_peek_event(&ev, DASH_POLL_MS) == TB_OK) {
      if (ev.type == TB_EVENT_KEY) {
        if (ev.key == TB_KEY_CTRL_C || ev.key == TB_KEY_ESC || ev.ch == 'q')
          break;
        if (ev.ch == 'r') {
          if (repo_root[0]) refresh_git(&gs, repo_root);
          need_redraw = 1;
        }
        if (ev.ch == 'c') {
          ring_clear(&ring);
          need_redraw = 1;
        }
      }
      if (ev.type == TB_EVENT_RESIZE)
        need_redraw = 1;
    }

    /* auto-refresh git every DASH_GIT_REFRESH_S seconds */
    if (repo_root[0] && time(NULL) - gs.refreshed >= DASH_GIT_REFRESH_S) {
      refresh_git(&gs, repo_root);
      need_redraw = 1;
    }

    /* probe tick: spawn/poll wire-host --dump every DASH_PROBE_INTERVAL_MS */
    if (port && probe_tick(&ps, port, baud_str))
      need_redraw = 1;

    if (need_redraw)
      do_redraw(&ring, &gs, &ps, port, baud);
  }

  tb_shutdown();
  if (serial_fd >= 0) close(serial_fd);
  if (ps.subprocess > 0) {
    terminate_pid(ps.subprocess);
    close(ps.pipe_fd);
  }
  return 0;
}
