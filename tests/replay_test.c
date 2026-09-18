#include "mkdbg.h"
#include "json.h"

static int failures;

static void expect(int condition, const char *message)
{
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
  }
}

static int write_bundle(const char *path, const char *pc, const char *lr,
                        const char *sp, const char *cfsr,
                        const char *decoded, int large)
{
  FILE *file = fopen(path, "wb");
  size_t i;
  if (!file) return -1;
  if (fputs("{\"padding\":\"", file) == EOF) goto fail;
  for (i = 0; large && i < 70000U; i++) {
    if (fputc('x', file) == EOF) goto fail;
  }
  if (fprintf(file,
              "\",\"halt_signal\":5,\"timeout\":0,\"pc\":\"%s\","
              "\"lr\":\"%s\",\"sp\":\"%s\",\"cfsr\":\"%s\","
              "\"cfsr_decoded\":\"%s\"}\n",
              pc, lr, sp, cfsr, decoded) < 0) goto fail;
  return fclose(file) == 0 ? 0 : -1;
fail:
  fclose(file);
  return -1;
}

static int capture_diff(const DiffOptions *options, char *out, size_t out_size)
{
  FILE *capture = tmpfile();
  int saved;
  int rc;
  size_t n;
  if (!capture) return -1;
  fflush(stdout);
  saved = dup(STDOUT_FILENO);
  if (saved < 0 || dup2(fileno(capture), STDOUT_FILENO) < 0) {
    if (saved >= 0) close(saved);
    fclose(capture);
    return -1;
  }
  rc = cmd_diff(options);
  fflush(stdout);
  if (dup2(saved, STDOUT_FILENO) < 0) rc = -1;
  close(saved);
  rewind(capture);
  n = fread(out, 1, out_size - 1U, capture);
  out[n] = '\0';
  if (ferror(capture)) rc = -1;
  fclose(capture);
  return rc;
}

static int valid_json(const char *text)
{
  FILE *file = tmpfile();
  JsonReader reader;
  JsonToken token;
  JsonTokenType type;
  if (!file) return 0;
  if (fputs(text, file) == EOF || fflush(file) != 0) { fclose(file); return 0; }
  rewind(file);
  json_reader_init(&reader, file);
  do {
    type = json_reader_next(&reader, &token);
  } while (type != JSON_TOKEN_EOF && type != JSON_TOKEN_ERROR);
  fclose(file);
  return type == JSON_TOKEN_EOF;
}

int main(void)
{
  char dir[] = "/tmp/mkdbg-replay-XXXXXX";
  char left[PATH_MAX];
  char right[PATH_MAX];
  char output[8192];
  BundleSummary summary;
  DiffOptions options;

  expect(mkdtemp(dir) != NULL, "create replay test directory");
  snprintf(left, sizeof(left), "%s/left\\\"bundle.json", dir);
  snprintf(right, sizeof(right), "%s/right.json", dir);
  expect(write_bundle(left, "0x1", "0x2", "0x3", "0x4", "precise fault", 1) == 0,
         "write bundle larger than 64 KiB");
  expect(write_bundle(right, "0x1", "0x9", "0xa", "0x4", "other fault", 0) == 0,
         "write comparison bundle");

  expect(load_bundle_summary(left, &summary) == 0,
         "stream bundle larger than 64 KiB");
  expect(strcmp(summary.lr, "0x2") == 0 && strcmp(summary.sp, "0x3") == 0,
         "read fields after 64 KiB boundary");

  memset(&options, 0, sizeof(options));
  options.left = left;
  options.right = right;
  options.json = 1;
  expect(capture_diff(&options, output, sizeof(output)) == 0, "render JSON diff");
  expect(valid_json(output), "render syntactically valid JSON");
  expect(strstr(output, "\"lr_changed\":true") != NULL,
         "include LR change in JSON diff");
  expect(strstr(output, "\"sp_changed\":true") != NULL,
         "include SP change in JSON diff");
  expect(strstr(output, "\"cfsr_decoded_changed\":true") != NULL,
         "include decoded CFSR change in JSON diff");
  expect(strstr(output, "left\\\\\\\"bundle.json") != NULL,
         "escape path quotes and backslashes");

  expect(unlink(left) == 0, "remove left fixture");
  expect(unlink(right) == 0, "remove right fixture");
  expect(rmdir(dir) == 0, "remove replay test directory");
  return failures == 0 ? 0 : 1;
}
