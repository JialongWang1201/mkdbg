#include "mkdbg.h"

static int failures;

static void expect(int condition, const char *message)
{
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
  }
}

int main(void)
{
  char temp[] = "/tmp/mkdbg-process-XXXXXX";
  char repo_path[PATH_MAX];
  char expected_path[PATH_MAX] = {0};
  char output[PATH_MAX];
  char *argv[] = {"pwd", NULL};

  expect(mkdtemp(temp) != NULL, "create temporary parent directory");
  snprintf(repo_path, sizeof(repo_path), "%s/repo\"; touch injected; #", temp);
  expect(mkdir(repo_path, 0700) == 0, "create repository path with shell metacharacters");
  expect(realpath(repo_path, expected_path) != NULL, "resolve repository path");
  expect(capture_process_output(argv, repo_path, output, sizeof(output)) == 0,
         "capture subprocess output");
  output[strcspn(output, "\r\n")] = '\0';
  expect(strcmp(output, expected_path) == 0, "pass repository path as cwd without shell parsing");
  expect(access("injected", F_OK) != 0, "do not execute path contents as a command");

  expect(rmdir(repo_path) == 0, "remove temporary repository directory");
  expect(rmdir(temp) == 0, "remove temporary parent directory");
  return failures == 0 ? 0 : 1;
}
