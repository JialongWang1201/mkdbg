#include "mkdbg.h"

speed_t baud_to_speed(int baud)
{
  switch (baud) {
  case 9600:   return B9600;
  case 19200:  return B19200;
  case 38400:  return B38400;
  case 57600:  return B57600;
  case 115200: return B115200;
  case 230400: return B230400;
  default:     return B0;
  }
}

const char *resolve_serial_port(const char *config_path,
                                const MkdbgConfig *config,
                                const SerialOptions *opts)
{
  const char *port = opts->port;
  if (port != NULL && port[0] != '\0') {
    return port;
  }
  {
    const char *repo_name;
    const RepoConfig *repo;
    resolve_repo_name(config, opts->repo, opts->target, &repo_name);
    repo = find_repo_const(config, repo_name);
    if (repo != NULL && repo->port[0] != '\0') {
      return repo->port;
    }
  }
  (void)config_path;
  die("serial command requires a port; pass --port or set port in config");
  return NULL;
}

int cmd_serial_tail(const SerialOptions *opts)
{
  char config_path[PATH_MAX];
  MkdbgConfig config;
  const char *port;
  int baud;
  speed_t speed;
  int fd;
  struct termios tty;
  unsigned char buf[SERIAL_BUF_SIZE];
  ssize_t n;

  if (find_config_upward(config_path, sizeof(config_path)) != 0) {
    die("missing %s; run `mkdbg init` first", CONFIG_NAME);
  }
  if (load_config_file(config_path, &config) != 0) {
    die("invalid config: %s", config_path);
  }

  port = resolve_serial_port(config_path, &config, opts);
  baud = opts->baud > 0 ? opts->baud : DEFAULT_BAUD;
  speed = baud_to_speed(baud);
  if (speed == B0) {
    die("unsupported baud rate: %d", baud);
  }

  printf("[mkdbg] serial tail port=%s baud=%d\n", port, baud);
  if (opts->dry_run) {
    return 0;
  }

  fd = open(port, O_RDONLY | O_NOCTTY);
  if (fd < 0) {
    die("cannot open serial port %s: %s", port, strerror(errno));
  }

  memset(&tty, 0, sizeof(tty));
  if (tcgetattr(fd, &tty) != 0) {
    close(fd);
    die("tcgetattr failed: %s", strerror(errno));
  }
  cfmakeraw(&tty);
  cfsetispeed(&tty, speed);
  cfsetospeed(&tty, speed);
  tty.c_cflag |= (CLOCAL | CREAD);
  tty.c_cc[VMIN] = 1;
  tty.c_cc[VTIME] = 0;
  if (tcsetattr(fd, TCSANOW, &tty) != 0) {
    close(fd);
    die("tcsetattr failed: %s", strerror(errno));
  }

  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    if (write(STDOUT_FILENO, buf, (size_t)n) < 0) {
      break;
    }
  }

  close(fd);
  return 0;
}

int cmd_serial_send(const SerialOptions *opts)
{
  char config_path[PATH_MAX];
  MkdbgConfig config;
  const char *port;
  int baud;
  speed_t speed;
  int fd;
  struct termios tty;
  const char *msg;
  size_t len;
  size_t i;

  if (opts->message == NULL || opts->message[0] == '\0') {
    die("serial send requires a message");
  }

  if (find_config_upward(config_path, sizeof(config_path)) != 0) {
    die("missing %s; run `mkdbg init` first", CONFIG_NAME);
  }
  if (load_config_file(config_path, &config) != 0) {
    die("invalid config: %s", config_path);
  }

  port = resolve_serial_port(config_path, &config, opts);
  baud = opts->baud > 0 ? opts->baud : DEFAULT_BAUD;
  speed = baud_to_speed(baud);
  if (speed == B0) {
    die("unsupported baud rate: %d", baud);
  }

  msg = opts->message;
  len = strlen(msg);

  printf("[mkdbg] serial send port=%s baud=%d len=%zu\n", port, baud, len);
  if (opts->dry_run) {
    return 0;
  }

  fd = open(port, O_WRONLY | O_NOCTTY);
  if (fd < 0) {
    die("cannot open serial port %s: %s", port, strerror(errno));
  }

  memset(&tty, 0, sizeof(tty));
  if (tcgetattr(fd, &tty) != 0) {
    close(fd);
    die("tcgetattr failed: %s", strerror(errno));
  }
  cfmakeraw(&tty);
  cfsetispeed(&tty, speed);
  cfsetospeed(&tty, speed);
  tty.c_cflag |= (CLOCAL | CREAD);
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 10;
  if (tcsetattr(fd, TCSANOW, &tty) != 0) {
    close(fd);
    die("tcsetattr failed: %s", strerror(errno));
  }

  if (opts->char_delay_ms > 0.0) {
    for (i = 0; i < len; ++i) {
      if (write(fd, &msg[i], 1) != 1) {
        close(fd);
        die("serial write failed: %s", strerror(errno));
      }
      usleep((useconds_t)(opts->char_delay_ms * 1000.0));
    }
  } else {
    if (write(fd, msg, len) != (ssize_t)len) {
      close(fd);
      die("serial write failed: %s", strerror(errno));
    }
  }

  close(fd);
  printf("[mkdbg] sent %zu bytes\n", len);
  return 0;
}
