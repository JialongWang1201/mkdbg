#include "mkdbg.h"

/* ── ANSI color macros ──────────────────────────────────────────────────── */
/* Active only when g_color == 1 (stdout is a TTY).                         */
#define C_BOLD   (g_color ? "\033[1m"    : "")
#define C_DIM    (g_color ? "\033[2m"    : "")
#define C_CYAN   (g_color ? "\033[1;36m" : "")
#define C_RESET  (g_color ? "\033[0m"    : "")

static int g_color = 0;   /* set once in main() via isatty(STDOUT_FILENO) */

/* ── ASCII logo + colored usage ─────────────────────────────────────────── */
static void usage(void)
{
  printf("\n%s"
    " _ __ ___  | | __ __| | |__   __ _\n"
    "| '_ ` _ \\ | |/ // _` | '_ \\ / _` |\n"
    "| | | | | ||   <| (_| | |_) | (_| |\n"
    "|_| |_| |_||_|\\_\\__,_|_.__/ \\__, |\n"
    "                              |___/%s  %s%s%s\n\n",
    C_CYAN, C_RESET, C_DIM, MKDBG_NATIVE_VERSION, C_RESET);

  printf("  Crash diagnostics for embedded firmware"
         " — over UART, no debug probe needed.\n\n");
  printf("  %sUsage:%s  mkdbg <command> [options]\n\n", C_BOLD, C_RESET);

  printf("%sSetup:%s\n", C_BOLD, C_RESET);
  printf("  %sinit%s      %screate .mkdbg.toml for this repo%s\n",
         C_BOLD, C_RESET, C_DIM, C_RESET);
  printf("  %sdoctor%s    %scheck tool + config health%s\n\n",
         C_BOLD, C_RESET, C_DIM, C_RESET);

  printf("%sDebug (no probe needed):%s\n", C_BOLD, C_RESET);
  printf("  %sattach%s    %sread crash report from MCU over UART%s\n",
         C_BOLD, C_RESET, C_DIM, C_RESET);
  printf("  %sdebug%s     %slive interactive debugger (breakpoints, step, registers)%s\n",
         C_BOLD, C_RESET, C_DIM, C_RESET);
  printf("  %sseam%s      %scausal-chain analysis from fault event ring%s\n\n",
         C_BOLD, C_RESET, C_DIM, C_RESET);

  printf("%sBuild & control:%s\n", C_BOLD, C_RESET);
  printf("  %sbuild%s     %srun build_cmd from config%s\n",
         C_BOLD, C_RESET, C_DIM, C_RESET);
  printf("  %sflash%s     %srun flash_cmd from config%s\n",
         C_BOLD, C_RESET, C_DIM, C_RESET);
  printf("  %sprobe%s     %shardware control (halt, resume, reset, read32, write32)%s\n\n",
         C_BOLD, C_RESET, C_DIM, C_RESET);

  printf("%sMonitor:%s\n", C_BOLD, C_RESET);
  printf("  %sdashboard%s %slive TUI: serial output + git status + crash probe%s\n",
         C_BOLD, C_RESET, C_DIM, C_RESET);
  printf("  %swatch%s     %swatch crash bundles in terminal%s\n",
         C_BOLD, C_RESET, C_DIM, C_RESET);
  printf("  %ssnapshot%s  %scapture triage bundle%s\n\n",
         C_BOLD, C_RESET, C_DIM, C_RESET);

  printf("%sSession:%s\n", C_BOLD, C_RESET);
  printf("  %sincident%s  %sopen/status/close a debug incident%s\n",
         C_BOLD, C_RESET, C_DIM, C_RESET);
  printf("  %scapture%s   %scapture and bundle crash artifacts%s\n",
         C_BOLD, C_RESET, C_DIM, C_RESET);
  printf("  %srepo%s/%starget%s  %smanage repo aliases%s\n",
         C_BOLD, C_RESET, C_BOLD, C_RESET, C_DIM, C_RESET);
  printf("  %srun%s       %srun arbitrary command in repo context%s\n\n",
         C_BOLD, C_RESET, C_DIM, C_RESET);

  printf("%sQuick start:%s\n", C_BOLD, C_RESET);
  printf("  mkdbg init --name myboard --port /dev/ttyUSB0\n");
  printf("  mkdbg attach --port /dev/ttyUSB0\n\n");
  printf("  %sDocs:%s https://github.com/JialongWang1201/mkdbg\n\n", C_DIM, C_RESET);
}

