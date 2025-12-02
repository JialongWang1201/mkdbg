#include "mkdbg.h"

static void usage(void)
{
  printf("mkdbg-native %s\n", MKDBG_NATIVE_VERSION);
  printf("usage: mkdbg-native [--version] <init|doctor|repo|target|incident|build|flash|hil|snapshot|watch|attach|probe|serial|git|run|capture> [options]\n");
}

static void init_default_repo_name(char *out, size_t out_size)
{
  char cwd[PATH_MAX];
  if (getcwd(cwd, sizeof(cwd)) != NULL) {
    copy_string(out, out_size, path_basename(cwd));
  } else {
    copy_string(out, out_size, "repo");
  }
}

static int cmd_init(const InitOptions *opts)
{
  char cwd[PATH_MAX];
  char config_path[PATH_MAX];
  char repo_name[MAX_NAME];
  MkdbgConfig config;
  RepoConfig repo;

  if (getcwd(cwd, sizeof(cwd)) == NULL) {
    die("getcwd failed: %s", strerror(errno));
  }
  join_path(cwd, CONFIG_NAME, config_path, sizeof(config_path));
  if (!opts->force && path_exists(config_path)) {
    die("%s already exists; use --force to overwrite", CONFIG_NAME);
  }

  init_default_repo_name(repo_name, sizeof(repo_name));
  if (opts->name != NULL) {
    copy_string(repo_name, sizeof(repo_name), opts->name);
  }

  memset(&config, 0, sizeof(config));
  config.version = 1;
  copy_string(config.default_repo, sizeof(config.default_repo), repo_name);
  config.repo_count = 1U;

  repo_set_defaults(&repo, opts->preset, ".");
  copy_string(repo.name, sizeof(repo.name), repo_name);
  if (opts->port != NULL) {
    copy_string(repo.port, sizeof(repo.port), opts->port);
  }
  config.repos[0] = repo;

  if (save_config_file(config_path, &config) != 0) {
    die("failed to write %s", config_path);
  }

  printf("wrote %s\n", config_path);
  printf("default repo: %s (%s)\n", repo_name, opts->preset);
  return 0;
}

static int cmd_doctor(const DoctorOptions *opts)
{
  char config_path[PATH_MAX];
  MkdbgConfig config;
  const char *repo_name;
  const RepoConfig *repo;
  char repo_root[PATH_MAX];
  char detail[PATH_MAX];
  int failed = 0;
  const char *port;

  if (opts->repo != NULL && opts->target != NULL) {
    die("pass either a repo name or --target, not both");
  }
  if (find_config_upward(config_path, sizeof(config_path)) != 0) {
    die("missing %s; run `mkdbg init` first", CONFIG_NAME);
  }
  if (load_config_file(config_path, &config) != 0) {
    die("invalid config: %s", config_path);
  }

  repo_name = opts->target != NULL ? opts->target : opts->repo;
  if (repo_name == NULL) {
    repo_name = config.default_repo;
  }
  repo = find_repo_const(&config, repo_name);
  if (repo == NULL) {
    die("repo `%s` not found in %s", repo_name, config_path);
  }

  resolve_repo_root(config_path, repo, repo_root, sizeof(repo_root));
  print_check(path_exists(config_path), "config", config_path, &failed);
  print_check(path_exists(repo_root), "root", repo_root, &failed);
  print_check(1, "repo", repo_name, &failed);

  port = (opts->port != NULL) ? opts->port : repo->port;
  if (repo->hil_cmd[0] != '\0' || repo->snapshot_cmd[0] != '\0') {
    print_check(port != NULL && port[0] != '\0', "port", (port != NULL && port[0] != '\0') ? port : "<missing>", &failed);
  }

  if (repo->build_cmd[0] != '\0') {
    char program[PATH_MAX];
    command_program(repo->build_cmd, program, sizeof(program));
    print_check(command_available(repo->build_cmd), "build_cmd", program, &failed);
  }
  if (repo->flash_cmd[0] != '\0') {
    char program[PATH_MAX];
    command_program(repo->flash_cmd, program, sizeof(program));
    print_check(command_available(repo->flash_cmd), "flash_cmd", program, &failed);
  }
  if (repo->hil_cmd[0] != '\0') {
    char program[PATH_MAX];
    command_program(repo->hil_cmd, program, sizeof(program));
    print_check(command_available(repo->hil_cmd), "hil_cmd", program, &failed);
  }
  if (repo->snapshot_cmd[0] != '\0') {
    char program[PATH_MAX];
    command_program(repo->snapshot_cmd, program, sizeof(program));
    print_check(command_available(repo->snapshot_cmd), "snapshot_cmd", program, &failed);
  }
  if (repo->elf_path[0] != '\0') {
    resolve_repo_file(config_path, repo, repo->elf_path, detail, sizeof(detail));
    print_check(path_exists(detail), "elf_path", detail, &failed);
  }
  if (repo->openocd_cfg[0] != '\0') {
    resolve_repo_file(config_path, repo, repo->openocd_cfg, detail, sizeof(detail));
    print_check(path_exists(detail), "openocd_cfg", detail, &failed);
  }
  if (repo->openocd_server_cmd[0] != '\0') {
    char program[PATH_MAX];
    command_program(repo->openocd_server_cmd, program, sizeof(program));
    print_check(command_available(repo->openocd_server_cmd), "openocd", program, &failed);
  } else {
    print_check(search_path("openocd"), "openocd", "openocd", &failed);
  }
  if (repo->gdb[0] != '\0') {
    print_check(command_available(repo->gdb), "gdb", repo->gdb, &failed);
  }

  return failed ? 1 : 0;
}

