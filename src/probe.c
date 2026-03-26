#include "mkdbg.h"

int cmd_probe_action(const ProbeOptions *opts, const char *command)
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

int cmd_probe_flash(const ProbeOptions *opts)
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

int cmd_probe_read32(const ProbeOptions *opts)
{
  char address[MAX_U32_TEXT];
  char command[MAX_VALUE];

  format_u32_hex(opts->address, "address", address, sizeof(address));
  snprintf(command, sizeof(command), "init; mdw %s; shutdown", address);
  return cmd_probe_action(opts, command);
}

int cmd_probe_write32(const ProbeOptions *opts)
{
  char address[MAX_U32_TEXT];
  char value[MAX_U32_TEXT];
  char command[MAX_VALUE];

  format_u32_hex(opts->address, "address", address, sizeof(address));
  format_u32_hex(opts->value, "value", value, sizeof(value));
  snprintf(command, sizeof(command), "init; mww %s %s; shutdown", address, value);
  return cmd_probe_action(opts, command);
}
