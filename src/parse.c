#include "mkdbg.h"

int parse_init_args(int argc, char **argv, InitOptions *opts)
{
  int i;
  opts->preset = "microkernel-mpu";
  opts->name = NULL;
  opts->port = NULL;
  opts->force = 0;

  for (i = 0; i < argc; ++i) {
    if (strcmp(argv[i], "--preset") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --preset");
      }
      opts->preset = argv[++i];
    } else if (strcmp(argv[i], "--name") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --name");
      }
      opts->name = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --port");
      }
      opts->port = argv[++i];
    } else if (strcmp(argv[i], "--force") == 0) {
      opts->force = 1;
    } else {
      die("unknown init argument: %s", argv[i]);
    }
  }
  if (strcmp(opts->preset, "microkernel-mpu") != 0 && strcmp(opts->preset, "generic") != 0) {
    die("unsupported preset: %s", opts->preset);
  }
  return 0;
}

int parse_doctor_args(int argc, char **argv, DoctorOptions *opts)
{
  int i;
  memset(opts, 0, sizeof(*opts));

  for (i = 0; i < argc; ++i) {
    if (strcmp(argv[i], "--target") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --target");
      }
      opts->target = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --port");
      }
      opts->port = argv[++i];
    } else if (argv[i][0] == '-') {
      die("unknown doctor argument: %s", argv[i]);
    } else if (opts->repo == NULL) {
      opts->repo = argv[i];
    } else {
      die("doctor accepts at most one repo name");
    }
  }
  return 0;
}

int parse_repo_add_args(int argc, char **argv, RepoAddOptions *opts)
{
  int i;
  memset(opts, 0, sizeof(*opts));
  opts->preset = "generic";
  if (argc < 1) {
    die("repo add requires a name");
  }
  opts->name = argv[0];
  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) opts->path = argv[++i];
    else if (strcmp(argv[i], "--preset") == 0 && i + 1 < argc) opts->preset = argv[++i];
    else if (strcmp(argv[i], "--default") == 0) opts->make_default = 1;
    else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) opts->port = argv[++i];
    else if (strcmp(argv[i], "--build-cmd") == 0 && i + 1 < argc) opts->build_cmd = argv[++i];
    else if (strcmp(argv[i], "--flash-cmd") == 0 && i + 1 < argc) opts->flash_cmd = argv[++i];
    else if (strcmp(argv[i], "--hil-cmd") == 0 && i + 1 < argc) opts->hil_cmd = argv[++i];
    else if (strcmp(argv[i], "--snapshot-cmd") == 0 && i + 1 < argc) opts->snapshot_cmd = argv[++i];
    else if (strcmp(argv[i], "--attach-cmd") == 0 && i + 1 < argc) opts->attach_cmd = argv[++i];
    else if (strcmp(argv[i], "--elf-path") == 0 && i + 1 < argc) opts->elf_path = argv[++i];
    else if (strcmp(argv[i], "--snapshot-output") == 0 && i + 1 < argc) opts->snapshot_output = argv[++i];
    else if (strcmp(argv[i], "--openocd-cfg") == 0 && i + 1 < argc) opts->openocd_cfg = argv[++i];
    else if (strcmp(argv[i], "--openocd-server-cmd") == 0 && i + 1 < argc) opts->openocd_server_cmd = argv[++i];
    else if (strcmp(argv[i], "--gdb") == 0 && i + 1 < argc) opts->gdb = argv[++i];
    else if (strcmp(argv[i], "--gdb-target") == 0 && i + 1 < argc) opts->gdb_target = argv[++i];
    else die("unknown repo add argument: %s", argv[i]);
  }
  if (opts->path == NULL) {
    die("repo add requires --path");
  }
  if (strcmp(opts->preset, "microkernel-mpu") != 0 && strcmp(opts->preset, "generic") != 0) {
    die("unsupported preset: %s", opts->preset);
  }
  return 0;
}

int parse_name_command_args(int argc, char **argv, NameCommandOptions *opts, const char *label)
{
  if (argc != 1) {
    die("%s requires exactly one name", label);
  }
  opts->name = argv[0];
  return 0;
}

int parse_incident_open_args(int argc, char **argv, IncidentOpenOptions *opts)
{
  int i;
  memset(opts, 0, sizeof(*opts));
  for (i = 0; i < argc; ++i) {
    if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) opts->target = argv[++i];
    else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) opts->name = argv[++i];
    else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) opts->port = argv[++i];
    else if (argv[i][0] == '-') die("unknown incident open argument: %s", argv[i]);
    else if (opts->repo == NULL) opts->repo = argv[i];
    else die("incident open accepts at most one repo name");
  }
  return 0;
}

int parse_incident_status_args(int argc, char **argv, IncidentStatusOptions *opts)
{
  int i;
  memset(opts, 0, sizeof(*opts));
  for (i = 0; i < argc; ++i) {
    if (strcmp(argv[i], "--json") == 0) opts->json = 1;
    else die("unknown incident status argument: %s", argv[i]);
  }
  return 0;
}