static void apply_repo_add_overrides(RepoConfig *repo, const RepoAddOptions *opts)
{
  if (opts->port != NULL) copy_string(repo->port, sizeof(repo->port), opts->port);
  if (opts->build_cmd != NULL) copy_string(repo->build_cmd, sizeof(repo->build_cmd), opts->build_cmd);
  if (opts->flash_cmd != NULL) copy_string(repo->flash_cmd, sizeof(repo->flash_cmd), opts->flash_cmd);
  if (opts->hil_cmd != NULL) copy_string(repo->hil_cmd, sizeof(repo->hil_cmd), opts->hil_cmd);
  if (opts->snapshot_cmd != NULL) copy_string(repo->snapshot_cmd, sizeof(repo->snapshot_cmd), opts->snapshot_cmd);
  if (opts->attach_cmd != NULL) copy_string(repo->attach_cmd, sizeof(repo->attach_cmd), opts->attach_cmd);
  if (opts->elf_path != NULL) copy_string(repo->elf_path, sizeof(repo->elf_path), opts->elf_path);
  if (opts->snapshot_output != NULL) copy_string(repo->snapshot_output, sizeof(repo->snapshot_output), opts->snapshot_output);
  if (opts->openocd_cfg != NULL) copy_string(repo->openocd_cfg, sizeof(repo->openocd_cfg), opts->openocd_cfg);
  if (opts->openocd_server_cmd != NULL) copy_string(repo->openocd_server_cmd, sizeof(repo->openocd_server_cmd), opts->openocd_server_cmd);
  if (opts->gdb != NULL) copy_string(repo->gdb, sizeof(repo->gdb), opts->gdb);
  if (opts->gdb_target != NULL) copy_string(repo->gdb_target, sizeof(repo->gdb_target), opts->gdb_target);
}

