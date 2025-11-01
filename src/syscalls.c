#include <errno.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

void board_uart_write(const char *s);
int board_uart_read_char(char *out);

int _close(int file) {
  (void)file;
  errno = ENOSYS;
  return -1;
}

int _fstat(int file, struct stat *st) {
  (void)file;
  if (st == NULL) {
    errno = EINVAL;
    return -1;
  }
  st->st_mode = S_IFCHR;
  return 0;
}

int _isatty(int file) {
  (void)file;
  return 1;
}

int _lseek(int file, int ptr, int dir) {
  (void)file;
  (void)ptr;
  (void)dir;
  errno = ENOSYS;
  return -1;
}

int _read(int file, char *ptr, int len) {
  (void)file;
  if (ptr == NULL || len <= 0) {
    return 0;
  }
  if (board_uart_read_char(&ptr[0])) {
    return 1;
  }
  return 0;
}

int _write(int file, char *ptr, int len) {
  (void)file;
  if (ptr == NULL || len <= 0) {
    return 0;
  }
  for (int i = 0; i < len; i++) {
    char buf[2] = {ptr[i], '\0'};
    board_uart_write(buf);
  }
  return len;
}

extern char _end;
extern char _estack;

void *_sbrk(ptrdiff_t incr) {
  static char *heap_end;
  char *prev;

  if (heap_end == NULL) {
    heap_end = &_end;
  }

  prev = heap_end;
  if (heap_end + incr > &_estack) {
    errno = ENOMEM;
    return (void *)-1;
  }

  heap_end += incr;
  return prev;
}
