#include "mkdbg.h"

void build_action_context(const char *config_path,
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

void format_action_command(const char *template,
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

int cmd_run(const RunOptions *opts)
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

int cmd_configured_action(const ActionOptions *opts, const char *field, int needs_port)
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