static int cmd_repo_add(const RepoAddOptions *opts)
{
  char config_path[PATH_MAX];
  char config_dir[PATH_MAX];
  char repo_path[MAX_VALUE];
  MkdbgConfig config;
  RepoConfig *existing;
  RepoConfig repo;

  if (find_config_upward(config_path, sizeof(config_path)) != 0) {
    die("missing %s; run `mkdbg init` first", CONFIG_NAME);
  }
  if (load_config_file(config_path, &config) != 0) {
    die("invalid config: %s", config_path);
  }
  path_dirname(config_path, config_dir, sizeof(config_dir));
  resolve_path(config_dir, opts->path, repo_path, sizeof(repo_path));

  existing = find_repo_mut(&config, opts->name);
  repo_set_defaults(&repo, opts->preset, repo_path);
  copy_string(repo.name, sizeof(repo.name), opts->name);
  if (existing != NULL) {
    repo = *existing;
    copy_string(repo.preset, sizeof(repo.preset), opts->preset);
    copy_string(repo.path, sizeof(repo.path), repo_path);
  }
  apply_repo_add_overrides(&repo, opts);

  if (existing != NULL) {
    *existing = repo;
  } else {
    if (config.repo_count >= MAX_REPOS) {
      die("too many repos in config");
    }
    config.repos[config.repo_count++] = repo;
  }
  if (opts->make_default) {
    copy_string(config.default_repo, sizeof(config.default_repo), opts->name);
  }
  if (save_config_file(config_path, &config) != 0) {
    die("failed to update %s", config_path);
  }
  printf("updated %s\n", config_path);
  return 0;
}

static int cmd_repo_list(void)
{
  char config_path[PATH_MAX];
  MkdbgConfig config;
  size_t i;

  if (find_config_upward(config_path, sizeof(config_path)) != 0) {
    die("missing %s; run `mkdbg init` first", CONFIG_NAME);
  }
  if (load_config_file(config_path, &config) != 0) {
    die("invalid config: %s", config_path);
  }

  for (i = 0; i < config.repo_count; ++i) {
    char root[PATH_MAX];
    char marker = (strcmp(config.repos[i].name, config.default_repo) == 0) ? '*' : ' ';
    resolve_repo_root(config_path, &config.repos[i], root, sizeof(root));
    printf("%c %s\tpreset=%s\tpath=%s\n", marker, config.repos[i].name, config.repos[i].preset, root);
  }
  return 0;
}

static int cmd_repo_use(const NameCommandOptions *opts)
{
  char config_path[PATH_MAX];
  MkdbgConfig config;

  if (find_config_upward(config_path, sizeof(config_path)) != 0) {
    die("missing %s; run `mkdbg init` first", CONFIG_NAME);
  }
  if (load_config_file(config_path, &config) != 0) {
    die("invalid config: %s", config_path);
  }
  if (find_repo_const(&config, opts->name) == NULL) {
    die("repo `%s` not found in %s", opts->name, config_path);
  }
  copy_string(config.default_repo, sizeof(config.default_repo), opts->name);
  if (save_config_file(config_path, &config) != 0) {
    die("failed to update %s", config_path);
  }
  printf("default repo: %s\n", opts->name);
  return 0;
}

static int cmd_capture_bundle(const CaptureBundleOptions *opts)
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

static int cmd_watch(const WatchOptions *opts)
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

