#include "mkdbg.h"

static void usage(void)
{
  printf("mkdbg-native %s\n", MKDBG_NATIVE_VERSION);
  printf("usage: mkdbg-native [--version] <init|doctor|repo|target|incident|build|flash|hil|snapshot|dashboard|watch|attach|probe|serial|git|run|capture|seam> [options]\n");
}

int main(int argc, char **argv)
{
  if (argc == 2 && strcmp(argv[1], "--version") == 0) {
    printf("mkdbg-native %s\n", MKDBG_NATIVE_VERSION);
    return 0;
  }
  if (argc < 2) {
    usage();
    return 2;
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
      return cmd_probe_action(&opts, "init; reset run; shutdown");
    }
    if (strcmp(argv[2], "halt") == 0) {
      parse_probe_args(argc - 3, argv + 3, &opts);
      return cmd_probe_action(&opts, "init; reset halt; shutdown");
    }
    if (strcmp(argv[2], "resume") == 0) {
      parse_probe_args(argc - 3, argv + 3, &opts);
      return cmd_probe_action(&opts, "init; resume; shutdown");
    }
    if (strcmp(argv[2], "flash") == 0) {
      parse_probe_args(argc - 3, argv + 3, &opts);
      return cmd_probe_flash(&opts);
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

  die("unknown command: %s", argv[1]);
  return 2;
}
