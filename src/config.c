#include "mkdbg.h"

const RepoConfig *find_repo_const(const MkdbgConfig *config, const char *name)
{
  size_t i;
  for (i = 0; i < config->repo_count; ++i) {
    if (strcmp(config->repos[i].name, name) == 0) {
      return &config->repos[i];
    }
  }
  return NULL;
}

RepoConfig *find_repo_mut(MkdbgConfig *config, const char *name)
{
  size_t i;
  for (i = 0; i < config->repo_count; ++i) {
    if (strcmp(config->repos[i].name, name) == 0) {
      return &config->repos[i];
    }
  }
  return NULL;
}

void repo_set_defaults(RepoConfig *repo, const char *preset, const char *repo_path)
{
  memset(repo, 0, sizeof(*repo));
  copy_string(repo->preset, sizeof(repo->preset), preset);
  copy_string(repo->path, sizeof(repo->path), repo_path);
  if (strcmp(preset, "microkernel-mpu") == 0) {
    copy_string(repo->build_cmd, sizeof(repo->build_cmd), "bash tools/build.sh");
    copy_string(repo->flash_cmd, sizeof(repo->flash_cmd), "bash tools/flash.sh");
    copy_string(repo->hil_cmd, sizeof(repo->hil_cmd), "bash tools/hil_gate.sh --port {port}");
    copy_string(repo->snapshot_cmd, sizeof(repo->snapshot_cmd),
                "python3 tools/triage_bundle.py --port {port} --output {snapshot_output}");
    copy_string(repo->elf_path, sizeof(repo->elf_path), "build/MicroKernel_MPU.elf");
    copy_string(repo->snapshot_output, sizeof(repo->snapshot_output), "build/mkdbg.bundle.json");
    copy_string(repo->openocd_cfg, sizeof(repo->openocd_cfg), "tools/openocd.cfg");
    copy_string(repo->gdb, sizeof(repo->gdb), "arm-none-eabi-gdb");
    copy_string(repo->gdb_target, sizeof(repo->gdb_target), "localhost:3333");
  } else {
    copy_string(repo->snapshot_output, sizeof(repo->snapshot_output), "build/mkdbg.bundle.json");
    copy_string(repo->gdb, sizeof(repo->gdb), "gdb");
    copy_string(repo->gdb_target, sizeof(repo->gdb_target), "localhost:3333");
  }
}

void write_config_value(FILE *f, const char *key, const char *value)
{
  if (value[0] != '\0') {
    fprintf(f, "%s = \"%s\"\n", key, value);
  }
}

void render_repo(FILE *f, const RepoConfig *repo)
{
  fprintf(f, "[repos.\"%s\"]\n", repo->name);
  write_config_value(f, "preset", repo->preset);
  write_config_value(f, "path", repo->path);
  write_config_value(f, "port", repo->port);
  write_config_value(f, "build_cmd", repo->build_cmd);
  write_config_value(f, "flash_cmd", repo->flash_cmd);
  write_config_value(f, "hil_cmd", repo->hil_cmd);
  write_config_value(f, "snapshot_cmd", repo->snapshot_cmd);
  write_config_value(f, "attach_cmd", repo->attach_cmd);
  write_config_value(f, "elf_path", repo->elf_path);
  write_config_value(f, "snapshot_output", repo->snapshot_output);
  write_config_value(f, "openocd_cfg", repo->openocd_cfg);
  write_config_value(f, "openocd_server_cmd", repo->openocd_server_cmd);
  write_config_value(f, "gdb", repo->gdb);
  write_config_value(f, "gdb_target", repo->gdb_target);
  fputc('\n', f);
}

int save_config_file(const char *config_path, const MkdbgConfig *config)
{
  FILE *f = fopen(config_path, "w");
  size_t i;

  if (f == NULL) {
    return -1;
  }
  fprintf(f, "version = %d\n", config->version);
  fprintf(f, "default_repo = \"%s\"\n\n", config->default_repo);
  for (i = 0; i < config->repo_count; ++i) {
    render_repo(f, &config->repos[i]);
  }
  if (fclose(f) != 0) {
    return -1;
  }
  return 0;
}

int parse_quoted_value(const char *value, char *out, size_t out_size)
{
  size_t len;
  if (value[0] != '"') {
    return -1;
  }
  len = strlen(value);
  if (len < 2U || value[len - 1U] != '"') {
    return -1;
  }
  if (len - 1U >= out_size) {
    return -1;
  }
  memmove(out, value + 1, len - 2U);
  out[len - 2U] = '\0';
  return 0;
}

void repo_assign_key(RepoConfig *repo, const char *key, const char *value)
{
  if (strcmp(key, "preset") == 0) {
    copy_string(repo->preset, sizeof(repo->preset), value);
  } else if (strcmp(key, "path") == 0) {
    copy_string(repo->path, sizeof(repo->path), value);
  } else if (strcmp(key, "port") == 0) {
    copy_string(repo->port, sizeof(repo->port), value);
  } else if (strcmp(key, "build_cmd") == 0) {
    copy_string(repo->build_cmd, sizeof(repo->build_cmd), value);
  } else if (strcmp(key, "flash_cmd") == 0) {
    copy_string(repo->flash_cmd, sizeof(repo->flash_cmd), value);
  } else if (strcmp(key, "hil_cmd") == 0) {
    copy_string(repo->hil_cmd, sizeof(repo->hil_cmd), value);
  } else if (strcmp(key, "snapshot_cmd") == 0) {
    copy_string(repo->snapshot_cmd, sizeof(repo->snapshot_cmd), value);
  } else if (strcmp(key, "attach_cmd") == 0) {
    copy_string(repo->attach_cmd, sizeof(repo->attach_cmd), value);
  } else if (strcmp(key, "elf_path") == 0) {
    copy_string(repo->elf_path, sizeof(repo->elf_path), value);
  } else if (strcmp(key, "snapshot_output") == 0) {
    copy_string(repo->snapshot_output, sizeof(repo->snapshot_output), value);
  } else if (strcmp(key, "openocd_cfg") == 0) {
    copy_string(repo->openocd_cfg, sizeof(repo->openocd_cfg), value);
  } else if (strcmp(key, "openocd_server_cmd") == 0) {
    copy_string(repo->openocd_server_cmd, sizeof(repo->openocd_server_cmd), value);
  } else if (strcmp(key, "gdb") == 0) {
    copy_string(repo->gdb, sizeof(repo->gdb), value);
  } else if (strcmp(key, "gdb_target") == 0) {
    copy_string(repo->gdb_target, sizeof(repo->gdb_target), value);
  }
}