static int cmd_attach(const AttachOptions *opts)
{
  char config_path[PATH_MAX];
  char repo_root[PATH_MAX];
  char elf_path[PATH_MAX];
  char openocd_cfg[PATH_MAX];
  char target_command[MAX_VALUE];
  const char *repo_name;
  const RepoConfig *repo;
  const char *gdb_bin;
  const char *gdb_target;
  MkdbgConfig config;
  char *openocd_argv[8];
  char *gdb_argv[2 + 1 + 2 + (2 * MAX_ATTACH_BREAKPOINTS) + (2 * MAX_ATTACH_COMMANDS) + 1];
  char *shell_argv[4];
  int openocd_argc = 0;
  int gdb_argc = 0;
  pid_t server_pid;
  int rc = 1;
  int server_rc = 1;
  size_t i;

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

  if (repo->elf_path[0] == '\0') {
    die("repo `%s` has no `elf_path` configured", repo_name);
  }
  if (repo->openocd_cfg[0] == '\0' && repo->openocd_server_cmd[0] == '\0') {
    die("repo `%s` has no `openocd_cfg` configured", repo_name);
  }

  resolve_repo_file(config_path, repo, repo->elf_path, elf_path, sizeof(elf_path));
  if (!path_exists(elf_path)) {
    die("repo `%s` is missing elf_path: %s", repo_name, elf_path);
  }

  if (repo->openocd_server_cmd[0] != '\0') {
    shell_argv[0] = "/bin/sh";
    shell_argv[1] = "-lc";
    shell_argv[2] = (char *)repo->openocd_server_cmd;
    shell_argv[3] = NULL;
  } else {
    resolve_repo_file(config_path, repo, repo->openocd_cfg, openocd_cfg, sizeof(openocd_cfg));
    if (!path_exists(openocd_cfg)) {
      die("repo `%s` is missing openocd_cfg: %s", repo_name, openocd_cfg);
    }
    openocd_argv[openocd_argc++] = "openocd";
    openocd_argv[openocd_argc++] = "-f";
    openocd_argv[openocd_argc++] = openocd_cfg;
    openocd_argv[openocd_argc++] = "-c";
    openocd_argv[openocd_argc++] = "gdb_port 3333; init; reset halt";
    openocd_argv[openocd_argc] = NULL;
  }

  gdb_bin = repo->gdb[0] != '\0' ? repo->gdb : "arm-none-eabi-gdb";
  gdb_target = repo->gdb_target[0] != '\0' ? repo->gdb_target : "localhost:3333";
  gdb_argv[gdb_argc++] = (char *)gdb_bin;
  gdb_argv[gdb_argc++] = elf_path;
  if (opts->batch) {
    gdb_argv[gdb_argc++] = "-batch";
  }
  copy_string(target_command, sizeof(target_command), "target extended-remote ");
  append_string(target_command, sizeof(target_command), gdb_target);
  gdb_argv[gdb_argc++] = "-ex";
  gdb_argv[gdb_argc++] = target_command;
  for (i = 0U; i < opts->breakpoint_count; ++i) {
    static char breakpoint_commands[MAX_ATTACH_BREAKPOINTS][MAX_VALUE];
    snprintf(breakpoint_commands[i], sizeof(breakpoint_commands[i]), "break %s", opts->breakpoints[i]);
    gdb_argv[gdb_argc++] = "-ex";
    gdb_argv[gdb_argc++] = breakpoint_commands[i];
  }
  for (i = 0U; i < opts->gdb_command_count; ++i) {
    gdb_argv[gdb_argc++] = "-ex";
    gdb_argv[gdb_argc++] = (char *)opts->gdb_commands[i];
  }
  gdb_argv[gdb_argc] = NULL;

  printf("[mkdbg] cwd=%s\n", repo_root);
  if (repo->openocd_server_cmd[0] != '\0') {
    printf("[mkdbg] openocd=%s\n", repo->openocd_server_cmd);
  } else {
    print_command_label("openocd", openocd_argv);
  }
  print_command_label("gdb", gdb_argv);
  if (opts->dry_run) {
    return 0;
  }

  if (repo->openocd_server_cmd[0] != '\0') {
    server_pid = spawn_process(shell_argv, repo_root);
  } else {
    server_pid = spawn_process(openocd_argv, repo_root);
  }

  sleep_seconds(opts->server_wait_s);
  if (try_reap_pid(server_pid, &server_rc)) {
    return server_rc;
  }

  rc = run_process(gdb_argv, repo_root, 0);
  terminate_pid(server_pid);
  return rc;
}

static int cmd_probe_action(const ProbeOptions *opts, const char *command)
{
  char config_path[PATH_MAX];
  char repo_root[PATH_MAX];
  char openocd_cfg[PATH_MAX];
  const char *repo_name;
  const RepoConfig *repo;
  MkdbgConfig config;
  char *argv[6];
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
  if (repo->openocd_cfg[0] == '\0') {
    die("repo `%s` has no `openocd_cfg` configured", repo_name);
  }

  resolve_repo_root(config_path, repo, repo_root, sizeof(repo_root));
  resolve_repo_file(config_path, repo, repo->openocd_cfg, openocd_cfg, sizeof(openocd_cfg));
  if (!opts->dry_run && !path_exists(openocd_cfg)) {
    die("repo `%s` is missing openocd_cfg: %s", repo_name, openocd_cfg);
  }

  argv[argc++] = "openocd";
  argv[argc++] = "-f";
  argv[argc++] = openocd_cfg;
  argv[argc++] = "-c";
  argv[argc++] = (char *)command;
  argv[argc] = NULL;
  return run_process(argv, repo_root, opts->dry_run);
}