int parse_capture_bundle_args(int argc, char **argv, CaptureBundleOptions *opts)
{
  int i;
  memset(opts, 0, sizeof(*opts));
  for (i = 0; i < argc; ++i) {
    if (strcmp(argv[i], "--target") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --target");
      }
      opts->target = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --port");
      }
      opts->port = argv[++i];
    } else if (strcmp(argv[i], "--source-log") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --source-log");
      }
      opts->source_log = argv[++i];
    } else if (strcmp(argv[i], "--output") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --output");
      }
      opts->output = argv[++i];
    } else if (strcmp(argv[i], "--json") == 0) {
      opts->json = 1;
    } else if (strcmp(argv[i], "--dry-run") == 0) {
      opts->dry_run = 1;
    } else if (argv[i][0] == '-') {
      die("unknown capture bundle argument: %s", argv[i]);
    } else if (opts->repo == NULL) {
      opts->repo = argv[i];
    } else {
      die("capture bundle accepts at most one repo name");
    }
  }
  return 0;
}

int parse_watch_args(int argc, char **argv, WatchOptions *opts)
{
  int i;
  memset(opts, 0, sizeof(*opts));
  for (i = 0; i < argc; ++i) {
    if (strcmp(argv[i], "--target") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --target");
      }
      opts->target = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --port");
      }
      opts->port = argv[++i];
    } else if (strcmp(argv[i], "--bundle-json") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --bundle-json");
      }
      opts->bundle_json = argv[++i];
    } else if (strcmp(argv[i], "--source-log") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --source-log");
      }
      opts->source_log = argv[++i];
    } else if (strcmp(argv[i], "--auto-refresh-s") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --auto-refresh-s");
      }
      opts->auto_refresh_s = argv[++i];
    } else if (strcmp(argv[i], "--width") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --width");
      }
      opts->width = argv[++i];
    } else if (strcmp(argv[i], "--height") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --height");
      }
      opts->height = argv[++i];
    } else if (strcmp(argv[i], "--render-once") == 0) {
      opts->render_once = 1;
    } else if (strcmp(argv[i], "--dry-run") == 0) {
      opts->dry_run = 1;
    } else if (argv[i][0] == '-') {
      die("unknown watch argument: %s", argv[i]);
    } else if (opts->repo == NULL) {
      opts->repo = argv[i];
    } else {
      die("watch accepts at most one repo name");
    }
  }
  return 0;
}

int parse_attach_args(int argc, char **argv, AttachOptions *opts)
{
  int i;
  memset(opts, 0, sizeof(*opts));
  opts->server_wait_s = 1.2;

  for (i = 0; i < argc; ++i) {
    if (strcmp(argv[i], "--target") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --target");
      }
      opts->target = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --port");
      }
      opts->port = argv[++i];
    } else if (strcmp(argv[i], "--baud") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --baud");
      }
      opts->baud = argv[++i];
    } else if (strcmp(argv[i], "--break") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --break");
      }
      if (opts->breakpoint_count >= MAX_ATTACH_BREAKPOINTS) {
        die("too many --break arguments");
      }
      opts->breakpoints[opts->breakpoint_count++] = argv[++i];
    } else if (strcmp(argv[i], "--command") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --command");
      }
      if (opts->gdb_command_count >= MAX_ATTACH_COMMANDS) {
        die("too many --command arguments");
      }
      opts->gdb_commands[opts->gdb_command_count++] = argv[++i];
    } else if (strcmp(argv[i], "--batch") == 0) {
      opts->batch = 1;
    } else if (strcmp(argv[i], "--dry-run") == 0) {
      opts->dry_run = 1;
    } else if (strcmp(argv[i], "--server-wait-s") == 0) {
      char *end = NULL;
      if (i + 1 >= argc) {
        die("missing value for --server-wait-s");
      }
      opts->server_wait_s = strtod(argv[++i], &end);
      if (end == NULL || *end != '\0' || opts->server_wait_s < 0.0) {
        die("invalid value for --server-wait-s: %s", argv[i]);
      }
    } else if (argv[i][0] == '-') {
      die("unknown attach argument: %s", argv[i]);
    } else if (opts->repo == NULL) {
      opts->repo = argv[i];
    } else {
      die("attach accepts at most one repo name");
    }
  }
  return 0;
}

int parse_probe_args(int argc, char **argv, ProbeOptions *opts)
{
  int i;
  memset(opts, 0, sizeof(*opts));

  for (i = 0; i < argc; ++i) {
    if (strcmp(argv[i], "--target") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --target");
      }
      opts->target = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --port");
      }
      opts->port = argv[++i];
    } else if (strcmp(argv[i], "--dry-run") == 0) {
      opts->dry_run = 1;
    } else if (argv[i][0] == '-') {
      die("unknown probe argument: %s", argv[i]);
    } else if (opts->repo == NULL) {
      opts->repo = argv[i];
    } else {
      die("probe accepts at most one repo name");
    }
  }
  return 0;
}

