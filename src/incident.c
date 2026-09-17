#include "mkdbg.h"
#include "json.h"

typedef int (*AtomicFileWriter)(FILE *file, const void *context);

static int sync_parent_directory(const char *path)
{
  char parent[PATH_MAX];
  int fd;
  path_dirname(path, parent, sizeof(parent));
#ifdef O_DIRECTORY
  fd = open(parent, O_RDONLY | O_DIRECTORY);
#else
  fd = open(parent, O_RDONLY);
#endif
  if (fd < 0) return -1;
  if (fsync(fd) != 0) { close(fd); return -1; }
  return close(fd) == 0 ? 0 : -1;
}

static int atomic_write_file(const char *path, AtomicFileWriter writer,
                             const void *context)
{
  char temp_path[PATH_MAX];
  int fd;
  FILE *file;
  int result = -1;
  int n = snprintf(temp_path, sizeof(temp_path), "%s.tmp.XXXXXX", path);
  if (n < 0 || (size_t)n >= sizeof(temp_path)) return -1;
  fd = mkstemp(temp_path);
  if (fd < 0) return -1;
  file = fdopen(fd, "w");
  if (!file) { close(fd); unlink(temp_path); return -1; }
  {
    int write_ok = writer(file, context) == 0 && fflush(file) == 0 && fsync(fd) == 0;
    int close_ok = fclose(file) == 0;
    if (write_ok && close_ok &&
        rename(temp_path, path) == 0 && sync_parent_directory(path) == 0)
      result = 0;
  }
  if (result != 0) unlink(temp_path);
  return result;
}

static int read_metadata_value(JsonReader *reader, JsonToken *value)
{
  JsonToken colon;
  if (json_reader_next(reader, &colon) != JSON_TOKEN_COLON) return -1;
  return json_reader_next(reader, value) == JSON_TOKEN_ERROR ? -1 : 0;
}

static int copy_metadata_string(char *out, size_t out_size,
                                const JsonToken *value)
{
  if (value->type != JSON_TOKEN_STRING || value->truncated ||
      strlen(value->text) >= out_size) return -1;
  copy_string(out, out_size, value->text);
  return 0;
}

void sanitize_slug(const char *input, char *out, size_t out_size)
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

static int load_incident_marker(const char *path, char *out, size_t out_size)
{
  FILE *f;
  f = fopen(path, "r");
  if (f == NULL) {
    return -1;
  }
  if (fgets(out, (int)out_size, f) == NULL) {
    fclose(f);
    return -1;
  }
  if (fclose(f) != 0) return -1;
  trim_in_place(out);
  return (out[0] == '\0') ? -1 : 0;
}

int load_current_incident_id(const char *config_path, char *out, size_t out_size)
{
  char current_path[PATH_MAX];
  current_incident_path_from_config(config_path, current_path, sizeof(current_path));
  return load_incident_marker(current_path, out, out_size);
}

int load_incident_metadata(const char *meta_path, IncidentMetadata *meta)
{
  FILE *file;
  JsonReader reader;
  JsonToken token;
  JsonToken key;

  memset(meta, 0, sizeof(*meta));
  file = fopen(meta_path, "rb");
  if (!file) return -1;
  json_reader_init(&reader, file);
  if (json_reader_next(&reader, &token) != JSON_TOKEN_OBJECT_BEGIN) goto fail;
  for (;;) {
    long number;
    JsonTokenType type = json_reader_next(&reader, &key);
    if (type == JSON_TOKEN_OBJECT_END) break;
    if (type == JSON_TOKEN_COMMA) continue;
    if (type != JSON_TOKEN_STRING || key.truncated ||
        read_metadata_value(&reader, &token) != 0) goto fail;
    if (strcmp(key.text, "id") == 0) {
      if (copy_metadata_string(meta->id, sizeof(meta->id), &token) != 0) goto fail;
    } else if (strcmp(key.text, "name") == 0) {
      if (copy_metadata_string(meta->name, sizeof(meta->name), &token) != 0) goto fail;
    } else if (strcmp(key.text, "status") == 0) {
      if (copy_metadata_string(meta->status, sizeof(meta->status), &token) != 0) goto fail;
    } else if (strcmp(key.text, "repo") == 0) {
      if (copy_metadata_string(meta->repo, sizeof(meta->repo), &token) != 0) goto fail;
    } else if (strcmp(key.text, "port") == 0) {
      if (copy_metadata_string(meta->port, sizeof(meta->port), &token) != 0) goto fail;
    } else if (strcmp(key.text, "opened_at") == 0) {
      if (json_token_to_long(&token, &number) != 0) goto fail;
      meta->opened_at = number;
    } else if (strcmp(key.text, "closed_at") == 0) {
      if (json_token_to_long(&token, &number) != 0) goto fail;
      meta->closed_at = number;
    }
  }
  if (json_reader_next(&reader, &token) != JSON_TOKEN_EOF || fclose(file) != 0)
    return -1;
  return 0;
fail:
  fclose(file);
  return -1;
}