static int cmd_probe_flash(const ProbeOptions *opts)
{
  char config_path[PATH_MAX];
  char repo_root[PATH_MAX];
  char openocd_cfg[PATH_MAX];
  char elf_path[PATH_MAX];
  char command[MAX_VALUE];
  const char *repo_name;
  const RepoConfig *repo;
  MkdbgConfig config;
  char *argv[6];
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
  if (repo->openocd_cfg[0] == '\0') {
    die("repo `%s` has no `openocd_cfg` configured", repo_name);
  }
  if (repo->elf_path[0] == '\0') {
    die("repo `%s` has no `elf_path` configured", repo_name);
  }

  resolve_repo_root(config_path, repo, repo_root, sizeof(repo_root));
  resolve_repo_file(config_path, repo, repo->openocd_cfg, openocd_cfg, sizeof(openocd_cfg));
  resolve_repo_file(config_path, repo, repo->elf_path, elf_path, sizeof(elf_path));
  if (!opts->dry_run && !path_exists(openocd_cfg)) {
    die("repo `%s` is missing openocd_cfg: %s", repo_name, openocd_cfg);
  }
  if (!opts->dry_run && !path_exists(elf_path)) {
    die("repo `%s` is missing elf_path: %s", repo_name, elf_path);
  }

  copy_string(command, sizeof(command), "program ");
  append_string(command, sizeof(command), elf_path);
  append_string(command, sizeof(command), " verify reset exit");
  argv[argc++] = "openocd";
  argv[argc++] = "-f";
  argv[argc++] = openocd_cfg;
  argv[argc++] = "-c";
  argv[argc++] = command;
  argv[argc] = NULL;
  return run_process(argv, repo_root, opts->dry_run);
}

static int cmd_probe_read32(const ProbeOptions *opts)
{
  char address[MAX_U32_TEXT];
  char command[MAX_VALUE];

  format_u32_hex(opts->address, "address", address, sizeof(address));
  snprintf(command, sizeof(command), "init; mdw %s; shutdown", address);
  return cmd_probe_action(opts, command);
}

static int cmd_probe_write32(const ProbeOptions *opts)
{
  char address[MAX_U32_TEXT];
  char value[MAX_U32_TEXT];
  char command[MAX_VALUE];

  format_u32_hex(opts->address, "address", address, sizeof(address));
  format_u32_hex(opts->value, "value", value, sizeof(value));
  snprintf(command, sizeof(command), "init; mww %s %s; shutdown", address, value);
  return cmd_probe_action(opts, command);
}

static int cmd_run(const RunOptions *opts)
{
  char config_path[PATH_MAX];
  char repo_root[PATH_MAX];
  const char *repo_name;
  const RepoConfig *repo;
  MkdbgConfig config;
  char *argv[256];
  int i;

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
  if (opts->command_argc <= 0 || opts->command == NULL) {
    die("run requires a command after `--`");
  }
  if (opts->command_argc >= (int)(sizeof(argv) / sizeof(argv[0]))) {
    die("run command too long");
  }

  resolve_repo_root(config_path, repo, repo_root, sizeof(repo_root));
  for (i = 0; i < opts->command_argc; ++i) {
    argv[i] = opts->command[i];
  }
  argv[opts->command_argc] = NULL;
  return run_process(argv, repo_root, opts->dry_run);
}

