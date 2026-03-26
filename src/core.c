#include "mkdbg.h"

void init_default_repo_name(char *out, size_t out_size)
{
  char cwd[PATH_MAX];
  if (getcwd(cwd, sizeof(cwd)) != NULL) {
    copy_string(out, out_size, path_basename(cwd));
  } else {
    copy_string(out, out_size, "repo");
  }
}

int cmd_init(const InitOptions *opts)
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

int cmd_doctor(const DoctorOptions *opts)
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

void apply_repo_add_overrides(RepoConfig *repo, const RepoAddOptions *opts)
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

int cmd_repo_add(const RepoAddOptions *opts)
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

int cmd_repo_list(void)
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

int cmd_repo_use(const NameCommandOptions *opts)
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