int load_config_file(const char *config_path, MkdbgConfig *config)
{
  FILE *f = fopen(config_path, "r");
  char line[2048];
  RepoConfig *current_repo = NULL;

  memset(config, 0, sizeof(*config));
  config->version = 1;
  if (f == NULL) {
    return -1;
  }

  while (fgets(line, sizeof(line), f) != NULL) {
    char *eq;
    char key[256];
    char value[MAX_VALUE];
    char repo_name[MAX_NAME];

    trim_in_place(line);
    if (line[0] == '\0' || line[0] == '#') {
      continue;
    }
    if (strncmp(line, "[repos.\"", 8) == 0) {
      const char *start = line + 8;
      const char *end = strstr(start, "\"]");
      if (end == NULL) {
        fclose(f);
        return -1;
      }
      if (config->repo_count >= MAX_REPOS) {
        fclose(f);
        return -1;
      }
      memset(repo_name, 0, sizeof(repo_name));
      snprintf(repo_name, sizeof(repo_name), "%.*s", (int)(end - start), start);
      current_repo = &config->repos[config->repo_count++];
      memset(current_repo, 0, sizeof(*current_repo));
      copy_string(current_repo->name, sizeof(current_repo->name), repo_name);
      continue;
    }

    eq = strchr(line, '=');
    if (eq == NULL) {
      fclose(f);
      return -1;
    }
    *eq = '\0';
    copy_string(key, sizeof(key), line);
    trim_in_place(key);
    copy_string(value, sizeof(value), eq + 1);
    trim_in_place(value);

    if (strcmp(key, "version") == 0) {
      config->version = atoi(value);
      continue;
    }

    if (parse_quoted_value(value, value, sizeof(value)) != 0) {
      fclose(f);
      return -1;
    }

    if (strcmp(key, "default_repo") == 0) {
      copy_string(config->default_repo, sizeof(config->default_repo), value);
    } else if (current_repo != NULL) {
      repo_assign_key(current_repo, key, value);
    }
  }

  fclose(f);
  if (config->default_repo[0] == '\0' || config->repo_count == 0U) {
    return -1;
  }
  return 0;
}

int find_config_upward(char *out, size_t out_size)
{
  char current[PATH_MAX];

  if (getcwd(current, sizeof(current)) == NULL) {
    return -1;
  }

  for (;;) {
    char candidate[PATH_MAX];
    join_path(current, CONFIG_NAME, candidate, sizeof(candidate));
    if (path_exists(candidate)) {
      copy_string(out, out_size, candidate);
      return 0;
    }
    if (strcmp(current, "/") == 0) {
      break;
    }
    path_dirname(current, current, sizeof(current));
  }
  return -1;
}

void resolve_repo_root(const char *config_path, const RepoConfig *repo, char *out, size_t out_size)
{
  char config_dir[PATH_MAX];
  const char *repo_path = repo->path[0] != '\0' ? repo->path : ".";
  path_dirname(config_path, config_dir, sizeof(config_dir));
  resolve_path(config_dir, repo_path, out, out_size);
}

void resolve_repo_file(const char *config_path,
                       const RepoConfig *repo,
                       const char *raw,
                       char *out,
                       size_t out_size)
{
  char root[PATH_MAX];
  resolve_repo_root(config_path, repo, root, sizeof(root));
  resolve_path(root, raw, out, out_size);
}

void state_root_from_config(const char *config_path, char *out, size_t out_size)
{
  char config_dir[PATH_MAX];
  path_dirname(config_path, config_dir, sizeof(config_dir));
  join_path(config_dir, STATE_DIR_NAME, out, out_size);
}

void incidents_root_from_config(const char *config_path, char *out, size_t out_size)
{
  char state_root[PATH_MAX];
  state_root_from_config(config_path, state_root, sizeof(state_root));
  join_path(state_root, INCIDENTS_DIR_NAME, out, out_size);
}

void current_incident_path_from_config(const char *config_path, char *out, size_t out_size)
{
  char incidents_root[PATH_MAX];
  incidents_root_from_config(config_path, incidents_root, sizeof(incidents_root));
  join_path(incidents_root, INCIDENT_CURRENT_NAME, out, out_size);
}

void incident_meta_path(const char *incident_dir, char *out, size_t out_size)
{
  join_path(incident_dir, INCIDENT_META_NAME, out, out_size);
}

int resolve_repo_name(const MkdbgConfig *config, const char *repo, const char *target, const char **out_name)
{
  if (repo != NULL && target != NULL) {
    die("pass either a repo name or --target, not both");
  }
  if (repo != NULL) {
    *out_name = repo;
  } else if (target != NULL) {
    *out_name = target;
  } else if (config->default_repo[0] != '\0') {
    *out_name = config->default_repo;
  } else {
    die("config has no default repo");
  }
  return 0;
}