static void git_resolve_repo_root(const GitOptions *opts,
                                  char *config_path,
                                  size_t config_path_size,
                                  char *repo_root,
                                  size_t repo_root_size)
{
  MkdbgConfig config;
  const char *repo_name;
  const RepoConfig *repo;

  if (find_config_upward(config_path, config_path_size) != 0) {
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
  resolve_repo_root(config_path, repo, repo_root, repo_root_size);
}

static int cmd_git_status(const GitOptions *opts)
{
  char config_path[PATH_MAX];
  char repo_root[PATH_MAX];
  char *argv[] = {(char *)"git", (char *)"status", NULL};

  git_resolve_repo_root(opts, config_path, sizeof(config_path),
                        repo_root, sizeof(repo_root));
  return run_process(argv, repo_root, opts->dry_run);
}

static int cmd_git_rev(const GitOptions *opts)
{
  char config_path[PATH_MAX];
  char repo_root[PATH_MAX];
  char *argv[] = {(char *)"git", (char *)"rev-parse", (char *)"HEAD", NULL};

  git_resolve_repo_root(opts, config_path, sizeof(config_path),
                        repo_root, sizeof(repo_root));
  return run_process(argv, repo_root, opts->dry_run);
}

static int cmd_git_new_branch(const GitOptions *opts)
{
  char config_path[PATH_MAX];
  char repo_root[PATH_MAX];
  char *argv[] = {(char *)"git", (char *)"checkout", (char *)"-b",
                  (char *)opts->branch_name, NULL};

  if (opts->branch_name == NULL || opts->branch_name[0] == '\0') {
    die("git new-branch requires a branch name");
  }
  git_resolve_repo_root(opts, config_path, sizeof(config_path),
                        repo_root, sizeof(repo_root));
  return run_process(argv, repo_root, opts->dry_run);
}

static int cmd_git_worktree(const GitOptions *opts)
{
  char config_path[PATH_MAX];
  char repo_root[PATH_MAX];
  char *argv[] = {(char *)"git", (char *)"worktree", (char *)"add",
                  (char *)opts->path, NULL};

  if (opts->path == NULL || opts->path[0] == '\0') {
    die("git worktree requires a path");
  }
  git_resolve_repo_root(opts, config_path, sizeof(config_path),
                        repo_root, sizeof(repo_root));
  return run_process(argv, repo_root, opts->dry_run);
}

static int cmd_git_push_current(const GitOptions *opts)
{
  char config_path[PATH_MAX];
  char repo_root[PATH_MAX];
  char *argv[] = {(char *)"git", (char *)"push", (char *)"-u",
                  (char *)"origin", (char *)"HEAD", NULL};

  git_resolve_repo_root(opts, config_path, sizeof(config_path),
                        repo_root, sizeof(repo_root));
  return run_process(argv, repo_root, opts->dry_run);
}

static speed_t baud_to_speed(int baud)
{
  switch (baud) {
  case 9600:   return B9600;
  case 19200:  return B19200;
  case 38400:  return B38400;
  case 57600:  return B57600;
  case 115200: return B115200;
  case 230400: return B230400;
  default:     return B0;
  }
}

static const char *resolve_serial_port(const char *config_path,
                                       const MkdbgConfig *config,
                                       const SerialOptions *opts)
{
  const char *port = opts->port;
  if (port != NULL && port[0] != '\0') {
    return port;
  }
  {
    const char *repo_name;
    const RepoConfig *repo;
    resolve_repo_name(config, opts->repo, opts->target, &repo_name);
    repo = find_repo_const(config, repo_name);
    if (repo != NULL && repo->port[0] != '\0') {
      return repo->port;
    }
  }
  (void)config_path;
  die("serial command requires a port; pass --port or set port in config");
  return NULL;
}

static int cmd_serial_tail(const SerialOptions *opts)
{
  char config_path[PATH_MAX];
  MkdbgConfig config;
  const char *port;
  int baud;
  speed_t speed;
  int fd;
  struct termios tty;
  unsigned char buf[SERIAL_BUF_SIZE];
  ssize_t n;

  if (find_config_upward(config_path, sizeof(config_path)) != 0) {
    die("missing %s; run `mkdbg init` first", CONFIG_NAME);
  }
  if (load_config_file(config_path, &config) != 0) {
    die("invalid config: %s", config_path);
  }

  port = resolve_serial_port(config_path, &config, opts);
  baud = opts->baud > 0 ? opts->baud : DEFAULT_BAUD;
  speed = baud_to_speed(baud);
  if (speed == B0) {
    die("unsupported baud rate: %d", baud);
  }

  printf("[mkdbg] serial tail port=%s baud=%d\n", port, baud);
  if (opts->dry_run) {
    return 0;
  }

  fd = open(port, O_RDONLY | O_NOCTTY);
  if (fd < 0) {
    die("cannot open serial port %s: %s", port, strerror(errno));
  }

  memset(&tty, 0, sizeof(tty));
  if (tcgetattr(fd, &tty) != 0) {
    close(fd);
    die("tcgetattr failed: %s", strerror(errno));
  }
  cfmakeraw(&tty);
  cfsetispeed(&tty, speed);
  cfsetospeed(&tty, speed);
  tty.c_cflag |= (CLOCAL | CREAD);
  tty.c_cc[VMIN] = 1;
  tty.c_cc[VTIME] = 0;
  if (tcsetattr(fd, TCSANOW, &tty) != 0) {
    close(fd);
    die("tcsetattr failed: %s", strerror(errno));
  }

  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    if (write(STDOUT_FILENO, buf, (size_t)n) < 0) {
      break;
    }
  }

  close(fd);
  return 0;
}

