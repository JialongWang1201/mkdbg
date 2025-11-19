#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MKDBG_NATIVE_VERSION "0.1.0"
#define CONFIG_NAME ".mkdbg.toml"
#define STATE_DIR_NAME ".mkdbg"
#define INCIDENTS_DIR_NAME "incidents"
#define INCIDENT_CURRENT_NAME "current"
#define INCIDENT_META_NAME "incident.json"
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

typedef struct {
  const char *name;
  const char *path;
  const char *preset;
  const char *port;
  const char *build_cmd;
  const char *flash_cmd;
  const char *hil_cmd;
  const char *snapshot_cmd;
  const char *attach_cmd;
  const char *elf_path;
  const char *snapshot_output;
  const char *openocd_cfg;
  const char *openocd_server_cmd;
  const char *gdb;
  const char *gdb_target;
  int make_default;
} RepoAddOptions;

typedef struct {
  const char *name;
} NameCommandOptions;

typedef struct {
  const char *repo;
  const char *target;
  const char *name;
  const char *port;
} IncidentOpenOptions;

typedef struct {
  int json;
} IncidentStatusOptions;

typedef struct {
  const char *repo;
  const char *target;
  const char *port;
  const char *source_log;
  const char *output;
  int json;
  int dry_run;
} CaptureBundleOptions;

typedef struct {
  char id[MAX_NAME];
  char name[MAX_NAME];
  char status[MAX_NAME];
  char repo[MAX_NAME];
  char port[MAX_VALUE];
  long opened_at;
} IncidentMetadata;

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

