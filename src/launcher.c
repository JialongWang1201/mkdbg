#include "mkdbg.h"

int cmd_capture_bundle(const CaptureBundleOptions *opts)
{
  char config_path[PATH_MAX];
  char repo_root[PATH_MAX];
  char script_path[PATH_MAX];
  char source_log_path[PATH_MAX];
  char output_path[PATH_MAX];
  char incident_dir[PATH_MAX];
  const char *repo_name;
  const RepoConfig *repo;
  const char *port;
  MkdbgConfig config;
  char *argv[10];
  int argc = 0;

  if (find_config_upward(config_path, sizeof(config_path)) != 0) {
    die("missing %s; run `mkdbg init` first", CONFIG_NAME);
  }
  if (load_config_file(config_path, &config) != 0) {
    die("invalid config: %s", config_path);
  }
  resolve_repo_name(&config, opts->repo, opts->target, &repo_name);
  repo = find_repo_const(&config, repo_name);
  if (repo == NULL) {
    die("repo `%s` not found in %s", repo_name, config_path);
  }
  if (opts->source_log != NULL && opts->port != NULL) {
    die("capture bundle accepts at most one of --source-log or --port");
  }

  resolve_repo_root(config_path, repo, repo_root, sizeof(repo_root));
  join_path(repo_root, "tools/triage_bundle.py", script_path, sizeof(script_path));
  if (!opts->dry_run && !path_exists(script_path)) {
    die("repo `%s` has no triage bundle script: %s", repo_name, script_path);
  }

  argv[argc++] = "python3";
  argv[argc++] = script_path;
  if (opts->source_log != NULL) {
    resolve_path(repo_root, opts->source_log, source_log_path, sizeof(source_log_path));
    argv[argc++] = "--source-log";
    argv[argc++] = source_log_path;
  } else {
    port = (opts->port != NULL) ? opts->port : repo->port;
    if (port == NULL || port[0] == '\0') {
      die("capture bundle requires --port or repo port");
    }
    argv[argc++] = "--port";
    argv[argc++] = (char *)port;
  }

  if (opts->output != NULL) {
    resolve_path(repo_root, opts->output, output_path, sizeof(output_path));
    argv[argc++] = "--output";
    argv[argc++] = output_path;
  } else if (load_current_incident_dir(config_path, incident_dir, sizeof(incident_dir)) == 0) {
    join_path(incident_dir, "bundle.json", output_path, sizeof(output_path));
    argv[argc++] = "--output";
    argv[argc++] = output_path;
  }
  if (opts->json) {
    argv[argc++] = "--json";
  }
  argv[argc] = NULL;
  return run_process(argv, repo_root, opts->dry_run);
}

int cmd_watch(const WatchOptions *opts)
{
  char config_path[PATH_MAX];
  char repo_root[PATH_MAX];
  char script_path[PATH_MAX];
  char bundle_json_path[PATH_MAX];
  char source_log_path[PATH_MAX];
  const char *repo_name;
  const RepoConfig *repo;
  const char *port;
  MkdbgConfig config;
  char *argv[16];
  int argc = 0;

  if (find_config_upward(config_path, sizeof(config_path)) != 0) {
    die("missing %s; run `mkdbg init` first", CONFIG_NAME);
  }
  if (load_config_file(config_path, &config) != 0) {
    die("invalid config: %s", config_path);
  }
  resolve_repo_name(&config, opts->repo, opts->target, &repo_name);
  repo = find_repo_const(&config, repo_name);
  if (repo == NULL) {
    die("repo `%s` not found in %s", repo_name, config_path);
  }
  if (opts->bundle_json != NULL && opts->source_log != NULL) {
    die("watch accepts at most one of --bundle-json or --source-log");
  }
  if ((opts->bundle_json != NULL || opts->source_log != NULL) && opts->port != NULL) {
    die("watch cannot combine --port with --bundle-json or --source-log");
  }

  resolve_repo_root(config_path, repo, repo_root, sizeof(repo_root));
  join_path(repo_root, "tools/bringup_ui.py", script_path, sizeof(script_path));
  if (!opts->dry_run && !path_exists(script_path)) {
    die("repo `%s` has no dashboard script: %s", repo_name, script_path);
  }

  argv[argc++] = "python3";
  argv[argc++] = script_path;
  if (opts->bundle_json != NULL) {
    resolve_path(repo_root, opts->bundle_json, bundle_json_path, sizeof(bundle_json_path));
    argv[argc++] = "--bundle-json";
    argv[argc++] = bundle_json_path;
  } else if (opts->source_log != NULL) {
    resolve_path(repo_root, opts->source_log, source_log_path, sizeof(source_log_path));
    argv[argc++] = "--source-log";
    argv[argc++] = source_log_path;
  } else {
    port = (opts->port != NULL) ? opts->port : repo->port;
    if (port == NULL || port[0] == '\0') {
      die("watch requires --port or repo port");
    }
    argv[argc++] = "--port";
    argv[argc++] = (char *)port;
  }

  if (opts->auto_refresh_s != NULL) {
    argv[argc++] = "--auto-refresh-s";
    argv[argc++] = (char *)opts->auto_refresh_s;
  }
  if (opts->render_once) {
    argv[argc++] = "--render-once";
  }
  if (opts->width != NULL) {
    argv[argc++] = "--width";
    argv[argc++] = (char *)opts->width;
  }
  if (opts->height != NULL) {
    argv[argc++] = "--height";
    argv[argc++] = (char *)opts->height;
  }
  argv[argc] = NULL;
  return run_process(argv, repo_root, opts->dry_run);
}