static int cmd_serial_send(const SerialOptions *opts)
{
  char config_path[PATH_MAX];
  MkdbgConfig config;
  const char *port;
  int baud;
  speed_t speed;
  int fd;
  struct termios tty;
  const char *msg;
  size_t len;
  size_t i;

  if (opts->message == NULL || opts->message[0] == '\0') {
    die("serial send requires a message");
  }

  if (find_config_upward(config_path, sizeof(config_path)) != 0) {
    die("missing %s; run `mkdbg init` first", CONFIG_NAME);
  }
  if (load_config_file(config_path, &config) != 0) {
    die("invalid config: %s", config_path);
  }

  port = resolve_serial_port(config_path, &config, opts);
  baud = opts->baud > 0 ? opts->baud : DEFAULT_BAUD;
  speed = baud_to_speed(baud);
  if (speed == B0) {
    die("unsupported baud rate: %d", baud);
  }

  msg = opts->message;
  len = strlen(msg);

  printf("[mkdbg] serial send port=%s baud=%d len=%zu\n", port, baud, len);
  if (opts->dry_run) {
    return 0;
  }

  fd = open(port, O_WRONLY | O_NOCTTY);
  if (fd < 0) {
    die("cannot open serial port %s: %s", port, strerror(errno));
  }

  memset(&tty, 0, sizeof(tty));
  if (tcgetattr(fd, &tty) != 0) {
    close(fd);
    die("tcgetattr failed: %s", strerror(errno));
  }
  cfmakeraw(&tty);
  cfsetispeed(&tty, speed);
  cfsetospeed(&tty, speed);
  tty.c_cflag |= (CLOCAL | CREAD);
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 10;
  if (tcsetattr(fd, TCSANOW, &tty) != 0) {
    close(fd);
    die("tcsetattr failed: %s", strerror(errno));
  }

  if (opts->char_delay_ms > 0.0) {
    for (i = 0; i < len; ++i) {
      if (write(fd, &msg[i], 1) != 1) {
        close(fd);
        die("serial write failed: %s", strerror(errno));
      }
      usleep((useconds_t)(opts->char_delay_ms * 1000.0));
    }
  } else {
    if (write(fd, msg, len) != (ssize_t)len) {
      close(fd);
      die("serial write failed: %s", strerror(errno));
    }
  }

  close(fd);
  printf("[mkdbg] sent %zu bytes\n", len);
  return 0;
}