static int write_metadata_file(FILE *file, const void *context)
{
  const IncidentMetadata *meta = context;
  JsonWriter writer;
  json_writer_init(&writer, file);
  if (json_writer_begin_object(&writer) != 0 ||
      json_writer_key(&writer, "id") != 0 || json_writer_string(&writer, meta->id) != 0 ||
      json_writer_key(&writer, "name") != 0 || json_writer_string(&writer, meta->name) != 0 ||
      json_writer_key(&writer, "status") != 0 || json_writer_string(&writer, meta->status) != 0 ||
      json_writer_key(&writer, "repo") != 0 || json_writer_string(&writer, meta->repo) != 0 ||
      json_writer_key(&writer, "port") != 0 || json_writer_string(&writer, meta->port) != 0 ||
      json_writer_key(&writer, "opened_at") != 0 || json_writer_long(&writer, meta->opened_at) != 0)
    return -1;
  if (meta->closed_at > 0L &&
      (json_writer_key(&writer, "closed_at") != 0 ||
       json_writer_long(&writer, meta->closed_at) != 0)) return -1;
  return json_writer_end_object(&writer) == 0 && json_writer_finish(&writer) == 0 ? 0 : -1;
}

int write_incident_metadata(const char *meta_path, const IncidentMetadata *meta,
                            long closed_at)
{
  IncidentMetadata copy = *meta;
  copy.closed_at = closed_at;
  return atomic_write_file(meta_path, write_metadata_file, &copy);
}

static int write_marker_file(FILE *file, const void *context)
{
  return fprintf(file, "%s\n", (const char *)context) >= 0 ? 0 : -1;
}

int write_current_incident_id(const char *current_path, const char *incident_id)
{
  return atomic_write_file(current_path, write_marker_file, incident_id);
}

int load_current_incident_dir(const char *config_path, char *out, size_t out_size)
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

int cmd_incident_open(const IncidentOpenOptions *opts)
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
  incident_id[0] = '\0';
  {
    char tsbuf[32];
    snprintf(tsbuf, sizeof(tsbuf), "%ld", (long)time(NULL));
    copy_string(incident_id, sizeof(incident_id), tsbuf);
  }
  append_string(incident_id, sizeof(incident_id), "-");
  append_string(incident_id, sizeof(incident_id), slug);
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

  if (write_current_incident_id(current_path, incident_id) != 0) {
    die("failed to write current incident marker");
  }

  printf("incident: %s\n", incident_id);
  printf("path: %s\n", incident_dir);
  printf("repo: %s\n", repo_name);
  if ((opts->port != NULL && opts->port[0] != '\0') || repo->port[0] != '\0') {
    printf("port: %s\n", opts->port != NULL ? opts->port : repo->port);
  }
  return 0;
}

int cmd_incident_status(const IncidentStatusOptions *opts)
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

int close_incident_state(const char *current_path, const char *meta_path,
                         IncidentMetadata *meta, long closed_at)
{
  char closing_path[PATH_MAX];
  int n = snprintf(closing_path, sizeof(closing_path), "%s.closing", current_path);
  if (n < 0 || (size_t)n >= sizeof(closing_path)) return -1;

  if (rename(current_path, closing_path) == 0) {
    if (sync_parent_directory(current_path) != 0) return -1;
  } else if (errno != ENOENT || access(closing_path, F_OK) != 0) {
    return -1;
  }

  copy_string(meta->status, sizeof(meta->status), "closed");
  if (meta->closed_at <= 0L) meta->closed_at = closed_at;
  if (write_incident_metadata(meta_path, meta, meta->closed_at) != 0) return -1;
  if (unlink(closing_path) != 0) return -1;
  return 0;
}

int cmd_incident_close(void)
{
  char config_path[PATH_MAX];
  char current_path[PATH_MAX];
  char incident_id[MAX_NAME];
  char incidents_root[PATH_MAX];
  char incident_dir[PATH_MAX];
  char meta_path[PATH_MAX];
  char closing_path[PATH_MAX];
  IncidentMetadata meta;

  if (find_config_upward(config_path, sizeof(config_path)) != 0) {
    die("missing %s; run `mkdbg init` first", CONFIG_NAME);
  }
  current_incident_path_from_config(config_path, current_path, sizeof(current_path));
  if (load_incident_marker(current_path, incident_id, sizeof(incident_id)) != 0) {
    int n = snprintf(closing_path, sizeof(closing_path), "%s.closing", current_path);
    if (n < 0 || (size_t)n >= sizeof(closing_path) ||
        load_incident_marker(closing_path, incident_id, sizeof(incident_id)) != 0)
      die("no active incident to close");
  }
  incidents_root_from_config(config_path, incidents_root, sizeof(incidents_root));
  join_path(incidents_root, incident_id, incident_dir, sizeof(incident_dir));
  incident_meta_path(incident_dir, meta_path, sizeof(meta_path));
  if (load_incident_metadata(meta_path, &meta) != 0) {
    die("missing incident metadata: %s", meta_path);
  }
  if (close_incident_state(current_path, meta_path, &meta, (long)time(NULL)) != 0)
    die("failed to close incident; retry the same command");
  printf("closed incident: %s\n", incident_id);
  return 0;
}