int cmd_attach(const AttachOptions *opts)
{
  char config_path[PATH_MAX];
  char repo_root[PATH_MAX];
  const char *repo_name;
  const RepoConfig *repo;
  MkdbgConfig config;
  char *shell_argv[4];

  if (find_config_upward(config_path, sizeof(config_path)) != 0) {
    die("missing %s; run `mkdbg init` first", CONFIG_NAME);
  }
  if (load_config_file(config_path, &config) != 0) {
    die("invalid config: %s", config_path);
  }
  resolve_repo_name(&config, opts->repo, opts->target, &repo_name);
  repo = find_repo_const(&config, repo_name);
  if (repo == NULL) {
    die("repo `%s` not found in %s", repo_name, config_path);
  }
  resolve_repo_root(config_path, repo, repo_root, sizeof(repo_root));

  /* wire-host --dump path: triggered by --port flag (direct UART attach) */
  if (opts->port != NULL) {
    if (opts->breakpoint_count > 0U || opts->gdb_command_count > 0U || opts->batch) {
      die("--port (wire dump mode) cannot be combined with --break, --command, or --batch");
    }
    if (opts->dry_run) {
      printf("[dry-run] wire-host --dump --port %s --baud %s\n",
             opts->port, opts->baud ? opts->baud : "115200");
      return 0;
    }
    WireCrashReport report;
    int rc = wire_probe_dump(opts->port, opts->baud, &report);
    if (rc < 0) {
      fprintf(stderr, "mkdbg: failed to run wire-host --dump\n");
      return 1;
    }
    if (report.timeout || report.halt_signal == 0) {
      printf("PROBE: no crash detected (timeout)\n");
      return 1;
    }
    /* Human-readable crash report */
    printf("PROBE: crash detected — %s (signal %d) at %s\n",
           report.regs[15][0] ? report.regs[15] : "unknown PC",
           report.halt_signal,
           report.timestamp);
    printf("  PC  = %s\n", report.regs[15]);
    printf("  LR  = %s\n", report.regs[14]);
    printf("  SP  = %s\n", report.regs[13]);
    if (report.nframes > 0) {
      printf("  Stack frames:");
      for (int i = 0; i < report.nframes; i++)
        printf(" %s", report.stack_frames[i]);
      printf("\n");
    }
    printf("  CFSR = %s (%s)\n",
           report.cfsr[0] ? report.cfsr : "0x00000000",
           report.cfsr_decoded[0] ? report.cfsr_decoded : "no faults");
    return 0;
  }

  if (repo->attach_cmd[0] != '\0') {
    if (opts->breakpoint_count > 0U || opts->gdb_command_count > 0U || opts->batch) {
      die("attach_cmd cannot be combined with --break, --command, or --batch");
    }
    shell_argv[0] = "/bin/sh";
    shell_argv[1] = "-lc";
    shell_argv[2] = (char *)repo->attach_cmd;
    shell_argv[3] = NULL;
    return run_process(shell_argv, repo_root, opts->dry_run);
  }

  fprintf(stderr,
          "mkdbg: attach requires --port to read a crash report over UART\n"
          "       mkdbg attach --port /dev/ttyUSB0\n");
  return 1;
}