/* ── First-run wizard ───────────────────────────────────────────────────── */
/* Fires only when: argc < 2 AND stdout+stdin are TTYs AND no config found. */
static int run_first_run_wizard(void)
{
  char name[64];
  char port[64];
  size_t nl;

  /* banner */
  printf("\n%s"
    " _ __ ___  | | __ __| | |__   __ _\n"
    "| '_ ` _ \\ | |/ // _` | '_ \\ / _` |\n"
    "| | | | | ||   <| (_| | |_) | (_| |\n"
    "|_| |_| |_||_|\\_\\__,_|_.__/ \\__, |\n"
    "                              |___/%s  %s%s%s\n\n",
    C_CYAN, C_RESET, C_DIM, MKDBG_NATIVE_VERSION, C_RESET);

  printf("  %sNo .mkdbg.toml found — let's set up mkdbg for this repo.%s\n",
         C_BOLD, C_RESET);
  printf("  Press Enter to continue, or Ctrl-C to cancel.\n");
  fflush(stdout);
  if (fgets(name, sizeof(name), stdin) == NULL) {
    printf("\n");
    return 1;
  }

  /* board name */
  printf("\n  Board name (e.g. stm32, my-board): ");
  fflush(stdout);
  if (fgets(name, sizeof(name), stdin) == NULL) {
    printf("\n");
    return 1;
  }
  nl = strcspn(name, "\n");
  name[nl] = '\0';
  if (name[0] == '\0') {
    fprintf(stderr, "error: board name is required\n");
    return 1;
  }

  /* UART port */
  printf("  UART port (e.g. /dev/ttyUSB0): ");
  fflush(stdout);
  if (fgets(port, sizeof(port), stdin) == NULL) {
    printf("\n");
    return 1;
  }
  nl = strcspn(port, "\n");
  port[nl] = '\0';
  if (port[0] == '\0') {
    fprintf(stderr, "error: UART port is required\n");
    return 1;
  }

  /* call cmd_init with the collected options */
  InitOptions opts;
  memset(&opts, 0, sizeof(opts));
  opts.name = name;
  opts.port = port;

  printf("\n");
  int rc = cmd_init(&opts);
  if (rc == 0) {
    printf("\n  %s✓  Created .mkdbg.toml%s\n\n", C_CYAN, C_RESET);
    printf("  Next steps:\n");
    printf("    mkdbg doctor                     — verify your setup\n");
    printf("    mkdbg attach --port %s", port);
    /* pad so the comment aligns reasonably */
    int portlen = (int)strlen(port);
    int pad = 30 - portlen;
    for (int i = 0; i < pad; i++) printf(" ");
    printf("— read crash report\n\n");
  }
  return rc;
}

