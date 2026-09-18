#include "mkdbg.h"

void print_shell_arg(FILE *f, const char *arg)
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

int run_process(char *const argv[], const char *cwd, int dry_run)
{
  pid_t pid;
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
  {
    int status;
    if (waitpid(pid, &status, 0) < 0) {
      die("waitpid failed: %s", strerror(errno));
    }
    if (WIFEXITED(status)) {
      return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
      return 128 + WTERMSIG(status);
    }
  }
  return 1;
}

int capture_process_output(char *const argv[], const char *cwd,
                           char *out, size_t out_size)
{
  int pipefd[2];
  pid_t pid;
  size_t used = 0;

  if (!argv || !argv[0] || !cwd || !out || out_size == 0U) {
    errno = EINVAL;
    return -1;
  }
  out[0] = '\0';
  if (pipe(pipefd) != 0) {
    return -1;
  }

  pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }
  if (pid == 0) {
    int null_fd;
    close(pipefd[0]);
    if (chdir(cwd) != 0 || dup2(pipefd[1], STDOUT_FILENO) < 0) {
      _exit(127);
    }
    close(pipefd[1]);
    null_fd = open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
      (void)dup2(null_fd, STDERR_FILENO);
      close(null_fd);
    }
    execvp(argv[0], argv);
    _exit(127);
  }

  close(pipefd[1]);
  for (;;) {
    char buf[256];
    ssize_t n = read(pipefd[0], buf, sizeof(buf));
    if (n > 0) {
      size_t available = out_size - 1U - used;
      size_t copy_n = (size_t)n < available ? (size_t)n : available;
      if (copy_n > 0U) {
        memcpy(out + used, buf, copy_n);
        used += copy_n;
      }
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
  close(pipefd[0]);
  out[used] = '\0';
  return wait_for_pid(pid);
}

void print_command_label(const char *label, char *const argv[])
{
  size_t i;

  printf("[mkdbg] %s=", label);
  for (i = 0U; argv[i] != NULL; ++i) {
    if (i != 0U) {
      fputc(' ', stdout);
    }
    print_shell_arg(stdout, argv[i]);
  }
  fputc('\n', stdout);
}

void sleep_seconds(double seconds)
{
  struct timespec req;

  if (seconds <= 0.0) {
    return;
  }
  req.tv_sec = (time_t)seconds;
  req.tv_nsec = (long)((seconds - (double)req.tv_sec) * 1000000000.0);
  if (req.tv_nsec < 0L) {
    req.tv_nsec = 0L;
  }
  while (nanosleep(&req, &req) != 0 && errno == EINTR) {
  }
}

int wait_status_to_rc(int status)
{
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return 1;
}

pid_t spawn_process(char *const argv[], const char *cwd)
{
  pid_t pid = fork();

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
  return pid;
}

int wait_for_pid(pid_t pid)
{
  int status;
  if (waitpid(pid, &status, 0) < 0) {
    die("waitpid failed: %s", strerror(errno));
  }
  return wait_status_to_rc(status);
}

int try_reap_pid(pid_t pid, int *rc)
{
  int status;
  pid_t got = waitpid(pid, &status, WNOHANG);

  if (got == 0) {
    return 0;
  }
  if (got < 0) {
    if (errno == ECHILD) {
      *rc = 1;
      return 1;
    }
    die("waitpid failed: %s", strerror(errno));
  }
  *rc = wait_status_to_rc(status);
  return 1;
}

void terminate_pid(pid_t pid)
{
  int rc = 0;
  int i;

  if (try_reap_pid(pid, &rc)) {
    return;
  }
  kill(pid, SIGTERM);
  for (i = 0; i < 20; ++i) {
    sleep_seconds(0.1);
    if (try_reap_pid(pid, &rc)) {
      return;
    }
  }
  kill(pid, SIGKILL);
  (void)wait_for_pid(pid);
}
