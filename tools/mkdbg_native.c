#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MKDBG_NATIVE_VERSION "0.1.0"
#define CONFIG_NAME ".mkdbg.toml"
#define MAX_REPOS 16
#define MAX_NAME 128
#define MAX_VALUE 1024

typedef struct {
  char name[MAX_NAME];
  char preset[MAX_NAME];
  char path[MAX_VALUE];
  char port[MAX_VALUE];
  char build_cmd[MAX_VALUE];
  char flash_cmd[MAX_VALUE];
  char hil_cmd[MAX_VALUE];
  char snapshot_cmd[MAX_VALUE];
  char attach_cmd[MAX_VALUE];
  char elf_path[MAX_VALUE];
  char snapshot_output[MAX_VALUE];
  char openocd_cfg[MAX_VALUE];
  char openocd_server_cmd[MAX_VALUE];
  char gdb[MAX_VALUE];
  char gdb_target[MAX_VALUE];
} RepoConfig;

typedef struct {
  int version;
  char default_repo[MAX_NAME];
  RepoConfig repos[MAX_REPOS];
  size_t repo_count;
} MkdbgConfig;

typedef struct {
  const char *preset;
  const char *name;
  const char *port;
  int force;
} InitOptions;

typedef struct {
  const char *repo;
  const char *target;
  const char *port;
} DoctorOptions;

static void die(const char *fmt, ...)
{
  va_list ap;
  fprintf(stderr, "error: ");
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(2);
}

static void trim_in_place(char *s)
{
  char *start = s;
  char *end;
  size_t len;

  while (*start != '\0' && isspace((unsigned char)*start)) {
    start++;
  }
  if (start != s) {
    memmove(s, start, strlen(start) + 1U);
  }
  len = strlen(s);
  while (len > 0U && isspace((unsigned char)s[len - 1U])) {
    s[len - 1U] = '\0';
    len--;
  }
  end = s + len;
  (void)end;
}

static void copy_string(char *dst, size_t dst_size, const char *src)
{
  if (dst_size == 0U) {
    return;
  }
  snprintf(dst, dst_size, "%s", src != NULL ? src : "");
}

static const char *path_basename(const char *path)
{
  const char *slash = strrchr(path, '/');
  if (slash == NULL || slash[1] == '\0') {
    return path;
  }
  return slash + 1;
}

static void path_dirname(const char *path, char *out, size_t out_size)
{
  const char *slash = strrchr(path, '/');
  size_t len;

  if (slash == NULL) {
    copy_string(out, out_size, ".");
    return;
  }
  if (slash == path) {
    copy_string(out, out_size, "/");
    return;
  }
  len = (size_t)(slash - path);
  if (len >= out_size) {
    len = out_size - 1U;
  }
  memcpy(out, path, len);
  out[len] = '\0';
}

static void join_path(const char *a, const char *b, char *out, size_t out_size)
{
  if (b == NULL || b[0] == '\0') {
    copy_string(out, out_size, a);
    return;
  }
  if (b[0] == '/') {
    copy_string(out, out_size, b);
    return;
  }
  if (strcmp(a, "/") == 0) {
    snprintf(out, out_size, "/%s", b);
    return;
  }
  snprintf(out, out_size, "%s/%s", a, b);
}

static void resolve_path(const char *base, const char *raw, char *out, size_t out_size)
{
  char combined[PATH_MAX];
  char resolved[PATH_MAX];

  join_path(base, raw, combined, sizeof(combined));
  if (realpath(combined, resolved) != NULL) {
    copy_string(out, out_size, resolved);
  } else {
    copy_string(out, out_size, combined);
  }
}

static int path_exists(const char *path)
{
  return access(path, F_OK) == 0;
}

static int path_executable(const char *path)
{
  return access(path, X_OK) == 0;
}

static void print_check(int ok, const char *label, const char *detail, int *failed)
{
  printf("[mkdbg] %-7s %s: %s\n", ok ? "ok" : "missing", label, detail);
  if (!ok) {
    *failed = 1;
  }
}

