#include "mkdbg.h"

void die(const char *fmt, ...)
{
  va_list ap;
  fprintf(stderr, "error: ");
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(2);
}

void trim_in_place(char *s)
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

void copy_string(char *dst, size_t dst_size, const char *src)
{
  size_t len;
  const char *value = (src != NULL) ? src : "";
  if (dst_size == 0U) {
    return;
  }
  len = strlen(value);
  if (len >= dst_size) {
    len = dst_size - 1U;
  }
  if (len > 0U) {
    memcpy(dst, value, len);
  }
  dst[len] = '\0';
}

void append_string(char *dst, size_t dst_size, const char *src)
{
  size_t used;
  size_t len;
  const char *value = (src != NULL) ? src : "";

  if (dst_size == 0U) {
    return;
  }
  used = strlen(dst);
  if (used >= dst_size - 1U) {
    dst[dst_size - 1U] = '\0';
    return;
  }
  len = strlen(value);
  if (len > dst_size - used - 1U) {
    len = dst_size - used - 1U;
  }
  if (len > 0U) {
    memcpy(dst + used, value, len);
  }
  dst[used + len] = '\0';
}

void replace_all(char *dst,
                 size_t dst_size,
                 const char *src,
                 const char *needle,
                 const char *replacement)
{
  size_t needle_len = strlen(needle);
  size_t replacement_len = strlen(replacement);
  size_t out = 0U;
  size_t i = 0U;

  if (dst_size == 0U) {
    return;
  }
  while (src[i] != '\0' && out + 1U < dst_size) {
    if (needle_len > 0U && strncmp(src + i, needle, needle_len) == 0) {
      size_t copy_len = replacement_len;
      if (copy_len > dst_size - out - 1U) {
        copy_len = dst_size - out - 1U;
      }
      if (copy_len > 0U) {
        memcpy(dst + out, replacement, copy_len);
        out += copy_len;
      }
      i += needle_len;
    } else {
      dst[out++] = src[i++];
    }
  }
  dst[out] = '\0';
}

void format_u32_hex(const char *input, const char *label, char *out, size_t out_size)
{
  unsigned long value;
  char *end = NULL;

  errno = 0;
  value = strtoul(input, &end, 0);
  if (input == NULL || input[0] == '\0' || end == NULL || *end != '\0' || errno != 0) {
    die("invalid %s: %s", label, input != NULL ? input : "");
  }
  if (value > 0xFFFFFFFFUL) {
    die("%s out of range: %s", label, input);
  }
  snprintf(out, out_size, "0x%08lx", value);
}

const char *path_basename(const char *path)
{
  const char *slash = strrchr(path, '/');
  if (slash == NULL || slash[1] == '\0') {
    return path;
  }
  return slash + 1;
}

void path_dirname(const char *path, char *out, size_t out_size)
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

void join_path(const char *a, const char *b, char *out, size_t out_size)
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

void resolve_path(const char *base, const char *raw, char *out, size_t out_size)
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

int path_exists(const char *path)
{
  return access(path, F_OK) == 0;
}

int path_executable(const char *path)
{
  return access(path, X_OK) == 0;
}

int ensure_dir(const char *path)
{
  if (mkdir(path, 0777) == 0 || errno == EEXIST) {
    return 0;
  }
  return -1;
}

void print_check(int ok, const char *label, const char *detail, int *failed)
{
  printf("[mkdbg] %-7s %s: %s\n", ok ? "ok" : "missing", label, detail);
  if (!ok && failed != NULL) {
    *failed = 1;
  }
}

int command_program(const char *command, char *out, size_t out_size)
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

int search_path(const char *program)
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

int command_available(const char *command)
{
  char program[PATH_MAX];
  if (command_program(command, program, sizeof(program)) != 0) {
    return 0;
  }
  return search_path(program);
}