static void build_action_context(const char *config_path,
                                 const char *repo_name,
                                 const RepoConfig *repo,
                                 const char *port,
                                 char *repo_root,
                                 size_t repo_root_size,
                                 char *elf_path,
                                 size_t elf_path_size,
                                 char *openocd_cfg,
                                 size_t openocd_cfg_size,
                                 char *snapshot_output,
                                 size_t snapshot_output_size,
                                 char *gdb_target,
                                 size_t gdb_target_size)
{
  resolve_repo_root(config_path, repo, repo_root, repo_root_size);
  if (repo->elf_path[0] != '\0') {
    resolve_repo_file(config_path, repo, repo->elf_path, elf_path, elf_path_size);
  } else {
    elf_path[0] = '\0';
  }
  if (repo->openocd_cfg[0] != '\0') {
    resolve_repo_file(config_path, repo, repo->openocd_cfg, openocd_cfg, openocd_cfg_size);
  } else {
    openocd_cfg[0] = '\0';
  }
  if (repo->snapshot_output[0] != '\0') {
    resolve_repo_file(config_path, repo, repo->snapshot_output, snapshot_output, snapshot_output_size);
  } else {
    resolve_repo_file(config_path, repo, "build/mkdbg.bundle.json", snapshot_output, snapshot_output_size);
  }
  copy_string(gdb_target, gdb_target_size, repo->gdb_target[0] != '\0' ? repo->gdb_target : "localhost:3333");
  (void)repo_name;
  (void)port;
}

static void format_action_command(const char *template,
                                  const char *repo_name,
                                  const char *repo_root,
                                  const char *port,
                                  const char *elf_path,
                                  const char *openocd_cfg,
                                  const char *snapshot_output,
                                  const char *gdb_target,
                                  char *out,
                                  size_t out_size)
{
  char stage0[4096];
  char stage1[4096];
  char stage2[4096];
  char stage3[4096];
  char stage4[4096];
  char stage5[4096];
  char stage6[4096];

  replace_all(stage0, sizeof(stage0), template, "{repo}", repo_name);
  replace_all(stage1, sizeof(stage1), stage0, "{repo_root}", repo_root);
  replace_all(stage2, sizeof(stage2), stage1, "{port}", port != NULL ? port : "");
  replace_all(stage3, sizeof(stage3), stage2, "{elf_path}", elf_path);
  replace_all(stage4, sizeof(stage4), stage3, "{openocd_cfg}", openocd_cfg);
  replace_all(stage5, sizeof(stage5), stage4, "{snapshot_output}", snapshot_output);
  replace_all(stage6, sizeof(stage6), stage5, "{gdb_target}", gdb_target);
  copy_string(out, out_size, stage6);
}

static int cmd_configured_action(const ActionOptions *opts, const char *field, int needs_port)
{
  char config_path[PATH_MAX];
  char repo_root[PATH_MAX];
  char elf_path[PATH_MAX];
  char openocd_cfg[PATH_MAX];
  char snapshot_output[PATH_MAX];
  char gdb_target[MAX_VALUE];
  char command[4096];
  const char *repo_name;
  const RepoConfig *repo;
  const char *template = NULL;
  const char *port = NULL;
  MkdbgConfig config;
  char *argv[4];

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

  if (strcmp(field, "build_cmd") == 0) template = repo->build_cmd;
  else if (strcmp(field, "flash_cmd") == 0) template = repo->flash_cmd;
  else if (strcmp(field, "hil_cmd") == 0) template = repo->hil_cmd;
  else if (strcmp(field, "snapshot_cmd") == 0) template = repo->snapshot_cmd;
  if (template == NULL || template[0] == '\0') {
    die("repo `%s` has no `%s` configured", repo_name, field);
  }

  port = opts->port != NULL ? opts->port : repo->port;
  if (needs_port && (port == NULL || port[0] == '\0')) {
    die("this command requires a serial port; pass `--port` or set `port` in config");
  }
  build_action_context(config_path,
                       repo_name,
                       repo,
                       port,
                       repo_root,
                       sizeof(repo_root),
                       elf_path,
                       sizeof(elf_path),
                       openocd_cfg,
                       sizeof(openocd_cfg),
                       snapshot_output,
                       sizeof(snapshot_output),
                       gdb_target,
                       sizeof(gdb_target));
  format_action_command(template,
                        repo_name,
                        repo_root,
                        port != NULL ? port : "",
                        elf_path,
                        openocd_cfg,
                        snapshot_output,
                        gdb_target,
                        command,
                        sizeof(command));

  argv[0] = "/bin/sh";
  argv[1] = "-lc";
  argv[2] = command;
  argv[3] = NULL;
  return run_process(argv, repo_root, opts->dry_run);
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

  die("unknown command: %s", argv[1]);
  return 2;
}