static const RepoConfig *find_repo_const(const MkdbgConfig *config, const char *name)
{
  size_t i;
  for (i = 0; i < config->repo_count; ++i) {
    if (strcmp(config->repos[i].name, name) == 0) {
      return &config->repos[i];
    }
  }
  return NULL;
}

static void repo_set_defaults(RepoConfig *repo, const char *preset, const char *repo_path)
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

static void write_config_value(FILE *f, const char *key, const char *value)
{
  if (value[0] != '\0') {
    fprintf(f, "%s = \"%s\"\n", key, value);
  }
}

static void render_repo(FILE *f, const RepoConfig *repo)
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

static int save_config_file(const char *config_path, const MkdbgConfig *config)
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

static int parse_quoted_value(const char *value, char *out, size_t out_size)
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

static void repo_assign_key(RepoConfig *repo, const char *key, const char *value)
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

static int load_config_file(const char *config_path, MkdbgConfig *config)
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

static int find_config_upward(char *out, size_t out_size)
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

static void resolve_repo_root(const char *config_path, const RepoConfig *repo, char *out, size_t out_size)
{
  char config_dir[PATH_MAX];
  const char *repo_path = repo->path[0] != '\0' ? repo->path : ".";
  path_dirname(config_path, config_dir, sizeof(config_dir));
  resolve_path(config_dir, repo_path, out, out_size);
}

static void resolve_repo_file(const char *config_path,
                              const RepoConfig *repo,
                              const char *raw,
                              char *out,
                              size_t out_size)
{
  char root[PATH_MAX];
  resolve_repo_root(config_path, repo, root, sizeof(root));
  resolve_path(root, raw, out, out_size);
}

static int command_program(const char *command, char *out, size_t out_size)
{
  size_t i = 0U;
  size_t j = 0U;
  int in_quote = 0;

  while (command[i] != '\0' && isspace((unsigned char)command[i])) {
    i++;
  }
  if (command[i] == '\0') {
    out[0] = '\0';
    return -1;
  }

  if (command[i] == '"' || command[i] == '\'') {
    in_quote = command[i];
    i++;
  }

  while (command[i] != '\0') {
    if (in_quote != 0) {
      if (command[i] == in_quote) {
        break;
      }
    } else if (isspace((unsigned char)command[i])) {
      break;
    }

    if (j + 1U < out_size) {
      out[j++] = command[i];
    }
    i++;
  }

  out[j] = '\0';
  return (j == 0U) ? -1 : 0;
}

static int search_path(const char *program)
{
  const char *env = getenv("PATH");
  char *dup;
  char *token;
  char *saveptr = NULL;

  if (program == NULL || program[0] == '\0') {
    return 0;
  }
  if (strchr(program, '/') != NULL) {
    return path_executable(program);
  }
  if (env == NULL) {
    return 0;
  }

  dup = strdup(env);
  if (dup == NULL) {
    return 0;
  }
  token = strtok_r(dup, ":", &saveptr);
  while (token != NULL) {
    char candidate[PATH_MAX];
    join_path(token, program, candidate, sizeof(candidate));
    if (path_executable(candidate)) {
      free(dup);
      return 1;
    }
    token = strtok_r(NULL, ":", &saveptr);
  }
  free(dup);
  return 0;
}

static int command_available(const char *command)
{
  char program[PATH_MAX];
  if (command_program(command, program, sizeof(program)) != 0) {
    return 0;
  }
  return search_path(program);
}

static void usage(void)
{
  printf("mkdbg-native %s\n", MKDBG_NATIVE_VERSION);
  printf("usage: mkdbg-native [--version] <init|doctor> [options]\n");
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

static int parse_init_args(int argc, char **argv, InitOptions *opts)
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

static int parse_doctor_args(int argc, char **argv, DoctorOptions *opts)
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

  die("unknown command: %s", argv[1]);
  return 2;
}