int parse_run_args(int argc, char **argv, RunOptions *opts)
{
  int i;
  int command_index = -1;

  memset(opts, 0, sizeof(*opts));
  for (i = 0; i < argc; ++i) {
    if (strcmp(argv[i], "--") == 0) {
      command_index = i + 1;
      break;
    }
    if (strcmp(argv[i], "--target") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --target");
      }
      opts->target = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --port");
      }
      opts->port = argv[++i];
    } else if (strcmp(argv[i], "--dry-run") == 0) {
      opts->dry_run = 1;
    } else if (argv[i][0] == '-') {
      die("unknown run argument: %s", argv[i]);
    } else if (opts->repo == NULL) {
      opts->repo = argv[i];
    } else {
      die("run requires a command after `--`");
    }
  }

  if (command_index < 0 || command_index >= argc) {
    die("run requires a command after `--`");
  }
  opts->command = argv + command_index;
  opts->command_argc = argc - command_index;
  return 0;
}

int parse_action_args(int argc, char **argv, ActionOptions *opts)
{
  int i;
  memset(opts, 0, sizeof(*opts));
  for (i = 0; i < argc; ++i) {
    if (strcmp(argv[i], "--target") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --target");
      }
      opts->target = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --port");
      }
      opts->port = argv[++i];
    } else if (strcmp(argv[i], "--dry-run") == 0) {
      opts->dry_run = 1;
    } else if (argv[i][0] == '-') {
      die("unknown action argument: %s", argv[i]);
    } else if (opts->repo == NULL) {
      opts->repo = argv[i];
    } else {
      die("action accepts at most one repo name");
    }
  }
  return 0;
}

int parse_git_args(int argc, char **argv, GitOptions *opts)
{
  int i;
  memset(opts, 0, sizeof(*opts));
  for (i = 0; i < argc; ++i) {
    if (strcmp(argv[i], "--target") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --target");
      }
      opts->target = argv[++i];
    } else if (strcmp(argv[i], "--dry-run") == 0) {
      opts->dry_run = 1;
    } else if (argv[i][0] == '-') {
      die("unknown git argument: %s", argv[i]);
    } else if (opts->repo == NULL) {
      opts->repo = argv[i];
    } else {
      die("git accepts at most one repo name");
    }
  }
  return 0;
}

int parse_serial_args(int argc, char **argv, SerialOptions *opts)
{
  int i;
  memset(opts, 0, sizeof(*opts));
  for (i = 0; i < argc; ++i) {
    if (strcmp(argv[i], "--target") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --target");
      }
      opts->target = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --port");
      }
      opts->port = argv[++i];
    } else if (strcmp(argv[i], "--baud") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --baud");
      }
      opts->baud = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--char-delay-ms") == 0) {
      if (i + 1 >= argc) {
        die("missing value for --char-delay-ms");
      }
      opts->char_delay_ms = atof(argv[++i]);
    } else if (strcmp(argv[i], "--dry-run") == 0) {
      opts->dry_run = 1;
    } else if (argv[i][0] == '-') {
      die("unknown serial argument: %s", argv[i]);
    } else if (opts->repo == NULL) {
      opts->repo = argv[i];
    } else {
      die("serial accepts at most one repo name");
    }
  }
  return 0;
}

int parse_dashboard_args(int argc, char **argv, DashboardOptions *opts)
{
  int i;
  memset(opts, 0, sizeof(*opts));

  for (i = 0; i < argc; ++i) {
    if (strcmp(argv[i], "--repo") == 0 || strcmp(argv[i], "-r") == 0) {
      if (i + 1 >= argc) die("missing value for --repo");
      opts->repo = argv[++i];
    } else if (strcmp(argv[i], "--target") == 0) {
      if (i + 1 >= argc) die("missing value for --target");
      opts->target = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0) {
      if (i + 1 >= argc) die("missing value for --port");
      opts->port = argv[++i];
    } else if (strcmp(argv[i], "--baud") == 0) {
      if (i + 1 >= argc) die("missing value for --baud");
      opts->baud = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--dry-run") == 0) {
      opts->dry_run = 1;
    } else if (argv[i][0] == '-') {
      die("unknown dashboard argument: %s", argv[i]);
    } else if (opts->repo == NULL) {
      opts->repo = argv[i];
    } else {
      die("dashboard accepts at most one repo name");
    }
  }
  return 0;
}

int parse_debug_args(int argc, char **argv, DebugOptions *opts)
{
  int i;
  memset(opts, 0, sizeof(*opts));

  for (i = 0; i < argc; ++i) {
    if (strcmp(argv[i], "--port") == 0) {
      if (i + 1 >= argc) die("missing value for --port");
      opts->port = argv[++i];
    } else if (strcmp(argv[i], "--baud") == 0) {
      if (i + 1 >= argc) die("missing value for --baud");
      opts->baud = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--elf") == 0) {
      if (i + 1 >= argc) die("missing value for --elf");
      opts->elf_path = argv[++i];
    } else if (strcmp(argv[i], "--arch") == 0) {
      if (i + 1 >= argc) die("missing value for --arch");
      opts->arch = argv[++i];
    } else if (argv[i][0] == '-') {
      die("unknown debug argument: %s", argv[i]);
    } else {
      die("debug takes no positional arguments");
    }
  }
  return 0;
}