static int ensure_dir(const char *path)
{
  if (mkdir(path, 0777) == 0 || errno == EEXIST) {
    return 0;
  }
  return -1;
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

static RepoConfig *find_repo_mut(MkdbgConfig *config, const char *name)
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

static void state_root_from_config(const char *config_path, char *out, size_t out_size)
{
  char config_dir[PATH_MAX];
  path_dirname(config_path, config_dir, sizeof(config_dir));
  join_path(config_dir, STATE_DIR_NAME, out, out_size);
}

static void incidents_root_from_config(const char *config_path, char *out, size_t out_size)
{
  char state_root[PATH_MAX];
  state_root_from_config(config_path, state_root, sizeof(state_root));
  join_path(state_root, INCIDENTS_DIR_NAME, out, out_size);
}

static void current_incident_path_from_config(const char *config_path, char *out, size_t out_size)
{
  char incidents_root[PATH_MAX];
  incidents_root_from_config(config_path, incidents_root, sizeof(incidents_root));
  join_path(incidents_root, INCIDENT_CURRENT_NAME, out, out_size);
}

static void incident_meta_path(const char *incident_dir, char *out, size_t out_size)
{
  join_path(incident_dir, INCIDENT_META_NAME, out, out_size);
}

static void sanitize_slug(const char *input, char *out, size_t out_size)
{
  size_t j = 0U;
  size_t i;
  int prev_dash = 0;

  for (i = 0; input != NULL && input[i] != '\0'; ++i) {
    unsigned char ch = (unsigned char)input[i];
    if (isalnum(ch)) {
      if (j + 1U < out_size) {
        out[j++] = (char)tolower(ch);
      }
      prev_dash = 0;
    } else if ((ch == '-' || ch == '_' || isspace(ch)) && !prev_dash) {
      if (j + 1U < out_size) {
        out[j++] = '-';
      }
      prev_dash = 1;
    }
  }

  while (j > 0U && out[j - 1U] == '-') {
    j--;
  }
  if (j == 0U && out_size > 1U) {
    copy_string(out, out_size, "incident");
    return;
  }
  out[j] = '\0';
}

static int load_current_incident_id(const char *config_path, char *out, size_t out_size)
{
  char current_path[PATH_MAX];
  FILE *f;

  current_incident_path_from_config(config_path, current_path, sizeof(current_path));
  if (!path_exists(current_path)) {
    return -1;
  }
  f = fopen(current_path, "r");
  if (f == NULL) {
    return -1;
  }
  if (fgets(out, (int)out_size, f) == NULL) {
    fclose(f);
    return -1;
  }
  fclose(f);
  trim_in_place(out);
  return (out[0] == '\0') ? -1 : 0;
}

static int load_incident_metadata(const char *meta_path, IncidentMetadata *meta)
{
  FILE *f;
  char line[512];

  memset(meta, 0, sizeof(*meta));
  f = fopen(meta_path, "r");
  if (f == NULL) {
    return -1;
  }
  while (fgets(line, sizeof(line), f) != NULL) {
    char *colon = strchr(line, ':');
    char *value;
    size_t len;
    if (colon == NULL) {
      continue;
    }
    *colon = '\0';
    trim_in_place(line);
    value = colon + 1;
    trim_in_place(value);
    len = strlen(value);
    if (len > 0U && value[len - 1U] == ',') {
      value[len - 1U] = '\0';
      trim_in_place(value);
    }
    if (strcmp(line, "\"id\"") == 0) parse_quoted_value(value, meta->id, sizeof(meta->id));
    else if (strcmp(line, "\"name\"") == 0) parse_quoted_value(value, meta->name, sizeof(meta->name));
    else if (strcmp(line, "\"status\"") == 0) parse_quoted_value(value, meta->status, sizeof(meta->status));
    else if (strcmp(line, "\"repo\"") == 0) parse_quoted_value(value, meta->repo, sizeof(meta->repo));
    else if (strcmp(line, "\"port\"") == 0) parse_quoted_value(value, meta->port, sizeof(meta->port));
    else if (strcmp(line, "\"opened_at\"") == 0) meta->opened_at = atol(value);
  }
  fclose(f);
  return 0;
}

static int write_incident_metadata(const char *meta_path, const IncidentMetadata *meta, long closed_at)
{
  FILE *f = fopen(meta_path, "w");
  if (f == NULL) {
    return -1;
  }
  fprintf(f,
          "{\n"
          "  \"id\": \"%s\",\n"
          "  \"name\": \"%s\",\n"
          "  \"status\": \"%s\",\n"
          "  \"repo\": \"%s\",\n"
          "  \"port\": \"%s\",\n"
          "  \"opened_at\": %ld",
          meta->id, meta->name, meta->status, meta->repo, meta->port, meta->opened_at);
  if (closed_at > 0L) {
    fprintf(f, ",\n  \"closed_at\": %ld", closed_at);
  }
  fprintf(f, "\n}\n");
  fclose(f);
  return 0;
}

static int load_current_incident_dir(const char *config_path, char *out, size_t out_size)
{
  char incident_id[MAX_NAME];
  char incidents_root[PATH_MAX];

  if (load_current_incident_id(config_path, incident_id, sizeof(incident_id)) != 0) {
    return -1;
  }
  incidents_root_from_config(config_path, incidents_root, sizeof(incidents_root));
  join_path(incidents_root, incident_id, out, out_size);
  return 0;
}

static void print_shell_arg(FILE *f, const char *arg)
{
  size_t i;
  int needs_quote = 0;

  for (i = 0; arg[i] != '\0'; ++i) {
    unsigned char ch = (unsigned char)arg[i];
    if (!(isalnum(ch) || ch == '/' || ch == '.' || ch == '_' || ch == '-' || ch == ':' || ch == '=')) {
      needs_quote = 1;
      break;
    }
  }
  if (!needs_quote && arg[0] != '\0') {
    fputs(arg, f);
    return;
  }
  fputc('\'', f);
  for (i = 0; arg[i] != '\0'; ++i) {
    if (arg[i] == '\'') {
      fputs("'\\''", f);
    } else {
      fputc(arg[i], f);
    }
  }
  fputc('\'', f);
}

static int run_process(char *const argv[], const char *cwd, int dry_run)
{
  pid_t pid;
  int status;
  size_t i;

  printf("[mkdbg] cwd=%s\n", cwd);
  printf("[mkdbg] cmd=");
  for (i = 0U; argv[i] != NULL; ++i) {
    if (i != 0U) {
      fputc(' ', stdout);
    }
    print_shell_arg(stdout, argv[i]);
  }
  fputc('\n', stdout);
  if (dry_run) {
    return 0;
  }

  pid = fork();
  if (pid < 0) {
    die("fork failed: %s", strerror(errno));
  }
  if (pid == 0) {
    if (chdir(cwd) != 0) {
      fprintf(stderr, "error: chdir failed: %s\n", strerror(errno));
      _exit(127);
    }
    execvp(argv[0], argv);
    fprintf(stderr, "error: exec failed for %s: %s\n", argv[0], strerror(errno));
    _exit(127);
  }
  if (waitpid(pid, &status, 0) < 0) {
    die("waitpid failed: %s", strerror(errno));
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return 1;
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
  const char *segment = env;

  if (program == NULL || program[0] == '\0') {
    return 0;
  }
  if (strchr(program, '/') != NULL) {
    return path_executable(program);
  }
  if (env == NULL) {
    return 0;
  }

  while (segment != NULL && segment[0] != '\0') {
    const char *next = strchr(segment, ':');
    size_t len = (next != NULL) ? (size_t)(next - segment) : strlen(segment);
    char candidate[PATH_MAX];
    char directory[PATH_MAX];

    if (len == 0U) {
      copy_string(directory, sizeof(directory), ".");
    } else {
      if (len >= sizeof(directory)) {
        len = sizeof(directory) - 1U;
      }
      memcpy(directory, segment, len);
      directory[len] = '\0';
    }
    join_path(directory, program, candidate, sizeof(candidate));
    if (path_executable(candidate)) {
      return 1;
    }
    if (next == NULL) {
      break;
    }
    segment = next + 1;
  }
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
  printf("usage: mkdbg-native [--version] <init|doctor|repo|target|incident|capture> [options]\n");
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

static int resolve_repo_name(const MkdbgConfig *config, const char *repo, const char *target, const char **out_name)
{
  if (repo != NULL && target != NULL) {
    die("pass either a repo name or --target, not both");
  }
  if (target != NULL) {
    *out_name = target;
  } else if (repo != NULL) {
    *out_name = repo;
  } else {
    *out_name = config->default_repo;
  }
  return 0;
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

static int cmd_incident_open(const IncidentOpenOptions *opts)
{
  char config_path[PATH_MAX];
  char incidents_root[PATH_MAX];
  char current_path[PATH_MAX];
  char current_id[MAX_NAME];
  char incident_id[MAX_NAME];
  char incident_dir[PATH_MAX];
  char meta_path[PATH_MAX];
  char slug[MAX_NAME];
  const char *repo_name;
  MkdbgConfig config;
  const RepoConfig *repo;
  FILE *f;
  IncidentMetadata meta;

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

  if (load_current_incident_id(config_path, current_id, sizeof(current_id)) == 0) {
    die("incident `%s` is already active; close it first", current_id);
  }

  incidents_root_from_config(config_path, incidents_root, sizeof(incidents_root));
  current_incident_path_from_config(config_path, current_path, sizeof(current_path));
  state_root_from_config(config_path, incident_dir, sizeof(incident_dir));
  if (ensure_dir(incident_dir) != 0 || ensure_dir(incidents_root) != 0) {
    die("failed to create incident state directories");
  }

  sanitize_slug(opts->name != NULL ? opts->name : repo_name, slug, sizeof(slug));
  snprintf(incident_id, sizeof(incident_id), "%ld-%s", (long)time(NULL), slug);
  join_path(incidents_root, incident_id, incident_dir, sizeof(incident_dir));
  if (ensure_dir(incident_dir) != 0) {
    die("failed to create incident directory: %s", incident_dir);
  }

  memset(&meta, 0, sizeof(meta));
  copy_string(meta.id, sizeof(meta.id), incident_id);
  copy_string(meta.name, sizeof(meta.name), opts->name != NULL ? opts->name : repo_name);
  copy_string(meta.status, sizeof(meta.status), "open");
  copy_string(meta.repo, sizeof(meta.repo), repo_name);
  copy_string(meta.port, sizeof(meta.port), opts->port != NULL ? opts->port : repo->port);
  meta.opened_at = (long)time(NULL);

  incident_meta_path(incident_dir, meta_path, sizeof(meta_path));
  if (write_incident_metadata(meta_path, &meta, 0L) != 0) {
    die("failed to write incident metadata");
  }

  f = fopen(current_path, "w");
  if (f == NULL) {
    die("failed to write current incident marker");
  }
  fprintf(f, "%s\n", incident_id);
  fclose(f);

  printf("incident: %s\n", incident_id);
  printf("path: %s\n", incident_dir);
  printf("repo: %s\n", repo_name);
  if ((opts->port != NULL && opts->port[0] != '\0') || repo->port[0] != '\0') {
    printf("port: %s\n", opts->port != NULL ? opts->port : repo->port);
  }
  return 0;
}

static int cmd_incident_status(const IncidentStatusOptions *opts)
{
  char config_path[PATH_MAX];
  char incident_id[MAX_NAME];
  char incidents_root[PATH_MAX];
  char incident_dir[PATH_MAX];
  char meta_path[PATH_MAX];
  IncidentMetadata meta;

  if (find_config_upward(config_path, sizeof(config_path)) != 0) {
    die("missing %s; run `mkdbg init` first", CONFIG_NAME);
  }
  if (load_current_incident_id(config_path, incident_id, sizeof(incident_id)) != 0) {
    if (opts->json) {
      printf("{\"ok\":true,\"active\":false}\n");
    } else {
      printf("incident: none\n");
    }
    return 0;
  }

  incidents_root_from_config(config_path, incidents_root, sizeof(incidents_root));
  join_path(incidents_root, incident_id, incident_dir, sizeof(incident_dir));
  incident_meta_path(incident_dir, meta_path, sizeof(meta_path));
  if (load_incident_metadata(meta_path, &meta) != 0) {
    die("missing incident metadata: %s", meta_path);
  }

  if (opts->json) {
    printf("{\"ok\":true,\"active\":true,\"id\":\"%s\",\"path\":\"%s\",\"name\":\"%s\",\"status\":\"%s\",\"repo\":\"%s\",\"port\":\"%s\",\"opened_at\":%ld}\n",
           incident_id, incident_dir, meta.name, meta.status, meta.repo, meta.port, meta.opened_at);
  } else {
    printf("incident: %s\n", incident_id);
    printf("path: %s\n", incident_dir);
    printf("status: %s\n", meta.status);
    printf("repo: %s\n", meta.repo);
    if (meta.port[0] != '\0') printf("port: %s\n", meta.port);
    printf("opened_at: %ld\n", meta.opened_at);
  }
  return 0;
}

static int cmd_incident_close(void)
{
  char config_path[PATH_MAX];
  char current_path[PATH_MAX];
  char incident_id[MAX_NAME];
  char incidents_root[PATH_MAX];
  char incident_dir[PATH_MAX];
  char meta_path[PATH_MAX];
  IncidentMetadata meta;

  if (find_config_upward(config_path, sizeof(config_path)) != 0) {
    die("missing %s; run `mkdbg init` first", CONFIG_NAME);
  }
  if (load_current_incident_id(config_path, incident_id, sizeof(incident_id)) != 0) {
    die("no active incident to close");
  }
  incidents_root_from_config(config_path, incidents_root, sizeof(incidents_root));
  join_path(incidents_root, incident_id, incident_dir, sizeof(incident_dir));
  incident_meta_path(incident_dir, meta_path, sizeof(meta_path));
  if (load_incident_metadata(meta_path, &meta) != 0) {
    die("missing incident metadata: %s", meta_path);
  }
  copy_string(meta.status, sizeof(meta.status), "closed");
  if (write_incident_metadata(meta_path, &meta, (long)time(NULL)) != 0) {
    die("failed to update incident metadata");
  }
  current_incident_path_from_config(config_path, current_path, sizeof(current_path));
  if (unlink(current_path) != 0) {
    die("failed to clear current incident marker");
  }
  printf("closed incident: %s\n", incident_id);
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

static int parse_repo_add_args(int argc, char **argv, RepoAddOptions *opts)
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

static int parse_name_command_args(int argc, char **argv, NameCommandOptions *opts, const char *label)
{
  if (argc != 1) {
    die("%s requires exactly one name", label);
  }
  opts->name = argv[0];
  return 0;
}

static int parse_incident_open_args(int argc, char **argv, IncidentOpenOptions *opts)
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

static int parse_incident_status_args(int argc, char **argv, IncidentStatusOptions *opts)
{
  int i;
  memset(opts, 0, sizeof(*opts));
  for (i = 0; i < argc; ++i) {
    if (strcmp(argv[i], "--json") == 0) opts->json = 1;
    else die("unknown incident status argument: %s", argv[i]);
  }
  return 0;
}

static int parse_capture_bundle_args(int argc, char **argv, CaptureBundleOptions *opts)
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

  die("unknown command: %s", argv[1]);
  return 2;
}