int main(int argc, char **argv)
{
  g_color = isatty(STDOUT_FILENO);

  if (argc == 2 && strcmp(argv[1], "--version") == 0) {
    printf("mkdbg %s\n", MKDBG_NATIVE_VERSION);
    return 0;
  }
  if (argc < 2 ||
      strcmp(argv[1], "--help") == 0 ||
      strcmp(argv[1], "-h") == 0 ||
      strcmp(argv[1], "help") == 0) {
    /* First-run wizard when no args, interactive, and no config present */
    if (argc < 2 && g_color && isatty(STDIN_FILENO)) {
      char config_path[PATH_MAX];
      if (find_config_upward(config_path, sizeof(config_path)) != 0) {
        return run_first_run_wizard();
      }
    }
    usage();
    return (argc < 2) ? 2 : 0;
  }

  if (strcmp(argv[1], "init") == 0) {
    InitOptions opts;
    parse_init_args(argc - 2, argv + 2, &opts);
    return cmd_init(&opts);
  }

  if (strcmp(argv[1], "doctor") == 0) {
    DoctorOptions opts;
    parse_doctor_args(argc - 2, argv + 2, &opts);
    return cmd_doctor(&opts);
  }

  if (strcmp(argv[1], "build") == 0) {
    ActionOptions opts;
    parse_action_args(argc - 2, argv + 2, &opts);
    return cmd_configured_action(&opts, "build_cmd", 0);
  }

  if (strcmp(argv[1], "flash") == 0) {
    ActionOptions opts;
    parse_action_args(argc - 2, argv + 2, &opts);
    return cmd_configured_action(&opts, "flash_cmd", 0);
  }

  if (strcmp(argv[1], "hil") == 0) {
    ActionOptions opts;
    parse_action_args(argc - 2, argv + 2, &opts);
    return cmd_configured_action(&opts, "hil_cmd", 1);
  }

  if (strcmp(argv[1], "snapshot") == 0) {
    ActionOptions opts;
    parse_action_args(argc - 2, argv + 2, &opts);
    return cmd_configured_action(&opts, "snapshot_cmd", 1);
  }

  if (strcmp(argv[1], "repo") == 0 || strcmp(argv[1], "target") == 0) {
    const int is_target = (strcmp(argv[1], "target") == 0);
    if (argc < 3) {
      die("%s requires a subcommand", argv[1]);
    }
    if (strcmp(argv[2], "add") == 0) {
      RepoAddOptions opts;
      parse_repo_add_args(argc - 3, argv + 3, &opts);
      return cmd_repo_add(&opts);
    }
    if (strcmp(argv[2], "list") == 0) {
      return cmd_repo_list();
    }
    if (strcmp(argv[2], "use") == 0) {
      NameCommandOptions opts;
      parse_name_command_args(argc - 3, argv + 3, &opts, is_target ? "target use" : "repo use");
      return cmd_repo_use(&opts);
    }
    die("unknown %s subcommand: %s", argv[1], argv[2]);
  }

  if (strcmp(argv[1], "incident") == 0) {
    if (argc < 3) {
      die("incident requires a subcommand");
    }
    if (strcmp(argv[2], "open") == 0) {
      IncidentOpenOptions opts;
      parse_incident_open_args(argc - 3, argv + 3, &opts);
      return cmd_incident_open(&opts);
    }
    if (strcmp(argv[2], "status") == 0) {
      IncidentStatusOptions opts;
      parse_incident_status_args(argc - 3, argv + 3, &opts);
      return cmd_incident_status(&opts);
    }
    if (strcmp(argv[2], "close") == 0) {
      if (argc != 3) {
        die("incident close accepts no extra arguments");
      }
      return cmd_incident_close();
    }
    die("unknown incident subcommand: %s", argv[2]);
  }

  if (strcmp(argv[1], "capture") == 0) {
    if (argc < 3) {
      die("capture requires a subcommand");
    }
    if (strcmp(argv[2], "bundle") == 0) {
      CaptureBundleOptions opts;
      parse_capture_bundle_args(argc - 3, argv + 3, &opts);
      return cmd_capture_bundle(&opts);
    }
    die("unknown capture subcommand: %s", argv[2]);
  }

  if (strcmp(argv[1], "dashboard") == 0) {
    DashboardOptions opts;
    parse_dashboard_args(argc - 2, argv + 2, &opts);
    return cmd_dashboard(&opts);
  }

  if (strcmp(argv[1], "watch") == 0) {
    WatchOptions opts;
    parse_watch_args(argc - 2, argv + 2, &opts);
    return cmd_watch(&opts);
  }

  if (strcmp(argv[1], "attach") == 0) {
    AttachOptions opts;
    parse_attach_args(argc - 2, argv + 2, &opts);
    return cmd_attach(&opts);
  }

  if (strcmp(argv[1], "probe") == 0) {
    ProbeOptions opts;
    if (argc < 3) {
      die("probe requires a subcommand");
    }
    if (strcmp(argv[2], "reset") == 0) {
      parse_probe_args(argc - 3, argv + 3, &opts);
      return cmd_probe_reset(&opts);
    }
    if (strcmp(argv[2], "halt") == 0) {
      parse_probe_args(argc - 3, argv + 3, &opts);
      return cmd_probe_halt(&opts);
    }
    if (strcmp(argv[2], "resume") == 0) {
      parse_probe_args(argc - 3, argv + 3, &opts);
      return cmd_probe_resume(&opts);
    }
    if (strcmp(argv[2], "flash") == 0) {
      fprintf(stderr, "mkdbg: probe flash removed — use `mkdbg flash` instead\n");
      return 1;
    }
    if (strcmp(argv[2], "read32") == 0) {
      if (argc < 5) {
        die("probe read32 requires an address");
      }
      parse_probe_args(argc - 4, argv + 3, &opts);
      opts.address = argv[argc - 1];
      return cmd_probe_read32(&opts);
    }
    if (strcmp(argv[2], "write32") == 0) {
      if (argc < 6) {
        die("probe write32 requires an address and value");
      }
      parse_probe_args(argc - 5, argv + 3, &opts);
      opts.address = argv[argc - 2];
      opts.value = argv[argc - 1];
      return cmd_probe_write32(&opts);
    }
    die("unknown probe subcommand: %s", argv[2]);
  }

  if (strcmp(argv[1], "serial") == 0) {
    SerialOptions opts;
    if (argc < 3) {
      die("serial requires a subcommand: tail, send");
    }
    if (strcmp(argv[2], "tail") == 0) {
      parse_serial_args(argc - 3, argv + 3, &opts);
      return cmd_serial_tail(&opts);
    }
    if (strcmp(argv[2], "send") == 0) {
      if (argc < 4) {
        die("serial send requires a message argument");
      }
      parse_serial_args(argc - 4, argv + 3, &opts);
      opts.message = argv[argc - 1];
      return cmd_serial_send(&opts);
    }
    die("unknown serial subcommand: %s", argv[2]);
  }

  if (strcmp(argv[1], "git") == 0) {
    GitOptions opts;
    if (argc < 3) {
      die("git requires a subcommand: status, rev, new-branch, worktree, push-current");
    }
    if (strcmp(argv[2], "status") == 0) {
      parse_git_args(argc - 3, argv + 3, &opts);
      return cmd_git_status(&opts);
    }
    if (strcmp(argv[2], "rev") == 0) {
      parse_git_args(argc - 3, argv + 3, &opts);
      return cmd_git_rev(&opts);
    }
    if (strcmp(argv[2], "new-branch") == 0) {
      if (argc < 4) {
        die("git new-branch requires a branch name");
      }
      parse_git_args(argc - 4, argv + 3, &opts);
      opts.branch_name = argv[argc - 1];
      return cmd_git_new_branch(&opts);
    }
    if (strcmp(argv[2], "worktree") == 0) {
      if (argc < 4) {
        die("git worktree requires a path");
      }
      parse_git_args(argc - 4, argv + 3, &opts);
      opts.path = argv[argc - 1];
      return cmd_git_worktree(&opts);
    }
    if (strcmp(argv[2], "push-current") == 0) {
      parse_git_args(argc - 3, argv + 3, &opts);
      return cmd_git_push_current(&opts);
    }
    die("unknown git subcommand: %s", argv[2]);
  }

  if (strcmp(argv[1], "run") == 0) {
    RunOptions opts;
    parse_run_args(argc - 2, argv + 2, &opts);
    return cmd_run(&opts);
  }

  if (strcmp(argv[1], "seam") == 0) {
    if (argc < 3) {
      die("seam requires a subcommand: analyze");
    }
    return mkdbg_cmd_seam(argc - 2, argv + 2);
  }

  if (strcmp(argv[1], "debug") == 0) {
    DebugOptions opts;
    parse_debug_args(argc - 2, argv + 2, &opts);
    return cmd_debug(&opts);
  }

  die("unknown command: %s", argv[1]);
  return 2;
}