static void read_git_rev(const char *repo_root, char *out, size_t out_size)
{
  int pipefd[2];
  pid_t pid;
  ssize_t n;

  copy_string(out, out_size, "unknown");
  if (pipe(pipefd) != 0) {
    return;
  }
  pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return;
  }
  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);
    execlp("git", "git", "-C", repo_root, "rev-parse", "HEAD", (char *)NULL);
    _exit(127);
  }
  close(pipefd[1]);
  n = read(pipefd[0], out, out_size > 0U ? out_size - 1U : 0U);
  close(pipefd[0]);
  wait_for_pid(pid);
  if (n > 0 && out_size > 0U) {
    out[n] = '\0';
    trim_in_place(out);
  }
}

int cmd_incident_export(const IncidentExportOptions *opts)
{
  char config_path[PATH_MAX];
  char incident_id[MAX_NAME];
  char incident_dir[PATH_MAX];
  char meta_path[PATH_MAX];
  char export_dir[PATH_MAX];
  char manifest_path[PATH_MAX];
  char repo_root[PATH_MAX];
  char bundle_path[PATH_MAX];
  char git_rev[64];
  MkdbgConfig config;
  const RepoConfig *repo;
  IncidentMetadata meta;
  FILE *f;

  if (find_config_upward(config_path, sizeof(config_path)) != 0) {
    die("missing %s; run `mkdbg init` first", CONFIG_NAME);
  }
  if (load_config_file(config_path, &config) != 0) {
    die("invalid config: %s", config_path);
  }
  if (load_current_incident_id(config_path, incident_id, sizeof(incident_id)) != 0) {
    die("no active incident to export");
  }
  if (load_current_incident_dir(config_path, incident_dir, sizeof(incident_dir)) != 0) {
    die("failed to resolve active incident directory");
  }
  incident_meta_path(incident_dir, meta_path, sizeof(meta_path));
  if (load_incident_metadata(meta_path, &meta) != 0) {
    die("missing incident metadata: %s", meta_path);
  }
  repo = find_repo_const(&config, meta.repo);
  if (repo == NULL) {
    die("repo `%s` not found in %s", meta.repo, config_path);
  }
  resolve_repo_root(config_path, repo, repo_root, sizeof(repo_root));
  read_git_rev(repo_root, git_rev, sizeof(git_rev));

  if (opts->output != NULL) {
    resolve_path(repo_root, opts->output, export_dir, sizeof(export_dir));
  } else {
    join_path(incident_dir, "export", export_dir, sizeof(export_dir));
  }
  if (ensure_dir(export_dir) != 0) {
    die("failed to create export directory: %s", export_dir);
  }

  join_path(incident_dir, "bundle.json", bundle_path, sizeof(bundle_path));
  join_path(export_dir, "manifest.txt", manifest_path, sizeof(manifest_path));
  f = fopen(manifest_path, "w");
  if (f == NULL) {
    die("failed to write export manifest: %s", manifest_path);
  }
  fprintf(f, "mkdbg incident export\n");
  fprintf(f, "incident_id: %s\n", meta.id);
  fprintf(f, "name: %s\n", meta.name);
  fprintf(f, "status: %s\n", meta.status);
  fprintf(f, "repo: %s\n", meta.repo);
  fprintf(f, "repo_root: %s\n", repo_root);
  fprintf(f, "git_rev: %s\n", git_rev);
  fprintf(f, "config: %s\n", config_path);
  fprintf(f, "incident_dir: %s\n", incident_dir);
  fprintf(f, "metadata: %s\n", meta_path);
  if (path_exists(bundle_path)) {
    fprintf(f, "bundle: %s\n", bundle_path);
    fprintf(f, "replay: mkdbg replay %s\n", bundle_path);
  } else {
    fprintf(f, "bundle: <missing>\n");
  }
  if (meta.port[0] != '\0') {
    fprintf(f, "capture: mkdbg capture bundle --port %s\n", meta.port);
  }
  fclose(f);

  printf("export: %s\n", export_dir);
  printf("manifest: %s\n", manifest_path);
  return 0;
}
